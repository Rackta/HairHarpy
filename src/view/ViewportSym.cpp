// Symmetry (Shift+X) and its settings — one set of them for the Topology room and the Hair room. Type: Planar (a mirror
// across each checked axis' plane, and their combinations), Topology (the planar pairs as seeds, then matched through
// the mesh's own connections — holds on a posed or a slightly uneven mesh), Radial (copies turned round the first
// checked axis). Space: Local (the mesh's own axes and origin), Global (the world's), Workplane (the gizmo put by
// Alt + drag — its place and axes), Custom (a typed point, the world's axes). Context: where the plane lies along each
// axis — Auto (the space's origin when the mesh is symmetric about it, else its box's middle), Origin, Box. Restrict:
// a point on the plane (the axis — radial) stays on it. Tolerance: how far a twin may be from the mirrored point.
#include "scene/MeshMods.h"
#include "scene/Topo.h"
#include "view/Viewport.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <unordered_map>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
Affine reflect(const Vec3& c, Vec3 n) {                        // across the plane through c with normal n
  n = n.normalized(); Affine a; const double v[3] = {n.x, n.y, n.z};
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) a.L[i][j] = (i == j ? 1.0 : 0.0) - 2 * v[i] * v[j];
  a.t = c - a.lin(c); return a;
}
Affine rotate(const Vec3& c, Vec3 n, double ang) {             // round the line through c along n (Rodrigues)
  n = n.normalized(); const double cs = std::cos(ang), sn = std::sin(ang), k = 1 - cs, x = n.x, y = n.y, z = n.z; Affine a;
  const double R[3][3] = {{cs + x * x * k, x * y * k - z * sn, x * z * k + y * sn}, {y * x * k + z * sn, cs + y * y * k, y * z * k - x * sn}, {z * x * k - y * sn, z * y * k + x * sn, cs + z * z * k}};
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) a.L[i][j] = R[i][j];
  a.t = c - a.lin(c); return a;
}
// each vertex's twin under T: the nearest vertex to T(p) within tol (a grid of cells tol wide, the neighbours looked in)
std::vector<int> twins(const std::vector<Vec3>& P, const Affine& T, double tol) {
  const int n = int(P.size()); std::vector<int> pr(size_t(n), -1); if (!n) return pr;
  const double q = std::max(tol, 1e-9); std::unordered_map<int64_t, std::vector<int>> grid;
  auto key = [](int64_t i, int64_t j, int64_t k) { return (i * 73856093) ^ (j * 19349663) ^ (k * 83492791); };
  auto cell = [&](double x) { return int64_t(std::floor(x / q)); };
  for (int v = 0; v < n; v++) grid[key(cell(P[size_t(v)].x), cell(P[size_t(v)].y), cell(P[size_t(v)].z))].push_back(v);
  for (int v = 0; v < n; v++) { const Vec3 m = T.apply(P[size_t(v)]); const int64_t i0 = cell(m.x), j0 = cell(m.y), k0 = cell(m.z); double bd = tol; int best = -1;
    for (int64_t i = i0 - 1; i <= i0 + 1; i++) for (int64_t j = j0 - 1; j <= j0 + 1; j++) for (int64_t k = k0 - 1; k <= k0 + 1; k++) {
      auto g = grid.find(key(i, j, k)); if (g == grid.end()) continue;
      for (int w : g->second) { const double d = P[size_t(w)].distanceTo(m); if (d <= bd) { bd = d; best = w; } } }
    pr[size_t(v)] = best; }
  return pr;
}
// Topology: the twins found by position as seeds, then matched along the mesh — an edge a→b whose twins a'→b' are an edge
// too: the polygon on a side of a→b and the one on the matching side of a'→b' walked round in opposite ways (a mirror
// turns the winding), corner by corner
void byTopology(const PolyMesh& B, std::vector<int>& pr) {
  const auto T = topo::ttopo(std::make_shared<PolyMesh>(B)); if (!T) return;
  for (int round = 0; round < 64; round++) { bool grew = false;
    for (int f = 0; f < T->nf; f++) { const int s = T->fo[size_t(f)], e = T->fo[size_t(f) + 1], n = e - s;
      for (int k = 0; k < n; k++) { const int a = T->fv[size_t(s + k)], b = T->fv[size_t(s + (k + 1) % n)];
        const int a2 = pr[size_t(a)], b2 = pr[size_t(b)]; if (a2 < 0 || b2 < 0) continue;
        const int eg = T->edgeOf(a2, b2); if (eg < 0) continue;
        for (int g : {T->ef0[size_t(eg)], T->ef1[size_t(eg)]}) { if (g < 0) continue; const int gs = T->fo[size_t(g)], gn = T->fo[size_t(g) + 1] - gs; if (gn != n) continue;
          int ka = -1; for (int j = 0; j < gn; j++) if (T->fv[size_t(gs + j)] == a2 && T->fv[size_t(gs + (j - 1 + gn) % gn)] == b2) ka = j;   // (b2 before a2 there: reversed)
          if (ka < 0) continue;
          for (int j = 0; j < n; j++) { const int v = T->fv[size_t(s + (k + j) % n)], w = T->fv[size_t(gs + (ka - j + gn * 2) % gn)];
            if (pr[size_t(v)] < 0 && (pr[size_t(w)] < 0 || pr[size_t(w)] == v)) { pr[size_t(v)] = w; pr[size_t(w)] = v; grew = true; } } } } }
    if (!grew) break; }
}
}  // namespace

QJsonObject Viewport::SymOpts::toJson() const {
  return QJsonObject{{"type", type}, {"space", space}, {"context", context}, {"x", ax[0]}, {"y", ax[1]}, {"z", ax[2]}, {"radial", radial}, {"restrict", restrict_}, {"planes", planes}, {"tol", tol},
                     {"cx", custom.x}, {"cy", custom.y}, {"cz", custom.z}};
}
void Viewport::SymOpts::fromJson(const QJsonObject& o) {
  type = o.value("type").toString("planar"); space = o.value("space").toString("local"); context = o.value("context").toString("auto");
  ax[0] = o.value("x").toBool(true); ax[1] = o.value("y").toBool(false); ax[2] = o.value("z").toBool(false); radial = std::clamp(o.value("radial").toInt(6), 2, 64);
  restrict_ = o.value("restrict").toBool(true); planes = o.value("planes").toBool(true); tol = std::max(1e-6, o.value("tol").toDouble(1));
  custom = Vec3(o.value("cx").toDouble(), o.value("cy").toDouble(), o.value("cz").toDouble());
}

// the symmetry of mesh m (its points base, the mesh's own space): each mirror / turn and each vertex's twin under it
Viewport::SymSet Viewport::symSet(int m, const std::vector<Vec3>& base) const {
  SymSet S; const Item* it = scene_->item(m); if (!it || base.empty()) return S;
  // (the cache: the same mesh, the same points, the same settings)
  double sum = 0; for (size_t i = 0; i < base.size(); i += std::max<size_t>(1, base.size() / 512)) sum += base[i].x * 1.3 + base[i].y * 0.7 + base[i].z;
  const QByteArray key = QByteArray::number(m) + "|" + QByteArray::number(qulonglong(base.size())) + "|" + QByteArray::number(sum, 'g', 17) + "|" + QJsonDocument(symo.toJson()).toJson(QJsonDocument::Compact);
  if (symCache_.key == key) return symCache_.S;
  // the space's origin and axes — in the mesh's own space
  const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse();
  Vec3 o, e[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  if (symo.space == "global" || symo.space == "custom" || symo.space == "workplane") {
    Vec3 wo = symo.space == "custom" ? symo.custom : Vec3(); Quat q;
    if (symo.space == "workplane" && gzCustom_.hasPivot) { wo = gzCustom_.pivot; q = gzCustom_.q; }
    o = Mi.apply(wo); for (int i = 0; i < 3; i++) e[i] = Mi.lin(q.rotate(Vec3(i == 0, i == 1, i == 2))).normalized();
  }
  S.tol = symo.tol;
  // Context: where the plane lies along each axis — the origin, the points' box middle, or (Auto) the origin when about
  // half the points find a twin across it
  Vec3 lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300); for (const Vec3& p : base) { lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)); hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)); }
  const Vec3 mid = (lo + hi) * 0.5;
  auto centre = [&](const Vec3& n) {
    if (symo.context == "origin") return o;
    const Vec3 cb = o + n * (mid - o).dot(n);
    if (symo.context == "box") return cb;
    const auto pr = twins(base, reflect(o, n), S.tol); size_t k = 0; for (int w : pr) if (w >= 0) k++;
    return k * 2 >= base.size() ? o : cb; };
  std::vector<int> axes; for (int i = 0; i < 3; i++) if (symo.ax[i]) axes.push_back(i);
  if (axes.empty()) { symCache_ = {key, S}; return S; }
  if (symo.type == "radial") {
    const Vec3 n = e[axes[0]], cc = symo.context == "box" ? o + (mid - o) - n * (mid - o).dot(n) : o;   // (round the axis through the space's origin — Box: through the points' middle)
    S.radial = true; S.axC = cc; S.axN = n;
    for (int k = 1; k < symo.radial; k++) { const Affine T = rotate(cc, n, 2 * M_PI * k / symo.radial); S.T.push_back(T); S.pr.push_back(twins(base, T, S.tol)); }
  } else {
    std::vector<Affine> R; for (int i : axes) { const Vec3 c = centre(e[i]); R.push_back(reflect(c, e[i])); S.planes.push_back({c, e[i]}); }
    for (int mask = 1; mask < (1 << int(R.size())); mask++) { Affine T; for (size_t i = 0; i < R.size(); i++) if (mask >> i & 1) T = Affine::mul(R[i], T); S.T.push_back(T); }   // (each mirror, and each pair / all three together)
    for (const Affine& T : S.T) { auto pr = twins(base, T, S.tol);
      if (symo.type == "topology" && T.det() < 0) { PolyMesh Bp = *baseOf(*it); Bp.pos = base; byTopology(Bp, pr); }
      S.pr.push_back(std::move(pr)); }
  }
  symCache_ = {key, S}; return S;
}

// the first mirror's twins (the one partner the selection / PolyPen's weld / Mirror selection want); Planar X — as ever
std::vector<int> Viewport::symFirstPairs(int m, const std::vector<Vec3>& base) const {
  const SymSet S = symSet(m, base); return S.pr.empty() ? std::vector<int>(base.size(), -1) : S.pr.front();
}

// the moved points' twins put where the mirror / the turn takes them; a point on the plane (the axis) kept on it
void Viewport::symApply(const SymSet& S, std::vector<Vec3>& P, const std::vector<int>& moved, const std::vector<Vec3>& base) const {
  std::vector<char> mv(P.size(), 0); for (int v : moved) if (size_t(v) < P.size()) mv[size_t(v)] = 1;
  for (int v : moved) { if (size_t(v) >= P.size()) continue;
    if (symo.restrict_ && size_t(v) < base.size()) {
      if (S.radial) { const Vec3 d = base[size_t(v)] - S.axC; if ((d - S.axN * d.dot(S.axN)).length() < S.tol) { const Vec3 q = P[size_t(v)] - S.axC; P[size_t(v)] = S.axC + S.axN * q.dot(S.axN); continue; } }
      else { bool on = false; for (auto& [c, n] : S.planes) if (std::abs((base[size_t(v)] - c).dot(n)) < S.tol) { P[size_t(v)] = P[size_t(v)] - n * (P[size_t(v)] - c).dot(n); on = true; }
        if (on && S.planes.size() == 1) continue; } }
    for (size_t k = 0; k < S.T.size(); k++) { const int w = size_t(v) < S.pr[k].size() ? S.pr[k][size_t(v)] : -1;
      if (w < 0 || w == v || mv[size_t(w)]) continue; P[size_t(w)] = S.T[k].apply(P[size_t(v)]); }
  }
}

}  // namespace hm

namespace hm {
// a setting changed: the twins found again, what shows them drawn again, kept
void Viewport::symChanged() { symCache_ = {}; symC_ = {}; saveTopoPrefs(); topoRev_++; overlayRev_++; uvChanged(); requestUpdate(); emit topoState(); }
}  // namespace hm

namespace hm {
// the planes shown (Show Planes): each a sheet as wide as the mesh along it — X red, Y green, Z blue —, its rim drawn;
// Radial — its axis, a line through the mesh
void Viewport::topoSymPlanes(TopoDraw& D, int m) const {
  const Item* it = scene_->item(m); if (!it) return; const PolyMesh* B = baseOf(*it); if (!B || B->pos.empty()) return;
  const SymSet S = symSet(m, B->pos); const Affine M = meshmods::meshMatrix(*scene_, *it);
  auto colOf = [](const Vec3& n) { const double ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z); return ax >= ay && ax >= az ? QColor(0xe8, 0x50, 0x50) : ay >= az ? QColor(0x60, 0xd0, 0x60) : QColor(0x50, 0x8c, 0xf0); };
  if (S.radial) { double lo = 1e300, hi = -1e300; for (const Vec3& p : B->pos) { const double t = (p - S.axC).dot(S.axN); lo = std::min(lo, t); hi = std::max(hi, t); }
    const double pad = (hi - lo) * 0.1; TopoDraw::Lines l; l.c = colOf(S.axN); l.w = 2.5f; l.noPdo = true; l.p = {M.apply(S.axC + S.axN * (lo - pad)), M.apply(S.axC + S.axN * (hi + pad))}; D.lines.push_back(l); return; }
  for (auto& [c, n0] : S.planes) { const Vec3 n = n0.normalized(); Vec3 u = std::abs(n.y) < 0.9 ? n.cross(Vec3(0, 1, 0)).normalized() : n.cross(Vec3(1, 0, 0)).normalized(); const Vec3 v = n.cross(u).normalized();
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300; for (const Vec3& p : B->pos) { const Vec3 d = p - c; u0 = std::min(u0, d.dot(u)); u1 = std::max(u1, d.dot(u)); v0 = std::min(v0, d.dot(v)); v1 = std::max(v1, d.dot(v)); }
    const double pu = (u1 - u0) * 0.06, pv = (v1 - v0) * 0.06; u0 -= pu; u1 += pu; v0 -= pv; v1 += pv;
    const Vec3 q[4] = {M.apply(c + u * u0 + v * v0), M.apply(c + u * u1 + v * v0), M.apply(c + u * u1 + v * v1), M.apply(c + u * u0 + v * v1)};
    TopoDraw::Tris t; t.c = colOf(n); t.op = 0.10f; t.p = {q[0], q[1], q[2], q[0], q[2], q[3]}; D.tris.push_back(t);
    TopoDraw::Lines l; l.c = colOf(n); l.w = 1.5f; l.noPdo = true; for (int k = 0; k < 4; k++) { l.p.push_back(q[k]); l.p.push_back(q[(k + 1) % 4]); } D.lines.push_back(l); }
}
}  // namespace hm

namespace hm {
// the Hair room: each dragged anchor's twins — the anchor where each mirror / turn of the settings puts it (within the
// tolerance; the space's Local — the head's centre, the world's axes), not dragged itself
Viewport::HairSym Viewport::hairSymTwins(const std::vector<int>& list) const {
  HairSym H; if (!tset.sym || room() != "hair" || list.empty()) return H;
  Vec3 o = cam_.sceneCenter; Quat q;
  if (symo.space == "global") o = Vec3(); else if (symo.space == "custom") o = symo.custom; else if (symo.space == "workplane" && gzCustom_.hasPivot) { o = gzCustom_.pivot; q = gzCustom_.q; }
  std::vector<int> axes; for (int i = 0; i < 3; i++) if (symo.ax[i]) axes.push_back(i); if (axes.empty()) return H;
  auto e = [&](int i) { return q.rotate(Vec3(i == 0, i == 1, i == 2)).normalized(); };
  if (symo.type == "radial") { H.radial = true; H.axC = o; H.axN = e(axes[0]); for (int k = 1; k < symo.radial; k++) H.T.push_back(rotate(o, H.axN, 2 * M_PI * k / symo.radial)); }
  else { std::vector<Affine> R; for (int i : axes) { R.push_back(reflect(o, e(i))); H.planes.push_back({o, e(i)}); }
    for (int mask = 1; mask < (1 << int(R.size())); mask++) { Affine T; for (size_t i = 0; i < R.size(); i++) if (mask >> i & 1) T = Affine::mul(R[i], T); H.T.push_back(T); } }
  std::set<int> in(list.begin(), list.end());
  for (size_t i = 0; i < list.size(); i++) { const Anchor* a = scene_->anchor(list[i]); if (!a) continue; H.start.push_back({list[i], a->pos});
    for (size_t k = 0; k < H.T.size(); k++) { const Vec3 m = H.T[k].apply(a->pos); int best = -1; double bd = symo.tol;
      for (const auto& [id, b] : scene_->anchors) { if (in.count(id)) continue; const double d = b.pos.distanceTo(m); if (d <= bd) { bd = d; best = id; } }
      if (best >= 0) H.tw.push_back({int(i), best, int(k)}); } }
  return H;
}
// the twins put where the mirrors take the dragged anchors now; an anchor that lay on the plane (the axis) kept on it
void Viewport::hairSymApply(const HairSym& H) {
  for (size_t i = 0; i < H.start.size(); i++) if (symo.restrict_) if (Anchor* a = scene_->anchor(H.start[i].first)) {
    if (H.radial) { const Vec3 d = H.start[i].second - H.axC; if ((d - H.axN * d.dot(H.axN)).length() < symo.tol) a->pos = H.axC + H.axN * (a->pos - H.axC).dot(H.axN); }
    else for (auto& [c, n] : H.planes) if (std::abs((H.start[i].second - c).dot(n)) < symo.tol) a->pos = a->pos - n * (a->pos - c).dot(n); }
  for (auto& [i, tw, k] : H.tw) if (const Anchor* a = scene_->anchor(H.start[size_t(i)].first)) if (Anchor* b = scene_->anchor(tw)) b->pos = H.T[size_t(k)].apply(a->pos);
}
std::vector<int> Viewport::HairSym::twinIds() const { std::vector<int> o; for (auto& t : tw) o.push_back(std::get<1>(t)); return o; }
}  // namespace hm

namespace hm {
// the Hair room's planes (Show Planes): through the space's point (Local — the head's centre), as wide as the anchors
void Viewport::hairSymPlanes(TopoDraw& D) const {
  const HairSym H = [&] { HairSym h; Vec3 o = cam_.sceneCenter; Quat q;
    if (symo.space == "global") o = Vec3(); else if (symo.space == "custom") o = symo.custom; else if (symo.space == "workplane" && gzCustom_.hasPivot) { o = gzCustom_.pivot; q = gzCustom_.q; }
    int first = -1; for (int i = 0; i < 3; i++) if (symo.ax[i]) { if (first < 0) first = i; if (symo.type != "radial") h.planes.push_back({o, q.rotate(Vec3(i == 0, i == 1, i == 2)).normalized()}); }
    if (symo.type == "radial" && first >= 0) { h.radial = true; h.axC = o; h.axN = q.rotate(Vec3(first == 0, first == 1, first == 2)).normalized(); }
    return h; }();
  std::vector<Vec3> pts; for (const auto& [id, a] : scene_->anchors) pts.push_back(a.pos);
  if (pts.empty()) { const double r = 100; pts = {cam_.sceneCenter - Vec3(r, r, r), cam_.sceneCenter + Vec3(r, r, r)}; }
  auto colOf = [](const Vec3& n) { const double ax = std::abs(n.x), ay = std::abs(n.y), az = std::abs(n.z); return ax >= ay && ax >= az ? QColor(0xe8, 0x50, 0x50) : ay >= az ? QColor(0x60, 0xd0, 0x60) : QColor(0x50, 0x8c, 0xf0); };
  if (H.radial) { double lo = 1e300, hi = -1e300; for (const Vec3& p : pts) { const double t = (p - H.axC).dot(H.axN); lo = std::min(lo, t); hi = std::max(hi, t); }
    TopoDraw::Lines l; l.c = colOf(H.axN); l.w = 2.5f; l.noPdo = true; l.p = {H.axC + H.axN * (lo - (hi - lo) * 0.1), H.axC + H.axN * (hi + (hi - lo) * 0.1)}; D.lines.push_back(l); return; }
  for (auto& [c, n] : H.planes) { Vec3 u = std::abs(n.y) < 0.9 ? n.cross(Vec3(0, 1, 0)).normalized() : n.cross(Vec3(1, 0, 0)).normalized(); const Vec3 v = n.cross(u).normalized();
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300; for (const Vec3& p : pts) { const Vec3 d = p - c; u0 = std::min(u0, d.dot(u)); u1 = std::max(u1, d.dot(u)); v0 = std::min(v0, d.dot(v)); v1 = std::max(v1, d.dot(v)); }
    const double pu = (u1 - u0) * 0.08, pv = (v1 - v0) * 0.08; u0 -= pu; u1 += pu; v0 -= pv; v1 += pv;
    const Vec3 q[4] = {c + u * u0 + v * v0, c + u * u1 + v * v0, c + u * u1 + v * v1, c + u * u0 + v * v1};
    TopoDraw::Tris t; t.c = colOf(n); t.op = 0.10f; t.p = {q[0], q[1], q[2], q[0], q[2], q[3]}; D.tris.push_back(t);
    TopoDraw::Lines l; l.c = colOf(n); l.w = 1.5f; l.noPdo = true; for (int k = 0; k < 4; k++) { l.p.push_back(q[k]); l.p.push_back(q[(k + 1) % 4]); } D.lines.push_back(l); }
}
}  // namespace hm
