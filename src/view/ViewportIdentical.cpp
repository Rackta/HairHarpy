// Mirror → Replace: the selected polygons mirrored onto the other side in place of what was there (the region the
// selection's border bounds there — found by the border's mirror on the mesh), welded along it. Apply To Identical: the
// selected retopology of one part of a high-poly copied onto every topologically identical part (the same mesh's
// pieces, other meshes of the same topology), each by the affine map between the parts. Both carry the UVs, polygroups,
// materials, UV seams, creases and hard edges.
#include "scene/MeshMods.h"
#include "scene/TopoEdit.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

namespace hm {
using tedit::MeshEd;
using tedit::ek;

namespace {
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
const PolyMesh* baseOf(const Item& m) { return baseSp(m).get(); }
// a grid of points for "the one within r of here"
struct PtGrid {
  double r; std::unordered_map<long long, std::vector<int>> g;
  explicit PtGrid(double r_) : r(std::max(r_, 1e-12)) {}
  long long key(const Vec3& p, int dx, int dy, int dz) const { const long long x = (long long)std::floor(p.x / r) + dx, y = (long long)std::floor(p.y / r) + dy, z = (long long)std::floor(p.z / r) + dz; return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); }
  void add(const Vec3& p, int i) { g[key(p, 0, 0, 0)].push_back(i); }
  // the nearest within r that ok() lets through (−1)
  template <class P, class Ok> int near(const Vec3& q, const P& pos, const Ok& ok) const {
    int best = -1; double bd = r;
    for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) { auto f = g.find(key(q, dx, dy, dz)); if (f == g.end()) continue;
      for (int i : f->second) { if (!ok(i)) continue; const double d = pos(i).distanceTo(q); if (d <= bd) { bd = d; best = i; } } }
    return best; }
  template <class F> void each(const Vec3& q, const F& f) const {   // (all in the cells round q)
    for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) if (auto c = g.find(key(q, dx, dy, dz)); c != g.end()) for (int i : c->second) f(i); }
};
// the point of triangle abc nearest p
Vec3 nearOnTri(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
  const Vec3 ab = b - a, ac = c - a, ap = p - a; const double d1 = ab.dot(ap), d2 = ac.dot(ap); if (d1 <= 0 && d2 <= 0) return a;
  const Vec3 bp = p - b; const double d3 = ab.dot(bp), d4 = ac.dot(bp); if (d3 >= 0 && d4 <= d3) return b;
  const double vc = d1 * d4 - d3 * d2; if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
  const Vec3 cp = p - c; const double d5 = ab.dot(cp), d6 = ac.dot(cp); if (d6 >= 0 && d5 <= d6) return c;
  const double vb = d5 * d2 - d1 * d6; if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4; if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double den = 1.0 / (va + vb + vc); return a + ab * (vb * den) + ac * (vc * den);
}
double meanEdge(const MeshEd& E) {
  double s = 0; int n = 0; for (auto& F : E.f) for (size_t k = 0; k < F.v.size(); k++) { s += E.pos[size_t(F.v[k])].distanceTo(E.pos[size_t(F.v[(k + 1) % F.v.size()])]); if (++n > 40000) return s / n; }
  return n ? s / n : 1;
}
// the marks of the copied polygons' edges onto the copy's (map: old vertex → new)
void copyMarks(MeshEd& E, const std::vector<int>& faces, const std::map<int, int>& map) {
  std::set<tedit::EK> es; for (int f : faces) { const auto& v = E.f[size_t(f)].v; for (size_t k = 0; k < v.size(); k++) es.insert(ek(v[k], v[(k + 1) % v.size()])); }
  for (const auto& e : es) { auto a = map.find(e.first), b = map.find(e.second); if (a == map.end() || b == map.end() || a->second == b->second) continue; const auto n = ek(a->second, b->second);
    if (E.hard.count(e)) E.hard.insert(n); if (E.seam.count(e)) E.seam.insert(n); if (auto c = E.crease.find(e); c != E.crease.end()) E.crease[n] = c->second; }
}
// a corner's own normal turned (mirrored) as the copy is — left (0,0,0), none of its own: stays so
std::array<float, 3> turnN(const std::array<float, 3>& q, const Affine& T) {
  if (q[0] == 0 && q[1] == 0 && q[2] == 0) return q; const Vec3 n = T.lin(Vec3(q[0], q[1], q[2])).normalized(); return {float(n.x), float(n.y), float(n.z)};
}
// polygons `faces` copied by T (local → local), each vertex onto map[v] if there, else a new one (or one already within
// eps: welded) → the copies' indices
std::vector<int> copyFaces(MeshEd& E, const std::vector<int>& faces, const Affine& T, std::map<int, int>& map, PtGrid* weld, bool normals = true) {
  const bool flip = T.det() < 0; std::set<int> vs; for (int f : faces) for (int v : E.f[size_t(f)].v) vs.insert(v);
  for (int v : vs) { if (map.count(v)) continue; const Vec3 w = T.apply(E.pos[size_t(v)]); int hit = -1;
    if (weld) hit = weld->near(w, [&](int i) { return E.pos[size_t(i)]; }, [&](int i) { return !vs.count(i); });
    if (hit < 0) { hit = E.addVert(w); if (weld) weld->add(w, hit); }
    map[v] = hit; }
  std::vector<int> made;
  const size_t n0 = E.f.size();
  for (size_t i = 0; i < faces.size(); i++) { MeshEd::F F = E.f[size_t(faces[i])]; for (int& v : F.v) v = map[v]; for (auto& q : F.n) q = normals ? turnN(q, T) : std::array<float, 3>{0, 0, 0};   // (the file's own normals carried over, turned with it — none (0,0,0): made from the shape)
    if (flip) { std::reverse(F.v.begin(), F.v.end()); std::reverse(F.uv.begin(), F.uv.end()); std::reverse(F.n.begin(), F.n.end()); }
    std::set<int> chk(F.v.begin(), F.v.end()); if (chk.size() < 3 || chk.size() != F.v.size()) continue; E.f.push_back(F); made.push_back(int(E.f.size()) - 1); }
  (void)n0; copyMarks(E, faces, map); return made;
}
// the least-squares affine map carrying the points A onto B
Affine fitAffine(const std::vector<Vec3>& A, const std::vector<Vec3>& B) {
  Affine r; const size_t n = std::min(A.size(), B.size()); if (!n) return r;
  Vec3 ca, cb; for (size_t i = 0; i < n; i++) { ca += A[i]; cb += B[i]; } ca = ca * (1.0 / n); cb = cb * (1.0 / n);
  double Sxx[3][3] = {}, Syx[3][3] = {};
  for (size_t i = 0; i < n; i++) { const Vec3 x = A[i] - ca, y = B[i] - cb; const double xv[3] = {x.x, x.y, x.z}, yv[3] = {y.x, y.y, y.z};
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) { Sxx[a][b] += xv[a] * xv[b]; Syx[a][b] += yv[a] * xv[b]; } }
  const double tr = Sxx[0][0] + Sxx[1][1] + Sxx[2][2]; for (int a = 0; a < 3; a++) Sxx[a][a] += (tr ? tr : 1) * 1e-9;
  Affine S; for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) S.L[a][b] = Sxx[a][b]; const Affine Si = S.inverse();
  for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) { double s = 0; for (int k = 0; k < 3; k++) s += Syx[a][k] * Si.L[k][b]; r.L[a][b] = s; }
  r.t = cb - r.lin(ca); return r;
}
}  // namespace

// ---------- Mirror → Replace ----------
QString Viewport::tMirrorReplace() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || topoHeavy(m)) { ui::toast(tr("Select a mesh")); return "!"; }
  if (topoMode == "object") { ui::toast(tr("Replace: select the polygons to carry over (a component mode)")); return "!"; }
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); const auto fs = tSelFaces(m);
  if (fs.empty()) { ui::toast(tr("Replace: select the polygons to carry over")); return "!"; }
  const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse(); const int a = std::clamp(ttools.mirAxis, 0, 2);
  Affine F; F.L[a][a] = -1; const Affine Gm = mirFrame(m); F = Affine::mul(Gm, Affine::mul(F, Gm.inverse()));   // (Local: about the object's gizmo)
  const Affine T = Affine::mul(Mi, Affine::mul(F, M));   // (local → mirrored local)
  const std::set<int> sel(fs.begin(), fs.end()); std::set<int> selV; for (int f : fs) for (int v : E.f[size_t(f)].v) selV.insert(v);
  const double edge = meanEdge(E), tol = edge * 0.35;
  // the selection's border, its mirror found on the mesh
  // (only where it meets the rest of the mesh: an open edge of it — a hole, the rim — bounds nothing over there)
  std::vector<std::pair<int, int>> border; for (auto& [x, y] : tedit::regionBorder(E, sel)) { bool other = false; for (int g : E.facesOfEdge(x, y)) if (!sel.count(g)) other = true; if (other) border.push_back({x, y}); }
  if (border.empty()) { ui::toast(tr("Replace: the selection does not border the rest of the mesh — nothing to put it in place of")); return "!"; }
  std::set<int> bv; for (auto& [x, y] : border) { bv.insert(x); bv.insert(y); }
  std::set<int> used; for (auto& Fc : E.f) used.insert(Fc.v.begin(), Fc.v.end());
  PtGrid G(tol); for (int v : used) G.add(E.pos[size_t(v)], v);
  std::map<int, int> image; int matched = 0;
  for (int v : bv) { const Vec3 w = T.apply(E.pos[size_t(v)]); const int u = G.near(w, [&](int i) { return E.pos[size_t(i)]; }, [&](int i) { return i == v || !selV.count(i); });
    if (u >= 0) { image[v] = u; matched++; } }
  if (bv.empty() || matched < int(bv.size()) * 6 / 10) {
    ui::toast(tr("Replace: the other side does not match the selection's border (%1 of %2 border vertices found mirrored) — the mesh is not symmetric there").arg(matched).arg(bv.size())); return "!"; }
  // the wall there: each border edge's ends' images joined by the shortest way along the mesh's own edges (the other
  // side's topology need not be the selection's mirrored); an end with no image found — the vertex nearest its mirrored
  // place (within an edge and a half, else the nearest at all)
  PtGrid G2(edge * 1.5); for (int v : used) if (!selV.count(v)) G2.add(E.pos[size_t(v)], v);
  auto imgOf = [&](int v) { auto i = image.find(v); if (i != image.end()) return i->second; const Vec3 q = T.apply(E.pos[size_t(v)]); int b = G2.near(q, [&](int x) { return E.pos[size_t(x)]; }, [](int) { return true; });
    if (b < 0) { double bd = 1e300; for (int x : used) if (!selV.count(x)) if (const double d = E.pos[size_t(x)].distanceTo(q); d < bd) { bd = d; b = x; } }   // (none that near: the nearest one at all)
    return b; };
  std::map<int, std::vector<int>> nbr; for (auto& Fc : E.f) for (size_t k = 0; k < Fc.v.size(); k++) { const int x = Fc.v[k], y = Fc.v[(k + 1) % Fc.v.size()]; nbr[x].push_back(y); nbr[y].push_back(x); }
  auto way = [&](int s, int t) { std::vector<int> out; if (s == t) return out; std::map<int, double> dist{{s, 0}}; std::map<int, int> from; std::set<std::pair<double, int>> q{{0.0, s}}; int pops = 0;
    while (!q.empty() && pops++ < 400) { const auto [d, x] = *q.begin(); q.erase(q.begin()); if (x == t || d > edge * 12) break;
      for (int y : nbr[x]) { if (selV.count(y) && y != t) continue; const double nd = d + E.pos[size_t(x)].distanceTo(E.pos[size_t(y)]); auto f = dist.find(y);
        if (f == dist.end() || nd < f->second) { if (f != dist.end()) q.erase({f->second, y}); dist[y] = nd; from[y] = x; q.insert({nd, y}); } } }
    if (!from.count(t)) return out; for (int x = t; x != s; x = from[x]) out.push_back(x); out.push_back(s); return out; };
  std::set<tedit::EK> wall; int gaps = 0; for (auto& [x, y] : border) { const int u = imgOf(x), w = imgOf(y); if (u < 0 || w < 0) { gaps++; continue; } const auto p = way(u, w); if (u != w && p.empty()) gaps++; for (size_t k = 0; k + 1 < p.size(); k++) wall.insert(ek(p[k], p[k + 1])); }
  lastReplaceInfo_ = QString("border %1 edges, %2 vertices, matched %3, wall %4 edges, gaps %5").arg(border.size()).arg(bv.size()).arg(matched).arg(wall.size()).arg(gaps);
  // the region there: from the polygons under the selection's mirrored middles, up to the mirrored border
  std::vector<Vec3> cen(E.f.size()); PtGrid FG(edge * 1.5); for (size_t f = 0; f < E.f.size(); f++) { cen[f] = E.center(int(f)); FG.add(cen[f], int(f)); }
  std::map<tedit::EK, std::vector<int>> byEdge; for (size_t f = 0; f < E.f.size(); f++) { const auto& v = E.f[f].v; for (size_t k = 0; k < v.size(); k++) byEdge[ek(v[k], v[(k + 1) % v.size()])].push_back(int(f)); }
  std::vector<char> del(E.f.size(), 0); std::vector<int> st;
  // (only near the selection's mirror — within two edges of one of its polygons: where the wall leaves a way out, at an
  // open end of the border, the region stops there all the same)
  PtGrid MG(edge * 2); for (int f : fs) MG.add(T.apply(cen[size_t(f)]), f); std::vector<Vec3> mc(E.f.size()); for (int f : fs) mc[size_t(f)] = T.apply(cen[size_t(f)]);
  auto nearMirror = [&](int g) { return MG.near(cen[size_t(g)], [&](int i) { return mc[size_t(i)]; }, [](int) { return true; }) >= 0; };
  for (int f : fs) { const int g = FG.near(T.apply(cen[size_t(f)]), [&](int i) { return cen[size_t(i)]; }, [&](int i) { return !sel.count(i); }); if (g >= 0 && !del[size_t(g)]) { del[size_t(g)] = 1; st.push_back(g); } }
  size_t taken = st.size(); bool leak = false;
  while (!st.empty()) { const int f = st.back(); st.pop_back(); const auto& v = E.f[size_t(f)].v;
    for (size_t k = 0; k < v.size(); k++) { const auto e = ek(v[k], v[(k + 1) % v.size()]); if (wall.count(e)) continue;
      for (int g : byEdge[e]) if (!del[size_t(g)] && !sel.count(g) && nearMirror(g)) { del[size_t(g)] = 1; st.push_back(g); if (++taken > fs.size() * 4 + 64) leak = true; } }
    if (leak) break; }
  if (leak) { ui::toast(tr("Replace: the mirrored border does not close a region on the other side — nothing replaced (Mirror without Replace adds a copy)")); return "!"; }
  // (kept out: what the region took that is not the mirrored copy's place — a gap in the wall, another layer of the cloth
  // reached through the seeds: a polygon goes when its middle lies on the mirrored copy (within 0.3 of an edge), or when
  // every polygon round it goes — a bit left over inside the region)
  // (the distance to the mirrored copy and the copy's normal there: a polygon facing the way the copy does goes within
  // an edge and a half — the other side need not be the exact mirror, nor its polygons the selection's sizes; one facing
  // away — the cloth's other layer — only when it lies right on the copy)
  { const bool flipT = T.det() < 0;
    auto d3 = [&](const Vec3& p, Vec3* nOut) { double bd = 1e300;
      MG.each(p, [&](int f) { const auto& V = E.f[size_t(f)].v; const Vec3 a = T.apply(E.pos[size_t(V[0])]);
        for (size_t k = 1; k + 1 < V.size(); k++) { const Vec3 b = T.apply(E.pos[size_t(V[k])]), c = T.apply(E.pos[size_t(V[k + 1])]); const double d = nearOnTri(p, a, b, c).distanceTo(p);
          if (d < bd) { bd = d; if (nOut) { const Vec3 u = b - a, w = c - a; *nOut = Vec3(u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x) * (flipT ? -1.0 : 1.0); } } } });
      return bd; };
    auto faceN = [&](size_t g) { const auto& V = E.f[g].v; Vec3 n; for (size_t k = 0; k < V.size(); k++) { const Vec3& a = E.pos[size_t(V[k])]; const Vec3& b = E.pos[size_t(V[(k + 1) % V.size()])]; n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)); } return n; };
    auto onCopy = [&](size_t g) { Vec3 nc; const double d = d3(cen[g], &nc); if (d <= edge * 0.3) return true; if (d > edge * 1.5) return false;
      const Vec3 ng = faceN(g); const double l = std::sqrt(ng.lengthSq() * nc.lengthSq()); return l > 0 && ng.dot(nc) / l > 0.3; };
    std::vector<char> reach = del; int kept = 0;
    // (and only on the other side of the plane: near the middle the selection's mirror falls on its own side too)
    double sc = 0; for (int f : fs) { const Vec3 w = M.apply(cen[size_t(f)]); sc += a == 0 ? w.x : a == 1 ? w.y : w.z; }
    for (size_t g = 0; g < del.size(); g++) if (del[g]) { const Vec3 w = M.apply(cen[g]); const double s = a == 0 ? w.x : a == 1 ? w.y : w.z; if (s * sc >= 0) { del[g] = 0; kept++; } }
    for (size_t g = 0; g < del.size(); g++) if (del[g] && !onCopy(g)) { del[g] = 0; kept++; }
    for (size_t g = 0; g < del.size(); g++) { if (!reach[g] || del[g]) continue; const auto& v = E.f[g].v; bool inside = true;
      for (size_t k = 0; k < v.size() && inside; k++) for (int h : byEdge[ek(v[k], v[(k + 1) % v.size()])]) if (h != int(g) && !del[size_t(h)] && !sel.count(h)) { inside = false; break; }
      if (inside) { del[g] = 1; kept--; } }
    lastReplaceInfo_ += QString(", kept %1 off the mirrored place").arg(kept); }
  // the copy: its border onto the vertices found there, a vertex on the plane kept as it is
  std::map<int, int> map; for (auto& [v, u] : image) map[v] = u;
  const auto made0 = copyFaces(E, fs, T, map, nullptr, ttools.mirNormals);
  del.resize(E.f.size(), 0);
  const int removed = int(std::count(del.begin(), del.end(), 1));
  E.removeFaces(del);
  std::vector<int> made; { std::vector<int> shift(del.size(), 0); int s = 0; for (size_t i = 0; i < del.size(); i++) { shift[i] = s; s += del[i]; } for (int f : made0) made.push_back(f - shift[size_t(f)]); }
  // (the copy's border where no vertex was found mirrored there: left open beside the mesh's own open edge — a sliver
  // of a gap; each such vertex welded onto the nearest open vertex of the mesh's own within 0.6 of an edge, one each)
  int stitched = 0;
  { std::map<tedit::EK, int> ec; for (const auto& Fc : E.f) for (size_t k = 0; k < Fc.v.size(); k++) ec[ek(Fc.v[k], Fc.v[(k + 1) % Fc.v.size()])]++;
    std::set<int> openV; for (auto& [e, c] : ec) if (c == 1) { openV.insert(e.first); openV.insert(e.second); }
    std::set<int> copyV; for (int f : made) for (int v : E.f[size_t(f)].v) copyV.insert(v);
    std::set<int> fixedV; for (auto& [v, u] : image) fixedV.insert(u);
    PtGrid OG(edge * 0.6); for (int v : openV) if (!copyV.count(v)) OG.add(E.pos[size_t(v)], v);
    std::vector<std::tuple<double, int, int>> cand;
    for (int v : openV) { if (!copyV.count(v) || fixedV.count(v)) continue;
      const int u = OG.near(E.pos[size_t(v)], [&](int i) { return E.pos[size_t(i)]; }, [](int) { return true; }); if (u >= 0) cand.push_back({E.pos[size_t(v)].distanceTo(E.pos[size_t(u)]), v, u}); }
    std::sort(cand.begin(), cand.end()); std::map<int, int> to; std::set<int> taken;
    for (auto& [d, v, u] : cand) if (!taken.count(u)) { taken.insert(u); to[v] = u; }
    if (!to.empty()) { std::vector<char> gone(E.f.size(), 0);
      for (size_t fi = 0; fi < E.f.size(); fi++) { auto& Fc = E.f[fi]; MeshEd::F N = Fc; N.v.clear(); N.uv.clear(); N.n.clear();
        for (size_t k = 0; k < Fc.v.size(); k++) { auto t = to.find(Fc.v[k]); const int v = t != to.end() ? t->second : Fc.v[k]; if (!N.v.empty() && N.v.back() == v) continue;
          N.v.push_back(v); if (E.hasUV && k < Fc.uv.size()) N.uv.push_back(Fc.uv[k]); if (E.hasN && k < Fc.n.size()) N.n.push_back(Fc.n[k]); }
        while (N.v.size() > 1 && N.v.front() == N.v.back()) { N.v.pop_back(); if (!N.uv.empty()) N.uv.pop_back(); if (!N.n.empty()) N.n.pop_back(); }
        const std::set<int> S(N.v.begin(), N.v.end()); if (N.v.size() < 3 || S.size() != N.v.size()) gone[fi] = 1; else Fc = N; }
      std::vector<int> shift(gone.size(), 0); int s = 0; for (size_t i = 0; i < gone.size(); i++) { shift[i] = s; s += gone[i]; }
      std::vector<int> m2; for (int f : made) if (!gone[size_t(f)]) m2.push_back(f - shift[size_t(f)]); made = m2;
      E.removeFaces(gone); stitched = int(to.size()); } }
  lastReplaceInfo_ += QString(", stitched %1").arg(stitched);
  return ttDone(m, E, B, tr("Mirror → Replace: %1 polygons across %2 = 0 in place of %3, welded along the border (%4 vertices)").arg(made.size()).arg(QString("XYZ")[a]).arg(removed).arg(matched), 'f', made);
}

// ---------- Apply To Identical ----------
QString Viewport::tApplyIdentical() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || topoHeavy(m)) { ui::toast(tr("Select the retopology mesh (its polygons on one part)")); return "!"; }
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B);
  auto fs = topoMode == "object" ? std::vector<int>{} : tSelFaces(m); if (fs.empty()) { fs.resize(E.f.size()); std::iota(fs.begin(), fs.end(), 0); }
  if (fs.empty()) { ui::toast(tr("Nothing to copy")); return "!"; }
  const Affine Mt = meshmods::meshMatrix(*scene_, *it), Mti = Mt.inverse();
  std::vector<int> sv; { std::set<int> s; for (int f : fs) for (int v : E.f[size_t(f)].v) s.insert(v); sv.assign(s.begin(), s.end()); }
  std::vector<Vec3> probe; for (size_t i = 0; i < sv.size(); i += std::max<size_t>(1, sv.size() / 48)) probe.push_back(Mt.apply(E.pos[size_t(sv[i])]));
  // the high-poly under it: the visible mesh whose vertices lie nearest, the piece most of them are on
  int ref = -1, piece = -1; double bestD = 1e300;
  for (auto& [id, x] : scene_->items) {
    if (id == m || x.type != ItemType::IMesh || !x.poly || !scene_->isVisible(x)) continue;
    auto Bp = baseSp(x); const auto& P = Bp->pos; if (P.empty()) continue; const Affine Mr = meshmods::meshMatrix(*scene_, x), Mri = Mr.inverse();
    double sum = 0; std::vector<int> nearV;
    for (const Vec3& w : probe) { const Vec3 q = Mri.apply(w); double bd = 1e300; int bi = -1; for (size_t v = 0; v < P.size(); v++) { const double d = (P[v] - q).lengthSq(); if (d < bd) { bd = d; bi = int(v); } }
      sum += Mr.apply(P[size_t(bi)]).distanceTo(w); nearV.push_back(bi); }
    sum /= probe.size(); if (sum >= bestD) continue;
    auto I = topo::isoClasses(Bp); std::map<int, int> votes; for (int v : nearV) if (size_t(v) < I->P.pv.size() && I->P.pv[size_t(v)] >= 0) votes[I->P.pv[size_t(v)]]++;
    int bp = -1, bn = 0; for (auto& [p, n] : votes) if (n > bn) { bn = n; bp = p; }
    bestD = sum; ref = id; piece = bp; }
  if (ref < 0 || piece < 0) { ui::toast(tr("Apply To Identical: no high-poly under the selected polygons")); return "!"; }
  const Item& R = *scene_->item(ref); auto Br = baseSp(R); auto I = topo::isoClasses(Br); const Affine Mr = meshmods::meshMatrix(*scene_, R), Mri = Mr.inverse();
  auto pieceMap = [&](int p) { Affine A; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) A.L[i][j] = I->M[size_t(p)][size_t(i * 3 + j)]; A.t = Vec3(I->T[size_t(p)][0], I->T[size_t(p)][1], I->T[size_t(p)][2]); return A; };   // (its class's piece → it, the file's space)
  // the selected polygons by the part of the high-poly each lies over (a grid of its vertices: the nearest one's part)
  std::map<int, std::vector<int>> groups;
  { const auto& P = Br->pos; Box3 bb = Br->bounds(); const double cs = std::max(1e-9, bb.size().length() / 160); std::unordered_map<long long, std::vector<int>> grid;
    auto key = [&](long long x, long long y, long long z) { return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
    for (size_t v = 0; v < P.size(); v++) if (I->P.pv[v] >= 0) grid[key((long long)std::floor(P[v].x / cs), (long long)std::floor(P[v].y / cs), (long long)std::floor(P[v].z / cs))].push_back(int(v));
    for (int f : fs) { const Vec3 q = Mri.apply(Mt.apply(E.center(f))); const long long cx = (long long)std::floor(q.x / cs), cy = (long long)std::floor(q.y / cs), cz = (long long)std::floor(q.z / cs); int bi = -1; double bd = 1e300;
      for (int r = 0; r < 160 && (bi < 0 || bd > (r - 1) * cs); r++) for (long long x = cx - r; x <= cx + r; x++) for (long long y = cy - r; y <= cy + r; y++) for (long long z = cz - r; z <= cz + r; z++) {
        if (std::max({std::llabs(x - cx), std::llabs(y - cy), std::llabs(z - cz)}) != r) continue; auto g = grid.find(key(x, y, z)); if (g == grid.end()) continue;
        for (int v : g->second) { const double d = P[size_t(v)].distanceTo(q); if (d < bd) { bd = d; bi = v; } } }
      groups[bi >= 0 ? I->P.pv[size_t(bi)] : piece].push_back(f); } }
  // (other meshes of the very same topology — copies of this one: each part of them too)
  std::vector<std::pair<int, Affine>> twins{{ref, Affine()}};
  for (auto& [id, x] : scene_->items) {
    if (id == ref || id == m || x.type != ItemType::IMesh || !x.poly || !scene_->isVisible(x)) continue; auto Bx = baseSp(x);
    if (Bx->pos.size() != Br->pos.size() || Bx->faceVerts != Br->faceVerts) continue;
    const Affine Mx = meshmods::meshMatrix(*scene_, x); std::vector<Vec3> A, C; const size_t step = std::max<size_t>(1, Br->pos.size() / 20000);
    for (size_t v = 0; v < Br->pos.size(); v += step) { A.push_back(Mr.apply(Br->pos[v])); C.push_back(Mx.apply(Bx->pos[v])); }
    twins.push_back({id, fitAffine(A, C)}); }
  const double edge = meanEdge(E); PtGrid weld(edge * 0.25); { std::set<int> used; for (auto& Fc : E.f) used.insert(Fc.v.begin(), Fc.v.end()); for (int v : used) weld.add(E.pos[size_t(v)], v); }
  std::vector<int> made; int parts = 0, skipped = 0;
  for (auto& [gp, gfs] : groups) {
    const Affine Api = pieceMap(gp).inverse(); std::vector<int> gv; { std::set<int> s; for (int f : gfs) for (int v : E.f[size_t(f)].v) s.insert(v); gv.assign(s.begin(), s.end()); }
    for (auto& [tid, W] : twins) for (int q = 0; q < I->P.np; q++) {
      if (I->cls[size_t(q)] != I->cls[size_t(gp)] || (tid == ref && q == gp)) continue;
      // local → world → that piece's place (in the file: back to its class's piece, on to q) → its mesh's place → local
      const Affine Aq = Affine::mul(pieceMap(q), Api); const Affine T = Affine::mul(Mti, Affine::mul(W, Affine::mul(Mr, Affine::mul(Aq, Affine::mul(Mri, Mt)))));
      // (a part done already: most of the copy's vertices on existing ones)
      int on = 0; for (int v : gv) if (weld.near(T.apply(E.pos[size_t(v)]), [&](int i) { return E.pos[size_t(i)]; }, [](int) { return true; }) >= 0) on++;
      if (on * 2 > int(gv.size())) { skipped++; continue; }
      std::map<int, int> map; auto got = copyFaces(E, gfs, T, map, &weld); if (got.empty()) continue;
      made.insert(made.end(), got.begin(), got.end()); parts++; } }
  if (!parts) { ui::toast(skipped ? tr("Apply To Identical: every identical part has this topology already (%1)").arg(skipped) : tr("Apply To Identical: no other part of the high-poly is topologically the same")); return "!"; }
  tt_.symNew = false;
  return ttDone(m, E, B, tr("Apply To Identical: %1 polygons onto %2 identical part%3%4").arg(made.size()).arg(parts).arg(parts == 1 ? "" : "s").arg(skipped ? tr(" (%1 done already)").arg(skipped) : QString()), 'f', made);
}

}  // namespace hm

namespace hm {
// (tests) a symmetric grid across X = 0 with a seam, a hard edge and a crease on its − side; three identical boxes
// turned about (a "chain") with a coarse retopology box round the first
QString Viewport::testSymGrid() {
  PolyMesh d; const int nx = 8, ny = 4;
  for (int j = 0; j <= ny; j++) for (int i = 0; i <= nx; i++) d.pos.push_back(Vec3(-6 + 12.0 * i / nx, 124 + 12.0 * j / ny, 16));
  for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++) { int f[4] = {j * (nx + 1) + i, j * (nx + 1) + i + 1, (j + 1) * (nx + 1) + i + 1, (j + 1) * (nx + 1) + i}; d.addFace(f, 4); }
  d.faceGroup.assign(size_t(d.faceCount()), 0); d.groupNames = {"Polygroup"};
  d.seams.push_back({1 * (nx + 1) + 1, 2 * (nx + 1) + 1}); d.hardEdges.push_back({2 * (nx + 1) + 1, 2 * (nx + 1) + 2}); d.creases.push_back({3 * (nx + 1) + 2, 3 * (nx + 1) + 3, 0.7f});
  const int id = topoNewMesh(-1, "SymGrid", d); sel_ = {}; sel_.items = {id}; onSelChange(); return QString::number(id);
}
QString Viewport::testChain() {
  auto box = [](PolyMesh& d, const Affine& T, int n, double h) {   // (a box of n×n quads a side, half-size h)
    const int base = int(d.pos.size()); std::map<std::array<long long, 3>, int> at;
    auto vert = [&](Vec3 p) { std::array<long long, 3> k{std::llround(p.x * 1000), std::llround(p.y * 1000), std::llround(p.z * 1000)}; auto f = at.find(k); if (f != at.end()) return f->second; d.pos.push_back(T.apply(p)); return at[k] = int(d.pos.size()) - 1; };
    for (int ax = 0; ax < 3; ax++) for (int s : {-1, 1}) for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
      auto P = [&](int a, int b) { double u = -h + 2 * h * a / n, v = -h + 2 * h * b / n; Vec3 p; double* c[3] = {&p.x, &p.y, &p.z}; *c[ax] = s * h; *c[(ax + 1) % 3] = u; *c[(ax + 2) % 3] = v; return p; };
      int f[4] = {vert(P(i, j)), vert(P(i + 1, j)), vert(P(i + 1, j + 1)), vert(P(i, j + 1))}; if (s < 0) std::swap(f[1], f[3]); d.addFace(f, 4); }
    (void)base; };
  auto rot = [](double ang, Vec3 t) { Affine A; const double c = std::cos(ang), s = std::sin(ang); A.L[0][0] = c; A.L[0][1] = -s; A.L[1][0] = s; A.L[1][1] = c; A.t = t; return A; };
  PolyMesh hp; for (int k = 0; k < 3; k++) box(hp, rot(0.6 * k, Vec3(40 + 10 * k, 160, 0)), 6, 2); hp.faceGroup.assign(size_t(hp.faceCount()), 0); hp.groupNames = {"Polygroup"};
  PolyMesh rt; box(rt, rot(0, Vec3(40, 160, 0)), 2, 2.1); rt.faceGroup.assign(size_t(rt.faceCount()), 0); rt.groupNames = {"Polygroup"};
  rt.seams.push_back({0, 1}); rt.hardEdges.push_back({1, 2});
  const int h = topoNewMesh(-1, "ChainHP", hp); const int r = topoNewMesh(-1, "ChainRetopo", rt); sel_ = {}; sel_.items = {r}; onSelChange();
  return QString("%1 %2").arg(h).arg(r);
}
}  // namespace hm
