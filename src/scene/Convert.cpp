#include "scene/Convert.h"
#include "scene/Braid.h"
#include "scene/Crown.h"
#include "scene/TopoEdit.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace hm {

namespace {

const double PI = 3.14159265358979323846, INF = std::numeric_limits<double>::infinity();
const int ROW_S = -1, ROW_E = -2;                               // a column's start / end ('S' / 'E'); ≥ 0 — a ring
using I64 = long long;

double jsRound(double x) { return std::floor(x + 0.5); }
int clampi(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }
double angleTo(const Vec3& a, const Vec3& b) {
  double d = std::sqrt(a.lengthSq() * b.lengthSq());
  return d == 0 ? PI / 2 : std::acos(clamp(a.dot(b) / d, -1, 1));
}
double nearestF(const std::vector<Vec3>& P, const Vec3& p) {
  double best = 0, bd = INF;
  for (size_t i = 0; i + 1 < P.size(); i++) {
    const Vec3& a = P[i]; double bx = P[i + 1].x - a.x, by = P[i + 1].y - a.y, bz = P[i + 1].z - a.z, l2 = bx * bx + by * by + bz * bz;
    double s = l2 > 0 ? ((p.x - a.x) * bx + (p.y - a.y) * by + (p.z - a.z) * bz) / l2 : 0; s = s < 0 ? 0 : s > 1 ? 1 : s;
    double dx = a.x + bx * s - p.x, dy = a.y + by * s - p.y, dz = a.z + bz * s - p.z, d = dx * dx + dy * dy + dz * dz;
    if (d < bd) { bd = d; best = double(i) + s; }
  }
  return best;
}
double cocoonF(const CocoonGeo& G, double s) {
  int i = 0; while (i < G.N - 2 && G.D[i + 1] < s) i++;
  double L = G.D[i + 1] - G.D[i];
  return i + (L > 0 ? clamp((s - G.D[i]) / L, 0, 1) : 0);
}
double distToSeg(const Vec3& p, const Vec3& a, const Vec3& b) {
  Vec3 ab = b - a; double l2 = ab.lengthSq(), t = l2 > 0 ? clamp((p - a).dot(ab) / l2, 0, 1) : 0;
  return (a + ab * t).distanceTo(p);
}
Vec3 faceNormal(const std::vector<Vec3>& pos, const std::vector<int>& f) {   // Newell
  Vec3 n;
  for (size_t i = 0; i < f.size(); i++) { const Vec3 &a = pos[f[i]], &b = pos[f[(i + 1) % f.size()]]; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
  return n;
}
Vec3 faceCenter(const std::vector<Vec3>& pos, const std::vector<int>& f) { Vec3 c; for (int i : f) c += pos[i]; return c / double(f.size()); }
// a face's corners: one in the same point as the one before dropped, then each point once (the JS chain of filters)
std::vector<int> cleanFace(const std::vector<int>& m) {
  std::vector<int> g; size_t n = m.size();
  for (size_t q = 0; q < n; q++) if (m[q] != m[(q + n - 1) % n]) g.push_back(m[q]);
  std::vector<int> u; for (int v : g) if (std::find(u.begin(), u.end(), v) == u.end()) u.push_back(v);
  return u;
}

struct Mesh { std::vector<Vec3> pos; std::vector<std::vector<int>> faces; };

// Weld points closer than d into one (their middle). Two corners of one face (or two points with a neighbour in
// common) are never welded; tips — the tip row: welded only closer than tipD, and with each other only when tipCollapse
// (a lock of one surface); openOnly — only points on an open edge (a crack, a loose end). → the points merged
struct WeldOpts { std::unordered_set<int> tips; double tipD = 0; bool tipCollapse = false, openOnly = false; };
int weldPoints(Mesh& data, double d, const WeldOpts& o) {
  const auto& pos = data.pos; const auto& faces = data.faces; int N = int(pos.size());
  if (!(d > 0) || N < 2) return 0;
  std::unordered_set<I64> adj; std::vector<std::unordered_set<int>> nb(N);
  for (auto& f : faces) for (int a : f) for (int b : f) if (a != b) { nb[a].insert(b); if (a < b) adj.insert(I64(a) * N + b); }
  auto near = [&](int i, int j) {
    if (adj.count(I64(i) * N + j)) return true;
    bool si = nb[i].size() < nb[j].size(); const auto& s = si ? nb[i] : nb[j]; const auto& l = si ? nb[j] : nb[i];
    for (int x : s) if (l.count(x)) return true;
    return false;
  };
  std::vector<char> open;
  if (o.openOnly) {
    std::unordered_map<I64, int> ec;
    for (auto& f : faces) for (size_t q = 0; q < f.size(); q++) { int a = f[q], b = f[(q + 1) % f.size()]; ec[a < b ? I64(a) * N + b : I64(b) * N + a]++; }
    open.assign(N, 0); for (auto& [k, c] : ec) if (c == 1) { open[size_t(k / N)] = 1; open[size_t(k % N)] = 1; }
  }
  std::vector<int> par(N); for (int i = 0; i < N; i++) par[i] = i;
  auto find = [&](int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; };
  std::map<std::array<I64, 3>, std::vector<int>> cell;
  auto cellOf = [&](const Vec3& p) { return std::array<I64, 3>{I64(std::floor(p.x / d)), I64(std::floor(p.y / d)), I64(std::floor(p.z / d))}; };
  for (int i = 0; i < N; i++) cell[cellOf(pos[i])].push_back(i);
  int merged = 0;
  for (int i = 0; i < N; i++) {
    const Vec3& p = pos[i]; auto c = cellOf(p);
    for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
      auto it = cell.find({c[0] + dx, c[1] + dy, c[2] + dz}); if (it == cell.end()) continue;
      for (int j : it->second) {
        if (j <= i) continue;
        if (o.openOnly && !(open[i] && open[j])) continue;
        bool ti = o.tips.count(i) > 0, tj = o.tips.count(j) > 0;
        if (ti && tj && !o.tipCollapse) continue;                 // (a thick lock's tip keeps its thickness)
        if (!(ti && tj) && near(i, j)) continue;                  // (a one-surface lock's tip may close into a point)
        if (p.distanceTo(pos[j]) >= (ti || tj ? o.tipD : d)) continue;
        int a = find(i), b = find(j); if (a != b) { par[b] = a; merged++; }
      }
    }
  }
  if (!merged) return 0;
  std::vector<int> rootOrder; std::unordered_map<int, std::pair<Vec3, int>> sum;
  for (int i = 0; i < N; i++) { int r = find(i); auto it = sum.find(r); if (it == sum.end()) { it = sum.emplace(r, std::make_pair(Vec3{}, 0)).first; rootOrder.push_back(r); } it->second.first += pos[i]; it->second.second++; }
  std::unordered_map<int, int> idx; std::vector<Vec3> out;
  for (int r : rootOrder) { idx[r] = int(out.size()); out.push_back(sum[r].first / double(sum[r].second)); }
  std::vector<std::vector<int>> nf;
  for (auto& f : faces) { std::vector<int> m; for (int i : f) m.push_back(idx[find(i)]); auto g = cleanFace(m); if (g.size() >= 3) nf.push_back(std::move(g)); }
  data.pos = std::move(out); data.faces = std::move(nf);
  return merged;
}

// Normals: the faces of each connected piece turned to agree across their shared edges (one walks an edge a → b, the
// other b → a; only across an edge of exactly two faces), then each piece turned to face out from the cocoon's axis
// (most of its area). → faces turned
int orientFaces(Mesh& data, const CocoonGeo& G) {
  auto& pos = data.pos; auto& faces = data.faces;
  I64 NV = I64(pos.size()) + 1;
  auto ek = [&](int a, int b) { return a < b ? I64(a) * NV + b : I64(b) * NV + a; };
  std::unordered_map<I64, std::vector<int>> edgeF;
  for (int fi = 0; fi < int(faces.size()); fi++) { auto& f = faces[fi]; for (size_t q = 0; q < f.size(); q++) edgeF[ek(f[q], f[(q + 1) % f.size()])].push_back(fi); }
  std::vector<char> seen(faces.size(), 0); int flips = 0;
  auto dir = [](const std::vector<int>& f, int a, int b) { for (size_t q = 0; q < f.size(); q++) if (f[q] == a && f[(q + 1) % f.size()] == b) return 1; return -1; };
  for (int s0 = 0; s0 < int(faces.size()); s0++) {
    if (seen[s0]) continue; seen[s0] = 1; std::vector<int> comp{s0}, st{s0};
    while (!st.empty()) {
      int fi = st.back(); st.pop_back(); const std::vector<int> f = faces[fi];
      for (size_t q = 0; q < f.size(); q++) {
        int a = f[q], b = f[(q + 1) % f.size()];
        auto it = edgeF.find(ek(a, b)); if (it == edgeF.end() || it->second.size() != 2) continue;
        for (int gi : it->second) {
          if (seen[gi]) continue; seen[gi] = 1; comp.push_back(gi); st.push_back(gi);
          if (dir(faces[gi], a, b) == 1) { std::reverse(faces[gi].begin(), faces[gi].end()); flips++; }
        }
      }
    }
    double out = 0;
    for (int fi : comp) { Vec3 n = faceNormal(pos, faces[fi]), c = faceCenter(pos, faces[fi]); int j = clampi(int(jsRound(nearestF(G.c, c))), 0, G.N - 1); out += n.dot(c - G.c[j]); }
    if (out < 0) { for (int fi : comp) std::reverse(faces[fi].begin(), faces[fi].end()); flips += int(comp.size()); }
  }
  return flips;
}

}  // namespace

WeldSettings weldSettings(const Scene& sc) {
  WeldSettings w; QJsonObject o = sc.fileRaw.value("weld").toObject();
  if (o.contains("on")) w.on = o.value("on").toBool(true);
  w.dist = o.value("dist").toDouble(0);
  return w;
}

double sceneScale(const Scene& sc) {
  Vec3 s = sc.bounds().size();
  double m = std::max({s.x, s.y, s.z}); return m > 0 ? m : 1;
}

// ======================= ringMeshOf: a lock's mesh from its cocoon's rings =======================
LockMesh ringMeshOf(const LockInput& in, const std::map<int, WallMesh>& wallOf, const WeldSettings& weld, double scale) {
  LockMesh res; const CocoonGeo& G = in.G; const double h = in.h; const auto& rings = in.rings; const auto& tipP = in.tipP;
  res.ringList = rings;
  // the walls and their twins (wallsOf), each a grid of its wall's mesh; a twin runs back from b to a
  struct WallRef { int id, a, b; bool twin; const WallMesh* m; const WallMesh::Grid* g; };
  std::vector<WallRef> walls;
  for (auto& w : in.walls) {
    auto it = wallOf.find(w.id);
    if (it == wallOf.end() || it->second.grids.empty()) { walls.push_back({w.id, w.a, w.b, false, nullptr, nullptr}); continue; }
    for (auto& g : it->second.grids) walls.push_back({w.id, g.twin ? w.b : w.a, g.twin ? w.a : w.b, g.twin, &it->second, &g});
  }
  if (walls.empty()) { res.err = "no walls"; return res; }

  // the columns: { key, P (root → tip), A (arc) }; a curve's column once (key 'c' + its id)
  struct Col { std::string key; std::vector<Vec3> P; std::vector<double> A; };
  std::vector<Col> cols; std::unordered_map<std::string, int> colIx;
  std::vector<std::vector<int>> wallCols;
  auto minTip = [&](const Vec3& p) { double m = INF; for (auto& t : tipP) m = std::min(m, t.distanceTo(p)); return m; };
  for (const WallRef& wr : walls) {
    if (!wr.g) continue;
    const WallMesh::Grid& g = *wr.g; const Vec3* d = wr.m->pos.data() + g.start;
    int M = g.M, NR = g.R; if (NR < 2) continue;
    // (a wall running on past the cocoon's tip — its edge curves passing the point where the strands end: its rows past
    // it cut off, the lock ends there)
    int R0_ = 0;
    if (!tipP.empty()) for (int j : {0, M - 1}) {
      int br = -1; double bd = 0.3 * h;
      for (int r = 1; r < NR - 1; r++) { double dd = minTip(d[r * M + j]); if (dd < bd) { bd = dd; br = r; } }
      if (br < 0) continue;
      bool atEnd = nearestF(G.c, d[(NR - 1) * M + j]) > nearestF(G.c, d[j]);   // (the rows past it — toward the tip)
      const Vec3& endPt = atEnd ? d[(NR - 1) * M + j] : d[j]; if (minTip(endPt) < 0.5 * h) continue;
      if (atEnd) NR = std::min(NR, br + 1); else R0_ = std::max(R0_, br);
    }
    if (NR - R0_ < 2) continue;
    std::vector<int> keys;
    for (int j = 0; j < M; j++) {
      std::string key = j == 0 ? "c" + std::to_string(wr.a) : j == M - 1 ? "c" + std::to_string(wr.b)
                                : "w" + std::to_string(wr.id) + (wr.twin ? "t" : "") + ":" + std::to_string(j);
      auto f = colIx.find(key); if (f != colIx.end()) { keys.push_back(f->second); continue; }
      Col c; c.key = key;
      for (int r = R0_; r < NR; r++) c.P.push_back(d[r * M + j]);
      if (nearestF(G.c, c.P.front()) > nearestF(G.c, c.P.back())) std::reverse(c.P.begin(), c.P.end());   // (root first)
      c.A.push_back(0); for (size_t i = 1; i < c.P.size(); i++) c.A.push_back(c.A[i - 1] + c.P[i].distanceTo(c.P[i - 1]));
      colIx[key] = int(cols.size()); keys.push_back(int(cols.size())); cols.push_back(std::move(c));
    }
    wallCols.push_back(keys);
  }
  if (wallCols.empty()) { res.err = "no walls built"; return res; }
  const int nC = int(cols.size()), nR = int(rings.size());

  // where a ring's plane crosses a column, near the ring: its length along the column (none — it doesn't)
  auto cross = [&](const Col& c, const CocoonRing& R, double lim) -> std::optional<double> {
    Vec3 PP = R.pp ? *R.pp : R.p, PN = R.pn ? *R.pn : R.n; std::optional<double> best; double bd = INF;
    for (size_t i = 0; i + 1 < c.P.size(); i++) {
      double d0 = (c.P[i] - PP).dot(PN), d1 = (c.P[i + 1] - PP).dot(PN); if (d0 == d1 || d0 * d1 > 0) continue;
      double f = d0 / (d0 - d1); double dd = c.P[i].lerp(c.P[i + 1], f).distanceTo(R.p); if (dd > lim || dd >= bd) continue;
      bd = dd; best = c.A[i] + (c.A[i + 1] - c.A[i]) * f;
    }
    return best;
  };
  const double gap = 0.15 * h;
  std::vector<double> lims(nR);
  for (int r = 0; r < nR; r++) { const CocoonRing& R = rings[r]; int i = clampi(int(jsRound(cocoonF(G, R.s))), 0, G.N - 1); lims[r] = R.lim ? *R.lim : std::max(2.5 * std::max({G.a[i], G.b[i], 1e-3}), 0.5 * h); }
  std::vector<signed char> xcSet(size_t(nC) * nR, 0); std::vector<double> xcVal(size_t(nC) * nR, 0);
  auto crossAt = [&](int ci, int ri) -> std::optional<double> {
    size_t k = size_t(ci) * nR + ri;
    if (!xcSet[k]) { auto v = cross(cols[ci], rings[ri], lims[ri]); xcSet[k] = v ? 1 : 2; xcVal[k] = v ? *v : 0; }
    return xcSet[k] == 1 ? std::optional<double>(xcVal[k]) : std::nullopt;
  };
  std::vector<Vec3> pos; std::unordered_map<I64, int> vid; std::vector<std::pair<int, int>> vidKeys;   // (col, row) of each made point
  auto ptAt = [&](const Col& c, double u) {
    size_t i = 1; while (i < c.A.size() - 1 && c.A[i] < u) i++;
    double L = c.A[i] - c.A[i - 1]; return c.P[i - 1].lerp(c.P[i], clamp((u - c.A[i - 1]) / (L ? L : 1), 0, 1));
  };
  // a point: its column and its row — 'S' (the column's start), 'E' (its end) or a ring's number
  std::unordered_set<int> tipV;                                     // (the points of the tip row — Auto weld points' tip rule)
  auto V = [&](int ci, int r) {
    I64 id = I64(ci) * (nR + 3) + (r + 2);
    auto it = vid.find(id); if (it != vid.end()) return it->second;
    const Col& c = cols[ci];
    Vec3 p = r == ROW_S ? c.P.front() : r == ROW_E ? c.P.back() : ptAt(c, crossAt(ci, r).value_or(0));
    int n = int(pos.size()); vid[id] = n; vidKeys.push_back({ci, r}); if (r == ROW_E) tipV.insert(n); pos.push_back(p); return n;
  };

  // ---- The lock's OUTER shell: on each ring one contour round the lock (or one strip, a lock of one surface) ----
  struct Sec { int row; std::vector<int> keys; bool closed; };
  std::deque<Sec> pool;
  auto newSec = [&](int row, std::vector<int> keys, bool closed) { pool.push_back({row, std::move(keys), closed}); return &pool.back(); };
  struct RingSec { int row; std::vector<Sec*> pieces; };
  std::vector<RingSec> ringSecs;
  std::vector<double> lastU(nC, -INF);
  struct WallK { std::vector<int> keys; int a, b; };
  std::vector<WallK> wallsK; for (auto& k : wallCols) wallsK.push_back({k, k.front(), k.back()});
  std::set<int> single;
  int dropped = 0;
  std::vector<double> presU(nC, 0); std::vector<char> pres(nC, 0); std::vector<int> presOrder;
  std::vector<int> repOf(nC, -1);
  std::vector<std::array<double, 2>> p2(nC);
  for (int ri = 0; ri < nR; ri++) {
    const CocoonRing& R = rings[ri];
    std::fill(pres.begin(), pres.end(), 0); presOrder.clear();
    for (int ci = 0; ci < nC; ci++) {
      auto u = crossAt(ci, ri); if (!u) continue; double Lc = cols[ci].A.back();
      if (*u - lastU[ci] < gap || *u < gap || Lc - *u < gap) continue;
      pres[ci] = 1; presU[ci] = *u; presOrder.push_back(ci);
    }
    // (a ring right at the lock's open end — every curve it crosses ending under a third of a loop step on, the ends
    // apart (not a tip where they meet — its fan): a thin loop next to the end row, junk)
    if (presOrder.size() >= 2 && ri > 0) {
      bool nearE = true, meet = true; const Vec3& e0 = cols[presOrder[0]].P.back();
      for (int ci : presOrder) { if (!(cols[ci].A.back() - presU[ci] < 0.33 * h)) nearE = false; if (!(cols[ci].P.back().distanceTo(e0) < 0.1 * h)) meet = false; }
      if (nearE && !meet) { dropped++; continue; }
    }
    auto P = [&](int ci) { return ptAt(cols[ci], presU[ci]); };
    Vec3 PN = (R.pn ? *R.pn : R.n).normalized();
    // (curves at one place here — running along a common trunk — are one node: the first of them by name)
    std::fill(repOf.begin(), repOf.end(), -1);
    std::vector<std::pair<int, Vec3>> reps; double tol = 0.05 * h * (R.fold ? 0.2 : 1);
    { std::vector<int> cks; for (int ci : presOrder) if (cols[ci].key[0] == 'c') cks.push_back(ci);
      std::sort(cks.begin(), cks.end(), [&](int x, int y) { return cols[x].key < cols[y].key; });
      for (int ci : cks) { Vec3 p = P(ci); int r = -1; for (auto& q : reps) if (q.second.distanceTo(p) < tol) { r = q.first; break; } if (r >= 0) repOf[ci] = r; else { reps.push_back({ci, p}); repOf[ci] = ci; } } }
    // (a wall's edge curve not begun yet at this ring, starting ON another curve crossing it: the wall taken to that curve)
    auto subK = [&](int ci) -> int {
      if (pres[ci]) return ci;
      if (crossAt(ci, ri)) return -1;
      const Vec3& p0 = cols[ci].P.front();
      for (int k : presOrder) { if (k == ci) continue; const Col& o = cols[k]; for (size_t i = 0; i + 1 < o.P.size(); i++) if (distToSeg(p0, o.P[i], o.P[i + 1]) < tol && o.A[i] >= presU[k] - gap) return k; }
      return -1;
    };
    struct WS { int a, b; std::vector<int> poly; };
    std::vector<WS> ws;
    for (auto& w : wallsK) {
      int sa = subK(w.a), sb = subK(w.b); if (sa < 0 || sb < 0) continue;
      int ra = repOf[sa], rb = repOf[sb]; if (ra == rb || ra < 0 || rb < 0) continue;
      WS x{ra, rb, {ra}}; for (size_t q = 1; q + 1 < w.keys.size(); q++) if (pres[w.keys[q]]) x.poly.push_back(w.keys[q]); x.poly.push_back(rb);
      ws.push_back(std::move(x));
    }
    if (ws.empty() && reps.size() == 1 && presOrder.size() >= 2) single.insert(ri);   // (all its curves at one point here)
    if (ws.empty()) { dropped++; continue; }
    Vec3 e1 = std::abs(PN.y) < 0.9 ? Vec3(0, 1, 0).cross(PN).normalized() : Vec3(1, 0, 0).cross(PN).normalized(), e2 = PN.cross(e1);
    for (int ci : presOrder) { Vec3 p = P(ci); p2[ci] = {p.dot(e1), p.dot(e2)}; }
    // the graph: from each curve its walls, each with its keys running from that curve
    struct Edge { int wi, to; std::vector<int> keys; };
    std::vector<int> adjOrder; std::unordered_map<int, std::vector<Edge>> adj;
    auto add = [&](int n, Edge e) { auto it = adj.find(n); if (it == adj.end()) { adjOrder.push_back(n); it = adj.emplace(n, std::vector<Edge>{}).first; } it->second.push_back(std::move(e)); };
    for (int wi = 0; wi < int(ws.size()); wi++) { auto& w = ws[wi]; add(w.a, {wi, w.b, w.poly}); std::vector<int> rv(w.poly.rbegin(), w.poly.rend()); add(w.b, {wi, w.a, rv}); }
    // its connected pieces: each a contour of its own
    std::vector<std::vector<int>> comps; std::unordered_set<int> seenN;
    for (int n : adjOrder) {
      if (seenN.count(n)) continue; std::vector<int> nodes{n}; seenN.insert(n);
      for (size_t q = 0; q < nodes.size(); q++) for (auto& e : adj[nodes[q]]) if (!seenN.count(e.to)) { seenN.insert(e.to); nodes.push_back(e.to); }
      comps.push_back(nodes);
    }
    auto ang = [](std::array<double, 2> a, std::array<double, 2> b) { double d = std::atan2(b[1], b[0]) - std::atan2(a[1], a[0]); return std::fmod(std::fmod(d, 2 * PI) + 2 * PI, 2 * PI); };
    auto dirOf = [&](const Edge& e) { auto a = p2[e.keys[0]], b = p2[e.keys.size() > 1 ? e.keys[1] : e.to]; return std::array<double, 2>{b[0] - a[0], b[1] - a[1]}; };
    const Vec3& tg = G.t[clampi(int(jsRound(cocoonF(G, R.s))), 0, G.N - 1)];
    auto contourOf = [&](const std::vector<int>& nodes) -> std::vector<Sec*> {
      int start = nodes[0]; for (int k : nodes) if (p2[k][0] < p2[start][0]) start = k;
      // the outer walk: at each curve the wall turning least counter-clockwise from the way back (the way back last)
      struct WE { int wi, to; std::vector<int> keys; int from; };
      std::vector<WE> walk; std::vector<std::vector<int>> eight; int cur = start; std::array<double, 2> back{0, 1}; bool haveFirst = false; std::pair<int, int> first;
      for (int guard = 0; guard < 4 * int(ws.size()) + 4; guard++) {
        auto& es = adj[cur]; const Edge* pick = nullptr; double pa = INF;
        for (auto& e : es) {
          auto d = dirOf(e); double a = ang({-back[0], -back[1]}, d);
          if (!walk.empty() && e.wi == walk.back().wi && es.size() > 1) a = 2 * PI + 1;
          if (a < 1e-9) a = 2 * PI;
          if (a < pa) { pa = a; pick = &e; }
        }
        if (!pick) break;
        std::pair<int, int> id{cur, pick->wi}; if (haveFirst && first == id) break; if (!haveFirst) { first = id; haveFirst = true; }
        walk.push_back({pick->wi, pick->to, pick->keys, cur});
        const auto& k = pick->keys; auto a = p2[k.size() >= 2 ? k[k.size() - 2] : cur], b = p2[pick->to]; back = {b[0] - a[0], b[1] - a[1]}; cur = pick->to;
      }
      // walls walked once — the closed contour; none — a ribbon: the longest path (over walls walked there and back)
      std::unordered_map<int, int> cnt; for (auto& e : walk) cnt[e.wi]++;
      std::vector<int> keys; bool closed = false;
      std::vector<const WE*> once; for (auto& e : walk) if (cnt[e.wi] == 1) once.push_back(&e);
      if (once.size() >= 2) {
        closed = true; for (auto* e : once) keys.insert(keys.end(), e->keys.begin(), e->keys.end() - 1);
        // (a curve met twice: a small loop hanging on the contour — the bigger one stays; both of 3 points or more — a
        // figure 8: both stay, each a contour of its own sharing that point)
        for (int guard = 0; guard < 20; guard++) {
          std::unordered_map<int, int> at; bool has = false; int q0 = 0, q1 = 0;
          for (int q = 0; q < int(keys.size()); q++) { auto it = at.find(keys[q]); if (it != at.end() && !has) { has = true; q0 = it->second; q1 = q; } at[keys[q]] = q; }
          if (!has) break;
          std::vector<int> inner(keys.begin() + q0, keys.begin() + q1), outer(keys.begin() + q1, keys.end()); outer.insert(outer.end(), keys.begin(), keys.begin() + q0);
          if (inner.size() >= 3 && outer.size() >= 3) { eight.push_back(inner); keys = outer; continue; }
          keys = inner.size() >= outer.size() ? inner : outer;
        }
        if (keys.size() < 3) closed = false;
      } else {
        auto far = [&](int from) {
          struct DV { int d; std::vector<const Edge*> path; };
          std::vector<int> dOrder{from}; std::unordered_map<int, DV> dist; dist[from] = {0, {}}; std::vector<int> q{from};
          for (size_t t = 0; t < q.size(); t++) {
            auto ai = adj.find(q[t]); if (ai == adj.end()) continue;
            for (auto& e : ai->second) if (!dist.count(e.to)) { DV v{dist[q[t]].d + 1, dist[q[t]].path}; v.path.push_back(&e); dist[e.to] = std::move(v); dOrder.push_back(e.to); q.push_back(e.to); }
          }
          int m = from; for (int n : dOrder) if (dist[n].d > dist[m].d) m = n;
          return dist[m].path;
        };
        auto p0 = far(start); int endN = p0.empty() ? start : p0.back()->to; auto path = far(endN);
        if (path.empty()) return {};
        for (size_t q = 0; q < path.size(); q++) keys.insert(keys.end(), path[q]->keys.begin() + (q ? 1 : 0), path[q]->keys.end());
      }
      // one way round for all (counter-clockwise looking down the cocoon)
      auto ccw = [&](std::vector<int> ks) {
        if (closed && ks.size() > 2) {
          double ar = 0; for (size_t q = 0; q < ks.size(); q++) { auto a = p2[ks[q]], b = p2[ks[(q + 1) % ks.size()]]; ar += a[0] * b[1] - b[0] * a[1]; }
          if ((ar > 0) != (PN.dot(tg) > 0)) std::reverse(ks.begin(), ks.end());
        }
        return ks;
      };
      std::vector<Sec*> out{newSec(ri, ccw(keys), closed)};
      for (auto& ks : eight) out.push_back(newSec(ri, ccw(ks), true));
      return out;
    };
    // (a closed contour of two points — a wall and its twin with no column between — has no width: left out)
    std::vector<Sec*> pieces;
    for (auto& n : comps) for (Sec* p : contourOf(n)) { std::set<int> u(p->keys.begin(), p->keys.end()); if (!(p->closed && u.size() < 3)) pieces.push_back(p); }
    if (pieces.empty()) { dropped++; continue; }
    ringSecs.push_back({ri, pieces});
    for (Sec* p : pieces) for (int k : p->keys) lastU[k] = presU[k];
  }
  const int rowsN = int(ringSecs.size());

  // ---- the zip between two contours A → B (B's start turned to A's first column, if it has it — else the nearest) ----
  std::vector<std::vector<int>> faces; std::vector<int> fInfo;
  struct Z { int a, b; bool chg; }; std::vector<Z> zs; int zCur = -1;
  auto pushF = [&](std::vector<int> f) { faces.push_back(std::move(f)); fInfo.push_back(zCur); };
  auto pt = [&](int ci, int row) { int i = V(ci, row); return pos[i]; };
  auto zip = [&](const Sec* A, const Sec* B) {
    bool closed = A->closed && B->closed;
    if (closed) {
      bool chg = A->keys.size() != B->keys.size();
      if (!chg) for (int x : A->keys) if (std::find(B->keys.begin(), B->keys.end(), x) == B->keys.end()) { chg = true; break; }
      zs.push_back({A->row, B->row, chg}); zCur = int(zs.size()) - 1;
    } else zCur = -1;
    std::vector<int> bk = B->keys;
    if (closed) {
      int at = -1; for (size_t q = 0; q < bk.size(); q++) if (bk[q] == A->keys[0]) { at = int(q); break; }
      if (at < 0) { Vec3 p = pt(A->keys[0], A->row); double bd = INF; for (size_t q = 0; q < bk.size(); q++) { double d = pt(bk[q], B->row).distanceTo(p); if (d < bd) { bd = d; at = int(q); } } }
      std::rotate(bk.begin(), bk.begin() + at, bk.end());
    } else if (!A->closed && !B->closed) {
      Vec3 a0 = pt(A->keys[0], A->row), b0 = pt(bk[0], B->row), b1 = pt(bk.back(), B->row);
      if (a0.distanceTo(b1) < a0.distanceTo(b0)) std::reverse(bk.begin(), bk.end());
    }
    const auto& ak = A->keys; int nA = int(ak.size()), nB = int(bk.size()), segs = closed ? 1 : 0;
    int mA = nA - 1 + segs, mB = nB - 1 + segs;                    // (steps along each: a closed one round to its start)
    auto va = [&](int q) { return V(ak[q % nA], A->row); };
    auto vb = [&](int q) { return V(bk[q % nB], B->row); };
    // the columns both contours have are the anchors (the longest run of them in order); between two anchors: quads, a
    // column coming or going — its extra steps triangles on its shortest segments
    std::vector<std::pair<int, int>> lcs;
    { int n = nA, m = nB; std::vector<std::vector<int>> T(n + 1, std::vector<int>(m + 1, 0));
      for (int x = n - 1; x >= 0; x--) for (int y = m - 1; y >= 0; y--) T[x][y] = ak[x] == bk[y] ? T[x + 1][y + 1] + 1 : std::max(T[x + 1][y], T[x][y + 1]);
      int x = 0, y = 0; while (x < n && y < m) { if (ak[x] == bk[y]) { lcs.push_back({x, y}); x++; y++; } else if (T[x + 1][y] >= T[x][y + 1]) x++; else y++; } }
    std::vector<std::pair<int, int>> anc{{0, 0}}; for (auto& p : lcs) if (p.first > 0 && p.second > 0) anc.push_back(p); anc.push_back({mA, mB});
    for (size_t q = 0; q + 1 < anc.size(); q++) {
      int i0 = anc[q].first, j0 = anc[q].second, i1 = anc[q + 1].first, j1 = anc[q + 1].second, k1 = i1 - i0, k2 = j1 - j0; if (k1 < 0 || k2 < 0) continue;
      auto shortest = [&](int n, const std::function<int(int)>& at, int k, int cnt) {
        std::vector<std::pair<int, double>> L; for (int t = 0; t < k; t++) { int a = at(n + t); int b = at(n + t + 1); L.push_back({t, pos[a].distanceTo(pos[b])}); }
        std::stable_sort(L.begin(), L.end(), [](auto& x, auto& y) { return x.second < y.second; });
        std::set<int> s; for (int t = 0; t < cnt && t < int(L.size()); t++) s.insert(n + L[t].first); return s;
      };
      if (k1 >= k2) {
        auto tri = shortest(i0, va, k1, k1 - k2); int j = j0;
        for (int s = i0; s < i1; s++) { if (tri.count(s)) { int a = va(s), b = va(s + 1), c = vb(j); pushF({a, b, c}); } else { int a = va(s), b = va(s + 1), c = vb(j + 1), d = vb(j); pushF({a, b, c, d}); j++; } }
      } else {
        auto tri = shortest(j0, vb, k2, k2 - k1); int i = i0;
        for (int s = j0; s < j1; s++) { if (tri.count(s)) { int a = va(i), b = vb(s + 1), c = vb(s); pushF({a, b, c}); } else { int a = va(i), b = va(i + 1), c = vb(s + 1), d = vb(s); pushF({a, b, c, d}); i++; } }
      }
    }
  };
  // ---- forks: one contour into two (a split) or two into one (a merge) ----
  auto cen = [&](const Sec* S) { Vec3 s; for (int k : S->keys) s += pt(k, S->row); return s / double(S->keys.size()); };
  // the bridge: on the two pieces' row their contours joined into one round both (at their two edges facing each other),
  // the single contour zipped with that, the crotch — a quad across the gap
  auto bridge = [&](const Sec* one, const Sec* a, const Sec* b, bool oneFirst) {
    const auto &A = a->keys, &B = b->keys; int nA = int(A.size()), nB = int(B.size());
    auto mid = [&](const std::vector<int>& K, int i, int n, int row) { Vec3 x = pt(K[i], row); Vec3 y = pt(K[(i + 1) % n], row); return (x + y) * 0.5; };
    int sh = -1; for (int k : A) if (std::find(B.begin(), B.end(), k) != B.end()) { sh = k; break; }
    if (sh >= 0) {
      int ia = int(std::find(A.begin(), A.end(), sh) - A.begin()), ib = int(std::find(B.begin(), B.end(), sh) - B.begin());
      std::vector<int> keys(A.begin() + ia + 1, A.end()); keys.insert(keys.end(), A.begin(), A.begin() + ia + 1);
      keys.insert(keys.end(), B.begin() + ib + 1, B.end()); keys.insert(keys.end(), B.begin(), B.begin() + ib + 1);
      Sec* C = newSec(a->row, keys, true);
      if (oneFirst) zip(one, C); else zip(C, one);
      return true;
    }
    int p = 0, q = 0; double bd = INF;
    for (int i = 0; i < nA; i++) for (int j = 0; j < nB; j++) { Vec3 m1 = mid(A, i, nA, a->row); Vec3 m2 = mid(B, j, nB, b->row); double d = m1.distanceTo(m2); if (d < bd) { bd = d; p = i; q = j; } }
    std::vector<int> keys(A.begin() + p + 1, A.end()); keys.insert(keys.end(), A.begin(), A.begin() + p + 1);
    keys.insert(keys.end(), B.begin() + q + 1, B.end()); keys.insert(keys.end(), B.begin(), B.begin() + q + 1);
    Sec* C = newSec(a->row, keys, true);
    if (oneFirst) zip(one, C); else zip(C, one);
    zCur = -1; { int v0 = V(A[(p + 1) % nA], a->row), v1 = V(A[p], a->row), v2 = V(B[(q + 1) % nB], b->row), v3 = V(B[q], b->row); pushF({v0, v1, v2, v3}); }
    return true;
  };
  auto fork = [&](const Sec* one, const Sec* a, const Sec* b, bool oneFirst) {
    if (!one->closed || !a->closed || !b->closed || one->keys.size() < 2) return false;
    std::set<int> inA(a->keys.begin(), a->keys.end()), inB(b->keys.begin(), b->keys.end()); Vec3 ca = cen(a), cb = cen(b);
    std::vector<char> lab; for (int k : one->keys) { if (inA.count(k)) lab.push_back('A'); else if (inB.count(k)) lab.push_back('B'); else { Vec3 x = pt(k, one->row); double da = x.distanceTo(ca); Vec3 y = pt(k, one->row); lab.push_back(da <= y.distanceTo(cb) ? 'A' : 'B'); } }
    int n = int(lab.size()), st = -1; for (int q = 0; q < n; q++) if (lab[q] == 'A' && lab[(q + n - 1) % n] == 'B') { st = q; break; }
    if (st < 0) return bridge(one, a, b, oneFirst);
    std::vector<int> arcA, arcB; char phase = 'A';
    for (int t = 0; t < n; t++) { int q = (st + t) % n; if (lab[q] != phase) { if (phase == 'B') return bridge(one, a, b, oneFirst); phase = 'B'; } (phase == 'A' ? arcA : arcB).push_back(one->keys[q]); }
    bool bad = arcA.empty() || arcB.empty();
    for (int k : arcB) if (inA.count(k)) bad = true;
    for (int k : arcA) if (inB.count(k)) bad = true;
    if (bad) return bridge(one, a, b, oneFirst);
    if (arcA.size() < 3 || arcB.size() < 3) return bridge(one, a, b, oneFirst);
    Sec* LA = newSec(one->row, arcA, true); Sec* LB = newSec(one->row, arcB, true);
    if (oneFirst) { zip(LA, a); zip(LB, b); } else { zip(a, LA); zip(b, LB); }
    zCur = -1; { int v0 = V(arcA.back(), one->row), v1 = V(arcB.front(), one->row), v2 = V(arcB.back(), one->row), v3 = V(arcA.front(), one->row); pushF({v0, v1, v2, v3}); }
    return true;
  };
  auto cap = [&](const Sec* p, int row) { return newSec(row, p->keys, p->closed); };   // (its columns at their starts / ends)
  // ps — the open ends: the last contour of each strand so far; a piece no contour of the row goes on stays open
  auto link = [&](const std::vector<Sec*>& ps, const std::vector<Sec*>& cs, const std::vector<Sec*>& prev) {
    if (ps.size() == 1 && cs.size() == 1) { zip(ps[0], cs[0]); return cs; }
    auto ov = [](const Sec* a, const Sec* b) { std::set<int> s(a->keys.begin(), a->keys.end()); int n = 0; for (int k : b->keys) if (s.count(k)) n++; return n; };
    int np = int(ps.size()), nc = int(cs.size());
    std::vector<std::vector<int>> M(np, std::vector<int>(nc)); for (int i = 0; i < np; i++) for (int j = 0; j < nc; j++) M[i][j] = ov(ps[i], cs[j]);
    std::set<int> uP, uC;
    for (int i = 0; i < np; i++) {
      std::vector<int> js; for (int j = 0; j < nc; j++) { if (M[i][j] <= 0) continue; bool only = true; for (int k = 0; k < np; k++) if (k != i && M[k][j] != 0) only = false; if (only) js.push_back(j); }
      if (js.size() == 2 && fork(ps[i], cs[js[0]], cs[js[1]], true)) { uP.insert(i); for (int j : js) uC.insert(j); }
    }
    for (int j = 0; j < nc; j++) {
      if (uC.count(j)) continue;
      std::vector<int> is; for (int i = 0; i < np; i++) { if (uP.count(i) || M[i][j] <= 0) continue; bool only = true; for (int l = 0; l < nc; l++) if (l != j && M[i][l] != 0) only = false; if (only) is.push_back(i); }
      if (is.size() == 2 && fork(cs[j], ps[is[0]], ps[is[1]], false)) { uC.insert(j); for (int i : is) uP.insert(i); }
    }
    struct PR { int m, i, j; }; std::vector<PR> pairs; for (int i = 0; i < np; i++) for (int j = 0; j < nc; j++) if (M[i][j] > 0) pairs.push_back({M[i][j], i, j});
    std::stable_sort(pairs.begin(), pairs.end(), [](const PR& x, const PR& y) { return x.m > y.m; });
    for (auto& pr : pairs) if (!uP.count(pr.i) && !uC.count(pr.j)) { zip(ps[pr.i], cs[pr.j]); uP.insert(pr.i); uC.insert(pr.j); }
    // (no column in common: the nearest left over of the row just before, within two loop steps)
    for (int i = 0; i < np; i++) {
      if (uP.count(i) || std::find(prev.begin(), prev.end(), ps[i]) == prev.end()) continue;
      int bj = -1; double bd = 2 * h;
      for (int j = 0; j < nc; j++) { if (uC.count(j)) continue; Vec3 c1 = cen(ps[i]); Vec3 c2 = cen(cs[j]); double d = c1.distanceTo(c2); if (d < bd) { bd = d; bj = j; } }
      if (bj >= 0) { zip(ps[i], cs[bj]); uP.insert(i); uC.insert(bj); }
    }
    for (int j = 0; j < nc; j++) if (!uC.count(j)) zip(cap(cs[j], ROW_S), cs[j]);
    std::vector<Sec*> out = cs; for (int i = 0; i < np; i++) if (!uP.count(i)) out.push_back(ps[i]);
    return out;
  };
  // (a lock that is a tube somewhere: its open stretches inside the cocoon it takes its loops from left out, the tube
  // closed by a lid where it begins / ends after such a stretch)
  bool lidTop = false, lidEnd = false;
  auto anyClosed = [](const RingSec& r) { for (auto* p : r.pieces) if (p->closed) return true; return false; };
  auto anyOpen = [](const RingSec& r) { for (auto* p : r.pieces) if (!p->closed) return true; return false; };
  if (std::any_of(ringSecs.begin(), ringSecs.end(), anyClosed)) {
    auto hid = [&](const RingSec& r) { return r.row >= 0 && r.row < nR && !rings[r.row].run.empty(); };
    auto junk = [&](const RingSec& r) { return hid(r) && anyOpen(r); };
    int firstC = 0; while (!anyClosed(ringSecs[firstC])) firstC++;
    int lastC = int(ringSecs.size()) - 1; while (!anyClosed(ringSecs[lastC])) lastC--;
    int jt = 0, je = 0; for (int q = 0; q < firstC; q++) if (junk(ringSecs[q])) jt++; for (int q = lastC + 1; q < int(ringSecs.size()); q++) if (junk(ringSecs[q])) je++;
    lidTop = jt >= 2; lidEnd = je >= 2;
    for (auto& r : ringSecs) if (hid(r)) { std::vector<Sec*> k; for (auto* p : r.pieces) if (p->closed) k.push_back(p); r.pieces = k; }
    for (int q = int(ringSecs.size()) - 1; q >= 0; q--) if (ringSecs[q].pieces.empty()) ringSecs.erase(ringSecs.begin() + q);
    if (lidTop && !ringSecs.empty() && anyOpen(ringSecs[0])) lidTop = false;   // (a strip kept above: no lid)
    if (lidEnd && !ringSecs.empty() && anyOpen(ringSecs.back())) lidEnd = false;
  }
  // a lid: the contour closed by quads from both ends inwards (a triangle in the middle if odd)
  auto lid = [&](const Sec* p) {
    const auto& k = p->keys; int n = int(k.size()), i = 0, j = n - 1; auto v = [&](int q) { return V(k[q], p->row); }; zCur = -1;
    while (j - i >= 3) { int a = v(i), b = v(i + 1), c = v(j - 1), d = v(j); pushF({a, b, c, d}); i++; j--; }
    if (j - i == 2) { int a = v(i), b = v(i + 1), c = v(j); pushF({a, b, c}); }
  };
  if (!ringSecs.empty()) {
    // (the root: its columns' starts — or, where its curves run on as one along a trunk above the first contour for more
    // than a loop step, the nearest ring there where they are at one point)
    auto rootCap = [&](const Sec* p) {
      for (int ri = p->row - 1; ri >= 0; ri--) if (single.count(ri)) {
        int k0 = p->keys[0]; const Col& c0 = cols[k0]; auto u = crossAt(k0, ri); if (!u || ptAt(c0, *u).distanceTo(c0.P.front()) < h) break;
        return newSec(ri, std::vector<int>(p->keys.size(), k0), p->closed);
      }
      return cap(p, ROW_S);
    };
    for (Sec* p : ringSecs[0].pieces) { if (lidTop) lid(p); else zip(rootCap(p), p); }   // the root: open
    std::vector<Sec*> open = ringSecs[0].pieces;
    for (size_t s = 0; s + 1 < ringSecs.size(); s++) open = link(open, ringSecs[s + 1].pieces, ringSecs[s].pieces);
    const auto& lastP = ringSecs.back().pieces;
    for (Sec* p : open) { if (lidEnd && std::find(lastP.begin(), lastP.end(), p) != lastP.end()) lid(p); else zip(p, cap(p, ROW_E)); }   // the tip
  }
  // FOLDS — a thin lock running between two loops far apart while a column comes or goes there: its quads twist and fold
  // over. A quad of such a zip bent more than 30° across its diagonal and the gap longer than 1.5 × the lock's width
  {
    std::vector<int> zOrder; std::unordered_map<int, double> byZ;
    auto tn = [&](int a, int b, int c) { return (pos[b] - pos[a]).cross(pos[c] - pos[a]).normalized(); };
    for (size_t fi = 0; fi < faces.size(); fi++) {
      int z = fInfo[fi]; const auto& f = faces[fi];
      if (z < 0 || !zs[z].chg || f.size() != 4 || zs[z].a < 0 || zs[z].b < 0) continue;
      double np = std::max(angleTo(tn(f[0], f[1], f[2]), tn(f[0], f[2], f[3])), angleTo(tn(f[0], f[1], f[3]), tn(f[1], f[2], f[3]))) * 180 / PI;
      if (np > 30) { auto it = byZ.find(z); if (it == byZ.end()) { zOrder.push_back(z); byZ[z] = std::max(0.0, np); } else it->second = std::max(it->second, np); }
    }
    auto dia = [&](int row) { std::vector<const Vec3*> ps; for (size_t i = 0; i < vidKeys.size(); i++) if (vidKeys[i].second == row) ps.push_back(&pos[i]); double m = 0; for (auto* p : ps) for (auto* q : ps) m = std::max(m, p->distanceTo(*q)); return m; };
    for (int z : zOrder) { double gp = rings[zs[z].b].s - rings[zs[z].a].s, w = std::max(dia(zs[z].a), dia(zs[z].b)); if (gp > 1.5 * w) res.folds.push_back({zs[z].a, zs[z].b}); }
  }
  // points at one place → one (a tip the curves share, a column met twice); faces left with < 3 corners go
  double eps = std::max(1e-6, 1e-4 * scale);
  std::map<std::array<I64, 3>, int> cell; std::vector<int> remap(pos.size()); Mesh data;
  for (size_t i = 0; i < pos.size(); i++) {
    std::array<I64, 3> kk{I64(jsRound(pos[i].x / eps)), I64(jsRound(pos[i].y / eps)), I64(jsRound(pos[i].z / eps))};
    auto it = cell.find(kk); if (it != cell.end()) remap[i] = it->second; else { int n = int(data.pos.size()); cell[kk] = n; remap[i] = n; data.pos.push_back(pos[i]); }
  }
  for (auto& f : faces) { std::vector<int> m; for (int i : f) m.push_back(remap[i]); auto g = cleanFace(m); if (g.size() >= 3) data.faces.push_back(std::move(g)); }
  // Auto weld points (S.weld): points closer than the weld distance become one — at the tip a smaller one; the tip of a
  // lock with thickness never into one point
  if (weld.on) {
    double d = weld.dist > 0 ? weld.dist : 0.25 * h;           // (auto — a quarter of this cocoon's loop step)
    WeldOpts o; for (int i : tipV) o.tips.insert(remap[i]); o.tipD = 0.3 * d; o.tipCollapse = walls.size() == 1; o.openOnly = true;
    res.welded = weldPoints(data, d, o);
  }
  // (a face twice on the same points — a lock flat here, its twin wall right on the other one: one of them stays)
  { std::set<std::vector<int>> seen; std::vector<std::vector<int>> k;
    for (auto& f : data.faces) { std::vector<int> s = f; std::sort(s.begin(), s.end()); if (seen.insert(s).second) k.push_back(f); }
    data.faces = std::move(k); }
  res.flips = orientFaces(data, G);
  res.pos = std::move(data.pos); res.faces = std::move(data.faces); res.dropped = dropped; res.rows = rowsN;
  return res;
}

// ======================= the fold pass, Convert =======================
// The folds (ringMeshOf → folds): a loop forced in halfway between the two rings of each, then that mesh made again (up
// to 3 times). Into the cocoon whose lock folds — every cocoon taking its loops gets it too. The sources first.
std::set<int> cocoonFoldPass(HairState& hs, const std::vector<int>& ks, const WeldSettings& weld, double scale) {
  CocoonSystem* cs = hs.cocoonSystem(); std::set<int> hit; if (!cs) return hit;
  std::vector<int> order; std::set<int> seen;
  std::function<void(int)> visit = [&](int k) { if (!seen.insert(k).second) return; for (int d : cs->deps(k)) visit(d); order.push_back(k); };
  for (int k : ks) visit(k);
  for (int k : order) if (!cs->folds(k).empty()) cs->setFolds(k, {});
  for (int k : order) {
    LockInput in; if (!cs->lockInput(k, in)) continue;
    double h = in.h;
    for (int it = 0; it < 3; it++) {
      if (!cs->lockInput(k, in)) break;
      LockMesh rm = ringMeshOf(in, hs.walls, weld, scale); if (rm.folds.empty()) break;
      int added = 0;
      for (auto& f : rm.folds) {
        const CocoonRing &A = rm.ringList[f.a], &B = rm.ringList[f.b]; if (B.s - A.s < 0.6 * h) continue;   // (never finer than ~ half a step)
        double s = (A.s + B.s) / 2; std::vector<double> fs = cs->folds(k);
        bool near = false; for (double x : fs) if (std::abs(x - s) < 0.1 * h) near = true; if (near) continue;
        fs.push_back(s); cs->setFolds(k, fs); hit.insert(k); added++;
      }
      if (!added) break;
    }
  }
  return hit;
}

void cocoonFoldClear(HairState& hs, const std::set<int>& hit) {
  if (CocoonSystem* cs = hs.cocoonSystem()) for (int k : hit) cs->setFolds(k, {});
}

// ======================= Weld locks =======================
namespace {

struct OkMark { Vec3 c; std::vector<int> ks; QString kind; };
// S.weldOk / S.weldSnap as the weld reads them; grow — DBG.lockWeldGrow (how far (1b) reaches, in weld distances)
// (radius: how far apart two locks' points may be to be stitched — in loop steps: S.lockWeldR, 0.1 by default)
struct WeldCtx { std::vector<OkMark> ok, snap; double grow = 4; bool classifying = false; double radius = 0.1; };
struct LockInfo { int id; const LockMesh* m; double h; int parent; std::vector<int> links; CocoonGeo G; };

std::vector<OkMark> readMarks(const QJsonValue& v) {
  std::vector<OkMark> out;
  for (const QJsonValue& x : v.toArray()) {
    QJsonObject o = x.toObject(); QJsonArray c = o.value("c").toArray(); OkMark m;
    m.c = {c.at(0).toDouble(), c.at(1).toDouble(), c.at(2).toDouble()};
    for (const QJsonValue& k : o.value("ks").toArray()) m.ks.push_back(k.toInt());
    m.kind = o.value("kind").toString();
    out.push_back(m);
  }
  return out;
}
QJsonArray writeMarks(const std::vector<OkMark>& ms) {
  QJsonArray a;
  for (auto& m : ms) { QJsonArray ks; for (int k : m.ks) ks.append(k); QJsonObject o{{"c", QJsonArray{m.c.x, m.c.y, m.c.z}}, {"ks", ks}}; if (!m.kind.isEmpty()) o.insert("kind", m.kind); a.append(o); }
  return a;
}
void addU(std::vector<int>& v, int x) { if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x); }
bool has(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

LockWeldResult weldLocksRun(const std::vector<LockInfo>& list, WeldCtx& W);

// The Ignore marks of an older file (no kind): an 'off' place (only walls a little off there) → kind 'off' (welded as
// usual, not reported); else a crossing ('x')
void weldOkClassify(const std::vector<LockInfo>& list, WeldCtx& W) {
  if (W.classifying) return;
  bool any = false; for (auto& o : W.ok) if (o.kind.isEmpty()) any = true; if (!any) return;
  W.classifying = true;
  for (size_t oi = 0; oi < W.ok.size(); oi++) {
    if (!W.ok[oi].kind.isEmpty()) continue;
    std::vector<LockInfo> two; for (auto& x : list) if (has(W.ok[oi].ks, x.id)) two.push_back(x);
    if (two.size() < 2) continue;
    // (as when the marks were put: 'off' reported up to 2.5 d)
    WeldCtx sub = W; sub.ok.erase(sub.ok.begin() + oi); sub.grow = 2.5;
    auto pr = weldLocksRun(two, sub).problems;
    double h = two[0].h; const Vec3 c = W.ok[oi].c; const WeldProblem* p = nullptr; double pd = INF;
    for (auto& q : pr) { double d = q.c.distanceTo(c); if (d < 0.5 * h && d < pd) { pd = d; p = &q; } }
    bool off = p && std::all_of(p->edges.begin(), p->edges.end(), [](auto& e) { return e.kind == "off"; });
    W.ok[oi].kind = off ? "off" : "x";
  }
  W.classifying = false;
}

LockWeldResult weldLocksRun(const std::vector<LockInfo>& list, WeldCtx& W) {
  LockWeldResult res;
  const int n = int(list.size());
  std::vector<Vec3> pos; std::vector<int> own, off;
  struct F { int li; std::vector<int> f; };
  std::vector<F> faces;
  for (int li = 0; li < n; li++) {
    off.push_back(int(pos.size()));
    for (auto& p : list[li].m->pos) { pos.push_back(p); own.push_back(li); }
    for (auto& f : list[li].m->faces) { F o{li, {}}; for (int i : f) o.f.push_back(i + off[li]); faces.push_back(std::move(o)); }
  }
  double h = INF; for (auto& x : list) h = std::min(h, x.h);
  const double d = W.radius * h;
  // (only a cocoon and its child, or two linked cocoons, are welded)
  std::vector<std::vector<char>> rel(n, std::vector<char>(n, 0));
  for (int x = 0; x < n; x++) for (int y = 0; y < n; y++) {
    const auto &X = list[x], &Y = list[y];
    rel[x][y] = x != y && (X.parent == Y.id || Y.parent == X.id || has(X.links, Y.id) || has(Y.links, X.id));
  }
  auto relIn = [&](const std::vector<int>& L) { for (int x : L) for (int y : L) if (rel[x][y]) return true; return false; };
  const int N = int(pos.size());
  // (1) the pairs of different locks within d, nearest first
  struct Pair { double dd; int i, j; };
  std::vector<Pair> pairs;
  { std::map<std::array<I64, 3>, std::vector<int>> cell;
    auto cellOf = [&](const Vec3& p) { return std::array<I64, 3>{I64(std::floor(p.x / d)), I64(std::floor(p.y / d)), I64(std::floor(p.z / d))}; };
    for (int i = 0; i < N; i++) cell[cellOf(pos[i])].push_back(i);
    for (int i = 0; i < N; i++) {
      auto c = cellOf(pos[i]);
      for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
        auto it = cell.find({c[0] + dx, c[1] + dy, c[2] + dz}); if (it == cell.end()) continue;
        for (int j : it->second) { if (j <= i || own[j] == own[i] || !rel[own[i]][own[j]]) continue; double dd = pos[i].distanceTo(pos[j]); if (dd < d) pairs.push_back({dd, i, j}); }
      }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.dd < b.dd; }); }
  // (inside a lock: the face's centre by ray parity against that lock's own, unwelded triangles — 3 rays, majority)
  std::vector<std::vector<std::array<Vec3, 3>>> tris(n); std::vector<Box3> bb(n);
  for (int li = 0; li < n; li++) { const auto& m = *list[li].m; for (auto& f : m.faces) for (size_t q = 1; q + 1 < f.size(); q++) tris[li].push_back({m.pos[f[0]], m.pos[f[q]], m.pos[f[q + 1]]}); for (auto& t : tris[li]) for (auto& p : t) bb[li].expand(p); }
  const Vec3 dirs[3] = {{0.577, 0.577, 0.577}, {-0.6, 0.48, -0.64}, {0.3, -0.9, 0.316}};
  auto hit = [](const Vec3& o, const Vec3& dv, const std::array<Vec3, 3>& t) {
    Vec3 e1 = t[1] - t[0], e2 = t[2] - t[0], pv = dv.cross(e2); double det = e1.dot(pv); if (std::abs(det) < 1e-12) return false;
    Vec3 sv = o - t[0]; double u = sv.dot(pv) / det; if (u < 0 || u > 1) return false;
    Vec3 qv = sv.cross(e1); double v = dv.dot(qv) / det; if (v < 0 || u + v > 1) return false;
    return e2.dot(qv) / det > 1e-6;
  };
  auto inside = [&](const Vec3& c, int li) {
    const Box3& b = bb[li]; if (c.x < b.min.x || c.x > b.max.x || c.y < b.min.y || c.y > b.max.y || c.z < b.min.z || c.z > b.max.z) return false;
    int m = 0; for (auto& dv : dirs) { int k = 0; for (auto& t : tris[li]) if (hit(c, dv, t)) k++; m += k & 1; }
    return m >= 2;
  };
  auto idxOf = [&](int id) { for (int l = 0; l < n; l++) if (list[l].id == id) return l; return -1; };
  // the places marked "correct": nothing of those locks welded or removed there — its zone grown a loop step at a time
  weldOkClassify(list, W);
  struct Sp { Vec3 c; std::vector<int> L; };
  std::vector<Sp> okSp, offIgn, snapSp;
  for (auto& o : W.ok) {
    if (o.kind != "off") { Sp s{o.c, {}}; for (int id : o.ks) { int l = idxOf(id); if (l >= 0) s.L.push_back(l); } if (s.L.size() >= 2) okSp.push_back(s); }
    else { Sp s{o.c, {}}; bool all = true; for (int id : o.ks) { int l = idxOf(id); if (l < 0) all = false; s.L.push_back(l); } if (all) offIgn.push_back(s); }
  }
  for (auto& o : W.snap) { Sp s{o.c, {}}; bool all = true; for (int id : o.ks) { int l = idxOf(id); if (l < 0) all = false; s.L.push_back(l); } if (all) snapSp.push_back(s); }
  std::unordered_set<I64> keepPair; std::unordered_set<int> keepFace; std::vector<Vec3> zonePts;
  for (auto& s : okSp) {
    struct It { Vec3 p; I64 pair; int face; };
    std::vector<It> items;
    for (auto& pr : pairs) if (has(s.L, own[pr.i]) && has(s.L, own[pr.j]) && pos[pr.i].distanceTo(s.c) < 8 * h) items.push_back({pos[pr.i], I64(pr.i) * N + pr.j, -1});
    for (int fi = 0; fi < int(faces.size()); fi++) {
      const auto& o = faces[fi]; if (!has(s.L, o.li)) continue;
      Vec3 c = faceCenter(pos, o.f); if (c.distanceTo(s.c) > 8 * h) continue;
      for (int lj : s.L) if (lj != o.li && inside(c, lj)) { items.push_back({c, -1, fi}); break; }
    }
    std::vector<int> zone; std::vector<char> inZ(items.size(), 0);
    for (size_t q = 0; q < items.size(); q++) if (items[q].p.distanceTo(s.c) < 1.5 * h) { zone.push_back(int(q)); inZ[q] = 1; }
    for (size_t q = 0; q < zone.size(); q++) for (size_t x = 0; x < items.size(); x++) if (!inZ[x] && items[x].p.distanceTo(items[zone[q]].p) < h) { inZ[x] = 1; zone.push_back(int(x)); }
    for (int x : zone) { if (items[x].face < 0) keepPair.insert(items[x].pair); else keepFace.insert(items[x].face); zonePts.push_back(items[x].p); }
    zonePts.push_back(s.c);
  }
  std::vector<int> par(N); for (int i = 0; i < N; i++) par[i] = i;
  auto find = [&](int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; };
  std::vector<std::vector<int>> locks(N); for (int i = 0; i < N; i++) locks[i] = {own[i]};
  std::vector<int> lpar(n); for (int l = 0; l < n; l++) lpar[l] = l;
  auto lfind = [&](int x) { while (lpar[x] != x) x = lpar[x]; return x; };
  int merged = 0;
  for (auto& pr : pairs) {
    if (keepPair.count(I64(pr.i) * N + pr.j)) continue;                // (a place marked correct)
    int a = find(pr.i), b = find(pr.j); if (a == b) continue;
    bool share = false; for (int l : locks[a]) if (has(locks[b], l)) share = true; if (share) continue;
    if (own[b] < own[a] || (own[b] == own[a] && b < a)) std::swap(a, b);   // (the earlier lock's point stays)
    par[b] = a; for (int l : locks[b]) addU(locks[a], l); merged++;
    int la = lfind(own[pr.i]), lb = lfind(own[pr.j]); if (la != lb) lpar[lb] = la;
  }
  // (1b) a wall the two locks share, a little off: reported ('off'); welded only where approved (S.weldSnap)
  struct Bad { int a, b; QString kind; std::vector<int> L; };
  std::vector<Bad> offs; std::unordered_set<int> snapRoots;
  const double gk = W.grow;
  auto snapOk = [&](const std::vector<std::pair<int, int>>& pr, int la, int lb, const std::vector<int>& fr) {
    for (auto& s : snapSp) {
      if (!has(s.L, la) || !has(s.L, lb)) continue;
      bool near = true; for (auto& [u, v] : pr) if (!(pos[u].distanceTo(s.c) < 3 * h)) near = false;
      bool root = false; for (int r : fr) if (snapRoots.count(r)) root = true;
      if (near || root) return true;
    }
    return false;
  };
  if (gk > 1) {
    auto freeV = [&](int u) { if (find(u) != u || locks[u].size() != 1) return false; for (auto& p : zonePts) if (p.distanceTo(pos[u]) < h) return false; return true; };
    auto pc = [&](int a, int b) { return list[a].parent == list[b].id || list[b].parent == list[a].id; };
    for (int round = 0; round < 50; round++) {
      std::vector<I64> eOrder; std::unordered_map<I64, std::vector<int>> byE;
      for (int fi = 0; fi < int(faces.size()); fi++) {
        const auto& f = faces[fi].f; std::vector<int> r; for (int v : f) r.push_back(find(v));
        for (size_t q = 0; q < r.size(); q++) {
          int a = r[q], b = r[(q + 1) % r.size()]; if (a == b || locks[a].size() < 2 || locks[b].size() < 2) continue;
          I64 k = a < b ? I64(a) * N + b : I64(b) * N + a;
          auto it = byE.find(k); if (it == byE.end()) { eOrder.push_back(k); it = byE.emplace(k, std::vector<int>{}).first; } it->second.push_back(fi);
        }
      }
      struct Cand { double m; std::vector<std::pair<int, int>> pr; };
      std::vector<Cand> cand;
      for (I64 key : eOrder) {
        const auto& fs = byE[key];
        for (int x : fs) for (int y : fs) {
          const F &A = faces[x], &B = faces[y]; if (A.li >= B.li || !pc(A.li, B.li) || A.f.size() != B.f.size()) continue;
          int ca = 0, cb = 0; for (int z : fs) { if (faces[z].li == A.li) ca++; if (faces[z].li == B.li) cb++; }
          if (ca != 2 || cb != 2) continue;
          std::vector<int> ra, rb; for (int v : A.f) addU(ra, find(v)); for (int v : B.f) addU(rb, find(v));
          std::vector<int> ua, ub; for (int v : A.f) if (!has(rb, find(v))) ua.push_back(v); for (int v : B.f) if (!has(ra, find(v))) ub.push_back(v);
          if (ua.empty() || ua.size() != ub.size() || ua.size() > 2) continue;
          bool allFree = true; for (int v : ua) if (!freeV(v)) allFree = false; for (int v : ub) if (!freeV(v)) allFree = false; if (!allFree) continue;
          std::vector<std::pair<int, int>> pr;
          if (ua.size() == 1) pr = {{ua[0], ub[0]}};
          else if (pos[ua[0]].distanceTo(pos[ub[0]]) + pos[ua[1]].distanceTo(pos[ub[1]]) <= pos[ua[0]].distanceTo(pos[ub[1]]) + pos[ua[1]].distanceTo(pos[ub[0]])) pr = {{ua[0], ub[0]}, {ua[1], ub[1]}};
          else pr = {{ua[0], ub[1]}, {ua[1], ub[0]}};
          // (not where a third lock comes in)
          auto third = [&](int o) { for (int j = 0; j < N; j++) if (own[j] != A.li && own[j] != B.li && (rel[own[j]][A.li] || rel[own[j]][B.li]) && pos[j].distanceTo(pos[o]) < 0.5 * h) return true; return false; };
          bool th = false; for (int v : A.f) if (third(v)) { th = true; break; } if (!th) for (int v : B.f) if (third(v)) { th = true; break; } if (th) continue;
          bool within = true; for (auto& [u, v] : pr) if (!(pos[u].distanceTo(pos[v]) < gk * d)) within = false; if (!within) continue;
          std::vector<int> fr; for (int x2 : ra) if (has(rb, x2)) fr.push_back(x2);
          if (!snapOk(pr, A.li, B.li, fr)) {
            bool ign = false;
            for (auto& s : offIgn) if (has(s.L, A.li) && has(s.L, B.li)) { bool nr = true; for (auto& [u, v] : pr) if (!(pos[u].distanceTo(s.c) < 3 * h)) nr = false; if (nr) ign = true; }
            if (!ign) for (auto& [u, v] : pr) offs.push_back({u, v, "off", {A.li, B.li}});
            continue;
          }
          double m = 0; for (auto& [u, v] : pr) m = std::max(m, pos[u].distanceTo(pos[v]));
          cand.push_back({m, pr});
        }
      }
      if (cand.empty()) break;
      std::stable_sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.m < b.m; });
      int k = 0;
      for (auto& c : cand) {
        bool ok = true; for (auto& [u, v] : c.pr) if (!freeV(u) || !freeV(v)) ok = false; if (!ok) continue;
        for (auto& [u, v] : c.pr) { par[v] = u; locks[u] = {own[u]}; addU(locks[u], own[v]); merged++; k++; snapRoots.insert(u); int la = lfind(own[u]), lb = lfind(own[v]); if (la != lb) lpar[lb] = la; }
      }
      if (!k) break;
    }
  }
  // ((1c), a child's thin tube welded flat onto its parent, is off in the HTML version — DBG.lockWeldThin 0)
  struct KF { int li, fi; std::vector<int> f; };
  std::vector<KF> Fs;
  for (int fi = 0; fi < int(faces.size()); fi++) { std::vector<int> m; for (int v : faces[fi].f) m.push_back(find(v)); auto g = cleanFace(m); if (g.size() >= 3) Fs.push_back({faces[fi].li, fi, g}); }
  // (2) double walls
  auto fk = [](const std::vector<int>& f) { std::vector<int> s = f; std::sort(s.begin(), s.end()); return s; };
  std::map<std::vector<int>, int> cnt; for (auto& o : Fs) cnt[fk(o.f)]++;
  std::vector<KF> keep; for (auto& o : Fs) if (cnt[fk(o.f)] == 1 || keepFace.count(o.fi)) keep.push_back(o);
  res.doubled = int(Fs.size() - keep.size());
  // (3) inside another lock
  { size_t before = keep.size(); std::vector<KF> k2;
    for (auto& o : keep) {
      bool out = false;
      if (!keepFace.count(o.fi)) { Vec3 c = faceCenter(pos, o.f); for (int lj = 0; lj < n; lj++) if (lj != o.li && rel[o.li][lj] && inside(c, lj)) { out = true; break; } }
      if (!out) k2.push_back(o);
    }
    keep = std::move(k2); res.inside = int(before - keep.size()); }
  // (4) the holes it left (a root — open in the lock's own mesh before any welding — stays open)
  auto ek = [&](int a, int b) { return a < b ? I64(a) * N + b : I64(b) * N + a; };
  std::unordered_map<I64, int> raw; std::unordered_set<I64> origE;
  std::vector<I64> rawOrder;
  for (auto& o : faces) for (size_t q = 0; q < o.f.size(); q++) { I64 k = ek(o.f[q], o.f[(q + 1) % o.f.size()]); if (!raw.count(k)) rawOrder.push_back(k); raw[k]++; }
  for (I64 k : rawOrder) if (raw[k] == 1) origE.insert(ek(find(int(k / N)), find(int(k % N))));
  std::vector<I64> eOrder; std::unordered_map<I64, int> E; std::unordered_set<I64> dir;
  for (auto& o : keep) for (size_t q = 0; q < o.f.size(); q++) { int a = o.f[q], b = o.f[(q + 1) % o.f.size()]; I64 k = ek(a, b); if (!E.count(k)) eOrder.push_back(k); E[k]++; dir.insert(I64(a) * N + b); }
  std::vector<int> nOrder; std::unordered_map<int, std::vector<int>> nxt;
  for (I64 k : eOrder) {
    if (E[k] != 1) continue; int a = int(k / N), b = int(k % N);
    int x = a, y = b; if (dir.count(I64(a) * N + b)) { x = b; y = a; }
    if (!nxt.count(x)) nOrder.push_back(x); nxt[x].push_back(y);
  }
  { std::unordered_set<int> seen; int filled = 0, open = 0;
    for (int s0 : nOrder) {
      if (seen.count(s0)) continue; std::vector<int> loop{s0}; int cur = s0;
      for (int g = 0; g < 1000; g++) {
        seen.insert(cur); std::vector<int> ns; auto it = nxt.find(cur); if (it != nxt.end()) for (int x : it->second) if (!seen.count(x) || x == s0) ns.push_back(x);
        int nx = -1; for (int x : ns) if (x != s0) { nx = x; break; } if (nx < 0 && !ns.empty()) nx = ns[0];
        if (nx < 0 || nx == s0) break; loop.push_back(nx); cur = nx;
      }
      bool root = loop.size() >= 3; for (size_t q = 0; q < loop.size() && root; q++) if (!origE.count(ek(loop[q], loop[(q + 1) % loop.size()]))) root = false;
      if (root) continue;
      if (loop.size() < 3 || loop.size() > 16) { open += int(loop.size()); continue; }
      int i = 0, j = int(loop.size()) - 1, li = 0; for (auto& o : keep) if (has(o.f, loop[0])) { li = o.li; break; }
      while (j - i >= 3) { keep.push_back({li, -1, {loop[i], loop[i + 1], loop[j - 1], loop[j]}}); i++; j--; filled++; }
      if (j - i == 2) { keep.push_back({li, -1, {loop[i], loop[i + 1], loop[j]}}); filled++; }
    }
    res.filled = filled; res.open = open; }
  // the joints that didn't come out: an edge left open that is no root, or one of 3+ faces where two locks meet
  std::vector<I64> efOrder; std::unordered_map<I64, std::vector<int>> EF;
  for (auto& o : keep) for (size_t q = 0; q < o.f.size(); q++) { I64 k = ek(o.f[q], o.f[(q + 1) % o.f.size()]); if (!EF.count(k)) efOrder.push_back(k); EF[k].push_back(o.li); }
  auto nearZone = [&](const Vec3& m) { for (auto& p : zonePts) if (p.distanceTo(m) < h) return true; return false; };
  std::vector<Bad> bad;
  for (I64 k : efOrder) {
    const auto& ls = EF[k]; int a = int(k / N), b = int(k % N);
    std::vector<int> L; for (int l : ls) addU(L, l); for (int l : locks[a]) addU(L, l); for (int l : locks[b]) addU(L, l);
    if (ls.size() == 1 && origE.count(k)) continue;
    if (ls.size() == 2) continue;
    if (ls.size() > 2) { std::vector<int> s; for (int l : ls) addU(s, l); if (!relIn(s)) continue; }   // (one lock's own — a fork)
    Vec3 m = (pos[a] + pos[b]) * 0.5;
    if (nearZone(m)) continue;                                          // (marked correct)
    if (!relIn(L)) {
      int bj = -1; double bd = h;
      for (int j = 0; j < N; j++) {
        if (has(L, own[j]) || find(j) != j) continue; bool r = false; for (int l : L) if (rel[l][own[j]]) r = true; if (!r) continue;
        double dd = pos[j].distanceTo(m); if (dd < bd) { bd = dd; bj = own[j]; }
      }
      if (bj >= 0) addU(L, bj);
    }
    if (!relIn(L)) continue;                                            // (no related lock there — not a joint)
    // (inside a related lock — the inner part, hidden in the welded mesh: not a joint to redo)
    bool hidden = false;
    for (int l : L) { bool r = false; for (int o : L) if (o != l && rel[o][l]) r = true; if (r && !has(ls, l) && inside(m, l)) { hidden = true; break; } }
    if (hidden) continue;
    bad.push_back({a, b, ls.size() == 1 ? "open" : "many", L});
  }
  for (auto& e : offs) { bool dup = false; for (auto& x : bad) if (x.a == e.a && x.b == e.b) dup = true; if (!dup) bad.push_back(e); }
  // a child lock not touching its parent at all: shown where the two come nearest
  { std::set<std::pair<int, int>> met;
    for (int i = 0; i < N; i++) if (find(i) == i && locks[i].size() > 1) for (int a : locks[i]) for (int b : locks[i]) met.insert({a, b});
    for (int l = 0; l < n; l++) {
      int p = idxOf(list[l].parent); if (p < 0 || met.count({l, p})) continue;
      double bd = INF; int bi = -1, bj = -1;
      for (int i = 0; i < N; i++) { if (own[i] != l) continue; for (int j = 0; j < N; j++) if (own[j] == p) { double dd = (pos[i] - pos[j]).lengthSq(); if (dd < bd) { bd = dd; bi = i; bj = j; } } }
      if (bi < 0 || std::sqrt(bd) > 2 * h) continue;
      if (nearZone((pos[bi] + pos[bj]) * 0.5)) continue;
      bad.push_back({bi, bj, "apart", {l, p}});
    }
  }
  // the places: edges within a loop step of each other
  for (auto& e : bad) {
    Vec3 m = (pos[e.a] + pos[e.b]) * 0.5; WeldProblem* s = nullptr;
    for (auto& q : res.problems) if (q.c.distanceTo(m) < h) { s = &q; break; }
    if (s) { s->edges.push_back({pos[e.a], pos[e.b], e.kind}); for (int l : e.L) addU(s->locks, l); s->c = (s->c * double(s->edges.size() - 1) + m) / double(s->edges.size()); }
    else { WeldProblem q; q.c = m; q.edges.push_back({pos[e.a], pos[e.b], e.kind}); q.locks = e.L; res.problems.push_back(q); }
  }
  for (auto& p : res.problems) for (int& l : p.locks) l = list[l].id;
  // one mesh per set of locks joined by welded points
  std::vector<int> roots; for (int l = 0; l < n; l++) addU(roots, lfind(l));
  for (int r : roots) {
    LockWeldResult::Group g; std::vector<int> mem; for (int l = 0; l < n; l++) if (lfind(l) == r) mem.push_back(l);
    std::unordered_map<int, int> idx; Mesh data;
    for (auto& o : keep) {
      if (!has(mem, o.li)) continue; std::vector<int> f;
      for (int v : o.f) { auto it = idx.find(v); if (it == idx.end()) { it = idx.emplace(v, int(data.pos.size())).first; data.pos.push_back(pos[v]); } f.push_back(it->second); }
      data.faces.push_back(f);
    }
    orientFaces(data, list[mem[0]].G);
    for (int l : mem) g.fam.push_back(list[l].id);
    g.pos = std::move(data.pos); g.faces = std::move(data.faces);
    res.groups.push_back(std::move(g));
  }
  res.merged = merged;
  for (auto* S : {&okSp, &offIgn}) for (auto& s : *S) { WeldProblem q; q.c = s.c; for (int l : s.L) q.locks.push_back(list[l].id); q.ok = true; res.accepted.push_back(q); }
  return res;
}

std::vector<LockInfo> lockInfos(Scene& sc, HairState& hs, const std::vector<LockIn>& in) {
  CocoonSystem* cs = hs.cocoonSystem(); std::vector<LockInfo> out;
  for (auto& x : in) {
    const Item* k = sc.item(x.id); LockInfo li{x.id, x.m, cs->loopStep(x.id), -1, {}, cs->geo(x.id)};
    const Item* p = k && k->parent >= 0 ? sc.item(k->parent) : nullptr; if (p && p->type == ItemType::Cocoon) li.parent = p->id;
    if (k) for (const QJsonValue& v : k->raw.value("links").toArray()) { const Item* L = sc.item(v.toInt()); if (L && L->type == ItemType::Cocoon && L != k) li.links.push_back(L->id); }
    out.push_back(std::move(li));
  }
  return out;
}

}  // namespace

LockWeldResult weldLocks(Scene& sc, HairState& hs, const std::vector<LockIn>& list) {
  WeldCtx W; W.ok = readMarks(sc.fileRaw.value("weldOk")); W.snap = readMarks(sc.fileRaw.value("weldSnap"));
  W.radius = std::clamp(sc.fileRaw.value("lockWeldR").toDouble(0.1), 0.01, 1.0);
  bool hadKinds = true; for (auto& o : W.ok) if (o.kind.isEmpty()) hadKinds = false;
  // ((1c) off: weldLocks' trial of each thin child is weldLocksRun once)
  LockWeldResult r = weldLocksRun(lockInfos(sc, hs, list), W);
  if (!hadKinds) sc.fileRaw.insert("weldOk", writeMarks(W.ok));   // (the marks classified: kept so in the project)
  return r;
}

std::optional<LockWeldResult> checkConnections(Scene& sc, HairState& hs, const std::vector<int>& sel) {
  CocoonSystem* cs = hs.cocoonSystem(); if (!cs) return std::nullopt;
  std::vector<int> ks;
  for (int id : sel) { const Item* k = sc.item(id); LockInput in; if (k && k->type == ItemType::Cocoon && cs->lockInput(id, in)) ks.push_back(id); }
  if (ks.empty()) {
    std::vector<const Item*> o; for (auto& [id, it] : sc.items) if (it.type == ItemType::Cocoon) o.push_back(&it);
    std::sort(o.begin(), o.end(), [](auto* a, auto* b) { return a->order < b->order; });
    for (auto* k : o) { LockInput in; if (cs->lockInput(k->id, in)) ks.push_back(k->id); }
  }
  WeldSettings weld = weldSettings(sc); double scale = sceneScale(sc);
  std::set<int> folded = cocoonFoldPass(hs, ks, weld, scale);
  std::map<int, LockMesh> rms; std::vector<int> order;
  for (int k : ks) { LockInput in; cs->lockInput(k, in); LockMesh r = ringMeshOf(in, hs.walls, weld, scale); if (!r.faces.empty()) { rms[k] = std::move(r); order.push_back(k); } }
  std::function<int(int, std::set<int>)> depth = [&](int k, std::set<int> seen) { if (seen.count(k)) return 0; seen.insert(k); int d = 0; for (int P : cs->deps(k)) d = std::max(d, depth(P, seen) + 1); return d; };
  std::vector<std::pair<int, int>> dk; for (int k : order) dk.push_back({k, depth(k, {})});
  std::stable_sort(dk.begin(), dk.end(), [](auto& a, auto& b) { return a.second < b.second; });
  std::vector<LockIn> list; for (auto& [k, dd] : dk) list.push_back({k, &rms[k]});
  std::optional<LockWeldResult> out;
  if (!list.empty()) out = weldLocks(sc, hs, list);
  cocoonFoldClear(hs, folded);
  return out;
}

// (by the locks' names; two cocoons of one lock — by the cocoons')
QString weldIssueText(const Scene& sc, HairState& hs, const WeldProblem& p) {
  CocoonSystem* cs = hs.cocoonSystem(); QStringList ln, kn;
  for (int id : p.locks) {
    const Item* k = sc.item(id); if (!k) continue; kn << k->name;
    int g = cs ? cs->lockGroup(id) : -1; QString n = g >= 0 && sc.item(g) ? sc.item(g)->name : k->name; if (!ln.contains(n)) ln << n;
  }
  return (ln.size() < kn.size() ? kn : ln).join(" ↔ ") + QString(" (y %1)").arg(int(jsRound(p.c.y)));
}

// "correct place" — left as it is from now on (S.weldOk): an 'off' place (walls a little off only) is welded as usual,
// only no longer reported; any other — a crossing: nothing welded there
void weldSpotSet(Scene& sc, HairState& hs, const WeldProblem& sp, bool on) {
  std::vector<int> ids = sp.locks; std::sort(ids.begin(), ids.end());
  double h = hs.cocoonSystem() && !sp.locks.empty() ? hs.cocoonSystem()->loopStep(sp.locks[0]) : 1;
  auto ok = readMarks(sc.fileRaw.value("weldOk")); int i = -1;
  for (size_t q = 0; q < ok.size(); q++) { auto ks = ok[q].ks; std::sort(ks.begin(), ks.end()); if (ok[q].c.distanceTo(sp.c) < 1.5 * h && ks == ids) { i = int(q); break; } }
  bool off = !sp.edges.empty() && std::all_of(sp.edges.begin(), sp.edges.end(), [](auto& e) { return e.kind == "off"; });
  if (on && i < 0) ok.push_back({sp.c, ids, off ? "off" : "x"}); else if (!on && i >= 0) ok.erase(ok.begin() + i);
  if (ok.empty()) sc.fileRaw.remove("weldOk"); else sc.fileRaw.insert("weldOk", writeMarks(ok));
}

namespace {
// nextNumber: the next "<Type> N" (counters[type] set, as the HTML version's addItem does for every new item)
int nextNumber(Scene& sc, ItemType type, const QString& base, const QString& key) {
  QRegularExpression re("^" + QRegularExpression::escape(base) + " (\\d+)$"); int mx = 0;
  for (auto& [id, it] : sc.items) if (it.type == type) { auto m = re.match(it.name); if (m.hasMatch()) mx = std::max(mx, m.captured(1).toInt()); }
  QJsonObject counters = sc.fileRaw.value("counters").toObject(); counters.insert(key, mx + 1); sc.fileRaw.insert("counters", counters);
  return mx + 1;
}
}  // namespace
int meshesGroup(Scene& sc) {
  for (auto& [id, it] : sc.items) if (it.type == ItemType::Group && it.raw.value("meshesGroup").toBool()) return id;
  nextNumber(sc, ItemType::Group, "Group", "group");
  Item& g = sc.addItem(ItemType::Group, -1, "Meshes"); g.expanded = true;
  g.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "show"}, {"conv", QJsonValue()}, {"guidesGroup", false}, {"meshesGroup", true}, {"importGroup", false},
                      {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}};
  return g.id;
}
// Meshes always there, Imported inside it — first there (meshHierarchy: every project as it opens)
int meshHierarchy(Scene& sc) {
  int g = meshesGroup(sc), ig = -1;
  for (auto& [id, it] : sc.items) if (it.type == ItemType::Group && it.raw.value("importGroup").toBool()) { ig = id; break; }
  if (ig >= 0) {
    Item& I = *sc.item(ig);
    if (I.parent != g) { sc.removeFromParent(ig); I.parent = g; sc.item(g)->children.insert(sc.item(g)->children.begin(), ig); }
    auto& ch = sc.item(g)->children; auto at = std::find(ch.begin(), ch.end(), ig);
    if (at != ch.begin() && at != ch.end()) { ch.erase(at); ch.insert(ch.begin(), ig); }
  }
  return g;
}
namespace {
// every converted mesh into the Meshes group (its group first among the roots)
void meshesIntoGroup(Scene& sc) {
  std::vector<Item*> ms; for (auto& [id, it] : sc.items) if (it.type == ItemType::Mesh && !it.raw.value("pending").toBool()) ms.push_back(&it);
  if (ms.empty()) return;
  std::sort(ms.begin(), ms.end(), [](Item* a, Item* b) { return a->order < b->order; });
  int gid = meshesGroup(sc); Item& g = *sc.item(gid);
  if (g.parent < 0) { auto& R = sc.roots; R.erase(std::remove(R.begin(), R.end(), gid), R.end()); R.insert(R.begin(), gid); }
  bool moved = false;
  for (Item* m : ms) if (m->parent != gid) { sc.removeFromParent(m->id); m->parent = gid; g.children.push_back(m->id); moved = true; }
  if (moved) g.expanded = true;
}
}  // namespace

std::vector<int> convChecked(const Scene& sc, HairState& hs, const std::vector<int>& sel) {
  std::vector<int> out; CocoonSystem* cs = hs.cocoonSystem(); if (!cs) return out;
  for (int id : sel) {
    const Item* k = sc.item(id); if (!k || k->type != ItemType::Cocoon) continue;
    int g = cs->lockGroup(id); if (g >= 0 && std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
  }
  return out;
}

ConvertResult convertCocoons(Scene& sc, HairState& hs, const std::vector<int>& sel, const Vec3& center) {
  ConvertResult out; CocoonSystem* cs = hs.cocoonSystem();
  // ⌇ the braids going with them: a selected braid, its folder, its curve or connector
  std::vector<int> brs; for (int id : sel) { const int b = braid::braidOfItem(sc, id); if (b >= 0) addU(brs, b); }
  std::vector<int> ks;
  if (cs) for (int id : sel) { const Item* k = sc.item(id); LockInput in; if (k && k->type == ItemType::Cocoon && cs->lockInput(id, in)) ks.push_back(id); }
  // ◠ the crowns going with them: a selected crown (or its group), a crown whose lower connector a converted lock's
  // curve starts on
  std::vector<int> crownsSel;
  for (int kid : crown::crowns(sc)) { const Item* K = sc.item(kid); for (int id : sel) if (id == kid || id == K->parent) { addU(crownsSel, kid); break; } }
  if (ks.empty() && crownsSel.empty() && brs.empty()) { out.message = "Select the cocoons (or braids) to convert"; return out; }
  WeldSettings weld = weldSettings(sc); double scale = sceneScale(sc);
  std::set<int> folded = cocoonFoldPass(hs, ks, weld, scale);
  std::map<int, LockMesh> rms;
  for (int k : ks) { LockInput in; cs->lockInput(k, in); LockMesh r = ringMeshOf(in, hs.walls, weld, scale); if (r.faces.empty()) out.bad << sc.item(k)->name; else rms[k] = std::move(r); }
  // (sources first)
  std::function<int(int, std::set<int>)> depth = [&](int k, std::set<int> seen) { if (seen.count(k)) return 0; seen.insert(k); int d = 0; for (int P : cs->deps(k)) d = std::max(d, depth(P, seen) + 1); return d; };
  std::vector<std::pair<int, int>> good; for (size_t i = 0; i < ks.size(); i++) if (rms.count(ks[i])) good.push_back({ks[i], depth(ks[i], {})});
  std::stable_sort(good.begin(), good.end(), [](auto& a, auto& b) { return a.second < b.second; });
  struct Made { int m, top; std::vector<int> fam; };
  std::vector<Made> made;
  // Weld locks: the converted locks that touch — one mesh (weldLocks); off — each its own
  struct Grp { std::vector<int> fam; const std::vector<Vec3>* pos; const std::vector<std::vector<int>>* faces; int crown = -1; };
  std::vector<Grp> groups;
  std::vector<std::unique_ptr<std::pair<std::vector<Vec3>, std::vector<std::vector<int>>>>> owned;
  if (sc.fileRaw.value("lockWeld").toBool()) {
    std::vector<LockIn> list; for (auto& [k, dep] : good) list.push_back({k, &rms[k]});
    out.lockWeld = weldLocks(sc, hs, list);
    for (auto& g : out.lockWeld->groups) groups.push_back({g.fam, &g.pos, &g.faces});
  } else for (auto& [k, dep] : good) groups.push_back({{k}, &rms[k].pos, &rms[k].faces});
  auto parentOf = [&](int k) { const Item* it = sc.item(k); const Item* p = it && it->parent >= 0 ? sc.item(it->parent) : nullptr; return p && p->type == ItemType::Cocoon ? p->id : -1; };
  // the crowns: their own mesh (no cocoon); with Weld locks on, the locks on the lower connector welded to it — the
  // lock's root vertices go onto the crown's loop vertices along the connector, one mesh
  for (int kid : crown::crowns(sc)) {
    const Item* K = sc.item(kid); const Item* low = sc.item(K->raw.value("low").toInt(-1)); if (!low) continue;
    std::vector<int> att;                                    // the converted locks attached to it
    for (auto& [k, dep] : good) { LockInput in; cs->lockInput(k, in); bool on = false;
      for (auto& w : in.walls) for (int c : {w.a, w.b}) if (const Item* ci = sc.item(c); ci && !ci->anchors.empty()) for (int a : {ci->anchors.front(), ci->anchors.back()}) if (crown::onConnector(sc, a, *low)) on = true;
      if (on) att.push_back(k); }
    if (att.empty() && !has(crownsSel, kid)) continue;
    auto M = crown::crownData(sc, hs.curves, *K); if (!M || M->faces.empty()) { out.bad << K->name; continue; }
    auto own = std::make_unique<std::pair<std::vector<Vec3>, std::vector<std::vector<int>>>>(M->pos, M->faces);
    Grp cg{{}, &own->first, &own->second, kid};
    if (sc.fileRaw.value("lockWeld").toBool() && !att.empty()) {
      double tol = 0.6 * loopStep(sc, *K);
      std::vector<Grp> rest;
      for (auto& g : groups) {
        bool mine = false; for (int x : g.fam) if (has(att, x)) mine = true;
        if (!mine || g.crown >= 0) { rest.push_back(g); continue; }
        // the lock's open boundary: its vertices near a crown root vertex become that vertex
        std::map<std::pair<int, int>, int> ec; for (auto& f : *g.faces) for (size_t q = 0; q < f.size(); q++) { int a = f[q], b = f[(q + 1) % f.size()]; ec[{std::min(a, b), std::max(a, b)}]++; }
        std::set<int> bnd; for (auto& [e, n] : ec) if (n == 1) { bnd.insert(e.first); bnd.insert(e.second); }
        std::vector<int> map(g.pos->size(), -1);
        for (int v : bnd) { int best = -1; double bd = tol; for (int r : M->root) { double d = (*g.pos)[size_t(v)].distanceTo(own->first[size_t(r)]); if (d < bd) { bd = d; best = r; } } if (best >= 0) { map[size_t(v)] = best; out.welded++; } }
        for (size_t v = 0; v < map.size(); v++) if (map[v] < 0) { map[v] = int(own->first.size()); own->first.push_back((*g.pos)[v]); }
        for (auto& f : *g.faces) { std::vector<int> nf; for (int v : f) { int x = map[size_t(v)]; if (nf.empty() || nf.back() != x) nf.push_back(x); }
          if (nf.size() > 1 && nf.front() == nf.back()) nf.pop_back(); if (nf.size() >= 3) own->second.push_back(nf); }
        cg.fam.insert(cg.fam.end(), g.fam.begin(), g.fam.end());
      }
      groups = rest;
    }
    groups.push_back(cg); owned.push_back(std::move(own));
  }
  for (auto& grp : groups) {
    if (grp.fam.empty() && grp.crown >= 0) {                 // (a crown alone)
      const Item* K = sc.item(grp.crown); int gid = K->parent; const Item* g = gid >= 0 ? sc.item(gid) : nullptr;
      nextNumber(sc, ItemType::Mesh, "Mesh", "mesh");
      Item& m = sc.addItem(ItemType::Mesh, gid, (g ? g->name : K->name) + " mesh");
      auto poly = std::make_shared<PolyMesh>(); poly->pos = *grp.pos; for (auto& f : *grp.faces) poly->addFace(f.data(), int(f.size())); m.poly = poly;
      QJsonArray P, F; for (auto& p : *grp.pos) P.append(QJsonArray{p.x, p.y, p.z}); for (auto& f : *grp.faces) { QJsonArray q; for (int i : f) q.append(i); F.append(q); }
      m.raw = QJsonObject{{"data", QJsonObject{{"pos", P}, {"faces", F}}}, {"srcSurfs", QJsonArray()}, {"srcCrowns", QJsonArray{grp.crown}}, {"symBaked", false}};
      made.push_back({m.id, grp.crown, {}});
      if (g) sc.item(gid)->visible = false;
      continue;
    }
    int k = grp.fam[0]; for (int x : grp.fam) if (!has(grp.fam, parentOf(x))) { k = x; break; }
    int gid = cs->lockGroup(k); const Item* g = gid >= 0 ? sc.item(gid) : nullptr;
    QString gname = g ? g->name : QString(), kname = sc.item(k)->name;
    std::vector<LockInput::Wall> walls; for (int x : grp.fam) { LockInput in; cs->lockInput(x, in); walls.insert(walls.end(), in.walls.begin(), in.walls.end()); }
    nextNumber(sc, ItemType::Mesh, "Mesh", "mesh");
    Item& m = sc.addItem(ItemType::Mesh, gid, (g ? gname : kname) + " mesh");
    auto poly = std::make_shared<PolyMesh>(); poly->pos = *grp.pos; for (auto& f : *grp.faces) poly->addFace(f.data(), int(f.size()));
    m.poly = poly;
    QJsonArray P, F, S; for (auto& p : *grp.pos) P.append(QJsonArray{p.x, p.y, p.z});
    for (auto& f : *grp.faces) { QJsonArray q; for (int i : f) q.append(i); F.append(q); }
    for (auto& w : walls) S.append(w.id);
    m.raw = QJsonObject{{"data", QJsonObject{{"pos", P}, {"faces", F}}}, {"srcSurfs", S}, {"srcCrowns", grp.crown >= 0 ? QJsonArray{grp.crown} : QJsonArray()}, {"symBaked", false}, {"fromCocoon", k}};
    made.push_back({m.id, k, grp.fam});
    if (grp.crown >= 0) { const Item* K = sc.item(grp.crown); if (K->parent >= 0) sc.item(K->parent)->visible = false; }   // (the crown's group hidden too)
    // (the locks' groups hidden — their curves and walls stay as they are; a wall outside any lock group — its own
    // group, else it alone)
    std::vector<int> hide; for (int x : grp.fam) { int lg = cs->lockGroup(x); if (lg >= 0) addU(hide, lg); }
    for (auto& w : walls) {
      Item* sf = sc.item(w.id); if (!sf) continue; Item* pg = sf->parent >= 0 ? sc.item(sf->parent) : nullptr;
      if (pg && pg->type == ItemType::Group) { bool in_ = false; for (int x : hide) if (x == pg->id || sc.isDescendant(pg->id, x)) in_ = true; if (!in_) hide.push_back(pg->id); }
      else sf->visible = false;
    }
    for (int x : hide) sc.item(x)->visible = false;
  }
  // the braids: each its separate tubes, a mesh of its own — welded to nothing (not to the locks, not to each other)
  for (int bid : brs) {
    const Item* B = sc.item(bid); PolyMesh pm = braid::tubesMesh(sc, hs.curves, *B, center); if (pm.faceCount() <= 0) { out.bad << B->name; continue; }
    const int gid = B->parent; const Item* g = gid >= 0 ? sc.item(gid) : nullptr; const QString name = (g && g->raw.value("braidGroup").toBool() ? g->name : B->name) + " mesh";
    nextNumber(sc, ItemType::Mesh, "Mesh", "mesh");
    Item& m = sc.addItem(ItemType::Mesh, gid, name); m.poly = std::make_shared<PolyMesh>(std::move(pm));
    QJsonArray P, F; for (auto& p : m.poly->pos) P.append(QJsonArray{p.x, p.y, p.z});
    for (int f = 0; f < m.poly->faceCount(); f++) { QJsonArray q; for (int k = m.poly->faceStart[size_t(f)]; k < m.poly->faceStart[size_t(f) + 1]; k++) q.append(m.poly->faceVerts[size_t(k)]); F.append(q); }
    m.raw = QJsonObject{{"data", QJsonObject{{"pos", P}, {"faces", F}}}, {"srcSurfs", QJsonArray()}, {"srcCrowns", QJsonArray()}, {"symBaked", false}, {"fromBraid", bid}};
    made.push_back({m.id, bid, {}});
    if (g && g->raw.value("braidGroup").toBool()) sc.item(gid)->visible = false; else sc.item(bid)->visible = false;
  }
  // (two meshes of one group — its two cocoons converted apart: each named by its cocoon too)
  { std::map<QString, int> cnt; for (auto& x : made) cnt[sc.item(x.m)->name]++;
    for (auto& x : made) { Item* m = sc.item(x.m); if (cnt[m->name] > 1) m->name = QString("%1 (%2)").arg(m->name, sc.item(x.top)->name); } }
  // (the converted cocoons' rings hidden — the mesh shows the loops now)
  for (auto& x : made) for (int k : x.fam) sc.item(k)->raw.insert("ringsOff", true);
  for (int T : folded) { out.folds += int(cs->folds(T).size()); out.folded << sc.item(T)->name; }
  cocoonFoldClear(hs, folded);
  meshesIntoGroup(sc);
  // (the meshes made manifold: no edge of three polygons or more, no parts touching only at a vertex, no doubled polygon)
  for (auto& x : made) if (Item* mi = sc.item(x.m); mi && mi->poly) { auto p = std::make_shared<PolyMesh>(*mi->poly); if (tedit::makeManifold(*p)) { const bool same = mi->base == mi->poly; mi->poly = p; if (same || !mi->base) mi->base = p; } }
  for (auto& x : made) out.meshes.push_back(x.m);
  for (auto& [k, r] : rms) { out.flips += r.flips; out.dropped += r.dropped; out.welded += r.welded; }
  int n = int(made.size());
  out.message = QString("%1 mesh%2 in \"Meshes\"").arg(n).arg(n == 1 ? "" : "es");
  if (weld.on) out.message += QString(" · %1 point%2 welded").arg(out.welded).arg(out.welded == 1 ? "" : "s");
  out.message += out.flips ? QString(" · normals: %1 face%2 turned").arg(out.flips).arg(out.flips == 1 ? "" : "s") : QString(" · normals OK");
  if (out.folds) out.message += QString(" · %1 loop%2 added where the lock folded (%3)").arg(out.folds).arg(out.folds == 1 ? "" : "s").arg(out.folded.join(", "));
  if (out.lockWeld) { auto& lw = *out.lockWeld; out.message += QString(" · locks welded: %1 points joined, %2 faces of double walls and %3 inside other locks removed, %4 faces closing holes").arg(lw.merged).arg(lw.doubled).arg(lw.inside).arg(lw.filled); }
  if (out.dropped) out.message += QString(" · %1 ring%2 left out (no wall of the lock crosses it near it, in order)").arg(out.dropped).arg(out.dropped == 1 ? "" : "s");
  if (!out.bad.isEmpty()) out.message += " · not converted: " + out.bad.join(", ");
  return out;
}

}  // namespace hm
