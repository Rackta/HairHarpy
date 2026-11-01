// The Topology room's shaping and cleaning tools (the others: ViewportTopoTools.cpp, ViewportTopoTools2.cpp):
//  Edge Shaper — selects as the Select Cursor; a drag in empty space shapes the selected chains of edges by how far it
//   goes (→): LMB — even spacing, the shape kept; RMB — Edge Flow (each vertex where the edges across it carry on
//   smoothly); Shift+LMB — flat on a plane (the chain's ends on it; three vertices picked — the plane through them);
//   Shift+RMB — straight (Straighten).
//  Slide — selects as the Select Cursor; a drag in empty space slides the selected vertices along their edges: LMB
//   proportionally (the same share of each edge), RMB in parallel (the same distance).
//  Align Normals / Reverse Normals, the Normals tool (a drag in empty space turns the selected vertices' normals;
//   Smooth — the selection smooth, hard at its border; Copy — from another mesh, Shift + click it), Optimize (Ctrl+RMB
//   drag — the weld distance's circle), Collapse Edge, Relax (Ctrl+R), Snap to the references, Mirror (by an axis,
//   round it, a kaleidoscope, half a flower) and the group with a virtual symmetry.
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/SceneGeometry.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_map>

namespace {
#include "view/FaceTemplate.inc"
}

namespace hm {

using tedit::MeshEd; using tedit::ek;

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
std::vector<Vec3> catmullR(const std::vector<Vec3>& a, bool closed, int per) {
  const int n = int(a.size()); if (n < 2) return a;
  auto at = [&](int i) { return closed ? a[size_t(((i % n) + n) % n)] : a[size_t(std::clamp(i, 0, n - 1))]; };
  std::vector<Vec3> out; const int spans = closed ? n : n - 1;
  for (int i = 0; i < spans; i++) { const Vec3 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
    for (int s = 0; s < per; s++) { const double t = double(s) / per, t2 = t * t, t3 = t2 * t; out.push_back((p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2 + (p1 * 3 - p0 - p2 * 3 + p3) * t3) * 0.5); } }
  out.push_back(closed ? a.front() : a.back()); return out;
}
std::vector<double> cumL(const std::vector<Vec3>& L) { std::vector<double> c(L.size(), 0); for (size_t i = 1; i < L.size(); i++) c[i] = c[i - 1] + L[i].distanceTo(L[i - 1]); return c; }
Vec3 atLen(const std::vector<Vec3>& L, const std::vector<double>& c, double d) {
  if (L.size() < 2) return L.empty() ? Vec3() : L[0]; d = std::clamp(d, 0.0, c.back());
  size_t i = size_t(std::upper_bound(c.begin(), c.end(), d) - c.begin()); i = std::clamp<size_t>(i, 1, L.size() - 1);
  const double s = c[i] - c[i - 1]; return L[i - 1] + (L[i] - L[i - 1]) * (s > 0 ? (d - c[i - 1]) / s : 0);
}
// (vertex adjacency of a mesh)
std::vector<std::vector<int>> adjOf(const PolyMesh& B) {
  std::vector<std::set<int>> a(B.pos.size());
  for (int f = 0; f < B.faceCount(); f++) { const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1];
    for (int k = s; k < e; k++) { const int x = B.faceVerts[size_t(k)], y = B.faceVerts[size_t(k + 1 < e ? k + 1 : s)]; a[size_t(x)].insert(y); a[size_t(y)].insert(x); } }
  std::vector<std::vector<int>> o(a.size()); for (size_t i = 0; i < a.size(); i++) o[i].assign(a[i].begin(), a[i].end()); return o;
}
// the smallest-spread direction of points (a plane's normal) — the covariance's weakest axis
Vec3 planeNormal(const std::vector<Vec3>& P, const Vec3& o, const Vec3* keep = nullptr) {
  double C[3][3] = {}; for (auto& p : P) { const Vec3 d = p - o; const double v[3] = {d.x, d.y, d.z}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) C[i][j] += v[i] * v[j]; }
  // (power iteration on the strongest two, the normal across them)
  auto mul = [&](const Vec3& v) { return Vec3(C[0][0] * v.x + C[0][1] * v.y + C[0][2] * v.z, C[1][0] * v.x + C[1][1] * v.y + C[1][2] * v.z, C[2][0] * v.x + C[2][1] * v.y + C[2][2] * v.z); };
  Vec3 a = keep ? keep->normalized() : Vec3(1, 0.3, 0.2).normalized();
  if (!keep) for (int i = 0; i < 40; i++) { Vec3 b = mul(a); if (b.lengthSq() < 1e-30) break; a = b.normalized(); }
  Vec3 b = std::abs(a.x) < 0.9 ? Vec3(1, 0, 0).cross(a).normalized() : Vec3(0, 1, 0).cross(a).normalized();
  for (int i = 0; i < 40; i++) { Vec3 c = mul(b); c = c - a * c.dot(a); if (c.lengthSq() < 1e-30) break; b = c.normalized(); }
  Vec3 n = a.cross(b); return n.lengthSq() > 0 ? n.normalized() : Vec3(0, 1, 0);
}
}  // namespace

// ---------- the selection as chains / vertices ----------
namespace {
struct ChainSel { std::vector<tedit::Chain> chains; };
}  // namespace

// ---------- Edge Shaper / Slide / Normals / Optimize: a drag in empty space ----------
bool Viewport::shapePress(const QPointF& mp, bool rmb, bool shift, bool ctrl) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || topoHeavy(m)) return false;
  // (Optimize and Normals select as the Select Cursor; their actions come from the menu — a button there arms the next
  // press: the weld distance picked by a drag, the normals turned by a drag, the mesh clicked to copy the normals from)
  if (tool_ == "toptimize") {
    if (tt_.arm != "optpick") return false; tt_.arm.clear();
    tt_.m = m; tt_.press = mp; tt_.drag = 16; tt_.adj0 = ttools.optWeld / std::max(1e-12, worldPerPixelAt(cam_.pivot)); td_ = {}; td_.type = 20; drag_ = Drag::Topo; return true;
  }
  if (tool_ == "tnormals" && tt_.arm == "ncopy") { tt_.arm.clear();
    if (auto r = tPick(mp, topoMeshes()); r && r->m != m) { tNormalsCopy(r->m); return true; }
    ui::toast(tr("Copy normals: click another mesh — try again from the menu")); return true;
  }
  if (tool_ == "tnormals" && tt_.arm != "nturn") return false;
  if (tool_ == "tnormals") tt_.arm.clear();
  if (ctrl && !rmb && (tool_ == "tshaper" || tool_ == "tslide" || tool_ == "tnormals")) return false;   // (Ctrl + LMB: the selection, as the Cursor)
  if (auto r0 = tool_ == "tnormals" ? std::optional<TopoPick>{} : tPick(mp, {m})) {   // (on the mesh: the selection, as the Select Cursor — but Edge Shaper / Slide:
    bool onSel = false; const char key = tKey();                    //  RMB anywhere, LMB from the selection itself — the shaping)
    if (tsel_.id == m) { const int el = key == 'f' ? r0->f : key == 'e' ? r0->e : r0->v; onSel = (key == 'f' ? tsel_.f : key == 'e' ? tsel_.e : tsel_.v).count(el) > 0; }
    if (!((tool_ == "tshaper" || tool_ == "tslide") && (rmb || onSel))) return false;
  }
  if (tool_ == "toptimize") return false;
  const PolyMesh& B = *baseOf(*it); auto T = topo::ttopo(baseSp(*it));
  tt_.m = m; tt_.press = mp; tt_.moved = false; tt_.verts.clear(); tt_.tgt.clear(); tt_.nbs.clear(); tt_.base = B.pos; tt_.lead = -1;
  if (tool_ == "tnormals") {
    tt_.verts = tSelVerts(m); if (tt_.verts.empty()) { ui::toast(tr("Select the vertices / polygons whose normals turn")); return true; }
    if (B.cornerNormal.size() == B.faceVerts.size() && !B.cornerNormal.empty()) tt_.n0 = B.cornerNormal;
    else { tt_.n0.clear(); for (auto& n : hardCornerNormals(B)) tt_.n0.push_back({float(n.x), float(n.y), float(n.z)}); }
    tt_.before = baseSp(*it); tt_.drag = 15; td_ = {}; td_.type = 20; drag_ = Drag::Topo; return true;
  }
  if (tool_ == "tslide") {
    tt_.verts = tSelVerts(m); if (tt_.verts.empty()) { ui::toast(tr("Select vertices / edges to slide")); return true; }
    const auto A = adjOf(B); std::set<int> sel(tt_.verts.begin(), tt_.verts.end());
    for (int v : tt_.verts) { std::vector<int> nb; for (int w : A[size_t(v)]) if (!sel.count(w)) nb.push_back(w); tt_.nbs.push_back(nb); }
    Projector P = projector(); const auto W = ttWorld(m); double bd = 1e300;
    for (size_t i = 0; i < tt_.verts.size(); i++) { ScreenPt s = P.toScreen(W[size_t(tt_.verts[i])]); const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (d < bd && !tt_.nbs[i].empty()) { bd = d; tt_.lead = int(i); } }
    if (tt_.lead < 0) { ui::toast(tr("Nothing to slide along")); return true; }
    tt_.shapeOp = rmb ? 1 : 0; tt_.drag = 14; td_ = {}; td_.type = 20; drag_ = Drag::Topo; return true;
  }
  if (tool_ != "tshaper") return false;
  // ---- Edge Shaper on polygons (or on edges / vertices covering whole polygons): the inside shaped, the rim kept —
  // Shift: flat on the plane the rim lies nearest, the grid evened out on it; RMB: every line of edges across it a smooth
  // curve (each vertex at its share along it — as a Smooth EdgeChain curve on each line, all at once); LMB: a smooth
  // surface spanned by the rim (each inner vertex the middle of its neighbours, till it settles) ----
  std::set<int> polyS; const char key0 = tKey();
  if (tsel_.id == m) {
    if (key0 == 'f') polyS.insert(tsel_.f.begin(), tsel_.f.end());
    else if (key0 == 'e' || (key0 == 'v' && !(shift && !rmb && tsel_.v.size() == 3))) {   // (three vertices, Shift+LMB: their plane — as before)
      std::set<std::pair<int, int>> se; if (key0 == 'e') for (int e : tsel_.e) se.insert({std::min(T->ea[size_t(e)], T->eb[size_t(e)]), std::max(T->ea[size_t(e)], T->eb[size_t(e)])});
      for (int f = 0; f < T->nf; f++) { bool all = true; const int s = T->fo[size_t(f)], e = T->fo[size_t(f) + 1];
        for (int k = s; k < e && all; k++) { const int a = T->fv[size_t(k)], b = T->fv[size_t(k + 1 < e ? k + 1 : s)];
          all = key0 == 'e' ? se.count({std::min(a, b), std::max(a, b)}) > 0 : tsel_.v.count(a) > 0; }
        if (all) polyS.insert(f); }
      if (polyS.size() < 2) polyS.clear(); } }
  if (!polyS.empty()) {
    MeshEd E = MeshEd::from(B); const std::set<int>& S = polyS; std::set<int> rim; for (auto& [a, b] : tedit::regionBorder(E, S)) { rim.insert(a); rim.insert(b); }
    std::set<int> in; for (int f : S) if (f >= 0 && size_t(f) < E.f.size()) for (int v : E.f[size_t(f)].v) if (!rim.count(v)) in.insert(v);
    if (in.empty()) { ui::toast(tr("No vertices inside the selection to shape — its rim stays as it is")); return true; }
    const auto A = adjOf(B); std::map<int, Vec3> tgt; std::vector<Vec3> X = B.pos; int op4 = 4;
    auto relax = [&](int n, const Vec3* o, const Vec3* nrm) { for (int it2 = 0; it2 < n; it2++) for (int v : in) { Vec3 s; int c = 0; for (int w : A[size_t(v)]) { s += X[size_t(w)]; c++; } if (!c) continue;
        Vec3 p = s * (1.0 / c); if (o) p = p - *nrm * (p - *o).dot(*nrm); X[size_t(v)] = p; } };
    if (shift) { std::vector<Vec3> R; Vec3 o; for (int v : rim) { R.push_back(B.pos[size_t(v)]); o += B.pos[size_t(v)]; } o = o * (1.0 / double(std::max<size_t>(1, R.size())));
      Vec3 nrm = planeNormal(R, o); if (nrm.lengthSq() <= 0) { ui::toast(tr("The rim lies on a line — no plane")); return true; } nrm = nrm.normalized();
      for (int v : in) X[size_t(v)] = B.pos[size_t(v)] - nrm * (B.pos[size_t(v)] - o).dot(nrm);
      relax(300, &o, &nrm); op4 = 2; }                               // (on the plane, the grid evened out — no creases left in it)
    else if (rmb) {                                                   // (the lines: walked from the rim across the inside, straight on through each
      std::map<int, std::vector<int>> fOf; for (int f : S) for (int v : E.f[size_t(f)].v) fOf[v].push_back(f);   //  vertex of four edges)
      auto inS = [&](int a, int b) { for (int f : fOf[a]) { const auto& V = E.f[size_t(f)].v; if (std::find(V.begin(), V.end(), b) != V.end()) return f; } return -1; };
      auto across = [&](int prev, int cur) { if (A[size_t(cur)].size() != 4) return -1; std::set<int> fp; for (int f : fOf[cur]) { const auto& V = E.f[size_t(f)].v; if (std::find(V.begin(), V.end(), prev) != V.end()) fp.insert(f); }
        for (int w : A[size_t(cur)]) { if (w == prev) continue; bool share = false; for (int f : fOf[cur]) { const auto& V = E.f[size_t(f)].v; if (fp.count(f) && std::find(V.begin(), V.end(), w) != V.end()) share = true; } if (!share) return w; }
        return -1; };
      std::vector<std::vector<int>> lines; std::set<std::array<int, 3>> seenL;
      std::set<int> stops = rim; for (int v : in) if (A[size_t(v)].size() != 4) stops.insert(v);   // (a line runs from the rim or a vertex of other than four edges to the next such)
      for (int r : stops) for (int w : A[size_t(r)]) { if (!in.count(w) || inS(r, w) < 0) continue; std::vector<int> L{r, w}; int prev = r, cur = w;
        for (int guard = 0; guard < 10000 && in.count(cur); guard++) { const int nx = across(prev, cur); if (nx < 0 || inS(cur, nx) < 0) break; L.push_back(nx); prev = cur; cur = nx; }
        if (L.size() < 4 || !stops.count(L.back())) continue; const std::array<int, 3> k = L.front() < L.back() ? std::array<int, 3>{L.front(), L.back(), L[1]} : std::array<int, 3>{L.back(), L.front(), L[L.size() - 2]};   // (walked from either end: once)
        if (seenL.insert(k).second) lines.push_back(L); }
      if (lines.empty()) { ui::toast(tr("No lines of edges across the selection (rim to rim, through vertices of four edges)")); return true; }
      for (int pass = 0; pass < 12; pass++) {                          // (each line a cubic from its ends, nearest its points; the crossing lines averaged)
        std::map<int, Vec3> sum; std::map<int, int> cnt;
        for (const auto& L : lines) { const size_t n = L.size(); std::vector<double> t(n, 0.0); for (size_t i = 1; i < n; i++) t[i] = t[i - 1] + X[size_t(L[i])].distanceTo(X[size_t(L[i - 1])]); if (t.back() <= 0) continue; for (auto& x : t) x /= t.back();
          const Vec3 P0 = X[size_t(L.front())], P3 = X[size_t(L.back())]; double a11 = 0, a12 = 0, a22 = 0; Vec3 r1, r2;
          for (size_t i = 0; i < n; i++) { const double u = t[i], v = 1 - u, b0 = v * v * v, b1 = 3 * u * v * v, b2 = 3 * u * u * v, b3 = u * u * u; const Vec3 r = X[size_t(L[i])] - P0 * b0 - P3 * b3;
            a11 += b1 * b1; a12 += b1 * b2; a22 += b2 * b2; r1 += r * b1; r2 += r * b2; }
          const double det = a11 * a22 - a12 * a12; if (std::abs(det) < 1e-18) continue; const Vec3 P1 = (r1 * a22 - r2 * a12) * (1.0 / det), P2 = (r2 * a11 - r1 * a12) * (1.0 / det);
          for (size_t i = 1; i + 1 < n; i++) { if (!in.count(L[i])) continue; const double u = t[i], v = 1 - u; sum[L[i]] += P0 * (v * v * v) + P1 * (3 * u * v * v) + P2 * (3 * u * u * v) + P3 * (u * u * u); cnt[L[i]]++; } }
        for (auto& [v, s] : sum) X[size_t(v)] = s * (1.0 / cnt[v]); }
      op4 = 5; }
    else relax(400, nullptr, nullptr);
    for (int v : in) if (X[size_t(v)].distanceTo(B.pos[size_t(v)]) > 0) tgt[v] = X[size_t(v)];
    if (tgt.empty()) { ui::toast(tr("Nothing to shape here")); return true; }
    for (auto& [v, p] : tgt) { tt_.verts.push_back(v); tt_.tgt.push_back(p); }
    tt_.shapeOp = op4; tt_.drag = 13; td_ = {}; td_.type = 20; drag_ = Drag::Topo;
    const char* N[] = {"", "", "Flat", "", "Smooth", "Smooth lines"}; ui::hud(tr("%1 — the rim kept — drag →").arg(tr(N[op4]))); return true;
  }
  // ---- Edge Shaper: the selected chains, each vertex's target at full strength ----
  std::vector<tedit::EK> es; const char key = tKey();
  if (tsel_.id == m) {
    if (key == 'e') for (int e : tsel_.e) es.push_back(ek(T->ea[size_t(e)], T->eb[size_t(e)]));
    else if (key == 'v') { for (int e = 0; e < T->ne; e++) if (tsel_.v.count(T->ea[size_t(e)]) && tsel_.v.count(T->eb[size_t(e)])) es.push_back(ek(T->ea[size_t(e)], T->eb[size_t(e)])); }
    else { MeshEd E = MeshEd::from(B); std::set<int> S(tsel_.f.begin(), tsel_.f.end()); for (auto& [a, b] : tedit::regionBorder(E, S)) es.push_back(ek(a, b)); }
  }
  const int op = (shift ? 2 : 0) + (rmb ? 1 : 0);                  // 0 equal, 1 flow, 2 plane, 3 straight
  std::map<int, Vec3> tgt;
  // (three vertices picked, a plane: through them, the vertices on the shortest ways between them)
  if (op == 2 && key == 'v' && tsel_.id == m && tsel_.v.size() == 3) {
    std::vector<int> a(tsel_.v.begin(), tsel_.v.end()); const Vec3 o = B.pos[size_t(a[0])]; Vec3 n = (B.pos[size_t(a[1])] - o).cross(B.pos[size_t(a[2])] - o);
    if (n.lengthSq() <= 0) { ui::toast(tr("The three vertices are in one line — no plane")); return true; } n = n.normalized();
    for (int k = 0; k < 3; k++) if (auto p = topo::path(*T, B.pos, 'v', a[size_t(k)], a[size_t((k + 1) % 3)])) for (int v : *p) tgt[v] = B.pos[size_t(v)] - n * (B.pos[size_t(v)] - o).dot(n);
  } else {
    auto chains = tedit::edgeChains(es);
    if (chains.empty()) { ui::toast(tr("Select a chain of edges (or vertices along one)")); return true; }
    const auto A = adjOf(B);
    for (auto& ch : chains) {
      const auto& V = ch.v; const size_t n = V.size(); if (n < 2) continue;
      std::vector<Vec3> P; for (int v : V) P.push_back(B.pos[size_t(v)]);
      if (op == 0) {                                                // (even: the same length between them along the smooth curve through them)
        std::vector<Vec3> Q = P; if (ch.closed) Q.push_back(P[0]);
        auto L = catmullR(P, ch.closed, 12); auto c = cumL(L); const double tot = c.back();
        for (size_t i = 0; i < n; i++) { if (!ch.closed && (i == 0 || i == n - 1)) continue; tgt[V[i]] = atLen(L, c, tot * double(i) / (ch.closed ? n : n - 1)); }
      } else if (op == 1) {                                         // (Edge Flow: where the edges across carry on smoothly)
        std::set<int> inChain(V.begin(), V.end());
        for (size_t i = 0; i < n; i++) {
          const int v = V[i]; std::vector<int> across; for (int w : A[size_t(v)]) if (!inChain.count(w)) across.push_back(w);
          if (across.size() != 2) continue;
          auto beyond = [&](int p) { const Vec3 d = (B.pos[size_t(p)] - B.pos[size_t(v)]).normalized(); int best = -1; double bd = 0.3;
            for (int q : A[size_t(p)]) { if (q == v) continue; const double al = (B.pos[size_t(q)] - B.pos[size_t(p)]).normalized().dot(d); if (al > bd) { bd = al; best = q; } } return best; };
          const int p = across[0], q = across[1], pp = beyond(p), qq = beyond(q); if (pp < 0 || qq < 0) continue;
          const Vec3 P0 = B.pos[size_t(pp)], P1 = B.pos[size_t(p)], P2 = B.pos[size_t(q)], P3 = B.pos[size_t(qq)]; const double t = 0.5, t2 = 0.25, t3 = 0.125;
          tgt[v] = (P1 * 2 + (P2 - P0) * t + (P0 * 2 - P1 * 5 + P2 * 4 - P3) * t2 + (P1 * 3 - P0 - P2 * 3 + P3) * t3) * 0.5;
        }
      } else if (op == 2) {                                         // (flat: the plane holding the chain's ends, nearest the rest)
        Vec3 o = P[0]; Vec3 nrm;
        if (!ch.closed && n >= 3) { const Vec3 d = (P.back() - P.front()).normalized(); std::vector<Vec3> R; for (auto& p : P) { Vec3 r = p - o; R.push_back(o + (r - d * r.dot(d))); }
          Vec3 c; for (auto& p : R) c += p; c = c * (1.0 / double(R.size())); Vec3 e1; double best = -1;
          for (auto& p : R) { Vec3 x = p - o; if (x.lengthSq() > best) { best = x.lengthSq(); e1 = x; } }
          nrm = e1.lengthSq() > 0 ? d.cross(e1.normalized()) : planeNormal(P, o); }
        else { Vec3 c; for (auto& p : P) c += p; o = c * (1.0 / double(n)); nrm = planeNormal(P, o); }
        if (nrm.lengthSq() <= 0) continue; nrm = nrm.normalized();
        for (size_t i = 0; i < n; i++) tgt[V[i]] = P[i] - nrm * (P[i] - o).dot(nrm);
      } else {                                                      // (straight: on the line between its ends, spaced as they were)
        if (ch.closed) { ui::toast(tr("A closed loop can't be straightened")); continue; }
        auto c = cumL(P); const double tot = c.back(); if (tot <= 0) continue;
        for (size_t i = 1; i + 1 < n; i++) tgt[V[i]] = P[0] + (P.back() - P[0]) * (c[i] / tot);
      }
    }
  }
  if (tgt.empty()) { ui::toast(tr("Nothing to shape here")); return true; }
  for (auto& [v, p] : tgt) { tt_.verts.push_back(v); tt_.tgt.push_back(p); }
  tt_.shapeOp = op; tt_.drag = 13; td_ = {}; td_.type = 20; drag_ = Drag::Topo;
  const char* N[] = {"Even spacing", "Edge Flow", "Flat", "Straight", "Smooth", "Smooth lines"}; ui::hud(tr("%1 — drag →").arg(tr(N[op])));
  return true;
}

void Viewport::shapeMove(const QPointF& mp) {
  const int m = tt_.m; Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || !it->base) return;
  const double dx = mp.x() - tt_.press.x(), dy = mp.y() - tt_.press.y(); tt_.moved = tt_.moved || std::hypot(dx, dy) >= 3;
  if (tt_.drag == 16) { ttools.optWeld = std::max(1e-6, std::max(1.0, tt_.adj0 + dx) * worldPerPixelAt(cam_.pivot)); ui::hud(tr("Weld distance: %1 mm").arg(ttools.optWeld * 10, 0, 'g', 3)); overlayRev_++; requestUpdate(); return; }
  if (tt_.drag == 13) {
    const double t = std::clamp(dx / 150, 0.0, 1.0); std::vector<Vec3> P = tt_.base;
    for (size_t i = 0; i < tt_.verts.size(); i++) P[size_t(tt_.verts[i])] = tt_.base[size_t(tt_.verts[i])] + (tt_.tgt[i] - tt_.base[size_t(tt_.verts[i])]) * t;
    topoLive(m, P); const char* N[] = {"Even spacing", "Edge Flow", "Flat", "Straight", "Smooth", "Smooth lines"}; ui::hud(tr("%1: %2%").arg(tr(N[tt_.shapeOp])).arg(int(std::lround(t * 100)))); return;
  }
  if (tt_.drag == 14) {                                             // (Slide: the lead vertex's side by the drag; the others the way most like it)
    Projector Pr = projector(); const Affine M = meshmods::meshMatrix(*scene_, *it); const QPointF d(dx, dy);
    const int lv = tt_.verts[size_t(tt_.lead)]; const auto& lnb = tt_.nbs[size_t(tt_.lead)];
    ScreenPt sl = Pr.toScreen(M.apply(tt_.base[size_t(lv)])); int lw = -1; double bdot = -1e300, sx = 0, sy = 0;
    for (int w : lnb) { ScreenPt s = Pr.toScreen(M.apply(tt_.base[size_t(w)])); const double vx = s.x - sl.x, vy = s.y - sl.y, l = std::hypot(vx, vy); if (l < 1e-9) continue;
      const double dd = (vx * d.x() + vy * d.y()) / l; if (dd > bdot) { bdot = dd; lw = w; sx = vx; sy = vy; } }
    if (lw < 0) return;
    const double t = std::clamp((sx * d.x() + sy * d.y()) / std::max(1e-9, sx * sx + sy * sy), 0.0, 1.0);
    const Vec3 ldir = (tt_.base[size_t(lw)] - tt_.base[size_t(lv)]); const double lLen = ldir.length();
    std::vector<Vec3> P = tt_.base;
    for (size_t i = 0; i < tt_.verts.size(); i++) {
      const int v = tt_.verts[i]; int w = -1; double ba = -2;
      for (int x : tt_.nbs[i]) { const double a = (tt_.base[size_t(x)] - tt_.base[size_t(v)]).normalized().dot(ldir.normalized()); if (a > ba) { ba = a; w = x; } }
      if (w < 0) continue; const Vec3 e = tt_.base[size_t(w)] - tt_.base[size_t(v)]; const double L = e.length(); if (L <= 0) continue;
      P[size_t(v)] = tt_.shapeOp == 0 ? tt_.base[size_t(v)] + e * t : tt_.base[size_t(v)] + e * (std::min(t * lLen, L * 0.98) / L);
    }
    topoLive(m, P); ui::hud(tr("Slide %1: %2%").arg(tt_.shapeOp == 0 ? tr("proportional") : tr("parallel")).arg(int(std::lround(t * 100)))); return;
  }
  if (tt_.drag == 15) {                                             // (the normals turned: ←→ round the view's up, ↑↓ round its right)
    const Affine Mi = meshmods::meshMatrix(*scene_, *it).inverse();
    auto rot = [](const Vec3& v, const Vec3& ax, double a) { const Vec3 k = ax.normalized(); return v * std::cos(a) + k.cross(v) * std::sin(a) + k * (k.dot(v) * (1 - std::cos(a))); };
    const Vec3 up = Mi.lin(cam_.upVec()).normalized(), rt = Mi.lin(cam_.right()).normalized(); const double a1 = dx * 0.01, a2 = dy * 0.01;
    auto nb = std::make_shared<PolyMesh>(*tt_.before); nb->cornerNormal = tt_.n0;
    std::set<int> vs(tt_.verts.begin(), tt_.verts.end());
    for (size_t k = 0; k < nb->faceVerts.size(); k++) if (vs.count(nb->faceVerts[k])) { const auto& n = tt_.n0[k]; Vec3 v(n[0], n[1], n[2]); v = rot(rot(v, up, a1), rt, a2).normalized(); nb->cornerNormal[k] = {float(v.x), float(v.y), float(v.z)}; }
    topo::shareTopo(nb, tt_.before); topoWrite(m, nb); tsel_.B = nb.get(); ttTouch();
    ui::hud(tr("Normals turned %1°, %2°").arg(int(std::lround(a1 * 180 / M_PI))).arg(int(std::lround(a2 * 180 / M_PI)))); return;
  }
}

void Viewport::shapeRelease() {
  const int m = tt_.m; Item* it = m >= 0 ? scene_->item(m) : nullptr; const int kind = tt_.drag; tt_.drag = 0; ui::hud({});
  if (kind == 16) { saveModelPrefs(); overlayRev_++; emit topoState(); return; }
  if (!it || !tt_.moved) { ttTouch(); return; }
  if (kind == 15) { tt_.before.reset(); scene_->revision++; commit(true); emit topoState(); return; }
  topoWrite(m, baseSp(*it)); commit(false); emit topoState(); ttTouch();
}

// ---------- the polygons' normals ----------
QString Viewport::tReverseNormals() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); auto fs = tSelFaces(m); if (fs.empty()) { fs.resize(E.f.size()); std::iota(fs.begin(), fs.end(), 0); }
  for (int f : fs) { auto& F = E.f[size_t(f)]; std::reverse(F.v.begin(), F.v.end()); std::reverse(F.uv.begin(), F.uv.end()); std::reverse(F.n.begin(), F.n.end()); for (auto& q : F.n) q = {-q[0], -q[1], -q[2]}; }
  return ttDone(m, E, B, tr("Normals reversed: %1 polygon%2").arg(fs.size()).arg(fs.size() == 1 ? "" : "s"), topoMode == "poly" ? 'f' : 0, fs);
}

// each joined piece of the selected polygons turned one way (as its polygon facing out the most); none selected — all
QString Viewport::tAlignNormals() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); auto fs = tSelFaces(m); if (fs.empty()) { fs.resize(E.f.size()); std::iota(fs.begin(), fs.end(), 0); }
  std::set<int> S(fs.begin(), fs.end()); std::map<tedit::EK, std::vector<int>> byEdge;
  for (int f : fs) { const auto& V = E.f[size_t(f)].v; for (size_t k = 0; k < V.size(); k++) byEdge[ek(V[k], V[(k + 1) % V.size()])].push_back(f); }
  Vec3 mc; for (auto& p : E.pos) mc += p; mc = mc * (1.0 / std::max<size_t>(1, E.pos.size()));
  std::set<int> seen; int flipped = 0;
  auto runs = [&](int f, int a, int b) { const auto& V = E.f[size_t(f)].v; for (size_t k = 0; k < V.size(); k++) if (V[k] == a && V[(k + 1) % V.size()] == b) return true; return false; };
  for (int f0 : fs) {
    if (seen.count(f0)) continue;
    // (the piece: walked across shared edges, each neighbour turned to agree)
    std::vector<int> piece{f0}, st{f0}; seen.insert(f0); std::vector<char> turned(E.f.size(), 0);
    while (!st.empty()) { const int f = st.back(); st.pop_back(); const auto V = E.f[size_t(f)].v;
      for (size_t k = 0; k < V.size(); k++) { const int a = V[k], b = V[(k + 1) % V.size()];
        for (int g : byEdge[ek(a, b)]) { if (g == f || seen.count(g)) continue; seen.insert(g); piece.push_back(g); st.push_back(g);
          if (runs(g, a, b)) { auto& G = E.f[size_t(g)]; std::reverse(G.v.begin(), G.v.end()); std::reverse(G.uv.begin(), G.uv.end()); std::reverse(G.n.begin(), G.n.end()); for (auto& q : G.n) q = {-q[0], -q[1], -q[2]}; turned[size_t(g)] = 1; } } } }
    // (the piece facing in on the whole — from the mesh's middle: all of it turned)
    double out = 0; for (int f : piece) out += E.normal(f).dot(E.center(f) - mc);
    if (out < 0) for (int f : piece) { auto& G = E.f[size_t(f)]; std::reverse(G.v.begin(), G.v.end()); std::reverse(G.uv.begin(), G.uv.end()); std::reverse(G.n.begin(), G.n.end()); for (auto& q : G.n) q = {-q[0], -q[1], -q[2]}; turned[size_t(f)] ^= 1; }
    for (int f : piece) flipped += turned[size_t(f)];
  }
  if (!flipped) { ui::toast(tr("The normals already face one way")); return "!"; }
  return ttDone(m, E, B, tr("Normals aligned: %1 polygon%2 turned").arg(flipped).arg(flipped == 1 ? "" : "s"), topoMode == "poly" ? 'f' : 0, fs);
}

// the averaged normals of the polygons fs (their vertices) as the tool's mode makes them — Smoothed shape, Sphere, Face
std::map<int, Vec3> Viewport::normTarget(const Item& it, const PolyMesh& d, const std::vector<int>& fs, bool ownN) const {
  std::map<int, Vec3> P; for (int f : fs) for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) P[d.faceVerts[size_t(k)]] = d.pos[size_t(d.faceVerts[size_t(k)])];
  std::map<int, Vec3> vn;
  if (ttools.normMode == "sphere") {
    Vec3 c; double r = 0; for (auto& [v, p] : P) c += p; c = c * (1.0 / double(P.size())); for (auto& [v, p] : P) r += p.distanceTo(c); r /= double(P.size());
    Vec3 avgN; for (int f : fs) { const int s = d.faceStart[size_t(f)], e = d.faceStart[size_t(f) + 1];
      for (int k = s + 1; k + 1 < e; k++) avgN += (d.pos[size_t(d.faceVerts[size_t(k)])] - d.pos[size_t(d.faceVerts[size_t(s)])]).cross(d.pos[size_t(d.faceVerts[size_t(k + 1)])] - d.pos[size_t(d.faceVerts[size_t(s)])]); }
    const Vec3 o = c - (avgN.lengthSq() > 0 ? avgN.normalized() : Vec3()) * (r * 0.6);   // (the middle set back inside the shape)
    for (auto& [v, p] : P) vn[v] = (p - o).normalized();
  } else if (ttools.normMode == "face") {
    // (a stylised face's normals, as game faces have them: the normals of a game face (FaceTemplate.inc — the flat cheeks,
    // the nose's sides turned sharply away, the chin's underside down: the shadow's ">" by the nose) laid over this face
    // from its nose tip, scaled by its width; round it — and on the head's sides and back — an ellipsoid's normals, the
    // gradient (p − c) / r² (tall 1 : 1.8 : 1, its centre deep inside, high up). Nose: 0 — the ellipsoid alone, 100% — the
    // game face's. Worked out in the face's own axes: up — the world's, forward — where the polygons face)
    const Affine M = meshmods::meshMatrix(*scene_, it), Mi = M.inverse();
    // (forward: the mesh's own normals when it has them — a ripped game model's polygons are often wound the other way;
    // else the polygons', turned toward +Z, where the characters look)
    Vec3 fw; for (int f : fs) { const int s = d.faceStart[size_t(f)], e = d.faceStart[size_t(f) + 1];
      if (ownN) { for (int k = s; k < e; k++) { const auto& c = d.cornerNormal[size_t(k)]; fw += M.normal(Vec3(c[0], c[1], c[2])); } continue; }
      for (int k = s + 1; k + 1 < e; k++) { const Vec3 a = M.apply(d.pos[size_t(d.faceVerts[size_t(s)])]); fw += (M.apply(d.pos[size_t(d.faceVerts[size_t(k)])]) - a).cross(M.apply(d.pos[size_t(d.faceVerts[size_t(k + 1)])]) - a); } }
    const Vec3 up(0, 1, 0); fw = fw - up * fw.dot(up); fw = fw.lengthSq() > 0 ? fw.normalized() : Vec3(0, 0, 1);
    if (!ownN && fw.z < 0) fw = fw * -1.0;
    const Vec3 sd = up.cross(fw).normalized();
    std::map<int, Vec3> L; Vec3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300);
    for (auto& [v, p] : P) { const Vec3 w = M.apply(p); const Vec3 q(w.dot(sd), w.dot(up), w.dot(fw)); L[v] = q;
      lo = Vec3(std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)); hi = Vec3(std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)); }
    // (placed from the nose tip — the front-most point — and the face's width at its height, not from the selection's box:
    // a selection up to the crown or with the ears in stays right)
    const Vec3 sz = hi - lo; Vec3 tip = lo; for (auto& [v, q] : L) if (q.z > tip.z) tip = q;
    double wl = 1e300, wh = -1e300; for (auto& [v, q] : L) if (std::abs(q.y - tip.y) < 0.03 * sz.y) { wl = std::min(wl, q.x); wh = std::max(wh, q.x); }
    const double cx = tip.x, W = std::max(1e-9, wh > wl ? wh - wl : sz.x), mid = std::clamp(ttools.normNose, 0.0, 2.0);
    const Vec3 cf(cx, tip.y + 0.538 * W, tip.z - 0.608 * W);
    for (auto& [v, q] : L) {
      const Vec3 e0 = q - cf; const Vec3 E = Vec3(e0.x, e0.y / (1.79 * 1.79), e0.z).normalized();
      // (the template, bilinear over the cells that have the face; its share — how many of the four do — and only on the
      // front: where the ellipsoid faces forward, so the head's sides and back keep the ellipsoid)
      const double fu = std::clamp(((q.x - cx) / W - FT_U0) / FT_ST, 0.0, FT_NU - 1.001), fv = std::clamp(((q.y - tip.y) / W - FT_V0) / FT_ST, 0.0, FT_NV - 1.001);
      const int i0 = int(fu), j0 = int(fv); const double a = fu - i0, b = fv - j0;
      Vec3 T; double al = 0;
      for (const auto& [di, dj, w] : {std::tuple{0, 0, (1 - a) * (1 - b)}, std::tuple{1, 0, a * (1 - b)}, std::tuple{0, 1, (1 - a) * b}, std::tuple{1, 1, a * b}}) {
        const short* n = &FT_N[size_t(((j0 + dj) * FT_NU + (i0 + di)) * 3)]; if (!n[0] && !n[1] && !n[2]) continue;
        T += Vec3(n[0], n[1], n[2]) * (w / 1000.0); al += w; }
      const double inU = (q.x - cx) / W, inV = (q.y - tip.y) / W;
      if (inU < FT_U0 || inU > FT_U0 + FT_ST * (FT_NU - 1) || inV < FT_V0 || inV > FT_V0 + FT_ST * (FT_NV - 1)) { T = Vec3(); al = 0; }
      const double front = std::clamp(E.z / 0.3, 0.0, 1.0); T = T * front; al *= front;
      const Vec3 G = (T + E * (1 - al)).normalized();
      const Vec3 g = (E + (G - E) * mid).lengthSq() > 0 ? (E + (G - E) * mid).normalized() : E;
      vn[v] = Mi.normal(sd * g.x + up * g.y + fw * g.z); }
  } else {
    // (each vertex: the polygons' normals round it averaged, weighted by their area and a Gaussian of the distance —
    // σ from Smoothness: the forms smaller than it (a nose, eyes) melt away, the larger shape stays. The polygons
    // gathered into cells of σ/2 first, so a large σ is as quick). Smoothness squared: the low end fine — 0 is almost the
    // plain smoothing, a few % already soften (σ from 0.2% to 32% of the selection's size)
    Box3 bb; for (auto& [v, p] : P) bb.expand(p);
    const double sm = std::clamp(ttools.normSmooth, 0.0, 1.0), sg = std::max(1e-9, (0.002 + sm * sm * 0.318) * (bb.max - bb.min).length()), h = sg * 0.5;
    // (every polygon round counts — the hair's layers together make the one large shape)
    struct Cell { Vec3 c, n; double a = 0; };
    auto dirOf = [](const Vec3& n) { const double ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z); return ax >= ay && ax >= az ? (n.x >= 0 ? 0 : 1) : ay >= az ? (n.y >= 0 ? 2 : 3) : (n.z >= 0 ? 4 : 5); };
    std::map<std::array<long long, 4>, Cell> cells; std::map<int, Vec3> own;
    auto key = [h](const Vec3& p) { return std::array<long long, 3>{std::llround(std::floor(p.x / h)), std::llround(std::floor(p.y / h)), std::llround(std::floor(p.z / h))}; };
    for (int f : fs) { const int s = d.faceStart[size_t(f)], e = d.faceStart[size_t(f) + 1]; Vec3 n, c;
      for (int k = s; k < e; k++) { const Vec3& a = d.pos[size_t(d.faceVerts[size_t(k)])]; const Vec3& b = d.pos[size_t(d.faceVerts[size_t(k + 1 < e ? k + 1 : s)])]; c += a;
        n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
      c = c * (1.0 / double(e - s)); const double ar = n.length() * 0.5; if (ar <= 0) continue;
      for (int k = s; k < e; k++) own[d.faceVerts[size_t(k)]] += n;
      const auto k3 = key(c); Cell& C = cells[{k3[0], k3[1], k3[2], dirOf(n)}]; C.c += c * ar; C.n += n * 0.5; C.a += ar; }
    auto pack = [](long long i, long long j, long long l) { return ((i & 0x1FFFFF) << 42) | ((j & 0x1FFFFF) << 21) | (l & 0x1FFFFF); };
    std::vector<Cell> L; L.reserve(cells.size()); std::unordered_map<long long, std::vector<int>> at;
    for (auto& [k, C] : cells) { Cell x = C; x.c = x.c * (1.0 / x.a); at[pack(k[0], k[1], k[2])].push_back(int(L.size())); L.push_back(x); }
    const int R = 5; const double inv = 1.0 / (2 * sg * sg);
    // (vol: the volume's share — each polygon also pushes the normal away from itself, by its area and how far it is in σ:
    // the gradient of the smoothed mass. With Smoothness high the polygons' normals cancel out — the layers of hair, the
    // two sides of an arm — and the mass's outward direction is what is left: the hair one soft volume, the arm a
    // rounded tube, never inside out. Low: almost only the normals, as before)
    const double vol = sm * sm;
    for (auto& [v, p] : P) { const auto k0 = key(p); Vec3 n; const Vec3 o = own.count(v) && own[v].lengthSq() > 0 ? own[v].normalized() : Vec3(0, 1, 0);
      for (long long i = -R; i <= R; i++) for (long long j = -R; j <= R; j++) for (long long l = -R; l <= R; l++) {
        auto x = at.find(pack(k0[0] + i, k0[1] + j, k0[2] + l)); if (x == at.end()) continue;
        for (int ci : x->second) { const Cell& C = L[size_t(ci)]; const double w = std::exp(-(C.c - p).lengthSq() * inv); n += C.n * w + (p - C.c) * (vol * C.a * w / sg); } }
      vn[v] = n.lengthSq() > 1e-18 ? n.normalized() : o; }
  }
  return vn;
}

// Smooth: the selected polygons' normals made as those of a simpler shape — the normals round each vertex averaged over a wide
// radius (the small forms — a nose, eyes — melt away, the large shape stays):
// the soft shading of a stylised face. Sphere: the normals from the selection's middle (a little behind it) outward. The
// border hard (when asked); the rest of the mesh keeps its normals
QString Viewport::tNormalsSmooth() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  auto fs = tSelFaces(m); if (fs.empty()) { ui::toast(tr("Select the polygons whose normals become smooth (a face, a head…)")); return "!"; }
  auto nb = std::make_shared<PolyMesh>(*baseOf(*it)); PolyMesh& d = *nb;
  const bool ownN = d.cornerNormal.size() == d.faceVerts.size() && !d.cornerNormal.empty();   // (the mesh came with normals of its own)
  if (!ownN) { d.cornerNormal.clear(); for (auto& n : hardCornerNormals(d)) d.cornerNormal.push_back({float(n.x), float(n.y), float(n.z)}); }
  std::map<tedit::EK, int> cnt; std::map<int, std::set<int>> adj;
  for (int f : fs) { const int s = d.faceStart[size_t(f)], e = d.faceStart[size_t(f) + 1];
    for (int k = s; k < e; k++) { const int a = d.faceVerts[size_t(k)], b = d.faceVerts[size_t(k + 1 < e ? k + 1 : s)]; cnt[ek(a, b)]++; adj[a].insert(b); adj[b].insert(a); } }
  const std::map<int, Vec3> vn = normTarget(*it, d, fs, ownN);
  for (int f : fs) for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) { const Vec3 n = vn.at(d.faceVerts[size_t(k)]); d.cornerNormal[size_t(k)] = {float(n.x), float(n.y), float(n.z)}; }
  // (inside: soft; the border: hard when asked)
  std::set<tedit::EK> H; for (auto& h : d.hardEdges) H.insert(ek(h[0], h[1]));
  for (auto& [e, c] : cnt) { if (c >= 2) H.erase(e); else if (ttools.normHardBorder) H.insert(e); }
  d.hardEdges.clear(); for (auto& e : H) d.hardEdges.push_back({e.first, e.second});
  topo::shareTopo(nb, baseSp(*it)); topoWrite(m, nb); tsel_.B = nb.get();
  if (!it->raw.value("smoothN").toBool()) { it->raw.insert("smoothN", true); staticRev_++; }   // (seen only with smooth shading)
  { // (the smoothing group kept among the selection sets, marked sg (named Normals N) — its polygons, how they were smoothed; the same
    // polygons smoothed again: that set brought up to date)
    std::vector<char> a(size_t(d.faceCount()), 0); for (int f : fs) if (f < d.faceCount()) a[size_t(f)] = 1; const QString enc = topo::smEnc(a);
    QJsonArray sets = it->raw.value("selSets").toArray(); int at = -1, k = 1;
    for (int i = 0; i < sets.size(); i++) { const QJsonObject o = sets[i].toObject(); if (!o.value("sg").toBool()) continue; if (o.value("d").toString() == enc) at = i; else k++; }
    const QString how = ttools.normMode == "sphere" ? tr("sphere") : ttools.normMode == "face" ? tr("face, nose %1%").arg(int(std::lround(ttools.normNose * 100))) : tr("smooth %1%").arg(int(std::lround(std::clamp(ttools.normSmooth, 0.0, 1.0) * 100)));
    const QString name = (at >= 0 ? sets[at].toObject().value("name").toString().section(" · ", 0, 0) : QString("Normals %1").arg(k)) + " · " + how;
    const QJsonObject o{{"k", "f"}, {"n", d.faceCount()}, {"d", enc}, {"name", name}, {"sg", true}};
    if (at >= 0) sets[at] = o; else sets.append(o); it->raw.insert("selSets", sets); }
  scene_->revision++; commit(true); emit topoState();
  tt_.lastOp = "nsmooth"; tt_.lastM = m; tt_.lastAfter = nb; tt_.lastSel = tsel_;
  const QString msg = ttools.normMode == "sphere" ? tr("Normals as a sphere's: %1 polygon%2").arg(fs.size()).arg(fs.size() == 1 ? "" : "s")
                    : ttools.normMode == "face" ? tr("Normals of a stylised face: %1 polygon%2").arg(fs.size()).arg(fs.size() == 1 ? "" : "s")
                                                  : tr("Normals of the smoothed shape: %1 polygon%2").arg(fs.size()).arg(fs.size() == 1 ? "" : "s");
  ui::toast(msg); return msg;
}

// ---------- the normals brush ----------
// the stroke's start: the mesh's normals as they are, the standard shading's (hard edges kept), the averaged ones the
// mode makes — of the selected polygons, none selected: of the whole mesh
void Viewport::nBrushStart(int m, char how) {
  nw_ = {}; Item* it = scene_->item(m); if (!it) return;
  nw_.m = m; nw_.how = how; nw_.before = baseSp(*it); const PolyMesh& B = *nw_.before;
  PolyMesh plain = B; plain.cornerNormal.clear(); for (auto& n : hardCornerNormals(plain)) nw_.def.push_back({float(n.x), float(n.y), float(n.z)});
  const bool ownN = B.cornerNormal.size() == B.faceVerts.size() && !B.cornerNormal.empty();
  nw_.cur = ownN ? B.cornerNormal : nw_.def;
  nw_.vc.assign(B.pos.size(), {}); nw_.nb.assign(B.pos.size(), {});
  for (int f = 0; f < B.faceCount(); f++) { const int s0 = B.faceStart[size_t(f)], e0 = B.faceStart[size_t(f) + 1];
    for (int k = s0; k < e0; k++) { const int a = B.faceVerts[size_t(k)], b = B.faceVerts[size_t(k + 1 < e0 ? k + 1 : s0)]; nw_.vc[size_t(a)].push_back(k); nw_.nb[size_t(a)].push_back(b); nw_.nb[size_t(b)].push_back(a); } }
  nw_.tgt.assign(B.pos.size(), Vec3()); nw_.hasT.assign(B.pos.size(), 0);
  std::vector<int> fs = tSelFaces(m);                               // (polygons selected — in any mode, Object too: the brush only on them)
  nw_.ck.assign(B.faceVerts.size(), fs.empty() ? 1 : 0); for (int f : fs) if (f < B.faceCount()) for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) nw_.ck[size_t(k)] = 1;
  if (fs.empty()) { fs.resize(size_t(B.faceCount())); std::iota(fs.begin(), fs.end(), 0); }
  if (how == 'p' && ttools.nBrushVal > 0) {
    PolyMesh d = B; d.cornerNormal = nw_.cur;
    for (auto& [v, n] : normTarget(*it, d, fs, true)) if (size_t(v) < nw_.tgt.size()) { nw_.tgt[size_t(v)] = n; nw_.hasT[size_t(v)] = 1; } }
  nw_.on = true;
}
// a dab: the vertices seen under the circle — fully in its hard middle, less toward its rim — their corners' normals
// toward: the standard shading's and the averaged one's mix by the share set (0 / Ctrl: the standard shading), Shift:
// their neighbours' normals (the change across smoothed)
void Viewport::nAlignDab(const QPointF& mp, bool) {
  const int m = nw_.m; const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || !it->poly || !nw_.on) return;
  const double R = brushR, hard = std::clamp(brushHard, 0.0, 0.95), P = std::clamp(ttools.nBrushVal, 0.0, 1.0);
  const auto vs = tAreaPick(m, mp.x() - R, mp.y() - R, mp.x() + R, mp.y() + R, 'v', [mp, R](double x, double y) { return std::hypot(x - mp.x(), y - mp.y()) <= R; });
  Projector Pj = projector(); const auto& W = it->poly->pos;
  auto V = [](const std::array<float, 3>& a) { return Vec3(a[0], a[1], a[2]); };
  auto vAvg = [&](int v) { Vec3 n; for (int k : nw_.vc[size_t(v)]) n += V(nw_.cur[size_t(k)]); return n; };
  std::vector<std::pair<int, Vec3>> smooth;                         // (Shift: from the neighbours as they were before this dab)
  for (int v : vs) { if (size_t(v) >= nw_.vc.size() || size_t(v) >= W.size()) continue; const ScreenPt sp = Pj.toScreen(W[size_t(v)]);
    const double f = std::clamp((R - std::hypot(sp.x - mp.x(), sp.y - mp.y())) / std::max(1e-9, R * (1 - hard)), 0.0, 1.0); if (f <= 0) continue;
    if (nw_.how == 's') { Vec3 a = vAvg(v); for (int u : nw_.nb[size_t(v)]) a += vAvg(u).normalized() * double(nw_.vc[size_t(v)].size());
      if (a.lengthSq() > 1e-12) smooth.push_back({v, a.normalized() * (f * 0.5)}); continue; }
    for (int k : nw_.vc[size_t(v)]) { if (!nw_.ck[size_t(k)]) continue; const Vec3 df = V(nw_.def[size_t(k)]);
      Vec3 want = df; if (nw_.how == 'p' && P > 0 && nw_.hasT[size_t(v)]) { want = df * (1 - P) + nw_.tgt[size_t(v)] * P; if (want.lengthSq() < 1e-12) want = df; }
      Vec3 n = V(nw_.cur[size_t(k)]) * (1 - f) + want.normalized() * f; if (n.lengthSq() < 1e-12) n = want; n = n.normalized();
      nw_.cur[size_t(k)] = {float(n.x), float(n.y), float(n.z)}; } }
  for (auto& [v, a] : smooth) { const double f = a.length(); const Vec3 to = a * (1 / std::max(1e-12, f));
    for (int k : nw_.vc[size_t(v)]) { if (!nw_.ck[size_t(k)]) continue; Vec3 n = V(nw_.cur[size_t(k)]) * (1 - f) + to * f; if (n.lengthSq() < 1e-12) continue; n = n.normalized(); nw_.cur[size_t(k)] = {float(n.x), float(n.y), float(n.z)}; } }
  if (vs.empty()) return;
  nw_.moved = true;
  auto nb = std::make_shared<PolyMesh>(*nw_.before); nb->cornerNormal = nw_.cur; topo::shareTopo(nb, nw_.before); topoWrite(m, nb); tsel_.B = nb.get();
  if (Item* i2 = scene_->item(m); i2 && !i2->raw.value("smoothN").toBool()) { i2->raw.insert("smoothN", true); staticRev_++; }   // (seen only with smooth shading)
  ttTouch();
  ui::hud(nw_.how == 's' ? tr("Normals: smoothed") : nw_.how == 'r' || P <= 0 ? tr("Normals: the standard shading") : tr("Normals: %1% averaged").arg(int(std::lround(P * 100))));
}
void Viewport::nAlignEnd() {
  const bool moved = nw_.moved; nw_.on = false; nw_.before.reset(); ui::hud({});
  if (!moved) return;
  tt_.lastOp.clear(); scene_->revision++; commit(true); emit topoState();   // (the stroke: one step)
}

QString Viewport::tNormalsCopy(int from) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; const Item* src = scene_->item(from); if (!it || !src || !src->poly) return "!";
  auto nb = std::make_shared<PolyMesh>(*baseOf(*it)); PolyMesh& d = *nb; const auto W = ttWorld(m);
  // (the source's normals at its vertices, in the world)
  const PolyMesh& S = *src->poly; std::vector<Vec3> sn(S.pos.size());
  for (int f = 0; f < S.faceCount(); f++) { const int s = S.faceStart[size_t(f)], e = S.faceStart[size_t(f) + 1]; Vec3 n;
    for (int k = s; k < e; k++) { const Vec3& a = S.pos[size_t(S.faceVerts[size_t(k)])]; const Vec3& b = S.pos[size_t(S.faceVerts[size_t(k + 1 < e ? k + 1 : s)])]; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
    for (int k = s; k < e; k++) sn[size_t(S.faceVerts[size_t(k)])] += n; }
  // (a grid over the source's vertices: the nearest one quickly)
  Box3 bb = S.bounds(); const double cell = std::max(1e-6, bb.size().length() / 64); std::unordered_map<long long, std::vector<int>> grid;
  auto key = [&](const Vec3& p) { const long long x = (long long)std::floor((p.x - bb.min.x) / cell), y = (long long)std::floor((p.y - bb.min.y) / cell), z = (long long)std::floor((p.z - bb.min.z) / cell); return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
  for (size_t v = 0; v < S.pos.size(); v++) grid[key(S.pos[v])].push_back(int(v));
  auto nearest = [&](const Vec3& p) { int best = -1; double bd = 1e300;
    for (int r = 0; r < 6 && best < 0; r++) for (int ix = -r; ix <= r; ix++) for (int iy = -r; iy <= r; iy++) for (int iz = -r; iz <= r; iz++) { const Vec3 q = p + Vec3(ix, iy, iz) * cell; auto g = grid.find(key(q)); if (g == grid.end()) continue;
      for (int v : g->second) { const double dd = S.pos[size_t(v)].distanceTo(p); if (dd < bd) { bd = dd; best = v; } } }
    return best; };
  if (d.cornerNormal.size() != d.faceVerts.size()) { d.cornerNormal.clear(); for (auto& n : hardCornerNormals(d)) d.cornerNormal.push_back({float(n.x), float(n.y), float(n.z)}); }
  const Affine Mi = meshmods::meshMatrix(*scene_, *it).inverse(); auto fs = tSelFaces(m); std::set<int> S2(fs.begin(), fs.end()); int done = 0;
  for (int f = 0; f < d.faceCount(); f++) { if (!S2.empty() && !S2.count(f)) continue;
    for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) { const int v = d.faceVerts[size_t(k)]; const int q = nearest(W[size_t(v)]); if (q < 0) continue;
      Vec3 n = Mi.lin(sn[size_t(q)]); if (n.lengthSq() <= 0) continue; n = n.normalized(); d.cornerNormal[size_t(k)] = {float(n.x), float(n.y), float(n.z)}; done++; } }
  topo::shareTopo(nb, baseSp(*it)); topoWrite(m, nb); tsel_.B = nb.get();
  scene_->revision++; commit(true); emit topoState(); const QString msg = tr("Normals copied from %1: %2 corners").arg(src->name).arg(done); ui::toast(msg); return msg;
}

// ---------- cleaning ----------
QString Viewport::tRelaxMesh() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || topoHeavy(m)) return "!";
  const PolyMesh& B = *baseOf(*it); auto T = topo::ttopo(baseSp(*it)); std::vector<int> vs = topoMode == "object" ? std::vector<int>{} : tSelVerts(m);
  if (vs.empty()) { vs.resize(B.pos.size()); std::iota(vs.begin(), vs.end(), 0); }
  topo::Bvh surf; std::vector<uint32_t> tris; for (int f = 0; f < B.faceCount(); f++) { const int s = B.faceStart[size_t(f)]; for (int k = s + 1; k + 1 < B.faceStart[size_t(f) + 1]; k++) { tris.push_back(uint32_t(B.faceVerts[size_t(s)])); tris.push_back(uint32_t(B.faceVerts[size_t(k)])); tris.push_back(uint32_t(B.faceVerts[size_t(k + 1)])); } }
  surf.build(B.pos, tris);
  std::vector<char> rim(B.pos.size(), 0); for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] < 0) rim[size_t(T->ea[size_t(e)])] = rim[size_t(T->eb[size_t(e)])] = 1;
  // (the border picked — its edges / vertices: smoothed along itself, the chain's ends staying; picked polygons: their
  // border stays)
  std::vector<std::vector<int>> rimNb(B.pos.size()); for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] < 0) { rimNb[size_t(T->ea[size_t(e)])].push_back(T->eb[size_t(e)]); rimNb[size_t(T->eb[size_t(e)])].push_back(T->ea[size_t(e)]); }
  std::vector<char> rimMove(B.pos.size(), 0);
  if (topoMode == "edge" || topoMode == "vert") { std::set<int> s(vs.begin(), vs.end());
    for (int v : vs) if (rim[size_t(v)] && rimNb[size_t(v)].size() == 2 && s.count(rimNb[size_t(v)][0]) && s.count(rimNb[size_t(v)][1])) rimMove[size_t(v)] = 1; }
  const auto A = adjOf(B); std::vector<Vec3> P = B.pos;
  for (int it2 = 0; it2 < 12; it2++) { std::vector<Vec3> Q = P;
    for (int v : vs) if (rimMove[size_t(v)]) { const Vec3 c = (P[size_t(rimNb[size_t(v)][0])] + P[size_t(rimNb[size_t(v)][1])]) * 0.5; Vec3 p = P[size_t(v)] + (c - P[size_t(v)]) * 0.5; double d; if (auto q = surf.closest(p, nullptr, &d)) p = *q; Q[size_t(v)] = p; }
    for (int v : vs) { if (rim[size_t(v)] || A[size_t(v)].empty()) continue; Vec3 c; for (int w : A[size_t(v)]) c += P[size_t(w)]; c = c * (1.0 / double(A[size_t(v)].size()));
      Vec3 p = P[size_t(v)] + (c - P[size_t(v)]) * 0.5; double d; if (auto q = surf.closest(p, nullptr, &d)) p = *q; Q[size_t(v)] = p; }
    P = Q; }
  topoLive(m, P); topoWrite(m, baseSp(*it)); scene_->revision++; commit(false); emit topoState();
  const QString msg = tr("Relaxed: %1 vert%2").arg(vs.size()).arg(vs.size() == 1 ? "ex" : "ices"); ui::toast(msg); return msg;
}

QString Viewport::tCollapseEdges() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || tsel_.id != m || topoMode == "object") { ui::toast(tr("Select edges to collapse")); return "!"; }
  const PolyMesh& B = *baseOf(*it); auto T = topo::ttopo(baseSp(*it)); std::vector<std::pair<int, int>> es;
  if (tKey() == 'e') for (int e : tsel_.e) es.push_back({T->ea[size_t(e)], T->eb[size_t(e)]});
  else if (tKey() == 'f') {                                      // (polygons: each collapsed across the strip — its two sides that join the others picked; alone: the shorter pair — a polygon loop becomes a line)
    std::set<int> S(tsel_.f.begin(), tsel_.f.end());
    for (int f : S) { const int s = T->fo[size_t(f)], n = T->fo[size_t(f) + 1] - s; if (n != 4) continue;
      int sh[4]; for (int k = 0; k < 4; k++) { const int e = T->ce[size_t(s + k)]; const int g = T->ef0[size_t(e)] == f ? T->ef1[size_t(e)] : T->ef0[size_t(e)]; sh[k] = g >= 0 && S.count(g) ? 1 : 0; }
      int pick;                                                     // (0: sides 0 and 2 collapsed — the rungs shared with the strip's neighbours; 1: sides 1 and 3)
      if (sh[0] + sh[2] != sh[1] + sh[3]) pick = sh[0] + sh[2] > sh[1] + sh[3] ? 0 : 1;
      else { auto len = [&](int k) { return B.pos[size_t(T->fv[size_t(s + k)])].distanceTo(B.pos[size_t(T->fv[size_t(s + (k + 1) % 4)])]); }; pick = len(0) + len(2) <= len(1) + len(3) ? 0 : 1; }
      for (int k : {pick, pick + 2}) es.push_back({T->fv[size_t(s + k)], T->fv[size_t(s + (k + 1) % 4)]}); }
    if (es.empty()) { auto vs = tSelVerts(m); std::set<int> s2(vs.begin(), vs.end()); for (int e = 0; e < T->ne; e++) if (s2.count(T->ea[size_t(e)]) && s2.count(T->eb[size_t(e)])) es.push_back({T->ea[size_t(e)], T->eb[size_t(e)]}); }
  } else { auto vs = tSelVerts(m); std::set<int> s(vs.begin(), vs.end()); for (int e = 0; e < T->ne; e++) if (s.count(T->ea[size_t(e)]) && s.count(T->eb[size_t(e)])) es.push_back({T->ea[size_t(e)], T->eb[size_t(e)]}); }
  if (es.empty()) { ui::toast(tr("Select edges to collapse")); return "!"; }
  std::vector<int> par(B.pos.size()); std::iota(par.begin(), par.end(), 0);
  std::function<int(int)> find = [&](int x) { while (par[size_t(x)] != x) x = par[size_t(x)] = par[size_t(par[size_t(x)])]; return x; };
  for (auto& [a, b] : es) { const int x = find(a), y = find(b); if (x != y) par[size_t(std::max(x, y))] = std::min(x, y); }
  MeshEd E = MeshEd::from(B); std::map<int, std::vector<int>> grp; for (auto& [a, b] : es) { grp[find(a)].push_back(a); grp[find(a)].push_back(b); }
  std::vector<int> to(E.pos.size(), -1), keep;
  for (auto& [r, vs0] : grp) { std::set<int> vs(vs0.begin(), vs0.end()); Vec3 c; for (int v : vs) c += E.pos[size_t(v)]; E.pos[size_t(r)] = c * (1.0 / double(vs.size())); for (int v : vs) if (v != r) to[size_t(v)] = r; keep.push_back(r); }
  E.weld(to);
  return ttDone(m, E, B, tr("Collapsed: %1 edge%2 → %3 vert%4").arg(es.size()).arg(es.size() == 1 ? "" : "s").arg(grp.size()).arg(grp.size() == 1 ? "ex" : "ices"), tKey() == 'v' ? 'v' : 0, keep);   // (the mode stays: from edges or polygons — no switch to vertices)
}

QString Viewport::tSnapToGeom() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  std::vector<std::shared_ptr<const topo::Bvh>> tg;
  for (auto& [id, t] : scene_->items) if (id != m && t.type == ItemType::IMesh && t.poly && t.raw.value("tref").toBool() && scene_->isVisible(t)) if (auto b = bvhOf(id, true)) tg.push_back(b);
  const bool refs = !tg.empty();
  if (!refs) for (auto& [id, t] : scene_->items) if (id != m && t.type == ItemType::IMesh && t.poly && scene_->isVisible(t)) if (auto b = bvhOf(id, true)) tg.push_back(b);
  if (tg.empty()) { ui::toast(tr("No reference geometry to snap to — mark a mesh as a reference (its settings)")); return "!"; }
  std::vector<int> vs = topoMode == "object" ? std::vector<int>{} : tSelVerts(m); const PolyMesh& B = *baseOf(*it);
  if (vs.empty()) { vs.resize(B.pos.size()); std::iota(vs.begin(), vs.end(), 0); }
  const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse(); std::vector<Vec3> P = B.pos;
  // (a vertex in sight — on the screen, the references' first surface along its ray turned to the camera and not well in
  // front of it (within about an edge): onto that surface, as the view sees it; any other — the back, hidden by the
  // reference — onto its nearest point, so a whole wrap never folds flat into the camera's view)
  Projector Pr = projector(); const double edge = B.bounds().size().length() / std::sqrt(double(std::max(1, B.faceCount())));
  // (the vertices' normals: one out of sight goes along its own normal, both ways, onto the nearest surface it meets
  // near it — the nearest point anywhere may be another part of the references, a fold, the inside of a cuff)
  std::vector<Vec3> vn(B.pos.size());
  for (int f = 0; f < B.faceCount(); f++) { const int s0 = B.faceStart[size_t(f)], e0 = B.faceStart[size_t(f) + 1]; Vec3 n;
    for (int k = s0; k < e0; k++) { const Vec3& a = B.pos[size_t(B.faceVerts[size_t(k)])]; const Vec3& b2 = B.pos[size_t(B.faceVerts[size_t(k + 1 < e0 ? k + 1 : s0)])]; n += a.cross(b2); }
    for (int k = s0; k < e0; k++) vn[size_t(B.faceVerts[size_t(k)])] += n; }
  std::vector<double> le(B.pos.size(), 1e300);   // (each vertex's shortest edge: how far in front of it the view's hit may be)
  for (int f = 0; f < B.faceCount(); f++) { const int s0 = B.faceStart[size_t(f)], e0 = B.faceStart[size_t(f) + 1];
    for (int k = s0; k < e0; k++) { const int a = B.faceVerts[size_t(k)], b2 = B.faceVerts[size_t(k + 1 < e0 ? k + 1 : s0)]; const double l = M.lin(B.pos[size_t(a)] - B.pos[size_t(b2)]).length(); le[size_t(a)] = std::min(le[size_t(a)], l); le[size_t(b2)] = std::min(le[size_t(b2)], l); } }
  int seen = 0;
  for (int v : vs) { const Vec3 w = M.apply(P[size_t(v)]); double bd = 1e300; Vec3 best = w; for (auto& b : tg) { double d; if (auto q = b->closest(w, nullptr, &d); q && d < bd) { bd = d; best = *q; } }
    if (vn[size_t(v)].lengthSq() > 0) { const Vec3 n = M.lin(vn[size_t(v)]).normalized(); double bn = std::max(edge * 4, bd * 3); std::optional<Vec3> hit;
      for (auto& b : tg) for (double sgn : {1.0, -1.0}) if (auto h = b->ray(w, n * sgn, bn); h && h->first < bn) { bn = h->first; hit = w + n * sgn * h->first; }
      if (hit) best = *hit; }
    const ScreenPt s = Pr.toScreen(w);
    if (s.z < 1 && s.x >= 0 && s.y >= 0 && s.x < width() && s.y < height()) {
      const Vec3 d = cam_.ortho() ? cam_.dir() : (w - cam_.pos).normalized(); const Vec3 o = cam_.ortho() ? w - d * 1e5 : cam_.pos; const double tv = (w - o).dot(d);
      double bt = 1e300; bool front = false;
      for (auto& b : tg) if (auto h = b->ray(o, d, 1e12); h && h->first < bt) { const size_t t = size_t(h->second); const Vec3 &a = b->P[b->idx[t * 3]], &bb = b->P[b->idx[t * 3 + 1]], &c = b->P[b->idx[t * 3 + 2]]; bt = h->first; front = (bb - a).cross(c - a).dot(d) < 0; }
      // (the view's hit only when about as near as the nearest surface: one well in front of the vertex — a brim, a ring
      // hanging over it — is not what it lies on)
      const Vec3 hv = o + d * bt; const double lim = std::max(3 * best.distanceTo(w), 0.3 * std::min(edge, le[size_t(v)]));
      if (front && bt > tv - std::max(edge, bd * 2) && hv.distanceTo(w) <= lim) { best = hv; seen++; } }
    P[size_t(v)] = Mi.apply(best); }
  topoLive(m, P); topoWrite(m, baseSp(*it)); scene_->revision++; commit(false); emit topoState();
  const QString msg = tr("Snapped: %1 vert%2 onto %3 (%4 in sight — onto what the view sees)").arg(vs.size()).arg(vs.size() == 1 ? "ex" : "ices").arg(refs ? tr("the references") : tr("the other meshes")).arg(seen); ui::toast(msg); return msg;
}

// Optimize (as Cinema 4D's): the points within the weld distance of each other welded into one, the unused points (in no
// polygon) taken out, the polygons of one or two points left taken out, the doubled polygons made one; a vertex in the
// middle of a side (two edges, two polygons) taken out too when asked — in the selection, or the whole mesh
QString Viewport::tOptimize() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || topoHeavy(m)) return "!";
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B);
  std::vector<int> vs = topoMode == "object" ? std::vector<int>{} : tSelVerts(m); std::vector<char> in(E.pos.size(), vs.empty() ? 1 : 0); for (int v : vs) in[size_t(v)] = 1;
  // (welded: a grid of cells the weld distance wide)
  const double r = std::max(1e-9, ttools.optWeld); std::unordered_map<long long, std::vector<int>> grid; int welded = 0;
  auto cellOf = [&](const Vec3& p, int dx, int dy, int dz) { const long long x = (long long)std::floor(p.x / r) + dx, y = (long long)std::floor(p.y / r) + dy, z = (long long)std::floor(p.z / r) + dz; return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
  std::vector<int> to(E.pos.size(), -1);
  std::set<int> used; for (auto& F : E.f) used.insert(F.v.begin(), F.v.end());   // (every vertex in the grid — a picked one welds to an unpicked one too)
  int unused = 0; for (size_t v = 0; v < E.pos.size(); v++) if (!used.count(int(v))) unused++;   // (in no polygon: dropped as the mesh is written)
  const bool merge = ttools.optMode == "merge";                 // (Merge: every weld — the inner walls it makes taken out after)
  // (which points may be welded: two on the mesh's open rims — a seam left open, a cap to its tube — or two of one
  // polygon; two points inside surfaces that only touch — two strands of hair lying on each other — never: that
  // would sew them together, edges of three polygons and more)
  std::vector<char> rimV(E.pos.size(), 0); std::map<std::pair<int, int>, int> ec0; std::vector<std::vector<int>> vf(E.pos.size());
  for (size_t fi = 0; fi < E.f.size(); fi++) { const auto& V = E.f[fi].v; for (size_t k = 0; k < V.size(); k++) { const int a = V[k], b = V[(k + 1) % V.size()]; ec0[{std::min(a, b), std::max(a, b)}]++; vf[size_t(a)].push_back(int(fi)); } }
  for (auto& [e, c] : ec0) if (c == 1) { rimV[size_t(e.first)] = 1; rimV[size_t(e.second)] = 1; }
  auto mayWeld = [&](int a, int b) { if (rimV[size_t(a)] && rimV[size_t(b)]) return true; for (int f : vf[size_t(a)]) if (std::find(vf[size_t(b)].begin(), vf[size_t(b)].end(), f) != vf[size_t(b)].end()) return true; return false; };
  for (size_t v = 0; v < E.pos.size(); v++) { if (!used.count(int(v))) continue; int hit = -1;
    for (int dx = -1; dx <= 1 && hit < 0; dx++) for (int dy = -1; dy <= 1 && hit < 0; dy++) for (int dz = -1; dz <= 1 && hit < 0; dz++) { auto g = grid.find(cellOf(E.pos[v], dx, dy, dz)); if (g == grid.end()) continue; for (int w : g->second) if (E.pos[size_t(w)].distanceTo(E.pos[v]) <= r && (merge || mayWeld(int(v), w))) { hit = w; break; } }
    if (hit >= 0 && (in[v] || in[size_t(hit)])) { to[v] = hit; welded++; } else grid[cellOf(E.pos[v], 0, 0, 0)].push_back(int(v)); }
  // (a weld that would still make an edge of three polygons or more, not one before: undone — the clean-up never adds
  // non-manifold edges)
  // (Merge: the inner walls the weld makes taken out first — two faces on the same corners turned against each other —
  // only what is left after that counts)
  auto dropWalls = [](MeshEd& X) { std::map<std::vector<int>, std::vector<int>> g; for (size_t f = 0; f < X.f.size(); f++) g[tedit::cycleKey(X.f[f].v)].push_back(int(f));
    std::vector<char> del(X.f.size(), 0); bool any = false;
    for (auto& [key, fs] : g) { if (fs.size() < 2) continue; int fw = 0; for (int f : fs) { const auto& V = X.f[size_t(f)].v; const size_t s = size_t(std::find(V.begin(), V.end(), key[0]) - V.begin()); fw += V[(s + 1) % V.size()] == key[1]; }
      if (fw > 0 && fw < int(fs.size())) { for (int f : fs) del[size_t(f)] = 1; any = true; } }
    if (any) X.removeFaces(del); };
  for (int pass = 0; pass < 6 && welded; pass++) {
    MeshEd T2 = E; T2.weld(to); if (merge) dropWalls(T2); std::map<std::pair<int, int>, int> ec;
    for (auto& F : T2.f) for (size_t k = 0; k < F.v.size(); k++) { const int a = F.v[k], b = F.v[(k + 1) % F.v.size()]; ec[{std::min(a, b), std::max(a, b)}]++; }
    std::set<int> bad; for (auto& [e, c] : ec) if (c > 2) { auto o = ec0.find(e); if (o != ec0.end() && o->second > 2) continue; bad.insert(e.first); bad.insert(e.second); }
    if (bad.empty()) break;
    for (size_t v = 0; v < to.size(); v++) if (to[v] >= 0 && (bad.count(to[v]) || bad.count(int(v)))) { to[v] = -1; welded--; } }
  if (welded) E.weld(to);
  // (the polygons of one or two points: a corner repeated side by side made one, then fewer than three left — out)
  int shortF = 0;
  { std::vector<char> del(E.f.size(), 0);
    for (size_t fi = 0; fi < E.f.size(); fi++) { auto& F = E.f[fi]; std::vector<int> keep; for (size_t k = 0; k < F.v.size(); k++) if (F.v[k] != F.v[(k + F.v.size() - 1) % F.v.size()]) keep.push_back(int(k));
      std::set<int> u(F.v.begin(), F.v.end()); if (u.size() < 3 || keep.size() < 3) { del[fi] = 1; shortF++; continue; }
      if (keep.size() < F.v.size()) { MeshEd::F N = F; N.v.clear(); N.uv.clear(); N.n.clear(); for (int k : keep) { N.v.push_back(F.v[size_t(k)]); if (k < int(F.uv.size())) N.uv.push_back(F.uv[size_t(k)]); if (k < int(F.n.size())) N.n.push_back(F.n[size_t(k)]); } F = N; } }
    if (shortF) E.removeFaces(del); }
  // (the vertices in the middle of a side: two edges, two polygons — taken out of both)
  std::map<int, std::set<int>> adj; std::map<int, int> nf;
  for (auto& F : E.f) { const size_t n = F.v.size(); for (size_t k = 0; k < n; k++) { adj[F.v[k]].insert(F.v[(k + 1) % n]); adj[F.v[(k + 1) % n]].insert(F.v[k]); nf[F.v[k]]++; } }
  int gone = 0; if (ttools.optMidSide)
  for (auto& [v, nb] : adj) {
    if (nb.size() != 2 || size_t(v) >= in.size() || !in[size_t(v)]) continue;
    if (nf[v] == 1) { const Vec3 a = (E.pos[size_t(*nb.begin())] - E.pos[size_t(v)]).normalized(), b = (E.pos[size_t(*nb.rbegin())] - E.pos[size_t(v)]).normalized(); if (a.dot(b) > -0.985) continue; }   // (on the border: only where it goes straight on — not a corner)
    else if (nf[v] != 2) continue;
    bool ok = true; for (auto& F : E.f) if (E.corner(int(&F - &E.f[0]), v) >= 0 && F.v.size() <= 3) ok = false;
    if (!ok) continue;
    for (auto& F : E.f) { auto at = std::find(F.v.begin(), F.v.end(), v); if (at == F.v.end()) continue; const long k = at - F.v.begin(); F.v.erase(at); if (E.hasUV) F.uv.erase(F.uv.begin() + k); if (size_t(k) < F.n.size()) F.n.erase(F.n.begin() + k); }
    gone++;
  }
  // (doubled polygons — the same corners twice: turned the same way, a copy — one kept, turned as its neighbours; in
  // Merge, turned against each other, an inner wall the weld made — two parts back to back: both taken out, the parts
  // one shell)
  int doubled = 0, inner = 0;
  { std::map<std::vector<int>, std::vector<int>> groups; for (size_t f = 0; f < E.f.size(); f++) groups[tedit::cycleKey(E.f[f].v)].push_back(int(f));
    auto forward = [&](int f, const std::vector<int>& key) { const auto& V = E.f[size_t(f)].v; const size_t n = V.size(); const size_t s = size_t(std::find(V.begin(), V.end(), key[0]) - V.begin()); return n < 2 || V[(s + 1) % n] == key[1]; };
    std::vector<char> del(E.f.size(), 0); std::vector<int> kept;
    for (auto& [key, fs] : groups) { if (fs.size() < 2) continue;
      int fw = 0; for (int f : fs) fw += forward(f, key); const bool backToBack = fw > 0 && fw < int(fs.size());
      if (merge && backToBack) { for (int f : fs) del[size_t(f)] = 1; inner += int(fs.size()); continue; }
      for (size_t i = 1; i < fs.size(); i++) { del[size_t(fs[i])] = 1; doubled++; } kept.push_back(fs[0]); }
    if (doubled || inner) { std::vector<int> shift(del.size(), 0); int s = 0; for (size_t i = 0; i < del.size(); i++) { shift[i] = s; s += del[i]; }
      E.removeFaces(del); for (int f : kept) E.orient(f - shift[size_t(f)]); } }
  if (!welded && !gone && !doubled && !inner && !unused && !shortF) { ui::toast(tr("Nothing to optimize — no points in one place (within %1 mm), no unused points, no polygons of one or two points, no doubled polygons").arg(ttools.optWeld * 10, 0, 'g', 3)); return "!"; }
  QStringList did; did << tr("%1 points welded").arg(welded); if (unused) did << tr("%1 unused points out").arg(unused); if (shortF) did << tr("%1 polygons of one or two points out").arg(shortF);
  if (doubled) did << tr("%1 doubled polygon%2 out").arg(doubled).arg(doubled == 1 ? "" : "s"); if (inner) did << tr("%1 inner wall polygons out").arg(inner); if (gone) did << tr("%1 taken out of the sides").arg(gone);
  return ttDone(m, E, B, tr("Optimized: ") + did.join(", "));
}

// ---------- symmetry ----------
namespace {
// the copies' transforms (the original not among them): by type, round axis a through the origin
std::vector<Affine> symCopies(const QString& type, int a, int count) {
  std::vector<Affine> out; const int N = std::max(2, count);
  auto rot = [&](double ang) { Affine R; const double c = std::cos(ang), s = std::sin(ang); const int i = (a + 1) % 3, j = (a + 2) % 3;
    for (int x = 0; x < 3; x++) for (int y = 0; y < 3; y++) R.L[x][y] = x == y ? 1 : 0;
    R.L[i][i] = c; R.L[i][j] = -s; R.L[j][i] = s; R.L[j][j] = c; return R; };
  auto refl = [&](int axis) { Affine F; F.L[axis][axis] = -1; return F; };
  if (type == "mirror") { out.push_back(refl(a)); return out; }
  if (type == "radial") { for (int k = 1; k < N; k++) out.push_back(rot(2 * M_PI * k / N)); return out; }
  if (type == "radialHalf") { for (int k = 1; k < N; k++) out.push_back(rot(M_PI * k / N)); return out; }
  // (radial mirror — a kaleidoscope: each turn, and each turn of the mirrored one)
  const Affine F = refl((a + 1) % 3);
  for (int k = 0; k < N; k++) { if (k) out.push_back(rot(2 * M_PI * k / N)); out.push_back(Affine::mul(rot(2 * M_PI * k / N), F)); }
  return out;
}
// the copies added to E (world → local by M), the seam welded: a copy's vertex onto an earlier one within eps
int addCopies(MeshEd& E, const std::vector<int>& faces, const std::vector<Affine>& Ts, const Affine& M, double eps, bool normals = true, std::vector<std::map<int, int>>* vmaps = nullptr, std::vector<std::pair<int, int>>* faceOf = nullptr) {
  const Affine Mi = M.inverse(); std::set<int> vs; for (int f : faces) for (int v : E.f[size_t(f)].v) vs.insert(v);
  std::unordered_map<long long, std::vector<int>> grid; const double r = std::max(eps, 1e-12);
  auto cell = [&](const Vec3& p, int dx, int dy, int dz) { const long long x = (long long)std::floor(p.x / r) + dx, y = (long long)std::floor(p.y / r) + dy, z = (long long)std::floor(p.z / r) + dz; return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
  for (int v : vs) grid[cell(M.apply(E.pos[size_t(v)]), 0, 0, 0)].push_back(v);
  int made = 0;
  for (const Affine& T : Ts) {
    const bool flip = T.det() < 0; std::map<int, int> map;
    for (int v : vs) { const Vec3 w = T.apply(M.apply(E.pos[size_t(v)])); int hit = -1;
      for (int dx = -1; dx <= 1 && hit < 0; dx++) for (int dy = -1; dy <= 1 && hit < 0; dy++) for (int dz = -1; dz <= 1 && hit < 0; dz++) { auto g = grid.find(cell(w, dx, dy, dz)); if (g == grid.end()) continue;
        for (int q : g->second) if (M.apply(E.pos[size_t(q)]).distanceTo(w) <= eps) { hit = q; break; } }
      if (hit < 0) { hit = E.addVert(Mi.apply(w)); grid[cell(w, 0, 0, 0)].push_back(hit); }
      map[v] = hit; }
    const Affine TL = Affine::mul(Mi, Affine::mul(T, M));          // (local → the copy's local: the corners' own normals turned with it)
    for (int f : faces) { MeshEd::F F = E.f[size_t(f)]; for (int& v : F.v) v = map[v]; for (auto& q : F.n) if (!normals) q = {0, 0, 0}; else if (q[0] != 0 || q[1] != 0 || q[2] != 0) { const Vec3 n = TL.lin(Vec3(q[0], q[1], q[2])).normalized(); q = {float(n.x), float(n.y), float(n.z)}; }
      if (flip) { std::reverse(F.v.begin(), F.v.end()); std::reverse(F.uv.begin(), F.uv.end()); std::reverse(F.n.begin(), F.n.end()); }
      std::set<int> chk(F.v.begin(), F.v.end()); if (chk.size() < 3) continue; E.f.push_back(F); made++; if (faceOf) faceOf->push_back({f, int(E.f.size()) - 1}); }
    // (the hard edges, seams and creases of the copied part — on the copy too)
    for (auto* S : {&E.hard, &E.seam}) { const auto was = *S; for (const auto& e : was) { auto a = map.find(e.first), b = map.find(e.second); if (a != map.end() && b != map.end() && a->second != b->second) S->insert(tedit::ek(a->second, b->second)); } }
    { const auto was = E.crease; for (auto& [e, w] : was) { auto a = map.find(e.first), b = map.find(e.second); if (a != map.end() && b != map.end() && a->second != b->second) E.crease[tedit::ek(a->second, b->second)] = w; } }
    if (vmaps) vmaps->push_back(map);
  }
  return made;
}
}  // namespace

Affine Viewport::mirFrame(int m) const {
  Affine G; if (!ttools.mirLocal || !scene_->item(m)) return G;
  const Quat Q = gzFrameLocal(); const Vec3 X = Q.rotate({1, 0, 0}), Y = Q.rotate({0, 1, 0}), Z = Q.rotate({0, 0, 1});
  for (int i = 0; i < 3; i++) { G.L[i][0] = (&X.x)[i]; G.L[i][1] = (&Y.x)[i]; G.L[i][2] = (&Z.x)[i]; }
  G.t = meshmods::meshMatrix(*scene_, *scene_->item(m)).apply(meshPivotLocal(m)); return G;
}

QString Viewport::tMirrorMesh() {
  if (ttools.mirType == "mirror" && ttools.mirReplace) return tMirrorReplace();   // (the copy in place of the other side's region)
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || topoHeavy(m)) { ui::toast(tr("Select a mesh")); return "!"; }
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); auto fs = topoMode == "object" ? std::vector<int>{} : tSelFaces(m);
  if (fs.empty()) { fs.resize(E.f.size()); std::iota(fs.begin(), fs.end(), 0); }
  const Affine M = meshmods::meshMatrix(*scene_, *it); const int a = std::clamp(ttools.mirAxis, 0, 2);
  // (the side it is on: the copies go to the other — the plane through the origin)
  Vec3 c; int cn = 0; for (int f : fs) for (int v : E.f[size_t(f)].v) { c += M.apply(E.pos[size_t(v)]); cn++; } if (cn) c = c * (1.0 / cn);
  const double eps = std::max(1e-6, B.bounds().size().length() * 1e-4);
  auto Ts = symCopies(ttools.mirType, a, ttools.mirCount);
  const Affine G = mirFrame(m), Gi = G.inverse(); for (auto& T : Ts) T = Affine::mul(G, Affine::mul(T, Gi)); c = Gi.apply(c);   // (Local: about the object's gizmo)
  std::vector<std::map<int, int>> vmaps; std::vector<std::pair<int, int>> faceOf;
  const int made = addCopies(E, fs, Ts, M, eps, ttools.mirNormals, &vmaps, &faceOf);
  if (!made) { ui::toast(tr("Nothing mirrored")); return "!"; }
  // (the selection sets: kept, each with its mirrored part added — counted on the new geometry)
  if (const QJsonArray sets = it->raw.value("selSets").toArray(); !sets.isEmpty()) {
    auto T0 = topo::ttopo(baseSp(*it)); std::vector<int> vm; auto nb = std::make_shared<PolyMesh>(E.to(B, &vm)); auto T1 = topo::ttopo(nb);
    auto V = [&](int v) { return size_t(v) < vm.size() ? vm[size_t(v)] : -1; };
    QJsonArray out;
    for (const QJsonValue& sv : sets) { QJsonObject s = sv.toObject(); const QString k = s.value("k").toString(); const char kc = k.isEmpty() ? 'f' : k[0].toLatin1();
      const int N0 = kc == 'f' ? T0->nf : kc == 'e' ? T0->ne : T0->n, N1 = kc == 'f' ? T1->nf : kc == 'e' ? T1->ne : T1->n;
      if (s.value("n").toInt() != N0) { out.append(s); continue; }   // (made for other geometry already: left as it is)
      const auto in = topo::smDec(s.value("d").toString(), N0); std::vector<char> a1(size_t(N1), 0);
      auto setV = [&](int v) { if (v >= 0 && v < N1) a1[size_t(v)] = 1; };
      auto setE = [&](int x, int y) { if (x >= 0 && y >= 0) if (int e = T1->edgeOf(x, y); e >= 0) a1[size_t(e)] = 1; };
      for (int i = 0; i < N0; i++) { if (!in[size_t(i)]) continue;
        if (kc == 'f') { setV(i); for (auto& [src, nf] : faceOf) if (src == i) setV(nf); }
        else if (kc == 'v') { setV(V(i)); for (auto& mp : vmaps) if (auto q = mp.find(i); q != mp.end()) setV(V(q->second)); }
        else { const int ea = T0->ea[size_t(i)], eb = T0->eb[size_t(i)]; setE(V(ea), V(eb));
          for (auto& mp : vmaps) { auto qa = mp.find(ea), qb = mp.find(eb); if (qa != mp.end() && qb != mp.end()) setE(V(qa->second), V(qb->second)); } } }
      s.insert("n", N1); s.insert("d", topo::smEnc(a1)); out.append(s); }
    it->raw.insert("selSets", out);
  }
  const double side = a == 0 ? c.x : a == 1 ? c.y : c.z;
  const QString what = ttools.mirType == "mirror" ? tr("mirrored across %1%3 = 0 (it was on the %2 side)").arg(QString("XYZ")[a]).arg(side >= 0 ? "+" : "−").arg(ttools.mirLocal ? tr(" (local)") : QString())
                                                  : tr("%1 copies round %2").arg(Ts.size()).arg(QString("XYZ")[a]);
  return ttDone(m, E, B, tr("Mirror: %1 — %2 polygons, the seam welded").arg(what).arg(made));
}

int Viewport::vsymGroupOf(int item) const {
  for (const Item* x = scene_->item(item); x; x = x->parent >= 0 ? scene_->item(x->parent) : nullptr) if (x->type == ItemType::Group && x->raw.value("vsym").isObject()) return x->id;
  return -1;
}

// the selected meshes into a new group whose symmetry is only shown (in any place) — Apply makes it real
QString Viewport::vsymCreate(const QString& type) {
  std::vector<int> ms; for (int id : sel_.items) if (const Item* x = scene_->item(id); x && x->type == ItemType::IMesh) ms.push_back(id);
  if (ms.empty()) { ui::toast(tr("Select the meshes for the group with a virtual symmetry")); return "!"; }
  QString msg;
  runEdit([&](Scene& s, Selection& sel, const CurveCache&) -> QString {
    const int parent = s.item(s.item(ms[0])->parent) ? s.item(ms[0])->parent : meshesGroup(s);   // (at the root: inside Meshes)
    Item& g = s.addItem(ItemType::Group, parent, tr("Symmetry group")); g.expanded = true;
    g.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "hide"}, {"conv", QJsonValue()}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false},
                        {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", "scene"}, {"spos", QJsonValue()},
                        {"vsym", QJsonObject{{"type", type}, {"axis", ttools.mirAxis}, {"count", ttools.mirCount}, {"axes", QJsonArray{QString("xyz")[std::clamp(ttools.mirAxis, 0, 2)] + QString("+")}}, {"weld", true}, {"weldDist", 0.05}}}};
    const int gid = g.id;
    for (int id : ms) { s.removeFromParent(id); s.item(id)->parent = gid; s.item(gid)->children.push_back(id); }
    sel.items = {gid}; msg = tr("A group with a virtual symmetry (%1): only shown — Apply in its menu makes it real").arg(type); return msg;
  });
  return msg;
}

QString Viewport::vsymApply(int group) {
  Item* g = scene_->item(group); if (!g || !g->raw.value("vsym").isObject()) { ui::toast(tr("Select a group with a virtual symmetry")); return "!"; }
  const auto Ts = meshmods::vsymWorld(*scene_, *g);                // (about the group's frame: its own point / axes, or its object's gizmo)
  std::vector<int> ms; std::function<void(int)> walk = [&](int id) { const Item* x = scene_->item(id); if (!x) return; if (x->type == ItemType::IMesh) ms.push_back(id); for (int c : x->children) walk(c); }; walk(group);
  int made = 0;
  const QJsonObject vs = g->raw.value("vsym").toObject(); const bool sides = !meshmods::vsymSides(vs).empty();
  const Affine G = meshmods::vsymFrame(*scene_, *g), Gi = G.inverse(); const double wd = vs.value("weld").toBool(true) ? vs.value("weldDist").toDouble(0.05) : 0.0;
  for (int m : ms) { Item* it = scene_->item(m); if (!it || !it->base) continue; const PolyMesh& B = *it->base; MeshEd E = MeshEd::from(B); const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse();
    if (sides) {                                                // (Mirror by sides: the seam welded onto the planes, the other sides' polygons out)
      for (auto& p : E.pos) p = Mi.apply(G.apply(meshmods::vsymWeld(vs, Gi.apply(M.apply(p)))));
      std::vector<char> del(E.f.size(), 0); bool any = false;
      for (size_t f = 0; f < E.f.size(); f++) if (!meshmods::vsymKeep(vs, Gi.apply(M.apply(E.center(int(f)))))) { del[f] = 1; any = true; }
      if (any) E.removeFaces(del); }
    std::vector<int> fs(E.f.size()); std::iota(fs.begin(), fs.end(), 0); made += addCopies(E, fs, Ts, M, std::max({1e-6, wd, B.bounds().size().length() * 1e-4}));
    topoWrite(m, std::make_shared<PolyMesh>(E.to(B))); }
  scene_->item(group)->raw.remove("vsym");
  scene_->revision++; commit(true); emit topoState(); emit sceneEdited();
  const QString msg = tr("Symmetry made real: %1 polygons in %2 mesh%3").arg(made).arg(ms.size()).arg(ms.size() == 1 ? "" : "es"); ui::toast(msg); return msg;
}

}  // namespace hm

namespace hm {
void Viewport::armOnce(const QString& a) {
  tt_.arm = a;
  ui::hud(a == "optpick" ? tr("Drag in the viewport: the weld distance (→ wider)") : a == "nturn" ? tr("Drag in the viewport: the selection's normals turned (←→ round the view's up, ↑↓ round its right)")
                                                               : tr("Click the mesh to copy the normals from"), 4000);
}
}  // namespace hm

namespace hm {
// Edges blend into the back wall: at each vertex of the selected edges the normal of the polygons round it turned away
// from the camera (the back wall — orange in two-tone) put into every corner there, so the edge's band shades as the
// wall behind it and the border melts away (a sleeve's cut edge, a hem's thickness)
QString Viewport::tNormalsFromBack() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  if (tsel_.id != m || tsel_.e.empty()) { ui::toast(tr("Select the edges whose normals should blend into the back wall (Edge mode)")); return "!"; }
  auto nb = std::make_shared<PolyMesh>(*baseOf(*it)); PolyMesh& d = *nb; auto T = topo::ttopo(baseSp(*it)); const auto W = ttWorld(m);
  if (d.cornerNormal.size() != d.faceVerts.size()) { d.cornerNormal.clear(); for (auto& n : hardCornerNormals(d)) d.cornerNormal.push_back({float(n.x), float(n.y), float(n.z)}); }
  std::set<int> vs; for (int e : tsel_.e) if (e >= 0 && e < T->ne) { vs.insert(T->ea[size_t(e)]); vs.insert(T->eb[size_t(e)]); }
  // (each polygon: its normal (the mesh's own space) and whether it turns away from the camera)
  const int nf = d.faceCount(); std::vector<Vec3> faceN(static_cast<size_t>(nf)); std::vector<char> awayF(static_cast<size_t>(nf), 0);
  for (int f = 0; f < nf; f++) { const int s = d.faceStart[size_t(f)], e = d.faceStart[size_t(f) + 1]; Vec3 n, nw, c;
    for (int k = s; k < e; k++) { const int a = d.faceVerts[size_t(k)], b = d.faceVerts[size_t(k + 1 < e ? k + 1 : s)];
      const Vec3& p = d.pos[size_t(a)]; const Vec3& q = d.pos[size_t(b)]; n.x += (p.y - q.y) * (p.z + q.z); n.y += (p.z - q.z) * (p.x + q.x); n.z += (p.x - q.x) * (p.y + q.y);
      const Vec3& pw = W[size_t(a)]; const Vec3& qw = W[size_t(b)]; nw.x += (pw.y - qw.y) * (pw.z + qw.z); nw.y += (pw.z - qw.z) * (pw.x + qw.x); nw.z += (pw.x - qw.x) * (pw.y + qw.y); c += pw; }
    // (one seen nearly edge-on — the hem's underside between the band and the wall — neither: away or turned to the camera
    // only past a margin, else the hem's normals taken for the wall's)
    c = c * (1.0 / double(e - s)); faceN[size_t(f)] = n; const double cs = nw.normalized().dot((cam_.ortho() ? cam_.dir() : c - cam_.pos).normalized());
    awayF[size_t(f)] = cs > 0.2 ? 1 : cs < -0.2 ? 0 : 2; }
  // (the band the edges bound — the polygons turned to the camera touching them: all of its corners blended, its far side too,
  // else its shading still runs from the wall's at the edge to its own and the band shows)
  std::vector<char> bandF(static_cast<size_t>(nf), 0);
  { const std::set<int> v0 = vs; for (int f = 0; f < nf; f++) { if (awayF[size_t(f)] != 0) continue; bool touch = false; for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1] && !touch; k++) touch = v0.count(d.faceVerts[size_t(k)]) > 0;
      if (touch) { bandF[size_t(f)] = 1; for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) vs.insert(d.faceVerts[size_t(k)]); } } }
  // (the wall's own normals as it is shaded now — smoothed, set by hand — not its polygons' bare geometry: else the band and the
  // wall still part at the seam)
  auto cornerN = [&](int k) { const auto& c = d.cornerNormal[size_t(k)]; return Vec3(c[0], c[1], c[2]); };
  std::map<int, Vec3> vn; for (int f = 0; f < nf; f++) if (awayF[size_t(f)] == 1) for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) { const int v = d.faceVerts[size_t(k)]; if (vs.count(v)) vn[v] += cornerN(k); }
  // (a vertex with no back-wall polygon round it — the edge's band a piece of its own, not welded to the wall: the nearest
  // polygons of the back wall, within a few of its edges' lengths, weighted by how near)
  { std::vector<int> backF; for (int f = 0; f < nf; f++) if (awayF[size_t(f)] == 1) backF.push_back(f);
    std::vector<Vec3> cen(static_cast<size_t>(nf)); for (int f : backF) { Vec3 c; const int s = d.faceStart[size_t(f)], e = d.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) c += d.pos[size_t(d.faceVerts[size_t(k)])]; cen[size_t(f)] = c * (1.0 / double(e - s)); }
    std::map<int, double> elen; for (int e : tsel_.e) if (e >= 0 && e < T->ne) { const double l = d.pos[size_t(T->ea[size_t(e)])].distanceTo(d.pos[size_t(T->eb[size_t(e)])]); for (int v : {T->ea[size_t(e)], T->eb[size_t(e)]}) elen[v] = std::max(elen[v], l); }
    // (first a vertex of the wall lying on this one — the layers' points put one onto the other, not welded: its normal
    // exactly, the back-wall polygons round it)
    Box3 bb; for (auto& q : d.pos) bb.expand(q); const double eps = std::max(1e-9, bb.size().length() * 1e-5);
    std::map<std::array<long long, 3>, std::vector<int>> at; auto key = [&](const Vec3& q) { return std::array<long long, 3>{std::llround(q.x / eps), std::llround(q.y / eps), std::llround(q.z / eps)}; };
    for (int u = 0; u < int(d.pos.size()); u++) at[key(d.pos[size_t(u)])].push_back(u);
    std::map<int, std::vector<int>> cornersAt; for (int f : backF) for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) cornersAt[d.faceVerts[size_t(k)]].push_back(k);
    for (int v : vs) { if (vn.count(v)) continue; Vec3 n; const auto k0 = key(d.pos[size_t(v)]);
      for (long long i = -1; i <= 1; i++) for (long long j = -1; j <= 1; j++) for (long long l = -1; l <= 1; l++) { auto f = at.find({k0[0] + i, k0[1] + j, k0[2] + l}); if (f == at.end()) continue;
        for (int u : f->second) if (u != v && d.pos[size_t(u)].distanceTo(d.pos[size_t(v)]) <= eps) { auto g = cornersAt.find(u); if (g != cornersAt.end()) for (int k : g->second) n += cornerN(k); } }
      if (n.lengthSq() > 0) vn[v] = n; }
    // (then one lying on an edge of the wall — the band a vertex more than the wall there: the normals of that edge's ends,
    // as far along it as it lies)
    std::map<int, Vec3> wallVN; for (auto& [u, ks] : cornersAt) { Vec3 s; for (int k : ks) s += cornerN(k); if (s.lengthSq() > 0) wallVN[u] = s.normalized(); }
    std::set<std::pair<int, int>> wallE; for (int f : backF) { const int s = d.faceStart[size_t(f)], e = d.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) { const int a = d.faceVerts[size_t(k)], b = d.faceVerts[size_t(k + 1 < e ? k + 1 : s)]; wallE.insert({std::min(a, b), std::max(a, b)}); } }
    for (int v : vs) { if (vn.count(v)) continue; const Vec3& p = d.pos[size_t(v)]; double best = 1e300; Vec3 n;
      for (auto& [a, b] : wallE) { if (a == v || b == v || !wallVN.count(a) || !wallVN.count(b)) continue; const Vec3 A = d.pos[size_t(a)], AB = d.pos[size_t(b)] - A; const double L2 = AB.lengthSq(); if (L2 <= 0) continue;
        const double t = std::clamp((p - A).dot(AB) / L2, 0.0, 1.0); const double dd = (A + AB * t).distanceTo(p); if (dd > 0.25 * std::sqrt(L2) || dd >= best) continue;
        best = dd; n = wallVN[a] * (1.0 - t) + wallVN[b] * t; }
      if (n.lengthSq() > 0) vn[v] = n; }
    for (int v : vs) { if (vn.count(v) || backF.empty()) continue; const Vec3& p = d.pos[size_t(v)];
      double best = 1e300; for (int f : backF) best = std::min(best, cen[size_t(f)].distanceTo(p));
      const double r = std::max(best * 1.5, elen[v] * 2.0); Vec3 n;
      for (int f : backF) { const double dd = cen[size_t(f)].distanceTo(p); if (dd > r) continue; Vec3 c; for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) c += cornerN(k); n += c.normalized() * (1.0 / (1e-9 + dd)); }
      if (n.lengthSq() > 0) vn[v] = n; } }
  int done = 0;
  // (every corner there — the wall's and the band's — the wall's normal: the band then shades as the wall, seen from
  // either side)
  // (the wall's and the band's only — a polygon seen edge-on, the hem's underside, keeps its own)
  for (int f = 0; f < nf; f++) if (awayF[size_t(f)] == 1 || bandF[size_t(f)]) for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) { auto it2 = vn.find(d.faceVerts[size_t(k)]); if (it2 == vn.end() || it2->second.lengthSq() <= 0) continue;
    // (a polygon turned the other way from the wall — the band folded back over it — the normal turned with it: the side seen
    // from the back is lit by the normal reversed, so the band and the wall then shade alike from either side)
    Vec3 n = it2->second.normalized(); if (n.dot(faceN[size_t(f)]) < 0) n = n * -1.0; d.cornerNormal[size_t(k)] = {float(n.x), float(n.y), float(n.z)}; done++; }
  if (!done) { ui::toast(tr("No back wall at those edges: turn the view so the wall they should blend into faces away (orange in two-tone)")); return "!"; }
  // (no hard edge left through those vertices: the shading runs on smoothly)
  std::vector<std::array<int, 2>> keep; for (auto& h : d.hardEdges) if (!(vn.count(h[0]) && vn.count(h[1]))) keep.push_back(h); d.hardEdges = keep;
  topo::shareTopo(nb, baseSp(*it)); topoWrite(m, nb); tsel_.B = nb.get();
  if (!it->raw.value("smoothN").toBool()) { it->raw.insert("smoothN", true); staticRev_++; }   // (seen only with smooth shading)
  scene_->revision++; commit(true); emit topoState();
  const QString msg = tr("Normals of %1 vertices from the back wall — the edges blend into it").arg(vn.size()); ui::toast(msg); return msg;
}

// the selected vertices' normals set to one direction: a world axis (+x … -z), toward the camera (cam) or their average
// (one); reset — back to the standard smoothing (smooth everywhere but the hard edges; nothing selected — the whole mesh)
QString Viewport::tNormalsSet(const QString& dir) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  const std::vector<int> sel = tSelVerts(m);
  auto nb = std::make_shared<PolyMesh>(*baseOf(*it)); PolyMesh& d = *nb;
  if (dir == "reset") {
    if (d.cornerNormal.empty()) { ui::toast(tr("The normals are already the standard ones")); return "!"; }
    if (sel.empty()) d.cornerNormal.clear();                        // (the whole mesh: no normals of its own kept at all)
    else { const std::set<int> vs(sel.begin(), sel.end()); const auto H = hardCornerNormals(d);
      for (size_t k = 0; k < d.faceVerts.size() && k < d.cornerNormal.size(); k++) if (vs.count(d.faceVerts[k])) d.cornerNormal[k] = {float(H[k].x), float(H[k].y), float(H[k].z)}; }
    topo::shareTopo(nb, baseSp(*it)); topoWrite(m, nb); tsel_.B = nb.get(); scene_->revision++; commit(true); emit topoState();
    const QString msg = sel.empty() ? tr("Normals reset to the standard smoothing — the whole mesh") : tr("Normals of %1 vertices reset to the standard smoothing").arg(sel.size()); ui::toast(msg); return msg;
  }
  if (sel.empty()) { ui::toast(tr("Select the vertices / polygons whose normals to set")); return "!"; }
  if (d.cornerNormal.size() != d.faceVerts.size()) { d.cornerNormal.clear(); for (auto& n : hardCornerNormals(d)) d.cornerNormal.push_back({float(n.x), float(n.y), float(n.z)}); }
  const std::set<int> vs(sel.begin(), sel.end());
  Vec3 n;
  if (dir == "one") { for (size_t k = 0; k < d.faceVerts.size(); k++) if (vs.count(d.faceVerts[k])) { const auto& c = d.cornerNormal[k]; n += Vec3(c[0], c[1], c[2]); } }
  else {                                                            // (a world direction, into the mesh's own space)
    Vec3 w = dir == "cam" ? cam_.dir() * -1.0 : Vec3(dir.endsWith('x'), dir.endsWith('y'), dir.endsWith('z')) * (dir.startsWith('-') ? -1.0 : 1.0);
    n = meshmods::meshMatrix(*scene_, *it).inverse().lin(w); }
  if (n.lengthSq() <= 0) { ui::toast(tr("No direction to set")); return "!"; }
  n = n.normalized(); int done = 0;
  for (size_t k = 0; k < d.faceVerts.size(); k++) if (vs.count(d.faceVerts[k])) { d.cornerNormal[k] = {float(n.x), float(n.y), float(n.z)}; done++; }
  std::vector<std::array<int, 2>> keep; for (auto& h : d.hardEdges) if (!(vs.count(h[0]) && vs.count(h[1]))) keep.push_back(h); d.hardEdges = keep;
  topo::shareTopo(nb, baseSp(*it)); topoWrite(m, nb); tsel_.B = nb.get();
  if (!it->raw.value("smoothN").toBool()) { it->raw.insert("smoothN", true); staticRev_++; }   // (seen only with smooth shading)
  scene_->revision++; commit(true); emit topoState();
  const QString msg = tr("Normals of %1 vertices set").arg(vs.size()); ui::toast(msg); return msg;
}
}  // namespace hm

namespace hm {
int Viewport::tSelectOpenEdges() {
  const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return 0;
  auto T = topo::ttopo(it->base ? it->base : it->poly); tsel_ = {}; tsel_.id = m;
  for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] < 0) tsel_.e.insert(e);
  tsel_.ver++; topoRev_++; emit topoState(); return int(tsel_.e.size());
}
}  // namespace hm
