// Topology Ghost: a source mesh (a base mesh, a mount…) marked as a ghost source lays its topology onto the meshes being
// retopologised — the references (else every shown mesh but the edited one and the sources; their snap masks honoured,
// so one layer of a many-layered outfit is chosen) — as a ghost of its polygons:
//  each vertex along its smoothed normal both ways, as far as the reach — in the source's edge lengths, so a model of
//  any scale gets the same (missed — the nearest point within it), then
//  relaxed on the surface (no folds in the hollows); its confidence — how far it went against the reach: green close,
//  yellow stretched, red doubtful; farther than the reach — gone (a skirt off the legs: drawn by hand). Trim to the
//  border: a polygon all of whose points only reached the surface's edge is left out — the ghost ends on the border.
//  Density 2: the source subdivided first (its loops kept, one more between each two).
// The source works hidden too. The Reveal Brush makes the ghost's polygons real (welded onto what is built there);
// PolyPen snaps to its vertices, then its edges (before the bare surface); a moved vertex let go near a ghost vertex
// sits on it; Shift+RMB fills the ghost polygon under the pointer.
#include "scene/MeshMods.h"
#include "scene/TopoEdit.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <array>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <QTimer>

namespace hm {
void symAddFaces(tedit::MeshEd& E, int nf0);   // (ViewportTopoTools.cpp: Symmetry — the new polygons mirrored)

using tedit::MeshEd;

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
}  // namespace

void Viewport::loadGhostPrefs() {
  const QJsonObject o = QJsonDocument::fromJson(QSettings().value("topo/ghost").toByteArray()).object();
  gho.dist = std::clamp(o.value("reachE").toDouble(4), 0.1, 50.0);   // (in the source's edge lengths; the old "dist", in centimetres, not read)
  gho.relax = std::clamp(o.value("relax").toInt(3), 0, 20);
  gho.density = o.value("density").toInt(1) == 2 ? 2 : 1; gho.trim = o.value("trim").toBool(true); gho.show = o.value("show").toBool(true);
}
void Viewport::saveGhostPrefs() const {
  QSettings().setValue("topo/ghost", QJsonDocument(QJsonObject{{"reachE", gho.dist}, {"relax", gho.relax}, {"density", gho.density}, {"trim", gho.trim}, {"show", gho.show}}).toJson(QJsonDocument::Compact));
}

bool Viewport::ghostAny() const {
  if (room() != "topo") return false;
  for (auto& [id, it] : scene_->items) if (it.type == ItemType::IMesh && it.raw.value("tghost").toBool() && (it.base || it.poly)) return true;
  return false;
}

// ---- the work (a thread of its own): the targets' hierarchies, each source laid on them ----
struct GhostJob {
  struct Tgt { std::vector<Vec3> pos; std::vector<uint32_t> tris; std::vector<int> t2f; std::vector<char> mask; };
  struct Src { int id = -1; std::vector<Vec3> P; std::vector<int> fo, fv; };
  std::vector<Tgt> tgts; std::vector<Src> srcs; Viewport::GhostOpts o; QByteArray key;
  std::vector<Viewport::Ghost> out; std::atomic<int> prog{0}, stage{0};   // (stage: hierarchy, points, relaxing, lifting)
  std::atomic<bool> done{false}, cancel{false};
};
namespace {
// ---- thin parts: a strap, a band narrower than the source's polygons — the ghost laid on it by points falls apart (its
// points on the strap's two rims, polygons hanging across the gaps to the next part). There instead: a strip of quads
// along the part — its rails on the part's two rims, on its outer face (the side the source's normals look to), a rung
// across it each source edge's length; the ghost's own polygons on the part dropped ----
struct PieceMap { std::vector<int> triPiece; };   // (per target: each triangle's piece — its triangles joined through shared vertices)
PieceMap piecesOf(const GhostJob::Tgt& t) {
  std::vector<int> up(t.pos.size()); for (size_t i = 0; i < up.size(); i++) up[i] = int(i);
  auto find = [&](int x) { while (up[size_t(x)] != x) x = up[size_t(x)] = up[size_t(up[size_t(x)])]; return x; };
  for (size_t i = 0; i + 2 < t.tris.size(); i += 3) { const int a = find(int(t.tris[i])); const int b = find(int(t.tris[i + 1])); up[size_t(b)] = a; const int c = find(int(t.tris[i + 2])); up[size_t(c)] = find(a); }
  PieceMap M; M.triPiece.resize(t.tris.size() / 3); for (size_t i = 0; i < M.triPiece.size(); i++) M.triPiece[i] = find(int(t.tris[i * 3])); return M;
}
// the ghost's vertices: the piece each lies on (target << 32 | piece); -1 — none
std::vector<long long> landing(const Viewport::Ghost& G, const std::vector<std::shared_ptr<topo::Bvh>>& tierA, const std::vector<PieceMap>& PM) {
  std::vector<long long> out(G.P.size(), -1);
  for (size_t v = 0; v < G.P.size(); v++) { if (!G.ok[v]) continue; double bd = 1e300; long long best = -1;
    for (size_t ti = 0; ti < tierA.size(); ti++) { double d; int tri = -1; if (auto q = tierA[ti]->closest(G.P[v], nullptr, &d, &tri); q && d < bd && tri >= 0 && size_t(tri) < PM[ti].triPiece.size()) { bd = d; best = (static_cast<long long>(ti) << 32) | PM[ti].triPiece[size_t(tri)]; } }
    out[v] = best; }
  return out;
}
// (the source's point nearest p: its normal says where the outside is)
Vec3 outAt(const std::vector<Vec3>& srcP, const std::vector<Vec3>& srcN, const Vec3& p) {
  size_t sb = 0; double sd = 1e300; for (size_t v = 0; v < srcP.size(); v++) if (const double d = (srcP[v] - p).lengthSq(); d < sd) { sd = d; sb = v; }
  return sb < srcN.size() ? srcN[sb] : Vec3(0, 0, 1);
}
int bandStrips(Viewport::Ghost& G, const std::vector<Vec3>& srcP, const std::vector<Vec3>& srcN, const std::vector<GhostJob::Tgt>& tgts, const std::vector<std::shared_ptr<topo::Bvh>>& tierA, const std::vector<PieceMap>& PM, std::atomic<bool>& cancel) {
  const double L = std::max(1e-6, G.avgL), h = L * 0.5;
  const auto land = landing(G, tierA, PM);
  std::map<long long, int> cnt; for (long long k : land) if (k >= 0) cnt[k]++;
  const size_t nf0 = G.fok.size(); std::vector<char> drop(nf0, 0); int total = 0;
  auto ck = [](long long x, long long y, long long z) { return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
  for (auto& [key, c] : cnt) {
    if (cancel) return 0; if (c < 2) continue;
    const size_t ti = size_t(key >> 32); const int piece = int(key & 0xffffffffLL); const auto& T = tgts[ti];
    // (the piece's triangles and vertices)
    std::vector<uint32_t> ptris; std::vector<char> pv(T.pos.size(), 0);
    for (size_t i = 0; i < PM[ti].triPiece.size(); i++) if (PM[ti].triPiece[i] == piece) for (int j = 0; j < 3; j++) { ptris.push_back(T.tris[i * 3 + size_t(j)]); pv[T.tris[i * 3 + size_t(j)]] = 1; }
    if (ptris.size() < 9) continue;
    // (its vertices in cells of half a source edge: their middles a rough centre line, the cells side by side joined)
    struct Cell { Vec3 c; int n = 0; std::vector<int> vs; };
    std::unordered_map<long long, int> at; std::vector<Cell> cells; std::vector<std::array<long long, 3>> cix;
    for (size_t v = 0; v < T.pos.size(); v++) if (pv[v]) { const Vec3& p = T.pos[v]; const long long x = (long long)std::floor(p.x / h), y = (long long)std::floor(p.y / h), z = (long long)std::floor(p.z / h);
      auto r = at.emplace(ck(x, y, z), int(cells.size())); if (r.second) { cells.push_back({}); cix.push_back({x, y, z}); } Cell& C = cells[size_t(r.first->second)]; C.c += p; C.n++; C.vs.push_back(int(v)); }
    if (cells.size() < 6 || cells.size() > 200000) continue;
    for (auto& C : cells) C.c = C.c * (1.0 / C.n);
    std::vector<std::vector<int>> nb(cells.size());
    for (size_t i = 0; i < cells.size(); i++) for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) { if (!dx && !dy && !dz) continue;
      auto f = at.find(ck(cix[i][0] + dx, cix[i][1] + dy, cix[i][2] + dz)); if (f != at.end()) nb[i].push_back(f->second); }
    auto bfs = [&](int s, const std::vector<char>* blocked, std::vector<int>& par, std::vector<int>& dist) { dist.assign(cells.size(), -1); par.assign(cells.size(), -1); std::vector<int> q{s}; dist[size_t(s)] = 0; int far = s;
      for (size_t k = 0; k < q.size(); k++) { const int x = q[k]; if (dist[size_t(x)] > dist[size_t(far)]) far = x; for (int y : nb[size_t(x)]) if (dist[size_t(y)] < 0 && !(blocked && (*blocked)[size_t(y)])) { dist[size_t(y)] = dist[size_t(x)] + 1; par[size_t(y)] = x; q.push_back(y); } }
      return far; };
    std::vector<int> par, dist; const int A = bfs(0, nullptr, par, dist); const int B = bfs(A, nullptr, par, dist);
    std::vector<int> path; for (int x = B; x >= 0; x = par[size_t(x)]) path.push_back(x);
    if (path.size() < 4) continue;
    // (round — a ring: a second way from B back to A past the first one's cells)
    bool closed = false; std::vector<int> back;
    { std::vector<char> blk(cells.size(), 0); for (size_t k = 2; k + 2 < path.size(); k++) { blk[size_t(path[k])] = 1; for (int y : nb[size_t(path[k])]) blk[size_t(y)] = 1; }
      for (int e : {A, B}) { blk[size_t(e)] = 0; for (int y : nb[size_t(e)]) blk[size_t(y)] = 0; }
      std::vector<int> p2, d2; bfs(B, &blk, p2, d2);
      if (d2[size_t(A)] > 0 && d2[size_t(A)] * 2 > int(path.size())) { closed = true; for (int x = p2[size_t(A)]; x >= 0 && x != B; x = p2[size_t(x)]) back.push_back(x); } }
    std::vector<Vec3> line; for (int x : path) line.push_back(cells[size_t(x)].c);   // (B … A)
    if (closed) for (size_t k = back.size(); k-- > 0;) line.push_back(cells[size_t(back[k])].c);   // (A … B by the other way: round)
    // (smoothed along itself, then points a source edge apart)
    for (int it = 0; it < 4; it++) { std::vector<Vec3> l2 = line; const size_t n = line.size();
      for (size_t k = 0; k < n; k++) { if (!closed && (k == 0 || k + 1 == n)) continue; l2[k] = (line[(k + n - 1) % n] + line[k] * 2 + line[(k + 1) % n]) * 0.25; } line = l2; }
    std::vector<double> cum{0}; const size_t lastK = closed ? line.size() : line.size() - 1;
    for (size_t k = 1; k <= lastK; k++) cum.push_back(cum.back() + line[k % line.size()].distanceTo(line[k - 1]));
    const double len = cum.back(); if (len < L * 2) continue;
    const int segs = std::max(2, int(std::lround(len / L)));
    auto along = [&](double d) { size_t k = 1; while (k + 1 < cum.size() && cum[k] < d) k++; const double t = cum[k] > cum[k - 1] ? (d - cum[k - 1]) / (cum[k] - cum[k - 1]) : 0; return line[k - 1] + (line[k % line.size()] - line[k - 1]) * t; };
    const int ns = closed ? segs : segs + 1;
    std::vector<Vec3> cs(static_cast<size_t>(ns)); for (int i = 0; i < ns; i++) cs[size_t(i)] = along(len * i / segs);
    // (each point: the part's slice across it — its width along the major axis, its thickness the minor; the outer side
    // as the source's normal there looks)
    topo::Bvh PB; PB.build(T.pos, ptris);
    std::vector<std::array<Vec3, 2>> rails(static_cast<size_t>(ns)); std::vector<char> thin(static_cast<size_t>(ns), 0); Vec3 wPrev;
    for (int i = 0; i < ns; i++) { const Vec3 c = cs[size_t(i)];
      Vec3 t = closed ? cs[size_t((i + 1) % ns)] - cs[size_t((i + ns - 1) % ns)] : cs[size_t(std::min(i + 1, ns - 1))] - cs[size_t(std::max(i - 1, 0))]; if (t.lengthSq() <= 0) continue; t = t.normalized();
      const Vec3 u0 = std::abs(t.x) < 0.9 ? Vec3(1, 0, 0).cross(t).normalized() : Vec3(0, 1, 0).cross(t).normalized(), u1 = t.cross(u0);
      std::vector<Vec3> pts; const long long x0 = (long long)std::floor(c.x / h), y0 = (long long)std::floor(c.y / h), z0 = (long long)std::floor(c.z / h);
      for (long long dx = -3; dx <= 3; dx++) for (long long dy = -3; dy <= 3; dy++) for (long long dz = -3; dz <= 3; dz++) { auto f = at.find(ck(x0 + dx, y0 + dy, z0 + dz)); if (f == at.end()) continue;
        for (int v : cells[size_t(f->second)].vs) { const Vec3 d = T.pos[size_t(v)] - c; if (std::abs(d.dot(t)) > h * 0.35 || d.length() > L * 1.5) continue; pts.push_back(d - t * d.dot(t)); } }
      if (pts.size() < 4) continue;
      Vec3 mean; for (auto& q : pts) mean += q; mean = mean * (1.0 / pts.size());
      double sxx = 0, sxy = 0, syy = 0; for (auto& q : pts) { const double a = (q - mean).dot(u0), b = (q - mean).dot(u1); sxx += a * a; sxy += a * b; syy += b * b; }
      const double ang = 0.5 * std::atan2(2 * sxy, sxx - syy); Vec3 w = u0 * std::cos(ang) + u1 * std::sin(ang), m = t.cross(w);
      if (m.dot(outAt(srcP, srcN, c)) < 0) m = m * -1.0;
      if (wPrev.lengthSq() > 0 && w.dot(wPrev) < 0) w = w * -1.0;
      double wl = 1e300, wh = -1e300, ml = 1e300, mh = -1e300; for (auto& q : pts) { wl = std::min(wl, q.dot(w)); wh = std::max(wh, q.dot(w)); ml = std::min(ml, q.dot(m)); mh = std::max(mh, q.dot(m)); }
      const double width = wh - wl; if (width >= L * 1.1 || width <= 0) continue;
      wPrev = w;
      // (a rail: from above the outer face near that rim, the nearest point of the part — on its outer face by the rim)
      for (int k = 0; k < 2; k++) { Vec3 q = c + w * ((k == 0 ? wh : wl) * 0.85) + m * (mh + (mh - ml) + L * 0.1);
        if (auto on = PB.closest(q, nullptr)) q = *on; rails[size_t(i)][size_t(k)] = q; }
      thin[size_t(i)] = 1; }
    // (runs of thin points: their quads, side by side sharing the rungs)
    std::vector<std::array<int, 2>> vid(static_cast<size_t>(ns), {-1, -1}); int made = 0;
    auto vOf = [&](int i, int k) { int& x = vid[size_t(i)][size_t(k)]; if (x < 0) { x = int(G.P.size()); G.P.push_back(rails[size_t(i)][size_t(k)]); G.ok.push_back(1); G.conf.push_back(0.15f); } return x; };
    for (int i = 0; i < (closed ? ns : ns - 1); i++) { const int j = (i + 1) % ns; if (!thin[size_t(i)] || !thin[size_t(j)]) continue;
      std::vector<int> q{vOf(i, 0), vOf(j, 0), vOf(j, 1), vOf(i, 1)};
      const Vec3 fn = (G.P[size_t(q[1])] - G.P[size_t(q[0])]).cross(G.P[size_t(q[3])] - G.P[size_t(q[0])]);
      if (fn.dot(outAt(srcP, srcN, (G.P[size_t(q[0])] + G.P[size_t(q[2])]) * 0.5)) < 0) std::reverse(q.begin(), q.end());
      G.fv.insert(G.fv.end(), q.begin(), q.end()); G.fo.push_back(int(G.fv.size())); G.fok.push_back(1); made++; }
    if (!made) continue;
    total += made;
    // (dropped: the ghost's polygons with a point on this part by its thin stretch — not the rest of a part that is thin
    // only in places, a strap cast in one piece with the leg)
    std::vector<Vec3> thinC; for (int i = 0; i < ns; i++) if (thin[size_t(i)]) thinC.push_back(cs[size_t(i)]);
    auto nearThin = [&](const Vec3& p) { for (const Vec3& q : thinC) if (p.distanceTo(q) <= L) return true; return false; };
    for (size_t f = 0; f < nf0; f++) for (int k = G.fo[f]; k < G.fo[f + 1]; k++) { const int v = G.fv[size_t(k)]; if (size_t(v) < land.size() && land[size_t(v)] == key && nearThin(G.P[size_t(v)])) { drop[f] = 1; break; } }
  }
  if (!total) return 0;
  for (size_t f = 0; f < nf0; f++) if (drop[f]) G.fok[f] = 0;
  std::set<std::pair<int, int>> es(G.edges.begin(), G.edges.end());
  for (size_t f = nf0; f + 1 < G.fo.size(); f++) for (int k = G.fo[f]; k < G.fo[f + 1]; k++) { const int a = G.fv[size_t(k)], b = G.fv[size_t(k + 1 < G.fo[f + 1] ? k + 1 : G.fo[f])]; es.insert(std::minmax(a, b)); }
  G.edges.assign(es.begin(), es.end());
  return total;
}
void ghostWork(GhostJob& J) {
  // (two tiers: the masked polygons first — the snap masks say where it belongs; the whole of the targets after)
  std::vector<std::shared_ptr<topo::Bvh>> tierM, tierA;
  for (auto& t : J.tgts) { if (J.cancel) return;
    auto a = std::make_shared<topo::Bvh>(); a->build(t.pos, t.tris); tierA.push_back(a);
    if (!t.mask.empty()) { std::vector<uint32_t> mt; for (size_t i = 0; i < t.t2f.size(); i++) if (size_t(t.t2f[i]) < t.mask.size() && t.mask[size_t(t.t2f[i])]) for (int j = 0; j < 3; j++) mt.push_back(t.tris[i * 3 + size_t(j)]);
      if (!mt.empty()) { auto b = std::make_shared<topo::Bvh>(); b->build(t.pos, mt); tierM.push_back(b); } } }
  std::vector<PieceMap> PM; for (auto& t : J.tgts) { if (J.cancel) return; PM.push_back(piecesOf(t)); }
  J.prog = 150; J.stage = 1;
  const double reachE = std::max(1e-3, J.o.dist);                // (the reach in the source's edge lengths — the same on a model of any scale)
  auto closest = [&](const std::vector<std::shared_ptr<topo::Bvh>>& tg, const Vec3& p, double* dist) -> std::optional<Vec3> { double bd = 1e300; std::optional<Vec3> best;
    for (auto& b : tg) { double d; if (auto q = b->closest(p, nullptr, &d); q && d < bd) { bd = d; best = q; } } if (dist) *dist = bd; return best; };
  size_t total = 0, doneN = 0; for (auto& s : J.srcs) total += s.P.size() * size_t(J.o.density == 2 ? 4 : 1);
  for (auto& S : J.srcs) {
    Viewport::Ghost G; G.src = S.id; std::vector<Vec3> P = S.P; G.fo = S.fo; G.fv = S.fv;
    if (J.o.density == 2) {                                         // (subdivided: a middle point on each edge and polygon, a quad round each corner)
      std::map<std::pair<int, int>, int> mid; std::vector<int> fo{0}, fv;
      auto em = [&](int a, int b) { auto k = std::minmax(a, b); auto r = mid.emplace(std::pair<int, int>{k.first, k.second}, int(P.size())); if (r.second) P.push_back((P[size_t(a)] + P[size_t(b)]) * 0.5); return r.first->second; };
      for (size_t f = 0; f + 1 < G.fo.size(); f++) { const int s = G.fo[f], e = G.fo[f + 1], n = e - s; Vec3 c; for (int k = s; k < e; k++) c += P[size_t(G.fv[size_t(k)])]; const int ci = int(P.size()); P.push_back(c * (1.0 / n));
        for (int k = 0; k < n; k++) { const int v = G.fv[size_t(s + k)], nx = G.fv[size_t(s + (k + 1) % n)], pv = G.fv[size_t(s + (k + n - 1) % n)];
          fv.push_back(v); fv.push_back(em(v, nx)); fv.push_back(ci); fv.push_back(em(pv, v)); fo.push_back(int(fv.size())); } }
      G.fo = fo; G.fv = fv;
    }
    const size_t n = P.size(); std::vector<std::vector<int>> adj(n); std::set<std::pair<int, int>> es; std::vector<Vec3> N(n);
    for (size_t f = 0; f + 1 < G.fo.size(); f++) { const int s = G.fo[f], e = G.fo[f + 1]; Vec3 fn;
      for (int k = s; k < e; k++) { const Vec3& a = P[size_t(G.fv[size_t(k)])]; const Vec3& b = P[size_t(G.fv[size_t(k + 1 < e ? k + 1 : s)])]; fn += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)); }
      for (int k = s; k < e; k++) { const int a = G.fv[size_t(k)], b = G.fv[size_t(k + 1 < e ? k + 1 : s)]; N[size_t(a)] += fn; if (es.insert(std::minmax(a, b)).second) { adj[size_t(a)].push_back(b); adj[size_t(b)].push_back(a); } } }
    G.edges.assign(es.begin(), es.end());
    for (auto& v : N) v = v.lengthSq() > 0 ? v.normalized() : Vec3(0, 1, 0);
    for (int it2 = 0; it2 < 2; it2++) { std::vector<Vec3> N2 = N; for (size_t v = 0; v < n; v++) { Vec3 s = N[v] * 2; for (int w : adj[v]) s += N[size_t(w)]; N2[v] = s.lengthSq() > 0 ? s.normalized() : N[v]; } N = N2; }   // (smoothed: the rays don't cross in the hollows)
    { double L = 0; for (auto& [a, b] : G.edges) L += P[size_t(a)].distanceTo(P[size_t(b)]); G.avgL = G.edges.empty() ? 1 : L / double(G.edges.size()); }
    const double D = std::max(1e-9, reachE * G.avgL);
    // ---- each point onto the targets: the masked first, then the rest — along its normal (both ways) within the reach,
    // else the nearest point ----
    G.P = P; G.ok.assign(n, 0); G.conf.assign(n, 1); std::vector<char> edgeOnly(n, 0), tier(n, 1);
    for (size_t v = 0; v < n; v++) {
      if ((v & 1023) == 0) { if (J.cancel) return; J.prog = 150 + int(700.0 * double(doneN + v) / double(std::max<size_t>(1, total))); }
      for (int ti = 0; ti < 2; ti++) { const auto& tg = ti == 0 ? tierM : tierA; if (tg.empty()) continue;
        double bt = 1e300; Vec3 hp; for (auto& b : tg) for (double sgn : {1.0, -1.0}) if (auto h = b->ray(P[v], N[v] * sgn, D); h && h->first < bt) { bt = h->first; hp = P[v] + N[v] * (sgn * h->first); }
        if (bt < 1e300) { G.P[v] = hp; G.ok[v] = 1; G.conf[v] = float(bt / D); tier[v] = char(ti); break; }
        double cd; auto cq = closest(tg, P[v], &cd);
        if (cq && cd <= D) { G.P[v] = *cq; G.ok[v] = 1; G.conf[v] = float(std::min(1.0, cd / D + 0.34)); tier[v] = char(ti);   // (missed — the nearest point: less sure)
          if (cd > 1e-6 * std::max(1.0, G.avgL) && std::abs((*cq - P[v]).dot(N[v])) < 0.7 * cd) edgeOnly[v] = 1;   // (off to the side of its normal: beyond the surface's edge)
          break; } }
    }
    doneN += n;
    J.stage = 2;
    // ---- relaxed on the surface it lies on: towards its neighbours' middle, back onto it — the folds smoothed out ----
    for (int r = 0; r < J.o.relax; r++) { if (J.cancel) return; std::vector<Vec3> Q = G.P;
      for (size_t v = 0; v < n; v++) { if (!G.ok[v] || edgeOnly[v]) continue; Vec3 c; int k = 0; for (int w : adj[v]) if (G.ok[size_t(w)]) { c += G.P[size_t(w)]; k++; } if (k < 2) continue;
        Vec3 p = G.P[v] * 0.5 + c * (0.5 / k); if (auto q = closest(tier[v] == 0 ? tierM : tierA, p, nullptr)) p = *q; Q[v] = p; }
      G.P = Q; J.prog = 850 + int(150.0 * (r + 1) / std::max(1, J.o.relax)); }
    J.stage = 3;
    // ---- a polygon whose inside dips under the surface (over a bump, a crease, another reference): its points lifted
    // along their normals so it lies over it — its middle and its sides' middles looked at ----
    for (int pass = 0; pass < 2 && !tierA.empty(); pass++) { if (J.cancel) return;
      std::vector<double> lift(n, 0.0); bool any = false; const double reach = G.avgL * 0.5, margin = G.avgL * 0.01;
      for (size_t f = 0; f + 1 < G.fo.size(); f++) { const int s = G.fo[f], e = G.fo[f + 1]; bool all = true; Vec3 c, fn;
        for (int k = s; k < e; k++) { const int v = G.fv[size_t(k)]; all = all && G.ok[size_t(v)]; c += G.P[size_t(v)];
          const Vec3& a = G.P[size_t(v)]; const Vec3& b = G.P[size_t(G.fv[size_t(k + 1 < e ? k + 1 : s)])]; fn += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)); }
        if (!all || fn.lengthSq() == 0) continue; c = c * (1.0 / (e - s)); fn = fn.normalized();
        Vec3 nAvg; for (int k = s; k < e; k++) nAvg += N[size_t(G.fv[size_t(k)])]; if (fn.dot(nAvg) < 0) fn = fn * -1.0;   // (outward: as the source's normals)
        double h = 0; std::vector<Vec3> smp{c}; for (int k = s; k < e; k++) smp.push_back((G.P[size_t(G.fv[size_t(k)])] + G.P[size_t(G.fv[size_t(k + 1 < e ? k + 1 : s)])]) * 0.5);
        for (const Vec3& p : smp) for (auto& b : tierA) if (auto hit = b->ray(p, fn, reach); hit && hit->first > 1e-9) {
          const size_t t = size_t(hit->second); const Vec3 tn = (b->P[b->idx[t * 3 + 1]] - b->P[b->idx[t * 3]]).cross(b->P[b->idx[t * 3 + 2]] - b->P[b->idx[t * 3]]);
          if (tn.dot(fn) > 0) h = std::max(h, hit->first); }   // (the surface's outside ahead: this part of it is under the surface)
        if (h > 0) { any = true; for (int k = s; k < e; k++) { double& l = lift[size_t(G.fv[size_t(k)])]; l = std::max(l, h + margin); } } }
      if (!any) break;
      for (size_t v = 0; v < n; v++) if (lift[v] > 0) G.P[v] += N[v] * lift[v];
    }
    // ---- the polygons: all its points reached; trimmed — not one only on the edge of the surface ----
    G.fok.assign(G.fo.size() - 1, 0);
    for (size_t f = 0; f + 1 < G.fo.size(); f++) { bool all = true, inner = false; for (int k = G.fo[f]; k < G.fo[f + 1]; k++) { const int v = G.fv[size_t(k)]; all = all && G.ok[size_t(v)]; inner = inner || !edgeOnly[size_t(v)]; }
      G.fok[f] = all && (!J.o.trim || inner); }
    if (tierA.size() == J.tgts.size()) bandStrips(G, P, N, J.tgts, tierA, PM, J.cancel);   // (thin parts: a strip of quads along them)
    { const size_t nAll = G.P.size(); std::vector<char> used(nAll, 0); std::set<std::pair<int, int>> ve;   // (the points and edges of the polygons shown)
      for (size_t f = 0; f < G.fok.size(); f++) if (G.fok[f]) { const int s = G.fo[f], e = G.fo[f + 1]; for (int k = s; k < e; k++) { const int a = G.fv[size_t(k)], b = G.fv[size_t(k + 1 < e ? k + 1 : s)]; used[size_t(a)] = 1; ve.insert(std::minmax(a, b)); } }
      for (size_t v = 0; v < nAll; v++) if (used[v]) G.vis.push_back(int(v)); G.visE.assign(ve.begin(), ve.end()); }
    J.out.push_back(std::move(G));
  }
}
}  // namespace

// the ghosts as they are; a source, a target or a setting changed — made again in a thread of its own (the old ones shown
// meanwhile; the window dimmed a little, the inspector thinking in its middle while it takes long)
const std::vector<Viewport::Ghost>& Viewport::ghosts() const {
  ProfScope prof_("ghosts");
  if (room() != "topo") { static const std::vector<Ghost> none; return none; }
  if (drag_ == Drag::Topo && td_.type == 4) return ghosts_;     // (a snap mask being painted: made again when the stroke ends)
  // ---- what it is made from: the sources (their geometry, place), the targets (theirs, masks, shown), the settings ----
  const int am = topoMesh(); bool anyRef = false;
  for (auto& [id, it] : scene_->items) if (it.type == ItemType::IMesh && it.poly && it.raw.value("tref").toBool() && it.visible && !it.raw.value("tghost").toBool()) anyRef = true;   // (its own switch — an isolation for a look doesn'"'"'t make it again)
  std::vector<int> srcs, tgts; QByteArray key = QByteArray::number(gho.dist) + "|" + QByteArray::number(gho.relax) + QByteArray::number(gho.density) + QByteArray::number(gho.trim) + "|";
  for (auto& [id, it] : scene_->items) {
    if (it.type != ItemType::IMesh || !(it.base || it.poly)) continue;
    const auto gs = geomStamp_.count(id) ? geomStamp_.at(id) : 0;
    if (it.raw.value("tghost").toBool()) { srcs.push_back(id); const Affine M = meshmods::meshMatrix(*scene_, it);
      key += "s" + QByteArray::number(id) + ":" + QByteArray::number(quintptr(baseOf(it))) + ":" + QByteArray::number(gs);
      for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) key += "," + QByteArray::number(M.L[r][c], 'g', 9);
      key += "," + QByteArray::number(M.t.x, 'g', 9) + "," + QByteArray::number(M.t.y, 'g', 9) + "," + QByteArray::number(M.t.z, 'g', 9); continue; }
    if (id == am || !it.poly || !it.visible || (anyRef && !it.raw.value("tref").toBool())) continue;
    tgts.push_back(id);
    key += "t" + QByteArray::number(id) + ":" + QByteArray::number(quintptr(it.poly.get())) + ":" + QByteArray::number(gs) + ":" + QByteArray::number(it.raw.value("smRev").toDouble(), 'f', 0) + QByteArray::number(int(it.raw.contains("smask"))) + QByteArray::number(it.raw.value("smOff").toBool());
  }
  if (key == ghostKey_ || (ghostJob_ && ghostJob_->key == key)) return ghosts_;
  for (auto& [k, g] : ghostRecent_) if (k == key) { if (ghostJob_) { ghostJob_->cancel = true; ghostJob_.reset(); } ghostThink_.reset(); ghosts_ = g; ghostKey_ = key; ghostDrawKey_.clear(); return ghosts_; }   // (one made a moment ago — an isolation and back)
  if (ghostJob_) { ghostJob_->cancel = true; ghostJob_.reset(); }
  if (srcs.empty() || tgts.empty()) { ghostKey_ = key; ghosts_.clear(); ghostThink_.reset(); return ghosts_; }
  // ---- the job: copies of what it reads (the scene may change meanwhile) ----
  auto J = std::make_shared<GhostJob>(); J->key = key; J->o = gho;
  for (int id : tgts) { const Item* it = scene_->item(id); const PickCache* pc = pickCacheOf(id); if (!it || !pc) continue;
    GhostJob::Tgt t; t.pos = it->poly->pos; t.tris = pc->tris; t.t2f = pc->tri2f; if (!it->raw.value("smOff").toBool()) t.mask = smArr(id); J->tgts.push_back(std::move(t)); }
  for (int sid : srcs) { const Item* it = scene_->item(sid); const PolyMesh& B = *baseOf(*it); const Affine M = meshmods::meshMatrix(*scene_, *it);
    GhostJob::Src s; s.id = sid; for (auto& p : B.pos) s.P.push_back(M.apply(p));
    for (int f = 0; f < B.faceCount(); f++) { s.fo.push_back(int(s.fv.size())); for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) s.fv.push_back(B.faceVerts[size_t(k)]); }
    s.fo.push_back(int(s.fv.size())); J->srcs.push_back(std::move(s)); }
  ghostJob_ = J; std::thread([J] { ghostWork(*J); J->done = true; }).detach();
  ghostThink_ = std::make_shared<ui::ThinkScope>(tr("Laying the Topology Ghost on the surface"));
  auto* self = const_cast<Viewport*>(this);
  if (!ghostPoll_) { ghostPoll_ = new QTimer(self); ghostPoll_->setInterval(80);
    QObject::connect(ghostPoll_, &QTimer::timeout, self, [self] {
      auto& job = self->ghostJob_; if (!job) { self->ghostPoll_->stop(); self->ghostThink_.reset(); return; }
      if (!job->done) { if (self->ghostThink_) { static const char* st[] = {QT_TR_NOOP("Laying the Topology Ghost: the references' surfaces sorted (the masked parts apart)"), QT_TR_NOOP("Laying the Topology Ghost: its points onto the surface — the snap mask first"), QT_TR_NOOP("Laying the Topology Ghost: relaxing it along the surface"), QT_TR_NOOP("Laying the Topology Ghost: lifting the polygons that dip under the surface")}; self->ghostThink_->progress(job->prog / 1000.0, tr(st[std::clamp(int(job->stage), 0, 3)])); } return; }
      if (!job->cancel) { self->ghosts_ = std::move(job->out); self->ghostKey_ = job->key; self->ghostDrawKey_.clear();
        self->ghostRecent_.push_back({job->key, self->ghosts_}); if (self->ghostRecent_.size() > 3) self->ghostRecent_.erase(self->ghostRecent_.begin()); }
      job.reset(); self->ghostThink_.reset(); self->ghostPoll_->stop(); self->topoRev_++; self->overlayRev_++; self->requestUpdate(); }); }
  ghostPoll_->start();
  return ghosts_;
}

// the ghost drawn: its edges by confidence (green — close, yellow — stretched, red — doubtful), its points; made once per
// ghost (not pulled nearer by the PDO — on the references)
void Viewport::ghostDraw(TopoDraw& D) const {
  if (!gho.show) return;
  const auto& Gs = ghosts(); if (Gs.empty()) return;
  if (ghostDrawKey_ != ghostKey_) {
    ghostDrawKey_ = ghostKey_; ghostDrawn_ = {};
    TopoDraw::Lines L[3]; const QColor C[3] = {QColor(0x5a, 0xd1, 0x7a), QColor(0xf2, 0xc1, 0x4e), QColor(0xe5, 0x53, 0x4b)};
    for (int i = 0; i < 3; i++) { L[i].c = C[i]; L[i].w = 1.4f; L[i].noPdo = true; }
    TopoDraw::Pts pt; pt.size = 4; pt.noPdo = true;
    for (const Ghost& G : Gs) {
      for (auto& [a, b] : G.visE) { const float c = std::max(G.conf[size_t(a)], G.conf[size_t(b)]); auto& l = L[c < 0.34f ? 0 : c < 0.67f ? 1 : 2]; l.p.push_back(G.P[size_t(a)]); l.p.push_back(G.P[size_t(b)]); }
      for (int v : G.vis) { pt.p.push_back(G.P[size_t(v)]); const float c = G.conf[size_t(v)]; pt.c.push_back(C[c < 0.34f ? 0 : c < 0.67f ? 1 : 2]); }
    }
    for (auto& l : L) if (!l.p.empty()) ghostDrawn_.lines.push_back(std::move(l));
    if (!pt.p.empty()) ghostDrawn_.pts.push_back(std::move(pt));
  }
  D.lines.insert(D.lines.end(), ghostDrawn_.lines.begin(), ghostDrawn_.lines.end());
  D.pts.insert(D.pts.end(), ghostDrawn_.pts.begin(), ghostDrawn_.pts.end());
}

bool Viewport::ghostVertAt(const QPointF& mp, double px, Vec3& out) const {
  ProfScope prof_("ghostVertAt");
  if (!gho.show) return false;
  Projector P = projector(); double bd = px; bool hit = false;
  for (const Ghost& G : ghosts()) for (int v : G.vis) { const ScreenPt s = P.toScreen(G.P[size_t(v)]); if (s.z >= 1) continue; const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (d < bd) { bd = d; out = G.P[size_t(v)]; hit = true; } }
  return hit;
}
bool Viewport::ghostEdgeAt(const QPointF& mp, double px, Vec3& out) const {
  ProfScope prof_("ghostEdgeAt");
  if (!gho.show) return false;
  Projector P = projector(); double bd = px; bool hit = false;
  for (const Ghost& G : ghosts()) for (auto& [a, b] : G.visE) { const ScreenPt sa = P.toScreen(G.P[size_t(a)]), sb = P.toScreen(G.P[size_t(b)]); if (sa.z >= 1 || sb.z >= 1) continue;
    if (std::min(sa.x, sb.x) > mp.x() + px || std::max(sa.x, sb.x) < mp.x() - px || std::min(sa.y, sb.y) > mp.y() + px || std::max(sa.y, sb.y) < mp.y() - px) continue;
    const double dx = sb.x - sa.x, dy = sb.y - sa.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? std::clamp(((mp.x() - sa.x) * dx + (mp.y() - sa.y) * dy) / l2, 0.0, 1.0) : 0;
    const double d = std::hypot(sa.x + dx * t - mp.x(), sa.y + dy * t - mp.y()); if (d < bd) { bd = d; out = G.P[size_t(a)] + (G.P[size_t(b)] - G.P[size_t(a)]) * t; hit = true; } }
  return hit;
}
bool Viewport::ghostFaceAt(const QPointF& mp, std::vector<Vec3>& out) const {
  if (!gho.show) return false;
  Projector P = projector(); double bz = 1e300; bool hit = false;
  for (const Ghost& G : ghosts()) for (size_t f = 0; f < G.fok.size(); f++) { if (!G.fok[f]) continue;
    std::vector<std::pair<double, double>> poly; double z = 0; bool vis = true;
    for (int k = G.fo[f]; k < G.fo[f + 1]; k++) { const ScreenPt s = P.toScreen(G.P[size_t(G.fv[size_t(k)])]); if (s.z >= 1) vis = false; poly.push_back({s.x, s.y}); z += s.z; }
    if (!vis || !insidePolygon(poly, mp.x(), mp.y())) continue; z /= double(poly.size());
    if (z < bz) { bz = z; out.clear(); for (int k = G.fo[f]; k < G.fo[f + 1]; k++) out.push_back(G.P[size_t(G.fv[size_t(k)])]); hit = true; } }
  return hit;
}

QString Viewport::ghostInfo() const {
  const auto& Gs = ghosts(); int f = 0, all = 0, v = 0; for (auto& G : Gs) { all += int(G.fok.size()); for (char x : G.fok) f += x; for (char x : G.ok) v += x; }
  return QString("ghosts %1 polys %2/%3 verts %4 edge %5").arg(Gs.size()).arg(f).arg(all).arg(v).arg(Gs.empty() ? 0.0 : Gs[0].avgL, 0, 'g', 4);
}

// ---------- the Reveal Brush: the ghost's polygons under the stroke gathered, made real at its end ----------
void Viewport::revealGather(const QPointF& mp) {
  Projector P = projector(); const double R = brushR; const auto& Gs = ghosts(); bool more = false;
  bool stale = ghostMidKey_ != ghostKey_ || ghostMid_.size() != Gs.size(); for (size_t gi = 0; !stale && gi < Gs.size(); gi++) stale = ghostMid_[gi].size() != Gs[gi].fok.size();
  if (stale) {   // (the ghost polygons' middles: once per ghost)
    ghostMidKey_ = ghostKey_; ghostMid_.assign(Gs.size(), {});
    for (size_t gi = 0; gi < Gs.size(); gi++) { const Ghost& G = Gs[gi]; auto& C = ghostMid_[gi]; C.resize(G.fok.size());
      for (size_t f = 0; f < G.fok.size(); f++) { Vec3 c; const int s = G.fo[f], e = G.fo[f + 1]; for (int k = s; k < e; k++) c += G.P[size_t(G.fv[size_t(k)])]; C[f] = c * (1.0 / std::max(1, e - s)); } } }
  // (what may hide a ghost polygon: the shown references — through their hierarchies, a high-poly one's only when ready)
  int tm = topoMesh(); if (const Item* t = tm >= 0 ? scene_->item(tm) : nullptr; !t || t->raw.value("tghost").toBool() || topoHeavy(tm)) tm = -1;   // (the mesh being built — not a reference)
  std::vector<std::pair<int, std::shared_ptr<const topo::Bvh>>> occ;
  for (auto& [id, x] : scene_->items) { if (id == tm || x.type != ItemType::IMesh || !x.poly || !scene_->isVisible(x)) continue;
    if (heavyRay(id, Ray{cam_.pos, cam_.dir()}, false)) occ.push_back({id, nullptr});
    else if ((long long)x.poly->faceVerts.size() - 2LL * x.poly->faceCount() < 200000) { if (auto b = bvhOf(id, false)) occ.push_back({id, b}); } }
  auto hidden = [&](const Vec3& eye, const Vec3& c, double tol) {
    const Vec3 d = (c - eye).normalized(); const double L = eye.distanceTo(c) - tol; if (L <= 0) return false;
    for (auto& [id, b] : occ) { if (b) { if (b->ray(eye, d, L)) return true; continue; }
      if (auto hr = heavyRay(id, Ray{eye, d}, false); hr && *hr && (*hr)->t < L) return true; }
    return false; };
  const bool ortho = cam_.ortho(); const Vec3 cd = cam_.dir();
  for (size_t gi = 0; gi < Gs.size(); gi++) { const Ghost& G = Gs[gi]; const auto& C = ghostMid_[gi];
    for (size_t f = 0; f < G.fok.size(); f++) { if (!G.fok[f] || revealFaces_.count({int(gi), int(f)})) continue;
      const ScreenPt sc = P.toScreen(C[f]); if (sc.z >= 1) continue;
      bool in = std::hypot(sc.x - mp.x(), sc.y - mp.y()) <= R;          // (all of the circle: its middle or a corner of it inside — hardness 100%)
      for (int k = G.fo[f]; k < G.fo[f + 1] && !in; k++) { const ScreenPt q = P.toScreen(G.P[size_t(G.fv[size_t(k)])]); in = q.z < 1 && std::hypot(q.x - mp.x(), q.y - mp.y()) <= R; }
      if (!in) continue;
      // (only what the camera sees: a polygon behind a surface — its source's or another reference's — left; not through)
      const Vec3 eye = ortho ? C[f] - cd * (cam_.pos.distanceTo(C[f]) + 1) : cam_.pos;
      if (hidden(eye, C[f], G.avgL * 0.5)) continue;
      revealFaces_.insert({int(gi), int(f)}); more = true; } }
  if (more) { if (const qint64 now = QDateTime::currentMSecsSinceEpoch(); now - revealShown_ > 40) { revealShown_ = now; topoRev_++; requestUpdate(); } else overlayRev_++; }
}

QString Viewport::revealApply() {
  const auto faces = revealFaces_; revealFaces_.clear(); topoRev_++;
  if (faces.empty()) return {};
  const auto& Gs = ghosts();
  int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (it && (topoHeavy(m) || it->raw.value("tghost").toBool())) { ui::toast(tr("Pick the mesh being built (not the ghost source, not a high-poly one) — or none: a new one is made")); return "!"; }
  // (a ghost point onto what is built there — a vertex of the mesh within a third of the ghost's edges — else a new one)
  std::vector<Vec3> W; Affine M, Mi; if (it) { M = meshmods::meshMatrix(*scene_, *it); Mi = M.inverse(); for (auto& p : baseOf(*it)->pos) W.push_back(M.apply(p)); }
  double avgL = 1; for (auto& G : Gs) avgL = std::min(avgL == 1 ? G.avgL : avgL, G.avgL); const double tol = avgL / 3;
  std::unordered_map<long long, std::vector<int>> grid; auto key = [&](const Vec3& p) { const double c = std::max(1e-9, tol); return ((long long)std::floor(p.x / c) * 73856093LL) ^ ((long long)std::floor(p.y / c) * 19349663LL) ^ ((long long)std::floor(p.z / c) * 83492791LL); };
  auto cellKey = [&](const Vec3& p, int dx, int dy, int dz) { const double c = std::max(1e-9, tol); return ((long long)(std::floor(p.x / c) + dx) * 73856093LL) ^ ((long long)(std::floor(p.y / c) + dy) * 19349663LL) ^ ((long long)(std::floor(p.z / c) + dz) * 83492791LL); };
  for (size_t v = 0; v < W.size(); v++) grid[key(W[v])].push_back(int(v));
  MeshEd E = it ? MeshEd::from(*baseOf(*it)) : MeshEd{};
  std::map<std::pair<int, int>, int> made;                         // (ghost, its point → the mesh's vertex)
  auto vertOf = [&](int gi, int gv) {
    auto f = made.find({gi, gv}); if (f != made.end()) return f->second;
    const Vec3 w = Gs[size_t(gi)].P[size_t(gv)]; int best = -1; double bd = tol;
    for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) { auto g = grid.find(cellKey(w, dx, dy, dz)); if (g == grid.end()) continue;
      for (int v : g->second) { const double d = (size_t(v) < W.size() ? W[size_t(v)] : M.apply(E.pos[size_t(v)])).distanceTo(w); if (d < bd) { bd = d; best = v; } } }
    if (best < 0) { best = E.addVert(it ? Mi.apply(w) : w); grid[key(w)].push_back(best); }
    made[{gi, gv}] = best; return best; };
  std::vector<int> newF; int skipped = 0;
  for (auto [gi, f] : faces) { const Ghost& G = Gs[size_t(gi)]; if (size_t(f) >= G.fok.size()) continue;
    std::vector<int> vs; for (int k = G.fo[size_t(f)]; k < G.fo[size_t(f) + 1]; k++) { const int v = vertOf(gi, G.fv[size_t(k)]); if (vs.empty() || (vs.back() != v && vs.front() != v)) vs.push_back(v); }
    if (vs.size() < 3) { skipped++; continue; }
    bool exists = false; for (int x : E.facesOfVert(vs[0])) { std::set<int> a(E.f[size_t(x)].v.begin(), E.f[size_t(x)].v.end()), b(vs.begin(), vs.end()); if (a == b) exists = true; }
    if (exists) { skipped++; continue; }
    newF.push_back(E.addFace(vs)); }
  if (newF.empty()) { ui::hud(tr("Reveal: those polygons are built already"), 900); return "!"; }
  const QString msg = tr("Revealed: %1 polygon%2 from the ghost").arg(newF.size()).arg(newF.size() == 1 ? "" : "s");
  if (it) { tt_.symNew = true; return ttDone(m, E, *baseOf(*it), msg, 'f', newF); }   // (Symmetry: the polygons made — mirrored exactly)
  // (no mesh picked: a new one of them — RetopoMesh)
  if (tset.sym && room() == "topo") symAddFaces(E, 0);   // (Symmetry: their exact mirror too)
  PolyMesh src; src.groupNames = {"Polygroup"}; PolyMesh d = E.to(src); if (d.faceGroup.size() != size_t(d.faceCount())) d.faceGroup.assign(size_t(d.faceCount()), 0); if (d.groupNames.empty()) d.groupNames = {"Polygroup"};
  std::set<QString> taken; for (auto& [id, x] : scene_->items) taken.insert(x.name);
  QString name = "RetopoMesh"; for (int i = 2; taken.count(name); i++) name = QString("RetopoMesh %1").arg(i);
  const int id = topoNewMesh(-1, name, d); if (id < 0) return "!";
  sel_ = {}; sel_.items = {id}; onSelChange(); scene_->revision++; emit sceneEdited(); commit(true); emit topoState();
  ui::toast(tr("%1: a new mesh — %2").arg(name, msg)); return msg;
}

}  // namespace hm
