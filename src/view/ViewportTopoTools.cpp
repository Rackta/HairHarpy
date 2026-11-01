// The Topology room's modelling tools — their pointer and keys, what they show, PolyPen, Loop, Knife and the Smooth
// EdgeChain (Weld, Plane Cut, Bevel / Extrude, Inset, Bridge: ViewportTopoTools2.cpp).
//  PolyPen: the vertex / edge / polygon under the pointer lit; a held LMB drags it (a vertex let go on another one —
//   welded); a click starts a polygon (its preview follows the pointer) — clicks add its corners, a click on the first
//   one or a double click (from the third) closes it, Esc drops it; from a vertex / an edge to another one of the same
//   polygon — the polygon cut there; between two places of the rim a triangle's / a quad's walk apart — that polygon
//   filled in; Ctrl + drag an edge — a new polygon pulled out of it (let go on another edge — the gap between them
//   filled), Ctrl + drag a polygon — extruded; Ctrl + click — the element deleted; Shift + click — an anchor point;
//   Shift+RMB — the polygon of the points round the pointer (anchors, vertices) filled; Ctrl+Shift held — Loop.
//  Loop: the cut across the ring of polygons through the edge under the pointer shown; the wheel — how many cuts;
//   Shift — onto the edge's 1/2, 1/4, 3/4, 1/6 … marks; Ctrl — parallel to the nearer side; LMB cuts, RMB cuts and
//   selects the new edges.
//  Knife: clicks on the mesh put the anchors of a smooth curve (Esc ends it, any number of curves); from empty space
//   a drag — a straight cut; Ctrl + drag an anchor — moved; Shift+RMB — cut, optimise, recompute, delete; Enter —
//   cut. Through: the far side cut too.
//  Smooth EdgeChain: the selected edges → a curve (Shift+RMB) the chain follows; its anchors dragged (a click on the
//   curve — a new one), smoothed, the edges evened along it.
#include "scene/MeshMods.h"
#include <QCursor>
#include <QGuiApplication>
#include "ui/Pie.h"
#include "view/SceneGeometry.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QDateTime>
#include <numeric>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace hm {

using tedit::MeshEd; using tedit::ek;

// the side of a reference the built mesh lies on: a point there with its normal n — the built mesh's polygon nearest it
// (within r) turned the other way means the reference's back, the inside of a garment with its thickness: no snapping
// onto it (no polygon of the built mesh near — any side)
struct SideGuard { const topo::Bvh* mb = nullptr; double r = 0; std::shared_ptr<const topo::Bvh> keep;
  bool ok(const Vec3& p, const Vec3& n) const {
    if (!mb) return true; double best = r * r; Vec3 bn, bc; bool any = false;
    mb->sphere(p, r, [&](int t) { const Vec3 &a = mb->P[mb->idx[size_t(t) * 3]], &b = mb->P[mb->idx[size_t(t) * 3 + 1]], &c = mb->P[mb->idx[size_t(t) * 3 + 2]];
      const Vec3 m = (a + b + c) * (1.0 / 3); const double d = (m - p).lengthSq(); if (d < best) { best = d; bn = (b - a).cross(c - a); bc = m; any = true; } });
    // (turned the other way — well past a right angle: a wall of a fold across is not — and behind the point: the garment's
    // other face, its thickness between; in front of it — the
    // opposite wall of a fold, facing it: that side is free to build on)
    return !any || bn.normalized().dot(n.normalized()) > -0.6 || (bc - p).dot(n.normalized()) > 0; } };
namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
// the vertices of polygons shown (a polygroup hidden with its eye, polygons hidden with H: theirs alone left out — the pen
// neither takes nor welds onto them); empty — every one
std::vector<char> shownVerts(const Item& it, const PolyMesh& B) {
  const std::vector<char> h = meshmods::hiddenFaces(it, B); if (h.empty()) return {};
  std::vector<char> s(B.pos.size(), 0);
  for (int f = 0; f < B.faceCount(); f++) if (!h[size_t(f)]) for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) if (size_t(B.faceVerts[size_t(k)]) < s.size()) s[size_t(B.faceVerts[size_t(k)])] = 1;
  return s; }
// the first hit along a ray on a triangle facing the way it comes from, on the side the built mesh lies on — the back of a
// reference passed through (never snapped onto: the polygon would be built inside it)
std::optional<std::pair<double, int>> frontHit(const topo::Bvh& b, const Vec3& o, const Vec3& d, double tmax, const SideGuard& g = {}) {
  double t0 = 0; Vec3 oo = o;
  for (int k = 0; k < 64 && t0 < tmax; k++) {
    const auto h = b.ray(oo, d, tmax - t0); if (!h) return std::nullopt;
    const size_t t = size_t(h->second); const Vec3 &a = b.P[b.idx[t * 3]], &bb = b.P[b.idx[t * 3 + 1]], &c = b.P[b.idx[t * 3 + 2]]; const Vec3 n = (bb - a).cross(c - a);
    if (n.dot(d) < 0 && g.ok(oo + d * h->first, n)) return std::make_pair(t0 + h->first, h->second);
    const double step = h->first + 1e-7 * (1 + std::abs(t0 + h->first)); t0 += step; oo = oo + d * step; }
  return std::nullopt;
}
// what the view sees of a reference alone (solo — the other meshes not in the way): its first hit along the ray, when that
// side is turned to the camera — the snapping's first choice
std::optional<double> soloHit(const topo::Bvh& b, const Vec3& o, const Vec3& d) {
  const auto h = b.ray(o, d, 1e12); if (!h) return std::nullopt; const size_t t = size_t(h->second);
  const Vec3 &a = b.P[b.idx[t * 3]], &bb = b.P[b.idx[t * 3 + 1]], &c = b.P[b.idx[t * 3 + 2]];
  if ((bb - a).cross(c - a).dot(d) < 0) return h->first; return std::nullopt;
}
bool facesEye(const topo::Bvh& b, int t, const Vec3& eye, const Vec3& dir, bool ortho) {   // (a triangle turned to the camera)
  const Vec3 &a = b.P[b.idx[size_t(t) * 3]], &bb = b.P[b.idx[size_t(t) * 3 + 1]], &c = b.P[b.idx[size_t(t) * 3 + 2]];
  return (bb - a).cross(c - a).dot(ortho ? dir : (a + bb + c) * (1.0 / 3) - eye) < 0;
}
const char* MODEL[] = {"tpen", "treveal", "tpatch", "tlobe", "tloop", "tknife", "tchain", "tweld", "tplane", "tbevel", "tinset", "tbridge", "tfill", "tshaper", "tslide", "tnormals", "toptimize", "tmirror", "tfixmesh"};
QWidget* mainWin() { for (QWidget* w : QApplication::topLevelWidgets()) if (w->inherits("QMainWindow")) return w; return nullptr; }
double segDistT(double px, double py, double ax, double ay, double bx, double by, double* tOut = nullptr) {
  double dx = bx - ax, dy = by - ay, L = dx * dx + dy * dy, t = L > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / L, 0.0, 1.0) : 0;
  if (tOut) *tOut = t; return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}
// two screen segments crossing: → their places along each (0…1)
bool segCross(double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy, double& t, double& u) {
  const double rx = bx - ax, ry = by - ay, sx = dx - cx, sy = dy - cy, den = rx * sy - ry * sx; if (std::abs(den) < 1e-12) return false;
  t = ((cx - ax) * sy - (cy - ay) * sx) / den; u = ((cx - ax) * ry - (cy - ay) * rx) / den;
  return t >= 0 && t <= 1 && u >= 0 && u <= 1;
}
// a smooth curve through the points (Catmull-Rom), `per` samples a span
std::vector<Vec3> catmull(const std::vector<Vec3>& a, bool closed, int per) {
  const int n = int(a.size()); if (n < 2) return a;
  auto at = [&](int i) { return closed ? a[size_t(((i % n) + n) % n)] : a[size_t(std::clamp(i, 0, n - 1))]; };
  std::vector<Vec3> out; const int spans = closed ? n : n - 1;
  for (int i = 0; i < spans; i++) {
    const Vec3 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
    for (int s = 0; s < per; s++) { const double t = double(s) / per, t2 = t * t, t3 = t2 * t;
      out.push_back((p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2 + (p1 * 3 - p0 - p2 * 3 + p3) * t3) * 0.5); }
  }
  if (!closed) out.push_back(a.back()); else out.push_back(a.front());
  return out;
}
// a polyline's point at the fraction u of its length
Vec3 alongLen(const std::vector<Vec3>& L, const std::vector<double>& cum, double u) {
  if (L.empty()) return {}; if (L.size() == 1 || cum.back() <= 0) return L[0];
  const double d = std::clamp(u, 0.0, 1.0) * cum.back(); size_t i = size_t(std::upper_bound(cum.begin(), cum.end(), d) - cum.begin()); i = std::clamp<size_t>(i, 1, L.size() - 1);
  const double seg = cum[i] - cum[i - 1]; const double t = seg > 0 ? (d - cum[i - 1]) / seg : 0; return L[i - 1] + (L[i] - L[i - 1]) * t;
}
std::vector<double> cumLen(const std::vector<Vec3>& L) { std::vector<double> c(L.size(), 0); for (size_t i = 1; i < L.size(); i++) c[i] = c[i - 1] + L[i].distanceTo(L[i - 1]); return c; }

// the ring of polygons across the edge a–b: each edge of it in the same sense (its a-ends on one side), the polygon after each
struct RingEl { int a, b; };
struct Ring { std::vector<RingEl> el; std::vector<int> faces; bool closed = false; };
// (P given: the ring goes on through a triangle — a corner's fan, a quad strip turning: in by one side, out by the other
// side nearer parallel to it — the fan's next spoke, not its rim; the cut there a triangle and a quad)
Ring ringOf(const topo::TTopo& T, int e0, const std::vector<Vec3>* P = nullptr) {
  Ring R; if (e0 < 0 || e0 >= T.ne) return R;
  // (in quad f with a, b side by side: the side across — d next to a, c next to b)
  auto across = [&](int f, int a, int b, int& c, int& d) {
    if (P && T.faceSize(f) == 3) { const int s = T.fo[size_t(f)]; int o = -1; for (int k = 0; k < 3; k++) { const int x = T.fv[size_t(s + k)]; if (x != a && x != b) o = x; } if (o < 0) return false;
      auto dirOf = [&](int x, int y) { const Vec3 d0 = (*P)[size_t(y)] - (*P)[size_t(x)]; return d0.lengthSq() > 0 ? d0.normalized() : d0; };
      const Vec3 ab = dirOf(a, b); const double viaA = std::abs(ab.dot(dirOf(a, o))), viaB = std::abs(ab.dot(dirOf(o, b)));
      if (viaA >= viaB) { d = a; c = o; } else { d = o; c = b; }   // (out by a–o: a stays on its side; by o–b: b stays on its)
      return true; }
    if (T.faceSize(f) != 4) return false; const int s = T.fo[size_t(f)];
    for (int k = 0; k < 4; k++) { const int x = T.fv[size_t(s + k)], y = T.fv[size_t(s + (k + 1) % 4)];
      if (x == a && y == b) { c = T.fv[size_t(s + (k + 2) % 4)]; d = T.fv[size_t(s + (k + 3) % 4)]; return true; }
      if (x == b && y == a) { d = T.fv[size_t(s + (k + 2) % 4)]; c = T.fv[size_t(s + (k + 3) % 4)]; return true; } }
    return false; };
  auto walk = [&](int f0, int a0, int b0, std::vector<RingEl>& els, std::vector<int>& fs) {
    int f = f0, a = a0, b = b0; std::set<int> seen; int tris = 0; size_t runEl = 0, runF = 0;
    while (f >= 0 && !seen.count(f)) {
      seen.insert(f); int c, d; if (!across(f, a, b, c, d)) return false;
      // (through triangles only as far as a corner's fan: a longer run of them — a triangulated stretch — the ring
      // stops before it, as at any polygon it can't cross)
      if (T.faceSize(f) == 3) { if (!tris++) { runEl = els.size(); runF = fs.size(); } if (tris > 8) { els.resize(runEl); fs.resize(runF); return false; } } else tris = 0;
      const int e = T.edgeOf(d, c); fs.push_back(f);
      if (e == e0) return true;                                    // (round: closed)
      els.push_back({d, c}); a = d; b = c;
      f = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)];
    }
    return false; };
  const int a = T.ea[size_t(e0)], b = T.eb[size_t(e0)];
  std::vector<RingEl> fe, be; std::vector<int> ff, bf;
  const bool closed = T.ef1[size_t(e0)] >= 0 ? walk(T.ef1[size_t(e0)], a, b, fe, ff) : false;
  if (closed) { R.closed = true; R.el.push_back({a, b}); R.el.insert(R.el.end(), fe.begin(), fe.end()); R.faces = ff; return R; }
  walk(T.ef0[size_t(e0)], a, b, be, bf);
  // (backwards reversed, then e0, then forwards: the polygon between each two — each walk's list as long as its edges)
  for (size_t i = be.size(); i-- > 0;) R.el.push_back(be[i]);
  for (size_t i = bf.size(); i-- > 0;) R.faces.push_back(bf[i]);
  R.el.push_back({a, b});
  R.el.insert(R.el.end(), fe.begin(), fe.end()); R.faces.insert(R.faces.end(), ff.begin(), ff.end());
  return R;
}
// (Loop, parallel: whether the cut keeps to the ring's a side — the side Ctrl fixed, else the nearer one)
bool ringNearA(const Ring& R, double t0, int lpVert, const tedit::EK& lpEdge) {
  if (lpVert >= 0) for (auto& el : R.el) if (ek(el.a, el.b) == lpEdge) { if (el.a == lpVert) return true; if (el.b == lpVert) return false; }
  return t0 <= 0.5;
}
// the shortest walk along the rim (edges of one polygon) from a to one of `to`, at most maxE edges, not over edge `skip`
std::vector<int> rimPath(const topo::TTopo& T, int a, const std::set<int>& to, int maxE, int skip = -1) {
  std::map<int, int> prev{{a, -1}}; std::vector<int> fr{a};
  for (int d = 0; d < maxE && !fr.empty(); d++) { std::vector<int> nx;
    for (int v : fr) for (int j = T.voff[size_t(v)]; j < T.voff[size_t(v) + 1]; j++) { const int e = T.vE[size_t(j)]; if (e == skip || T.ef1[size_t(e)] >= 0) continue;
      const int w = T.other(e, v); if (prev.count(w)) continue; prev[w] = v; nx.push_back(w);
      if (to.count(w)) { std::vector<int> p; for (int x = w; x >= 0; x = prev[x]) p.push_back(x); std::reverse(p.begin(), p.end()); return p; } }
    fr = nx; }
  return {};
}
}  // namespace

bool Viewport::isModelTool() const { if (room() != "topo") return false; for (const char* t : MODEL) if (tool_ == t) return true; return false; }

// ---------- settings ----------
void Viewport::loadModelPrefs() {
  const QJsonObject s = QJsonDocument::fromJson(QSettings().value("topo/model").toByteArray()).object(); TTools& T = ttools;
  auto d = [&](const char* k, double& v, double lo, double hi) { if (s.contains(k)) v = std::clamp(s.value(k).toDouble(v), lo, hi); };
  auto i = [&](const char* k, int& v, int lo, int hi) { if (s.contains(k)) v = std::clamp(s.value(k).toInt(v), lo, hi); };
  auto b = [&](const char* k, bool& v) { if (s.contains(k)) v = s.value(k).toBool(v); };
  d("normSmooth", T.normSmooth, 0, 1); b("normHardBorder", T.normHardBorder); if (const QString nm = s.value("normMode").toString(); nm == "sphere" || nm == "face") T.normMode = nm; d("normNose", T.normNose, 0, 2); d("nBrushVal", T.nBrushVal, 0, 1);
  d("optWeld", T.optWeld, 1e-6, 1e4); T.fixSplit = s.value("fixSplit").toBool(true); T.fixWeld = s.value("fixWeld").toBool(true); T.fixDoubles = s.value("fixDoubles").toBool(true); T.fixManifold = s.value("fixManifold").toBool(true); T.fixOrient = s.value("fixOrient").toBool(true); T.optMidSide = s.value("optMidSide").toBool(false); T.optMode = s.value("optMode").toString("safe") == "merge" ? "merge" : "safe"; i("mirAxis", T.mirAxis, 0, 2); b("mirLocal", T.mirLocal); i("mirCount", T.mirCount, 2, 64); d("normLen", T.normLen, 0.01, 10); d("normOp", T.normOp, 0.05, 1); b("normShow", T.normShow);
  { QString t = s.value("mirType").toString("mirror"); T.mirType = t == "radial" || t == "radialMirror" || t == "radialHalf" ? t : "mirror"; }
  T.mirReplace = s.value("mirReplace").toBool(false); T.mirNormals = s.value("mirNormals").toBool(true);
  d("penWeld", T.penWeld, 2, 80); d("penFill", T.penFill, 10, 400); d("penSmooth", T.penSmooth, 8, 800); { QString k = s.value("penBrush").toString("relax"); T.penBrush = k == "smooth" || k == "inflate" || k == "pinch" ? k : "relax"; } i("loopCuts", T.loopCuts, 1, 32); d("loopPct", T.loopPct, 0.01, 0.99); b("loopParallel", T.loopParallel);
  d("knifeWeld", T.knifeWeld, 0, 60); b("knifeThrough", T.knifeThrough); d("chainSmooth", T.chainSmooth, 0, 1); b("chainEqual", T.chainEqual);
  if (s.value("weldMode").toString() == "pair") T.weldMode = "pair";
  if (s.value("weldPos").toString() == "side") T.weldPos = "side";
  { QString m = s.value("planeMode").toString("all"); T.planeMode = m == "split" || m == "removeA" || m == "removeB" ? m : "all"; }
  i("planeCuts", T.planeCuts, 1, 50); d("planeSpacing", T.planeSpacing, 0.001, 1e4); b("planeSel", T.planeSel); b("planeSelCuts", T.planeSelCuts);
  d("bevOffset", T.bevOffset, -1e4, 1e4); d("bevBevel", T.bevBevel, 0, 1e4); i("bevSeg", T.bevSeg, 1, 32); b("bevTop", T.bevTop); b("bevBottom", T.bevBottom); b("bevSmart", T.bevSmart); b("bevProp", T.bevProp);
  b("bevSelSides", T.bevSelSides); b("bevSelTop", T.bevSelTop); b("bevSelBottom", T.bevSelBottom);
  d("insOffset", T.insOffset, 0, 1e4); d("insDepth", T.insDepth, -1e4, 1e4); i("insSeg", T.insSeg, 1, 32); b("insGroups", T.insGroups); b("insScale", T.insScale);
  d("insCircleR", T.insCircleR, 0.001, 1e4); i("insCircleN", T.insCircleN, 3, 64); b("insCircleHole", T.insCircleHole);
  i("brSeg", T.brSeg, 0, 64); d("brTension", T.brTension, -2, 2); i("brSpin", T.brSpin, -64, 64); b("brReverse", T.brReverse); b("brKeep", T.brKeep);
  { QString f = s.value("fillMode").toString("quads"); T.fillMode = f == "quadtri" || f == "fan" || f == "tris" || f == "ngon" ? f : "quads"; }
  i("fillSub", T.fillSub, 0, 32); d("fillFlow", T.fillFlow, 0, 1);
}
void Viewport::saveModelPrefs() const {
  const TTools& T = ttools;
  QSettings().setValue("topo/model", QJsonDocument(QJsonObject{
    {"penWeld", T.penWeld}, {"penFill", T.penFill}, {"penSmooth", T.penSmooth}, {"penBrush", T.penBrush}, {"loopCuts", T.loopCuts}, {"loopPct", T.loopPct}, {"loopParallel", T.loopParallel},
    {"knifeWeld", T.knifeWeld}, {"knifeThrough", T.knifeThrough}, {"chainSmooth", T.chainSmooth}, {"chainEqual", T.chainEqual}, {"weldMode", T.weldMode}, {"weldPos", T.weldPos},
    {"planeMode", T.planeMode}, {"planeCuts", T.planeCuts}, {"planeSpacing", T.planeSpacing}, {"planeSel", T.planeSel}, {"planeSelCuts", T.planeSelCuts},
    {"bevOffset", T.bevOffset}, {"bevBevel", T.bevBevel}, {"bevSeg", T.bevSeg}, {"bevTop", T.bevTop}, {"bevBottom", T.bevBottom}, {"bevSmart", T.bevSmart}, {"bevProp", T.bevProp},
    {"bevSelSides", T.bevSelSides}, {"bevSelTop", T.bevSelTop}, {"bevSelBottom", T.bevSelBottom},
    {"insOffset", T.insOffset}, {"insDepth", T.insDepth}, {"insSeg", T.insSeg}, {"insGroups", T.insGroups}, {"insScale", T.insScale}, {"insCircleR", T.insCircleR}, {"insCircleN", T.insCircleN}, {"insCircleHole", T.insCircleHole},
    {"nBrushVal", T.nBrushVal}, {"brSeg", T.brSeg}, {"brTension", T.brTension}, {"brSpin", T.brSpin}, {"brReverse", T.brReverse}, {"brKeep", T.brKeep}, {"fillMode", T.fillMode}, {"fillSub", T.fillSub}, {"fillFlow", T.fillFlow}, {"optWeld", T.optWeld}, {"mirAxis", T.mirAxis}, {"mirCount", T.mirCount}, {"mirType", T.mirType}, {"mirReplace", T.mirReplace}, {"mirLocal", T.mirLocal}, {"fixSplit", T.fixSplit}, {"fixWeld", T.fixWeld}, {"fixDoubles", T.fixDoubles}, {"fixManifold", T.fixManifold}, {"fixOrient", T.fixOrient}, {"optMidSide", T.optMidSide}, {"optMode", T.optMode}, {"mirNormals", T.mirNormals}, {"normLen", T.normLen}, {"normOp", T.normOp}, {"normShow", T.normShow}, {"normSmooth", T.normSmooth}, {"normMode", T.normMode}, {"normNose", T.normNose}, {"normHardBorder", T.normHardBorder}}).toJson(QJsonDocument::Compact));
}

// ---------- helpers ----------
std::vector<Vec3> Viewport::ttWorld(int m) const {
  const Item* it = scene_->item(m); if (!it) return {};
  const PolyMesh* B = baseOf(*it); if (it->poly && it->poly->pos.size() == B->pos.size()) return it->poly->pos;
  const Affine M = meshmods::meshMatrix(*scene_, *it); std::vector<Vec3> W; W.reserve(B->pos.size()); for (auto& p : B->pos) W.push_back(M.apply(p)); return W;
}

// a new point under the pointer: Snap on — on the other meshes' surface; else on the mesh; else on the plane through
// the mesh's middle facing the camera
SideGuard Viewport::sideGuard(int m) const {   // (the built mesh m's polygons — the side of the references it lies on)
  SideGuard g; const Item* mi = m >= 0 ? scene_->item(m) : nullptr; if (!mi || !mi->poly) return g;
  const auto b = bvhOf(m, false); if (!b) return g; g.keep = b; g.mb = b.get();
  g.r = mi->poly->bounds().size().length() / std::sqrt(double(std::max(1, baseOf(*mi)->faceCount()))) * 2; return g;
}
Vec3 Viewport::ttFreePoint(const QPointF& mp, int m) const {
  fpRefused_ = false;
  if (tset.snap) { std::vector<int> others; for (int x : topoMeshes()) if (x != m) others.push_back(x);
    for (auto& [id, it] : scene_->items) if (id != m && it.type == ItemType::IMesh && it.poly && it.raw.value("tref").toBool() && scene_->isVisible(it)) others.push_back(id);   // (the references among them)
    { const Ray R = ray(mp); const SideGuard sg = sideGuard(m); double bt = 1e300;
      for (int id : others) if (auto b = bvhOf(id, true)) if (auto t = soloHit(*b, R.o, R.d); t && *t < bt) bt = *t;   // (first: what is seen of a reference on its own)
      if (bt < 1e300) return R.o + R.d * bt;
      for (int id : others) if (auto b = bvhOf(id, true)) if (auto h = frontHit(*b, R.o, R.d, 1e12, sg); h && h->first < bt) bt = h->first;   // (only a side turned to the camera — never the back of a reference)
      if (bt < 1e300) return R.o + R.d * bt;
      // (the surface under the pointer the back of a reference, or its inside — the side the built mesh does not lie on: no
      // point there — the press refused)
      if (const auto seen = pickSurface(mp); seen && seen->itemId != m && (seen->normal.dot(R.d) >= 0 || !sg.ok(seen->point, seen->normal))) fpRefused_ = true; } }
  if (m < 0) { std::vector<int> all = topoMeshes(); for (auto& [id, it] : scene_->items) if (it.type == ItemType::IMesh && it.poly && it.raw.value("tref").toBool() && scene_->isVisible(it)) all.push_back(id);
    if (auto r = tPick(mp, all)) return r->point; }                 // (no mesh yet: onto what is under the pointer)
  if (auto r = tPick(mp, {m})) if (const Item* mi = scene_->item(m); mi && r->f >= 0) {   // (its own polygon under the pointer — only one turned to the camera: not its back seen through)
    const PolyMesh& B = *baseOf(*mi); const auto W = ttWorld(m); Vec3 n; const int s0 = B.faceStart[size_t(r->f)], e0 = B.faceStart[size_t(r->f) + 1];
    for (int k = s0; k < e0; k++) { const Vec3& a = W[size_t(B.faceVerts[size_t(k)])]; const Vec3& b = W[size_t(B.faceVerts[size_t(k + 1 < e0 ? k + 1 : s0)])]; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
    if (n.dot(cam_.ortho() ? cam_.dir() : r->point - cam_.pos) < 0) return r->point; }
  const Item* it = scene_->item(m); Vec3 c = it && it->poly ? it->poly->bounds().center() : cam_.pivot;
  if (auto h = planeHit(ray(mp), c, cam_.dir())) return *h;
  return c;
}

// the element under the pointer: an anchor, a vertex (10 px), an edge (8 px), else free (on a polygon: *face)
Viewport::TTPt Viewport::ttPickPt(const QPointF& mp, int m, bool anchors, bool back) const {
  ProfScope prof_("ttPickPt");
  TTPt p; Projector P = projector();
  auto ghost = [&](Vec3& g) { return ghostVertAt(mp, 10, g) || ghostEdgeAt(mp, 6, g); };   // (Topology Ghost: its vertex, else its edge — before the bare surface)
  if (anchors) for (size_t i = 0; i < tt_.anchors.size(); i++) { ScreenPt s = P.toScreen(tt_.anchors[i]); if (s.z < 1 && std::hypot(s.x - mp.x(), s.y - mp.y()) < 10) { p.anchor = int(i); p.w = tt_.anchors[i]; return p; } }
  const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) { Vec3 g; p.w = ghost(g) ? g : ttFreePoint(mp, m); return p; }
  auto r = tPick(mp, {m});
  if (r) {   // (its polygon hidden behind another mesh — not the one worked on: nothing of it lit; a free point)
    std::vector<int> others; for (auto& [id, o] : scene_->items) if (id != m && o.type == ItemType::IMesh && o.poly && scene_->isVisible(o)) others.push_back(id);
    if (!others.empty()) if (auto o = tPick(mp, others)) {
      const double share = std::max(0.004, topoPdo(m)) + 0.002;   // (the active mesh lies on the references: as near as the PDO draws it counts as in front)
      const double dm = cam_.ortho() ? r->point.dot(cam_.dir()) : r->point.distanceTo(cam_.pos), dq = cam_.ortho() ? o->point.dot(cam_.dir()) : o->point.distanceTo(cam_.pos);
      if (dq < dm - std::abs(dm) * share - 1e-6) { Vec3 g; p.w = ghost(g) ? g : ttFreePoint(mp, m); return p; } } }
  if (r && r->f >= 0 && !back) {                               // (its polygon under the pointer seen from behind — its far side through a hole: nothing of it to take; a free point)
    const PolyMesh& B = *baseOf(*it); const auto W = ttWorld(m); Vec3 n; const int s0 = B.faceStart[size_t(r->f)], e0 = B.faceStart[size_t(r->f) + 1];
    for (int k = s0; k < e0; k++) { const Vec3& a = W[size_t(B.faceVerts[size_t(k)])]; const Vec3& b = W[size_t(B.faceVerts[size_t(k + 1 < e0 ? k + 1 : s0)])]; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
    if (n.dot(cam_.ortho() ? cam_.dir() : r->point - cam_.pos) > 0) { Vec3 g; p.w = ghost(g) ? g : ttFreePoint(mp, m); return p; } }
  if (!r) {                                                       // (off the mesh — beside its rim: its nearest vertex / rim edge still)
    auto T = topo::ttopo(baseSp(*it)); const auto W = ttWorld(m); double dv = 10, de = 8; int bv = -1, be = -1; double bt = 0;
    const auto sh = shownVerts(*it, *baseOf(*it)); auto shown = [&](int v) { return sh.empty() || (size_t(v) < sh.size() && sh[size_t(v)]); };
    // (not one behind the surface — the far side of the reference or of the mesh, seen through it)
    for (int v = 0; v < T->n; v++) { if (!shown(v)) continue; ScreenPt s = P.toScreen(W[size_t(v)]); const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (s.z < 1 && d < dv && ttSeen(W[size_t(v)], m)) { dv = d; bv = v; } }
    if (bv >= 0) { p.v = bv; p.w = W[size_t(bv)]; return p; }
    for (int e = 0; e < T->ne; e++) { if (T->ef1[size_t(e)] >= 0 || !shown(T->ea[size_t(e)]) || !shown(T->eb[size_t(e)])) continue; ScreenPt a = P.toScreen(W[size_t(T->ea[size_t(e)])]), b = P.toScreen(W[size_t(T->eb[size_t(e)])]); double t; const double d = segDistT(mp.x(), mp.y(), a.x, a.y, b.x, b.y, &t);
      if (a.z < 1 && b.z < 1 && d < de && t > 0.02 && t < 0.98 && ttSeen(W[size_t(T->ea[size_t(e)])] + (W[size_t(T->eb[size_t(e)])] - W[size_t(T->ea[size_t(e)])]) * t, m)) { de = d; be = e; bt = t; } }
    if (be >= 0) { p.ea = T->ea[size_t(be)]; p.eb = T->eb[size_t(be)]; p.t = bt; p.w = W[size_t(p.ea)] + (W[size_t(p.eb)] - W[size_t(p.ea)]) * bt; return p; }
    { Vec3 g; p.w = ghost(g) ? g : ttFreePoint(mp, m); } return p;
  }
  { // (a vertex near the pointer first — any of the mesh's, not hidden behind the surface hit: one under another polygon's edge too)
    auto T0 = topo::ttopo(baseSp(*it)); const auto W0 = ttWorld(m); const double dh = cam_.ortho() ? 0 : r->point.distanceTo(cam_.pos);
    double bd = 10; int bv0 = -1; const auto sh = shownVerts(*it, *baseOf(*it));
    for (int v = 0; v < T0->n; v++) { if (!sh.empty() && (size_t(v) >= sh.size() || !sh[size_t(v)])) continue; const ScreenPt s = P.toScreen(W0[size_t(v)]); if (s.z >= 1) continue; const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (d >= bd) continue;
      if (!cam_.ortho() && W0[size_t(v)].distanceTo(cam_.pos) > dh * 1.03 + 1e-6) continue;   // (behind what the pointer is on)
      bd = d; bv0 = v; }
    if (bv0 >= 0) { p.v = bv0; p.w = W0[size_t(bv0)]; return p; } }
  auto T = topo::ttopo(baseSp(*it)); const auto W = ttWorld(m);
  double dv = 1e300, de = 1e300, bt = 0; int bv = -1, ba = -1, bb = -1;
  for (int k = T->fo[size_t(r->f)]; k < T->fo[size_t(r->f) + 1]; k++) {
    const int nk = k + 1 < T->fo[size_t(r->f) + 1] ? k + 1 : T->fo[size_t(r->f)]; const int a = T->fv[size_t(k)], b = T->fv[size_t(nk)];
    ScreenPt sa = P.toScreen(W[size_t(a)]), sb = P.toScreen(W[size_t(b)]);
    const double d0 = std::hypot(sa.x - mp.x(), sa.y - mp.y()); if (d0 < dv) { dv = d0; bv = a; }
    double t; const double d1 = segDistT(mp.x(), mp.y(), sa.x, sa.y, sb.x, sb.y, &t); if (d1 < de) { de = d1; ba = a; bb = b; bt = t; }
  }
  if (dv < 10) { p.v = bv; p.w = W[size_t(bv)]; return p; }
  if (de < 8 && bt > 0.02 && bt < 0.98) { p.ea = ba; p.eb = bb; p.t = bt; p.w = W[size_t(ba)] + (W[size_t(bb)] - W[size_t(ba)]) * bt; return p; }
  { Vec3 g; p.w = ghost(g) ? g : r->point; } tt_.hf = r->f; return p;
}

// Symmetry on, an edit that made polygons: each new one (from nf0) mirrored across the mesh's X = 0 — a vertex near the
// plane (within a sixth of the new polygons' edges) put on it and shared, the mirrored one there already used (the
// nearest within a tenth of an edge), else made; a mirror that is the polygon itself or one there already skipped
void symAddFaces(MeshEd& E, int nf0) {
  if (E.pos.empty() || int(E.f.size()) <= nf0) return;
  double sl = 0; int ne = 0;
  for (int fi = nf0; fi < int(E.f.size()); fi++) { const auto& V = E.f[size_t(fi)].v; for (size_t k = 0; k < V.size(); k++) { sl += E.pos[size_t(V[k])].distanceTo(E.pos[size_t(V[(k + 1) % V.size()])]); ne++; } }
  // (eps: the other side's vertex taken for the mirror within a third of an edge — that side edited by hand apart from
  // this one is never exact; past that a new vertex would make a sliver beside the one there)
  const double L = std::max(1e-9, ne ? sl / ne : 1.0), eps = L * 0.3, plane = L / 6;
  // (what is made on both sides already — a tube round a body on the plane, a patch across it — has its mirror: none made;
  // a polygon lying across the plane mirrors onto itself — left as it is)
  std::vector<char> one(E.f.size(), 0); bool posSide = false, negSide = false;
  for (int fi = nf0; fi < int(E.f.size()); fi++) { bool p = false, q = false; for (int v : E.f[size_t(fi)].v) { if (E.pos[size_t(v)].x > plane) p = true; if (E.pos[size_t(v)].x < -plane) q = true; }
    if (p != q) { one[size_t(fi)] = 1; (p ? posSide : negSide) = true; } }
  if (posSide && negSide) return;
  for (int fi = nf0; fi < int(E.f.size()); fi++) if (one[size_t(fi)]) for (int v : E.f[size_t(fi)].v) if (std::abs(E.pos[size_t(v)].x) <= plane) E.pos[size_t(v)].x = 0;   // (onto the plane)
  std::unordered_map<long long, std::vector<int>> grid; auto cell = [&](const Vec3& p, int dx, int dy, int dz) { return ((long long)(std::floor(p.x / eps) + dx) * 73856093LL) ^ ((long long)(std::floor(p.y / eps) + dy) * 19349663LL) ^ ((long long)(std::floor(p.z / eps) + dz) * 83492791LL); };
  for (size_t v = 0; v < E.pos.size(); v++) grid[cell(E.pos[v], 0, 0, 0)].push_back(int(v));
  auto mirrorOf = [&](int v) { const Vec3 p = E.pos[size_t(v)]; if (p.x == 0) return v; const Vec3 q(-p.x, p.y, p.z); int best = -1; double bd = eps;
    for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) { auto g = grid.find(cell(q, dx, dy, dz)); if (g == grid.end()) continue; for (int w : g->second) if (const double d = E.pos[size_t(w)].distanceTo(q); w != v && d <= bd) { bd = d; best = w; } }
    if (best >= 0) return best;
    const int w = E.addVert(q); grid[cell(q, 0, 0, 0)].push_back(w); return w; };
  const int n0 = int(E.f.size());
  for (int fi = nf0; fi < n0; fi++) {
    if (!one[size_t(fi)]) continue;
    const auto V = E.f[size_t(fi)].v; std::vector<int> M; for (int v : V) M.push_back(mirrorOf(v)); std::reverse(M.begin(), M.end());
    std::set<int> a(V.begin(), V.end()), b(M.begin(), M.end()); if (a == b || b.size() < 3) continue;
    bool exists = false; for (int x : E.facesOfVert(M[0])) { std::set<int> c(E.f[size_t(x)].v.begin(), E.f[size_t(x)].v.end()); if (c == b) { exists = true; break; } }
    if (!exists) E.addFace(M, E.f[size_t(fi)].g, E.f[size_t(fi)].mat);
  }
}

// an edit of the tools done: the mesh written (the file's normals made anew if it had them), the selection set, one
// undo step (commitIt: else only shown — a live drag)
QString Viewport::ttDone(int m, const MeshEd& E_, const PolyMesh& src, const QString& msg, char selKey, const std::vector<int>& sel, const std::vector<std::pair<int, int>>& selE, bool commitIt) {
  // (the polygons a building tool made: into the polygroup picked in the list — else a neighbour's, as MeshEd gives them)
  MeshEd Eg; const bool regroup = (tool_ == "tpen" || tool_ == "tpatch" || tool_ == "tfill" || tool_ == "tbridge") && tgSel.id == m && tgSel.gi >= 0 && tgSel.gi < int(src.groupNames.size()) && int(E_.f.size()) > src.faceCount();
  if (regroup) { Eg = E_; for (size_t i = size_t(src.faceCount()); i < Eg.f.size(); i++) Eg.f[i].g = tgSel.gi; }
  // (none picked: a new polygon in a hidden polygroup — the first one, given to a polygon with no neighbour — goes into the
  // newest shown one instead: it would vanish as it is made)
  else if ((tool_ == "tpen" || tool_ == "tpatch" || tool_ == "tfill" || tool_ == "tbridge") && int(E_.f.size()) > src.faceCount()) if (const Item* it = scene_->item(m)) {
    std::set<int> hid; for (const QJsonValue& x : it->raw.value("hidGroups").toArray()) hid.insert(x.toInt());
    int shown = -1; for (int g = int(src.groupNames.size()) - 1; g >= 0 && shown < 0; g--) if (!hid.count(g)) shown = g;
    if (!hid.empty() && shown >= 0) { bool any = false; for (size_t i = size_t(src.faceCount()); i < E_.f.size(); i++) if (hid.count(E_.f[i].g)) any = true;
      if (any) { Eg = E_; for (size_t i = size_t(src.faceCount()); i < Eg.f.size(); i++) if (hid.count(Eg.f[i].g)) Eg.f[i].g = shown; } } }
  const bool regrouped = regroup || !Eg.f.empty();
  const MeshEd& E = regrouped ? Eg : E_;
  MeshEd Es; const bool mir = tt_.symNew && tset.sym && room() == "topo" && int(E.f.size()) > src.faceCount(); tt_.symNew = false;   // (Symmetry: the polygons made — mirrored too)
  if (mir) { Es = E; symAddFaces(Es, src.faceCount()); }
  std::vector<int> vmap; PolyMesh d = (mir ? Es : E).to(src, &vmap); QString keepMode;
  // (the file's own normals carried along — a new corner's, left (0,0,0), made from the shape round it)
  if (d.cornerNormal.size() == d.faceVerts.size() && !d.cornerNormal.empty()) { std::vector<Vec3> cn; for (auto& n : d.cornerNormal) if (n[0] == 0 && n[1] == 0 && n[2] == 0) { cn = hardCornerNormals(d); break; }
    if (!cn.empty()) for (size_t k = 0; k < cn.size(); k++) { auto& n = d.cornerNormal[k]; if (n[0] == 0 && n[1] == 0 && n[2] == 0) n = {float(cn[k].x), float(cn[k].y), float(cn[k].z)}; } }
  auto nb = std::make_shared<PolyMesh>(std::move(d));
  // (the selection the edit didn't set itself: kept on what is still where it was — by the places of its corners)
  using Q = std::array<long long, 3>; std::set<std::vector<Q>> keepF; std::set<Q> keepV; std::set<std::pair<Q, Q>> keepE; char keepKey = 0;
  std::set<std::vector<int>> keepFi; std::set<std::pair<int, int>> keepEi; std::set<int> keepVi;   // (and by the vertices themselves: one moved keeps its polygons picked)
  const double qq = std::max(1e-9, src.bounds().size().length() * 1e-5);
  auto qk = [qq](const Vec3& p) { return Q{std::llround(p.x / qq), std::llround(p.y / qq), std::llround(p.z / qq)}; };
  auto vm = [&](int v) { return size_t(v) < vmap.size() ? vmap[size_t(v)] : -1; };
  // (PolyPen with a selection there: it stays as it was — what the pen made isn't picked instead)
  const bool penKeep = selKey && tool_ == "tpen" && topoMode != "object" && tsel_.id == m && (!tsel_.f.empty() || !tsel_.e.empty() || !tsel_.v.empty());
  if (penKeep) selKey = 0;
  if (!selKey && tsel_.id == m && (!tsel_.f.empty() || !tsel_.e.empty() || !tsel_.v.empty())) if (Item* it0 = scene_->item(m)) {
    auto T0 = topo::ttopo(baseSp(*it0)); const auto& P0 = baseSp(*it0)->pos; keepKey = tKey(); const bool same = baseSp(*it0).get() == &src;
    if (keepKey == 'f') for (int f : tsel_.f) { if (f < 0 || f >= T0->nf) continue; std::vector<Q> k; std::vector<int> ki; bool ok = same;
      for (int j = T0->fo[size_t(f)]; j < T0->fo[size_t(f) + 1]; j++) { const int v = T0->fv[size_t(j)]; k.push_back(qk(P0[size_t(v)])); ki.push_back(vm(v)); if (vm(v) < 0) ok = false; }
      std::sort(k.begin(), k.end()); keepF.insert(k); if (ok) { std::sort(ki.begin(), ki.end()); keepFi.insert(ki); } }
    else if (keepKey == 'e') for (int e : tsel_.e) { if (e < 0 || e >= T0->ne) continue; Q a = qk(P0[size_t(T0->ea[size_t(e)])]), b = qk(P0[size_t(T0->eb[size_t(e)])]); if (b < a) std::swap(a, b); keepE.insert({a, b});
      const int ia = vm(T0->ea[size_t(e)]), ib = vm(T0->eb[size_t(e)]); if (same && ia >= 0 && ib >= 0) keepEi.insert({std::min(ia, ib), std::max(ia, ib)}); }
    else for (int v : tsel_.v) if (v >= 0 && size_t(v) < P0.size()) { keepV.insert(qk(P0[size_t(v)])); if (same && vm(v) >= 0) keepVi.insert(vm(v)); }
  }
  topoWrite(m, nb);
  const int ver = tsel_.ver + 1; tsel_ = {}; tsel_.id = m; tsel_.B = nb.get(); tsel_.ver = ver;
  if (keepKey) { auto T1 = topo::ttopo(nb); const auto& P1 = nb->pos;
    if (keepKey == 'f') { for (int f = 0; f < T1->nf; f++) { std::vector<Q> k; std::vector<int> ki; for (int j = T1->fo[size_t(f)]; j < T1->fo[size_t(f) + 1]; j++) { k.push_back(qk(P1[size_t(T1->fv[size_t(j)])])); ki.push_back(T1->fv[size_t(j)]); }
      std::sort(k.begin(), k.end()); std::sort(ki.begin(), ki.end()); if (keepF.count(k) || keepFi.count(ki)) tsel_.f.insert(f); } }
    else if (keepKey == 'e') { for (int e = 0; e < T1->ne; e++) { const int ia = T1->ea[size_t(e)], ib = T1->eb[size_t(e)]; Q a = qk(P1[size_t(ia)]), b = qk(P1[size_t(ib)]); if (b < a) std::swap(a, b);
      if (keepE.count({a, b}) || keepEi.count({std::min(ia, ib), std::max(ia, ib)})) tsel_.e.insert(e); } }
    else for (int v = 0; v < int(P1.size()); v++) if (keepV.count(qk(P1[size_t(v)])) || keepVi.count(v)) tsel_.v.insert(v); }
  if (selKey) {
    const QString mode = selKey == 'f' ? "poly" : selKey == 'e' ? "edge" : "vert";
    // (PolyPen: an edit mode stays as it is — what it made picked in that mode; from Object — the last edit mode)
    keepMode = tool_ == "tpen" && topoMode != mode ? (topoMode == "object" ? lastEditMode_ : topoMode) : QString();
    if (topoMode != mode) { topoMode = mode; updateGizmo(); }
    if (selKey == 'f') tsel_.f.insert(sel.begin(), sel.end());
    else if (selKey == 'v') { for (int v : sel) if (size_t(v) < vmap.size() && vmap[size_t(v)] >= 0) tsel_.v.insert(vmap[size_t(v)]); }
    else { auto T2 = topo::ttopo(nb); for (auto& [a, b] : selE) if (size_t(a) < vmap.size() && size_t(b) < vmap.size() && vmap[size_t(a)] >= 0 && vmap[size_t(b)] >= 0) if (int e = T2->edgeOf(vmap[size_t(a)], vmap[size_t(b)]); e >= 0) tsel_.e.insert(e); }
  }
  if (!keepMode.isEmpty() && keepMode != topoMode) tConvertSel(keepMode);
  thov_ = {}; tt_.hov = {}; ttTouch(); updateGizmo();
  if (!commitIt) { emit topoState(); return msg; }
  tt_.lastAfter = nb; tt_.lastM = m;
  scene_->revision++; commit(true); emit topoState(); if (!msg.isEmpty()) ui::toast(msg);
  return msg;
}

// ---------- PolyPen ----------
namespace {
// (a point of the pen as a vertex of E: a vertex; on an edge — the edge cut there; an anchor / free — a new vertex)
int resolvePt(MeshEd& E, const Viewport::TTPt& p, const Affine& Mi, const std::vector<Vec3>& anchors) {
  if (p.v >= 0) return p.v;
  if (p.ea >= 0) { if (E.edgeExists(p.ea, p.eb)) return E.splitEdge(p.ea, p.eb, p.t); return E.addVert(E.pos[size_t(p.ea)] + (E.pos[size_t(p.eb)] - E.pos[size_t(p.ea)]) * p.t); }
  return E.addVert(Mi.apply(p.anchor >= 0 && size_t(p.anchor) < anchors.size() ? anchors[size_t(p.anchor)] : p.w));
}
bool samePt(const Viewport::TTPt& a, const Viewport::TTPt& b) {
  if (a.v >= 0 || b.v >= 0) return a.v == b.v; if (a.anchor >= 0 || b.anchor >= 0) return a.anchor == b.anchor;
  if (a.ea >= 0 && b.ea >= 0) return ek(a.ea, a.eb) == ek(b.ea, b.eb) && std::abs((a.ea == b.ea ? a.t : 1 - a.t) - b.t) < 0.05;
  return false;
}
// (a new polygon sharing no edge with another: its front to the camera — else it lies as the clicks went, often turned away)
void faceCam(MeshEd& E, int fi, const Affine& M, const Camera& cam) {
  const auto& N = E.f[size_t(fi)].v; const size_t n = N.size();
  for (size_t k = 0; k < n; k++) { const int a = N[k], b = N[(k + 1) % n];
    for (size_t i = 0; i < E.f.size(); i++) { if (int(i) == fi) continue; const auto& V = E.f[i].v; const size_t m = V.size();
      for (size_t j = 0; j < m; j++) if ((V[j] == a && V[(j + 1) % m] == b) || (V[j] == b && V[(j + 1) % m] == a)) return; } }
  const Vec3 c = M.apply(E.center(fi)), nw = M.lin(E.normal(fi)); const Vec3 view = cam.ortho() ? cam.dir() : c - cam.pos;
  if (nw.dot(view) <= 0) return;
  auto& F = E.f[size_t(fi)]; std::reverse(F.v.begin(), F.v.end()); if (!F.uv.empty()) std::reverse(F.uv.begin(), F.uv.end()); if (!F.n.empty()) std::reverse(F.n.begin(), F.n.end());
}
// (Symmetry on, a polygon cut from va to vb: the mirrored polygon cut the same way — each end's mirror a vertex there, else
// the edge there split at it; a cut lying on the plane itself, or one with no mirror found, left as it is)
void symMirrorCut(MeshEd& E, int va, int vb) {
  double sl = 0; int ne = 0;
  for (int v : {va, vb}) for (int fi : E.facesOfVert(v)) { const auto& V = E.f[size_t(fi)].v; for (size_t k = 0; k < V.size(); k++) { sl += E.pos[size_t(V[k])].distanceTo(E.pos[size_t(V[(k + 1) % V.size()])]); ne++; } }
  const double eps = std::max(1e-9, (ne ? sl / ne : 1.0) * 0.05);
  auto mirrorAt = [&](int v) -> int {
    const Vec3 q(-E.pos[size_t(v)].x, E.pos[size_t(v)].y, E.pos[size_t(v)].z); if (std::abs(E.pos[size_t(v)].x) <= eps) return v;
    int best = -1; double bd = eps; for (size_t w = 0; w < E.pos.size(); w++) if (const double d = E.pos[w].distanceTo(q); d <= bd) { bd = d; best = int(w); }
    if (best >= 0) return best;
    int ea = -1, eb = -1; double et = 0; bd = eps;   // (on an edge: that edge split there)
    for (const auto& F : E.f) for (size_t k = 0; k < F.v.size(); k++) { const int a = F.v[k], b = F.v[(k + 1) % F.v.size()]; const Vec3 A = E.pos[size_t(a)], D = E.pos[size_t(b)] - A; const double L2 = D.dot(D); if (L2 <= 0) continue;
      const double t = (q - A).dot(D) / L2; if (t <= 0.001 || t >= 0.999) continue; if (const double d = (A + D * t).distanceTo(q); d <= bd) { bd = d; ea = a; eb = b; et = t; } }
    return ea >= 0 ? E.splitEdge(ea, eb, et) : -1; };
  const int ma = mirrorAt(va); if (ma < 0) return; const int mb = mirrorAt(vb); if (mb < 0 || ma == mb) return;
  if ((ma == va && mb == vb) || (ma == vb && mb == va) || E.edgeExists(ma, mb)) return;
  for (size_t fi = 0; fi < E.f.size(); fi++) if (E.corner(int(fi), ma) >= 0 && E.corner(int(fi), mb) >= 0) { E.splitFace(int(fi), ma, mb); return; }
}
bool faceHas(const MeshEd& E, int fi, const Viewport::TTPt& p) {
  if (p.v >= 0) return E.corner(fi, p.v) >= 0;
  if (p.ea < 0) return false; const auto& V = E.f[size_t(fi)].v; const size_t n = V.size();
  for (size_t k = 0; k < n; k++) if (ek(V[k], V[(k + 1) % n]) == ek(p.ea, p.eb)) return true; return false;
}
}  // namespace

QString Viewport::penClick(const TTPt& p, bool dbl) {
  const int m = tt_.m; Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it) {                                                      // (no mesh: the corners where they fall; closed — a new RetopoMesh)
    if (tt_.chain.empty()) { tt_.chain = {p}; ttTouch(); ui::hud(tr("PolyPen: a new RetopoMesh — click the next corners; the first one or a double click closes; Esc drops it")); return {}; }
    auto onFirst = [&] { if (samePt(p, tt_.chain[0])) return true; Projector P = projector(); const ScreenPt a = P.toScreen(tt_.chain[0].anchor >= 0 && size_t(tt_.chain[0].anchor) < tt_.anchors.size() ? tt_.anchors[size_t(tt_.chain[0].anchor)] : tt_.chain[0].w), b = P.toScreen(p.w);
      return a.z < 1 && b.z < 1 && std::hypot(a.x - b.x, a.y - b.y) < 10; };   // (a free point: the first one within 10 px)
    const bool close = (tt_.chain.size() >= 3 && onFirst()) || (dbl && tt_.chain.size() >= 2);
    if (!close) { tt_.chain.push_back(p); ttTouch(); return {}; }
    if (dbl && !onFirst() && !samePt(p, tt_.chain.back()) && !ttNearPx(ttPtW(p), ttPtW(tt_.chain.back()), 6)) tt_.chain.push_back(p);
    if (tt_.chain.size() < 3) return {};
    return penNewMesh();
  }
  const Affine Mi = meshmods::meshMatrix(*scene_, *it).inverse(); const PolyMesh& B = *baseOf(*it);
  auto polygon = [&]() -> QString {
    MeshEd E = MeshEd::from(B); std::vector<int> vs; std::set<int> usedA;
    for (const TTPt& q : tt_.chain) { const int v = resolvePt(E, q, Mi, tt_.anchors); if (vs.empty() || vs.back() != v) vs.push_back(v); if (q.anchor >= 0) usedA.insert(q.anchor); }
    if (vs.size() > 2 && vs.front() == vs.back()) vs.pop_back();
    tt_.chain.clear();
    if (vs.size() < 3) { ttTouch(); return "!"; }
    const int fi = E.addFace(vs); faceCam(E, fi, Mi.inverse(), cam_);
    std::vector<Vec3> keep; for (size_t i = 0; i < tt_.anchors.size(); i++) if (!usedA.count(int(i))) keep.push_back(tt_.anchors[i]); tt_.anchors = keep;
    tt_.symNew = true; return ttDone(m, E, B, tr("A polygon of %1 corners").arg(vs.size()), 'f', {fi});
  };
  if (tt_.chain.empty()) { tt_.chain = {p}; ttTouch(); ui::hud(tr("PolyPen: click the next corners — the first one or a double click closes; Esc drops it")); return {}; }
  if (tt_.chain.size() >= 3 && samePt(p, tt_.chain[0])) return polygon();
  if (tt_.chain.size() == 1 && (p.v >= 0 || p.ea >= 0) && (tt_.chain[0].v >= 0 || tt_.chain[0].ea >= 0)) {
    const TTPt a = tt_.chain[0];
    if (samePt(a, p)) return {};
    MeshEd E = MeshEd::from(B);
    // (both on one polygon, not side by side: that polygon cut between them)
    for (size_t fi = 0; fi < E.f.size(); fi++) {
      if (!faceHas(E, int(fi), a) || !faceHas(E, int(fi), p)) continue;
      if (a.v >= 0 && p.v >= 0 && E.edgeExists(a.v, p.v)) break;
      if ((a.ea >= 0 && p.ea >= 0 && ek(a.ea, a.eb) == ek(p.ea, p.eb))) break;
      const int va = resolvePt(E, a, Mi, tt_.anchors), vb = resolvePt(E, p, Mi, tt_.anchors);
      const int nf = E.splitFace(int(fi), va, vb); if (nf < 0) break;
      if (tset.sym && room() == "topo") symMirrorCut(E, va, vb);   // (Symmetry: the mirror polygon cut too)
      tt_.chain.clear();
      return ttDone(m, E, B, tr("The polygon cut in two"), 'e', {}, {{va, vb}});
    }
    // (both on the rim, a triangle's / a quad's walk apart: the gap filled)
    auto T = topo::ttopo(baseSp(*it));
    auto ends = [&](const TTPt& q) { return q.v >= 0 ? std::set<int>{q.v} : std::set<int>{q.ea, q.eb}; };
    auto rimOK = [&](const TTPt& q) { return q.v >= 0 || (T->edgeOf(q.ea, q.eb) >= 0 && T->ef1[size_t(T->edgeOf(q.ea, q.eb))] < 0); };
    if (rimOK(a) && rimOK(p)) {
      const int extra = (a.ea >= 0) + (p.ea >= 0);
      std::vector<int> best;
      for (int s : ends(a)) { auto path = rimPath(*T, s, ends(p), 3 - extra, -1);
        if (!path.empty() && (best.empty() || path.size() < best.size())) best = path; }
      if (!best.empty() && best.size() + size_t(extra) >= 3) {
        const int va = a.ea >= 0 ? resolvePt(E, a, Mi, tt_.anchors) : -1, vb = p.ea >= 0 ? resolvePt(E, p, Mi, tt_.anchors) : -1;
        std::vector<int> vs; if (va >= 0) vs.push_back(va); vs.insert(vs.end(), best.begin(), best.end()); if (vb >= 0) vs.push_back(vb);
        tt_.chain.clear(); const int fi = E.addFace(vs); faceCam(E, fi, Mi.inverse(), cam_);
        tt_.symNew = true; return ttDone(m, E, B, tr("The gap filled: a polygon of %1 corners").arg(vs.size()), 'f', {fi});
      }
    }
  }
  // (the chain came onto a vertex of the rim one or two rim edges from the vertex it started on: the contour is closed
  // by them — the polygon made with those edges as its sides)
  if (!dbl && tt_.chain.size() >= 2 && p.v >= 0 && tt_.chain[0].v >= 0 && p.v != tt_.chain[0].v) {
    auto T = topo::ttopo(baseSp(*it)); const auto path = rimPath(*T, p.v, {tt_.chain[0].v}, 2, -1);
    bool clash = path.size() < 2; for (size_t i = 1; i < tt_.chain.size() && !clash; i++) for (size_t j = 1; j + 1 < path.size(); j++) if (tt_.chain[i].v == path[j]) clash = true;
    if (!clash) { tt_.chain.push_back(p); for (size_t j = 1; j + 1 < path.size(); j++) { TTPt q; q.v = path[j]; q.w = ttWorld(m)[size_t(path[j])]; tt_.chain.push_back(q); }
      return polygon(); }
  }
  // (a double click's second press lands where the first put its point: not a corner again)
  if (!(dbl && (samePt(p, tt_.chain.back()) || ttNearPx(ttPtW(p), ttPtW(tt_.chain.back()), 6)))) tt_.chain.push_back(p);
  if (dbl && tt_.chain.size() >= 3) return polygon();
  ttTouch(); return {};
}

// the polygon drawn with no mesh picked: a new mesh of it (RetopoMesh, RetopoMesh 2…) — facing the camera — picked, the
// PolyPen going on in it
QString Viewport::penNewMesh() {
  std::vector<Vec3> L; std::set<int> usedA;
  for (const TTPt& q : tt_.chain) { const Vec3 w = q.anchor >= 0 && size_t(q.anchor) < tt_.anchors.size() ? tt_.anchors[size_t(q.anchor)] : q.w; if (q.anchor >= 0) usedA.insert(q.anchor);
    if (L.empty() || L.back().distanceTo(w) > 1e-9) L.push_back(w); }
  if (L.size() > 2 && L.front().distanceTo(L.back()) <= 1e-9) L.pop_back();
  tt_.chain.clear(); if (L.size() < 3) { ttTouch(); return "!"; }
  Vec3 n; for (size_t k = 0; k < L.size(); k++) { const Vec3& a = L[k]; const Vec3& b = L[(k + 1) % L.size()]; n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)); }
  if (n.dot(cam_.dir()) > 0) std::reverse(L.begin(), L.end());   // (its front to the camera)
  PolyMesh d; d.pos = L; std::vector<int> vs(L.size()); std::iota(vs.begin(), vs.end(), 0); d.addFace(vs.data(), int(vs.size())); d.faceGroup = {0}; d.groupNames = {"Polygroup"};
  if (tset.sym && room() == "topo") { MeshEd E = MeshEd::from(d); symAddFaces(E, 0); d = E.to(d); }   // (Symmetry: its mirror too)
  std::set<QString> taken; for (auto& [id, it] : scene_->items) taken.insert(it.name);
  QString name = "RetopoMesh"; for (int i = 2; taken.count(name); i++) name = QString("RetopoMesh %1").arg(i);
  const int id = topoNewMesh(-1, name, d); if (id < 0) return "!";
  std::vector<Vec3> keep; for (size_t i = 0; i < tt_.anchors.size(); i++) if (!usedA.count(int(i))) keep.push_back(tt_.anchors[i]); tt_.anchors = keep;
  sel_ = {}; sel_.items = {id}; tt_.m = id; onSelChange();
  scene_->revision++; emit sceneEdited(); commit(true); emit topoState(); ttTouch();
  const QString msg = tr("%1: a new mesh — its first polygon of %2 corners").arg(name).arg(L.size()); ui::toast(msg); return msg;
}

// snap on: a world point put onto the other shown meshes where the camera sees it — along the view through it (the
// surface under it on screen), else their nearest point
Vec3 Viewport::ttSnapW(const Vec3& w, int m) const {
  ProfScope prof_("ttSnapW");
  std::vector<std::shared_ptr<const topo::Bvh>> tg;
  for (auto& [id, t] : scene_->items) if (id != m && t.type == ItemType::IMesh && t.poly && scene_->isVisible(t)) if (auto b = bvhOf(id, true)) tg.push_back(b);
  if (tg.empty()) return w;
  const Vec3 d = cam_.ortho() ? cam_.dir() : (w - cam_.pos).normalized(); const Vec3 o = cam_.ortho() ? w - d * 1e5 : cam_.pos;
  double bt = 1e300; for (auto& b : tg) if (auto t = soloHit(*b, o, d); t && *t < bt) bt = *t;   // (first: what is seen of a reference on its own)
  if (bt < 1e300) return o + d * bt;
  const SideGuard sg = sideGuard(m); for (auto& b : tg) if (auto h = frontHit(*b, o, d, 1e12, sg); h && h->first < bt) bt = h->first;   // (only a side turned to the camera, the built mesh's side)
  if (bt < 1e300) return o + d * bt;
  double bd = 1e300; Vec3 best = w; for (auto& b : tg) { double dd; auto q = b->closest(w, [&](int t) { if (!facesEye(*b, t, cam_.pos, cam_.dir(), cam_.ortho())) return false; const Vec3 &a = b->P[b->idx[size_t(t) * 3]], &bb = b->P[b->idx[size_t(t) * 3 + 1]], &c = b->P[b->idx[size_t(t) * 3 + 2]]; return sg.ok((a + bb + c) * (1.0 / 3), (bb - a).cross(c - a)); }, &dd); if (q && dd < bd) { bd = dd; best = *q; } }
  return best;
}
bool Viewport::ttSeen(const Vec3& w, int m) const {
  const Item* it = scene_->item(m); const double big = it && it->poly ? it->poly->bounds().size().length() : 1.0, tol = std::max(1e-6, big * 1e-3);
  const Vec3 o = cam_.ortho() ? w - cam_.dir() * (big * 10) : cam_.pos; const double L = o.distanceTo(w); if (L <= tol) return true; const Vec3 d = (w - o) * (1.0 / L);
  if (auto b = bvhOf(m, false); b && b->ray(o, d, L - tol)) return false;
  const double tolR = std::max(big * 0.01, worldPerPixelAt(w) * 4);   // (the built mesh lies on the references, a little under them too: a hit right by the point — not in front of it)
  for (auto& [id, t] : scene_->items) if (id != m && t.type == ItemType::IMesh && t.poly && scene_->isVisible(t)) if (auto b = bvhOf(id, true); b && b->ray(o, d, L - tolR)) return false;
  return true;
}
bool Viewport::ttNearPx(const Vec3& a, const Vec3& b, double px) const {
  Projector P = projector(); const ScreenPt s = P.toScreen(a), t = P.toScreen(b);
  return s.z < 1 && t.z < 1 && std::hypot(s.x - t.x, s.y - t.y) < px;
}
Vec3 Viewport::ttPtW(const TTPt& p) const {
  if (p.anchor >= 0 && size_t(p.anchor) < tt_.anchors.size()) return tt_.anchors[size_t(p.anchor)];
  const int m = tt_.m; const Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (it && (p.v >= 0 || p.ea >= 0)) { const auto W = ttWorld(m); if (p.v >= 0 && size_t(p.v) < W.size()) return W[size_t(p.v)];
    if (size_t(std::max(p.ea, p.eb)) < W.size()) return W[size_t(p.ea)] + (W[size_t(p.eb)] - W[size_t(p.ea)]) * p.t; }
  return p.w;
}

// Shift+RMB: the points round the pointer — the anchors and the rim's vertices, the nearest first (out to many times the
// fill radius): the 4 (else 3) of the nearest few that hold the pointer between them, the nearest such
std::vector<Viewport::TTPt> Viewport::penFillPick(const QPointF& mp) const {
  struct C { TTPt p; double x, y, d; }; std::vector<C> cs; Projector P = projector(); const double R = std::max(ttools.penFill * 6, 400.0);
  for (size_t i = 0; i < tt_.anchors.size(); i++) { ScreenPt s = P.toScreen(tt_.anchors[i]); double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (s.z < 1 && d < R) { TTPt q; q.anchor = int(i); q.w = tt_.anchors[i]; cs.push_back({q, s.x, s.y, d}); } }
  const int m = (tool_ == "tnormals" || (tool_ == "tweld" && drag_ != Drag::Topo)) && topoMesh() >= 0 ? topoMesh()   // (Normals / Weld: the selected mesh — never the one an earlier action left in tt_.m)
              : tt_.m >= 0 && scene_->item(tt_.m) ? tt_.m : topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr;
  std::set<int> holeV; for (const TTPt& q : penFillHole(mp)) holeV.insert(q.v);   // (the pointer in a hole of the mesh: only its vertices — not the rims round about)
  if (it) { const auto W = ttWorld(m); auto T = topo::ttopo(it->base ? it->base : it->poly);
    for (int v = 0; v < T->n; v++) { bool rim = false; for (int j = T->voff[size_t(v)]; j < T->voff[size_t(v) + 1]; j++) if (T->ef1[size_t(T->vE[size_t(j)])] < 0) rim = true;
      if (!rim && T->voff[size_t(v) + 1] > T->voff[size_t(v)]) continue; if (!holeV.empty() && !holeV.count(v)) continue;
      ScreenPt s = P.toScreen(W[size_t(v)]); double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (s.z < 1 && d < R) { TTPt q; q.v = v; q.w = W[size_t(v)]; cs.push_back({q, s.x, s.y, d}); } } }
  std::sort(cs.begin(), cs.end(), [](const C& a, const C& b) { return a.d < b.d; });
  { std::vector<C> kept; for (auto& c : cs) { if (kept.size() >= 12) break; if (c.p.anchor >= 0 || ttSeen(c.p.w, m)) kept.push_back(c); } cs = kept; }   // (a rim vertex behind the surface — the far side seen through it: none of the pick)
  // (the polygons there are: their middles on screen — a pick covering one is no fill of a gap)
  std::vector<std::pair<double, double>> mids; std::vector<std::vector<std::pair<double, double>>> fpolys;   // (their outlines too)
  if (it) { const auto W = ttWorld(m); auto T = topo::ttopo(it->base ? it->base : it->poly);
    for (int f = 0; f < T->nf; f++) { Vec3 c; const int s0 = T->fo[size_t(f)], e0 = T->fo[size_t(f) + 1]; std::vector<std::pair<double, double>> fp; bool front = true;
      for (int k = s0; k < e0; k++) { c += W[size_t(T->fv[size_t(k)])]; const ScreenPt q = P.toScreen(W[size_t(T->fv[size_t(k)])]); if (q.z >= 1) front = false; fp.push_back({q.x, q.y}); }
      c = c * (1.0 / double(e0 - s0)); const ScreenPt s = P.toScreen(c); if (!front || s.z >= 1 || std::hypot(s.x - mp.x(), s.y - mp.y()) > R) continue;
      { Vec3 fn; for (int k = s0; k < e0; k++) { const Vec3& a = W[size_t(T->fv[size_t(k)])]; const Vec3& b = W[size_t(T->fv[size_t(k + 1 < e0 ? k + 1 : s0)])]; fn.x += (a.y - b.y) * (a.z + b.z); fn.y += (a.z - b.z) * (a.x + b.x); fn.z += (a.x - b.x) * (a.y + b.y); }
        if (fn.dot(cam_.ortho() ? cam_.dir() : c - cam_.pos) > 0) continue; }   // (turned away — the far side seen through a hole: no polygon there to keep off)
      mids.push_back({s.x, s.y}); fpolys.push_back(std::move(fp)); } }
  auto cross = [](std::pair<double, double> a, std::pair<double, double> b, std::pair<double, double> c, std::pair<double, double> d) {   // (two sides crossing, not at their ends)
    auto o = [](std::pair<double, double> p, std::pair<double, double> q, std::pair<double, double> r) { return (q.first - p.first) * (r.second - p.second) - (q.second - p.second) * (r.first - p.first); };
    const double d1 = o(a, b, c), d2 = o(a, b, d), d3 = o(c, d, a), d4 = o(c, d, b), eps = 1e-6;
    return ((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) && ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps)); };
  auto holds = [&](std::vector<C> pick) { std::sort(pick.begin(), pick.end(), [&](const C& a, const C& b) { return std::atan2(a.y - mp.y(), a.x - mp.x()) < std::atan2(b.y - mp.y(), b.x - mp.x()); });
    std::vector<std::pair<double, double>> poly; for (auto& c : pick) poly.push_back({c.x, c.y}); if (!insidePolygon(poly, mp.x(), mp.y())) return std::vector<C>{};
    for (auto& q : mids) if (insidePolygon(poly, q.first, q.second)) return std::vector<C>{};   // (over a polygon there is)
    double gx = 0, gy = 0; for (auto& p : poly) { gx += p.first; gy += p.second; } gx /= double(poly.size()); gy /= double(poly.size());
    for (size_t i = 0; i < poly.size(); i++) { const auto a = poly[i], b = poly[(i + 1) % poly.size()];
      // (the side's middle stepped 3 px into the pick: a side shared with a polygon there is lies on its edge — not through it)
      std::pair<double, double> mid{(a.first + b.first) / 2, (a.second + b.second) / 2}; { const double dx = gx - mid.first, dy = gy - mid.second, l = std::hypot(dx, dy); if (l > 0) { mid.first += dx / l * std::min(3.0, l * 0.5); mid.second += dy / l * std::min(3.0, l * 0.5); } }
      for (auto& fp : fpolys) { if (insidePolygon(fp, mid.first, mid.second)) return std::vector<C>{};   // (a side through a polygon there is)
        for (size_t j = 0; j < fp.size(); j++) if (cross(a, b, fp[j], fp[(j + 1) % fp.size()])) return std::vector<C>{}; } }
    for (auto& c : cs) { bool in = false; for (auto& p : pick) if (&p != &c && p.x == c.x && p.y == c.y) in = true; if (!in && insidePolygon(poly, c.x, c.y)) return std::vector<C>{}; }   // (a point inside it)
    return pick; };
  for (size_t k : {size_t(4), size_t(3)}) {
    if (cs.size() < k) continue;
    std::vector<C> best; double bs = 1e300; const size_t n = cs.size();
    std::vector<int> idx(k); std::function<void(size_t, size_t)> rec = [&](size_t start, size_t depth) {
      if (depth == k) { std::vector<C> pick; double s = 0; for (int i : idx) { pick.push_back(cs[size_t(i)]); s += cs[size_t(i)].d; } if (s >= bs) return; auto h = holds(pick); if (!h.empty()) { bs = s; best = h; } return; }
      for (size_t i = start; i < n; i++) { idx[depth] = int(i); rec(i + 1, depth + 1); } };
    rec(0, 0);
    if (!best.empty()) { std::vector<TTPt> out; for (auto& c : best) out.push_back(c.p); return out; }
  }
  return {};
}

// Shift+RMB with nothing to pick round the pointer: the hole in the mesh the pointer is in (an open outline round it on
// screen — the smallest such), its vertices in turn (up to 24 — a bigger hole: Fill Hole)
std::vector<Viewport::TTPt> Viewport::penFillHole(const QPointF& mp) const {
  const int m = tt_.m >= 0 && scene_->item(tt_.m) ? tt_.m : topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return {};
  auto T = topo::ttopo(it->base ? it->base : it->poly); const auto W = ttWorld(m); Projector P = projector();
  std::vector<std::vector<int>> nb(size_t(T->n));
  for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] < 0) { nb[size_t(T->ea[size_t(e)])].push_back(T->eb[size_t(e)]); nb[size_t(T->eb[size_t(e)])].push_back(T->ea[size_t(e)]); }
  std::set<std::pair<int, int>> used; std::vector<int> best; double bestA = 1e300;
  for (int v0 = 0; v0 < T->n; v0++) for (int first : nb[size_t(v0)]) {
    if (used.count({std::min(v0, first), std::max(v0, first)})) continue;
    std::vector<int> L{v0}; int prev = v0, cur = first; used.insert({std::min(v0, first), std::max(v0, first)});
    for (int guard = 0; cur != v0 && guard < T->n + 2; guard++) { L.push_back(cur); int nx = -1;
      for (int w : nb[size_t(cur)]) if (w != prev && !used.count({std::min(cur, w), std::max(cur, w)})) { nx = w; break; }
      if (nx < 0) break; used.insert({std::min(cur, nx), std::max(cur, nx)}); prev = cur; cur = nx; }
    if (cur != v0 || L.size() < 3) continue;
    std::vector<std::pair<double, double>> sp; bool front = true;
    for (int v : L) { const ScreenPt s = P.toScreen(W[size_t(v)]); if (s.z >= 1) front = false; sp.push_back({s.x, s.y}); }
    if (!front || !insidePolygon(sp, mp.x(), mp.y())) continue;
    double a = 0; for (size_t i = 0, j = sp.size() - 1; i < sp.size(); j = i++) a += sp[j].first * sp[i].second - sp[i].first * sp[j].second;
    if (std::abs(a) < bestA) { bestA = std::abs(a); best = L; } }
  if (best.empty() || best.size() > 24) return {};
  std::vector<TTPt> out; for (int v : best) { TTPt q; q.v = v; q.w = W[size_t(v)]; out.push_back(q); }
  return out;
}

// Shift+RMB's points: the user's anchors first — a pick round the pointer holding one of them; else the ghost's polygon
// under the pointer (each corner onto an anchor or a vertex built there, within a few px); else the rim's vertices
std::vector<Viewport::TTPt> Viewport::penFillPoints(const QPointF& mp) const {
  std::vector<TTPt> user = penFillPick(mp);
  for (const TTPt& q : user) if (q.anchor >= 0) return user;
  std::vector<Vec3> gf; if (!ghostFaceAt(mp, gf)) return user;
  Projector P = projector(); const int m0 = tt_.m >= 0 && scene_->item(tt_.m) ? tt_.m : -1; std::vector<TTPt> pick(gf.size());
  std::vector<std::pair<double, double>> sp; for (const Vec3& w : gf) { const ScreenPt s = P.toScreen(w); sp.push_back({s.x, s.y}); }
  for (size_t k = 0; k < gf.size(); k++) pick[k].w = gf[k];
  // (the user's anchors over the ghost's polygon — inside it or by its outline: each takes the place of the corner nearest it)
  struct AC { double d; size_t a, k; }; std::vector<AC> ac;
  for (size_t i = 0; i < tt_.anchors.size(); i++) { const ScreenPt a = P.toScreen(tt_.anchors[i]); if (a.z >= 1) continue;
    double edge = 1e300; for (size_t k = 0; k < sp.size(); k++) { const auto& p0 = sp[k]; const auto& p1 = sp[(k + 1) % sp.size()]; double t; edge = std::min(edge, segDistT(a.x, a.y, p0.first, p0.second, p1.first, p1.second, &t)); }
    if (!insidePolygon(sp, a.x, a.y) && edge > 15) continue;
    for (size_t k = 0; k < sp.size(); k++) ac.push_back({std::hypot(a.x - sp[k].first, a.y - sp[k].second), i, k}); }
  std::sort(ac.begin(), ac.end(), [](const AC& x, const AC& y) { return x.d < y.d; });
  std::set<size_t> usedA, usedK; for (const AC& c : ac) { if (usedA.count(c.a) || usedK.count(c.k)) continue; usedA.insert(c.a); usedK.insert(c.k); pick[c.k].anchor = int(c.a); pick[c.k].w = tt_.anchors[c.a]; }
  for (size_t k = 0; k < pick.size(); k++) { TTPt& q = pick[k]; const ScreenPt s{sp[k].first, sp[k].second, 0};
    if (q.anchor >= 0) continue;
    if (m0 >= 0) { q.v = ttVertNear(m0, QPointF(s.x, s.y), 4); if (q.v >= 0) q.w = ttWorld(m0)[size_t(q.v)]; }
  }
  return pick;
}

// Shift+RMB in a hole of the mesh (no pick round the pointer): filled from its middle — the user's anchor inside it (the
// nearest its middle), else a point made there on the references — a fan of quads round it (a triangle closing an odd
// count); a hole of 3 or 4 vertices — one polygon
QString Viewport::penFillHoleMake(const QPointF& mp) {
  const auto L = penFillHole(mp); const int m = tt_.m; Item* it = m >= 0 ? scene_->item(m) : nullptr; if (L.size() < 3 || !it) return {};
  MeshEd E = MeshEd::from(*baseOf(*it)); const Affine Mi = meshmods::meshMatrix(*scene_, *it).inverse(); std::vector<int> made;
  std::vector<int> rim; for (const TTPt& q : L) rim.push_back(q.v);
  if (L.size() <= 4) { const int fi = E.addFace(rim); faceCam(E, fi, Mi.inverse(), cam_); made.push_back(fi); }
  else {
    Projector P = projector(); std::vector<std::pair<double, double>> sp; Vec3 c; for (const TTPt& q : L) { const ScreenPt s = P.toScreen(q.w); sp.push_back({s.x, s.y}); c += q.w; } c = c * (1.0 / double(L.size()));
    const ScreenPt cs = P.toScreen(c); int ba = -1; double bd = 1e300;
    { std::vector<double> ds; for (auto& p : sp) ds.push_back(std::hypot(p.first - cs.x, p.second - cs.y)); std::vector<double> sd = ds; std::sort(sd.begin(), sd.end());   // (a hole running on far — a crack along a seam: no fan into it)
      if (sd.back() > sd[sd.size() / 2] * 3) { ui::toast(tr("This hole runs on far (a crack along a seam) — put an anchor in it and fill round the anchor (Shift+RMB between it and the corners), or weld the crack")); return "!"; } }
    for (size_t i = 0; i < tt_.anchors.size(); i++) { const ScreenPt a = P.toScreen(tt_.anchors[i]); if (a.z >= 1 || !insidePolygon(sp, a.x, a.y)) continue; const double d = std::hypot(a.x - cs.x, a.y - cs.y); if (d < bd) { bd = d; ba = int(i); } }
    TTPt mid; if (ba >= 0) { mid.anchor = ba; mid.w = tt_.anchors[size_t(ba)]; }
    else {                                                        // (no anchor: the hole's middle on screen — the surface seen there)
      double ax = 0, ay = 0, aa = 0; for (size_t i = 0, j = sp.size() - 1; i < sp.size(); j = i++) { const double cr = sp[j].first * sp[i].second - sp[i].first * sp[j].second; aa += cr; ax += (sp[j].first + sp[i].first) * cr; ay += (sp[j].second + sp[i].second) * cr; }
      const QPointF gc = std::abs(aa) > 1e-9 ? QPointF(ax / (3 * aa), ay / (3 * aa)) : QPointF(cs.x, cs.y);
      const auto h = pickSurface(gc); mid.w = h && h->point.distanceTo(c) < c.distanceTo(L[0].w) * 2 ? h->point : patchOnRefs(c); }
    const int cv = resolvePt(E, mid, Mi, tt_.anchors); const size_t n = rim.size();
    for (size_t i = 0; i + 1 < n; i += 2) { std::vector<int> q{cv, rim[i], rim[i + 1], rim[(i + 2) % n]}; if (i + 2 == n) q = {cv, rim[i], rim[i + 1], rim[0]}; const int fi = E.addFace(q); faceCam(E, fi, Mi.inverse(), cam_); made.push_back(fi); }
    if (n % 2) { const int fi = E.addFace({cv, rim[n - 1], rim[0]}); faceCam(E, fi, Mi.inverse(), cam_); made.push_back(fi); }
    if (ba >= 0) tt_.anchors.erase(tt_.anchors.begin() + ba);
  }
  tt_.symNew = true; return ttDone(m, E, *baseOf(*it), tr("The hole filled: %1 polygon%2").arg(made.size()).arg(made.size() == 1 ? "" : "s"), 'f', made);
}

// Shift+RMB: the polygon of the points round the pointer — anchors and the rim's vertices within the fill radius,
// the nearest four (three) that hold the pointer between them
QString Viewport::penFill(const QPointF& mp) {
  std::vector<TTPt> pick = penFillPoints(mp);
  if (pick.empty()) { tt_.m = tt_.m >= 0 && scene_->item(tt_.m) ? tt_.m : topoMesh(); const QString h = penFillHoleMake(mp); if (!h.isEmpty()) return h;   // (in a hole: filled from its middle)
    ui::toast(tr("No points round the pointer to fill — put anchors (Shift + click) or draw by the rim's vertices")); return "!"; }
  const int m = tt_.m; Item* it = m >= 0 ? scene_->item(m) : nullptr;
  tt_.chain = pick; if (!it) return penNewMesh();                 // (no mesh yet: the anchors make a new one)
  MeshEd E = MeshEd::from(*baseOf(*it)); const Affine Mi = meshmods::meshMatrix(*scene_, *it).inverse(); std::vector<int> vs; std::set<int> usedA;
  for (auto& q : tt_.chain) { vs.push_back(resolvePt(E, q, Mi, tt_.anchors)); if (q.anchor >= 0) usedA.insert(q.anchor); }
  tt_.chain.clear();
  const int fi = E.addFace(vs); faceCam(E, fi, Mi.inverse(), cam_);
  std::vector<Vec3> keep; for (size_t i = 0; i < tt_.anchors.size(); i++) if (!usedA.count(int(i))) keep.push_back(tt_.anchors[i]); tt_.anchors = keep;
  tt_.symNew = true; return ttDone(m, E, *baseOf(*it), tr("Filled: a polygon of %1 corners").arg(vs.size()), 'f', {fi});
}

// Ctrl + click: a vertex — the polygons on it gone; an edge — inside: the two polygons one, on the rim: its polygon
// gone; a polygon — gone
QString Viewport::penDelete(const TTPt& p, int f) {
  const int m = tt_.m; Item* it = scene_->item(m); if (!it) return "!";
  if (p.anchor >= 0) { tt_.anchors.erase(tt_.anchors.begin() + p.anchor); ttTouch(); return tr("Anchor deleted"); }
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); std::vector<char> del(E.f.size(), 0);
  if (p.v >= 0) {
    // a vertex on a straight run of one edge (two neighbours — between two polygons, or on the rim): dissolved, its
    // polygons keep their place with a corner less; else the polygons on it gone
    const auto fs = E.facesOfVert(p.v); std::set<int> nb; bool keepsAll = !fs.empty();
    for (int fi : fs) { const auto& F = E.f[size_t(fi)]; const int n = int(F.v.size()), k = E.corner(fi, p.v); nb.insert(F.v[size_t((k + 1) % n)]); nb.insert(F.v[size_t((k + n - 1) % n)]); if (n <= 3) keepsAll = false; }
    if (nb.size() == 2 && keepsAll) {
      for (int fi : fs) { auto& F = E.f[size_t(fi)]; const int k = E.corner(fi, p.v); F.v.erase(F.v.begin() + k); if (k < int(F.uv.size())) F.uv.erase(F.uv.begin() + k); if (k < int(F.n.size())) F.n.erase(F.n.begin() + k); }
      return ttDone(m, E, B, tr("Vertex dissolved — %1 polygon%2 kept").arg(fs.size()).arg(fs.size() == 1 ? "" : "s"));
    }
    for (int fi : fs) del[size_t(fi)] = 1;
  }
  else if (p.ea >= 0) {
    auto fs = E.facesOfEdge(p.ea, p.eb);
    if (fs.size() == 2) {                                           // (the edge melted: one polygon of the two)
      const auto A = E.f[size_t(fs[0])], Bf = E.f[size_t(fs[1])];
      int a = p.ea, b = p.eb; { const int ka = E.corner(fs[0], a); if (A.v[size_t((ka + 1) % int(A.v.size()))] != b) std::swap(a, b); }   // (A runs a → b)
      MeshEd::F N; N.g = A.g; N.mat = A.mat; N.sets = A.sets; N.live = A.live;
      const int n1 = int(A.v.size()), n2 = int(Bf.v.size()); const int kb = E.corner(fs[0], b), ka2 = E.corner(fs[1], a);
      for (int k = kb;; k = (k + 1) % n1) { N.v.push_back(A.v[size_t(k)]); if (E.hasUV) N.uv.push_back(A.uv[size_t(k)]); if (A.v[size_t(k)] == a) break; }
      for (int k = (ka2 + 1) % n2; Bf.v[size_t(k)] != b; k = (k + 1) % n2) { N.v.push_back(Bf.v[size_t(k)]); if (E.hasUV) N.uv.push_back(Bf.uv[size_t(k)]); }
      std::set<int> chk(N.v.begin(), N.v.end());
      if (chk.size() == N.v.size() && N.v.size() >= 3) { E.f[size_t(fs[0])] = N; del[size_t(fs[1])] = 1; E.removeFaces(del); return ttDone(m, E, B, tr("Edge deleted — the two polygons one")); }
      del[size_t(fs[0])] = del[size_t(fs[1])] = 1;
    } else for (int fi : fs) del[size_t(fi)] = 1;
  } else if (f >= 0 && size_t(f) < del.size()) del[size_t(f)] = 1;
  int n = 0; for (char x : del) n += x; if (!n) return "!";
  if (n == int(E.f.size())) { ui::toast(tr("That is the whole mesh")); return "!"; }
  E.removeFaces(del);
  return ttDone(m, E, B, tr("Deleted: %1 polygon%2").arg(n).arg(n == 1 ? "" : "s"));
}

QString Viewport::penDeleteAnchors() { const int n = int(tt_.anchors.size()); tt_.anchors.clear(); tt_.chain.erase(std::remove_if(tt_.chain.begin(), tt_.chain.end(), [](const TTPt& p) { return p.anchor >= 0; }), tt_.chain.end()); ttTouch(); emit topoState(); return tr("%1 anchor%2 deleted").arg(n).arg(n == 1 ? "" : "s"); }

// ---------- Loop ----------
QString Viewport::loopApply(int m, int e, double t0, bool selectNew, int cuts, bool parallel) {
  Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  auto Bs = baseSp(*it); const PolyMesh& B = *Bs; auto T = topo::ttopo(Bs);
  Ring R = ringOf(*T, e, &B.pos); if (R.el.empty()) return "!";
  const int ea0 = T->ea[size_t(e)], eb0 = T->eb[size_t(e)];
  // (the hovered edge's place along the ring)
  size_t i0 = 0; for (size_t i = 0; i < R.el.size(); i++) if (ek(R.el[i].a, R.el[i].b) == ek(ea0, eb0)) { i0 = i; if (R.el[i].a != ea0) t0 = 1 - t0; break; }
  MeshEd E = MeshEd::from(B); const size_t ne = R.el.size();
  const double L0 = E.pos[size_t(R.el[i0].a)].distanceTo(E.pos[size_t(R.el[i0].b)]);
  std::vector<std::vector<int>> nv(ne); const bool nearA = ringNearA(R, t0, tt_.lpVert, tt_.lpEdge);
  for (size_t i = 0; i < ne; i++) {
    const int a = R.el[i].a, b = R.el[i].b; const double L = E.pos[size_t(a)].distanceTo(E.pos[size_t(b)]); std::vector<double> ts;
    for (int k = 1; k <= cuts; k++) {
      double t = cuts == 1 ? t0 : double(k) / (cuts + 1);
      if (cuts == 1 && parallel && L > 0) t = nearA ? std::clamp(t0 * L0 / L, 0.02, 0.98) : std::clamp(1 - (1 - t0) * L0 / L, 0.02, 0.98);
      ts.push_back(t);
    }
    for (double t : ts) nv[i].push_back(E.addVert(E.pos[size_t(a)] + (E.pos[size_t(b)] - E.pos[size_t(a)]) * t));
    E.insertOnEdge(a, b, nv[i], ts);
  }
  std::vector<std::pair<int, int>> made;
  const size_t nfc = R.closed ? ne : ne - 1;
  for (size_t i = 0; i < nfc; i++) {
    const size_t j = (i + 1) % ne;
    for (int k = 0; k < cuts; k++) {
      const int u = nv[i][size_t(k)], w = nv[j][size_t(k)];
      int fi = -1; for (int x : E.facesOfVert(u)) if (E.corner(x, w) >= 0) { fi = x; break; }
      if (fi >= 0 && E.splitFace(fi, u, w) >= 0) made.push_back({u, w});
    }
  }
  if (tset.snap) {                                                // (Snap: the new vertices onto the references — the nearest point: the loop runs round the back too)
    std::vector<std::shared_ptr<const topo::Bvh>> tg;
    for (auto& [id, t] : scene_->items) if (id != m && t.type == ItemType::IMesh && t.poly && scene_->isVisible(t)) if (auto b = bvhOf(id, true)) tg.push_back(b);
    if (!tg.empty()) { const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse();
      for (auto& L : nv) for (int v : L) { const Vec3 p = M.apply(E.pos[size_t(v)]); double bd = 1e300; Vec3 best = p;
        for (auto& b : tg) { double d; auto q = b->closest(p, nullptr, &d); if (q && d < bd) { bd = d; best = *q; } }
        E.pos[size_t(v)] = Mi.apply(best); } }
  }
  if (made.empty()) return "!";
  tt_.lastOp = "loop"; tt_.lastArgs = QJsonObject{{"a", ea0}, {"b", eb0}, {"t", tt_.lt}, {"sel", selectNew}};
  return ttDone(m, E, B, tr("%1 loop%2 cut across %3 polygon%4").arg(cuts).arg(cuts == 1 ? "" : "s").arg(nfc).arg(nfc == 1 ? "" : "s"), selectNew ? 'e' : 0, {}, made);
}

// ---------- Knife ----------
// Ctrl+Z / Ctrl+Y with the Knife and its curves drawn: their own steps first
bool Viewport::kUndoRedo(bool redo) {
  auto& from = redo ? tt_.kRedo : tt_.kUndo; auto& to = redo ? tt_.kUndo : tt_.kRedo;
  if (from.empty()) return false;
  to.push_back({tt_.kc, tt_.kcur}); tt_.kc = from.back().first; tt_.kcur = from.back().second; from.pop_back();
  ttTouch(); ui::hud(redo ? tr("Knife: redone") : tr("Knife: undone"), 700); return true;
}
int Viewport::ttVertNear(int m, const QPointF& mp, double px) const {
  if (m < 0 || !scene_->item(m)) return -1;
  const auto W = ttWorld(m); Projector P = projector(); int best = -1; double bd = px;
  for (size_t v = 0; v < W.size(); v++) { const ScreenPt s = P.toScreen(W[v]); if (s.z >= 1) continue; const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (d < bd) { bd = d; best = int(v); } }
  return best;
}

// the curves → screen paths; every edge they cross cut there (near a corner: onto it — the weld radius), each polygon
// with two of the cuts on its sides split between them
QString Viewport::knifeCut() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  if (tt_.kc.empty()) { ui::toast(tr("Draw a cut first: clicks on the mesh, or a drag from empty space")); return "!"; }
  Projector P = projector(); const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B);
  const Affine M = meshmods::meshMatrix(*scene_, *it); std::vector<Vec3> W; std::vector<ScreenPt> S;
  const bool ortho = cam_.ortho(); const Vec3 eye = cam_.pos, dir = cam_.dir();
  std::vector<char> ok(E.f.size(), 1);
  // (no polygon left out by which way it faces: a mesh turned inside out in places — normals inwards — would be cut on
  // its hidden side and not where it is seen; what is hidden is told by the ray from the eye below)
  // (not cut through: only where the mesh itself is seen — a polygon facing the camera behind another part of it, as
  // the inside of a fold, stays whole)
  topo::Bvh seen; if (!ttools.knifeThrough) { std::vector<Vec3> W0; for (auto& p : E.pos) W0.push_back(M.apply(p)); std::vector<uint32_t> tris;
    for (auto& F : E.f) for (size_t k = 1; k + 1 < F.v.size(); k++) { tris.push_back(uint32_t(F.v[0])); tris.push_back(uint32_t(F.v[k])); tris.push_back(uint32_t(F.v[k + 1])); }
    seen.build(W0, tris); }
  const double far = std::max(1.0, B.bounds().size().length() * 100), edgeLen = B.bounds().size().length() / std::sqrt(double(std::max(1, B.faceCount())));
  int hiddenSkipped = 0;
  auto visible = [&](const Vec3& p) { if (ttools.knifeThrough) return true;
    const Vec3 o = ortho ? p - dir * far : eye; const Vec3 d = ortho ? dir : (p - eye).normalized(); const double dist = ortho ? far : (p - eye).length();
    return !seen.ray(o, d, dist - std::max(dist * 5e-3, edgeLen * 0.5)); };   // (a non-flat polygon's triangles may stand a little in front of a point on its own side: not a cover)
  int cutsMade = 0;
  for (auto& K : tt_.kc) {
    W.clear(); S.clear(); for (auto& p : E.pos) W.push_back(M.apply(p)); for (auto& w : W) S.push_back(P.toScreen(w));   // (the vertices an earlier curve made too)
    // (the crossings: on each edge of an open polygon — its place along the path; near a corner: onto it)
    struct Hit { int a, b; double t, s; int v = -1; };
    std::map<tedit::EK, std::vector<Hit>> byEdge; std::map<int, double> vHit;
    auto addHit = [&](int a, int b, double t, double s) {   // (t: from a)
      const tedit::EK e = ek(a, b); if (e.first != a) t = 1 - t;
      const ScreenPt& A = S[size_t(e.first)]; const ScreenPt& Bp = S[size_t(e.second)];
      const double len = A.z < 1 && Bp.z < 1 ? std::hypot(Bp.x - A.x, Bp.y - A.y) : ttools.knifeWeld * 10;   // (off the screen: a tenth of the edge)
      if (t * len < ttools.knifeWeld) { if (!vHit.count(e.first) || vHit[e.first] > s) vHit[e.first] = s; return; }
      if ((1 - t) * len < ttools.knifeWeld) { if (!vHit.count(e.second) || vHit[e.second] > s) vHit[e.second] = s; return; }
      byEdge[e].push_back({e.first, e.second, t, s}); };
    if (!ttools.knifeThrough && !K.line) {
      // (on the surface: the spline's samples laid on the mesh — each its nearest point there — and the way between two
      // next ones walked over the polygons along the plane through both and the surface's normal: the cut goes where the
      // curve was drawn on the mesh, round the back of a tube too, and not where its picture falls on the screen now)
      std::vector<uint32_t> tris; std::vector<int> t2f;
      for (size_t fi = 0; fi < E.f.size(); fi++) { const auto& V = E.f[fi].v; for (size_t k = 1; k + 1 < V.size(); k++) { tris.push_back(uint32_t(V[0])); tris.push_back(uint32_t(V[k])); tris.push_back(uint32_t(V[k + 1])); t2f.push_back(int(fi)); } }
      topo::Bvh bv; bv.build(W, tris);
      auto fN = [&](int fi) { Vec3 n; const auto& V = E.f[size_t(fi)].v; for (size_t k = 0; k < V.size(); k++) n = n + W[size_t(V[k])].cross(W[size_t(V[(k + 1) % V.size()])]); return n.normalized(); };
      struct SP { Vec3 p; int f; }; std::vector<SP> sp;
      for (auto& q : catmull(K.a, K.closed, 12)) { int t = -1; if (auto c = bv.closest(q, nullptr, nullptr, &t); c && t >= 0 && (sp.empty() || sp.back().p.distanceTo(*c) > 1e-9)) sp.push_back({*c, t2f[size_t(t)]}); }
      struct X { int a, b; double t; }; std::vector<X> xs;
      for (size_t i = 0; i + 1 < sp.size(); i++) {
        const Vec3 p0 = sp[i].p, p1 = sp[i + 1].p, dir = p1 - p0; const int f0 = sp[i].f, f1 = sp[i + 1].f;
        const double L2 = dir.lengthSq(); if (f0 == f1 || L2 < 1e-24) continue;
        Vec3 n0 = fN(f0), n1 = fN(f1); if (n0.dot(n1) < 0) n1 = n1 * -1.0;   // (normals turned inwards in places: one way)
        Vec3 N = dir.cross(n0 + n1); if (N.lengthSq() < 1e-30) continue; N = N.normalized();
        std::vector<X> run; int f = f0; tedit::EK from{-1, -1}; bool reached = false;
        for (int step = 0; step < 4096 && !reached; step++) {   // (out of each polygon by the side the plane crosses furthest on)
          const auto& V = E.f[size_t(f)].v; double bestP = -1e300; X bx{-1, -1, 0};
          for (size_t k = 0; k < V.size(); k++) { const int a = V[k], b = V[(k + 1) % V.size()]; if (ek(a, b) == from) continue;
            const double da = N.dot(W[size_t(a)] - p0), db = N.dot(W[size_t(b)] - p0); if ((da >= 0) == (db >= 0)) continue;
            const double t = da / (da - db), pr = (W[size_t(a)] + (W[size_t(b)] - W[size_t(a)]) * t - p0).dot(dir) / L2;
            if (pr > bestP) { bestP = pr; bx = {a, b, t}; } }
          if (bx.a < 0 || bestP > 1.5 || bestP < -0.5) break;   // (gone past the next sample, or back: lost)
          run.push_back(bx);
          int nf = -1; for (int g : E.facesOfEdge(bx.a, bx.b)) if (g != f) { nf = g; break; }
          if (nf < 0) break;                                     // (the mesh's rim)
          from = ek(bx.a, bx.b); f = nf; reached = f == f1; }
        if (!reached) continue;
        for (auto& x : run) { if (!xs.empty() && ek(xs.back().a, xs.back().b) == ek(x.a, x.b)) xs.pop_back(); else xs.push_back(x); }   // (over a side and straight back: neither)
      }
      for (size_t i = 0; i < xs.size(); i++) addHit(xs[i].a, xs[i].b, xs[i].t, double(i));
    } else {
      std::vector<std::pair<double, double>> path;
      if (K.line) { for (auto& a : K.a) { ScreenPt s = P.toScreen(a); path.push_back({s.x, s.y}); } }
      else for (auto& p : catmull(K.a, K.closed, 12)) { ScreenPt s = P.toScreen(p); if (s.z < 1) path.push_back({s.x, s.y}); }
      if (path.size() < 2) continue;
      std::set<tedit::EK> edges; for (size_t fi = 0; fi < E.f.size(); fi++) if (ok[fi]) { const auto& V = E.f[fi].v; for (size_t k = 0; k < V.size(); k++) edges.insert(ek(V[k], V[(k + 1) % V.size()])); }
      for (auto& e : edges) {
        const ScreenPt& A = S[size_t(e.first)]; const ScreenPt& Bp = S[size_t(e.second)]; if (A.z >= 1 || Bp.z >= 1) continue;
        for (size_t i = 0; i + 1 < path.size(); i++) { double t, u;
          if (!segCross(A.x, A.y, Bp.x, Bp.y, path[i].first, path[i].second, path[i + 1].first, path[i + 1].second, t, u)) continue;
          if (!visible(W[size_t(e.first)] + (W[size_t(e.second)] - W[size_t(e.first)]) * t)) { hiddenSkipped++; continue; }   // (behind the mesh's own surface)
          addHit(e.first, e.second, t, double(i) + u); }
      }
    }
    // (the edges cut: their new vertices, then each polygon's cuts in the path's order — split pairwise)
    std::map<int, double> sOf(vHit.begin(), vHit.end());
    for (auto& [e, hs] : byEdge) { std::sort(hs.begin(), hs.end(), [](const Hit& x, const Hit& y) { return x.t < y.t; });
      std::vector<int> vs; std::vector<double> ts; for (auto& h : hs) { const int v = E.addVert(E.pos[size_t(e.first)] + (E.pos[size_t(e.second)] - E.pos[size_t(e.first)]) * h.t); vs.push_back(v); ts.push_back(h.t); sOf[v] = h.s; }
      E.insertOnEdge(e.first, e.second, vs, ts); }
    if (sOf.size() < 2) continue;
    std::vector<std::pair<double, int>> order; for (auto& [v, s] : sOf) order.push_back({s, v}); std::sort(order.begin(), order.end());
    const size_t pairs = K.closed && order.size() > 2 ? order.size() : order.size() - 1;   // (a closed curve: the last cut joined to the first too)
    for (size_t i = 0; i < pairs; i++) {
      const int u = order[i].second, w = order[(i + 1) % order.size()].second; if (u == w || E.edgeExists(u, w)) continue;   // (along a side already)
      for (int fi : E.facesOfVert(u)) { if (size_t(fi) < ok.size() && !ok[size_t(fi)]) continue; if (E.corner(fi, w) < 0) continue;
        if (E.splitFace(fi, u, w) >= 0) { ok.push_back(1); cutsMade++; } break; }
    }
  }
  if (!cutsMade) { ui::toast(tr("The cut crosses no polygon of the mesh")); return "!"; }
  tt_.kUndo.clear(); tt_.kRedo.clear();
  tt_.kc.clear(); tt_.kcur = -1;
  return ttDone(m, E, B, tr("Knife: %1 polygon cut%2").arg(cutsMade).arg(cutsMade == 1 ? "" : "s") + (hiddenSkipped ? tr(" · %1 crossing%2 behind the mesh left whole").arg(hiddenSkipped).arg(hiddenSkipped == 1 ? "" : "s") : QString()));
}

// ---------- Smooth EdgeChain ----------
QString Viewport::chainCreate() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || tsel_.id != m) { ui::toast(tr("Select a chain of edges first")); return "!"; }
  auto T = topo::ttopo(baseSp(*it)); std::vector<tedit::EK> es;
  if (tKey() == 'e') for (int e : tsel_.e) es.push_back(ek(T->ea[size_t(e)], T->eb[size_t(e)]));
  else { auto vs = tSelVerts(m); std::set<int> s(vs.begin(), vs.end()); for (int e = 0; e < T->ne; e++) if (s.count(T->ea[size_t(e)]) && s.count(T->eb[size_t(e)])) es.push_back(ek(T->ea[size_t(e)], T->eb[size_t(e)])); }
  auto chains = tedit::edgeChains(es); if (chains.empty()) { ui::toast(tr("Select a chain of edges first")); return "!"; }
  auto best = *std::max_element(chains.begin(), chains.end(), [](const tedit::Chain& a, const tedit::Chain& b) { return a.v.size() < b.v.size(); });
  if (best.v.size() < 3) { ui::toast(tr("The chain needs three vertices or more")); return "!"; }
  auto& C = tt_.ec; C = {}; C.m = m; C.verts = best.v; C.closed = best.closed; C.base = baseOf(*it)->pos;
  std::vector<Vec3> L; for (int v : C.verts) L.push_back(C.base[size_t(v)]); if (C.closed) L.push_back(L[0]);
  auto cum = cumLen(L); for (size_t i = 0; i < C.verts.size(); i++) C.u.push_back(cum.back() > 0 ? cum[i] / cum.back() : 0);
  const int n = int(C.verts.size()), na = std::clamp(n / 3 + 1, 3, 12);    // (a few anchors along it)
  for (int k = 0; k < na; k++) { const double u = C.closed ? double(k) / na : double(k) / (na - 1); C.a.push_back(alongLen(L, cum, u)); }
  C.a0 = C.a; ttTouch(); emit topoState();
  ui::hud(tr("The chain follows the curve: drag its anchors, a click on it — a new anchor; Shift+RMB — smooth, optimise, delete"), 2500);
  return tr("Curve on %1 vertices").arg(n);
}

// the chain's vertices onto the curve (smoothed by the setting; evenly spaced when asked); commitIt — one undo step
QString Viewport::chainApply(bool commitIt) {
  auto& C = tt_.ec; Item* it = C.m >= 0 ? scene_->item(C.m) : nullptr; if (!it || !it->base) return "!";
  std::vector<Vec3> A = C.a;
  for (int it2 = 0; it2 < int(std::lround(ttools.chainSmooth * 20)); it2++) {   // (the anchors smoothed: each towards its neighbours)
    std::vector<Vec3> nA = A; const int n = int(A.size());
    for (int i = 0; i < n; i++) { if (!C.closed && (i == 0 || i == n - 1)) continue; const Vec3& p = A[size_t((i + n - 1) % n)]; const Vec3& q = A[size_t((i + 1) % n)]; nA[size_t(i)] = A[size_t(i)] * 0.5 + (p + q) * 0.25; }
    A = nA; }
  auto L = catmull(A, C.closed, 16); auto cum = cumLen(L);
  std::vector<Vec3> P = it->base->pos; const size_t n = C.verts.size();
  for (size_t i = 0; i < n; i++) { const double u = ttools.chainEqual ? (C.closed ? double(i) / n : double(i) / (n - 1)) : C.u[i]; P[size_t(C.verts[i])] = alongLen(L, cum, u); }
  if (tset.snap) {                                                // (Snap: the chain's vertices onto the references — the nearest point)
    std::vector<std::shared_ptr<const topo::Bvh>> tg; for (auto& [id, t] : scene_->items) if (id != C.m && t.type == ItemType::IMesh && t.poly && scene_->isVisible(t)) if (auto b = bvhOf(id, true)) tg.push_back(b);
    if (!tg.empty()) { const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse();
      for (int v : C.verts) { const Vec3 p = M.apply(P[size_t(v)]); double bd = 1e300; Vec3 best = p; for (auto& b : tg) { double d; auto q = b->closest(p, nullptr, &d); if (q && d < bd) { bd = d; best = *q; } } P[size_t(v)] = Mi.apply(best); } }
  }
  topoLive(C.m, P);
  if (commitIt) { topoWrite(C.m, baseSp(*it)); scene_->revision++; commit(false); emit topoState(); }
  ttTouch();
  return tr("Chain on the curve");
}

// ---------- the pointer ----------
bool Viewport::topoToolPress(QMouseEvent* e) {
  const QPointF mp = e->position(); const bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  if (e->button() != Qt::LeftButton) return false;
  penAskClose();                                                   // (a weld asked about and not answered: ignored — not welded)
  if (tool_ == "tpatch") return patchPress(e);
  if (tool_ == "tlobe") return lobePress(e);
  if (nBrushOn()) {                                                // (the normals brush: a stroke gives the share set — Ctrl: the standard shading, Shift: smoothed)
    const int am = topoMesh(); const Item* ai = am >= 0 ? scene_->item(am) : nullptr;
    if (!ai || topoHeavy(am)) { ui::toast(tr("Select the mesh whose normals you paint")); return true; }
    nBrushStart(am, shift ? 's' : ctrl ? 'r' : 'p');
    tt_.m = am; tt_.ctrl = ctrl; tt_.press = tt_.p1 = mp; tt_.moved = false; tt_.drag = 22; td_ = {}; td_.type = 20; drag_ = Drag::Topo; nAlignDab(mp, ctrl); return true; }
  int m = topoMesh();
  if (ctrl && !shift && m >= 0 && tool_ != "tknife" && tool_ != "toptimize" && !selectsAsCursor()) {   // (those selecting as the Select Cursor: as it does)   // (every tool: Ctrl + a drag from free space — the selection cleared)
    bool off = !tPick(mp, {m});
    if (off && tool_ == "tpen") { const TTPt p = ttPickPt(mp, m, true); off = p.v < 0 && p.ea < 0; }
    if (off) { td_ = {}; td_.mode = "remove"; td_.x0 = mp.x(); td_.y0 = mp.y(); td_.empty = true; td_.last = mp; td_.type = 7; td_.clearAll = true; drag_ = Drag::Topo; return true; }
  }
  if (tool_ == "treveal") {                                       // (Reveal Brush: the ghost's polygons under the stroke gathered — made real at its end)
    if (!ghostAny()) { ui::toast(tr("No Topology Ghost — mark a source mesh (its settings, Snapping: the ghost button)")); return true; }
    revealFaces_.clear(); tt_.press = tt_.p1 = mp; tt_.moved = false; tt_.drag = 21; td_ = {}; td_.type = 20; drag_ = Drag::Topo; revealGather(mp); return true; }
  if (m < 0 && tool_ == "tpen" && !ctrl) {                       // (no mesh picked: the polygon drawn will be a new RetopoMesh's first)
    tt_.m = -1; tt_.press = mp; tt_.moved = false; tt_.ctrl = ctrl; tt_.shift = shift;
    if (shift) { const Vec3 w = ttFreePoint(mp, -1); if (fpRefused_) { ui::hud(tr("Not on the back or the inside of a reference — put the point on its outer side"), 1500); return true; } tt_.anchors.push_back(w); ttTouch(); emit topoState(); return true; }
    fpRefused_ = false; tt_.hf = -1; tt_.pressPt = ttPickPt(mp, -1, true); tt_.pressF = -1;
    if (fpRefused_ && tt_.pressPt.anchor < 0) { ui::hud(tr("Not on the back or the inside of a reference — put the point on its outer side"), 1500); return true; }
    tt_.drag = 1; td_ = {}; td_.type = 20; drag_ = Drag::Topo; return true;
  }
  if (m < 0 && tool_ != "tplane") { if (auto r = tPick(mp, topoMeshes())) { sel_ = {}; sel_.items = {r->m}; onSelChange(); m = r->m; } }
  if (m < 0) { ui::toast(tr("Select a mesh first")); return true; }
  if (topoHeavy(m)) { ui::toast(tr("A high-poly mesh — Object mode only")); return true; }
  tt_.m = m; tt_.press = mp; tt_.moved = false; tt_.ctrl = ctrl; tt_.shift = shift; tt_.drag = 0;
  if (tool_ == "tpen" && topoMode == "object") setTopoMode(lastEditMode_, true);   // (PolyPen from Object: the edit mode last used)
  auto startDrag = [&](int kind) { tt_.drag = kind; td_ = {}; td_.type = 20; drag_ = Drag::Topo; };
  const bool loopMode = tool_ == "tloop" || (tool_ == "tpen" && ctrl && shift);
  if (loopMode) { if (tt_.le >= 0) loopApply(m, tt_.le, tt_.lt, false, ttools.loopCuts, ttools.loopParallel || tt_.lctrl); tt_.le = -1; tt_.loopPrev.clear(); ttTouch(); return true; }
  if (tool_ == "tpen") {
    if (shift) { const Vec3 w = ttFreePoint(mp, m); if (fpRefused_) { ui::hud(tr("Not on the back or the inside of a reference — put the point on its outer side"), 1500); return true; } tt_.anchors.push_back(w); ttTouch(); emit topoState(); ui::hud(tr("Anchor %1 — Shift+RMB between points fills a polygon").arg(tt_.anchors.size()), 1200); return true; }
    fpRefused_ = false; tt_.hf = -1; tt_.pressPt = ttPickPt(mp, m, true, ctrl); tt_.pressF = tt_.hf;
    if (fpRefused_ && tt_.pressPt.v < 0 && tt_.pressPt.ea < 0 && tt_.pressPt.anchor < 0 && !ctrl) { ui::hud(tr("Not on the back or the inside of a reference — put the point on its outer side"), 1500); return true; }   // (a new point there: refused)
    startDrag(ctrl ? 2 : 1); return true;
  }
  if (tool_ == "tknife") {
    Projector P = projector();
    if (ctrl) { for (size_t c = 0; c < tt_.kc.size(); c++) for (size_t a = 0; a < tt_.kc[c].a.size(); a++) { ScreenPt s = P.toScreen(tt_.kc[c].a[a]); if (s.z < 1 && std::hypot(s.x - mp.x(), s.y - mp.y()) < 10) { tt_.kdc = int(c); tt_.kda = int(a); kSave(); startDrag(4); return true; } }
      return true; }
    // (a click on the curve's first anchor — three or more placed: the curve closed, ended)
    if (tt_.kcur >= 0 && size_t(tt_.kcur) < tt_.kc.size() && tt_.kc[size_t(tt_.kcur)].a.size() >= 3 && !tt_.kc[size_t(tt_.kcur)].line) {
      const ScreenPt s0 = P.toScreen(tt_.kc[size_t(tt_.kcur)].a.front());
      if (s0.z < 1 && std::hypot(s0.x - mp.x(), s0.y - mp.y()) < 10) { kSave(); tt_.kc[size_t(tt_.kcur)].closed = true; tt_.kcur = -1; ttTouch();
        ui::hud(tr("Knife: the curve closed · Enter or Shift+RMB → Cut"), 1500); return true; } }
    auto r = tPick(mp, {m});
    if (!r && tt_.kcur < 0) { tt_.p0 = tt_.p1 = mp; startDrag(5); return true; }   // (from empty space: a straight cut)
    if (!r) return true;
    if (tt_.kcur < 0) { tt_.kc.push_back({}); tt_.kcur = int(tt_.kc.size()) - 1; }
    kSave(); Vec3 p = r->point; if (int v = ttVertNear(m, mp, std::max(8.0, ttools.knifeWeld)); v >= 0) p = ttWorld(m)[size_t(v)];   // (onto a vertex near it)
    tt_.kc[size_t(tt_.kcur)].a.push_back(p); ttTouch();
    ui::hud(tr("Knife: clicks — the curve's anchors; Esc ends it · Enter or Shift+RMB → Cut"), 1500); return true;
  }
  if (tool_ == "tchain" && tt_.ec.m == m) {
    Projector P = projector();
    for (size_t a = 0; a < tt_.ec.a.size(); a++) { ScreenPt s = P.toScreen(meshmods::meshMatrix(*scene_, *scene_->item(m)).apply(tt_.ec.a[a])); if (s.z < 1 && std::hypot(s.x - mp.x(), s.y - mp.y()) < 10) { tt_.eca = int(a); startDrag(6); return true; } }
    // (a click on the curve: a new anchor there)
    const Affine M = meshmods::meshMatrix(*scene_, *scene_->item(m));
    for (size_t a = 0; a + (tt_.ec.closed ? 0 : 1) < tt_.ec.a.size(); a++) {
      const size_t b = (a + 1) % tt_.ec.a.size(); ScreenPt sa = P.toScreen(M.apply(tt_.ec.a[a])), sb = P.toScreen(M.apply(tt_.ec.a[b])); double t;
      if (segDistT(mp.x(), mp.y(), sa.x, sa.y, sb.x, sb.y, &t) < 6) { tt_.ec.a.insert(tt_.ec.a.begin() + long(a + 1), tt_.ec.a[a] + (tt_.ec.a[b] - tt_.ec.a[a]) * t); tt_.eca = int(a + 1); startDrag(6); ttTouch(); return true; }
    }
    return false;                                                 // (else: the selection, as the Select Cursor)
  }
  if (tool_ == "tplane") { tt_.p0 = tt_.p1 = mp; startDrag(7); return true; }
  if (tool_ == "tbevel" || tool_ == "tinset") {
    if (tool_ == "tinset" && tKey() == 'v') { insetCircles(); return true; }
    tt_.bevPull = false;
    tt_.bevEdges = tool_ == "tbevel" && tKey() == 'e' && tsel_.id == m && !tsel_.e.empty();   // (Bevel on edges: a chamfer)
    if (tt_.bevEdges) { tt_.before = baseSp(*scene_->item(m)); tt_.verts.assign(tsel_.e.begin(), tsel_.e.end()); tt_.lastSel = tsel_; startDrag(8); return true; }
    if (tSelFaces(m).empty()) { ui::toast(tr(tool_ == "tbevel" ? "Select polygons (extrude) or edges (bevel) first" : "Select polygons first")); return true; }
    tt_.bevPull = tool_ == "tbevel" && shift;                       // (Shift: the polygons pulled out — moved, none made)
    tt_.before = baseSp(*scene_->item(m)); tt_.verts = tSelFaces(m); tt_.lastSel = tsel_; startDrag(8); return true;
  }
  if (tool_ == "tbridge") {                                      // (as the Select Cursor on the mesh; a drag from free space — the bridge)
    if (ctrl || shift || tPick(mp, {m})) return false;              // (Ctrl / Shift: the selection, as the Cursor)
    tt_.adjDone = 0; tt_.adj0 = 0; startDrag(17); return true; }
  if (tool_ == "tfill") { fillApply(); return true; }
  if (tool_ == "toptimize") return shapePress(mp, false, shift, ctrl);   // (as the Select Cursor — but for the weld distance picked from the menu)
  if (tool_ == "tweld" && !shift && !ctrl && ttools.weldMode != "pair") {   // (Weld: a drag from a vertex onto another — welded there; the picked ones with it)
    const int v = ttVertNear(m, mp, 12); if (v < 0) return false;   // (off a vertex: the selection, as the Cursor)
    tt_.wsrc = v; tt_.wtgt = -1; tt_.wsrcs = {v};
    if (tsel_.id == m && tKey() == 'v' && tsel_.v.count(v)) tt_.wsrcs.assign(tsel_.v.begin(), tsel_.v.end());
    startDrag(19); return true; }
  if (tool_ == "tmirror") return false;                           // (Mirror: only its button runs it — a click here picks, as the Cursor)
  if (tool_ == "tshaper" || tool_ == "tslide" || tool_ == "tnormals") return shapePress(mp, false, shift, ctrl);   // (on the mesh: the selection)
  return false;                                                   // (Weld, EdgeChain: the selection as the Select Cursor)
}

bool Viewport::topoToolMove(const QPointF& mp, Qt::KeyboardModifiers mods) {
  tt_.mouse = mp;
  if (penAsk_.w) {                                                 // (the pointer gone well away from the weld question: ignored — not welded)
    const QRect r = penAsk_.w->frameGeometry(); const QPoint g = mapToGlobal(mp.toPoint());
    const int dx = std::max({r.left() - g.x(), 0, g.x() - r.right()}), dy = std::max({r.top() - g.y(), 0, g.y() - r.bottom()});
    if (std::hypot(dx, dy) > 140) penAskClose(); }
  if (tool_ == "tpatch") return patchMove(mp);
  if (tool_ == "tlobe") return lobeMove(mp);
  const int m = tt_.m >= 0 && scene_->item(tt_.m) ? tt_.m : topoMesh();
  if (drag_ == Drag::Topo && td_.type == 20 && tt_.drag >= 13 && tt_.drag <= 16) { shapeMove(mp); return true; }   // (Edge Shaper, Slide, Normals, Optimize's circle)
  if (drag_ == Drag::Topo && td_.type == 20 && tt_.drag == 22) {   // (the alignment brush: along the stroke, steps of a third of its radius)
    const int n = std::max(1, int(std::ceil(std::hypot(mp.x() - tt_.p1.x(), mp.y() - tt_.p1.y()) / std::max(2.0, brushR / 3))));
    for (int i = 1; i <= n; i++) nAlignDab(tt_.p1 + (mp - tt_.p1) * (double(i) / n), tt_.ctrl);
    tt_.p1 = mp; tt_.moved = true; overlayRev_++; requestUpdate(); return true; }
  if (drag_ == Drag::Topo && td_.type == 20 && tt_.drag == 21) {   // (Reveal Brush: along the stroke, steps of a third of its radius)
    const int n = std::max(1, int(std::ceil(std::hypot(mp.x() - tt_.p1.x(), mp.y() - tt_.p1.y()) / std::max(2.0, brushR / 3))));
    for (int i = 1; i <= n; i++) revealGather(tt_.p1 + (mp - tt_.p1) * (double(i) / n));
    tt_.p1 = mp; tt_.moved = true; overlayRev_++; requestUpdate(); return true; }
  if (drag_ == Drag::Topo && td_.type == 20 && tt_.drag == 19) {   // (Weld: the vertex dragged — the one under the pointer its target)
    tt_.moved = tt_.moved || std::hypot(mp.x() - tt_.press.x(), mp.y() - tt_.press.y()) >= 4;
    int t = ttVertNear(m, mp, 14); if (std::find(tt_.wsrcs.begin(), tt_.wsrcs.end(), t) != tt_.wsrcs.end()) t = -1;
    tt_.wtgt = t; ui::hud(t >= 0 ? tr("Let go: welded onto this vertex") : tr("Drag onto a vertex to weld")); ttTouch(); return true; }
  if (drag_ == Drag::Topo && td_.type == 20 && tt_.drag == 17) {   // (Bridge: the drag from free space makes it, once)
    if (tt_.adjDone == 0 && std::hypot(mp.x() - tt_.press.x(), mp.y() - tt_.press.y()) >= 6) { tt_.adjDone = 1; tt_.moved = true; bridgeNow(); }
    return true; }
  if (drag_ == Drag::Topo && td_.type == 20 && tt_.drag == 18) {   // (Bridge: RMB held, ←→ — its bulge, made again as it goes)
    const double dx = mp.x() - tt_.press.x(); tt_.moved = tt_.moved || std::abs(dx) >= 3; if (!tt_.moved) return true;
    ttools.brTension = std::clamp(tt_.adj0 + dx / 250, -2.0, 2.0); ui::hud(tr("Bridge: bulge %1").arg(ttools.brTension, 0, 'f', 2));
    if (std::abs(ttools.brTension - tt_.adjDone) >= 0.02) { tt_.adjDone = ttools.brTension; modelReapply(); }
    return true; }
  if (drag_ == Drag::Topo && td_.type == 20 && (tt_.drag == 9 || tt_.drag == 10)) {   // (Fill Hole: Edge Flow / Subdivision dragged)
    const double dx = mp.x() - tt_.press.x(); tt_.moved = tt_.moved || std::abs(dx) >= 3;
    if (tt_.drag == 9) { ttools.fillFlow = std::clamp(tt_.adj0 + dx / 300, 0.0, 1.0); ui::hud(tr("Edge Flow: %1%").arg(int(std::lround(ttools.fillFlow * 100))));
      if (std::abs(ttools.fillFlow - tt_.adjDone) >= 0.03) { tt_.adjDone = ttools.fillFlow; fillAdjustShow(); } }
    else { ttools.fillSub = std::clamp(int(std::lround(tt_.adj0 + dx / 30)), 0, 16); ui::hud(tr("Subdivision: %1").arg(ttools.fillSub));
      if (ttools.fillSub != int(tt_.adjDone)) { tt_.adjDone = ttools.fillSub; fillAdjustShow(); } }
    return true;
  }
  if (drag_ == Drag::Topo && td_.type == 20) {
    if (!tt_.moved && std::hypot(mp.x() - tt_.press.x(), mp.y() - tt_.press.y()) < 4) return true;
    Item* it = scene_->item(m); if (!it) return true;
    const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse();
    if (!tt_.moved) {
      tt_.moved = true;
      if (tt_.drag == 1 || tt_.drag == 2) {                       // (the pen's drag starts: what it moves / pulls out, from where it is)
        const TTPt& p = tt_.pressPt; tt_.verts.clear();
        if (p.v >= 0) tt_.verts = {p.v}; else if (p.ea >= 0) tt_.verts = {p.ea, p.eb};
        else if (tt_.pressF >= 0) { auto T = topo::ttopo(baseSp(*it)); for (int k = T->fo[size_t(tt_.pressF)]; k < T->fo[size_t(tt_.pressF) + 1]; k++) tt_.verts.push_back(T->fv[size_t(k)]); }
        if (tt_.verts.empty() || (tt_.drag == 2 && p.v >= 0)) { tt_.drag = 0; return true; }
        tt_.base = baseOf(*it)->pos; tt_.n = cam_.dir(); tt_.ref = M.apply(tt_.base[size_t(tt_.verts[0])]); tt_.hit0 = planeHit(ray(tt_.press), tt_.ref, tt_.n);
        tt_.symPairs.clear(); if (tset.sym && room() == "topo") tt_.symPairs = symFirstPairs(m, tt_.base);   // (Symmetry: the mirror pairs moved too)
      }
      if (tt_.drag == 8) { tt_.ref = cam_.pivot; tt_.delta = Vec3(); }
    }
    if (tt_.drag == 1 || tt_.drag == 2) {
      auto h = planeHit(ray(mp), tt_.ref, tt_.n); if (!h || !tt_.hit0) return true;
      tt_.delta = *h - *tt_.hit0;
      bool snapped = false;   // (the element snap: the vertex taken hold of onto the element under the pointer, the rest with it)
      if (tset.esnap && tt_.drag == 1) { const std::set<int> mv(tt_.verts.begin(), tt_.verts.end()); esnapHit_ = elemSnap(mp, m, mv, tt_.ref); if (esnapHit_) { tt_.delta = *esnapHit_ - tt_.ref; snapped = true; } overlayRev_++; }
      const Vec3 l = Mi.lin(tt_.delta);
      if (tt_.drag == 1) { std::vector<Vec3> P = tt_.base; for (int v : tt_.verts) P[size_t(v)] = tt_.base[size_t(v)] + l;
        { const bool s0 = tset.snap; if (snapped) tset.snap = false; topoAfterMove(m, P, tt_.verts, tt_.base, tt_.symPairs.empty() ? nullptr : &tt_.symPairs); tset.snap = s0; } topoLive(m, P); }
      else { tt_.loopPrev.clear(); std::vector<Vec3> q; for (int v : tt_.verts) q.push_back(M.apply(tt_.base[size_t(v)])); std::vector<Vec3> ring = q; for (auto it2 = q.rbegin(); it2 != q.rend(); ++it2) ring.push_back(*it2 + tt_.delta); ring.push_back(q[0]); tt_.loopPrev.push_back(ring); }
      tt_.hov = ttPickPt(mp, m, false); ttTouch(); return true;
    }
    if (tt_.drag == 4) { auto r = tPick(mp, {m}); if (r && tt_.kdc >= 0) { Vec3 p = r->point; if (int v = ttVertNear(m, mp, std::max(8.0, ttools.knifeWeld)); v >= 0) p = ttWorld(m)[size_t(v)]; tt_.kc[size_t(tt_.kdc)].a[size_t(tt_.kda)] = p; } ttTouch(); return true; }
    if (tt_.drag == 5 || tt_.drag == 7) { tt_.p1 = mp; overlayRev_++; requestUpdate(); return true; }
    if (tt_.drag == 6) {
      auto h = planeHit(ray(mp), M.apply(tt_.ec.a[size_t(tt_.eca)]), cam_.dir()); if (!h) return true;
      Vec3 w = *h; if (tset.snap) w = ttSnapW(w, m);   // (Snap: onto the references where it is seen; off — free, in the view's plane)
      tt_.ec.a[size_t(tt_.eca)] = Mi.apply(w); chainApply(false); return true;
    }
    if (tt_.drag == 8) {
      const double wpp = worldPerPixelAt(tt_.ref), off0 = (tool_ == "tinset" ? ttools.insOffset : ttools.bevOffset) + (mp.x() - tt_.press.x()) * wpp, off = tool_ == "tinset" || tt_.bevEdges ? std::max(0.0, off0) : off0;   // (Extrude: to the left past 0 — inward)
      tt_.delta = Vec3(off, 0, 0); bevelApply(m, off, tool_ == "tinset");
      ui::hud(tr("%1: %2").arg(tool_ == "tinset" ? tr("Inset") : tr("Extrude")).arg(off, 0, 'g', 3)); return true;
    }
    return true;
  }
  if (drag_ != Drag::None) return false;
  // ---- hover ----
  if (tool_ == "tpen") tt_.shiftHeld = (mods & Qt::ShiftModifier) && !(mods & Qt::ControlModifier);   // (Shift: the fill's polygon shown)
  if (m < 0 && tool_ == "tpen") { tt_.hf = -1; tt_.hov = ttPickPt(mp, -1, true); tt_.hovOn = true; ttTouch(); return true; }   // (a new RetopoMesh to come)
  if (tool_ == "treveal" || nBrushOn()) { overlayRev_++; requestUpdate(); return true; }   // (its circle follows — no mesh picked, or a high-poly one, too)
  if (m < 0 || topoHeavy(m)) return false;
  const bool loopMode = tool_ == "tloop" || (tool_ == "tpen" && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier));
  if (loopMode) {
    Item* it = scene_->item(m); auto r = tPick(mp, {m}); tt_.loopPrev.clear(); tt_.le = -1;
    if (r && r->e >= 0) {
      auto T = topo::ttopo(baseSp(*it)); const auto W = ttWorld(m); Projector P = projector();
      const int e = r->e; ScreenPt a = P.toScreen(W[size_t(T->ea[size_t(e)])]), b = P.toScreen(W[size_t(T->eb[size_t(e)])]); double t; segDistT(mp.x(), mp.y(), a.x, a.y, b.x, b.y, &t);
      tt_.lshift = mods & Qt::ShiftModifier; tt_.lctrl = (mods & Qt::ControlModifier) && tool_ == "tloop";
      if (tt_.lshift && tool_ == "tloop") { const double F[] = {0.5, 0.25, 0.75, 1.0 / 6, 2.0 / 6, 4.0 / 6, 5.0 / 6}; double bt = 0.5, bd = 1e9; for (double f : F) if (std::abs(f - t) < bd) { bd = std::abs(f - t); bt = f; } t = bt; }
      t = std::clamp(t, 0.02, 0.98); tt_.le = e; tt_.lt = t; ttools.loopPct = t;
      Ring R = ringOf(*T, e, &baseOf(*it)->pos);
      { const bool inRing = std::any_of(R.el.begin(), R.el.end(), [&](const RingEl& x) { return ek(x.a, x.b) == tt_.lpEdge; });
        if ((tt_.lctrl && !tt_.lpCtrl) || ((ttools.loopParallel || tt_.lctrl) && !inRing)) { tt_.lpEdge = ek(T->ea[size_t(e)], T->eb[size_t(e)]); tt_.lpVert = t <= 0.5 ? T->ea[size_t(e)] : T->eb[size_t(e)]; }   // (Ctrl down: the side it keeps to, fixed)
        tt_.lpCtrl = tt_.lctrl; }
      double t0 = t; const int ea0 = T->ea[size_t(e)]; size_t i0 = 0; for (size_t i = 0; i < R.el.size(); i++) if (ek(R.el[i].a, R.el[i].b) == ek(ea0, T->eb[size_t(e)])) { i0 = i; if (R.el[i].a != ea0) t0 = 1 - t0; break; }
      const double L0 = W[size_t(R.el[i0].a)].distanceTo(W[size_t(R.el[i0].b)]); const int cuts = ttools.loopCuts; const bool par = ttools.loopParallel || tt_.lctrl; const bool nearA = ringNearA(R, t0, tt_.lpVert, tt_.lpEdge);
      for (int k = 1; k <= cuts; k++) { std::vector<Vec3> L;
        for (auto& el : R.el) { const Vec3& A = W[size_t(el.a)]; const Vec3& Bv = W[size_t(el.b)]; const double Ln = A.distanceTo(Bv);
          double tt = cuts == 1 ? t0 : double(k) / (cuts + 1);
          if (cuts == 1 && par && Ln > 0) tt = nearA ? std::clamp(t0 * L0 / Ln, 0.02, 0.98) : std::clamp(1 - (1 - t0) * L0 / Ln, 0.02, 0.98);
          L.push_back(A + (Bv - A) * tt); }
        if (R.closed && !L.empty()) L.push_back(L[0]);
        tt_.loopPrev.push_back(L); }
      tt_.loopClosed = R.closed;
      ui::hud(tr("Loop: %1 cut%2 · %3% · wheel — cuts · Shift — marks · Ctrl — parallel").arg(cuts).arg(cuts == 1 ? "" : "s").arg(int(std::lround(t * 100))));
    }
    ttTouch(); return true;
  }
  if (tool_ == "tpen") { tt_.loopPrev.clear(); tt_.le = -1; tt_.hf = -1; tt_.hov = ttPickPt(mp, m, true, mods & Qt::ControlModifier); tt_.hovOn = true; ttTouch(); return true; }   // (Ctrl+Shift let go: no loop cut shown)
  if (tool_ == "tweld" && tsel_.id == m && tSelVerts(m).size() >= 2) ttTouch();   // (Weld: the preview follows the pointer)
  if (tool_ == "treveal") { overlayRev_++; requestUpdate(); return true; }   // (its circle follows)
  if (tool_ == "tknife" || tool_ == "tplane") { overlayRev_++; requestUpdate(); return true; }
  if (tool_ == "tfill") { fillPreview(mp); return true; }
  return false;
}

bool Viewport::topoToolRelease(const QPointF& mp) {
  if (tool_ == "tpatch") return patchRelease(mp);
  if (tool_ == "tlobe") return lobeRelease(mp);
  const int m = tt_.m; Item* it = m >= 0 ? scene_->item(m) : nullptr; const int kind = tt_.drag; tt_.drag = 0;
  if (kind >= 13 && kind <= 16) { tt_.drag = kind; shapeRelease(); return true; }
  if (kind == 21) { revealApply(); ttTouch(); return true; }
  if (kind == 22) { nAlignEnd(); return true; }                   // (the alignment brush: the stroke kept — the last averaging made again with it)        // (Reveal Brush: the stroke's polygons made, one undo step)
  if (kind == 19) {                                                // (Weld: a click picks the vertex; let go on another — welded onto it)
    const int src = tt_.wsrc, tgt = tt_.wtgt; const auto srcs = tt_.wsrcs; tt_.wsrc = tt_.wtgt = -1; tt_.wsrcs.clear(); ui::hud({});
    if (!tt_.moved) { if (tKey() == 'v') { tselApply(m, {src}, "set"); tsel_.last = src; } ttTouch(); return true; }
    if (tgt < 0 || !it) { ttTouch(); return true; }
    const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); std::vector<int> to(E.pos.size(), -1); for (int s : srcs) if (s != tgt) to[size_t(s)] = tgt;
    E.weld(to); return !ttDone(m, E, B, srcs.size() == 1 ? tr("Welded onto the vertex") : tr("Welded: %1 vertices onto one").arg(srcs.size()), 'v', {tgt}).isEmpty(); }
  if (kind == 17) { if (!tt_.moved && tt_.adj0 == 0 && tsel_.f.size() + tsel_.e.size() + tsel_.v.size()) tselClear(); return true; }   // (a click in free space: the selection off, as the Cursor; on the selection — it stays)
  if (kind == 18) { if (tt_.moved && std::abs(ttools.brTension - tt_.adjDone) > 1e-9) modelReapply(); saveModelPrefs(); ui::hud({}); emit topoState(); return true; }
  if (kind == 9 || kind == 10) {                                   // (the drag's last value shown, kept)
    if (kind == 9 ? std::abs(ttools.fillFlow - tt_.adjDone) > 1e-9 : ttools.fillSub != int(tt_.adjDone)) fillAdjustShow();
    saveModelPrefs(); ui::hud({}); emit topoState(); return true; }
  if (!it && tool_ == "tpen" && kind == 1 && !tt_.moved) {     // (no mesh yet: the click goes on the polygon that will make one)
    const qint64 now = QDateTime::currentMSecsSinceEpoch(); const bool dbl = now - tt_.lastClick < 400 && std::hypot(mp.x() - tt_.lastClickPos.x(), mp.y() - tt_.lastClickPos.y()) < 6;
    tt_.lastClick = now; tt_.lastClickPos = mp; penClick(tt_.pressPt, dbl && !tt_.chain.empty()); return true; }
  if (!it) return true;
  if (kind == 1 && !tt_.moved) {                                   // (the pen's click)
    const qint64 now = QDateTime::currentMSecsSinceEpoch(); const bool dbl = now - tt_.lastClick < 400 && std::hypot(mp.x() - tt_.lastClickPos.x(), mp.y() - tt_.lastClickPos.y()) < 6;
    tt_.lastClick = now; tt_.lastClickPos = mp;
    penClick(tt_.pressPt, dbl && !tt_.chain.empty()); return true;
  }
  if (kind == 2 && !tt_.moved) { penDelete(tt_.pressPt, tt_.pressF); return true; }
  if (kind == 1 && tt_.moved) {                                    // (the pen's drag ended: a vertex let go on another — welded)
    { // (each moved vertex let go on another — a vertex, an edge or a polygon dragged — welded to it; onto a vertex of its
      // own polygons only when put right onto it (a sliver collapsed on purpose — not an edge folded by a drag passing near), not onto one hidden behind the surface)
      Projector P = projector(); const auto W = ttWorld(m); std::set<int> mv(tt_.verts.begin(), tt_.verts.end()); std::vector<int> to(W.size(), -1); int n = 0;
      const PolyMesh& B0 = *baseOf(*it); std::map<int, std::set<int>> own; std::vector<char> used(W.size(), 0);
      const std::vector<char> hidF = meshmods::hiddenFaces(*it, B0);   // (a hidden polygroup's vertices: not welded onto)
      for (int fi = 0; fi < B0.faceCount(); fi++) { const int s0 = B0.faceStart[size_t(fi)], e0 = B0.faceStart[size_t(fi) + 1]; bool has = false; const bool vis = hidF.empty() || !hidF[size_t(fi)];
        for (int k = s0; k < e0; k++) { const int x = B0.faceVerts[size_t(k)]; if (vis && size_t(x) < used.size()) used[size_t(x)] = 1; if (mv.count(x)) has = true; }
        if (has) for (int k = s0; k < e0; k++) for (int k2 = s0; k2 < e0; k2++) if (mv.count(B0.faceVerts[size_t(k)])) own[B0.faceVerts[size_t(k)]].insert(B0.faceVerts[size_t(k2)]); }
      auto seen = [&](const Vec3& p) { return ttSeen(p, m); };   // (not one behind the surface — its own or a reference's)
      std::vector<std::pair<Vec3, Vec3>> doubt; std::set<int> doubtTo;   // (within the weld radius but not for sure — its own polygon's vertex, one behind the surface, past the narrowed radius: asked)
      for (int v : tt_.verts) { if (size_t(v) >= W.size()) continue; int target = -1; ScreenPt sv = P.toScreen(W[size_t(v)]); const auto& ov = own[v];
        // (the weld radius no wider than about half its shortest side on the screen: small parts — an eyelid, a nostril — not
        // welded to their own neighbours that happen to lie within it)
        double bd = ttools.penWeld; for (int w : ov) if (w != v && size_t(w) < W.size()) { const ScreenPt s = P.toScreen(W[size_t(w)]); if (s.z < 1) bd = std::min(bd, std::max(3.0, 0.45 * std::hypot(s.x - sv.x, s.y - sv.y))); }
        for (size_t w = 0; w < W.size(); w++) { if (mv.count(int(w)) || !used[w]) continue; ScreenPt s = P.toScreen(W[w]); const double d = std::hypot(s.x - sv.x, s.y - sv.y); if (s.z < 1 && d < (ov.count(int(w)) ? std::min(bd, 4.0) : bd) && seen(W[w])) { bd = d; target = int(w); } }
        if (target >= 0) { to[size_t(v)] = target; n++; continue; }
        if (sv.z >= 1) continue;
        double dd = ttools.penWeld; int maybe = -1;
        const double near3 = ttools.penWeld * 2 * worldPerPixelAt(W[size_t(v)]);   // (near in space too: not one on the far side of the mesh that only lines up with it on the screen)
        for (size_t w = 0; w < W.size(); w++) { if (mv.count(int(w)) || !used[w]) continue; const ScreenPt s = P.toScreen(W[w]); const double d = std::hypot(s.x - sv.x, s.y - sv.y); if (s.z < 1 && d < dd && (!ov.count(int(w)) || d < 8) && W[w].distanceTo(W[size_t(v)]) <= near3) { dd = d; maybe = int(w); } }   // (its own polygon.s vertex: asked only when put close onto it)
        if (maybe >= 0 && !doubtTo.count(maybe)) { doubt.push_back({W[size_t(v)], W[size_t(maybe)]}); doubtTo.insert(maybe); } }
      if (n && !tt_.symPairs.empty()) { const auto& Sp = tt_.symPairs; const auto t0 = to;   // (Symmetry: the mirror vertex welded onto the target's mirror too)
        for (int v : tt_.verts) { if (size_t(v) >= t0.size() || t0[size_t(v)] < 0 || size_t(v) >= Sp.size()) continue; const int pv = Sp[size_t(v)], pt = size_t(t0[size_t(v)]) < Sp.size() ? Sp[size_t(t0[size_t(v)])] : -1;
          if (pv >= 0 && pt >= 0 && pv != v && pv != pt && size_t(pv) < to.size() && to[size_t(pv)] < 0 && (size_t(pt) >= to.size() || to[size_t(pt)] != pv)) to[size_t(pv)] = pt; } }
      if (n) { MeshEd E = MeshEd::from(*baseOf(*it)); to.resize(E.pos.size(), -1); E.weld(to); const bool ok = !ttDone(m, E, *baseOf(*it), n == 1 ? tr("Welded") : tr("Welded: %1 vertices").arg(n)).isEmpty();
        if (!doubt.empty()) { penAsk_.m = m; penAsk_.pairs = doubt; penWeldAsk(mp); }
        return ok; }
      if (!doubt.empty()) { topoWrite(m, baseSp(*it)); commit(false); emit topoState(); ttTouch(); penAsk_.m = m; penAsk_.pairs = doubt; penWeldAsk(mp); return true; }
    }
    topoWrite(m, baseSp(*it)); commit(false); emit topoState(); ttTouch(); return true;
  }
  if (kind == 2 && tt_.moved) {                                    // (Ctrl + drag: pulled out of an edge / a polygon)
    const Affine Mi = meshmods::meshMatrix(*scene_, *it).inverse(); const Vec3 l = Mi.lin(tt_.delta);
    const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); tt_.loopPrev.clear();
    // (the new vertices: onto the surface when Snap is on, and each on an old vertex near it on screen — welded there; not on
    // those it was pulled out of)
    auto settle = [&](MeshEd& E2, const std::vector<int>& nv) {
      const Affine M = meshmods::meshMatrix(*scene_, *it); Projector P = projector();
      if (tset.snap) for (int v : nv) E2.pos[size_t(v)] = Mi.apply(ttSnapW(M.apply(E2.pos[size_t(v)]), m));
      std::set<int> skip(nv.begin(), nv.end()); skip.insert(tt_.verts.begin(), tt_.verts.end()); std::set<int> used; { const auto sh = shownVerts(*it, B); for (auto& F : E2.f) for (int x : F.v) if (sh.empty() || size_t(x) >= sh.size() || sh[size_t(x)]) used.insert(x); }   // (not onto a hidden polygroup's)
      std::vector<int> to(E2.pos.size(), -1); bool any = false;
      double weldCap = 1e9;   // (no wider than about half the side pulled out, on the screen: a small one not welded back onto its own neighbours)
      if (tt_.verts.size() == 2) { const ScreenPt s0 = P.toScreen(M.apply(E2.pos[size_t(tt_.verts[0])])), s1 = P.toScreen(M.apply(E2.pos[size_t(tt_.verts[1])])); weldCap = std::max(3.0, 0.45 * std::hypot(s0.x - s1.x, s0.y - s1.y)); }
      for (int v : nv) { const ScreenPt sv = P.toScreen(M.apply(E2.pos[size_t(v)])); double bd = std::min(ttools.penWeld, weldCap); int target = -1;
        for (int w : used) { if (skip.count(w)) continue; const ScreenPt s = P.toScreen(M.apply(E2.pos[size_t(w)])); const double d = std::hypot(s.x - sv.x, s.y - sv.y); if (s.z < 1 && sv.z < 1 && d < bd && ttSeen(M.apply(E2.pos[size_t(w)]), m)) { bd = d; target = w; } }   // (not one hidden behind a surface)
        if (target >= 0) { to[size_t(v)] = target; any = true; } }
      if (any) E2.weld(to);
      return to; };
    if (tt_.verts.size() == 2 && tt_.pressPt.ea >= 0) {
      const int a = tt_.pressPt.ea, b = tt_.pressPt.eb; TTPt over = ttPickPt(mp, m, false);
      if (over.ea >= 0 && ek(over.ea, over.eb) != ek(a, b)) {      // (let go on another edge: the gap between them)
        const int c = over.ea, d = over.eb; std::vector<int> vs;
        if (c == a || c == b || d == a || d == b) { const int shared = (c == a || c == b) ? c : d, far = shared == c ? d : c; vs = {a, b}; if (far != a && far != b) vs.push_back(far); }
        else { const double s1 = E.pos[size_t(b)].distanceTo(E.pos[size_t(c)]) + E.pos[size_t(a)].distanceTo(E.pos[size_t(d)]), s2 = E.pos[size_t(b)].distanceTo(E.pos[size_t(d)]) + E.pos[size_t(a)].distanceTo(E.pos[size_t(c)]);
          vs = s1 <= s2 ? std::vector<int>{a, b, c, d} : std::vector<int>{a, b, d, c}; }
        if (vs.size() >= 3) { const int fi = E.addFace(vs); faceCam(E, fi, Mi.inverse(), cam_); tt_.symNew = true; return !ttDone(m, E, B, tr("The gap between the edges filled"), 'f', {fi}).isEmpty(); }
      }
      int a2 = E.addVert(E.pos[size_t(a)] + l), b2 = E.addVert(E.pos[size_t(b)] + l);
      const int fi = E.addFace({a, b, b2, a2}); (void)fi;
      const auto to = settle(E, {a2, b2}); if (to[size_t(a2)] >= 0) a2 = to[size_t(a2)]; if (to[size_t(b2)] >= 0) b2 = to[size_t(b2)];
      tt_.symNew = true; return !ttDone(m, E, B, tr("A polygon pulled out of the edge"), 'e', {}, {{a2, b2}}).isEmpty();
    }
    if (tt_.pressF >= 0) {                                         // (a polygon extruded: moved out, walls to where it was)
      const int fi = tt_.pressF; const auto V = E.f[size_t(fi)].v; std::vector<int> nv; for (int v : V) nv.push_back(E.addVert(E.pos[size_t(v)] + l));
      const int g = E.f[size_t(fi)].g, mt = E.f[size_t(fi)].mat; E.f[size_t(fi)].v = nv;
      for (size_t k = 0; k < V.size(); k++) { const size_t k2 = (k + 1) % V.size(); MeshEd::F F; F.v = {V[k], V[k2], nv[k2], nv[k]}; F.g = g; F.mat = mt; if (E.hasUV) F.uv.assign(4, {0, 0}); E.f.push_back(F); }
      return !ttDone(m, E, B, tr("Polygon extruded"), 'f', {fi}).isEmpty();
    }
    ttTouch(); return true;
  }
  if (kind == 4) { tt_.kdc = tt_.kda = -1; ttTouch(); return true; }
  if (kind == 5) {                                                 // (a straight cut drawn from empty space)
    if (std::hypot(tt_.p1.x() - tt_.p0.x(), tt_.p1.y() - tt_.p0.y()) < 6) return true;
    const Vec3 c = it->poly ? it->poly->bounds().center() : cam_.pivot; TTState::KCurve K; K.line = true;
    for (QPointF q : {tt_.p0, tt_.p1}) if (auto h = planeHit(ray(q), c, cam_.dir())) K.a.push_back(*h);
    if (K.a.size() == 2) { tt_.kc.push_back(K); knifeCut(); }
    ttTouch(); return true;
  }
  if (kind == 6) { chainApply(true); tt_.eca = -1; return true; }
  if (kind == 7) {                                                 // (the Plane Cut's line: the plane through it, edge-on to the view)
    if (std::hypot(tt_.p1.x() - tt_.p0.x(), tt_.p1.y() - tt_.p0.y()) < 6) { overlayRev_++; requestUpdate(); return true; }
    const Vec3 c = it->poly ? it->poly->bounds().center() : cam_.pivot; auto h0 = planeHit(ray(tt_.p0), c, cam_.dir()), h1 = planeHit(ray(tt_.p1), c, cam_.dir());
    overlayRev_++;
    if (h0 && h1) { const Vec3 along = (*h1 - *h0).normalized(), n = along.cross(cam_.dir()).normalized(); planeApply(m, *h0, n, along); }
    return true;
  }
  if (kind == 8) {
    if (!tt_.moved) { bevelApply(m, tool_ == "tinset" ? ttools.insOffset : ttools.bevOffset, tool_ == "tinset"); }
    else { (tool_ == "tinset" ? ttools.insOffset : ttools.bevOffset) = tt_.delta.x; saveModelPrefs(); }
    tt_.before.reset();
    scene_->revision++; commit(true); emit topoState(); ui::hud({});
    tt_.lastOp = tool_ == "tinset" ? "inset" : "bevel"; tt_.lastM = m; tt_.lastAfter = baseSp(*scene_->item(m));
    return true;
  }
  ttTouch(); return true;
}

// RMB of the tools: the Loop — cut and select the new edges; Weld — weld; the Shift+RMB menus
bool Viewport::topoToolRmb(QMouseEvent* e) {
  if (!isModelTool() || e->button() != Qt::RightButton) return false;
  if (tool_ == "tpatch") return patchRmb(e);
  if (tool_ == "tlobe") return lobeRmb(e);
  const QPointF mp = e->position(); const bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  if (nBrushOn() && ctrl && !shift) { bd_ = {}; bd_.start = mp; bd_.r0 = brushR; bd_.h0 = brushHard; drag_ = Drag::BrushAdjust; return true; }   // (the alignment brush: Ctrl + RMB drag — its size)
  if (tool_ == "treveal") {                                      // (the Reveal Brush: Ctrl + RMB drag — its size, as the brushes; no menus)
    if (ctrl) { bd_ = {}; bd_.start = mp; bd_.r0 = brushR; bd_.h0 = brushHard; drag_ = Drag::BrushAdjust; } return true; }
  const QPoint g = e->globalPosition().toPoint();
  const int m = topoMesh(); if (m >= 0) tt_.m = m;
  if ((tool_ == "tloop" || (tool_ == "tpen" && ctrl && shift)) && !(shift && !ctrl)) { if (tt_.le >= 0 && m >= 0) loopApply(m, tt_.le, tt_.lt, true, ttools.loopCuts, ttools.loopParallel || tt_.lctrl); tt_.le = -1; tt_.loopPrev.clear(); ttTouch(); return true; }
  if (tool_ == "tpen" && shift && !ctrl) { tt_.m = m; penFill(mp); return true; }   // (no mesh: the anchors make a new RetopoMesh)
  if (tool_ == "tpen" && !shift && !ctrl) {                       // (RMB held: the topology under the circle smoothed — relaxed on its own surface, snapped as any edit)
    if (m < 0) { ui::hud(tr("Select a mesh first"), 900); return true; }
    if (topoHeavy(m)) { ui::toast(tr("A high-poly mesh — Object mode only")); return true; }
    tbPress(mp, false, false, penBrushKind(), ttools.penSmooth, m);
    return true; }
  if (tool_ == "tweld" && !shift && !ctrl) { weldNow(&mp); return true; }
  if ((tool_ == "tshaper" && !ctrl) || (tool_ == "tslide" && !ctrl && !shift) || (tool_ == "tnormals" && tt_.arm == "nturn") || (tool_ == "toptimize" && tt_.arm == "optpick"))
    if (shapePress(mp, true, shift, ctrl)) return true;            // (a drag in empty space; on the mesh — the usual menus)
  if (tool_ == "tfill" && shift != ctrl) {                         // (Shift + RMB drag — Edge Flow, Ctrl + RMB drag — Subdivision: ←→)
    tt_.m = m; tt_.press = mp; tt_.moved = false; tt_.drag = shift ? 9 : 10; tt_.adj0 = tt_.adjDone = shift ? ttools.fillFlow : double(ttools.fillSub);
    td_ = {}; td_.type = 20; drag_ = Drag::Topo;
    ui::hud(shift ? tr("Edge Flow: %1% — drag ←→").arg(int(std::lround(ttools.fillFlow * 100))) : tr("Subdivision: %1 — drag ←→").arg(ttools.fillSub)); return true; }
  if (tool_ == "tbridge" && !shift && !ctrl) {                    // (Bridge: RMB held, ←→ — the bulge)
    tt_.m = m; tt_.press = mp; tt_.moved = false; tt_.drag = 18; tt_.adj0 = tt_.adjDone = ttools.brTension; td_ = {}; td_.type = 20; drag_ = Drag::Topo;
    ui::hud(tr("Bridge: bulge %1 — drag ←→").arg(ttools.brTension, 0, 'f', 2)); return true; }
  if (tool_ == "tfill" && !shift && !ctrl) {                       // (RMB: the next way of filling it — turned round)
    tt_.fillOff++; fillPreview(mp); ui::hud(tr("Fill: variant %1").arg(tt_.fillOff + 1), 900); return true; }
  if ((tool_ == "tknife" || (tool_ == "tchain" && tt_.ec.m >= 0)) && !shift) {   // (Knife / Smooth EdgeChain: RMB on the curve — a new anchor there; Ctrl+RMB on an anchor — it goes)
    Projector P = projector(); const bool kn = tool_ == "tknife";
    const Affine Mc = !kn && scene_->item(tt_.ec.m) ? meshmods::meshMatrix(*scene_, *scene_->item(tt_.ec.m)) : Affine();
    struct Cv { std::vector<Vec3> a; bool closed; }; std::vector<Cv> cs;
    if (kn) for (auto& K : tt_.kc) cs.push_back({K.a, false}); else { Cv c{{}, tt_.ec.closed}; for (auto& a : tt_.ec.a) c.a.push_back(Mc.apply(a)); cs.push_back(c); }
    if (ctrl) { int bc = -1, ba = -1; double bd = 12;
      for (size_t c = 0; c < cs.size(); c++) for (size_t a = 0; a < cs[c].a.size(); a++) { const ScreenPt s = P.toScreen(cs[c].a[a]); const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (s.z < 1 && d < bd) { bd = d; bc = int(c); ba = int(a); } }
      if (ba < 0) { ui::hud(tr("Ctrl+RMB on an anchor deletes it"), 900); return true; }
      if (kn) { kSave(); auto& A = tt_.kc[size_t(bc)].a; A.erase(A.begin() + ba); if (A.empty()) { tt_.kc.erase(tt_.kc.begin() + bc); tt_.kcur = -1; } ttTouch(); }
      else { if (tt_.ec.a.size() <= 2) { ui::toast(tr("The curve keeps two anchors at least")); return true; } tt_.ec.a.erase(tt_.ec.a.begin() + ba); chainApply(true); }
      ui::hud(tr("Anchor deleted"), 700); return true; }
    int bc = -1, bs = -1; double bd = 10;                          // (the span nearest the pointer — along the drawn curve)
    for (size_t c = 0; c < cs.size(); c++) { const auto& A = cs[c].a; const size_t spans = cs[c].closed ? A.size() : (A.size() ? A.size() - 1 : 0); if (!spans) continue;
      auto L = catmull(A, cs[c].closed, 12);
      for (size_t i = 0; i + 1 < L.size(); i++) { const ScreenPt s0 = P.toScreen(L[i]), s1 = P.toScreen(L[i + 1]); if (s0.z >= 1 || s1.z >= 1) continue; double t;
        const double d = segDistT(mp.x(), mp.y(), s0.x, s0.y, s1.x, s1.y, &t); if (d < bd) { bd = d; bc = int(c); bs = int(std::min(spans - 1, i / 12)); } } }
    if (bs < 0) { ui::hud(tr("RMB on the curve adds an anchor there"), 900); return true; }
    Vec3 w; if (auto r = tPick(mp, {kn ? m : tt_.ec.m})) w = r->point; else { const Vec3 c0 = cs[size_t(bc)].a[size_t(bs)]; auto h = planeHit(ray(mp), c0, cam_.dir()); w = h ? *h : c0; }
    if (kn) { if (int v = ttVertNear(m, mp, std::max(8.0, ttools.knifeWeld)); v >= 0) w = ttWorld(m)[size_t(v)]; kSave(); auto& A = tt_.kc[size_t(bc)].a; A.insert(A.begin() + bs + 1, w); ttTouch(); }
    else { tt_.ec.a.insert(tt_.ec.a.begin() + bs + 1, Mc.inverse().apply(w)); chainApply(true); }
    ui::hud(tr("Anchor added"), 700); return true;
  }
  if (tool_ == "tknife" && shift && !ctrl) {
    ui::PieSpec s; s.title = tr("Knife");
    auto item = [](const QString& l, std::function<void()> f) { ui::PieItem i; i.label = l; i.act = std::move(f); return i; };
    s.top = {item(tr("✂ Cut"), [this] { knifeCut(); }),
             item(tr("Optimise the curve"), [this] { kSave(); for (auto& K : tt_.kc) if (!K.line && K.a.size() > 3) { std::vector<Vec3> o{K.a[0]}; for (size_t i = 1; i + 1 < K.a.size(); i += 2) o.push_back(K.a[i]); o.push_back(K.a.back()); K.a = o; } ttTouch(); }),
             item(tr("Recompute the curve"), [this, m] { if (m < 0) return; kSave(); auto bv = bvhOf(m, false);
               for (auto& K : tt_.kc) if (!K.line && K.a.size() >= 2) { auto L = catmull(K.a, false, 8); auto cum = cumLen(L); const size_t n = K.a.size(); std::vector<Vec3> o;
                 for (size_t i = 0; i < n; i++) { Vec3 p = alongLen(L, cum, double(i) / (n - 1)); if (bv) { double d; if (auto q = bv->closest(p, nullptr, &d)) p = *q; } o.push_back(p); } K.a = o; }
               ttTouch(); })};
    s.bottom = {item(tr("Delete the curve"), [this] { kSave(); if (!tt_.kc.empty()) tt_.kc.erase(tt_.kc.begin() + (tt_.kcur >= 0 ? tt_.kcur : int(tt_.kc.size()) - 1)); tt_.kcur = -1; ttTouch(); }),
                item(tr("Delete all curves"), [this] { kSave(); tt_.kc.clear(); tt_.kcur = -1; ttTouch(); })};
    ui::Pie::open(mainWin(), g, s, true); return true;
  }
  if (tool_ == "tchain" && shift && !ctrl) {
    ui::PieSpec s; s.title = tr("Edge chain");
    auto item = [](const QString& l, std::function<void()> f) { ui::PieItem i; i.label = l; i.act = std::move(f); return i; };
    if (tt_.ec.m < 0) s.top = {item(tr("Curve from the selected edges"), [this] { chainCreate(); })};
    else {
      s.top = {item(tr("Smooth the curve"), [this] { auto& A = tt_.ec.a; const int n = int(A.size()); std::vector<Vec3> nA = A;
                  for (int i = 0; i < n; i++) { if (!tt_.ec.closed && (i == 0 || i == n - 1)) continue; nA[size_t(i)] = A[size_t(i)] * 0.5 + (A[size_t((i + n - 1) % n)] + A[size_t((i + 1) % n)]) * 0.25; }
                  A = nA; chainApply(true); }),
               item(tr("Optimise the curve"), [this] { auto& A = tt_.ec.a; if (A.size() > 3) { std::vector<Vec3> o{A[0]}; for (size_t i = 1; i + 1 < A.size(); i += 2) o.push_back(A[i]); o.push_back(A.back()); A = o; } chainApply(true); }),
               item(tr("Even the edges along it"), [this] { ttools.chainEqual = true; saveModelPrefs(); chainApply(true); emit topoState(); }),
               item(tr("Recompute the handles"), [this] {   // (the anchors laid evenly along the curve as it is now: its handles — the turns at them — made anew)
                 auto& A = tt_.ec.a; const size_t n = A.size(); if (n < 3) return; const auto P = catmull(A, tt_.ec.closed, 24); if (P.size() < 2) return;
                 std::vector<double> c(P.size(), 0); for (size_t i = 1; i < P.size(); i++) c[i] = c[i - 1] + P[i].distanceTo(P[i - 1]);
                 const double L = c.back(); std::vector<Vec3> nA; const size_t steps = tt_.ec.closed ? n : n - 1;
                 for (size_t k = 0; k < n; k++) { const double s = L * double(k) / double(steps); size_t j = 1; while (j + 1 < P.size() && c[j] < s) j++;
                   const double t = c[j] > c[j - 1] ? std::clamp((s - c[j - 1]) / (c[j] - c[j - 1]), 0.0, 1.0) : 0.0; nA.push_back(P[j - 1] + (P[j] - P[j - 1]) * t); }
                 A = nA; chainApply(true); })};
      s.bottom = {item(tr("Delete the curve"), [this] { tt_.ec = {}; ttTouch(); emit topoState(); }),
                  item(tr("Curve from the selected edges (anew)"), [this] { chainCreate(); })};
    }
    ui::Pie::open(mainWin(), g, s, true); return true;
  }
  return false;
}

bool Viewport::topoToolKey(int key, Qt::KeyboardModifiers mods) {
  if (!isModelTool()) return false;
  if (tool_ == "tpatch") return patchKey(key);
  if (tool_ == "tlobe") return lobeKey(key);
  (void)mods;
  if (key == Qt::Key_Escape) {
    if (!tt_.chain.empty()) { tt_.chain.clear(); ttTouch(); ui::hud(tr("Polygon dropped"), 900); return true; }
    if (tt_.kcur >= 0) { tt_.kcur = -1; ttTouch(); ui::hud(tr("Curve ended — another click starts a new one; Enter cuts"), 1200); return true; }
    if (!tt_.kc.empty()) { kSave(); tt_.kc.clear(); ttTouch(); return true; }
    if (tt_.ec.m >= 0) { tt_.ec = {}; ttTouch(); emit topoState(); return true; }
    return false;
  }
  if ((key == Qt::Key_Return || key == Qt::Key_Enter) && tool_ == "tknife") { knifeCut(); return true; }
  if ((key == Qt::Key_Return || key == Qt::Key_Enter) && tool_ == "tpen" && tt_.chain.size() >= 3) { TTPt first = tt_.chain[0]; penClick(first, false); return true; }
  if ((key == Qt::Key_BracketLeft || key == Qt::Key_BracketRight) && tool_ == "tpen") {
    ttools.penFill = std::clamp(ttools.penFill * (key == Qt::Key_BracketRight ? 1.15 : 0.87), 10.0, 400.0); saveModelPrefs();
    ui::hud(tr("Fill radius: %1 px").arg(int(std::lround(ttools.penFill))), 900); tt_.lastClick = 0; fillShow_ = QDateTime::currentMSecsSinceEpoch(); overlayRev_++; requestUpdate(); emit topoState(); return true;
  }
  return false;
}

bool Viewport::topoToolWheel(int steps, Qt::KeyboardModifiers mods) {
  const bool smoothing = drag_ == Drag::Topo && td_.type == 8, bare = !(mods & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier));
  if (tool_ == "tpen" && (mods & Qt::ControlModifier) && !(mods & (Qt::ShiftModifier | Qt::AltModifier))) {   // (PolyPen: Ctrl+wheel — the RMB brush's kind)
    static const char* K[] = {"relax", "smooth", "inflate", "pinch"}; static const char* N[] = {"Relax", "Smooth", "Inflate", "Pinch"};
    int i = 0; for (int k = 0; k < 4; k++) if (ttools.penBrush == K[k]) i = k;
    i = (i + (steps > 0 ? 1 : -1) + 4) % 4; ttools.penBrush = K[i]; saveModelPrefs();
    if (smoothing) tb_.kind = penBrushKind();
    smoothShow_ = QDateTime::currentMSecsSinceEpoch(); ui::hud(tr("RMB brush: %1").arg(tr(N[i])), 900); overlayRev_++; requestUpdate(); panelsLater(); return true;
  }
  if (tool_ == "tpatch" && bare && patchWheel(steps)) return true;   // (PatchTool: the wheel over a curve — its edges)
  if (nBrushOn() && bare && pickSurface(mouse_)) { brushResize(steps > 0 ? 1.1 : 0.9); ui::hud(tr("Alignment brush: %1 px").arg(int(std::lround(brushR))), 800); panelsLater(); return true; }
  if (tool_ == "treveal" && bare && pickSurface(mouse_)) { brushResize(steps > 0 ? 1.1 : 0.9); ui::hud(tr("Reveal Brush: %1 px").arg(int(std::lround(brushR))), 800); panelsLater(); return true; }   // (Reveal: the wheel over the geometry — its size)
  // (PolyPen: the wheel over the geometry — the RMB brush's radius, the camera stays; off it — the camera as ever)
  if (tool_ == "tpen" && bare && (smoothing || pickSurface(mouse_))) {
    ttools.penSmooth = std::clamp(ttools.penSmooth * (steps > 0 ? 1.1 : 0.9), 8.0, 800.0); saveModelPrefs();
    if (smoothing) { Projector P = projector(); const ScreenPt a = P.toScreen(tb_.hit0); const double u = cam_.sceneScale * 0.01; const ScreenPt b = P.toScreen(tb_.hit0 + cam_.upVec() * u);
      tb_.Rw = ttools.penSmooth * u / std::max(1e-9, std::hypot(b.x - a.x, b.y - a.y)); }
    smoothShow_ = QDateTime::currentMSecsSinceEpoch(); ui::hud(tr("Brush radius: %1 px").arg(int(std::lround(ttools.penSmooth))), 900); overlayRev_++; requestUpdate(); panelsLater(); return true;
  }
  if (tool_ == "tbridge" && bare) {                                // (Bridge: the wheel — its subdivision, the last bridge made again)
    ttools.brSeg = std::clamp(ttools.brSeg + (steps > 0 ? 1 : -1), 0, 64); saveModelPrefs(); modelReapply();
    ui::hud(tr("Bridge: %1 subdivision%2").arg(ttools.brSeg).arg(ttools.brSeg == 1 ? "" : "s"), 900); panelsLater(); return true; }
  if (tool_ == "tfill" && !(mods & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier))) {   // (Fill Hole: the wheel — the kind of filling)
    static const char* K[] = {"quads", "quadtri", "fan", "tris", "ngon"}; static const char* N[] = {"Quads", "Quads + tris", "Tris to a centre", "Tris, no centre", "N-gon"};
    int i = 0; for (int k = 0; k < 5; k++) if (ttools.fillMode == K[k]) i = k;
    i = (i + (steps > 0 ? 1 : -1) + 5) % 5; ttools.fillMode = K[i]; saveModelPrefs();
    tt_.press = tt_.mouse; fillAdjustShow(); emit topoState(); ui::hud(tr("Fill: %1").arg(tr(N[i])), 1000); return true;
  }
  const bool loopMode = tool_ == "tloop" || (tool_ == "tpen" && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier));
  if (!loopMode || tt_.le < 0) return false;
  ttools.loopCuts = std::clamp(ttools.loopCuts + (steps > 0 ? 1 : -1), 1, 32); saveModelPrefs();
  topoToolMove(tt_.mouse, mods); emit topoState(); return true;
}

// ---------- what they show ----------
void Viewport::topoToolDraw(TopoDraw& D) {
  if (!isModelTool()) return;
  if (tool_ == "tpatch") { patchDraw(D); return; }
  if (tool_ == "tlobe") { lobeDraw(D); return; }
  auto poly = [&](const std::vector<Vec3>& L, const QColor& c, float w, bool closed) {
    TopoDraw::Lines l; l.c = c; l.w = w; for (size_t i = 0; i + 1 < L.size(); i++) { l.p.push_back(L[i]); l.p.push_back(L[i + 1]); }
    if (closed && L.size() > 2) { l.p.push_back(L.back()); l.p.push_back(L.front()); } if (!l.p.empty()) D.lines.push_back(std::move(l)); };
  auto pts = [&](const std::vector<Vec3>& P, const QColor& c, float size) { TopoDraw::Pts p; p.size = size; p.xray = true; for (auto& x : P) { p.p.push_back(x); p.c.push_back(c); } if (!p.p.empty()) D.pts.push_back(std::move(p)); };
  const int m = (tool_ == "tnormals" || (tool_ == "tweld" && drag_ != Drag::Topo)) && topoMesh() >= 0 ? topoMesh()   // (Normals / Weld: the selected mesh — never the one an earlier action left in tt_.m)
              : tt_.m >= 0 && scene_->item(tt_.m) ? tt_.m : topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr;
  const QColor hot(0xff, 0xd8, 0x3a), cut(0xff, 0x6b, 0x4f), cy(0x4c, 0xd6, 0xf0);
  for (auto& L : tt_.loopPrev) poly(L, tool_ == "tpen" && tt_.drag == 2 ? cy : hot, 2.5f, false);
  if (tool_ == "tpen" && tt_.shiftHeld && tt_.chain.empty() && drag_ == Drag::None) {   // (Shift held: the polygon Shift+RMB would fill)
    std::vector<Vec3> L; for (auto& p : penFillPoints(tt_.mouse)) L.push_back(ttPtW(p));   // (the user's anchors first, then the ghost's polygon)
    if (!L.empty()) { poly(L, cy, 2.5f, true); pts(L, cy, 9);
      TopoDraw::Tris tr2; tr2.c = cy; tr2.op = 0.2f; for (size_t k = 1; k + 1 < L.size(); k++) { tr2.p.push_back(L[0]); tr2.p.push_back(L[k]); tr2.p.push_back(L[k + 1]); } D.tris.push_back(std::move(tr2)); }
  }
  if (tool_ == "tpen" && !it) {                                  // (no mesh yet: the polygon being drawn for a new one)
    pts(tt_.anchors, QColor(0xff, 0x9a, 0x2e), 9);
    auto ptW = [&](const TTPt& p) { return p.anchor >= 0 && size_t(p.anchor) < tt_.anchors.size() ? tt_.anchors[size_t(p.anchor)] : p.w; };
    if (tt_.hovOn) pts({ptW(tt_.hov)}, Qt::white, 9);
    if (!tt_.chain.empty()) { std::vector<Vec3> L; for (auto& p : tt_.chain) L.push_back(ptW(p)); if (tt_.hovOn && inViewport_) L.push_back(ptW(tt_.hov)); poly(L, hot, 2.5f, L.size() >= 3); pts(L, hot, 8);
      if (L.size() >= 3) { TopoDraw::Tris tr2; tr2.c = hot; tr2.op = 0.18f; for (size_t k = 1; k + 1 < L.size(); k++) { tr2.p.push_back(L[0]); tr2.p.push_back(L[k]); tr2.p.push_back(L[k + 1]); } D.tris.push_back(std::move(tr2)); } }
  }
  if (tool_ == "tpen" && it) {
    pts(tt_.anchors, QColor(0xff, 0x9a, 0x2e), 9);
    const auto W = ttWorld(m); auto T = topo::ttopo(baseSp(*it));
    auto ptW = [&](const TTPt& p) { if (p.v >= 0 && size_t(p.v) < W.size()) return W[size_t(p.v)]; if (p.ea >= 0 && size_t(std::max(p.ea, p.eb)) < W.size()) return W[size_t(p.ea)] + (W[size_t(p.eb)] - W[size_t(p.ea)]) * p.t; if (p.anchor >= 0 && size_t(p.anchor) < tt_.anchors.size()) return tt_.anchors[size_t(p.anchor)]; return p.w; };
    if (drag_ != Drag::Topo || td_.type != 20) {
      const TTPt& h = tt_.hov;
      if (h.v >= 0 && size_t(h.v) < W.size()) pts({W[size_t(h.v)]}, Qt::white, 11);
      else if (h.ea >= 0 && size_t(std::max(h.ea, h.eb)) < W.size()) { poly({W[size_t(h.ea)], W[size_t(h.eb)]}, Qt::white, 3.f, false); pts({ptW(h)}, hot, 7); }
      else if (h.anchor >= 0) pts({ptW(h)}, Qt::white, 12);
      else if (tt_.hf >= 0 && tt_.hf < T->nf) { TopoDraw::Tris tr2; tr2.c = Qt::white; tr2.op = 0.15f; const int s = T->fo[size_t(tt_.hf)];
        for (int k = s + 1; k + 1 < T->fo[size_t(tt_.hf) + 1]; k++) { tr2.p.push_back(W[size_t(T->fv[size_t(s)])]); tr2.p.push_back(W[size_t(T->fv[size_t(k)])]); tr2.p.push_back(W[size_t(T->fv[size_t(k + 1)])]); } D.tris.push_back(tr2); }
    }
    if (!tt_.chain.empty()) {                                      // (the polygon being drawn: its corners and the pointer)
      std::vector<Vec3> L; for (auto& p : tt_.chain) L.push_back(ptW(p)); L.push_back(ptW(tt_.hov));
      poly(L, hot, 2.5f, L.size() >= 3); pts(L, hot, 8);
      if (L.size() >= 3) { TopoDraw::Tris tr2; tr2.c = hot; tr2.op = 0.18f; for (size_t k = 1; k + 1 < L.size(); k++) { tr2.p.push_back(L[0]); tr2.p.push_back(L[k]); tr2.p.push_back(L[k + 1]); } D.tris.push_back(tr2); }
    }
  }
  if (tool_ == "tweld" && it) {                                    // (Weld: where the vertices go — the drag's, else the picked ones' onto the vertex under the pointer (else their middle))
    const auto W = ttWorld(m); const QColor wc(0x4c, 0xd6, 0xf0);
    if (drag_ == Drag::Topo && tt_.drag == 19 && tt_.wsrc >= 0 && size_t(tt_.wsrc) < W.size()) {
      Vec3 to = tt_.wtgt >= 0 && size_t(tt_.wtgt) < W.size() ? W[size_t(tt_.wtgt)] : planeHit(ray(tt_.mouse), W[size_t(tt_.wsrc)], cam_.dir()).value_or(W[size_t(tt_.wsrc)]);
      TopoDraw::Lines l; l.c = tt_.wtgt >= 0 ? wc : hot; l.w = 2.5f; std::vector<Vec3> src; for (int s : tt_.wsrcs) if (size_t(s) < W.size()) { l.p.push_back(W[size_t(s)]); l.p.push_back(to); src.push_back(W[size_t(s)]); }
      D.lines.push_back(l); pts(src, hot, 9); if (tt_.wtgt >= 0) pts({to}, wc, 13);
    } else if (drag_ == Drag::None && tsel_.id == m && ttools.weldMode != "pair") {
      auto vs = tSelVerts(m); if (vs.size() >= 2) {
        int t = ttVertNear(m, tt_.mouse, 14); Vec3 to; if (t >= 0) to = W[size_t(t)]; else { for (int v : vs) to += W[size_t(v)]; to = to * (1.0 / double(vs.size())); }
        TopoDraw::Lines l; l.c = wc; l.w = 1.5f; for (int v : vs) { l.p.push_back(W[size_t(v)]); l.p.push_back(to); } D.lines.push_back(l); pts({to}, wc, 12); } }
  }
  if (tool_ == "tnormals" && it && ttools.normShow) {              // (the normals: of the selected vertices — else every one, a few thousand at most)
    const PolyMesh& NB = *baseOf(*it); const Affine M = meshmods::meshMatrix(*scene_, *it);
    std::vector<Vec3> W; W.reserve(NB.pos.size()); for (const Vec3& p : NB.pos) W.push_back(M.apply(p));   // (from the base: the shown copy may lack its hidden parts — its vertices numbered otherwise)
    std::vector<Vec3> cn; if (NB.cornerNormal.size() == NB.faceVerts.size() && !NB.cornerNormal.empty()) for (auto& n : NB.cornerNormal) cn.push_back(Vec3(n[0], n[1], n[2])); else cn = hardCornerNormals(NB);
    std::set<int> sv; if (tsel_.id == m) { auto l = tSelVerts(m); sv.insert(l.begin(), l.end()); }
    const double len = NB.bounds().size().length() / 20 * ttools.normLen; TopoDraw::Lines l; l.c = QColor(0x4c, 0xd6, 0xf0); l.c.setAlphaF(float(std::pow(std::clamp(ttools.normOp, 0.05, 1.0), 2.2)));   // (blended in linear light: the share squared-ish, so the slider reads as it looks) l.w = 1.5f;
    const std::vector<char> hid = faceLocked(m); std::vector<int> faceOf(NB.faceVerts.size(), 0);   // (the hidden polygons' normals not drawn)
    for (int f = 0; f < NB.faceCount(); f++) for (int k = NB.faceStart[size_t(f)]; k < NB.faceStart[size_t(f) + 1]; k++) faceOf[size_t(k)] = f;
    for (size_t k = 0; k < NB.faceVerts.size() && l.p.size() < 12000; k++) { const int v = NB.faceVerts[k]; if (!sv.empty() && !sv.count(v)) continue; if (!hid.empty() && size_t(faceOf[k]) < hid.size() && hid[size_t(faceOf[k])]) continue; if (size_t(v) >= W.size() || k >= cn.size()) continue;
      const Vec3 n = M.lin(cn[k]).normalized(); if (n.dot(cam_.ortho() ? -cam_.dir() : cam_.pos - W[size_t(v)]) <= 0) continue;   // (its back side's: hidden behind it, only their tips sticking out — left out)
      l.p.push_back(W[size_t(v)]); l.p.push_back(W[size_t(v)] + n * len); }
    if (!l.p.empty()) D.lines.push_back(std::move(l));
  }
  if (tool_ == "tfill") for (auto& Pf : tt_.fillPrev) {             // (the filling's polygons: their edges and a tint)
    poly(Pf, hot, 2.f, true); TopoDraw::Tris tr2; tr2.c = hot; tr2.op = 0.25f;
    for (size_t k = 1; k + 1 < Pf.size(); k++) { tr2.p.push_back(Pf[0]); tr2.p.push_back(Pf[k]); tr2.p.push_back(Pf[k + 1]); } D.tris.push_back(tr2); }
  if (tool_ == "tknife") {   // (not cut through: the curve laid on the surface — where the cut will go)
    std::shared_ptr<const topo::Bvh> kb; Affine kM, kMi; if (!ttools.knifeThrough && it) { kb = bvhOf(m, false); kM = meshmods::meshMatrix(*scene_, *it); kMi = kM.inverse(); }
    for (size_t c = 0; c < tt_.kc.size(); c++) { const auto& K = tt_.kc[c];
      std::vector<Vec3> L = K.line ? K.a : catmull(K.a, K.closed, 12); if (kb && !K.line) for (auto& p : L) if (auto q = kb->closest(kMi.apply(p), nullptr)) p = kM.apply(*q);
      poly(L, cut, 2.5f, K.closed && !K.line); pts(K.a, int(c) == tt_.kcur ? hot : cut, 8); } }
  if (tool_ == "tchain" && tt_.ec.m >= 0) if (const Item* ci = scene_->item(tt_.ec.m)) {
    const Affine M = meshmods::meshMatrix(*scene_, *ci); std::vector<Vec3> A; for (auto& a : tt_.ec.a) A.push_back(M.apply(a));
    poly(catmull(A, tt_.ec.closed, 16), cy, 2.5f, false); pts(A, cy, 9); }
}

void Viewport::topoToolOverlay(std::vector<Overlay>& out) const {
  if (!isModelTool()) return;
  if (tool_ == "tpatch") { patchOverlay(out); return; }
  if (tool_ == "tlobe") return;
  auto circle = [&](QPointF c, double r, QColor col) { std::vector<std::pair<double, double>> P; for (int i = 0; i < 48; i++) { double a = 2 * M_PI * i / 48; P.push_back({c.x() + r * std::cos(a), c.y() + r * std::sin(a)}); } out.push_back({P, col, true}); };
  const QPointF mp = mouse_;
  if (tool_ == "toptimize" && (tt_.arm == "optpick" || (drag_ == Drag::Topo && tt_.drag == 16))) {   // (the weld distance: while it is being picked)
    const double px = ttools.optWeld / std::max(1e-12, worldPerPixelAt(cam_.pivot)); circle(drag_ == Drag::Topo && tt_.drag == 16 ? tt_.press : mp, std::clamp(px, 1.0, 2000.0), QColor(0xff, 0xbe, 0x5a)); }
  if (tool_ == "tpen") { circle(mp, ttools.penWeld, QColor(0x7a, 0xb8, 0xff));
    if (QDateTime::currentMSecsSinceEpoch() - fillShow_ < 1500) circle(mp, ttools.penFill, QColor(0xff, 0x9a, 0x2e));
    if ((drag_ == Drag::Topo && td_.type == 8) || QDateTime::currentMSecsSinceEpoch() - smoothShow_ < 1500) circle(mp, ttools.penSmooth, ttools.penBrush == "inflate" ? QColor(0xff, 0x9a, 0x6a) : ttools.penBrush == "pinch" ? QColor(0xc8, 0x8a, 0xff) : QColor(0x7c, 0xe0, 0x8a)); }
  if (tool_ == "treveal") circle(drag_ == Drag::BrushAdjust ? bd_.start : mp, brushR, QColor(0x5a, 0x9c, 0xff));
  if (nBrushOn()) circle(drag_ == Drag::BrushAdjust ? bd_.start : mp, brushR, QColor(0x4c, 0xd6, 0xf0));
  if (tool_ == "tknife") { circle(mp, std::max(2.0, ttools.knifeWeld), QColor(0xff, 0x6b, 0x4f)); if (drag_ == Drag::Topo && tt_.drag == 5) out.push_back({{{tt_.p0.x(), tt_.p0.y()}, {tt_.p1.x(), tt_.p1.y()}}, QColor(0xff, 0x6b, 0x4f), false}); }
  if (tool_ == "tplane" && drag_ == Drag::Topo && tt_.drag == 7) {
    const double dx = tt_.p1.x() - tt_.p0.x(), dy = tt_.p1.y() - tt_.p0.y(), L = std::hypot(dx, dy); out.push_back({{{tt_.p0.x(), tt_.p0.y()}, {tt_.p1.x(), tt_.p1.y()}}, QColor(0xff, 0xd8, 0x3a), false});
    if (L > 1) { const double nx = -dy / L * 14, ny = dx / L * 14, cx = (tt_.p0.x() + tt_.p1.x()) / 2, cy = (tt_.p0.y() + tt_.p1.y()) / 2; out.push_back({{{cx, cy}, {cx + nx, cy + ny}}, QColor(0xff, 0xd8, 0x3a), false}); }
  }
  // (Loop with Shift: the marks along the edge)
  if ((tool_ == "tloop") && tt_.le >= 0 && tt_.lshift) if (const Item* it = scene_->item(tt_.m >= 0 ? tt_.m : topoMesh())) {
    auto T = topo::ttopo(baseSp(*it)); if (tt_.le < T->ne) { const auto W = ttWorld(it->id); Projector P = projector(); ScreenPt a = P.toScreen(W[size_t(T->ea[size_t(tt_.le)])]), b = P.toScreen(W[size_t(T->eb[size_t(tt_.le)])]);
      for (double f : {0.5, 0.25, 0.75, 1.0 / 6, 2.0 / 6, 4.0 / 6, 5.0 / 6}) { const double x = a.x + (b.x - a.x) * f, y = a.y + (b.y - a.y) * f; circle(QPointF(x, y), 3, QColor(0xff, 0xd8, 0x3a)); } } }
}

}  // namespace hm

namespace hm {
// (tests: an anchor put at a (none — zero), Shift+RMB of the PolyPen where the point at shows on screen → its message)
QString Viewport::testPenFill(const Vec3& a, const Vec3& at) {   // (a, at: screen points in x, y — a.x < 0: no anchor)
  tt_.m = topoMesh(); if (a.x >= 0) { if (auto h = pickSurface(QPointF(a.x, a.y))) tt_.anchors.push_back(h->point); else return "no surface for the anchor"; }
  const QPointF mp(at.x, at.y); const auto pick = penFillPick(mp); QString r = QString("pick %1 · ").arg(pick.size());
  return r + penFill(mp);
}
}  // namespace hm

namespace hm {
// (tests: where a PolyPen click at a point of the screen puts its point — and what the first surface there is turned to)
QString Viewport::testFreePoint(const QPointF& mp) {
  const int m = topoMesh(); const Vec3 p = ttFreePoint(mp, m); QString r = (fpRefused_ ? QString("refused") : QString("ok")) + QString(" point %1 %2 %3").arg(p.x).arg(p.y).arg(p.z);
  if (auto h = pickSurface(mp)) r += QString(" · first surface %1 %2 %3 (item %4), %5").arg(h->point.x).arg(h->point.y).arg(h->point.z).arg(h->itemId).arg(h->normal.dot(ray(mp).d) < 0 ? "front" : "back");
  { const Ray R = ray(mp); const SideGuard sg = sideGuard(m); r += QString(" · guard r %1").arg(sg.r);
    for (auto& [id, x] : scene_->items) if (id != m && x.type == ItemType::IMesh && x.poly && scene_->isVisible(x)) if (auto b = bvhOf(id, true)) if (auto h = b->ray(R.o, R.d, 1e12)) {
      const size_t t = size_t(h->second); const Vec3 &a = b->P[b->idx[t * 3]], &bb = b->P[b->idx[t * 3 + 1]], &c = b->P[b->idx[t * 3 + 2]]; const Vec3 n = (bb - a).cross(c - a).normalized(); const Vec3 q = R.o + R.d * h->first;
      double best = sg.r * sg.r; Vec3 bn, bc; if (sg.mb) sg.mb->sphere(q, sg.r, [&](int u) { const Vec3 &x0 = sg.mb->P[sg.mb->idx[size_t(u) * 3]], &x1 = sg.mb->P[sg.mb->idx[size_t(u) * 3 + 1]], &x2 = sg.mb->P[sg.mb->idx[size_t(u) * 3 + 2]]; const Vec3 mm = (x0 + x1 + x2) * (1.0 / 3); const double d = (mm - q).lengthSq(); if (d < best) { best = d; bn = (x1 - x0).cross(x2 - x0).normalized(); bc = mm; } });
      r += QString(" · item %1 tri n·d %2 ok %3 near %4 n·n %5 ahead %6").arg(id).arg(n.dot(R.d)).arg(sg.ok(q, n)).arg(std::sqrt(best)).arg(bn.dot(n)).arg((bc - q).dot(n)); } }
  return r;
}
}  // namespace hm

namespace hm {
// (checks) the vertices of mesh m close on screen to another one not welded with it — pairs, their screen points
QString Viewport::testNearVerts(int m, double px) {
  const Item* it = scene_->item(m); if (!it) return "no mesh"; Projector P = projector(); const auto W = ttWorld(m); QStringList o; const PolyMesh& B = *(it->base ? it->base : it->poly);
  std::vector<char> used(W.size(), 0); for (int v : B.faceVerts) if (size_t(v) < used.size()) used[size_t(v)] = 1;
  std::vector<ScreenPt> s(W.size()); for (size_t v = 0; v < W.size(); v++) s[v] = P.toScreen(W[v]);
  if (px < 0) { for (size_t v = W.size() - std::min(W.size(), size_t(-px)); v < W.size(); v++) o << QString("%1(%2,%3) used %4 at %5 %6 %7").arg(v).arg(s[v].x, 0, 'f', 0).arg(s[v].y, 0, 'f', 0).arg(int(used[v])).arg(W[v].x).arg(W[v].y).arg(W[v].z); return o.join("; "); }
  for (size_t a = 0; a < W.size(); a++) for (size_t b = a + 1; b < W.size(); b++) if (used[a] && used[b] && s[a].z < 1 && s[b].z < 1 && s[a].x > 0 && s[a].y > 0 && s[a].x < width() && s[a].y < height() && std::hypot(s[a].x - s[b].x, s[a].y - s[b].y) < px)
    o << QString("%1(%2,%3)-%4(%5,%6) 3d %7").arg(a).arg(s[a].x, 0, 'f', 0).arg(s[a].y, 0, 'f', 0).arg(b).arg(s[b].x, 0, 'f', 0).arg(s[b].y, 0, 'f', 0).arg(W[a].distanceTo(W[b]), 0, 'g', 3);
  return o.join("; ");
}
}  // namespace hm

namespace hm {
// (checks) mesh m's corner normals against their mirror across X = 0: of the corners with a normal of the file, how many
// have a corner at the mirrored place with the mirrored normal
QString Viewport::testMirrorNormals(int m) {
  const Item* it = scene_->item(m); if (!it) return "no mesh"; const PolyMesh& B = *(it->base ? it->base : it->poly);
  if (B.cornerNormal.size() != B.faceVerts.size()) return "no corner normals";
  const double q = std::max(1e-9, B.bounds().size().length() * 1e-4); auto key = [&](double x, double y, double z) { return QString("%1,%2,%3").arg(std::llround(x / q)).arg(std::llround(y / q)).arg(std::llround(z / q)); };
  std::multimap<QString, std::array<float, 3>> at; for (size_t k = 0; k < B.faceVerts.size(); k++) { const Vec3& p = B.pos[size_t(B.faceVerts[k])]; at.insert({key(p.x, p.y, p.z), B.cornerNormal[k]}); }
  int all = 0, ok = 0; double worst = 0;
  for (size_t k = 0; k < B.faceVerts.size(); k++) { const Vec3& p = B.pos[size_t(B.faceVerts[k])]; if (std::abs(p.x) < q * 2) continue; all++; const auto& n = B.cornerNormal[k]; double best = 1e9;
    auto rg = at.equal_range(key(-p.x, p.y, p.z)); for (auto i = rg.first; i != rg.second; ++i) { const auto& o = i->second; best = std::min(best, double(std::abs(o[0] + n[0]) + std::abs(o[1] - n[1]) + std::abs(o[2] - n[2]))); }
    if (best < 0.02) ok++; else if (best < 1e8) worst = std::max(worst, best); }
  return QString("%1 of %2 corners mirrored right (worst off %3)").arg(ok).arg(all).arg(worst, 0, 'f', 3);
}
}  // namespace hm

namespace hm {
// (checks) the active mesh cut to its +X half, mirrored back whole (Mirror, Object mode) — its normals against their mirror
QString Viewport::testMirrorHalf() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "no mesh";
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); std::vector<char> del(E.f.size(), 0); for (size_t f = 0; f < E.f.size(); f++) del[f] = E.center(int(f)).x < 0;
  E.removeFaces(del); ttDone(m, E, B, {}, 0, {});
  setTopoMode("object", true); ttools.mirType = "mirror"; ttools.mirAxis = 0; ttools.mirReplace = false;
  const QString r = tMirrorMesh(); return r + " → " + testMirrorNormals(m);
}
}  // namespace hm

namespace hm {
// (checks) Mirror → Replace: the polygons within r of the active mesh's vertex nearest (x, mid height, front) picked, then
// put in place of the other side
QString Viewport::testReplace(double x, double r) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "no mesh";
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); const Box3 bx = B.bounds(); const Vec3 aim(x, (bx.min.y + bx.max.y) / 2, bx.min.z);
  int pv = -1; double bd = 1e300; for (size_t v = 0; v < B.pos.size(); v++) if (const double d = B.pos[v].distanceTo(aim); d < bd) { bd = d; pv = int(v); }
  std::vector<int> fs; int there = 0; const Vec3 pm(-B.pos[size_t(pv)].x, B.pos[size_t(pv)].y, B.pos[size_t(pv)].z); for (size_t f = 0; f < E.f.size(); f++) { if (E.center(int(f)).distanceTo(B.pos[size_t(pv)]) < r) fs.push_back(int(f)); if (E.center(int(f)).distanceTo(pm) < r) there++; }
  setTopoMode("poly", true); tselApply(m, fs, "set", 'f'); ttools.mirType = "mirror"; ttools.mirAxis = 0; ttools.mirReplace = true;
  return QString("%1 picked (%5 there) round %2 %3 %4 → ").arg(fs.size()).arg(B.pos[size_t(pv)].x).arg(B.pos[size_t(pv)].y).arg(B.pos[size_t(pv)].z).arg(there) + tMirrorMesh() + " · " + ui::lastToast() + " · " + lastReplaceInfo_;
}
}  // namespace hm

namespace hm {
// (checks) the active mesh's polygons all moved by d (world) as a drag would, Snap on: how many vertices end up in one
// place with another that were apart before
QString Viewport::testMoveSel(const Vec3& d) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "no mesh";
  const PolyMesh& B = *baseOf(*it); const std::vector<Vec3> base = B.pos; std::vector<int> vs; { std::set<int> s(B.faceVerts.begin(), B.faceVerts.end()); vs.assign(s.begin(), s.end()); }
  const Affine Mi = meshmods::meshMatrix(*scene_, *it).inverse(); std::vector<Vec3> P = base; for (int v : vs) P[size_t(v)] = base[size_t(v)] + Mi.lin(d);
  const bool snap0 = tset.snap; tset.snap = true; topoAfterMove(m, P, vs, base, nullptr); tset.snap = snap0;
  const double q = B.bounds().size().length() * 1e-4; int stuck = 0; QString smp0;
  std::map<std::tuple<long long, long long, long long>, std::vector<int>> g; for (int v : vs) g[{std::llround(P[size_t(v)].x / q), std::llround(P[size_t(v)].y / q), std::llround(P[size_t(v)].z / q)}].push_back(v);
  for (auto& [k, l] : g) for (size_t a = 0; a < l.size(); a++) for (size_t b = a + 1; b < l.size(); b++) if (base[size_t(l[a])].distanceTo(base[size_t(l[b])]) > q * 3) { if (stuck < 4) smp0 += QString(" · stuck v%1 (%2,%3,%4) v%5 (%6,%7,%8) at %9,%10,%11").arg(l[a]).arg(base[size_t(l[a])].x, 0, 'f', 2).arg(base[size_t(l[a])].y, 0, 'f', 2).arg(base[size_t(l[a])].z, 0, 'f', 2).arg(l[b]).arg(base[size_t(l[b])].x, 0, 'f', 2).arg(base[size_t(l[b])].y, 0, 'f', 2).arg(base[size_t(l[b])].z, 0, 'f', 2).arg(P[size_t(l[a])].x, 0, 'f', 2).arg(P[size_t(l[a])].y, 0, 'f', 2).arg(P[size_t(l[a])].z, 0, 'f', 2); stuck++; }
  QString smp; for (size_t i = 0; i < vs.size(); i += vs.size() / 6 + 1) { const int v = vs[i]; smp += QString(" · v%1 %2,%3,%4 → %5,%6,%7").arg(v).arg(base[size_t(v)].x, 0, 'f', 2).arg(base[size_t(v)].y, 0, 'f', 2).arg(base[size_t(v)].z, 0, 'f', 2).arg(P[size_t(v)].x, 0, 'f', 2).arg(P[size_t(v)].y, 0, 'f', 2).arg(P[size_t(v)].z, 0, 'f', 2); }
  int jumped = 0; QString js; for (int v : vs) if (P[size_t(v)].distanceTo(base[size_t(v)] + Mi.lin(d)) > d.length() * 10 + q) { if (jumped < 4) js += QString(" · v%1 %2,%3,%4 → %5,%6,%7").arg(v).arg(base[size_t(v)].x, 0, 'f', 2).arg(base[size_t(v)].y, 0, 'f', 2).arg(base[size_t(v)].z, 0, 'f', 2).arg(P[size_t(v)].x, 0, 'f', 2).arg(P[size_t(v)].y, 0, 'f', 2).arg(P[size_t(v)].z, 0, 'f', 2); jumped++; }
  return QString("%1 vertices moved, %2 pairs now in one place (apart before), %3 jumped far").arg(vs.size()).arg(stuck).arg(jumped) + js;
}
}  // namespace hm

namespace hm {
// a moved vertex onto the surface: where the view sees it — unless the surface right by the vertex is much nearer than
// that point (a thin garment seen at a slant along its side: the view's ray slips past the near layer onto the one inside)
Vec3 Viewport::ttSnapMoved(const Vec3& w, int m) const {
  const Vec3 h = ttSnapW(w, m); const double dh = h.distanceTo(w); if (dh <= 0) return h;
  double bd = 1e300; Vec3 best = h;
  for (auto& [id, t] : scene_->items) if (id != m && t.type == ItemType::IMesh && t.poly && scene_->isVisible(t)) if (auto b = bvhOf(id, true)) { double dd; if (auto q = b->closest(w, nullptr, &dd); q && dd < bd) { bd = dd; best = *q; } }
  return bd < 1e300 && dh > 2 * bd + 1e-9 ? best : h;
}
}  // namespace hm

namespace hm {
void Viewport::modifiersChanged() {
  if (room() == "hair") { groupHover(mouse_, QGuiApplication::queryKeyboardModifiers()); return; }   // (Ctrl+Shift: the group under the pointer lit)
  if (room() != "topo" || drag_ != Drag::None || (tool_ != "tpen" && tool_ != "tloop")) return;
  const QPoint p = mapFromGlobal(QCursor::pos()); if (p.x() < 0 || p.y() < 0 || p.x() >= width() || p.y() >= height()) return;
  topoToolMove(QPointF(p), QGuiApplication::queryKeyboardModifiers());
}

// PolyPen: a vertex let go near another but not surely onto it (its own polygon's vertex, one behind the surface, past
// the radius narrowed by its short sides) — asked beside the pointer: Weld, else nothing (ignored — the next click, or a
// few seconds: not welded)
void Viewport::penWeldAsk(const QPointF& at) {
  penAskClose(); if (penAsk_.pairs.empty()) return;
  auto* f = new QFrame(QApplication::activeWindow(), Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint); f->setAttribute(Qt::WA_DeleteOnClose); f->setObjectName("penAsk");
  f->setStyleSheet("#penAsk{background:#1e1e1e;border:1px solid #444}");   // (the look of the app's own questions — the anchors' Join)
  auto* h = new QHBoxLayout(f); h->setContentsMargins(12, 9, 12, 9); h->setSpacing(8);
  const int k = int(penAsk_.pairs.size());
  h->addWidget(new QLabel(k == 1 ? tr("Weld this vertex here?") : tr("Weld these %1 vertices here?").arg(k)));
  auto* yes = new QPushButton(tr("Weld")); auto* no = new QPushButton(tr("Keep apart")); no->setToolTip(tr("Leave them apart (ignoring this does the same)"));
  h->addWidget(yes); h->addWidget(no);
  connect(yes, &QPushButton::clicked, this, [this] { penAskWeld(); });
  connect(no, &QPushButton::clicked, this, [this] { penAskClose(); });
  f->adjustSize(); f->move(mapToGlobal(at.toPoint() + QPoint(18, 14))); f->show(); f->raise();
  penAsk_.w = f; QPointer<QFrame> g(f);
  QTimer::singleShot(4000, this, [this, g] { if (g && penAsk_.w == g) penAskClose(); });
}
void Viewport::penAskClose() { if (penAsk_.w) { QWidget* w = penAsk_.w; penAsk_.w = nullptr; w->close(); } }
void Viewport::penAskWeld() {
  const int m = penAsk_.m; const auto pairs = penAsk_.pairs; penAskClose(); penAsk_.pairs.clear();
  Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || room() != "topo") return;
  const auto W = ttWorld(m); auto at = [&](const Vec3& p) { int b = -1; double bd = 1e-6; for (size_t i = 0; i < W.size(); i++) { const double d = W[i].distanceTo(p); if (d < bd) { bd = d; b = int(i); } } return b; };
  std::vector<int> to(W.size(), -1); int n = 0;
  for (auto& [s, t] : pairs) { const int a = at(s), b = at(t); if (a >= 0 && b >= 0 && a != b) { to[size_t(a)] = b; n++; } }
  if (!n) { ui::toast(tr("Nothing to weld — the vertices moved meanwhile")); return; }
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); to.resize(E.pos.size(), -1); E.weld(to);
  ttDone(m, E, B, n == 1 ? tr("Welded") : tr("Welded: %1 vertices").arg(n));
}
}  // namespace hm
