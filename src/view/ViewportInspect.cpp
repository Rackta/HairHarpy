// The Inspector's highlights on the shown meshes (both rooms): n-gons, open edges (the rim), non-planar polygons (a corner
// off the polygon's plane by over the slider's share of its longest side), non-manifold edges (shared by more than two polygons), and
// the interior markers — a mesh's loose pieces lying inside its main piece (cast rays from the piece's middle: most of
// them meet the main piece — it is enclosed; an open tube's leftovers too, only its ends are open). Worked out once per
// geometry.
#include "scene/MeshMods.h"
#include "scene/TopoEdit.h"
#include "view/Viewport.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <numeric>

namespace hm {

void Viewport::loadInspect() {
  const QJsonObject o = QJsonDocument::fromJson(QSettings().value("inspect").toByteArray()).object();
  insp.ngons = o.value("ngons").toBool(); insp.open = o.value("open").toBool(); insp.nonPlanar = o.value("nonPlanar").toBool();
  insp.nonManifold = o.value("nonManifold").toBool(); insp.interior = o.value("interior").toBool(); insp.hard = o.value("hard").toBool(); insp.doubles = o.value("doubles").toBool(); insp.planarTol = std::clamp(o.value("planarTol").toDouble(0.06), 0.0, 1.0);
}
void Viewport::inspChanged() {
  QSettings().setValue("inspect", QJsonDocument(QJsonObject{{"ngons", insp.ngons}, {"open", insp.open}, {"nonPlanar", insp.nonPlanar}, {"nonManifold", insp.nonManifold}, {"doubles", insp.doubles}, {"interior", insp.interior}, {"hard", insp.hard}, {"planarTol", insp.planarTol}}).toJson(QJsonDocument::Compact));
  topoRev_++; requestUpdate();
}

const Viewport::InspC& Viewport::inspOf(int m) const {
  const Item* it = scene_->item(m); static const InspC none;
  if (!it || !it->poly) return none;
  auto B = it->base ? it->base : it->poly; const PolyMesh* key = B.get();
  InspC& c = inspC_[m];
  // (the slow checks — the double walls' overlaps, the rays of the interior — only while their highlight is on: each
  // edit of a mesh of tens of thousands of polygons made them again, near a second a step)
  const int heavyNow = (insp.doubles ? 1 : 0) | (insp.interior ? 2 : 0);
  if (c.key == key && c.shown == it->poly.get() && c.tol == insp.planarTol && c.heavy == heavyNow) return c;
  c = {}; c.key = key; c.shown = it->poly.get(); c.tol = insp.planarTol; c.heavy = heavyNow;
  auto T = topo::ttopo(B); const auto& P = it->poly->pos; if (P.size() < size_t(T->n)) return c;
  const std::vector<char> hid = meshmods::hiddenFaces(*it, *B); auto shown = [&](int f) { return hid.empty() || !hid[size_t(f)]; };
  // n-gons, non-planar
  for (int f = 0; f < T->nf; f++) {
    if (!shown(f)) continue; const int s = T->fo[size_t(f)], n = T->faceSize(f);
    if (n > 4) c.ngons.push_back(f);
    if (n < 4) continue;
    Vec3 nn, ctr; double longest = 0;
    for (int k = 0; k < n; k++) { const Vec3& a = P[size_t(T->fv[size_t(s + k)])]; const Vec3& b = P[size_t(T->fv[size_t(s + (k + 1) % n)])];
      nn += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)); ctr += a; longest = std::max(longest, a.distanceTo(b)); }
    if (nn.lengthSq() <= 0 || longest <= 0) continue; nn = nn.normalized(); ctr = ctr * (1.0 / n);
    double dev = 0; for (int k = 0; k < n; k++) dev = std::max(dev, std::abs((P[size_t(T->fv[size_t(s + k)])] - ctr).dot(nn)));
    if (dev > std::max(1e-9, insp.planarTol * 0.5) * longest) c.nonPlanar.push_back(f);   // (the slider: 0 — any bend, 1 — off by half the side)
  }
  // double walls: polygons lying one on another — the same corners' places (a mirror copied onto itself, a piece pasted
  // twice), whether their vertices are shared or not
  if (insp.doubles) { const double q = std::max(1e-9, B->bounds().size().length() * 1e-4); std::map<std::vector<std::array<long long, 3>>, std::vector<int>> at;
    for (int f = 0; f < T->nf; f++) { if (!shown(f)) continue; const int s = T->fo[size_t(f)], n = T->faceSize(f); std::vector<std::array<long long, 3>> k;
      for (int j = 0; j < n; j++) { const Vec3& p = P[size_t(T->fv[size_t(s + j)])]; k.push_back({std::llround(p.x / q), std::llround(p.y / q), std::llround(p.z / q)}); }
      at[tedit::cycleKey(k)].push_back(f); }
    for (auto& [k, fs] : at) if (fs.size() > 1) c.doubles.insert(c.doubles.end(), fs.begin(), fs.end()); }
  // (and polygons slid partly over one another — a seam not welded, its rows lying over the next ones: no corner shared,
  // facing one way, flush — within a twentieth of an edge — their areas overlapping by a tenth of the smaller one)
  if (insp.doubles && T->nf <= 60000) {
    // (each triangle: its normal and its box's first cell kept; a pair tested in one cell only — the first both boxes
    // share — so no set of pairs tested; the cheap tests — facing, flush — before the corners shared and the clipping)
    struct Tri { Vec3 a, b, c, n; int f; long long x0, y0, z0; };
    std::vector<Tri> tr; double el = 0; int ne = 0;
    for (int f = 0; f < T->nf; f++) { if (!shown(f)) continue; const int s = T->fo[size_t(f)], n = T->faceSize(f);
      for (int k = 0; k < n; k++) { el += P[size_t(T->fv[size_t(s + k)])].distanceTo(P[size_t(T->fv[size_t(s + (k + 1) % n)])]); ne++; }
      for (int k = 1; k + 1 < n; k++) { Tri t{P[size_t(T->fv[size_t(s)])], P[size_t(T->fv[size_t(s + k)])], P[size_t(T->fv[size_t(s + k + 1)])], {}, f, 0, 0, 0};
        const Vec3 nn = (t.b - t.a).cross(t.c - t.a); if (nn.lengthSq() <= 0) continue; t.n = nn.normalized(); tr.push_back(t); } }
    el = ne ? el / ne : 0;
    if (el > 0) {
      const double cell = el * 2, tolD = el * 0.06; std::unordered_map<long long, std::vector<int>> grid; grid.reserve(tr.size() * 2);
      auto ck = [](long long x, long long y, long long z) { return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
      auto cf = [&](double v) { return (long long)std::floor(v / cell); };
      for (size_t i = 0; i < tr.size(); i++) { Tri& t = tr[i]; Box3 b; b.expand(t.a); b.expand(t.b); b.expand(t.c);
        t.x0 = cf(b.min.x - tolD); t.y0 = cf(b.min.y - tolD); t.z0 = cf(b.min.z - tolD); const long long x1 = cf(b.max.x + tolD), y1 = cf(b.max.y + tolD), z1 = cf(b.max.z + tolD);
        for (long long x = t.x0; x <= x1; x++) for (long long y = t.y0; y <= y1; y++) for (long long z = t.z0; z <= z1; z++) grid[ck(x, y, z)].push_back(int(i)); }
      auto shared = [&](int f, int g) { int k = 0; for (int i = T->fo[size_t(f)]; i < T->fo[size_t(f) + 1]; i++) for (int j = T->fo[size_t(g)]; j < T->fo[size_t(g) + 1]; j++) k += T->fv[size_t(i)] == T->fv[size_t(j)]; return k; };
      using V2 = std::array<double, 2>;
      auto area2 = [](const std::vector<V2>& p) { double s = 0; for (size_t k = 0, j = p.size() - 1; k < p.size(); j = k++) s += p[j][0] * p[k][1] - p[k][0] * p[j][1]; return s * 0.5; };
      auto clip = [](std::vector<V2> poly, const V2& a, const V2& b) {   // (kept: the left of a→b)
        std::vector<V2> out; auto side = [&](const V2& p) { return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]); };
        for (size_t k = 0; k < poly.size(); k++) { const V2& p = poly[k]; const V2& q = poly[(k + 1) % poly.size()]; const double sp = side(p), sq = side(q);
          if (sp >= 0) out.push_back(p);
          if ((sp >= 0) != (sq >= 0)) { const double t = sp / (sp - sq); out.push_back({p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t}); } }
        return out; };
      std::set<int> over;
      for (size_t i = 0; i < tr.size(); i++) { const Tri& A = tr[i]; const long long x1 = cf(std::max({A.a.x, A.b.x, A.c.x}) + tolD), y1 = cf(std::max({A.a.y, A.b.y, A.c.y}) + tolD), z1 = cf(std::max({A.a.z, A.b.z, A.c.z}) + tolD);
        for (long long x = A.x0; x <= x1; x++) for (long long y = A.y0; y <= y1; y++) for (long long z = A.z0; z <= z1; z++) {
          auto g = grid.find(ck(x, y, z)); if (g == grid.end()) continue;
          for (int j : g->second) { if (size_t(j) <= i) continue; const Tri& Bt = tr[size_t(j)]; if (A.f == Bt.f) continue;
            if (x != std::max(A.x0, Bt.x0) || y != std::max(A.y0, Bt.y0) || z != std::max(A.z0, Bt.z0)) continue;   // (the pair's one cell)
            const Vec3& na = A.n; if (na.dot(Bt.n) < 0.9) continue;          // (facing the same way, within 25°)
            if (std::abs((Bt.a - A.a).dot(na)) > tolD || std::abs((Bt.b - A.a).dot(na)) > tolD || std::abs((Bt.c - A.a).dot(na)) > tolD) continue;
            if (shared(A.f, Bt.f)) continue;                          // (no corner shared — a seam left apart; neighbours: no fault)
            const Vec3 u = (A.b - A.a).normalized(), w = na.cross(u); auto to2 = [&](const Vec3& p) { const Vec3 d = p - A.a; return V2{d.dot(u), d.dot(w)}; };
            const std::vector<V2> ta{to2(A.a), to2(A.b), to2(A.c)}; std::vector<V2> poly{to2(Bt.a), to2(Bt.b), to2(Bt.c)};
            const double sa = area2(ta), sb = std::abs(area2(poly)); if (sa <= 0 || sb <= 0) continue;   // (A is counter-clockwise in its own frame)
            for (int k = 0; k < 3 && poly.size() >= 3; k++) poly = clip(poly, ta[size_t(k)], ta[size_t((k + 1) % 3)]);
            const double ov = poly.size() >= 3 ? std::abs(area2(poly)) : 0; if (ov <= 0) continue;
            if (ov > 0.1 * std::min(sa, sb)) { over.insert(A.f); over.insert(Bt.f); } } } }
      const std::set<int> had(c.doubles.begin(), c.doubles.end()); for (int f : over) if (!had.count(f)) c.doubles.push_back(f); } }
  // the hard edges (shading broken along them)
  for (auto& h : B->hardEdges) if (h[0] < T->n && h[1] < T->n) if (int e = T->edgeOf(h[0], h[1]); e >= 0) c.hard.push_back(e);
  // open, non-manifold edges
  std::vector<int> use(size_t(T->ne), 0); for (size_t k = 0; k < T->ce.size(); k++) use[size_t(T->ce[k])]++;
  for (int e = 0; e < T->ne; e++) {
    const bool vis = shown(T->ef0[size_t(e)]) || (T->ef1[size_t(e)] >= 0 && shown(T->ef1[size_t(e)]));
    if (!vis) continue;
    if (use[size_t(e)] > 2) c.nonMan.push_back(e); else if (T->ef1[size_t(e)] < 0) c.open.push_back(e);
  }
  // interior polygons joined to the rest (an inner wall sewn onto the outer shell — no loose piece for the test below):
  // from a polygon's middle, rays every way nearly all hit the mesh — it is closed in all round. A polygon of the outer
  // shell sees out on its open side
  if (!insp.interior) return c;
  if (T->nf <= 60000) {
    std::vector<uint32_t> tris; std::vector<int> triFace;
    for (int f = 0; f < T->nf; f++) { const int s = T->fo[size_t(f)]; for (int k = s + 1; k + 1 < T->fo[size_t(f) + 1]; k++) {
      tris.push_back(uint32_t(T->fv[size_t(s)])); tris.push_back(uint32_t(T->fv[size_t(k)])); tris.push_back(uint32_t(T->fv[size_t(k + 1)])); triFace.push_back(f); } }
    topo::Bvh all; all.build(P, tris);
    std::vector<Vec3> dirs; for (int x = -1; x <= 1; x++) for (int y = -1; y <= 1; y++) for (int z = -1; z <= 1; z++) if ((x != 0) + (y != 0) + (z != 0) != 2) if (x || y || z) dirs.push_back(Vec3(x, y, z).normalized());   // (14 ways: the axes and the corners)
    const double off = std::max(1e-9, B->bounds().size().length() * 1e-5);
    std::vector<int> inner;
    for (int f = 0; f < T->nf; f++) {
      if (!shown(f)) continue; Vec3 ctr; const int s = T->fo[size_t(f)], n = T->faceSize(f); for (int k = 0; k < n; k++) ctr += P[size_t(T->fv[size_t(s + k)])]; ctr = ctr * (1.0 / n);
      int open = 0;
      // (a ray closed in only when it meets the mesh from inside — the back of a polygon: a crevice between two pieces
      // meets their fronts, and is no inside)
      for (const Vec3& d : dirs) { auto h = all.ray(ctr + d * off, d, 1e300); bool closed = false;
        if (h && h->second >= 0 && size_t(h->second) < triFace.size() && triFace[size_t(h->second)] != f) {
          const size_t t3 = size_t(h->second) * 3; const Vec3 &a = P[tris[t3]], &b = P[tris[t3 + 1]], &cc = P[tris[t3 + 2]];
          closed = (b - a).cross(cc - a).dot(d) > 0; }
        if (!closed && ++open > 1) break; }
      if (open <= 1) inner.push_back(f); }
    if (!inner.empty()) {   // (each clump of them one marker)
      std::set<int> in(inner.begin(), inner.end()), seen;
      for (int f0 : inner) { if (seen.count(f0)) continue; std::vector<int> q{f0}; seen.insert(f0); Vec3 ctr; int cnt = 0;
        while (!q.empty()) { const int f = q.back(); q.pop_back(); c.interior.push_back(f);
          for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int v = T->fv[size_t(k)]; ctr += P[size_t(v)]; cnt++;
            for (int i = T->vfo[size_t(v)]; i < T->vfo[size_t(v) + 1]; i++) { const int g = T->vF[size_t(i)]; if (in.count(g) && seen.insert(g).second) q.push_back(g); } } }
        if (cnt) c.interiorPts.push_back(ctr * (1.0 / cnt)); } }
  }
  // interior pieces: the pieces (joined through edges), the biggest the main one; the others whose middle is closed in by it
  std::vector<int> up(static_cast<size_t>(T->nf)); std::iota(up.begin(), up.end(), 0);
  std::function<int(int)> find = [&](int x) { while (up[size_t(x)] != x) x = up[size_t(x)] = up[size_t(up[size_t(x)])]; return x; };
  for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] >= 0) { int a = find(T->ef0[size_t(e)]), b = find(T->ef1[size_t(e)]); if (a != b) up[size_t(a)] = b; }
  // (pieces touching through a vertex only: one piece too)
  for (int v = 0; v < T->n; v++) { int f0 = -1; for (int i = T->vfo[size_t(v)]; i < T->vfo[size_t(v) + 1]; i++) { int f = T->vF[size_t(i)]; if (f0 < 0) f0 = f; else { int a = find(f0), b = find(f); if (a != b) up[size_t(a)] = b; } } }
  std::map<int, std::vector<int>> pieces; for (int f = 0; f < T->nf; f++) pieces[find(f)].push_back(f);
  if (pieces.size() < 2) return c;
  int mainR = -1; size_t mainN = 0; for (auto& [r, fs] : pieces) if (fs.size() > mainN) { mainN = fs.size(); mainR = r; }
  std::vector<uint32_t> tris;
  for (int f : pieces[mainR]) { const int s = T->fo[size_t(f)]; for (int k = s + 1; k + 1 < T->fo[size_t(f) + 1]; k++) { tris.push_back(uint32_t(T->fv[size_t(s)])); tris.push_back(uint32_t(T->fv[size_t(k)])); tris.push_back(uint32_t(T->fv[size_t(k + 1)])); } }
  topo::Bvh bvh; bvh.build(P, tris);
  std::vector<Vec3> dirs; for (int x = -1; x <= 1; x++) for (int y = -1; y <= 1; y++) for (int z = -1; z <= 1; z++) if (x || y || z) dirs.push_back(Vec3(x, y, z).normalized());   // (26 ways out)
  const std::set<int> already(c.interior.begin(), c.interior.end());   // (found by the rays above: not marked twice)
  for (auto& [r, fs] : pieces) {
    if (r == mainR) continue;
    if (already.count(fs[0])) continue;
    std::set<int> vs; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) vs.insert(T->fv[size_t(k)]);
    Vec3 ctr; for (int v : vs) ctr += P[size_t(v)]; ctr = ctr * (1.0 / std::max<size_t>(1, vs.size()));
    int hits = 0; for (const Vec3& d : dirs) if (bvh.ray(ctr, d, 1e300)) hits++;
    if (hits >= int(dirs.size() * 0.75)) { c.interior.insert(c.interior.end(), fs.begin(), fs.end()); c.interiorPts.push_back(ctr); }
  }
  return c;
}

}  // namespace hm

namespace hm {
QString Viewport::testDoubles(int m) {
  const bool was = insp.doubles; insp.doubles = true; inspC_.erase(m); const auto& c = inspOf(m); insp.doubles = was;
  const Item* it = scene_->item(m); if (!it) return "no mesh"; const PolyMesh& B = *(it->base ? it->base : it->poly);
  QStringList o; for (int f : c.doubles) { QStringList vs; for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) vs << QString::number(B.faceVerts[size_t(k)]); o << QString("f%1[%2]").arg(f).arg(vs.join(",")); }
  return QString("%1 polygons: ").arg(c.doubles.size()) + o.join(" ");
}
}  // namespace hm

namespace hm {
// (checks) mesh m: its edges of three polygons or more, its vertices where parts only touch (their polygons round them
// not one fan through shared sides), its polygons on the same corners twice
QString Viewport::testNonManifold(int m) const {
  const Item* it = scene_->item(m); if (!it || !it->poly) return "no mesh"; const PolyMesh& B = *(it->base ? it->base : it->poly);
  std::map<std::pair<int, int>, int> ec; std::vector<std::vector<int>> vf(B.pos.size());
  for (int f = 0; f < B.faceCount(); f++) { const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1];
    for (int k = s; k < e; k++) { const int a = B.faceVerts[size_t(k)], b = B.faceVerts[size_t(k + 1 < e ? k + 1 : s)]; ec[{std::min(a, b), std::max(a, b)}]++; vf[size_t(a)].push_back(f); } }
  int nmE = 0; QString where; for (auto& [e, c] : ec) if (c > 2) { if (nmE < 7) { const Vec3& a = B.pos[size_t(e.first)]; QStringList fs; for (int f : vf[size_t(e.first)]) { const int s = B.faceStart[size_t(f)], en = B.faceStart[size_t(f) + 1]; bool has = false; for (int k = s; k < en; k++) if (B.faceVerts[size_t(k)] == e.second) has = true; if (has) fs << QString("%1(%2)").arg(f).arg(en - s); } where += QString(" [%1-%2 x%3 at %4,%5,%6 faces %7 g%8]").arg(e.first).arg(e.second).arg(c).arg(a.x, 0, 'f', 2).arg(a.y, 0, 'f', 2).arg(a.z, 0, 'f', 2).arg(fs.join(",")).arg(B.faceGroup.empty() ? -1 : B.faceGroup[size_t(vf[size_t(e.first)][0])]); } nmE++; }
  int bow = 0;
  for (size_t v = 0; v < vf.size(); v++) { const auto& fs = vf[v]; if (fs.size() < 2) continue;
    std::map<int, int> up; for (int f : fs) up[f] = f; std::function<int(int)> root = [&](int x) { while (up[x] != x) x = up[x] = up[up[x]]; return x; };
    for (size_t i = 0; i < fs.size(); i++) for (size_t j = i + 1; j < fs.size(); j++) {   // (two polygons round v sharing a side through v)
      auto side = [&](int f, int o) { const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) if (B.faceVerts[size_t(k)] == int(v)) { const int nx = B.faceVerts[size_t(k + 1 < e ? k + 1 : s)], pv = B.faceVerts[size_t(k > s ? k - 1 : e - 1)]; if (nx == o || pv == o) return true; } return false; };
      const int s = B.faceStart[size_t(fs[i])], e = B.faceStart[size_t(fs[i]) + 1];
      for (int k = s; k < e; k++) { const int o = B.faceVerts[size_t(k)]; if (o != int(v) && side(fs[i], o) && side(fs[j], o)) { up[root(fs[i])] = root(fs[j]); break; } } }
    std::set<int> r; for (int f : fs) r.insert(root(f)); if (r.size() > 1) bow++; }
  std::map<std::vector<int>, int> cyc; int dup = 0;
  for (int f = 0; f < B.faceCount(); f++) { std::vector<int> c(B.faceVerts.begin() + B.faceStart[size_t(f)], B.faceVerts.begin() + B.faceStart[size_t(f) + 1]); if (cyc[tedit::cycleKey(c)]++) dup++; }
  return QString("%1: %2 faces · non-manifold edges %3 · bow-tie vertices %4 · doubled polygons %5").arg(it->name).arg(B.faceCount()).arg(nmE).arg(bow).arg(dup) + where;
}
}  // namespace hm
