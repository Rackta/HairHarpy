// The Topology room's modelling edits on the selection (the tools' pointer: ViewportTopoTools.cpp):
//  Weld — in a point: the selected vertices onto the vertex under the pointer (else their middle); in pairs: two chains
//   of edges, vertex by vertex (two edges of a box → a tube).
//  Plane Cut — the mesh cut by the plane through the line drawn (edge-on to the view): cut only, split apart, or one
//   side removed; several cuts at a spacing; only the selection; the cuts selected.
//  Bevel / Extrude — the selected polygons pushed out along their normals, their rim bevelled (rounded by segments),
//   caps top / bottom, what is selected after; Inset — the polygons' rim drawn in (together or each one), deeper by
//   the depth; on vertices — a circle of so many sides round each one.
//  Bridge — between two chains of edges (or the rims of two selections of polygons): rows of polygons, subdivided,
//   bulged by the tension, turned by the spin steps.
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <deque>
#include <QJsonArray>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>

namespace hm {

using tedit::MeshEd; using tedit::ek;

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
QJsonArray v3(const Vec3& p) { return {p.x, p.y, p.z}; }
Vec3 v3(const QJsonValue& v) { auto a = v.toArray(); return {a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble()}; }

// the region's polygons pushed out: rings along its rim at (height, inset) each, its inside to the last height;
// walls between the rings; top (the moved polygons) / bottom (the old place, turned round) caps → the polygons made
struct ExtOut { std::vector<int> top, side, bottom; };
// (smart: where the rim borders polygons not in the region, no walls — its vertices there stay put, the region stitched
// to those polygons, the rest of it pushed out: a slope from them, no inner walls)
ExtOut extrudeRegion(MeshEd& E, const std::set<int>& S, const std::vector<std::pair<double, double>>& prof, bool top, bool bottom, bool smart = false) {
  ExtOut o; if (prof.empty() || S.empty()) return o;
  std::set<int> RV; for (int fi : S) for (int v : E.f[size_t(fi)].v) RV.insert(v);
  std::set<std::pair<int, int>> stitched; std::set<int> pin; std::set<int> rimF;
  if (smart) {
    // (the rim (кайма): thin polygons — a side well under half the longest — joined to the region's edge, up to three rows,
    // that reach the mesh's open edge: they go, and the region's edge there moves out free — no wall, nothing stitched)
    std::map<tedit::EK, int> ecnt; for (const auto& F : E.f) { if (F.v.size() < 3) continue; for (size_t k = 0; k < F.v.size(); k++) ecnt[ek(F.v[k], F.v[(k + 1) % F.v.size()])]++; }
    std::vector<std::vector<int>> vf(E.pos.size()); for (size_t fi = 0; fi < E.f.size(); fi++) if (E.f[fi].v.size() >= 3) for (int v : E.f[fi].v) vf[size_t(v)].push_back(int(fi));
    auto thin = [&](int fi) { const auto& V = E.f[size_t(fi)].v; double lo = 1e300, hi = 0; for (size_t k = 0; k < V.size(); k++) { const double l = E.pos[size_t(V[k])].distanceTo(E.pos[size_t(V[(k + 1) % V.size()])]); lo = std::min(lo, l); hi = std::max(hi, l); } return hi > 0 && lo < hi * 0.5; };
    auto open = [&](int fi) { const auto& V = E.f[size_t(fi)].v; for (size_t k = 0; k < V.size(); k++) if (ecnt[ek(V[k], V[(k + 1) % V.size()])] == 1) return true; return false; };
    std::set<int> front; for (int v : RV) for (int g : vf[size_t(v)]) if (!S.count(g) && thin(g)) front.insert(g);
    std::set<int> cand = front;
    for (int row = 1; row < 3 && !front.empty(); row++) { std::set<int> nx; for (int g : front) for (int v : E.f[size_t(g)].v) for (int h : vf[size_t(v)]) if (!S.count(h) && !cand.count(h) && thin(h)) nx.insert(h); cand.insert(nx.begin(), nx.end()); front = nx; }
    // (kept only the pieces of it that touch the open edge)
    std::set<int> seen; for (int g0 : cand) { if (seen.count(g0)) continue; std::vector<int> part{g0}, st{g0}; seen.insert(g0); bool reach = false;
      while (!st.empty()) { const int g = st.back(); st.pop_back(); reach = reach || open(g); for (int v : E.f[size_t(g)].v) for (int h : vf[size_t(v)]) if (cand.count(h) && !seen.count(h)) { seen.insert(h); st.push_back(h); part.push_back(h); } }
      if (reach) rimF.insert(part.begin(), part.end()); }
    std::set<std::pair<int, int>> other;
    for (size_t fi = 0; fi < E.f.size(); fi++) { if (S.count(int(fi)) || rimF.count(int(fi)) || E.f[fi].v.size() < 3) continue; const auto& V = E.f[fi].v; for (size_t k = 0; k < V.size(); k++) other.insert({V[(k + 1) % V.size()], V[k]}); }
    std::set<std::pair<int, int>> loose; for (int g : rimF) { const auto& V = E.f[size_t(g)].v; for (size_t k = 0; k < V.size(); k++) loose.insert({V[(k + 1) % V.size()], V[k]}); }
    for (auto& [a, b] : tedit::regionBorder(E, S)) { if (other.count({a, b})) { stitched.insert({a, b}); pin.insert(a); pin.insert(b); } else if (loose.count({a, b})) stitched.insert({a, b}); } }
  std::map<int, Vec3> nrm; for (int fi : S) { Vec3 n = E.normal(fi); if (n.lengthSq() > 0) n = n.normalized(); for (int v : E.f[size_t(fi)].v) nrm[v] += n; }
  for (auto& [v, n] : nrm) n = n.lengthSq() > 0 ? n.normalized() : Vec3(0, 1, 0);
  // (the rim: each edge in its polygon's sense, the polygon; inward — towards the region's inside, in its plane)
  std::map<std::pair<int, int>, int> owner; for (int fi : S) { const auto& V = E.f[size_t(fi)].v; for (size_t k = 0; k < V.size(); k++) owner[{V[k], V[(k + 1) % V.size()]}] = fi; }
  auto border = tedit::regionBorder(E, S);
  std::map<int, Vec3> inward;
  for (auto& [a, b] : border) { Vec3 n = E.normal(owner[{a, b}]); if (n.lengthSq() > 0) n = n.normalized(); Vec3 in = n.cross(E.pos[size_t(b)] - E.pos[size_t(a)]).normalized(); inward[a] += in; inward[b] += in; }
  for (auto& [v, n] : inward) n = n.lengthSq() > 0 ? n.normalized() : Vec3();
  const size_t R = prof.size(); std::map<int, std::vector<int>> ring;
  for (auto& [v, in] : inward) for (size_t r = 0; r < R; r++) ring[v].push_back(pin.count(v) ? v : E.addVert(E.pos[size_t(v)] + nrm[v] * prof[r].first + in * prof[r].second));
  std::map<int, int> topV; for (int v : RV) topV[v] = pin.count(v) ? v : ring.count(v) ? ring[v].back() : E.addVert(E.pos[size_t(v)] + nrm[v] * prof.back().first);
  std::vector<MeshEd::F> bot; if (bottom) for (int fi : S) { MeshEd::F b = E.f[size_t(fi)]; std::reverse(b.v.begin(), b.v.end()); std::reverse(b.uv.begin(), b.uv.end()); std::reverse(b.n.begin(), b.n.end()); for (auto& q : b.n) q = {-q[0], -q[1], -q[2]}; bot.push_back(b); }
  std::map<std::pair<int, int>, std::pair<tedit::UVx, tedit::UVx>> rimUV;   // (the walls: the rim's UVs carried up)
  for (auto& [a, b] : border) { const int fi = owner[{a, b}]; const int ca = E.corner(fi, a), cb = E.corner(fi, b);
    if (E.hasUV) rimUV[{a, b}] = {E.f[size_t(fi)].uv[size_t(ca)], E.f[size_t(fi)].uv[size_t(cb)]}; }
  for (int fi : S) for (int& v : E.f[size_t(fi)].v) v = topV[v];
  for (auto& [a, b] : border) {
    if (stitched.count({a, b})) continue;                       // (smart: stitched there — no wall)
    const int fi = owner[{a, b}]; int a0 = a, b0 = b;
    for (size_t r = 0; r < R; r++) { const int a1 = ring[a][r], b1 = ring[b][r];
      std::vector<int> q; for (int x : {a0, b0, b1, a1}) if (std::find(q.begin(), q.end(), x) == q.end()) q.push_back(x);   // (one end pinned: a triangle)
      if (q.size() < 3) { a0 = a1; b0 = b1; continue; }
      MeshEd::F s; s.v = q; s.g = E.f[size_t(fi)].g; s.mat = E.f[size_t(fi)].mat; s.sets = E.f[size_t(fi)].sets; s.live = E.f[size_t(fi)].live;
      if (E.hasUV) { auto [ua, ub] = rimUV[{a, b}]; const int all4[4] = {a0, b0, b1, a1}; const tedit::UVx u4[4] = {ua, ub, ub, ua}; for (int x : q) for (int k = 0; k < 4; k++) if (all4[k] == x) { s.uv.push_back(u4[k]); break; } }
      E.f.push_back(s); o.side.push_back(int(E.f.size()) - 1); a0 = a1; b0 = b1; }
  }
  for (auto& b : bot) { E.f.push_back(b); o.bottom.push_back(int(E.f.size()) - 1); }
  if (top) o.top.assign(S.begin(), S.end());
  if (!top || !rimF.empty()) {                                  // (no top: the moved polygons dropped; smart: the rim gone — the others renumbered)
    std::vector<char> del(E.f.size(), 0); if (!top) for (int fi : S) del[size_t(fi)] = 1; for (int g : rimF) del[size_t(g)] = 1;
    std::vector<int> shift(E.f.size(), 0); int c = 0; for (size_t i = 0; i < E.f.size(); i++) { shift[i] = c; if (del[i]) c++; }
    for (int& x : o.top) x -= shift[size_t(x)]; for (int& x : o.side) x -= shift[size_t(x)]; for (int& x : o.bottom) x -= shift[size_t(x)];
    E.removeFaces(del);
  }
  return o;
}
}  // namespace

// ---------- Weld ----------
QString Viewport::weldNow(const QPointF* at) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it || tsel_.id != m || topoMode == "object") { ui::toast(tr("Select the vertices (or, in pairs, two chains of edges) to weld")); return "!"; }
  if (ttools.weldMode != "pair") { const size_t n = tSelVerts(m).size(), nv = baseOf(*it)->pos.size();   // (the whole mesh selected by mistake: refused before any work)
    if (n > 1000 && n * 2 > nv) { ui::toast(tr("Weld merges the selected vertices into ONE point — %1 of them would collapse the mesh. Select only the vertices to merge (close ones: Optimize)").arg(n)); return "!"; } }
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); auto T = topo::ttopo(baseSp(*it));
  std::vector<int> to(E.pos.size(), -1);
  if (ttools.weldMode == "pair") {
    std::vector<tedit::EK> es;
    if (tKey() == 'e') for (int e : tsel_.e) es.push_back(ek(T->ea[size_t(e)], T->eb[size_t(e)]));
    else { auto vs = tSelVerts(m); std::set<int> s(vs.begin(), vs.end()); for (int e = 0; e < T->ne; e++) if (s.count(T->ea[size_t(e)]) && s.count(T->eb[size_t(e)])) es.push_back(ek(T->ea[size_t(e)], T->eb[size_t(e)])); }
    std::vector<std::pair<int, int>> pr;                          // (the pairs: [0] of the first chain, [1] of the second)
    auto ch = tedit::edgeChains(es);
    if (ch.size() == 2 && ch[0].v.size() == ch[1].v.size() && ch[0].closed == ch[1].closed) {
      auto c1 = ch[0].v, c2 = ch[1].v;
      const size_t n = c1.size(); auto cost = [&](const std::vector<int>& b) { double s = 0; for (size_t i = 0; i < n; i++) s += E.pos[size_t(c1[i])].distanceTo(E.pos[size_t(b[i])]); return s; };
      std::vector<int> best = c2; double bc = cost(c2);
      for (int dirn = 0; dirn < 2; dirn++) { std::vector<int> b = c2; if (dirn) std::reverse(b.begin(), b.end());
        for (size_t off = 0; off < (ch[0].closed ? n : 1); off++) { std::vector<int> r(n); for (size_t i = 0; i < n; i++) r[i] = b[(i + off) % n]; double c = cost(r); if (c < bc) { bc = c; best = r; } } }
      for (size_t i = 0; i < n; i++) if (c1[i] != best[i]) pr.push_back({c1[i], best[i]});
    } else {
      // (not two even chains — edges across the band picked too, a chain broken, uneven counts: each vertex paired with
      // its nearest picked one when that one's nearest is it back; the leftovers onto the nearest vertex already paired
      // when it lies as close as the pairs do)
      std::vector<int> vs; { std::set<int> s; for (auto& e : es) { s.insert(e.first); s.insert(e.second); } vs.assign(s.begin(), s.end()); }
      const size_t n = vs.size(); std::vector<int> nn(n, -1);
      for (size_t i = 0; i < n; i++) { double bd = 1e300; for (size_t j = 0; j < n; j++) if (j != i) { const double d = E.pos[size_t(vs[i])].distanceTo(E.pos[size_t(vs[j])]); if (d < bd) { bd = d; nn[i] = int(j); } } }
      std::vector<char> used(n, 0); std::vector<double> ds;
      for (size_t i = 0; i < n; i++) { const int j = nn[i]; if (j > int(i) && nn[size_t(j)] == int(i)) { pr.push_back({vs[i], vs[size_t(j)]}); used[i] = used[size_t(j)] = 1; ds.push_back(E.pos[size_t(vs[i])].distanceTo(E.pos[size_t(vs[size_t(j)])])); } }
      if (pr.empty()) { ui::toast(tr("Pairs: no two picked vertices face each other — pick the two edge chains to weld")); return "!"; }
      std::sort(ds.begin(), ds.end()); const double lim = ds[ds.size() / 2] * 1.75 + 1e-9;
      for (size_t i = 0; i < n; i++) if (!used[i]) { double bd = lim; int bj = -1;
        for (size_t j = 0; j < n; j++) if (used[j]) { const double d = E.pos[size_t(vs[i])].distanceTo(E.pos[size_t(vs[j])]); if (d < bd) { bd = d; bj = int(j); } }
        if (bj >= 0) pr.push_back({vs[i], vs[size_t(bj)]}); }
    }
    // (the two sides: the picked edges left once those across the band are dropped, gathered into pieces; Onto a side —
    // the side under the pointer (the button: the one picked last) stays, the other is welded onto it; else the middle)
    std::set<tedit::EK> rung; for (auto& [a, b] : pr) rung.insert(ek(a, b));
    std::map<int, int> comp; { std::map<int, std::vector<int>> adj; for (auto& e : es) if (!rung.count(e)) { adj[e.first].push_back(e.second); adj[e.second].push_back(e.first); }
      int c = 0; for (auto& [v, a] : adj) if (!comp.count(v)) { std::vector<int> st{v}; comp[v] = c; while (!st.empty()) { const int x = st.back(); st.pop_back(); for (int w : adj[x]) if (!comp.count(w)) { comp[w] = c; st.push_back(w); } } c++; } }
    int keep = -1;
    if (ttools.weldPos == "side") {
      if (at) {
        // (RMB on a polygon of the side to keep: from it outwards over the mesh — the first polygon that touches one side
        // only (not the band between them) names it; off the mesh — the side nearest the pointer on the screen)
        if (auto hit = tPick(*at, {m}); hit && hit->f >= 0 && hit->f < int(E.f.size())) {
          std::map<int, std::vector<int>> vf; for (size_t fi = 0; fi < E.f.size(); fi++) for (int v : E.f[fi].v) vf[v].push_back(int(fi));
          std::set<int> seen{hit->f}; std::deque<int> q{hit->f};
          while (!q.empty() && keep < 0 && seen.size() < 4000) { const int f = q.front(); q.pop_front(); std::set<int> cs;
            for (int v : E.f[size_t(f)].v) if (auto c = comp.find(v); c != comp.end()) cs.insert(c->second);
            if (cs.size() == 1) { keep = *cs.begin(); break; }
            for (int v : E.f[size_t(f)].v) for (int g : vf[v]) if (seen.insert(g).second) q.push_back(g); } }
        if (keep < 0) { Projector P = projector(); const auto W = ttWorld(m); double bd = 1e300;
          for (auto& [v, c] : comp) { ScreenPt s = P.toScreen(W[size_t(v)]); const double d = std::hypot(s.x - at->x(), s.y - at->y()); if (s.z < 1 && d < bd) { bd = d; keep = c; } } } }
      else if (tKey() == 'e' && tsel_.last >= 0 && tsel_.last < T->ne) { auto f = comp.find(T->ea[size_t(tsel_.last)]); if (f != comp.end()) keep = f->second; }
    }
    int moved = 0;
    for (auto [a, b] : pr) {
      if (a == b || to[size_t(a)] >= 0 || to[size_t(b)] >= 0) continue;
      auto ca = comp.find(a), cb = comp.find(b);
      const bool aKeep = keep >= 0 && ca != comp.end() && ca->second == keep, bKeep = keep >= 0 && cb != comp.end() && cb->second == keep;
      if (aKeep && !bKeep) std::swap(a, b);                       // (b stays: a goes onto it)
      else if (!bKeep) E.pos[size_t(b)] = (E.pos[size_t(a)] + E.pos[size_t(b)]) * 0.5;
      to[size_t(a)] = b; moved++;
    }
    if (!moved) { ui::toast(tr("Pairs: nothing to weld")); return "!"; }
    E.weld(to);
    return ttDone(m, E, B, tr("Welded in pairs: %1 vertices").arg(moved), 0, {});   // (the mode as it was — no switch to vertices)
  }
  auto vs = tSelVerts(m); if (vs.size() < 2) { ui::toast(tr("Select two vertices or more")); return "!"; }
  int tv = -1;
  if (at) { Projector P = projector(); const auto W = ttWorld(m); double bd = 14;
    for (size_t v = 0; v < W.size(); v++) { ScreenPt s = P.toScreen(W[v]); const double d = std::hypot(s.x - at->x(), s.y - at->y()); if (s.z < 1 && d < bd) { bd = d; tv = int(v); } } }
  if (tv < 0) { Vec3 c; for (int v : vs) c += E.pos[size_t(v)]; c = c * (1.0 / double(vs.size())); tv = vs[0]; E.pos[size_t(tv)] = c; }
  for (int v : vs) if (v != tv) to[size_t(v)] = tv;
  E.weld(to);
  // (the whole mesh — or nearly — selected by mistake: welding it into one point would leave nothing; refused)
  { size_t live = 0; for (const auto& f : E.f) if (f.v.size() >= 3) live++;
    if (live == 0 || (vs.size() > 1000 && vs.size() * 2 > B.pos.size())) { ui::toast(tr("Weld merges the selected vertices into ONE point — %1 of them would collapse the mesh. Select only the vertices to merge (close ones: Optimize)").arg(vs.size())); return "!"; } }
  return ttDone(m, E, B, tr("Welded: %1 vertices into one").arg(vs.size()), 0, {});
}

// ---------- Plane Cut ----------
QString Viewport::planeApply(int m, const Vec3& o, const Vec3& n, const Vec3& along) {
  Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); const Affine M = meshmods::meshMatrix(*scene_, *it); tt_.lastSel = tsel_;
  auto W = [&](int v) { return M.apply(E.pos[size_t(v)]); };
  const double eps = std::max(1e-9, B.bounds().size().length() * 1e-7);
  std::vector<char> ok(E.f.size(), 1);
  if (ttools.planeSel) { auto fs = tSelFaces(m); if (fs.empty()) { ui::toast(tr("Restrict to selection: select polygons first")); return "!"; } std::fill(ok.begin(), ok.end(), 0); for (int f : fs) ok[size_t(f)] = 1; }
  const int N = ttools.planeCuts; std::vector<std::pair<int, int>> cuts; int crossed = 0;
  auto dOf = [&](int v, double s) { return (W(v) - o).dot(n) - s; };
  std::vector<double> planes; for (int k = 0; k < N; k++) planes.push_back((k - (N - 1) / 2.0) * ttools.planeSpacing);
  for (double s : planes) {
    std::map<tedit::EK, int> made;
    std::set<tedit::EK> edges; for (size_t fi = 0; fi < E.f.size(); fi++) if (ok[fi]) { const auto& V = E.f[fi].v; for (size_t k = 0; k < V.size(); k++) edges.insert(ek(V[k], V[(k + 1) % V.size()])); }
    for (auto& e : edges) { const double da = dOf(e.first, s), db = dOf(e.second, s);
      if ((da < -eps && db > eps) || (da > eps && db < -eps)) { const double t = da / (da - db); const int v = E.addVert(E.pos[size_t(e.first)] + (E.pos[size_t(e.second)] - E.pos[size_t(e.first)]) * t); made[e] = v; } }
    for (auto& [e, v] : made) { const double da = dOf(e.first, s), db = dOf(e.second, s); E.insertOnEdge(e.first, e.second, {v}, {da / (da - db)}); }
    const size_t nf = E.f.size();
    for (size_t fi = 0; fi < nf; fi++) {
      if (!ok[fi]) continue;
      std::vector<int> on; for (int v : E.f[fi].v) if (std::abs(dOf(v, s)) <= eps) on.push_back(v);
      if (on.size() < 2) continue;
      std::sort(on.begin(), on.end(), [&](int a, int b) { return W(a).dot(along) < W(b).dot(along); });
      for (size_t i = 0; i + 1 < on.size(); i += 2) {
        int fx = -1; for (int x : E.facesOfVert(on[i])) if (size_t(x) < ok.size() ? ok[size_t(x)] : true) if (E.corner(x, on[i + 1]) >= 0) { fx = x; break; }
        if (fx >= 0 && E.splitFace(fx, on[i], on[i + 1]) >= 0) { ok.push_back(1); cuts.push_back({on[i], on[i + 1]}); crossed++; }
      }
    }
    // (Split: the vertices on the plane doubled for the polygons on its far side)
    if (ttools.planeMode == "split") {
      std::map<int, int> twin;
      for (size_t fi = 0; fi < E.f.size(); fi++) { if (!ok[fi]) continue; double dc = 0; for (int v : E.f[fi].v) dc += dOf(v, s); if (dc <= 0) continue;
        for (int& v : E.f[fi].v) if (std::abs(dOf(v, s)) <= eps) { auto t = twin.find(v); if (t == twin.end()) t = twin.emplace(v, E.addVert(E.pos[size_t(v)])).first; v = t->second; } }
    }
  }
  if (!crossed) { ui::toast(tr("The plane misses the mesh")); return "!"; }
  if (ttools.planeMode == "removeA" || ttools.planeMode == "removeB") {   // (one side gone — of the middle plane)
    std::vector<char> del(E.f.size(), 0); int n0 = 0;
    for (size_t fi = 0; fi < E.f.size(); fi++) { if (!ok[fi]) continue; Vec3 c = M.apply(E.center(int(fi))); const double d = (c - o).dot(n); if ((ttools.planeMode == "removeA") == (d > 0)) { del[fi] = 1; n0++; } }
    if (n0 == int(E.f.size())) { ui::toast(tr("That would remove the whole mesh")); return "!"; }
    E.removeFaces(del);
  }
  tt_.lastOp = "plane"; tt_.lastArgs = QJsonObject{{"o", v3(o)}, {"n", v3(n)}, {"along", v3(along)}};
  const QString what = ttools.planeMode == "split" ? tr(" — split apart") : ttools.planeMode == "removeA" ? tr(" — part A removed") : ttools.planeMode == "removeB" ? tr(" — part B removed") : QString();
  return ttDone(m, E, B, tr("Plane Cut: %1 polygon%2 cut%3").arg(crossed).arg(crossed == 1 ? "" : "s").arg(what), ttools.planeSelCuts ? 'e' : 0, {}, cuts);
}

// ---------- Bevel / Extrude, Inset ----------
QString Viewport::bevelApply(int m, double offset, bool inset) {
  Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  if (!inset && (bool(tt_.before) ? tt_.bevEdges : (tKey() == 'e' && tsel_.id == m && !tsel_.e.empty()))) return bevelEdgesApply(m, offset);   // (edges: a chamfer)
  const bool live = bool(tt_.before); if (!live) tt_.lastSel = tsel_;
  std::shared_ptr<PolyMesh> src = live ? tt_.before : baseSp(*it);
  std::vector<int> faces = live ? tt_.verts : tSelFaces(m);
  if (faces.empty()) { ui::toast(tr("Select polygons first")); return "!"; }
  MeshEd E = MeshEd::from(*src); for (int f : faces) if (f >= int(E.f.size())) return "!";
  if (!inset && live && tt_.bevPull) {                            // (Shift: pulled out — the polygons' vertices moved along their normals, nothing made)
    std::map<int, Vec3> nrm; for (int fi : faces) { Vec3 n = E.normal(fi); if (n.lengthSq() > 0) n = n.normalized(); for (int v : E.f[size_t(fi)].v) nrm[v] += n; }
    for (auto& [v, n] : nrm) if (n.lengthSq() > 0) E.pos[size_t(v)] += n.normalized() * offset;
    return ttDone(m, E, *src, QString(), 'f', faces, {}, false); }
  std::vector<std::pair<double, double>> prof; bool top = true, bottom = false;
  if (inset) { const int R = ttools.insSeg; for (int r = 1; r <= R; r++) prof.push_back({ttools.insDepth * r / R, offset * r / R}); }
  else {
    // (a negative offset: the extrude goes in — a pocket; the rounding as out, mirrored)
    const double sg = offset < 0 ? -1 : 1, h = std::abs(offset), b = std::min(ttools.bevBevel, h); const int R = ttools.bevSeg;
    if (b <= 0) for (int r = 1; r <= R; r++) prof.push_back({sg * h * r / R, 0.0});
    else { if (h - b > 1e-9) prof.push_back({sg * (h - b), 0.0}); for (int r = 1; r <= R; r++) { const double a = M_PI / 2 * r / R; prof.push_back({sg * (h - b + b * std::sin(a)), b * (1 - std::cos(a))}); } }
    top = ttools.bevTop; bottom = ttools.bevBottom;
  }
  ExtOut all;
  if (inset && !ttools.insGroups) {                               // (each polygon on its own)
    std::sort(faces.begin(), faces.end());
    for (int f : faces) { ExtOut o = extrudeRegion(E, {f}, prof, true, false); all.top.insert(all.top.end(), o.top.begin(), o.top.end()); all.side.insert(all.side.end(), o.side.begin(), o.side.end()); }
  } else {
    // (Inset, Scale the inside: the region's inner vertices drawn in with its rim — scaled about its middle as much as
    // the rim shrank — not left where they were)
    const std::set<int> S0(faces.begin(), faces.end()); std::set<int> rim0; for (auto& [a, b] : tedit::regionBorder(E, S0)) { rim0.insert(a); rim0.insert(b); }
    auto spread = [&](const std::set<int>& vs, Vec3& c) { c = Vec3(); for (int v : vs) c += E.pos[size_t(v)]; c = c * (1.0 / std::max<size_t>(1, vs.size())); double r = 0; for (int v : vs) r += E.pos[size_t(v)].distanceTo(c); return r / std::max<size_t>(1, vs.size()); };
    Vec3 c0; const double r0 = spread(rim0, c0);
    all = extrudeRegion(E, S0, prof, top, bottom, !inset && ttools.bevSmart);
    if (inset && ttools.insScale && r0 > 0 && !all.top.empty()) {
      const std::set<int> S1(all.top.begin(), all.top.end()); std::set<int> rim1; for (auto& [a, b] : tedit::regionBorder(E, S1)) { rim1.insert(a); rim1.insert(b); }
      Vec3 c1; const double s = spread(rim1, c1) / r0; std::set<int> inner; for (int f : S1) for (int v : E.f[size_t(f)].v) if (!rim1.count(v)) inner.insert(v);
      for (int v : inner) E.pos[size_t(v)] = c1 + (E.pos[size_t(v)] - c1) * s;
    }
  }
  std::vector<int> sel;
  if (inset) sel = all.top;
  else { if (ttools.bevSelTop) sel.insert(sel.end(), all.top.begin(), all.top.end()); if (ttools.bevSelSides) sel.insert(sel.end(), all.side.begin(), all.side.end()); if (ttools.bevSelBottom) sel.insert(sel.end(), all.bottom.begin(), all.bottom.end()); }
  const QString msg = inset ? tr("Inset: %1 polygon%2").arg(faces.size()).arg(faces.size() == 1 ? "" : "s") : tr("Extruded: %1 polygon%2").arg(faces.size()).arg(faces.size() == 1 ? "" : "s");
  if (!live) { tt_.lastOp = inset ? "inset" : "bevel"; }
  return ttDone(m, E, *src, live ? QString() : msg, 'f', sel, {}, !live);
}

// Bevel on edges: each vertex of the picked edges split into one per side of them (slid along its edge on that side by
// the offset — at most most of the way), the picked edges' polygons on each side taking their side's copy, a polygon
// between the two copies of each picked edge — a chamfer, two loops where one was
QString Viewport::bevelEdgesApply(int m, double offset) {
  Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  const bool live = bool(tt_.before); std::shared_ptr<PolyMesh> src = live ? tt_.before : baseSp(*it);
  auto T = topo::ttopo(src); std::vector<int> eids = live ? tt_.verts : std::vector<int>(tsel_.e.begin(), tsel_.e.end());
  MeshEd E = MeshEd::from(*src); std::set<tedit::EK> sel; std::set<int> verts;
  for (int e : eids) { if (e < 0 || e >= T->ne) return "!"; sel.insert(ek(T->ea[size_t(e)], T->eb[size_t(e)])); verts.insert(T->ea[size_t(e)]); verts.insert(T->eb[size_t(e)]); }
  if (sel.empty()) { ui::toast(tr("Select edges to bevel")); return "!"; }
  const double off = std::max(0.0, offset);
  // (the polygons round each vertex of the picked edges — found once: the mesh's every polygon looked through per
  // vertex, per new polygon and per its turn made a drag on a loop of a big mesh crawl)
  std::map<int, std::vector<int>> vf; for (size_t fi = 0; fi < E.f.size(); fi++) for (int v : E.f[fi].v) if (verts.count(v)) vf[v].push_back(int(fi));
  auto cornerOf = [&](int fi, int v) { const auto& V = E.f[size_t(fi)].v; for (size_t k = 0; k < V.size(); k++) if (V[k] == v) return int(k); return -1; };
  // (Proportional: each vertex slid the same share of its side edges' length — the offset is that share of their mean
  // length, so a narrow place gets a narrow chamfer, a wide one a wide one)
  double meanSide = 0; if (ttools.bevProp) { int k = 0;
    for (int v : verts) for (int fi : vf[v]) { const auto& V = E.f[size_t(fi)].v; const int c = cornerOf(fi, v), n = int(V.size());
      for (int w : {V[size_t((c + n - 1) % n)], V[size_t((c + 1) % n)]}) if (!sel.count(ek(v, w))) { meanSide += E.pos[size_t(w)].distanceTo(E.pos[size_t(v)]); k++; } }
    meanSide = k ? meanSide / k : 0; }
  std::map<int, std::map<int, int>> side;                        // (vertex → polygon → its copy on that polygon's side)
  for (int v : verts) {
    const auto fs = vf[v]; const size_t n = fs.size(); std::vector<int> par(n); std::iota(par.begin(), par.end(), 0);
    std::function<int(int)> find = [&](int x) { while (par[size_t(x)] != x) x = par[size_t(x)] = par[size_t(par[size_t(x)])]; return x; };
    auto nbs = [&](int fi) { const auto& V = E.f[size_t(fi)].v; const int c = cornerOf(fi, v), k = int(V.size()); return std::pair<int, int>{V[size_t((c + k - 1) % k)], V[size_t((c + 1) % k)]}; };
    for (size_t i = 0; i < n; i++) for (size_t j = i + 1; j < n; j++) { auto [p1, q1] = nbs(fs[i]); auto [p2, q2] = nbs(fs[j]);
      for (int w : {p1, q1}) if ((w == p2 || w == q2) && !sel.count(ek(v, w))) par[size_t(find(int(i)))] = find(int(j)); }   // (joined across an edge not picked)
    std::map<int, std::vector<int>> grp; for (size_t i = 0; i < n; i++) grp[find(int(i))].push_back(fs[i]);
    if (grp.size() < 2) continue;                                 // (the picked edges don't part the polygons round it — it stays)
    for (auto& [g, gf] : grp) {
      Vec3 dir; double lim = 1e300, len = 0; int cnt = 0; std::set<int> ws;
      for (int fi : gf) { auto [p, q] = nbs(fi); for (int w : {p, q}) if (!sel.count(ek(v, w))) ws.insert(w); }
      for (int w : ws) { const Vec3 d = E.pos[size_t(w)] - E.pos[size_t(v)]; const double l = d.length(); if (l <= 0) continue; dir += d * (1 / l); lim = std::min(lim, l * 0.9); len += l; cnt++; }
      if (!cnt) { Vec3 c; for (int fi : gf) c += E.center(fi); c = c * (1.0 / double(gf.size())); dir = c - E.pos[size_t(v)]; lim = dir.length() * 0.9; len = dir.length(); cnt = 1; }
      if (dir.lengthSq() > 0) dir = dir.normalized();
      const double want = ttools.bevProp && meanSide > 0 ? off / meanSide * (len / cnt) : off;
      const int nv = E.addVert(E.pos[size_t(v)] + dir * std::min(want, lim)); for (int fi : gf) side[v][fi] = nv;
    }
  }
  if (side.empty()) { ui::toast(tr("The picked edges part no polygons — pick an edge loop (or edges between polygons)")); return "!"; }
  for (auto& [v, byF] : side) for (auto& [fi, nv] : byF) for (int& x : E.f[size_t(fi)].v) if (x == v) x = nv;
  std::vector<std::pair<int, int>> selE; int made = 0;
  for (int e : eids) {
    const int a = T->ea[size_t(e)], b = T->eb[size_t(e)], f0 = T->ef0[size_t(e)], f1 = T->ef1[size_t(e)]; if (f0 < 0 || f1 < 0) continue;
    auto cp = [&](int v, int f) { auto s = side.find(v); if (s == side.end()) return v; auto q = s->second.find(f); return q == s->second.end() ? v : q->second; };
    const int a0 = cp(a, f0), b0 = cp(b, f0), a1 = cp(a, f1), b1 = cp(b, f1);
    std::vector<int> q{a0, b0, b1, a1}, u; for (int x : q) if (std::find(u.begin(), u.end(), x) == u.end()) u.push_back(x);
    if (u.size() < 3) continue;
    // (the chamfer's polygon: its attributes from the polygon beside it, turned to run against its neighbours' edges —
    // no look through the whole mesh)
    MeshEd::F N; const MeshEd::F& F0 = E.f[size_t(f0)]; N.v = u; N.g = F0.g; N.mat = F0.mat; N.sets = F0.sets; N.live = F0.live;
    if (E.hasUV) for (int x : u) { tedit::UVx w{}; for (int ff : {f0, f1}) { const int c = cornerOf(ff, x); if (c >= 0 && size_t(c) < E.f[size_t(ff)].uv.size()) { w = E.f[size_t(ff)].uv[size_t(c)]; break; } } N.uv.push_back(w); }
    if (E.hasN) N.n.assign(u.size(), {0, 0, 0});
    bool flip = false;
    for (size_t k = 0; k < u.size() && !flip; k++) { const int x = u[k], y = u[(k + 1) % u.size()];
      for (int ff : {f0, f1}) { const auto& V = E.f[size_t(ff)].v; for (size_t j = 0; j < V.size(); j++) if (V[j] == x && V[(j + 1) % V.size()] == y) flip = true; } }
    if (flip) { std::reverse(N.v.begin(), N.v.end()); std::reverse(N.uv.begin(), N.uv.end()); }
    E.f.push_back(std::move(N)); made++;
    if (a0 != b0) selE.push_back({a0, b0}); if (a1 != b1) selE.push_back({a1, b1});
  }
  if (!live) { tt_.lastSel = tsel_; tt_.lastOp = "bevel"; }
  return ttDone(m, E, *src, live ? QString() : tr("Bevel: %1 edge%2 — a loop each side").arg(made).arg(made == 1 ? "" : "s"), 'e', {}, selE, !live);
}

// Inset on vertices: a circle of so many sides round each picked vertex (its polygons opened round it; the circle a
// polygon of its own, or a hole)
QString Viewport::insetCircles() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it || tsel_.id != m || tsel_.v.empty()) { ui::toast(tr("Select vertices (circles round them) or polygons (inset)")); return "!"; }
  tt_.lastSel = tsel_;
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); std::vector<int> caps; int made = 0;
  for (int v : std::vector<int>(tsel_.v.begin(), tsel_.v.end())) {
    auto fs = E.facesOfVert(v); if (fs.size() < 2) continue;
    std::set<int> nb; for (int fi : fs) { const auto& V = E.f[size_t(fi)].v; const int c = E.corner(fi, v), n = int(V.size()); nb.insert(V[size_t((c + n - 1) % n)]); nb.insert(V[size_t((c + 1) % n)]); }
    bool rim = false; for (int w : nb) if (E.rimEdge(v, w)) rim = true; if (rim) continue;
    const Vec3 n = E.vertNormal(v); Vec3 u = std::abs(n.x) < 0.9 ? Vec3(1, 0, 0).cross(n).normalized() : Vec3(0, 1, 0).cross(n).normalized(); const Vec3 w2 = n.cross(u);
    double r = ttools.insCircleR; for (int w : nb) r = std::min(r, 0.9 * E.pos[size_t(v)].distanceTo(E.pos[size_t(w)]));
    std::map<int, double> ang;                                     // (the new points on the edges, on the circle)
    for (int w : nb) { const Vec3 d = E.pos[size_t(w)] - E.pos[size_t(v)]; const double L = d.length(); const int p = E.splitEdge(v, w, r / std::max(1e-12, L));
      Vec3 t = d - n * d.dot(n); t = t.lengthSq() > 0 ? t.normalized() : u; E.pos[size_t(p)] = E.pos[size_t(v)] + t * r; ang[p] = std::atan2(t.dot(w2), t.dot(u)); }
    std::vector<int> cyc; for (auto& [p, a] : ang) cyc.push_back(p); std::sort(cyc.begin(), cyc.end(), [&](int a, int b) { return ang[a] < ang[b]; });
    const int k = int(cyc.size()), extra = std::max(0, ttools.insCircleN - k);
    std::vector<std::vector<int>> gap(static_cast<size_t>(k)); int left = extra;
    std::vector<double> span(static_cast<size_t>(k)); for (int i = 0; i < k; i++) { double a0 = ang[cyc[size_t(i)]], a1 = ang[cyc[size_t((i + 1) % k)]]; double s = a1 - a0; if (s <= 0) s += 2 * M_PI; span[size_t(i)] = s; }
    for (int i = 0; i < k; i++) { int c = i == k - 1 ? left : int(std::lround(extra * span[size_t(i)] / (2 * M_PI))); c = std::clamp(c, 0, left); left -= c;
      for (int j = 1; j <= c; j++) { const double a = ang[cyc[size_t(i)]] + span[size_t(i)] * j / (c + 1); gap[size_t(i)].push_back(E.addVert(E.pos[size_t(v)] + (u * std::cos(a) + w2 * std::sin(a)) * r)); } }
    // (each polygon round v: v taken out, the circle's points between its two edge points put in)
    for (int fi : E.facesOfVert(v)) {
      auto& F = E.f[size_t(fi)]; const int c = E.corner(fi, v), nn = int(F.v.size()); const int a = F.v[size_t((c + nn - 1) % nn)], b = F.v[size_t((c + 1) % nn)];
      std::vector<int> ins; const auto ia = std::find(cyc.begin(), cyc.end(), a), ib = std::find(cyc.begin(), cyc.end(), b);
      if (ia != cyc.end() && ib != cyc.end()) { const int i = int(ia - cyc.begin()), j = int(ib - cyc.begin());
        if ((i + 1) % k == j) ins = gap[size_t(i)]; else if ((j + 1) % k == i) { ins = gap[size_t(j)]; std::reverse(ins.begin(), ins.end()); } }
      const tedit::UVx uvv = E.hasUV ? F.uv[size_t(c)] : tedit::UVx{};
      F.v.erase(F.v.begin() + c); if (E.hasUV) F.uv.erase(F.uv.begin() + c); if (size_t(c) < F.n.size()) F.n.erase(F.n.begin() + c);
      F.v.insert(F.v.begin() + c, ins.begin(), ins.end()); if (E.hasUV) F.uv.insert(F.uv.begin() + c, ins.size(), uvv); if (E.hasN) F.n.insert(F.n.begin() + std::min<long>(c, long(F.n.size())), ins.size(), std::array<float, 3>{0, 0, 0});
    }
    std::vector<int> ringV; for (int i = 0; i < k; i++) { ringV.push_back(cyc[size_t(i)]); ringV.insert(ringV.end(), gap[size_t(i)].begin(), gap[size_t(i)].end()); }
    if (!ttools.insCircleHole) caps.push_back(E.addFace(ringV));
    made++;
  }
  if (!made) { ui::toast(tr("No vertex inside the mesh among the selected (the rim's are skipped)")); return "!"; }
  tt_.lastOp = "circles";
  return ttDone(m, E, B, tr("%1 circle%2 of %3 sides").arg(made).arg(made == 1 ? "" : "s").arg(ttools.insCircleN), caps.empty() ? 0 : 'f', caps);
}

// ---------- Bridge ----------
QString Viewport::bridgeNow() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it || tsel_.id != m || topoMode == "object") { ui::toast(tr("Select two chains of edges (or two groups of polygons) to bridge")); return "!"; }
  tt_.lastSel = tsel_;
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); auto T = topo::ttopo(baseSp(*it));
  std::vector<tedit::EK> es;
  if (tKey() == 'f') { std::set<int> S(tsel_.f.begin(), tsel_.f.end()); for (auto& [a, b] : tedit::regionBorder(E, S)) es.push_back(ek(a, b));
    if (!ttools.brKeep) { std::vector<char> del(E.f.size(), 0); for (int f : S) del[size_t(f)] = 1; E.removeFaces(del); } }
  else if (tKey() == 'e') for (int e : tsel_.e) es.push_back(ek(T->ea[size_t(e)], T->eb[size_t(e)]));
  else { auto vs = tSelVerts(m); std::set<int> s(vs.begin(), vs.end()); for (int e = 0; e < T->ne; e++) if (s.count(T->ea[size_t(e)]) && s.count(T->eb[size_t(e)])) es.push_back(ek(T->ea[size_t(e)], T->eb[size_t(e)])); }
  auto ch = tedit::edgeChains(es);
  if (ch.size() != 2) { ui::toast(tr("Bridge: two chains of edges are needed (%1 now)").arg(ch.size())); return "!"; }
  auto c1 = ch[0].v, c2 = ch[1].v; const bool closed = ch[0].closed && ch[1].closed;
  if (c1.size() != c2.size()) return bridgeUneven(m, E, B, c1, c2, closed);
  const size_t n = c1.size();
  auto cost = [&](const std::vector<int>& b) { double s = 0; for (size_t i = 0; i < n; i++) s += E.pos[size_t(c1[i])].distanceTo(E.pos[size_t(b[i])]); return s; };
  std::vector<int> best = c2; double bc = 1e300;
  for (int dirn = 0; dirn < 2; dirn++) { std::vector<int> b = c2; if (dirn) std::reverse(b.begin(), b.end());
    for (size_t off = 0; off < (closed ? n : 1); off++) { std::vector<int> r(n); for (size_t i = 0; i < n; i++) r[i] = b[(i + off) % n]; const double c = cost(r); if (c < bc) { bc = c; best = r; } } }
  if (ttools.brReverse) { if (closed) std::reverse(best.begin() + 1, best.end()); else std::reverse(best.begin(), best.end()); }
  if (closed && ttools.brSpin) { std::vector<int> r(n); const long s = ((ttools.brSpin % long(n)) + long(n)) % long(n); for (size_t i = 0; i < n; i++) r[i] = best[(i + size_t(s)) % n]; best = r; }
  // (the tension: each end bulged out of its own polygons — along the rim's outward direction)
  auto outward = [&](int v, const std::vector<int>& chain) { Vec3 o; for (size_t i = 0; i < chain.size(); i++) { if (chain[i] != v) continue;
      for (int w : {i > 0 ? chain[i - 1] : (closed ? chain.back() : -1), i + 1 < chain.size() ? chain[i + 1] : (closed ? chain.front() : -1)}) { if (w < 0) continue;
        auto fs = E.facesOfEdge(v, w); if (fs.size() != 1) continue; Vec3 nf = E.normal(fs[0]); if (nf.lengthSq() > 0) nf = nf.normalized();
        const int c = E.corner(fs[0], v); const auto& V = E.f[size_t(fs[0])].v; const bool fwd = V[size_t((c + 1) % int(V.size()))] == w;
        Vec3 d = fwd ? E.pos[size_t(w)] - E.pos[size_t(v)] : E.pos[size_t(v)] - E.pos[size_t(w)]; o += d.cross(nf).normalized(); } }
    return o.lengthSq() > 0 ? o.normalized() : Vec3(); };
  const int S = ttools.brSeg; std::vector<std::vector<int>> rings{c1};
  for (int k = 1; k <= S; k++) { const double t = double(k) / (S + 1), h00 = 2 * t * t * t - 3 * t * t + 1, h10 = t * t * t - 2 * t * t + t, h01 = -2 * t * t * t + 3 * t * t, h11 = t * t * t - t * t;
    std::vector<int> r; for (size_t i = 0; i < n; i++) { const Vec3 p0 = E.pos[size_t(c1[i])], p1 = E.pos[size_t(best[i])]; const double d = p0.distanceTo(p1) * ttools.brTension;
      const Vec3 m0 = outward(c1[i], c1) * d, m1 = outward(best[i], best) * (-d); r.push_back(E.addVert(p0 * h00 + m0 * h10 + p1 * h01 + m1 * h11)); }
    rings.push_back(r); }
  rings.push_back(best);
  std::vector<int> made;
  for (size_t k = 0; k + 1 < rings.size(); k++) for (size_t i = 0; i + (closed ? 0 : 1) < n; i++) { const size_t j = (i + 1) % n;
    std::vector<int> q{rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i]}; std::set<int> chk(q.begin(), q.end());
    if (chk.size() == 4) made.push_back(E.addFace(q)); else if (chk.size() == 3) { std::vector<int> t3; for (int x : q) if (std::find(t3.begin(), t3.end(), x) == t3.end()) t3.push_back(x); made.push_back(E.addFace(t3)); } }
  if (made.empty()) return "!";
  tt_.lastOp = "bridge";
  tt_.symNew = true; return ttDone(m, E, B, tr("Bridge: %1 polygon%2").arg(made.size()).arg(made.size() == 1 ? "" : "s"), 'f', made);
}

// Bridge between chains of unlike counts: the rows to the first chain's count (each point where its share of the way
// along the other chain falls), then the last row stitched to the other chain — a quad where both step on together,
// a triangle where one of them runs ahead
QString Viewport::bridgeUneven(int m, MeshEd& E, const PolyMesh& B, std::vector<int> c1, std::vector<int> c2, bool closed) {
  auto param = [&](const std::vector<int>& c) { std::vector<double> u{0}; const size_t n = c.size(); for (size_t i = 1; i < n + (closed ? 1 : 0); i++) u.push_back(u.back() + E.pos[size_t(c[i - 1])].distanceTo(E.pos[size_t(c[i % n])]));
    const double L = std::max(1e-12, u.back()); for (auto& x : u) x /= L; return u; };   // (closed: one more — back at the start, 1)
  auto at = [&](const std::vector<int>& c, const std::vector<double>& u, double t) { const size_t n = c.size(); for (size_t i = 0; i + 1 < u.size(); i++) if (t <= u[i + 1] || i + 2 == u.size()) {
      const double s = u[i + 1] - u[i], k = s > 0 ? std::clamp((t - u[i]) / s, 0.0, 1.0) : 0; return E.pos[size_t(c[i % n])] + (E.pos[size_t(c[(i + 1) % n])] - E.pos[size_t(c[i % n])]) * k; }
    return E.pos[size_t(c.back())]; };
  // (the way round that matches best: direction, and — closed — the start nearest the first chain's)
  auto cost = [&](const std::vector<int>& b) { const auto u1 = param(c1), u2 = param(b); double s = 0; for (size_t i = 0; i < c1.size(); i++) s += E.pos[size_t(c1[i])].distanceTo(at(b, u2, u1[i])); return s; };
  std::vector<int> best = c2; double bc = 1e300;
  for (int dirn = 0; dirn < 2; dirn++) { std::vector<int> b = c2; if (dirn) std::reverse(b.begin(), b.end());
    if (closed) { size_t o = 0; double bd = 1e300; for (size_t i = 0; i < b.size(); i++) { const double d = E.pos[size_t(b[i])].distanceTo(E.pos[size_t(c1[0])]); if (d < bd) { bd = d; o = i; } } std::rotate(b.begin(), b.begin() + long(o), b.end()); }
    const double c = cost(b); if (c < bc) { bc = c; best = b; } }
  if (ttools.brReverse) { if (closed) std::reverse(best.begin() + 1, best.end()); else std::reverse(best.begin(), best.end()); }
  if (closed && ttools.brSpin) { const long n = long(best.size()), s = ((ttools.brSpin % n) + n) % n; std::rotate(best.begin(), best.begin() + s, best.end()); }
  const auto u1 = param(c1), u2 = param(best); const int S = ttools.brSeg;
  std::vector<int> last = c1;                                       // (the rows: as many points as the first chain)
  std::vector<int> made;
  for (int k = 1; k <= S; k++) { const double t = double(k) / (S + 1); std::vector<int> r;
    for (size_t i = 0; i < c1.size(); i++) r.push_back(E.addVert(E.pos[size_t(c1[i])] + (at(best, u2, u1[i]) - E.pos[size_t(c1[i])]) * t));
    for (size_t i = 0; i + (closed ? 0 : 1) < c1.size(); i++) { const size_t j = (i + 1) % c1.size(); made.push_back(E.addFace({last[i], last[j], r[j], r[i]})); }
    last = r; }
  // (the stitch: walk both by their share of the way)
  std::vector<int> A = last, Bv = best; std::vector<double> uA = u1, uB = u2; if (closed) { A.push_back(A[0]); Bv.push_back(Bv[0]); }
  const double stA = 1.0 / std::max<size_t>(1, A.size() - 1), stB = 1.0 / std::max<size_t>(1, Bv.size() - 1); int tris = 0;
  size_t i = 0, j = 0;
  while (i + 1 < A.size() || j + 1 < Bv.size()) {
    const bool endA = i + 1 >= A.size(), endB = j + 1 >= Bv.size();
    if (!endA && !endB && std::abs(uA[i + 1] - uB[j + 1]) < 0.5 * std::min(stA, stB)) { made.push_back(E.addFace({A[i], A[i + 1], Bv[j + 1], Bv[j]})); i++; j++; continue; }
    if (endB || (!endA && uA[i + 1] < uB[j + 1])) { made.push_back(E.addFace({A[i], A[i + 1], Bv[j]})); i++; }
    else { made.push_back(E.addFace({A[i], Bv[j + 1], Bv[j]})); j++; }
    tris++;
  }
  if (made.empty()) return "!";
  tt_.lastOp = "bridge";
  tt_.symNew = true; return ttDone(m, E, B, tr("Bridge: %1 polygon%2 — %3 triangle%4 where the chains differ (%5 and %6 vertices)").arg(made.size()).arg(made.size() == 1 ? "" : "s").arg(tris).arg(tris == 1 ? "" : "s").arg(c1.size()).arg(c2.size()), 'f', made);
}

// a setting of the tool changed: its last edit undone and made again with it (only while it is the last step)
QString Viewport::modelReapply() {
  const QString op = tt_.lastOp; const int m = tt_.lastM; Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (op.isEmpty() || !it || baseSp(*it) != tt_.lastAfter) return "!";
  const QString want = op == "loop" ? "tloop" : op == "plane" ? "tplane" : op == "bevel" ? "tbevel" : op == "inset" || op == "circles" ? "tinset" : op == "bridge" ? "tbridge" : op == "fill" ? "tfill" : op == "nsmooth" ? "tnormals" : QString();
  if (want != tool_ && !(op == "loop" && tool_ == "tpen")) return "!";
  const QJsonObject a = tt_.lastArgs;
  if (op == "fill" && (tsel_.id != m || tsel_.f != tt_.fillSelAfter)) return "!";   // (the patch: re-made only while it is still the selection)
  if (op == "nsmooth" && (tsel_.id != m || tsel_.f != tt_.lastSel.f || tsel_.e != tt_.lastSel.e || tsel_.v != tt_.lastSel.v)) return "!";   // (the normals: only while those polygons are still the selection — another one picked since: the new mode waits for Average)
  undo();
  it = scene_->item(m); if (!it) return "!";
  tt_.before.reset();
  { const int ver = tsel_.ver + 1; tsel_ = tt_.lastSel; tsel_.ver = ver; tsel_.B = baseOf(*it); }   // (what the edit worked on — selected again)
  if (op == "loop") { auto T = topo::ttopo(baseSp(*it)); const int e = T->edgeOf(a.value("a").toInt(), a.value("b").toInt()); if (e < 0) return "!";
    tt_.lt = ttools.loopPct; return loopApply(m, e, ttools.loopPct, a.value("sel").toBool(), ttools.loopCuts, ttools.loopParallel); }
  if (op == "plane") return planeApply(m, v3(a.value("o")), v3(a.value("n")), v3(a.value("along")));
  if (op == "bevel") return bevelApply(m, ttools.bevOffset, false);
  if (op == "inset") return bevelApply(m, ttools.insOffset, true);
  if (op == "circles") return insetCircles();
  if (op == "bridge") return bridgeNow();
  if (op == "nsmooth") return tNormalsSmooth();
  if (op == "nback") return tNormalsFromBack();
  if (op == "fill") {
    tt_.fillLoops.clear(); for (const QJsonValue& L : a.value("loops").toArray()) { std::vector<int> l; for (const QJsonValue& v : L.toArray()) l.push_back(v.toInt()); tt_.fillLoops.push_back(l); }
    tt_.fillOff = a.value("off").toInt(); tt_.fillB = baseOf(*it); return fillApply(); }
  return "!";
}

}  // namespace hm
