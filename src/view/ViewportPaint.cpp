// The Paint room in the viewport: the map being painted shown on the meshes (Scene::paintView — the texture canvas's picture, on the
// polygons of the UV set painted in) and the brush on the model itself — the surface under the pointer, its place in that set's UV,
// handed to the canvas's brush as a point of the texture.
#include "view/Viewport.h"
#include "paint/Brush.h"
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "ui/Keymap.h"
#include "scene/Topo.h"
#include "scene/Uv.h"
#include "view/Picking.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QTabletEvent>
#include <algorithm>
#include <QPolygonF>
#include <QGuiApplication>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <map>
#include <thread>

namespace hm {

namespace {
// a mesh's fan triangles → its polygons (as pickSurface numbers them), made once per geometry
const std::vector<int>& triFaces(const std::shared_ptr<PolyMesh>& sp) {
  static std::map<const PolyMesh*, std::pair<std::weak_ptr<PolyMesh>, std::vector<int>>> cache;
  for (auto c = cache.begin(); c != cache.end();) { if (c->second.first.expired()) c = cache.erase(c); else ++c; }
  auto f = cache.find(sp.get()); if (f != cache.end() && f->second.first.lock() == sp) return f->second.second;
  std::vector<int> t; const PolyMesh& p = *sp;
  for (int fi = 0; fi < p.faceCount(); fi++) for (int k = p.faceStart[size_t(fi)] + 1; k + 1 < p.faceStart[size_t(fi) + 1]; k++) t.push_back(fi);
  return (cache[sp.get()] = {sp, std::move(t)}).second;
}
// the point's weights in a triangle (clamped onto it), and how far off its plane / sides it lies
double baryIn(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, double w[3]) {
  const Vec3 v0 = b - a, v1 = c - a, v2 = p - a; const double d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1), den = d00 * d11 - d01 * d01;
  if (std::abs(den) < 1e-30) { w[0] = 1; w[1] = w[2] = 0; return 1e300; }
  double v = (d11 * d20 - d01 * d21) / den, u2 = (d00 * d21 - d01 * d20) / den, u = 1 - v - u2;
  const double out = std::max({-u, -v, -u2, 0.0});
  u = std::max(0.0, u); v = std::max(0.0, v); u2 = std::max(0.0, u2); const double s = u + v + u2; w[0] = u / s; w[1] = v / s; w[2] = u2 / s;
  const Vec3 q = a * w[0] + b * w[1] + c * w[2];
  return q.distanceTo(p) + out * std::sqrt(std::max(d00, d11));
}
}  // namespace

// the texture point (0…1 across, 0…1 down — as the picture's rows go) of the surface under a window point: of a mesh's polygon that
// is in the UV set painted in. nullopt — nothing there, a mesh with no UV, a polygon of another set
std::optional<QPointF> Viewport::paintUvAt(const QPointF& m) const { const auto hit = pickSurface(m); if (!hit) return std::nullopt; return paintUvOfHit(*hit); }
// the UV set of the polygon under a window point — the one painted now if the polygon is in it, else the first it is in; empty —
// nothing there, a mesh with no UV sets, a polygon the brush leaves alone
QString Viewport::paintSetUnder(const QPointF& m) const {
  const auto h = pickSurface(m); if (!h) return {}; const Item* it = scene_->item(h->itemId); if (!it || it->type != ItemType::IMesh || !it->poly) return {};
  const PolyMesh& P = *it->poly; if (P.uvSetNames.empty() || P.cornerUV.size() != P.faceVerts.size() || P.cornerUV.empty()) return {};
  const int f = pvPolyFace(*h, *it); if (f < 0 || size_t(f) >= P.faceSets.size() || pvFaceBlocked(*it, f)) return {};
  const auto bits = P.faceSets[size_t(f)]; const QString& cur = scene_->paintView.set;
  for (size_t k = 0; k < P.uvSetNames.size(); k++) if ((bits >> k & 1) && P.uvSetNames[k] == cur) return cur;
  for (size_t k = 0; k < P.uvSetNames.size(); k++) if (bits >> k & 1) return P.uvSetNames[k];
  return {};
}
std::optional<QPointF> Viewport::paintUvOfHit(const RayHit& hitIn, double* texelsPerUnit) const {
  const RayHit* hit = &hitIn;
  const Item* it = scene_->item(hit->itemId); if (!it || it->type != ItemType::IMesh || !it->poly) return std::nullopt;
  const PolyMesh& P = *it->poly; if (P.cornerUV.size() != P.faceVerts.size() || P.cornerUV.empty()) return std::nullopt;
  const QString& set = scene_->paintView.set; int k = -1;
  if (!set.isEmpty()) { for (size_t i = 0; i < P.uvSetNames.size(); i++) if (P.uvSetNames[i] == set) k = int(i); if (k < 0) return std::nullopt; }
  const auto& t2f = triFaces(it->poly);
  // (the polygon: the hit's triangle if it is numbered as the fans are — checked by the point lying on it — else the nearest one looked for)
  auto tryFace = [&](int f, double& bestD, int& bf, int& bc, double bw[3]) {
    const int s = P.faceStart[size_t(f)], e = P.faceStart[size_t(f) + 1];
    for (int c = s + 1; c + 1 < e; c++) { double w[3]; const double d = baryIn(hit->point, P.pos[size_t(P.faceVerts[size_t(s)])], P.pos[size_t(P.faceVerts[size_t(c)])], P.pos[size_t(P.faceVerts[size_t(c + 1)])], w);
      if (d < bestD) { bestD = d; bf = f; bc = c; bw[0] = w[0]; bw[1] = w[1]; bw[2] = w[2]; } } };
  double bd = 1e300, bw[3] = {1, 0, 0}; int bf = -1, bc = -1;
  const double tol = P.bounds().size().length() * 1e-5;
  if (hit->tri >= 0 && size_t(hit->tri) < t2f.size()) tryFace(t2f[size_t(hit->tri)], bd, bf, bc, bw);
  if (bd > tol) for (int f = 0; f < P.faceCount(); f++) tryFace(f, bd, bf, bc, bw);
  if (bf < 0 || bd > tol * 100) return std::nullopt;
  if (pvFaceBlocked(*it, bf)) return std::nullopt;               // (locked, or not among the selected objects: the brush leaves it alone)
  if (k >= 0 && !(size_t(bf) < P.faceSets.size() && (P.faceSets[size_t(bf)] >> k & 1))) return std::nullopt;   // (a polygon of another set)
  // (the set's own UV where the polygon shows another set's now)
  const bool own = k >= 0 && size_t(k) < P.setUV.size() && P.setUV[size_t(k)].size() == P.faceVerts.size() && !(size_t(bf) < P.faceLive.size() && P.faceLive[size_t(bf)] == k);
  const auto& UV = own ? P.setUV[size_t(k)] : P.cornerUV; const int s = P.faceStart[size_t(bf)];
  const auto &a = UV[size_t(s)], &b = UV[size_t(bc)], &c = UV[size_t(bc + 1)];
  const double u = a[0] * bw[0] + b[0] * bw[1] + c[0] * bw[2], v = a[1] * bw[0] + b[1] * bw[1] + c[1] * bw[2];
  if (texelsPerUnit) { const Vec3 &pa = P.pos[size_t(P.faceVerts[size_t(s)])], &pb = P.pos[size_t(P.faceVerts[size_t(bc)])], &pc = P.pos[size_t(P.faceVerts[size_t(bc + 1)])];
    const double wa = (pb - pa).cross(pc - pa).length() / 2, ua = std::abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) / 2 * paintTexSize.width() * paintTexSize.height();
    *texelsPerUnit = wa > 1e-20 ? std::sqrt(ua / wa) : 0; }
  return QPointF(u - std::floor(u), P.flipY ? 1 - (v - std::floor(v)) : v - std::floor(v));
}

// the Paint room's brush on the model: the left button paints where the pointer is over a mesh of the set (Alt / the other buttons
// still move the camera) — true: the press was taken
bool Viewport::paintPress(QMouseEvent* e) {
  if (room() != "paint") return false;
  const bool psCtl = ui::Keymap::get().nav().paint == "ps";      // (the Photoshop / Substance controls: Ctrl + RMB sizes, Alt + click takes the colour)
  if (e->modifiers() & Qt::AltModifier) {                         // (Alt: the camera - a click that does not move it takes the colour there)
    altPickArm_ = psCtl && e->button() == Qt::LeftButton && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)); altPickAt_ = e->position(); return false; }
  // (R held + drag: the view rolls round its own axis — the pointer turned round the window's middle, as the texture window's R does)
  if (keyRoll && e->button() == Qt::LeftButton && cam_.view != "uv") { rollDrag_ = true; rollAt_ = e->position(); rollRaw_ = rollDone_ = 0; return true; }
  // (RMB drag with a painting tool, no modifiers: the brush's size ←→ and opacity ↑↓ — as in 3D-Coat; the ring stays where it began.
  // The room's pie is H held, not the RMB)
  if (e->button() == Qt::RightButton && paintRing && paintAdjust && (psCtl ? (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) == Qt::ControlModifier : !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)))) {
    paintAdj_ = true; paintAdjHard_ = psCtl; paintAdjAt_ = paintLast_ = e->position(); paintAdjust(0, {}); return true; }
  if (e->button() != Qt::LeftButton) return false;
  paintLast_ = e->position();
  // (a polygon of another UV set under the pointer: that set is shown and painted — the brush goes wherever it is put down)
  if (paintSetSwitch && !scene_->paintView.set.isEmpty()) { const QString st = paintSetUnder(e->position()); if (!st.isEmpty() && st != scene_->paintView.set) paintSetSwitch(st, false); }
  paintInv_ = (e->modifiers() & Qt::ControlModifier) || paintEraserTip();
  if ((e->modifiers() & Qt::ControlModifier) && (e->modifiers() & Qt::ShiftModifier) && paintPickLayer) {   // (Ctrl + Shift + click: the layer painted here taken as the active one)
    if (const auto uv = paintUvAt(e->position())) paintPickLayer(*uv); else ui::toast(tr("Ctrl + Shift + click picks the layer painted under the pointer — click on the model")); return true; }
  if (paintToolInput && paintToolInput(0, e->position(), e->modifiers())) { paintToolDrag_ = true; return true; }   // (a tool other than the brush took it)
  if (!paintOnModel || !paintInput) return false;
  // (LMB on empty space with a painting tool: the camera turns — the 3D-Coat way, the author's most used navigation)
  // (LMB on empty space turns the camera — but a press a few pixels off a thin lock is a stroke that missed its start: it paints.
  // The author's recording: 98 presses beside a lock jerked the camera instead of laying a stroke)
  auto nearModel = [&](const QPointF& p) { if (pickSurface(p)) return true; const double rr = std::clamp(paintRingRadiusPx(), 5.0, 10.0);
    for (double r : {rr * 0.5, rr}) for (int k = 0; k < 8; k++) { const double a = k * 0.785398163; if (pickSurface(p + QPointF(std::cos(a) * r, std::sin(a) * r))) return true; } return false; };
  if (paintRing && !psCtl && !nearModel(e->position())) { drag_ = Drag::Orbit; cam_.trackball = ui::Keymap::get().nav().rotation == "trackball"; return true; }
  if (const auto ph = pickSurface(e->position())) cam_.pivot = ph->point;   // (the camera turns round where the brush went down last — as 3D-Coat's, whose pivot follows the pen)
  const bool inv = (e->modifiers() & Qt::ControlModifier) || paintEraserTip();   // (Ctrl, or the pen turned over: it erases)
  steady_ = e->position(); steadyOn_ = paintSteadyPx && paintSteadyPx() > 0.005;   // (Steady Stroke: the brush starts where the pointer is)
  if (paintProj && paintProj(0, e->position(), inv)) { paintProjDrag_ = true; return true; }   // (laid on the screen, carried into the texture)
  const auto uv = paintUvAt(e->position());
  if (!uv) {                                                    // (off the model: the click selects / the lasso, as ever — a mesh that can't be painted says why)
    if (const auto h = pickSurface(e->position())) if (const Item* it = scene_->item(h->itemId); it && it->type == ItemType::IMesh && it->poly) {
      if (pvFaceBlocked(*it, pvPolyFace(*h, *it))) ui::toast(pvLocked(it->id) || !it->raw.value("pLockF").toString().isEmpty() ? tr("%1 is locked here — the brush leaves it alone (the lock in the Paint room's lists)").arg(it->name) : tr("%1 is not among the selected objects — only those are painted (Deselect all: every object)").arg(it->name));
      else if (it->poly->cornerUV.size() != it->poly->faceVerts.size() || it->poly->cornerUV.empty()) ui::toast(tr("%1 has no UVs — it can't be painted. Unfold it in the Topology room (UV Editor), or import it with UVs on").arg(it->name));
      else if (!scene_->paintView.set.isEmpty()) ui::toast(tr("This polygon is not in the UV set %1 — pick its set over the texture, in the left column").arg(scene_->paintView.set)); }
    return true; }                                               // (the left button is the brush's here: the objects are selected in the room's list)
  paintDrag_ = true; paintTex_ = *uv; paintScaleUpdate(e->position(), *uv); if (paintStrokeEnd) paintStrokeEnd(); paintMirrorAt(e->position()); paintInput(0, *uv, inv); return true;
}

// the pen over the viewport: its pressure and tip noted for the brush (paintPressureNow), the event left unaccepted — Qt then makes the
// mouse event of it, and everything the mouse does (the camera, the tools, the brush) goes on as before
void Viewport::tabletEvent(QTabletEvent* e) {
  penT_ = QDateTime::currentMSecsSinceEpoch(); penEraser_ = e->pointerType() == QPointingDevice::PointerType::Eraser;
  if (e->type() == QEvent::TabletPress) penDown_ = true; else if (e->type() == QEvent::TabletRelease) penDown_ = false;
  if (e->type() != QEvent::TabletRelease) penP_ = std::clamp(float(e->pressure()), 0.f, 1.f);
  paint::g_penTilt = e->type() == QEvent::TabletRelease ? 1.f : std::clamp(float(std::hypot(e->xTilt(), e->yTilt()) / 60.0), 0.f, 1.f);   // (the pen's tilt, for the brush's tilt-driven settings)
  e->ignore();
}
namespace { constexpr qint64 kPenFresh = 150; }   // (ms: a pen event older than this — the mouse is the one moving now)
bool Viewport::paintPenNow() const { return penDown_ && QDateTime::currentMSecsSinceEpoch() - penT_ < kPenFresh; }
bool Viewport::paintEraserTip() const { return penEraser_ && QDateTime::currentMSecsSinceEpoch() - penT_ < kPenFresh; }
float Viewport::paintPressureNow() const { return !penLine_ && paintPenNow() ? penP_ : 1.f; }
// the stroke on the model goes on to a screen point (after Steady Stroke): the projection takes the screen point, the older way the
// texture point under it
void Viewport::paintStrokeTo(const QPointF& m) {
  if (paintProjDrag_) { if (paintProj) paintProj(1, m, false); return; }
  if (const auto uv = paintUvAt(m)) { paintTex_ = *uv; paintScaleUpdate(m, *uv); paintMirrorAt(m); paintInput(1, *uv, false); } else { if (paintStrokeEnd) paintStrokeEnd(); paintInput(3, {}, false); }   // (off the model / onto another set: the stroke lifts, and goes on when it comes back)
}
void Viewport::paintRemoteAt(std::optional<QPointF> uv, double r) {
  std::optional<QPointF> at; if (uv) if (const auto h = paintHitOfUv(*uv)) { const ScreenPt s = projector().toScreen(h->point); if (s.z < 1) at = QPointF(s.x, s.y); }
  if (at == paintRemote_ && std::abs(r - paintRemoteR_) < 0.5) return; paintRemote_ = at; paintRemoteR_ = r; overlayRev_++; requestUpdate();
}
bool Viewport::paintLocateAt(const QPointF& uv) {
  const auto h = paintHitOfUv(uv); if (!h) return false; cam_.pivot = h->point; cam_.lookAt(h->point); overlayRev_++; requestUpdate(); return true;
}
bool Viewport::paintMove(const QPointF& m) {
  if (rollDrag_) {                                              // (Shift: in 15° steps)
    const QPointF c(width() / 2.0, height() / 2.0); double d = (std::atan2(m.y() - c.y(), m.x() - c.x()) - std::atan2(rollAt_.y() - c.y(), rollAt_.x() - c.x())) * 180 / 3.14159265358979;
    while (d > 180) d -= 360; while (d < -180) d += 360; rollAt_ = m; rollRaw_ += d;
    const double to = (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) ? std::round(rollRaw_ / 15) * 15 : rollRaw_;
    if (std::abs(to - rollDone_) > 1e-6) { rollView(to - rollDone_); rollDone_ = to; }
    return true; }
  if (paintHoverUv && room() == "paint" && !paintAdj_) paintHoverUv(paintUvAt(m));   // (the brush's place on the model, shown on the texture)
  if (paintAdj_) { if (paintAdjust) paintAdjust(1, m - paintAdjAt_); overlayRev_++; requestUpdate(); return true; }   // (the ring stays where the drag began)
  paintLast_ = m; if (paintRing && room() == "paint") { overlayRev_++; requestUpdate(); }   // (the brush's ring follows the pointer)
  if (paintToolDrag_) { if (paintToolInput) paintToolInput(1, m, QGuiApplication::keyboardModifiers()); return true; }
  if (!paintDrag_ && !paintProjDrag_) { if (room() == "paint" && paintToolInput && drag_ == Drag::None) paintToolInput(4, m, QGuiApplication::keyboardModifiers()); return false; }
  // (a stroke laid in the texture itself — Blur, Smudge, the bump, a mask — crossed onto a polygon of another UV set: it ends in the
  // one and goes on in the other. The projected brush needs none of it: it writes every set it lies over)
  if (paintDrag_ && paintSetSwitch && !scene_->paintView.set.isEmpty()) { const QString st = paintSetUnder(m);
    if (!st.isEmpty() && st != scene_->paintView.set && paintSetSwitch(st, true)) {
      paintDrag_ = false; paintInput(2, {}, false); if (paintStrokeEnd) paintStrokeEnd();
      paintSetSwitch(st, false); steady_ = m;
      if (const auto uv = paintUvAt(m)) { paintDrag_ = true; paintTex_ = *uv; paintScaleUpdate(m, *uv); paintMirrorAt(m); paintInput(0, *uv, paintInv_); }
      return true; } }
  QPointF at = m;
  if (steadyOn_) {                                              // (Steady Stroke: the brush moves only when the pointer pulls its string tight)
    const double R = (paintSteadyPx ? paintSteadyPx() : 0) * 4 * paintRingRadiusPx(); const QPointF d = m - steady_; const double dist = std::hypot(d.x(), d.y());
    if (dist <= R) return true;
    steady_ += d * ((dist - R) / dist); at = steady_; }
  paintStrokeTo(at);
  return true;
}
bool Viewport::paintRelease() {
  if (rollDrag_) { rollDrag_ = false; return true; }
  if (paintAdj_) { paintAdj_ = false; if (paintAdjust) paintAdjust(2, {}); paintLast_ = paintAdjAt_; return true; }
  if (paintToolDrag_) { paintToolDrag_ = false; if (paintToolInput) paintToolInput(2, paintLast_, QGuiApplication::keyboardModifiers()); return true; }
  if ((paintDrag_ || paintProjDrag_) && steadyOn_) {            // (Steady Stroke: the brush catches up with where the pointer was let go)
    const QPointF d = paintLast_ - steady_; const int n = std::max(1, int(std::hypot(d.x(), d.y()) / 4));
    for (int i = 1; i <= n; i++) paintStrokeTo(steady_ + d * (double(i) / n)); steady_ = paintLast_; }
  steadyOn_ = false;
  if (paintProjDrag_) { paintProjDrag_ = false; if (paintProj) paintProj(2, paintLast_, false); return true; }
  if (!paintDrag_) return false; paintDrag_ = false; paintInput(2, {}, false); if (paintStrokeEnd) paintStrokeEnd(); return true; }

}  // namespace hm

// ======================= the room's own visibility and locks =======================
// Kept with the items and seen by the Paint room only: pHid — the object's eye, pHidF — its hidden polygons, pLock — the brush leaves
// the object alone, pLockF — its locked polygons; noPaint — the NoPainted label: the object is not in the room at all. A polygroup's
// and a visibility set's eye and lock work on their polygons. H and its kin hide the part under the pointer. None of it is an undo step.
namespace hm {

namespace {
std::shared_ptr<PolyMesh> bsp(const Item& m) { return m.base ? m.base : m.poly; }
bool anyOf(const std::vector<char>& a) { for (char c : a) if (c) return true; return false; }
bool allOf(const std::vector<char>& a) { if (a.empty()) return false; for (char c : a) if (!c) return false; return true; }
// the parts of a mesh — polygons joined through shared vertices: a part's number per polygon (made once per geometry)
const std::vector<int>& partsOf(const std::shared_ptr<PolyMesh>& sp) {
  static std::map<const PolyMesh*, std::pair<std::weak_ptr<PolyMesh>, std::vector<int>>> cache;
  for (auto c = cache.begin(); c != cache.end();) { if (c->second.first.expired()) c = cache.erase(c); else ++c; }
  auto f = cache.find(sp.get()); if (f != cache.end() && f->second.first.lock() == sp && int(f->second.second.size()) == sp->faceCount()) return f->second.second;
  const PolyMesh& p = *sp; std::vector<int> par(p.pos.size()); for (size_t i = 0; i < par.size(); i++) par[i] = int(i);
  auto find = [&](int x) { while (par[size_t(x)] != x) { par[size_t(x)] = par[size_t(par[size_t(x)])]; x = par[size_t(x)]; } return x; };
  for (int fi = 0; fi < p.faceCount(); fi++) { const int s = p.faceStart[size_t(fi)], e = p.faceStart[size_t(fi) + 1]; if (e <= s) continue; const int a = find(p.faceVerts[size_t(s)]);
    for (int k = s + 1; k < e; k++) { const int b = find(p.faceVerts[size_t(k)]); if (b != a) par[size_t(b)] = a; } }
  std::vector<int> part(size_t(p.faceCount()), -1);
  for (int fi = 0; fi < p.faceCount(); fi++) { const int s = p.faceStart[size_t(fi)]; if (p.faceStart[size_t(fi) + 1] > s) part[size_t(fi)] = find(p.faceVerts[size_t(s)]); }
  return (cache[sp.get()] = {sp, std::move(part)}).second;
}
QJsonArray pvSetsOf(const Scene& s) { return s.fileRaw.value("visSets").toArray(); }
int pvFindSet(const QJsonArray& a, const QString& name) { for (int i = 0; i < a.size(); i++) if (a[i].toObject().value("name").toString() == name) return i; return -1; }
// a shown polygon of the mesh → its base's polygon (empty: the same numbers; −1: not known)
std::vector<int> polyToBase(const Item& it) {
  if (!it.base || !it.poly || it.poly == it.base) return {};
  const int nb = it.base->faceCount(), np = it.poly->faceCount();
  const std::vector<char> h = meshmods::hiddenFaces(it, *it.base); std::vector<int> list;
  for (int f = 0; f < nb; f++) if (h.empty() || !h[size_t(f)]) list.push_back(f);
  if (int(list.size()) == nb && np == nb) return {};
  std::vector<int> m(size_t(np), -1); if (list.empty() || np % int(list.size())) return m;   // (a modifier cut some away: not known)
  for (int i = 0; i < np; i++) m[size_t(i)] = list[size_t(i) % list.size()];                  // (a symmetry's copies after the mesh's own, in the same order)
  return m;
}
}  // namespace

// Symmetry in the Paint room: where the stroke's point lies on the mirrored side — the hit carried by the symmetry's first mirror (the
// mesh's own space), the surface looked for there along the mirrored normal; its place in the UV set. nullopt — no symmetry, nothing there
std::vector<QPointF> Viewport::paintMirrorsUv(const RayHit& hit) const {
  std::vector<QPointF> out;
  const Item* it = scene_->item(hit.itemId); if (!it || !it->poly || it->type != ItemType::IMesh) return out;
  const std::shared_ptr<PolyMesh> B = it->base ? it->base : it->poly;
  struct C { const PolyMesh* base = nullptr; QByteArray key; std::vector<Affine> T; }; static std::map<int, C> cache;
  C& c = cache[it->id]; const QByteArray key = QJsonDocument(symo.toJson()).toJson(QJsonDocument::Compact);
  if (c.base != B.get() || c.key != key) { c.base = B.get(); c.key = key; c.T = symSet(it->id, B->pos).T; }
  if (c.T.empty()) return out;
  const bool moved = it->poly != B; const Affine X = moved ? meshmods::meshMatrix(*scene_, *it) : Affine(), Xi = X.inverse();
  const double size = std::max(1e-9, it->poly->bounds().size().length()), eps = size * 2e-3;
  // (its triangles, as pickSurface numbers them — made once per geometry)
  static std::map<const PolyMesh*, std::pair<std::weak_ptr<PolyMesh>, std::vector<uint32_t>>> tc;
  for (auto q = tc.begin(); q != tc.end();) { if (q->second.first.expired()) q = tc.erase(q); else ++q; }
  auto f = tc.find(it->poly.get());
  if (f == tc.end() || f->second.first.lock() != it->poly) { std::vector<uint32_t> t; const PolyMesh& p = *it->poly;
    for (int fi = 0; fi < p.faceCount(); fi++) { const int s = p.faceStart[size_t(fi)], e = p.faceStart[size_t(fi) + 1]; for (int k = s + 1; k < e - 1; k++) { t.push_back(uint32_t(p.faceVerts[size_t(s)])); t.push_back(uint32_t(p.faceVerts[size_t(k)])); t.push_back(uint32_t(p.faceVerts[size_t(k + 1)])); } }
    tc[it->poly.get()] = {it->poly, std::move(t)}; f = tc.find(it->poly.get()); }
  std::vector<PickMesh> list(1); list[0].itemId = it->id; list[0].pos = &it->poly->pos; list[0].trisRef = &f->second.second; list[0].box = it->poly->bounds();
  for (const Affine& T : c.T) {                                  // (each mirror / turn of the symmetry: the point carried by it, the surface looked for there)
    auto carry = [&](const Vec3& p) { return X.apply(T.apply(Xi.apply(p))); };
    const Vec3 pm = carry(hit.point); Vec3 nm = carry(hit.point + hit.normal * eps) - pm; if (nm.lengthSq() < 1e-30) continue; nm = nm.normalized();
    std::optional<RayHit> best;
    for (int side = 0; side < 2; side++) { const Vec3 d = side ? nm : nm * -1.0; const auto h = raycast(Ray{pm - d * (eps * 6), d}, list);
      if (h && h->t < eps * 14 && (!best || std::abs(h->t - eps * 6) < std::abs(best->t - eps * 6))) best = h; }
    if (!best || best->point.distanceTo(hit.point) < eps * 0.05) continue;   // (nothing there; on the plane itself: the stroke is its own mirror)
    if (const auto uv = paintUvOfHit(*best)) out.push_back(*uv);
  }
  return out;
}
void Viewport::paintMirrorAt(const QPointF& m) {
  if (!paintMirror) return;
  std::vector<QPointF> uv; if (tset.sym) if (const auto h = pickSurface(m)) uv = paintMirrorsUv(*h);
  paintMirror(uv);
}
void Viewport::setPaintDisp(const QString& k0) { const QString k = k0 == "wire" || k0 == "material" ? QStringLiteral("std") : k0;   // (those two displays are gone: an old setting comes back as Standard)
  if (k == paintDisp) return; paintDisp = k; showWire = k == "wire"; staticRev_++; sceneChanged(false); requestUpdate(); emit paintVisChanged(); }

int Viewport::meshUnder(const QPointF& m) const { const auto h = pickSurface(m); const Item* it = h ? scene_->item(h->itemId) : nullptr; return it && pvIsMesh(*it) ? it->id : -1; }
void Viewport::rollView(double deg) {
  if (cam_.view == "uv") return;
  if (deg == 0) { cam_.freeRoll = false; cam_.level(); }
  else { cam_.freeRoll = true; const Quat q = Quat::fromAxisAngle(cam_.dir(), -deg * 3.14159265358979 / 180); cam_.quat = (q * cam_.quat).normalized(); cam_.up = q.rotate(cam_.up); }
  overlayRev_++; requestUpdate(); emit viewChanged(cam_.view);
}
bool Viewport::pvIsMesh(const Item& it) const { return (it.type == ItemType::IMesh || it.type == ItemType::Mesh) && (it.poly || it.base) && !scene_->noPaint(it); }
std::vector<int> Viewport::pvMeshes() const {
  std::vector<int> out; std::function<void(int)> walk = [&](int id) { const Item* it = scene_->item(id); if (!it) return; if (pvIsMesh(*it)) out.push_back(id); for (int c : it->children) walk(c); };
  for (int r : scene_->roots) walk(r);
  return out;
}
void Viewport::pvTouch() { pvRev_++; staticRev_++; overlayRev_++; changes_++; emit changed(); emit paintVisChanged(); requestUpdate(); }

void Viewport::pvSetNoPaint(const std::vector<int>& ids, bool on) {
  if (ids.empty()) return;
  runEdit([&](Scene& s, Selection&, const CurveCache&) { for (int id : ids) if (Item* it = s.item(id)) { if (on) it->raw.insert("noPaint", true); else it->raw.remove("noPaint"); } s.revision++; return QString(); }, false);
  pickCache_.clear(); pvTouch(); emit sceneEdited();
  ui::toast(on ? tr("NoPainted: %1 object%2 left out of the Paint room").arg(ids.size()).arg(ids.size() == 1 ? "" : "s") : tr("NoPainted taken off %1 object%2").arg(ids.size()).arg(ids.size() == 1 ? "" : "s"));
}

std::vector<char> Viewport::pvVec(const Item& it, const char* all, const char* faces) const {
  const auto B = bsp(it); const int n = B ? B->faceCount() : 0; std::vector<char> v(size_t(n), 0);
  if (it.raw.value(QLatin1String(all)).toBool()) { std::fill(v.begin(), v.end(), char(1)); return v; }
  const QString s = it.raw.value(QLatin1String(faces)).toString();
  if (!s.isEmpty()) { std::vector<char> d = topo::smDec(s, n); if (int(d.size()) == n) v = std::move(d); }
  return v;
}
void Viewport::pvWrite(Item& it, const char* all, const char* faces, const std::vector<char>& v) {
  if (allOf(v)) { it.raw.insert(QLatin1String(all), true); it.raw.remove(QLatin1String(faces)); }
  else { it.raw.remove(QLatin1String(all)); if (anyOf(v)) it.raw.insert(QLatin1String(faces), topo::smEnc(v)); else it.raw.remove(QLatin1String(faces)); }
  pickCache_.erase(it.id);
}

bool Viewport::pvShown(int id) const { const Item* it = scene_->item(id); return it && !it->raw.value("pHid").toBool(); }
bool Viewport::pvLocked(int id) const { const Item* it = scene_->item(id); return it && it->raw.value("pLock").toBool(); }
bool Viewport::pvPartHidden(int id) const { const Item* it = scene_->item(id); return it && (!it->raw.value("pHidF").toString().isEmpty() || !it->raw.value("pLockF").toString().isEmpty()); }
void Viewport::pvSetShown(const std::vector<int>& ids, bool on) {
  for (int id : ids) if (Item* it = scene_->item(id)) { if (on) it->raw.remove("pHid"); else it->raw.insert("pHid", true); pickCache_.erase(id); }
  pvTouch();
}
void Viewport::pvSetLocked(const std::vector<int>& ids, bool on) {
  for (int id : ids) if (Item* it = scene_->item(id)) { if (on) it->raw.insert("pLock", true); else it->raw.remove("pLock"); it->raw.remove("pLockF"); }
  pvTouch();
}
void Viewport::pvSolo(const std::vector<int>& ids, bool lock) {
  const std::vector<int> all = pvMeshes(); const std::set<int> S(ids.begin(), ids.end()); const char* k = lock ? "pLock" : "pHid";
  bool already = true; for (int id : all) { const bool off = scene_->item(id)->raw.value(QLatin1String(k)).toBool(); if (S.count(id) ? off : !off) { already = false; break; } }
  for (int id : all) { Item* it = scene_->item(id); const bool off = !already && !S.count(id);
    if (off) it->raw.insert(QLatin1String(k), true); else it->raw.remove(QLatin1String(k));
    if (lock) it->raw.remove("pLockF"); else if (already) it->raw.remove("pHidF");
    pickCache_.erase(id); }
  pvTouch();
  ui::toast(already ? (lock ? tr("Every object unlocked") : tr("Every object shown")) : lock ? tr("Only the picked can be painted — the rest locked") : tr("Only the picked shown"));
}

std::vector<Viewport::PvGroup> Viewport::pvGroups(int m) const {
  const Item* it = scene_->item(m); const auto B = it ? bsp(*it) : nullptr; if (!B || int(B->faceGroup.size()) != B->faceCount() || B->groupNames.empty()) return {};
  const std::vector<char> h = pvVec(*it, "pHid", "pHidF"), l = pvVec(*it, "pLock", "pLockF");
  const size_t G = B->groupNames.size(); std::vector<int> n(G, 0), nh(G, 0), nl(G, 0);
  for (size_t f = 0; f < B->faceGroup.size(); f++) { const int g = B->faceGroup[f]; if (g < 0 || size_t(g) >= G) continue; n[size_t(g)]++; nh[size_t(g)] += h[f]; nl[size_t(g)] += l[f]; }
  std::vector<PvGroup> out; for (size_t g = 0; g < G; g++) if (n[g]) out.push_back({int(g), B->groupNames[g], n[g], nh[g] < n[g], nl[g] == n[g]});
  return out;
}
void Viewport::pvGroupSet(int m, const std::vector<int>& gs, bool lock, bool on) {
  Item* it = scene_->item(m); const auto B = it ? bsp(*it) : nullptr; if (!B || int(B->faceGroup.size()) != B->faceCount()) return;
  const std::set<int> S(gs.begin(), gs.end()); std::vector<char> v = lock ? pvVec(*it, "pLock", "pLockF") : pvVec(*it, "pHid", "pHidF");
  for (size_t f = 0; f < v.size(); f++) if (S.count(B->faceGroup[f])) v[f] = lock ? on : !on;
  if (lock) pvWrite(*it, "pLock", "pLockF", v); else pvWrite(*it, "pHid", "pHidF", v);
  pvTouch();
}
void Viewport::pvGroupSolo(int m, int g, bool lock) {
  Item* it = scene_->item(m); const auto B = it ? bsp(*it) : nullptr; if (!B || int(B->faceGroup.size()) != B->faceCount()) return;
  std::vector<char> v = lock ? pvVec(*it, "pLock", "pLockF") : pvVec(*it, "pHid", "pHidF");
  bool already = true; for (size_t f = 0; f < v.size(); f++) if (bool(v[f]) != (B->faceGroup[f] != g)) { already = false; break; }
  for (size_t f = 0; f < v.size(); f++) v[f] = !already && B->faceGroup[f] != g;
  if (lock) pvWrite(*it, "pLock", "pLockF", v); else pvWrite(*it, "pHid", "pHidF", v);
  pvTouch();
}

// what of a mesh a visibility set holds — Default: everything; a UV set: its polygons; the user's: what was shown when it was made
int Viewport::pvSetFaces(const QString& key, const Item& it, std::vector<char>& flags) const {
  const auto B = bsp(it); if (!B) return 0; const int n = B->faceCount();
  if (key == "default") return 1;
  auto tell = [&]() { bool any = false, all = true; for (char c : flags) { if (c) any = true; else all = false; } return !any ? 0 : all ? 1 : 2; };
  if (key.startsWith("uv:")) {
    if (!topo::hasUvSets(*B)) return 0; const QString nm = key.mid(3); int k = -1; for (size_t i = 0; i < B->uvSetNames.size(); i++) if (B->uvSetNames[i] == nm) k = int(i); if (k < 0) return 0;
    flags.assign(size_t(n), 0); for (int f = 0; f < n && size_t(f) < B->faceSets.size(); f++) flags[size_t(f)] = char(B->faceSets[size_t(f)] >> k & 1u);
    return tell();
  }
  if (key.startsWith("set:")) {
    const QJsonArray a = pvSetsOf(*scene_); const int i = pvFindSet(a, key.mid(4)); if (i < 0) return 0; const QJsonObject o = a[i].toObject();
    std::set<int> hid; for (const QJsonValue& v : o.value("hid").toArray()) hid.insert(v.toInt());
    for (const Item* p = &it; p; p = p->parent >= 0 ? scene_->item(p->parent) : nullptr) if (hid.count(p->id)) return 0;
    const QString s = o.value("f").toObject().value(QString::number(it.id)).toString(); if (s.isEmpty()) return 1;
    std::vector<char> h = topo::smDec(s, n); if (int(h.size()) != n) return 1;
    flags.assign(size_t(n), 0); for (int f = 0; f < n; f++) flags[size_t(f)] = !h[size_t(f)];
    return tell();
  }
  return 0;
}
std::vector<Viewport::PvSet> Viewport::pvSets() const {
  std::vector<PvSet> out; out.push_back({"default", tr("Default"), 0});
  for (const QString& n : uvSetNamesAll()) out.push_back({"uv:" + n, n, 1});
  for (const QString& n : visSetNames()) out.push_back({"set:" + n, n, 2});
  std::vector<int> tot(out.size(), 0), sh(out.size(), 0), lk(out.size(), 0);
  for (int id : pvMeshes()) { const Item& it = *scene_->item(id); const bool vis = scene_->isVisibleBase(it);
    const std::vector<char> h = pvVec(it, "pHid", "pHidF"), l = pvVec(it, "pLock", "pLockF");
    for (size_t i = 0; i < out.size(); i++) { std::vector<char> fl; const int st = pvSetFaces(out[i].key, it, fl); if (!st) continue;
      for (size_t f = 0; f < h.size(); f++) if (st == 1 || fl[f]) { tot[i]++; sh[i] += vis && !h[f]; lk[i] += l[f]; } } }
  for (size_t i = 0; i < out.size(); i++) { out[i].shown = sh[i] > 0; out[i].locked = tot[i] > 0 && lk[i] == tot[i]; }
  return out;
}
void Viewport::pvSetApply(const QString& key) {
  for (auto& [id, it] : scene_->items) if (it.type != ItemType::IMesh && it.raw.contains("pHid")) it.raw.remove("pHid");   // (a folder hidden by an older set: its meshes carry it now)
  for (int id : pvMeshes()) { Item& it = *scene_->item(id); std::vector<char> fl; const int st = pvSetFaces(key, it, fl);
    if (st == 0) { it.raw.insert("pHid", true); it.raw.remove("pHidF"); pickCache_.erase(id); }
    else if (st == 1) { it.raw.remove("pHid"); it.raw.remove("pHidF"); pickCache_.erase(id); }
    else { for (char& c : fl) c = !c; pvWrite(it, "pHid", "pHidF", fl); } }
  pvTouch();
}
void Viewport::pvSetToggle(const QString& key, bool lock, bool on) {
  for (int id : pvMeshes()) { Item& it = *scene_->item(id); std::vector<char> fl; const int st = pvSetFaces(key, it, fl); if (!st) continue;
    std::vector<char> v = lock ? pvVec(it, "pLock", "pLockF") : pvVec(it, "pHid", "pHidF");
    for (size_t f = 0; f < v.size(); f++) if (st == 1 || fl[f]) v[f] = lock ? on : !on;
    if (lock) pvWrite(it, "pLock", "pLockF", v); else pvWrite(it, "pHid", "pHidF", v); }
  pvTouch();
}
void Viewport::pvSetSoloLock(const QString& key) {
  struct M { Item* it; int st; std::vector<char> fl; }; std::vector<M> ms; bool already = true;
  for (int id : pvMeshes()) { M m; m.it = scene_->item(id); m.st = pvSetFaces(key, *m.it, m.fl); const std::vector<char> l = pvVec(*m.it, "pLock", "pLockF");
    for (size_t f = 0; f < l.size(); f++) { const bool in = m.st == 1 || (m.st == 2 && m.fl[f]); if (bool(l[f]) == in) already = false; }
    ms.push_back(std::move(m)); }
  for (M& m : ms) { std::vector<char> v = pvVec(*m.it, "pLock", "pLockF");
    for (size_t f = 0; f < v.size(); f++) v[f] = !already && !(m.st == 1 || (m.st == 2 && m.fl[f]));
    pvWrite(*m.it, "pLock", "pLockF", v); }
  pvTouch();
  ui::toast(already ? tr("Everything unlocked") : tr("Only this set can be painted — the rest locked"));
}

// the shown polygon under a hit (as pickSurface numbers the triangles; a heavy mesh's hit is looked for)
int Viewport::pvPolyFace(const RayHit& hit, const Item& it) const {
  if (!it.poly) return -1; const PolyMesh& P = *it.poly; const auto& t2f = triFaces(it.poly);
  double bd = 1e300; int bf = -1;
  auto tryFace = [&](int f) { const int s = P.faceStart[size_t(f)], e = P.faceStart[size_t(f) + 1];
    for (int c = s + 1; c + 1 < e; c++) { double w[3]; const double d = baryIn(hit.point, P.pos[size_t(P.faceVerts[size_t(s)])], P.pos[size_t(P.faceVerts[size_t(c)])], P.pos[size_t(P.faceVerts[size_t(c + 1)])], w); if (d < bd) { bd = d; bf = f; } } };
  const double tol = P.bounds().size().length() * 1e-5;
  if (hit.tri >= 0 && size_t(hit.tri) < t2f.size()) tryFace(t2f[size_t(hit.tri)]);
  if (bd > tol) for (int f = 0; f < P.faceCount(); f++) tryFace(f);
  return bd > tol * 100 ? -1 : bf;
}

// H — the part under the pointer hidden (the polygons joined to the one pointed at: one lock of the whole hair); Ctrl+H — the whole
// object; Shift+H — everything but that part; Ctrl+Shift+H — everything but that object; Ctrl+X — everything shown
QString Viewport::pvKey(const QString& op) {
  if (room() != "paint") return {};
  auto done = [&](const QString& s) { pvTouch(); ui::toast(s); return s; };
  if (op == "showAll") { int n = 0;
    for (auto& [id, it] : scene_->items) if (it.raw.contains("pHid") || it.raw.contains("pHidF")) { it.raw.remove("pHid"); it.raw.remove("pHidF"); pickCache_.erase(id); n++; }
    return done(n ? tr("Everything shown") : tr("Nothing was hidden")); }
  if (op == "show") {                                            // (the selected polygons that are hidden shown again — the manager's preview picks hidden ones too)
    int n = 0; for (const auto& [m, fs] : pvSel) { Item* x = scene_->item(m); if (!x) continue; std::vector<char> h = pvVec(*x, "pHid", "pHidF"); x->raw.remove("pHid");
      for (int f : fs) if (size_t(f) < h.size() && h[size_t(f)]) { h[size_t(f)] = 0; n++; } pvWrite(*x, "pHid", "pHidF", h); }
    pvSel.clear(); pvSelRev_++;
    return done(n ? tr("%1 hidden polygons shown again").arg(n) : tr("None of the selected was hidden")); }
  if (!pvSel.empty()) {                                          // (polygons selected: they are what is hidden / kept)
    const bool whole = op == "hideObj" || op == "onlyObj", only = op == "only" || op == "onlyObj"; int n = 0;
    if (only) for (int id : pvMeshes()) if (!pvSel.count(id)) { scene_->item(id)->raw.insert("pHid", true); pickCache_.erase(id); }
    for (const auto& [m, fs] : pvSel) { Item* x = scene_->item(m); if (!x) continue; n += int(fs.size());
      if (whole) { if (!only) { x->raw.insert("pHid", true); pickCache_.erase(m); } continue; }
      std::vector<char> h = pvVec(*x, "pHid", "pHidF"); if (only) std::fill(h.begin(), h.end(), char(1));
      for (int f : fs) if (size_t(f) < h.size()) h[size_t(f)] = !only;
      pvWrite(*x, "pHid", "pHidF", h); }
    pvSel.clear(); pvSelRev_++;
    return done(only ? tr("Only the selected shown (%1 polygons) — Ctrl+X shows everything").arg(n) : tr("The selected hidden (%1 polygons) — Ctrl+X shows everything").arg(n)); }
  const auto hit = pickSurface(mouse_); Item* it = hit ? scene_->item(hit->itemId) : nullptr;
  if (!it || !pvIsMesh(*it)) { const QString s = tr("Point at the model: the part under the pointer is hidden"); ui::toast(s); return s; }
  const bool whole = op == "hideObj" || op == "onlyObj", only = op == "only" || op == "onlyObj";
  std::vector<char> part;                                        // (the base's polygons of the part pointed at)
  if (!whole) { const auto B = bsp(*it); const int pf = pvPolyFace(*hit, *it); const std::vector<int> map = polyToBase(*it);
    const int bf = pf < 0 ? -1 : map.empty() ? pf : size_t(pf) < map.size() ? map[size_t(pf)] : -1;
    if (bf < 0 || bf >= B->faceCount()) { const QString s = tr("No polygon found under the pointer"); ui::toast(s); return s; }
    part.assign(size_t(B->faceCount()), 0);
    for (int f : pvWalkAt(mouse_, it->id, bf, pvHKind)) if (f >= 0 && size_t(f) < part.size()) part[size_t(f)] = 1; }   // (what the selection kind means under the pointer: the part, the loop / ring across the strand, the fill)
  if (only) for (int id : pvMeshes()) if (id != it->id) { Item* o = scene_->item(id); o->raw.insert("pHid", true); pickCache_.erase(id); }
  if (op == "hideObj") { it->raw.insert("pHid", true); pickCache_.erase(it->id); return done(tr("%1 hidden — Ctrl+X shows everything").arg(it->name)); }
  if (op == "onlyObj") return done(tr("Only %1 shown — Ctrl+X shows everything").arg(it->name));
  // (the symmetry on: the part's twin goes with it - hidden too, or kept with it; it may lie on another mesh)
  std::map<int, std::vector<char>> parts; parts[it->id] = part;
  if (tset.sym) { std::vector<int> pf; for (size_t f = 0; f < part.size(); f++) if (part[f]) pf.push_back(int(f));
    for (const auto& [mm, ff] : pvMirrored(it->id, pf)) { Item* o = scene_->item(mm); if (!o) continue; auto& v = parts[mm]; if (v.empty()) v.assign(size_t(bsp(*o)->faceCount()), 0); for (int f : ff) if (size_t(f) < v.size()) v[size_t(f)] = 1;
      if (only) { o->raw.remove("pHid"); pickCache_.erase(mm); } } }
  int n = 0;
  for (auto& [mm, pv] : parts) { Item* o = scene_->item(mm); if (!o) continue; std::vector<char> h = pvVec(*o, "pHid", "pHidF"); if (h.size() != pv.size()) continue;
    for (size_t f = 0; f < h.size(); f++) { if (only) h[f] = !pv[f]; else if (pv[f]) h[f] = 1; n += pv[f]; }
    pvWrite(*o, "pHid", "pHidF", h); }
  return done(only ? tr("Only a part of %1 shown (%2 polygons) — Ctrl+X shows everything").arg(it->name).arg(n) : tr("A part of %1 hidden (%2 polygons) — Ctrl+X shows everything").arg(it->name).arg(n));
}

// what the brush may not touch: a hidden polygon, a locked one; while some objects are selected — every object not among them
std::vector<char> Viewport::pvBlocked(const Item& it) const {
  const auto B = bsp(it); if (!B) return {}; const size_t n = size_t(B->faceCount());
  bool selOnly = false; for (int id : sel_.items) if (const Item* x = scene_->item(id); x && pvIsMesh(*x)) { selOnly = true; break; }
  if ((selOnly && !sel_.hasItem(it.id)) || !scene_->isVisible(it)) return std::vector<char>(n, 1);
  std::vector<char> h = pvVec(it, "pHid", "pHidF"); const std::vector<char> l = pvVec(it, "pLock", "pLockF"); bool any = false;
  for (size_t f = 0; f < n; f++) { if (l[f]) h[f] = 1; any |= bool(h[f]); }
  return any ? h : std::vector<char>{};
}
bool Viewport::pvFaceBlocked(const Item& it, int polyFace) const {
  PvCache& c = pvCache_[it.id]; const qint64 key = pvKeyNow();
  if (c.key != key || c.poly != it.poly.get()) {
    c.key = key; c.poly = it.poly.get(); c.blk.clear();
    const std::vector<char> b = pvBlocked(it);
    if (!b.empty() && it.poly) { const int np = it.poly->faceCount(); const std::vector<int> map = polyToBase(it);
      if (allOf(b)) c.blk.assign(size_t(np), 1);
      else { c.blk.assign(size_t(np), 0); for (int f = 0; f < np; f++) { const int bf = map.empty() ? f : map[size_t(f)]; if (bf >= 0 && size_t(bf) < b.size()) c.blk[size_t(f)] = b[size_t(bf)]; } } }
  }
  return polyFace >= 0 && size_t(polyFace) < c.blk.size() && c.blk[size_t(polyFace)];
}

// the same in the texture: the texels of the polygons the brush may not touch — black; the rest (the gaps between the shells too) white
QImage Viewport::pvAllowMask(const QString& set, int w, int h) const {
  struct M { const PolyMesh* B; int k; std::vector<char> b; }; std::vector<M> ms; bool any = false;
  for (const auto& [id, it] : scene_->items) {
    if (it.type != ItemType::IMesh) continue; const auto Bs = bsp(it); if (!Bs) continue; const PolyMesh& B = *Bs;
    if (B.cornerUV.size() != B.faceVerts.size() || B.cornerUV.empty()) continue;
    int k = -1; if (!set.isEmpty()) { for (size_t i = 0; i < B.uvSetNames.size(); i++) if (B.uvSetNames[i] == set) k = int(i); if (k < 0) continue; }
    M m; m.B = &B; m.k = k; m.b = scene_->noPaint(it) ? std::vector<char>(size_t(B.faceCount()), 1) : pvBlocked(it); any |= !m.b.empty(); ms.push_back(std::move(m));
  }
  if (!any || w <= 0 || h <= 0) return {};
  QImage img(w, h, QImage::Format_Grayscale8); img.fill(255);
  QPainter p(&img); p.setRenderHint(QPainter::Antialiasing, false);
  for (int pass = 0; pass < 2; pass++) {                         // (the locked ones first, the free ones over them: a border texel is the free one's)
    p.setBrush(pass ? Qt::white : Qt::black); p.setPen(pass ? QPen(Qt::white, 1) : Qt::NoPen);
    for (const M& m : ms) { const PolyMesh& B = *m.B; if (m.b.empty() && pass == 0) continue; if (pass == 1 && allOf(m.b)) continue;
      const bool hasOwn = m.k >= 0 && size_t(m.k) < B.setUV.size() && B.setUV[size_t(m.k)].size() == B.faceVerts.size();
      for (int f = 0; f < B.faceCount(); f++) {
        const bool blk = !m.b.empty() && m.b[size_t(f)]; if (blk != (pass == 0)) continue;
        if (m.k >= 0 && !(size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> m.k & 1u))) continue;
        const bool own = hasOwn && !(size_t(f) < B.faceLive.size() && B.faceLive[size_t(f)] == m.k); const auto& UV = own ? B.setUV[size_t(m.k)] : B.cornerUV;
        const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; if (e - s < 3) continue;
        double cu = 0, cv = 0; for (int c = s; c < e; c++) { cu += UV[size_t(c)][0]; cv += UV[size_t(c)][1]; } const double ou = std::floor(cu / (e - s)), ov = std::floor(cv / (e - s));
        QPolygonF poly; for (int c = s; c < e; c++) { const double u = UV[size_t(c)][0] - ou, v = UV[size_t(c)][1] - ov; poly << QPointF(u * w, (B.flipY ? 1 - v : v) * h); }
        p.drawPolygon(poly);
      } }
  }
  p.end();
  return img;
}

// ---------- the manager window's reads and writes ----------
std::shared_ptr<const PolyMesh> Viewport::pvBase(int id) const { const Item* it = scene_->item(id); return it ? bsp(*it) : nullptr; }
std::vector<char> Viewport::pvFaceFlags(int id, bool lock) const { const Item* it = scene_->item(id); if (!it) return {}; return lock ? pvVec(*it, "pLock", "pLockF") : pvVec(*it, "pHid", "pHidF"); }
Viewport::PvVis Viewport::pvVisState() const {
  static const char* K[4] = {"pHid", "pHidF", "pLock", "pLockF"}; PvVis v;
  for (const auto& [id, it] : scene_->items) { std::array<QJsonValue, 4> a; bool any = false;
    for (int k = 0; k < 4; k++) { const auto f = it.raw.constFind(QLatin1String(K[k])); if (f != it.raw.constEnd()) { a[size_t(k)] = *f; any = true; } }
    if (any) v.raw[id] = a; }
  v.sel = pvSel; return v;
}
void Viewport::pvVisRestore(const PvVis& v) {
  static const char* K[4] = {"pHid", "pHidF", "pLock", "pLockF"};
  for (auto& [id, it] : scene_->items) { const auto f = v.raw.find(id);
    for (int k = 0; k < 4; k++) { const QLatin1String key(K[k]); if (f != v.raw.end() && !f->second[size_t(k)].isUndefined()) it.raw.insert(key, f->second[size_t(k)]); else it.raw.remove(key); } }
  pickCache_.clear(); pvSel = v.sel; pvSelRev_++; pvTouch();
}
std::vector<int> Viewport::pvLoopAt(int mesh, int bf, int corner, bool ring) const {
  const Item* it = scene_->item(mesh); const auto B = it ? bsp(*it) : nullptr; if (!B || bf < 0 || bf >= B->faceCount()) return {};
  const auto T = topo::ttopo(B); if (bf >= T->nf) return {bf}; const int n = T->faceSize(bf); if (n <= 0) return {bf};
  TopoPick r; r.m = mesh; r.f = bf; r.e = T->ce[size_t(T->fo[size_t(bf)] + ((corner % n) + n) % n)];
  auto fs = tLoopOf(mesh, r, QPointF(), ring, 'F'); if (fs.empty()) fs.push_back(bf); return fs;
}
void Viewport::pvSelSet(const std::map<int, std::vector<int>>& faces, int op) {
  if (op == 0) pvSel.clear();
  for (const auto& [m, fs] : faces) { std::map<int, std::vector<int>> all = pvMirrored(m, fs); { auto& v = all[m]; v.insert(v.end(), fs.begin(), fs.end()); }
    for (const auto& [mm, ff] : all) { std::set<int>& S = pvSel[mm]; for (int f : ff) { if (op == 2) S.erase(f); else S.insert(f); } if (S.empty()) pvSel.erase(mm); } }
  pvSelRev_++; overlayRev_++; requestUpdate(); emit paintVisChanged();
}
// per base polygon the colour of the painted map at its middle (a few texels round it averaged) — the picture the room shows on it
std::vector<QRgb> Viewport::pvFaceColours(int id) const {
  const Item* it = scene_->item(id); const auto Bs = it ? bsp(*it) : nullptr; if (!Bs) return {}; const PolyMesh& B = *Bs;
  std::vector<QRgb> out(size_t(B.faceCount()), 0); const auto& pv = scene_->paintView;
  if (B.cornerUV.size() != B.faceVerts.size() || B.cornerUV.empty()) return out;
  std::map<QString, QImage> conv;                               // (a picture of another format: made 32-bit once)
  auto imgOf = [&](const QString& set) -> const QImage* {
    const QImage* im = nullptr; if (set == pv.set) im = pv.flat.isNull() ? nullptr : &pv.flat; else if (const auto f = pv.others.find(set); f != pv.others.end() && !f->second.first.isNull()) im = &f->second.first;
    if (!im || im->format() == QImage::Format_ARGB32 || im->format() == QImage::Format_ARGB32_Premultiplied || im->format() == QImage::Format_RGB32) return im;
    auto& c = conv[set]; if (c.isNull()) c = im->convertToFormat(QImage::Format_ARGB32); return &c; };
  const size_t NS = B.uvSetNames.size(); std::vector<const QImage*> im(std::max<size_t>(1, NS), nullptr); std::vector<char> own(NS, 0);
  if (NS == 0) im[0] = imgOf(QString()); else for (size_t k = 0; k < NS; k++) { im[k] = imgOf(B.uvSetNames[k]); own[k] = k < B.setUV.size() && B.setUV[k].size() == B.faceVerts.size(); }
  for (int f = 0; f < B.faceCount(); f++) {
    int k = -1; if (NS) { const int live = size_t(f) < B.faceLive.size() ? B.faceLive[size_t(f)] : -1; const quint32 bits = size_t(f) < B.faceSets.size() ? quint32(B.faceSets[size_t(f)]) : 0u;
      if (live >= 0 && size_t(live) < NS && (bits >> live & 1u) && im[size_t(live)]) k = live; else for (size_t j = 0; j < NS; j++) if ((bits >> j & 1u) && im[j]) { k = int(j); break; }
      if (k < 0) continue; }
    const QImage* I = im[size_t(std::max(0, k))]; if (!I) continue;
    const auto& UV = (k >= 0 && own[size_t(k)] && !(size_t(f) < B.faceLive.size() && B.faceLive[size_t(f)] == k)) ? B.setUV[size_t(k)] : B.cornerUV;
    const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; if (e - s < 3) continue;
    double cu = 0, cv = 0; for (int c = s; c < e; c++) { cu += UV[size_t(c)][0]; cv += UV[size_t(c)][1]; } cu /= (e - s); cv /= (e - s); const double ou = std::floor(cu), ov = std::floor(cv);
    const int W = I->width(), H = I->height(); int r = 0, g = 0, b = 0, n = 0;
    auto take = [&](double u, double v) { u -= ou; v -= ov; if (B.flipY) v = 1 - v; const int x = std::clamp(int(u * W), 0, W - 1), y = std::clamp(int(v * H), 0, H - 1);
      const QRgb p = reinterpret_cast<const QRgb*>(I->constScanLine(y))[x]; const int a = qAlpha(p); if (a < 8) return;
      const bool pre = I->format() == QImage::Format_ARGB32_Premultiplied; r += pre ? qRed(p) * 255 / a : qRed(p); g += pre ? qGreen(p) * 255 / a : qGreen(p); b += pre ? qBlue(p) * 255 / a : qBlue(p); n++; };
    take(cu, cv); for (int c = s; c < e && c < s + 4; c++) take(cu * 0.5 + UV[size_t(c)][0] * 0.5, cv * 0.5 + UV[size_t(c)][1] * 0.5);
    if (n) out[size_t(f)] = qRgb(std::min(255, r / n), std::min(255, g / n), std::min(255, b / n)) | 0xFF000000u; }
  return out;
}

}  // namespace hm

// ======================= the room's tools on the model =======================
// The tools other than the brush (a straight line, a curve, a shape, the polygons' selection, the topological symmetry's marks, the text)
// are the left column's: the viewport hands them the mouse (paintToolInput) and the keys, draws what they show (paintToolOverlay) and lays
// a stroke of the brush along a line of the screen for them.
namespace hm {

// a stroke of the brush in hand along a line on the screen: every few pixels the surface under it, its place in the texture — off the
// model the stroke lifts
void Viewport::paintStrokeAlong(const std::vector<QPointF>& pts, const std::function<double(double)>& width) {
  if (!paintInput || pts.size() < 2) return;
  struct LineOn { bool& f; LineOn(bool& x) : f(x) { f = true; } ~LineOn() { f = false; } } lineOn(penLine_);   // (a tool's line: even, whatever the pen pressed when it was laid)
  struct ScaleBack { float& s; ~ScaleBack() { s = 1; } } scaleBack{lineScale_}; lineScale_ = 1;
  double total = 0, gone = 0; for (size_t i = 0; i + 1 < pts.size(); i++) total += std::hypot(pts[i + 1].x() - pts[i].x(), pts[i + 1].y() - pts[i].y());
  if (!width && paintProj && paintProj(0, pts[0], false)) {      // (projected: the line as it lies on the screen)
    for (size_t i = 0; i + 1 < pts.size(); i++) { const QPointF a = pts[i], b = pts[i + 1]; const int n = std::max(1, int(std::ceil(std::hypot(b.x() - a.x(), b.y() - a.y()) / 4.0))); for (int k = 1; k <= n; k++) paintProj(1, a + (b - a) * (double(k) / n), false); }
    paintProj(2, pts.back(), false); requestUpdate(); return; }
  const bool was = paintDrag_; paintDrag_ = true; bool down = false; if (paintStrokeEnd) paintStrokeEnd();
  auto at = [&](const QPointF& p) {
    const auto h = pickSurface(p); const auto uv = h ? paintUvOfHit(*h) : std::nullopt;
    if (!uv) { if (down) { if (paintStrokeEnd) paintStrokeEnd(); paintInput(3, {}, false); } return; }
    paintTex_ = *uv; paintScaleUpdate(p, *uv); if (paintMirror) paintMirror(tset.sym ? paintMirrorsUv(*h) : std::vector<QPointF>{});
    paintInput(down ? 1 : 0, *uv, false); down = true; };
  for (size_t i = 0; i + 1 < pts.size(); i++) { const QPointF a = pts[i], b = pts[i + 1]; const double L = std::hypot(b.x() - a.x(), b.y() - a.y()); const int n = std::max(1, int(std::ceil(L / 3.0)));
    for (int k = i ? 1 : 0; k <= n; k++) { if (width) lineScale_ = float(std::max(0.02, width(total > 1e-9 ? (gone + L * k / n) / total : 0))); at(a + (b - a) * (double(k) / n)); }
    gone += L; }
  paintInput(2, {}, false); if (paintStrokeEnd) paintStrokeEnd(); paintDrag_ = was; requestUpdate();
}

namespace {
// a mesh's fan triangles as the ray caster takes them (made once per geometry)
const std::vector<uint32_t>& fanTris(const std::shared_ptr<PolyMesh>& sp) {
  static std::map<const PolyMesh*, std::pair<std::weak_ptr<PolyMesh>, std::vector<uint32_t>>> tc;
  for (auto q = tc.begin(); q != tc.end();) { if (q->second.first.expired()) q = tc.erase(q); else ++q; }
  auto f = tc.find(sp.get()); if (f != tc.end() && f->second.first.lock() == sp) return f->second.second;
  std::vector<uint32_t> t; const PolyMesh& p = *sp;
  for (int fi = 0; fi < p.faceCount(); fi++) { const int s = p.faceStart[size_t(fi)], e = p.faceStart[size_t(fi) + 1]; for (int k = s + 1; k < e - 1; k++) { t.push_back(uint32_t(p.faceVerts[size_t(s)])); t.push_back(uint32_t(p.faceVerts[size_t(k)])); t.push_back(uint32_t(p.faceVerts[size_t(k + 1)])); } }
  return (tc[sp.get()] = {sp, std::move(t)}).second;
}
}  // namespace

std::optional<Viewport::PaintAnchor> Viewport::paintAnchorAt(const QPointF& m) const {
  const auto h = pickSurface(m); if (!h) return std::nullopt; const Item* it = scene_->item(h->itemId); if (!it || !pvIsMesh(*it) || !it->poly) return std::nullopt;
  if (!paintUvOfHit(*h)) return std::nullopt;                   // (not of the UV set painted, or locked: no point to hang a line on)
  return PaintAnchor{h->point, h->normal, h->itemId};
}
std::optional<QPointF> Viewport::paintAnchorScreen(const PaintAnchor& a) const { const ScreenPt s = projector().toScreen(a.p); if (s.z >= 1) return std::nullopt; return QPointF(s.x, s.y); }

bool g_lineAnyWall = false;
void Viewport::paintStrokeThrough(const std::vector<PaintAnchor>& pts, const std::function<double(double)>& width) {
  if (!paintInput || pts.size() < 2) return;
  struct LineOn { bool& f; LineOn(bool& x) : f(x) { f = true; } ~LineOn() { f = false; } } lineOn(penLine_);
  struct ScaleBack { float& s; ~ScaleBack() { s = 1; } } scaleBack{lineScale_}; lineScale_ = 1;
  // (the meshes the points lie on: the surface is looked for on them)
  std::vector<PickMesh> list; double size = 1e-9;
  for (const PaintAnchor& a : pts) { bool have = false; for (const PickMesh& pm : list) have |= pm.itemId == a.item; if (have) continue;
    const Item* it = scene_->item(a.item); if (!it || !it->poly) continue; PickMesh pm; pm.itemId = it->id; pm.pos = &it->poly->pos; pm.trisRef = &fanTris(it->poly); pm.box = it->poly->bounds(); size = std::max(size, pm.box.size().length()); list.push_back(pm); }
  if (list.empty()) return;
  double total = 0, gone = 0; for (size_t i = 0; i + 1 < pts.size(); i++) total += pts[i + 1].p.distanceTo(pts[i].p);
  const double step = size * 0.0015; const Projector P = projector();
  // (the brush's size on the screen, taken before the first texel: at the first point of the line the camera sees — a line that begins
  // on the far side is as wide there as where it comes into view, not the size left from the last stroke)
  for (const PaintAnchor& a : pts) { const ScreenPt s = P.toScreen(a.p); if (s.z >= 1) continue; const QPointF sp(s.x, s.y);
    const auto fh = pickSurface(sp); if (!fh || fh->point.distanceTo(a.p) > size * 0.003) continue; if (const auto uv = paintUvOfHit(*fh)) { paintScaleUpdate(sp, *uv); break; } }
  const bool was = paintDrag_; paintDrag_ = true; bool down = false; if (paintStrokeEnd) paintStrokeEnd();
  auto lift = [&] { if (down) { if (paintStrokeEnd) paintStrokeEnd(); paintInput(3, {}, false); } };
  for (size_t i = 0; i + 1 < pts.size(); i++) {
    const PaintAnchor &A = pts[i], &B = pts[i + 1]; const double L = B.p.distanceTo(A.p); const int n = std::clamp(int(std::ceil(L / step)), 1, 2000);
    for (int k = i ? 1 : 0; k <= n; k++) { const double t = double(k) / n; const Vec3 Pt = A.p + (B.p - A.p) * t; Vec3 N = A.n * (1 - t) + B.n * t; if (N.lengthSq() < 1e-20) N = A.n; N = N.normalized();
      // (the surface under the straight line: looked for along the normal from outside and from inside, every wall the ray crosses.
      // A wall facing the way the points' own wall faces wins over one facing the other way, then the nearest to the line: between two
      // points on a tube the straight line runs inside it, and a tube with a thickness has its inner wall nearer to the line there —
      // the paint would go through onto it (as the Topology knife once did). The inner wall faces inward, so it loses)
      const double reach = L * 0.6 + size * 0.01; std::optional<RayHit> best; bool bestFacing = false; const double skip = size * 1e-5;
      for (int side = 0; side < 2; side++) { const Vec3 d = side ? N : N * -1.0; double gone0 = 0;
        for (int k = 0; k < 4; k++) { const auto h0 = raycast(Ray{Pt - d * (reach - gone0), d}, list); if (!h0) break;
          RayHit h = *h0; h.t += gone0; if (h.t >= 2 * reach) break;
          const bool facing = g_lineAnyWall || h.normal.dot(N) > 0;   // (g_lineAnyWall: the old choice — the nearest wall whichever way it faces; a check compares the two)
          if (!best || (facing && !bestFacing) || (facing == bestFacing && std::abs(h.t - reach) < std::abs(best->t - reach))) { best = h; bestFacing = facing; }
          gone0 = h.t + skip; } }
      const auto uv = best ? paintUvOfHit(*best) : std::nullopt;
      if (!uv) { lift(); continue; }
      paintTex_ = *uv;
      // (the brush's size is the one taken before the loop and kept the whole way: measured again at every point it went thin where
      // the tube is seen edge-on — a third of its width there, 18.11 — and the line is one width all round)
      if (paintMirror) paintMirror(tset.sym ? paintMirrorsUv(*best) : std::vector<QPointF>{});
      if (width) lineScale_ = float(std::max(0.02, width(total > 1e-12 ? (gone + L * t) / total : 0)));
      paintInput(down ? 1 : 0, *uv, false); down = true; }
    gone += L; }
  paintInput(2, {}, false); if (paintStrokeEnd) paintStrokeEnd(); paintDrag_ = was; requestUpdate();
}

// (a check: a tube with a thickness — an outer wall facing out, an inner one facing in, rims between — round the camera's pivot, axis up;
// the outer wall's UVs in the left half of the texture, the inner wall's in the right half: paint on the inner wall is seen apart)
int Viewport::testThickTube(double R, double thick) {
  PolyMesh m; m.faceStart.push_back(0); const int S = 32, H = 6; const Vec3 c = cam_.pivot; const double h = R * 3;
  auto at = [&](double r, int i, int j) { const double a = 6.283185307 * i / S; return c + Vec3(std::cos(a) * r, -h / 2 + h * j / H, std::sin(a) * r); };
  for (int w = 0; w < 2; w++) for (int j = 0; j <= H; j++) for (int i = 0; i < S; i++) m.pos.push_back(at(w ? R - thick : R, i, j));
  auto vid = [&](int w, int i, int j) { return w * (H + 1) * S + j * S + (i % S); };
  auto uv = [&](int w, int i, int j) { return std::array<float, 2>{float(0.02 + 0.44 * i / S + (w ? 0.5 : 0)), float(0.05 + 0.9 * j / H)}; };
  for (int w = 0; w < 2; w++) for (int j = 0; j < H; j++) for (int i = 0; i < S; i++) {
    int v[4] = {vid(w, i, j), vid(w, i + 1, j), vid(w, i + 1, j + 1), vid(w, i, j + 1)}; std::array<float, 2> u[4] = {uv(w, i, j), uv(w, i + 1, j), uv(w, i + 1, j + 1), uv(w, i, j + 1)};
    if (w == 0) { std::swap(v[1], v[3]); std::swap(u[1], u[3]); }   // (the outer wall wound to face out, the inner one to face in)
    m.addFace(v, 4); for (auto& x : u) m.cornerUV.push_back(x); }
  for (int j : {0, H}) for (int i = 0; i < S; i++) { int v[4] = {vid(0, i, j), vid(0, i + 1, j), vid(1, i + 1, j), vid(1, i, j)}; if (j == H) std::swap(v[1], v[3]);
    m.addFace(v, 4); for (int k = 0; k < 4; k++) m.cornerUV.push_back({0.97f, 0.97f}); }
  m.uvSetNames = {"Tube"}; m.faceSets.assign(size_t(m.faceCount()), 1u); m.faceLive.assign(size_t(m.faceCount()), 0); m.setUV = {m.cornerUV};   // (its own UV set)
  const int id = topoNewMesh(-1, "ThickTube", m); sceneChanged(false); return id;
}
QString Viewport::testOpenSides(int item, const std::vector<Vec3>& pts, const std::vector<Vec3>& nrm) {
  const Item* it = scene_->item(item); if (!it || !it->poly) return "no mesh";
  std::vector<PickMesh> list(1); list[0].itemId = it->id; list[0].pos = &it->poly->pos; list[0].trisRef = &fanTris(it->poly); list[0].box = it->poly->bounds();
  const double eps = it->poly->bounds().size().length() * 1e-4; int out = 0, back = 0, shut = 0;
  for (size_t i = 0; i < pts.size() && i < nrm.size(); i++) {
    const bool a = !raycast(Ray{pts[i] + nrm[i] * eps, nrm[i]}, list), b = !raycast(Ray{pts[i] - nrm[i] * eps, nrm[i] * -1.0}, list);
    if (a) out++; else if (b) back++; else shut++; }
  return QString("open along +n %1, along -n only %2, walled in both ways %3").arg(out).arg(back).arg(shut);
}
QString Viewport::testWrapLineAt(const QPointF& m, double frac, int n, Vec3* cOut, Vec3* axOut, double* rOut) {
  const auto h = pickSurface(m); if (!h) return "nothing under the point";
  const Item* hi = scene_->item(h->itemId); QString who = hi ? QString("%1 \"%2\" uv %3 sets [%4] · ").arg(hi->id).arg(hi->name).arg(hi->poly ? int(hi->poly->cornerUV.size()) : -1).arg(hi->poly ? QStringList(hi->poly->uvSetNames.begin(), hi->poly->uvSetNames.end()).join(",") : QString()) : QString();
  return who + testWrapLine(h->itemId, -1, frac, n, cOut, axOut, rOut, &h->point);
}
QString Viewport::testWrapLine(int item, double a, double b, int n, Vec3* cOut, Vec3* axOut, double* rOut, const Vec3* near) {
  const Item* it = scene_->item(item); if (!it || !it->poly) return "no mesh";
  const auto& P = it->poly->pos; const Box3 bx = it->poly->bounds(); const double x0 = bx.min.x + (bx.max.x - bx.min.x) * a, x1 = bx.min.x + (bx.max.x - bx.min.x) * b;
  std::vector<Vec3> v; if (near) { const double R = bx.size().length() * b; for (const Vec3& p : P) if (p.distanceTo(*near) <= R) v.push_back(p); }
  else for (const Vec3& p : P) if (p.x >= x0 && p.x <= x1) v.push_back(p);
  if (v.size() < 20) return QString("too few vertices: %1").arg(v.size());
  Vec3 c; for (const Vec3& p : v) c += p; c = c / double(v.size());
  double C[3][3] = {}; for (const Vec3& p : v) { const double d[3] = {p.x - c.x, p.y - c.y, p.z - c.z}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) C[i][j] += d[i] * d[j]; }
  Vec3 ax(1, 0.3, 0.1); for (int k = 0; k < 60; k++) { ax = Vec3(C[0][0] * ax.x + C[0][1] * ax.y + C[0][2] * ax.z, C[1][0] * ax.x + C[1][1] * ax.y + C[1][2] * ax.z, C[2][0] * ax.x + C[2][1] * ax.y + C[2][2] * ax.z).normalized(); }
  double r = 0; for (const Vec3& p : v) { const Vec3 d = p - c; r = std::max(r, (d - ax * d.dot(ax)).length()); }
  const Vec3 e1 = (std::abs(ax.y) < 0.9 ? Vec3(0, 1, 0) : Vec3(1, 0, 0)).cross(ax).normalized(), e2 = ax.cross(e1).normalized();
  std::vector<PickMesh> list(1); list[0].itemId = it->id; list[0].pos = &P; list[0].trisRef = &fanTris(it->poly); list[0].box = bx;
  std::vector<PaintAnchor> pts;
  for (int k = 0; k < n; k++) { const double ang = 6.283185307 * k / n; const Vec3 d = e1 * std::cos(ang) + e2 * std::sin(ang);
    const auto h = raycast(Ray{c + d * (r * 3), d * -1.0}, list); if (!h) continue;
    pts.push_back({h->point, h->normal, it->id}); }   // (the normal as wound — as a click on the model gives it)
  if (pts.size() < 3) return QString("anchors %1").arg(pts.size());
  QStringList under; for (const PaintAnchor& q : pts) { const Vec3 dn = q.n.normalized() * (q.n.dot(q.p - c) > 0 ? -1.0 : 1.0);   // (into the part)
    const auto h2 = raycast(Ray{q.p + dn * (r * 1e-3), dn}, list); under << (h2 ? QString::number(h2->t / r, 'f', 3) : QString("-")); }
  pts.push_back(pts.front());
  paintStrokeThrough(pts);
  if (cOut) *cOut = c; if (axOut) *axOut = ax; if (rOut) *rOut = r;
  return QString("the next wall under each point, of the radius: %1 · ").arg(under.join(" ")) + QString("anchors %1 · ring centre %2,%3,%4 · axis %5,%6,%7 · radius %8").arg(pts.size() - 1).arg(c.x, 0, 'f', 3).arg(c.y, 0, 'f', 3).arg(c.z, 0, 'f', 3).arg(ax.x, 0, 'f', 2).arg(ax.y, 0, 'f', 2).arg(ax.z, 0, 'f', 2).arg(r, 0, 'f', 3);
}

// ---------- the polygons selected in the room ----------
Viewport::PvFace Viewport::pvFaceAtUv(const QPointF& uv) const {
  const auto hit = paintHitOfUv(uv); const Item* it = hit ? scene_->item(hit->itemId) : nullptr; if (!it || !pvIsMesh(*it)) return {};
  const int pf = pvPolyFace(*hit, *it); const std::vector<int> map = polyToBase(*it);
  const int bf = pf < 0 ? -1 : map.empty() ? pf : size_t(pf) < map.size() ? map[size_t(pf)] : -1;
  return bf < 0 ? PvFace{} : PvFace{it->id, bf};
}
std::vector<QPointF> Viewport::paintMirrorsUvAt(const QPointF& m) const {
  if (!tset.sym) return {}; const auto h = pickSurface(m); if (!h) return {}; return paintMirrorsUv(*h);
}
Viewport::PvFace Viewport::pvFaceAt(const QPointF& m) const {
  const auto hit = pickSurface(m); const Item* it = hit ? scene_->item(hit->itemId) : nullptr; if (!it || !pvIsMesh(*it)) return {};
  const int pf = pvPolyFace(*hit, *it); const std::vector<int> map = polyToBase(*it);
  const int bf = pf < 0 ? -1 : map.empty() ? pf : size_t(pf) < map.size() ? map[size_t(pf)] : -1;
  return bf < 0 ? PvFace{} : PvFace{it->id, bf};
}
int Viewport::pvSelCount() const { int n = 0; for (const auto& [m, fs] : pvSel) n += int(fs.size()); return n; }
void Viewport::pvSelClear() { if (pvSel.empty()) return; pvSel.clear(); pvSelRev_++; overlayRev_++; requestUpdate(); emit paintVisChanged(); }
// kind: face — the polygon; part — the polygons joined to it; group — its polygroup. op: 0 — it alone, 1 — added, 2 — taken off
void Viewport::pvSelPick(const QPointF& m, const QString& kind, int op) {
  const PvFace h = pvFaceAt(m); if (h.mesh < 0) { if (op == 0) pvSelClear(); return; }
  const Item* it = scene_->item(h.mesh); const auto B = bsp(*it); std::vector<int> fs{h.face};
  if (kind == "loop" || kind == "ring" || kind == "fill" || kind == "part") fs = pvWalkAt(m, h.mesh, h.face, kind);
  else if (kind == "group" && int(B->faceGroup.size()) == B->faceCount()) { fs.clear(); const int g = B->faceGroup[size_t(h.face)]; for (int f = 0; f < B->faceCount(); f++) if (B->faceGroup[size_t(f)] == g) fs.push_back(f); }
  if (op == 0) pvSel.clear();
  std::map<int, std::vector<int>> all = pvMirrored(h.mesh, fs); { auto& v = all[h.mesh]; v.insert(v.end(), fs.begin(), fs.end()); }   // (the symmetry: the twin selected / taken off too)
  for (const auto& [mm, ff] : all) { std::set<int>& S = pvSel[mm]; for (int f : ff) { if (op == 2) S.erase(f); else S.insert(f); } if (S.empty()) pvSel.erase(mm); }
  pvSelRev_++; overlayRev_++; requestUpdate(); emit paintVisChanged();
}
std::vector<int> Viewport::pvWalkAt(const QPointF& m, int mesh, int bf, const QString& kind) const {
  const Item* it = scene_->item(mesh); const auto B = it ? bsp(*it) : nullptr; if (!B || bf < 0 || bf >= B->faceCount()) return {};
  if (kind == "loop" || kind == "ring") { TopoPick r; r.m = mesh; r.f = bf; auto fs = tLoopOf(mesh, r, m, kind == "ring", 'f'); if (fs.empty()) fs.push_back(bf); return fs; }
  if (kind == "fill") {                                          // (as the Topology room's Fill: joined polygons up to a UV seam or the selection's border)
    const auto T = topo::ttopo(B); const auto seam = uv::seamFlags(*B, *T); const auto sit = pvSel.find(mesh); const std::set<int>* sel = sit != pvSel.end() ? &sit->second : nullptr;
    const bool inSel = sel && sel->count(bf);
    std::vector<char> seen(size_t(T->nf), 0); std::vector<int> out, q{bf}; seen[size_t(bf)] = 1;
    while (!q.empty()) { const int f = q.back(); q.pop_back(); out.push_back(f);
      for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int e = T->ce[size_t(k)]; if (size_t(e) < seam.size() && seam[size_t(e)]) continue;
        const int g = T->ef0[size_t(e)] == f ? T->ef1[size_t(e)] : T->ef0[size_t(e)]; if (g < 0 || seen[size_t(g)]) continue;
        if (sel && bool(sel->count(g)) != inSel) continue;         // (the selection's border: not crossed)
        seen[size_t(g)] = 1; q.push_back(g); } }
    return out; }
  if (kind == "part") { std::vector<int> fs; const auto& parts = partsOf(B); const int p0 = parts[size_t(bf)]; for (int f = 0; f < B->faceCount(); f++) if (parts[size_t(f)] == p0) fs.push_back(f); return fs; }
  if (kind == "group" && int(B->faceGroup.size()) == B->faceCount()) { std::vector<int> fs; const int g = B->faceGroup[size_t(bf)]; for (int f = 0; f < B->faceCount(); f++) if (B->faceGroup[size_t(f)] == g) fs.push_back(f); return fs; }
  return {bf};
}
// ---- the symmetry of the room's selection and hiding ----
// The polygons that mirror these, under each mirror / turn of the symmetry: a polygon's middle carried across in space, the polygon of
// any painted mesh whose middle lies nearest there, within the polygons' own size - a lock split into a left and a right mesh finds its
// twin too. The middles of all the room's polygons are kept in a grid, made again when the scene changes
std::map<int, std::vector<int>> Viewport::pvMirrored(int mesh, const std::vector<int>& fs) const {
  std::map<int, std::vector<int>> out; if (!tset.sym || fs.empty()) return out;
  const Item* it = scene_->item(mesh); if (!it || !it->poly || it->type != ItemType::IMesh) return out;
  const auto& Ts = pvSymT(*it); if (Ts.empty()) return out;
  struct Idx { QString key; std::vector<int> mesh, face; std::vector<Vec3> c; std::vector<float> r; std::map<int, int> start; double cell = 1; std::unordered_map<long long, std::vector<int>> g; };
  static Idx I;
  const QString key = QString("%1|%2|%3").arg(scene_->revision).arg(scene_->items.size()).arg(pvKeyNow());
  auto cellKey = [&](long long x, long long y, long long z) { return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
  if (I.key != key) {
    I = Idx(); I.key = key; double rs = 0;
    for (int id : pvMeshes()) { const Item* m = scene_->item(id); if (!m || !m->poly || m->type != ItemType::IMesh) continue; const auto B = bsp(*m); const auto& pos = m->poly->pos; if (pos.size() < B->pos.size()) continue;
      I.start[id] = int(I.c.size());
      for (int f = 0; f < B->faceCount(); f++) { const int a = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1]; Vec3 c; for (int k = a; k < e; k++) c += pos[size_t(B->faceVerts[size_t(k)])]; c = c * (1.0 / std::max(1, e - a));
        double r = 0; for (int k = a; k < e; k++) r = std::max(r, pos[size_t(B->faceVerts[size_t(k)])].distanceTo(c));
        I.mesh.push_back(id); I.face.push_back(f); I.c.push_back(c); I.r.push_back(float(r)); rs += r; } }
    if (I.c.empty()) return out;
    I.cell = std::max(1e-9, 2 * rs / double(I.c.size()));
    for (size_t i = 0; i < I.c.size(); i++) { const Vec3& c = I.c[i]; I.g[cellKey((long long)std::floor(c.x / I.cell), (long long)std::floor(c.y / I.cell), (long long)std::floor(c.z / I.cell))].push_back(int(i)); } }
  const auto st = I.start.find(mesh); if (st == I.start.end()) return out;
  const bool moved = it->poly != bsp(*it); const Affine X = moved ? meshmods::meshMatrix(*scene_, *it) : Affine(), Xi = X.inverse();
  std::set<std::pair<int, int>> got;
  for (const Affine& T : Ts) { const Affine W = Affine::mul(X, Affine::mul(T, Xi));
    for (int f : fs) { const size_t si = size_t(st->second + f); if (f < 0 || si >= I.c.size() || I.mesh[si] != mesh) continue;
      const Vec3 p = W.apply(I.c[si]); const double thr0 = I.r[si] * 0.9; const int span = std::max(1, int(std::ceil(thr0 / I.cell)));
      const long long cx = (long long)std::floor(p.x / I.cell), cy = (long long)std::floor(p.y / I.cell), cz = (long long)std::floor(p.z / I.cell);
      int best = -1; double bd = 1e300;
      for (int dz = -span; dz <= span; dz++) for (int dy = -span; dy <= span; dy++) for (int dx = -span; dx <= span; dx++) {
        const auto q = I.g.find(cellKey(cx + dx, cy + dy, cz + dz)); if (q == I.g.end()) continue;
        for (int j : q->second) { const double d = I.c[size_t(j)].distanceTo(p); if (d < bd && d <= std::max(thr0, double(I.r[size_t(j)]) * 0.9)) { bd = d; best = j; } } }
      if (best >= 0 && !(I.mesh[size_t(best)] == mesh && I.face[size_t(best)] == f) && got.insert({I.mesh[size_t(best)], I.face[size_t(best)]}).second) out[I.mesh[size_t(best)]].push_back(I.face[size_t(best)]); } }
  return out;
}

void Viewport::pvHoverAt(const QPointF& m, const QString& kind) {
  std::map<int, std::vector<int>> h;
  if (kind == "loop" || kind == "ring" || kind == "fill") { const PvFace pf = pvFaceAt(m); if (pf.mesh >= 0) h[pf.mesh] = pvWalkAt(m, pf.mesh, pf.face, kind); }
  if (h == pvHover_) return; pvHover_ = std::move(h); pvHoverC_ = kind == "fill" ? QColor(0xff, 0x9a, 0x2e) : QColor(0xff, 0xd8, 0x3a); overlayRev_++; requestUpdate();
}
const paint::IslandMap& Viewport::paintIslands(const QString& set, int W, int H) const {
  const QString key = QString("%1|%2x%3|%4|%5").arg(set).arg(W).arg(H).arg(scene_->revision).arg(scene_->items.size());
  if (key == islandsKey_) return islands_;
  islandsKey_ = key; islands_ = {}; islandsWire_.clear(); if (W <= 0 || H <= 0) return islands_;
  // the polygons of each mesh in the set, as texture polygons; islands — joined where two polygons share a UV edge
  struct F { std::vector<QPointF> uv; int root; };
  std::vector<F> fs; std::vector<int> par;
  std::function<int(int)> find = [&](int x) { while (par[size_t(x)] != x) { par[size_t(x)] = par[size_t(par[size_t(x)])]; x = par[size_t(x)]; } return x; };
  for (int id : pvMeshes()) {
    const Item* it = scene_->item(id); const auto Bs = it ? bsp(*it) : nullptr; if (!Bs) continue; const PolyMesh& B = *Bs;
    if (B.cornerUV.size() != B.faceVerts.size() || B.cornerUV.empty()) continue;
    int ks = -1; if (!set.isEmpty()) { for (size_t i = 0; i < B.uvSetNames.size(); i++) if (B.uvSetNames[i] == set) ks = int(i); if (ks < 0) continue; }
    const bool hasOwn = ks >= 0 && size_t(ks) < B.setUV.size() && B.setUV[size_t(ks)].size() == B.faceVerts.size();
    std::unordered_map<quint64, int> edgeFace;                   // (a UV edge — its two ends' texels — to the polygon first seen with it)
    auto q = [&](const QPointF& p) { return quint64(quint32(std::lround(p.x() * 65536.0))) << 32 | quint32(std::lround(p.y() * 65536.0)); };
    for (int f = 0; f < B.faceCount(); f++) {
      if (ks >= 0 && !(size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> ks & 1u))) continue;
      const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; if (e - s < 3) continue;
      const bool own = hasOwn && !(size_t(f) < B.faceLive.size() && B.faceLive[size_t(f)] == ks); const auto& UV = own ? B.setUV[size_t(ks)] : B.cornerUV;
      double cu = 0, cv = 0; for (int k = s; k < e; k++) { cu += UV[size_t(k)][0]; cv += UV[size_t(k)][1]; } const double ou = std::floor(cu / (e - s)), ov = std::floor(cv / (e - s));
      F x; for (int k = s; k < e; k++) { const double u = UV[size_t(k)][0] - ou, v = UV[size_t(k)][1] - ov; x.uv.push_back(QPointF(u, B.flipY ? 1 - v : v)); }
      const int me = int(fs.size()); x.root = me; par.push_back(me); fs.push_back(x);
      for (size_t k = 0; k < fs[size_t(me)].uv.size(); k++) {
        quint64 a = q(fs[size_t(me)].uv[k]), b = q(fs[size_t(me)].uv[(k + 1) % fs[size_t(me)].uv.size()]); if (a > b) std::swap(a, b);
        const quint64 h = a * 1000003ull ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2));
        auto [itE, fresh] = edgeFace.try_emplace(h, me); if (!fresh) { const int ra = find(me), rb = find(itE->second); if (ra != rb) par[size_t(ra)] = rb; } } }
  }
  if (fs.empty()) return islands_;
  std::unordered_map<int, int> num; for (size_t i = 0; i < fs.size(); i++) { const int r = find(int(i)); if (!num.count(r)) num[r] = int(num.size()) + 1; fs[i].root = num[r]; }
  QImage ids(W, H, QImage::Format_RGB32); ids.fill(0);
  if (fs.size() <= 300000) for (const F& f : fs) { QPolygonF poly; for (const QPointF& u : f.uv) poly << QPointF(u.x() * W, u.y() * H); islandsWire_.push_back(poly); }   // (the wire over the texture: not for a monstrous mesh)
  { QPainter p(&ids); p.setRenderHint(QPainter::Antialiasing, false); p.setPen(Qt::NoPen);
    for (const F& f : fs) { QPolygonF poly; for (const QPointF& u : f.uv) poly << QPointF(u.x() * W, u.y() * H); p.setBrush(QColor(QRgb(0xFF000000u | quint32(f.root)))); p.drawPolygon(poly); } }
  // (grown 3 texels into the empty texels round them — the padding the bleed fills)
  for (int pass = 0; pass < 3; pass++) { const QImage was = ids;
    for (int y = 0; y < H; y++) { const quint32* r0 = reinterpret_cast<const quint32*>(was.constScanLine(y)); quint32* out = reinterpret_cast<quint32*>(ids.scanLine(y));
      const quint32* up = y > 0 ? reinterpret_cast<const quint32*>(was.constScanLine(y - 1)) : nullptr; const quint32* dn = y + 1 < H ? reinterpret_cast<const quint32*>(was.constScanLine(y + 1)) : nullptr;
      for (int x = 0; x < W; x++) { if (r0[x] & 0xFFFFFFu) continue; quint32 v = 0;
        if (x > 0 && (r0[x - 1] & 0xFFFFFFu)) v = r0[x - 1]; else if (x + 1 < W && (r0[x + 1] & 0xFFFFFFu)) v = r0[x + 1]; else if (up && (up[x] & 0xFFFFFFu)) v = up[x]; else if (dn && (dn[x] & 0xFFFFFFu)) v = dn[x];
        if (v) out[x] = v; } } }
  // each island's long axis: the principal direction of its corners (texture px)
  const int n = int(num.size()); std::vector<double> sx(size_t(n) + 1), sy(size_t(n) + 1), sxx(size_t(n) + 1), syy(size_t(n) + 1), sxy(size_t(n) + 1), cnt(size_t(n) + 1);
  for (const F& f : fs) for (const QPointF& u : f.uv) { const double x = u.x() * W, y = u.y() * H; const size_t k = size_t(f.root); sx[k] += x; sy[k] += y; sxx[k] += x * x; syy[k] += y * y; sxy[k] += x * y; cnt[k]++; }
  islands_.angle.assign(size_t(n) + 1, std::numeric_limits<float>::quiet_NaN());
  for (int k = 1; k <= n; k++) { const double c = cnt[size_t(k)]; if (c < 3) continue; const double mx = sx[size_t(k)] / c, my = sy[size_t(k)] / c;
    const double a = sxx[size_t(k)] / c - mx * mx, b = sxy[size_t(k)] / c - mx * my, d = syy[size_t(k)] / c - my * my;
    islands_.angle[size_t(k)] = float(0.5 * std::atan2(2 * b, a - d) * 57.29577951); }
  islands_.ids = ids;
  return islands_;
}
void Viewport::pvSelLock(bool on) {
  for (const auto& [m, fs] : pvSel) if (Item* it = scene_->item(m)) { std::vector<char> v = pvVec(*it, "pLock", "pLockF"); for (int f : fs) if (size_t(f) < v.size()) v[size_t(f)] = on; pvWrite(*it, "pLock", "pLockF", v); }
  pvTouch();
}
// the selection's outline and the topological symmetry's two marks, over the model
void Viewport::pvOverlay(std::vector<Overlay>& out) const {
  if (room() != "paint") return;
  paintRingOverlay(out);
  if (pvSelSegRev_ != pvSelRev_) {                               // (the outline: the edges one selected polygon has alone — made once per selection)
    pvSelSegRev_ = pvSelRev_; pvSelSeg_.clear();
    for (const auto& [m, fs] : pvSel) { const Item* it = scene_->item(m); const auto B = it ? bsp(*it) : nullptr; if (!B) continue;
      std::map<std::pair<int, int>, int> cnt;
      for (int f : fs) { if (f < 0 || f >= B->faceCount()) continue; const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1];
        for (int k = s; k < e; k++) { const int a = B->faceVerts[size_t(k)], b = B->faceVerts[size_t(k + 1 < e ? k + 1 : s)]; cnt[{std::min(a, b), std::max(a, b)}]++; } }
      for (const auto& [ed, n] : cnt) if (n == 1) { pvSelSeg_.push_back({m, ed.first, ed.second}); if (pvSelSeg_.size() > 8000) break; } }
  }
  const Projector P = projector();
  auto seg = [&](const Vec3& a, const Vec3& b, const QColor& c) { const ScreenPt s0 = P.toScreen(a), s1 = P.toScreen(b); if (s0.z < 1 && s1.z < 1) out.push_back({{{s0.x, s0.y}, {s1.x, s1.y}}, c, false}); };
  for (const auto& sg : pvSelSeg_) { const Item* it = scene_->item(sg[0]); if (!it || !it->poly || !scene_->isVisible(*it)) continue; const auto& pos = it->poly->pos;
    if (size_t(sg[1]) < pos.size() && size_t(sg[2]) < pos.size()) seg(pos[size_t(sg[1])], pos[size_t(sg[2])], QColor(0xff, 0xd8, 0x4a)); }
  for (const auto& [hm, hfs] : pvHover_) { const Item* it = scene_->item(hm); const auto B = it ? bsp(*it) : nullptr; if (!B || !it->poly) continue; const auto& pos = it->poly->pos;   // (the walk under the pointer)
    std::map<std::pair<int, int>, int> cnt; for (int f : hfs) { if (f < 0 || f >= B->faceCount()) continue; const int s0 = B->faceStart[size_t(f)], e0 = B->faceStart[size_t(f) + 1];
      for (int k = s0; k < e0; k++) { const int a = B->faceVerts[size_t(k)], b = B->faceVerts[size_t(k + 1 < e0 ? k + 1 : s0)]; cnt[{std::min(a, b), std::max(a, b)}]++; } }
    for (const auto& [ed, n] : cnt) if (n == 1 && size_t(ed.first) < pos.size() && size_t(ed.second) < pos.size()) seg(pos[size_t(ed.first)], pos[size_t(ed.second)], pvHoverC_); }
  if (const Item* it = scene_->item(pvTs.mesh); it && it->poly) { const auto B = bsp(*it); const auto& pos = it->poly->pos;
    for (auto [f, c] : {std::pair{pvTs.red, QColor(0xff, 0x40, 0x40)}, std::pair{pvTs.blue, QColor(0x50, 0x80, 0xff)}}) if (f >= 0 && f < B->faceCount()) {
      const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1];
      for (int k = s; k < e; k++) { const int a = B->faceVerts[size_t(k)], b = B->faceVerts[size_t(k + 1 < e ? k + 1 : s)]; if (size_t(a) < pos.size() && size_t(b) < pos.size()) seg(pos[size_t(a)], pos[size_t(b)], c); } } }
}

// ---------- the topological symmetry ----------
// Two neighbouring polygons across the model's middle line are marked — the red one on one side, the blue one on the other; from them the
// mesh is walked on both sides at once, polygon by polygon through the edges: each red polygon gets its blue twin, corner by corner —
// whatever the pose and however uneven the two halves are, as long as their topology is the same
QString Viewport::pvTsPick(const QPointF& m, bool blue) {
  const PvFace h = pvFaceAt(m); if (h.mesh < 0) return tr("Point at a polygon of the model");
  if (h.mesh != pvTs.mesh) pvTs = PvTopoSym{h.mesh, -1, -1};
  (blue ? pvTs.blue : pvTs.red) = h.face; if (pvTs.red == pvTs.blue) (blue ? pvTs.red : pvTs.blue) = -1;
  overlayRev_++; requestUpdate(); emit paintVisChanged();
  if (pvTs.red < 0 || pvTs.blue < 0) return blue ? tr("The blue polygon marked — now the red one: its neighbour across the middle line, or the same polygon of an alike part") : tr("The red polygon marked — now the blue one (Ctrl + click): its neighbour across the middle line, or the same polygon of an alike part");
  QString err; const auto prs = pvTsPairs(scene_->paintView.set, &err);
  return prs.empty() ? err : tr("Pairs found: %1 polygons of each side").arg(prs.size()) + (pvTsOdd_ ? tr(" — but the two sides differ in many places: check that the marks are on the middle line, or on the same polygon of two alike parts") : QString());
}
std::vector<Viewport::PvTsPair> Viewport::pvTsPairs(const QString& set, QString* err) const {
  auto fail = [&](const QString& s) { if (err) *err = s; return std::vector<PvTsPair>{}; };
  const Item* it = scene_->item(pvTs.mesh); const auto Bs = it ? bsp(*it) : nullptr;
  if (!Bs || pvTs.red < 0 || pvTs.blue < 0) return fail(tr("Mark a red and a blue polygon first: two neighbours across the model's middle line"));
  const PolyMesh& B = *Bs; const int nf = B.faceCount(); if (pvTs.red >= nf || pvTs.blue >= nf) return fail(tr("The marks are of another mesh — mark them again"));
  if (B.cornerUV.size() != B.faceVerts.size() || B.cornerUV.empty()) return fail(tr("%1 has no UVs").arg(it->name));
  int ks = -1; if (!set.isEmpty()) { for (size_t i = 0; i < B.uvSetNames.size(); i++) if (B.uvSetNames[i] == set) ks = int(i); if (ks < 0) return fail(tr("%1 is not in the UV set %2").arg(it->name, set)); }
  auto ekey = [](int a, int b) { return (quint64(quint32(std::min(a, b))) << 32) | quint32(std::max(a, b)); };
  std::unordered_map<quint64, std::array<int, 2>> em; em.reserve(B.faceVerts.size());
  auto corner = [&](int f, int i) { const int s = B.faceStart[size_t(f)], n = B.faceStart[size_t(f) + 1] - s; return B.faceVerts[size_t(s + ((i % n) + n) % n)]; };
  auto size = [&](int f) { return B.faceStart[size_t(f) + 1] - B.faceStart[size_t(f)]; };
  for (int f = 0; f < nf; f++) for (int i = 0; i < size(f); i++) { auto& e = em.try_emplace(ekey(corner(f, i), corner(f, i + 1)), std::array<int, 2>{-1, -1}).first->second; if (e[0] < 0) e[0] = f; else if (e[1] < 0) e[1] = f; else e[1] = -2; }   // (−2: more than two polygons on the edge — not walked through)
  auto across = [&](int f, int a, int b) { const auto q = em.find(ekey(a, b)); if (q == em.end() || q->second[1] < 0) return -1; return q->second[0] == f ? q->second[1] : q->second[1] == f ? q->second[0] : -1; };
  auto indexOf = [&](int f, int v) { for (int i = 0; i < size(f); i++) if (corner(f, i) == v) return i; return -1; };
  const int fr = pvTs.red, fb = pvTs.blue, n0 = size(fr); if (size(fb) != n0) return fail(tr("The red and the blue polygon have a different number of corners"));
  if (fr == fb) return fail(tr("The red and the blue mark are on the same polygon"));
  // the walk from a seed — the red polygon's corners told which of the blue one's they are: pair after pair across the edges
  struct Pr { int r, b; std::vector<int> map; };
  struct Walk { std::vector<Pr> prs; int st[5] = {0, 0, 0, 0, 0}; double err = 0; };
  auto walk = [&](const std::vector<int>& seed) {
    Walk w; std::vector<Pr>& prs = w.prs; int* st = w.st; std::vector<char> usedR(size_t(nf), 0), usedB(size_t(nf), 0);
    prs.push_back({fr, fb, seed}); usedR[size_t(fr)] = usedB[size_t(fb)] = 1;
    for (size_t q = 0; q < prs.size(); q++) { const Pr p = prs[q]; const int n = size(p.r);
      for (int k = 0; k < n; k++) { const int ru = corner(p.r, k), rv = corner(p.r, k + 1), bu = corner(p.b, p.map[size_t(k)]), bv = corner(p.b, p.map[size_t((k + 1) % n)]);
        const int nr = across(p.r, ru, rv), nb = across(p.b, bu, bv);
        if (nr < 0 || nb < 0) { st[0]++; continue; } if (nr == nb) { st[1]++; continue; } if (usedR[size_t(nr)] && usedB[size_t(nb)]) { st[2]++; continue; }
        if (usedR[size_t(nr)] || usedB[size_t(nb)] || usedB[size_t(nr)] || usedR[size_t(nb)]) { st[3]++; continue; } if (size(nr) != size(nb)) { st[4]++; continue; }
        const int m = size(nr), a = indexOf(nr, ru), a2 = indexOf(nr, rv), c = indexOf(nb, bu), c2 = indexOf(nb, bv); if (a < 0 || a2 < 0 || c < 0 || c2 < 0) continue;
        const int da = ((a2 - a) % m + m) % m == 1 ? 1 : -1, dc = ((c2 - c) % m + m) % m == 1 ? 1 : -1;
        Pr x; x.r = nr; x.b = nb; x.map.resize(size_t(m)); for (int t = 0; t < m; t++) x.map[size_t(((a + t * da) % m + m) % m)] = ((c + t * dc) % m + m) % m;
        usedR[size_t(nr)] = usedB[size_t(nb)] = 1; prs.push_back(std::move(x)); } }
    // (how alike the two sides are in shape: the distances between paired points — the same on both sides whether one is a copy of
    //  the other, turned, moved or mirrored. 0 — the same shape)
    const size_t stepN = std::max<size_t>(1, prs.size() / 160); double sum = 0, dif = 0; Vec3 pr0, pb0; bool have = false;
    for (size_t i = 0; i < prs.size(); i += stepN) { const Vec3 a = B.pos[size_t(corner(prs[i].r, 0))], b = B.pos[size_t(corner(prs[i].b, prs[i].map[0]))];
      if (have) { const double dr = a.distanceTo(pr0), db = b.distanceTo(pb0); sum += dr; dif += std::abs(dr - db); } pr0 = a; pb0 = b; have = true; }
    w.err = sum > 1e-12 ? dif / sum : 0; return w; };
  // the seed. Two neighbours: the edge they share — its ends are their own twins (the two halves of one part, across its middle line).
  // Two polygons apart: the same polygon of two alike parts — a copy of a part, or its mirror copy: every way the red one's corners
  // can lie on the blue one's is walked, and the way that pairs the most polygons into the most alike shape is taken
  std::vector<int> seed; for (int i = 0; i < n0 && seed.empty(); i++) { const int u = corner(fr, i), v = corner(fr, i + 1); if (across(fr, u, v) != fb) continue;
    const int ju = indexOf(fb, u), jv = indexOf(fb, v); if (ju < 0 || jv < 0) continue; const int d = ((jv - ju) % n0 + n0) % n0 == 1 ? 1 : -1;
    seed.resize(size_t(n0)); for (int t = 0; t < n0; t++) seed[size_t(((i + t) % n0 + n0) % n0)] = ((ju + t * d) % n0 + n0) % n0; }
  Walk best; bool apart = seed.empty();
  if (!apart) best = walk(seed);
  else { bool any = false;
    for (int d : {-1, 1}) { if ((pvTsWay == 1 && d != -1) || (pvTsWay == 2 && d != 1)) continue;   // (−1: mirrored — the corners run the other way round; 1: the same way round)
      for (int r = 0; r < n0; r++) { std::vector<int> s(static_cast<size_t>(n0)); for (int t = 0; t < n0; t++) s[size_t(t)] = ((r + t * d) % n0 + n0) % n0;
        Walk w = walk(s); const double score = double(w.prs.size()) * (1 - std::min(1.0, w.err * 4)), bs = any ? double(best.prs.size()) * (1 - std::min(1.0, best.err * 4)) : -1;
        if (!any || score > bs) { best = std::move(w); any = true; } } }
    if (!any || best.prs.size() < 2) return fail(tr("The two marked polygons are not neighbours and no alike parts were found round them — mark two neighbours across the middle line, or the same polygon of two alike parts")); }
  const std::vector<Pr>& prs = best.prs; const int* st = best.st;
  pvTsOdd_ = apart ? best.err > 0.08 : (st[3] + st[4]) * 4 > int(prs.size());   // (the two sides differ in many places: the marked edge is hardly on the middle line — or the two parts are not alike)
  pvTsStat_ = QString("%8pairs %1 of %2 polygons · stops: border %3, same %4, done %5, clash %6, corners %7 · shape difference %9").arg(prs.size()).arg(nf).arg(st[0]).arg(st[1]).arg(st[2]).arg(st[3]).arg(st[4]).arg(apart ? "two parts · " : "").arg(best.err, 0, 'f', 3);
  // the pairs in the texture
  const bool hasOwn = ks >= 0 && size_t(ks) < B.setUV.size() && B.setUV[size_t(ks)].size() == B.faceVerts.size();
  auto inSet = [&](int f) { return ks < 0 || (size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> ks & 1u)); };
  auto uvOf = [&](int f) { const bool own = hasOwn && !(size_t(f) < B.faceLive.size() && B.faceLive[size_t(f)] == ks); const auto& UV = own ? B.setUV[size_t(ks)] : B.cornerUV;
    const int s = B.faceStart[size_t(f)], n = size(f); double cu = 0, cv = 0; for (int i = 0; i < n; i++) { cu += UV[size_t(s + i)][0]; cv += UV[size_t(s + i)][1]; } const double ou = std::floor(cu / n), ov = std::floor(cv / n);
    std::vector<QPointF> o; for (int i = 0; i < n; i++) { const double u = UV[size_t(s + i)][0] - ou, v = UV[size_t(s + i)][1] - ov; o.push_back(QPointF(u, B.flipY ? 1 - v : v)); } return o; };
  std::vector<PvTsPair> out;
  for (const Pr& p : prs) { if (!inSet(p.r) || !inSet(p.b)) continue; PvTsPair x; x.red = uvOf(p.r); const std::vector<QPointF> b = uvOf(p.b); for (size_t i = 0; i < x.red.size(); i++) x.blue.push_back(b[size_t(p.map[i])]); out.push_back(std::move(x)); }
  if (out.empty()) return fail(tr("None of the paired polygons is in the UV set painted"));
  return out;
}

}  // namespace hm

// ======================= what the camera sees of the room's meshes =======================
namespace hm {

namespace {
// a mesh's triangles in a tree to cast rays at (made once per geometry)
// (own: the mesh's own polygons — the first of the shown ones; a symmetry's copies after them lie where the other half is and would
//  hide it from every ray)
const topo::Bvh& pvBvhOf(const std::shared_ptr<PolyMesh>& sp, int own = -1) {
  static std::map<const PolyMesh*, std::pair<std::weak_ptr<PolyMesh>, std::shared_ptr<topo::Bvh>>> cache;
  for (auto c = cache.begin(); c != cache.end();) { if (c->second.first.expired()) c = cache.erase(c); else ++c; }
  auto f = cache.find(sp.get()); if (f != cache.end() && f->second.first.lock() == sp) return *f->second.second;
  std::vector<uint32_t> t; const PolyMesh& p = *sp;
  const int nfOwn = own > 0 && own < p.faceCount() ? own : p.faceCount();
  for (int fi = 0; fi < nfOwn; fi++) { const int s = p.faceStart[size_t(fi)], e = p.faceStart[size_t(fi) + 1]; for (int k = s + 1; k < e - 1; k++) { t.push_back(uint32_t(p.faceVerts[size_t(s)])); t.push_back(uint32_t(p.faceVerts[size_t(k)])); t.push_back(uint32_t(p.faceVerts[size_t(k + 1)])); } }
  auto b = std::make_shared<topo::Bvh>(); b->build(p.pos, t);
  return *(cache[sp.get()] = {sp, b}).second;
}
}  // namespace

void Viewport::resetView() { cam_.reset(); overlayRev_++; requestUpdate(); emit viewChanged(cam_.view); }

// the polygons of the room's meshes whose middle is seen by the camera inside a rectangle of the window (not hidden, nothing in front):
// each with its corners on the screen and — a mesh of the UV set — in the texture (0…1)
// (mirrors: with Symmetry on — also the polygons whose mirror image the camera sees there, each with the corners of that image on the
//  screen: what a stroke on one side lays on the other)
const std::vector<Affine>& Viewport::pvSymT(const Item& it) const {
  struct C { const PolyMesh* base = nullptr; QByteArray key; std::vector<Affine> T; }; static std::map<int, C> cache;
  const std::shared_ptr<PolyMesh> B = bsp(it); C& c = cache[it.id]; const QByteArray key = QJsonDocument(symo.toJson()).toJson(QJsonDocument::Compact);
  if (c.base != B.get() || c.key != key) { c.base = B.get(); c.key = key; c.T = symSet(it.id, B->pos).T; }
  return c.T;
}
std::vector<Viewport::PvProj> Viewport::pvFacesIn(const QRectF& box, const QString& set, bool needUv, bool mirrors, bool hiddenToo) const {
  std::vector<PvProj> out; const Projector P = projector(); int dbg[4] = {0, 0, 0, 0}; std::map<QString, int> occBy;
  struct M { const Item* it; std::shared_ptr<PolyMesh> B; const topo::Bvh* bvh; }; std::vector<M> ms;
  for (int id : pvMeshes()) { const Item* it = scene_->item(id); if (!it->poly || !scene_->isVisible(*it)) continue; const auto B0 = bsp(*it); int own = -1;
    if (it->poly != B0 && it->base) { const std::vector<char> h = meshmods::hiddenFaces(*it, *B0); int n = 0; for (int f = 0; f < B0->faceCount(); f++) n += h.empty() || !h[size_t(f)]; if (n > 0 && it->poly->faceCount() > n && it->poly->faceCount() % n == 0) own = n; }
    ms.push_back({it, B0, &pvBvhOf(it->poly, own)}); }
  for (const M& m : ms) {
    const PolyMesh& B = *m.B; const auto& pos = m.it->poly->pos; if (pos.size() < B.pos.size()) continue;
    const bool hasUv = m.it->type == ItemType::IMesh && B.cornerUV.size() == B.faceVerts.size() && !B.cornerUV.empty();
    int ks = -1; bool inSetMesh = hasUv; if (hasUv && !set.isEmpty()) { for (size_t i = 0; i < B.uvSetNames.size(); i++) if (B.uvSetNames[i] == set) ks = int(i); inSetMesh = ks >= 0; }
    if (needUv && !inSetMesh) continue;
    const bool hasOwn = ks >= 0 && size_t(ks) < B.setUV.size() && B.setUV[size_t(ks)].size() == B.faceVerts.size();
    const std::vector<char> hid = pvVec(*m.it, "pHid", "pHidF"); const double size = std::max(1e-9, B.bounds().size().length());
    std::vector<Affine> Ts(1);                                      // (as it is; then each mirror of the symmetry — the mesh carried back by it)
    if (mirrors && tset.sym && m.it->type == ItemType::IMesh) { const bool moved = m.it->poly != m.B; const Affine X = moved ? meshmods::meshMatrix(*scene_, *m.it) : Affine(), Xi = X.inverse();
      for (const Affine& T : pvSymT(*m.it)) Ts.push_back(Affine::mul(X, Affine::mul(T.inverse(), Xi))); }
    for (size_t ti = 0; ti < Ts.size(); ti++) { const Affine& T = Ts[ti];
    for (int f = 0; f < B.faceCount(); f++) {
      if (hid[size_t(f)]) { dbg[0]++; continue; } const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; if (e - s < 3) continue;
      if (needUv && ks >= 0 && !(size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> ks & 1u))) { dbg[1]++; continue; }
      Vec3 c; for (int k = s; k < e; k++) c += pos[size_t(B.faceVerts[size_t(k)])]; c = c * (1.0 / (e - s)); if (ti) c = T.apply(c);
      const ScreenPt sc = P.toScreen(c); if (sc.z >= 1 || !box.contains(QPointF(sc.x, sc.y))) { dbg[2]++; continue; }
      const Ray R = ray(QPointF(sc.x, sc.y)); const double tc = (c - R.o).dot(R.d), eps = size * 2e-4; bool hidden = false;
      if (!hiddenToo) for (const M& o : ms) if (o.bvh->ray(R.o, R.d, tc - eps)) { hidden = true; occBy[o.it->name + (ti ? "/m" : "")]++; break; }   // (something nearer on the way to its middle)
      if (hidden) { dbg[3]++; continue; }
      PvProj p; p.mesh = m.it->id; p.face = f; p.mirror = int(ti);
      for (int k = s; k < e; k++) { const Vec3& v = pos[size_t(B.faceVerts[size_t(k)])]; const ScreenPt q = P.toScreen(ti ? T.apply(v) : v); p.scr.push_back(QPointF(q.x, q.y)); }
      if (needUv) { const bool own = hasOwn && !(size_t(f) < B.faceLive.size() && B.faceLive[size_t(f)] == ks); const auto& UV = own ? B.setUV[size_t(ks)] : B.cornerUV;
        double cu = 0, cv = 0; for (int k = s; k < e; k++) { cu += UV[size_t(k)][0]; cv += UV[size_t(k)][1]; } const double ou = std::floor(cu / (e - s)), ov = std::floor(cv / (e - s));
        for (int k = s; k < e; k++) { const double u = UV[size_t(k)][0] - ou, v = UV[size_t(k)][1] - ov; p.uv.push_back(QPointF(u, B.flipY ? 1 - v : v)); } }
      out.push_back(std::move(p));
    } }
  }
  pvFacesWhy_ = QString("hidden %1 otherset %2 offbox %3 occluded %4 [").arg(dbg[0]).arg(dbg[1]).arg(dbg[2]).arg(dbg[3]); for (const M& m : ms) pvFacesWhy_ += QString("%1:%2/%3 ").arg(m.it->name).arg(m.it->poly->faceCount()).arg(m.bvh->triCount()); pvFacesWhy_ += "] by:"; for (auto& [n, c] : occBy) pvFacesWhy_ += QString(" %1=%2").arg(n).arg(c);
  return out;
}
// ---- the camera's view for the projected brush (Viewport::PvView) ----
bool Viewport::PvView::seen(int face, const QPointF& sp, float z) const {
  const int x = int(std::floor(sp.x())), y = int(std::floor(sp.y())); if (x < 0 || y < 0 || x >= W || y >= H) return false;
  // (its own polygon there, or beside there — the edges of a polygon fall between pixels; else by the depth: nothing nearer within a pixel round it)
  float far = -1e30f;
  for (int dy = -1; dy <= 1; dy++) { const int yy = y + dy; if (yy < 0 || yy >= H) continue;
    for (int dx = -1; dx <= 1; dx++) { const int xx = x + dx; if (xx < 0 || xx >= W) continue; const size_t i = size_t(yy) * W + xx; if (id[i] == face) return true; far = std::max(far, id[i] == -1 ? 1e30f : depth[i]); } }
  return z <= far + eps;
}
float Viewport::PvView::frontAt(const QPointF& sp) const {
  const int x = int(std::floor(sp.x())), y = int(std::floor(sp.y())); float n = 1e30f;
  for (int dy = -1; dy <= 1; dy++) { const int yy = y + dy; if (yy < 0 || yy >= H) continue;
    for (int dx = -1; dx <= 1; dx++) { const int xx = x + dx; if (xx < 0 || xx >= W) continue; n = std::min(n, depth[size_t(yy) * W + xx]); } }
  return n;
}
void Viewport::PvView::facesIn(const QRect& r, std::vector<int>& out) const {
  out.clear(); if (gw <= 0 || r.isEmpty()) return;
  const int x0 = std::clamp(r.left() / cell, 0, gw - 1), x1 = std::clamp(r.right() / cell, 0, gw - 1), y0 = std::clamp(r.top() / cell, 0, gh - 1), y1 = std::clamp(r.bottom() / cell, 0, gh - 1);
  for (int gy = y0; gy <= y1; gy++) for (int gx = x0; gx <= x1; gx++) { const auto& c = grid[size_t(gy) * gw + gx]; out.insert(out.end(), c.begin(), c.end()); }
  std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
}
QByteArray Viewport::pvViewKey() const {
  const Projector P = projector(); const bool single = scene_->paintView.set.isEmpty();
  QByteArray key(reinterpret_cast<const char*>(P.vp.constData()), int(16 * sizeof(float))); key += QByteArray::number(paneW()) + "x" + QByteArray::number(paneH()) + "|" + QByteArray::number(pvKeyNow()) + "|" + QByteArray::number(staticRev_) + (single ? "|1" : "|n");
  if (tset.sym) key += "|sym" + QJsonDocument(symo.toJson()).toJson(QJsonDocument::Compact);
  return key;
}
const Viewport::PvView& Viewport::pvView() const {
  static PvView V; const Projector P = projector(); const bool mirrors = tset.sym; const bool single = scene_->paintView.set.isEmpty();
  const QByteArray key = pvViewKey();
  if (key == V.key) return V;
  QElapsedTimer pvT; pvT.start();
  V = PvView(); V.key = key; V.W = paneW(); V.H = paneH(); V.depth.assign(size_t(V.W) * V.H, 1e30f); V.id.assign(size_t(V.W) * V.H, -1);
  V.gw = (V.W + V.cell - 1) / V.cell; V.gh = (V.H + V.cell - 1) / V.cell; V.grid.assign(size_t(V.gw) * V.gh, {});
  const Vec3 eye = cam_.pos, dir = cam_.dir(); double sizeAll = 0;
  auto setIdx = [&](const QString& n) { for (size_t i = 0; i < V.sets.size(); i++) if (V.sets[i] == n) return int(i); V.sets.push_back(n); return int(V.sets.size()) - 1; };
  // (a triangle into the depth picture: the nearest wins; which — the polygon's number, or −2 for what can't be painted)
  auto raster = [&](const QPointF& a, const QPointF& b, const QPointF& c, float za, float zb, float zc, int which) {
    const double den = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y()); if (std::abs(den) < 1e-9) return;
    const int x0 = std::max(0, int(std::floor(std::min({a.x(), b.x(), c.x()})))), x1 = std::min(V.W - 1, int(std::ceil(std::max({a.x(), b.x(), c.x()}))));
    const int y0 = std::max(0, int(std::floor(std::min({a.y(), b.y(), c.y()})))), y1 = std::min(V.H - 1, int(std::ceil(std::max({a.y(), b.y(), c.y()}))));
    for (int y = y0; y <= y1; y++) { const double py = y + 0.5;
      for (int x = x0; x <= x1; x++) { const double px = x + 0.5;
        const double w0 = ((b.y() - c.y()) * (px - c.x()) + (c.x() - b.x()) * (py - c.y())) / den, w1 = ((c.y() - a.y()) * (px - c.x()) + (a.x() - c.x()) * (py - c.y())) / den, w2 = 1 - w0 - w1;
        if (w0 < -1e-6 || w1 < -1e-6 || w2 < -1e-6) continue; const float z = float(za * w0 + zb * w1 + zc * w2); const size_t i = size_t(y) * V.W + x;
        if (z < V.depth[i]) { V.depth[i] = z; V.id[i] = which; } } } };
  for (int id : pvMeshes()) { const Item* it = scene_->item(id); if (!it->poly || !scene_->isVisible(*it)) continue;
    const std::shared_ptr<PolyMesh> Bp = bsp(*it); const PolyMesh& B = *Bp; const auto& pos = it->poly->pos; if (pos.size() < B.pos.size()) continue;
    const bool hasUv = it->type == ItemType::IMesh && B.cornerUV.size() == B.faceVerts.size() && !B.cornerUV.empty();
    const std::vector<char> hid = pvVec(*it, "pHid", "pHidF"); sizeAll = std::max(sizeAll, B.bounds().size().length());
    std::vector<Affine> Ts(1);
    if (mirrors && it->type == ItemType::IMesh) { const bool moved = it->poly != Bp; const Affine X = moved ? meshmods::meshMatrix(*scene_, *it) : Affine(), Xi = X.inverse();
      for (const Affine& T : pvSymT(*it)) Ts.push_back(Affine::mul(X, Affine::mul(T.inverse(), Xi))); }
    for (size_t ti = 0; ti < Ts.size(); ti++) { const Affine& T = Ts[ti];
      for (int f = 0; f < B.faceCount(); f++) {
        if (hid[size_t(f)]) continue; const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; if (e - s < 3) continue;
        PvView::F F; F.mesh = it->id; F.face = f; F.mirror = int(ti); bool off = false; double mnx = 1e30, mxx = -1e30, mny = 1e30, mxy = -1e30;
        Vec3 nrm, cen, prevV; bool first = true; Vec3 firstV;      // (Newell's normal and the middle: how squarely the camera sees it)
        for (int k = s; k < e; k++) { Vec3 v = pos[size_t(B.faceVerts[size_t(k)])]; if (ti) v = T.apply(v); const ScreenPt q = P.toScreen(v); if (q.z >= 1) { off = true; break; }
          if (first) { firstV = v; first = false; } else { nrm.x += (prevV.y - v.y) * (prevV.z + v.z); nrm.y += (prevV.z - v.z) * (prevV.x + v.x); nrm.z += (prevV.x - v.x) * (prevV.y + v.y); } prevV = v; cen += v;
          F.scr.push_back(QPointF(q.x, q.y)); F.z.push_back(float((v - eye).dot(dir))); mnx = std::min(mnx, q.x); mxx = std::max(mxx, q.x); mny = std::min(mny, q.y); mxy = std::max(mxy, q.y); }
        if (!off && !first) { nrm.x += (prevV.y - firstV.y) * (prevV.z + firstV.z); nrm.y += (prevV.z - firstV.z) * (prevV.x + firstV.x); nrm.z += (prevV.x - firstV.x) * (prevV.y + firstV.y);
          cen = cen * (1.0 / (e - s)); const Vec3 toEye = cam_.ortho() ? dir : eye - cen; const double ln = nrm.length(), le = toEye.length();
          F.facing = ln > 1e-12 && le > 1e-12 ? float(std::abs(nrm.dot(toEye)) / (ln * le)) : 1.f; }
        if (off || mxx < 0 || mxy < 0 || mnx >= V.W || mny >= V.H) continue;
        // (its UV: in the project's single texture as it is; else in the first UV set it belongs to — that set's own where the polygon shows another's)
        int ks = -1; bool paintable = hasUv;
        if (paintable && !single) { const auto bits = size_t(f) < B.faceSets.size() ? B.faceSets[size_t(f)] : 0; for (size_t k = 0; k < B.uvSetNames.size() && ks < 0; k++) if (bits >> k & 1u) ks = int(k); if (ks < 0) paintable = false; }
        int which = -2;
        if (paintable) { const bool own = ks >= 0 && size_t(ks) < B.setUV.size() && B.setUV[size_t(ks)].size() == B.faceVerts.size() && !(size_t(f) < B.faceLive.size() && B.faceLive[size_t(f)] == ks);
          const auto& UV = own ? B.setUV[size_t(ks)] : B.cornerUV; double cu = 0, cv = 0; for (int k = s; k < e; k++) { cu += UV[size_t(k)][0]; cv += UV[size_t(k)][1]; } const double ou = std::floor(cu / (e - s)), ov = std::floor(cv / (e - s));
          for (int k = s; k < e; k++) { const double u = UV[size_t(k)][0] - ou, v = UV[size_t(k)][1] - ov; F.uv.push_back(QPointF(u, B.flipY ? 1 - v : v)); }
          F.set = ks >= 0 ? setIdx(B.uvSetNames[size_t(ks)]) : -1; F.box = QRectF(QPointF(mnx, mny), QPointF(mxx, mxy)); which = int(V.faces.size()); }
        // (the mesh itself is what hides and is seen; a mirror's image lies where the other half is — judged against that half's depth)
        if (ti == 0) for (size_t i = 1; i + 1 < F.scr.size(); i++) raster(F.scr[0], F.scr[i], F.scr[i + 1], F.z[0], F.z[i], F.z[i + 1], which);
        if (which < 0) continue;
        const int gx0 = std::clamp(int(mnx) / V.cell, 0, V.gw - 1), gx1 = std::clamp(int(mxx) / V.cell, 0, V.gw - 1), gy0 = std::clamp(int(mny) / V.cell, 0, V.gh - 1), gy1 = std::clamp(int(mxy) / V.cell, 0, V.gh - 1);
        for (int gy = gy0; gy <= gy1; gy++) for (int gx = gx0; gx <= gx1; gx++) V.grid[size_t(gy) * V.gw + gx].push_back(which);
        V.faces.push_back(std::move(F));
      } }
  }
  V.eps = float(std::max(1e-6, sizeAll * 4e-4)); V.ms = int(pvT.elapsed());
  return V;
}

// the polygons seen inside an outline of the window selected (op: 0 — they alone, 1 — added, 2 — taken off)
void Viewport::pvSelArea(const QRectF& box, const std::function<bool(const QPointF&)>& inside, int op) {
  if (op == 0) pvSel.clear();
  std::map<int, std::vector<int>> hit;
  for (const PvProj& p : pvFacesIn(box, QString(), false)) { QPointF c; for (const QPointF& q : p.scr) c += q; c /= double(p.scr.size()); if (!inside(c)) continue; hit[p.mesh].push_back(p.face); }
  if (tset.sym) { std::map<int, std::vector<int>> tw; for (const auto& [m, fs] : hit) for (const auto& [mm, ff] : pvMirrored(m, fs)) { auto& v = tw[mm]; v.insert(v.end(), ff.begin(), ff.end()); }   // (the symmetry: the twins too)
    for (const auto& [mm, ff] : tw) { auto& v = hit[mm]; v.insert(v.end(), ff.begin(), ff.end()); } }
  for (const auto& [m, fs] : hit) for (int fc : fs) {
    if (op == 2) { auto f = pvSel.find(m); if (f != pvSel.end()) { f->second.erase(fc); if (f->second.empty()) pvSel.erase(f); } } else pvSel[m].insert(fc); }
  pvSelRev_++; overlayRev_++; requestUpdate(); emit paintVisChanged();
}

// the surface a point of the texture lies on (0…1 across and down): a mesh of the UV set painted, the point and the normal there —
// for the symmetry of a stroke made on the texture itself. The polygons are looked up in a grid over the texture, made once
std::optional<RayHit> Viewport::paintHitOfUv(const QPointF& uv) const {
  struct Tri { int item; QPointF a, b, c; int ia, ib, ic; }; struct Idx { qint64 key = -1; QString set; int G = 96; std::vector<std::vector<int>> cell; std::vector<Tri> tris; };
  static Idx X; const QString& set = scene_->paintView.set; const qint64 key = pvKeyNow() * 31 + staticRev_;
  if (X.key != key || X.set != set) {
    X.key = key; X.set = set; X.tris.clear(); X.cell.assign(size_t(X.G * X.G), {});
    for (int id : pvMeshes()) { const Item* it = scene_->item(id); if (!it->poly || it->type != ItemType::IMesh || !scene_->isVisible(*it)) continue; const PolyMesh& Pm = *it->poly;
      if (Pm.cornerUV.size() != Pm.faceVerts.size() || Pm.cornerUV.empty()) continue;
      int ks = -1; if (!set.isEmpty()) { for (size_t i = 0; i < Pm.uvSetNames.size(); i++) if (Pm.uvSetNames[i] == set) ks = int(i); if (ks < 0) continue; }
      const bool hasOwn = ks >= 0 && size_t(ks) < Pm.setUV.size() && Pm.setUV[size_t(ks)].size() == Pm.faceVerts.size();
      for (int f = 0; f < Pm.faceCount(); f++) { if (ks >= 0 && !(size_t(f) < Pm.faceSets.size() && (Pm.faceSets[size_t(f)] >> ks & 1u))) continue;
        const bool own = hasOwn && !(size_t(f) < Pm.faceLive.size() && Pm.faceLive[size_t(f)] == ks); const auto& UV = own ? Pm.setUV[size_t(ks)] : Pm.cornerUV;
        const int s = Pm.faceStart[size_t(f)], e = Pm.faceStart[size_t(f) + 1]; if (e - s < 3) continue;
        double cu = 0, cv = 0; for (int k = s; k < e; k++) { cu += UV[size_t(k)][0]; cv += UV[size_t(k)][1]; } const double ou = std::floor(cu / (e - s)), ov = std::floor(cv / (e - s));
        auto pt = [&](int k) { const double u = UV[size_t(k)][0] - ou, v = UV[size_t(k)][1] - ov; return QPointF(u, Pm.flipY ? 1 - v : v); };
        for (int k = s + 1; k + 1 < e; k++) { Tri t{id, pt(s), pt(k), pt(k + 1), Pm.faceVerts[size_t(s)], Pm.faceVerts[size_t(k)], Pm.faceVerts[size_t(k + 1)]}; const int ti = int(X.tris.size()); X.tris.push_back(t);
          const int x0 = std::clamp(int(std::floor(std::min({t.a.x(), t.b.x(), t.c.x()}) * X.G)), 0, X.G - 1), x1 = std::clamp(int(std::floor(std::max({t.a.x(), t.b.x(), t.c.x()}) * X.G)), 0, X.G - 1);
          const int y0 = std::clamp(int(std::floor(std::min({t.a.y(), t.b.y(), t.c.y()}) * X.G)), 0, X.G - 1), y1 = std::clamp(int(std::floor(std::max({t.a.y(), t.b.y(), t.c.y()}) * X.G)), 0, X.G - 1);
          for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) X.cell[size_t(y * X.G + x)].push_back(ti); } } }
  }
  const int cx = int(std::floor(uv.x() * X.G)), cy = int(std::floor(uv.y() * X.G)); if (cx < 0 || cy < 0 || cx >= X.G || cy >= X.G) return std::nullopt;
  for (int ti : X.cell[size_t(cy * X.G + cx)]) { const Tri& t = X.tris[size_t(ti)];
    const double den = (t.b.y() - t.c.y()) * (t.a.x() - t.c.x()) + (t.c.x() - t.b.x()) * (t.a.y() - t.c.y()); if (std::abs(den) < 1e-14) continue;
    const double w0 = ((t.b.y() - t.c.y()) * (uv.x() - t.c.x()) + (t.c.x() - t.b.x()) * (uv.y() - t.c.y())) / den, w1 = ((t.c.y() - t.a.y()) * (uv.x() - t.c.x()) + (t.a.x() - t.c.x()) * (uv.y() - t.c.y())) / den, w2 = 1 - w0 - w1;
    if (w0 < -1e-6 || w1 < -1e-6 || w2 < -1e-6) continue;
    const Item* it = scene_->item(t.item); if (!it || !it->poly) continue; const auto& pos = it->poly->pos; if (size_t(std::max({t.ia, t.ib, t.ic})) >= pos.size()) continue;
    const Vec3 a = pos[size_t(t.ia)], b = pos[size_t(t.ib)], c = pos[size_t(t.ic)]; Vec3 n = (b - a).cross(c - a); if (n.lengthSq() < 1e-30) continue;
    return RayHit{t.item, 0, a * w0 + b * w1 + c * w2, n.normalized(), -1}; }
  return std::nullopt;
}

}  // namespace hm

namespace hm {
// the surface under every texel of the UV set's texture at a size: its unit normal (smooth) and its place — for the material builder's
// light and rim. A texel no polygon covers takes its nearest covered neighbour's (a few texels out: no dark line along the shells' edges)
int Viewport::pvSetScalp(bool on) {
  int n = 0;
  for (const auto& [m, fs] : pvSel) { Item* it = scene_->item(m); if (!it) continue; std::vector<char> v = pvVec(*it, "pScalpAll", "pScalp");
    for (int f : fs) if (size_t(f) < v.size()) v[size_t(f)] = on; pvWrite(*it, "pScalpAll", "pScalp", v); }
  for (int id : pvMeshes()) if (const Item* it = scene_->item(id)) { const std::vector<char> v = pvVec(*it, "pScalpAll", "pScalp"); for (char c : v) n += c != 0; }
  pvTouch(); return n;
}
bool Viewport::pvHasScalp() const { for (int id : pvMeshes()) if (const Item* it = scene_->item(id)) if (!it->raw.value("pScalp").toString().isEmpty() || it->raw.value("pScalpAll").toBool()) return true; return false; }
QStringList Viewport::pvScalpSets() const {
  QStringList out;
  for (int id : pvMeshes()) { const Item* it = scene_->item(id); if (!it || !it->poly) continue; const std::vector<char> v = pvVec(*it, "pScalpAll", "pScalp"); const auto B = it->base ? it->base : it->poly;
    for (int f = 0; f < B->faceCount() && size_t(f) < v.size(); f++) { if (!v[size_t(f)]) continue; const auto bits = size_t(f) < B->faceSets.size() ? B->faceSets[size_t(f)] : 0u;
      for (size_t k = 0; k < B->uvSetNames.size(); k++) if ((bits >> k & 1u) && !out.contains(B->uvSetNames[k])) out << B->uvSetNames[k]; } }
  return out;
}
Box3 Viewport::pvSceneBox() const { Box3 b; for (int id : pvMeshes()) if (const Item* it = scene_->item(id); it && it->poly) b.unite(it->poly->bounds()); return b; }
double Viewport::pvSceneSize() const { Box3 b; for (int id : pvMeshes()) if (const Item* it = scene_->item(id); it && it->poly) b.unite(it->poly->bounds()); return b.empty() ? 1.0 : b.size().length(); }
QImage Viewport::pvScalpNear(const QString& set, int w, int h, double reach) const {
  if (w <= 0 || h <= 0 || reach <= 0) return {};
  // (the scalp's points: its vertices and its polygons' middles and edges' midpoints, in a grid of cells as big as the reach)
  std::vector<Vec3> pts;
  for (int id : pvMeshes()) { const Item* it = scene_->item(id); if (!it || !it->poly) continue; const std::vector<char> v = pvVec(*it, "pScalpAll", "pScalp"); const auto B = it->base ? it->base : it->poly; const auto& pos = it->poly->pos;
    if (pos.size() < B->pos.size()) continue;
    for (int f = 0; f < B->faceCount() && size_t(f) < v.size(); f++) { if (!v[size_t(f)]) continue; const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1]; Vec3 c;
      for (int k = s; k < e; k++) { const Vec3& a = pos[size_t(B->faceVerts[size_t(k)])], &b = pos[size_t(B->faceVerts[size_t(k + 1 < e ? k + 1 : s)])]; pts.push_back(a); pts.push_back((a + b) * 0.5); c += a; }
      pts.push_back(c * (1.0 / std::max(1, e - s))); } }
  if (pts.empty()) return {};
  const double cell = reach; auto key = [&](long long x, long long y, long long z) { return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
  std::unordered_map<long long, std::vector<int>> grid; for (size_t i = 0; i < pts.size(); i++) grid[key((long long)std::floor(pts[i].x / cell), (long long)std::floor(pts[i].y / cell), (long long)std::floor(pts[i].z / cell))].push_back(int(i));
  const PvGeom& G = pvBakeGeom(set, w, h); if (G.w != w || G.h != h) return {};
  QImage out(w, h, QImage::Format_Grayscale8); out.fill(0);
  auto rows = [&](int y0, int y1) { for (int y = y0; y < y1; y++) { uchar* o = out.scanLine(y);
      for (int x = 0; x < w; x++) { const size_t i = (size_t(y) * w + x) * 3; if (G.n[i] == 0 && G.n[i + 1] == 0 && G.n[i + 2] == 0) continue;
        const Vec3 p(G.p[i], G.p[i + 1], G.p[i + 2]); const long long cx = (long long)std::floor(p.x / cell), cy = (long long)std::floor(p.y / cell), cz = (long long)std::floor(p.z / cell); double best = reach * reach;
        for (int dz = -1; dz <= 1; dz++) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { const auto g = grid.find(key(cx + dx, cy + dy, cz + dz)); if (g == grid.end()) continue;
          for (int j : g->second) { const Vec3 d = pts[size_t(j)] - p; best = std::min(best, d.dot(d)); } }
        const double t = 1 - std::sqrt(best) / reach; if (t > 0) o[x] = uchar(std::clamp(t * t * 255 + 0.5, 0.0, 255.0)); } } };
  const int nt = std::clamp(int(std::thread::hardware_concurrency()), 1, 12); std::vector<std::thread> ts; const int step = (h + nt - 1) / nt;
  for (int k = 1; k < nt; k++) ts.emplace_back(rows, std::min(h, k * step), std::min(h, (k + 1) * step)); rows(0, std::min(h, step)); for (auto& t : ts) t.join();
  return out;
}

const Viewport::PvGeom& Viewport::pvBakeGeom(const QString& set, int w, int h) const {
  static PvGeom G; static qint64 key = -1; static QString kset;
  const qint64 k = (pvKeyNow() * 31 + staticRev_) * 4099 + w * 7 + h;
  if (k == key && kset == set && G.w == w && G.h == h) return G;
  key = k; kset = set; G.w = w; G.h = h; const size_t N = size_t(w) * size_t(h); G.n.assign(N * 3, 0.f); G.p.assign(N * 3, 0.f); std::vector<uchar> cov(N, 0);
  for (int id : pvMeshes()) { const Item* it = scene_->item(id); if (!it->poly || it->type != ItemType::IMesh || !scene_->isVisible(*it)) continue; const PolyMesh& Pm = *it->poly;
    if (Pm.cornerUV.size() != Pm.faceVerts.size() || Pm.cornerUV.empty()) continue;
    int ks = -1; if (!set.isEmpty()) { for (size_t i = 0; i < Pm.uvSetNames.size(); i++) if (Pm.uvSetNames[i] == set) ks = int(i); if (ks < 0) continue; }
    const bool hasOwn = ks >= 0 && size_t(ks) < Pm.setUV.size() && Pm.setUV[size_t(ks)].size() == Pm.faceVerts.size();
    std::vector<Vec3> vn(Pm.pos.size());                           // (smooth normals: the polygons' own, summed at the vertices by area)
    for (int f = 0; f < Pm.faceCount(); f++) { const int s0 = Pm.faceStart[size_t(f)], e0 = Pm.faceStart[size_t(f) + 1]; Vec3 nn;
      for (int q = s0 + 1; q + 1 < e0; q++) nn += (Pm.pos[size_t(Pm.faceVerts[size_t(q)])] - Pm.pos[size_t(Pm.faceVerts[size_t(s0)])]).cross(Pm.pos[size_t(Pm.faceVerts[size_t(q + 1)])] - Pm.pos[size_t(Pm.faceVerts[size_t(s0)])]);
      for (int q = s0; q < e0; q++) vn[size_t(Pm.faceVerts[size_t(q)])] += nn; }
    for (Vec3& v : vn) if (v.lengthSq() > 1e-30) v = v.normalized();
    for (int f = 0; f < Pm.faceCount(); f++) { if (ks >= 0 && !(size_t(f) < Pm.faceSets.size() && (Pm.faceSets[size_t(f)] >> ks & 1u))) continue;
      const bool own = hasOwn && !(size_t(f) < Pm.faceLive.size() && Pm.faceLive[size_t(f)] == ks); const auto& UV = own ? Pm.setUV[size_t(ks)] : Pm.cornerUV;
      const int s0 = Pm.faceStart[size_t(f)], e0 = Pm.faceStart[size_t(f) + 1]; if (e0 - s0 < 3) continue;
      double cu = 0, cv = 0; for (int q = s0; q < e0; q++) { cu += UV[size_t(q)][0]; cv += UV[size_t(q)][1]; } const double ou = std::floor(cu / (e0 - s0)), ov = std::floor(cv / (e0 - s0));
      auto pt = [&](int q) { const double u = UV[size_t(q)][0] - ou, v = UV[size_t(q)][1] - ov; return QPointF(u * w, (Pm.flipY ? 1 - v : v) * h); };
      for (int q = s0 + 1; q + 1 < e0; q++) { const QPointF d0 = pt(s0), d1 = pt(q), d2 = pt(q + 1); const int i0 = Pm.faceVerts[size_t(s0)], i1 = Pm.faceVerts[size_t(q)], i2 = Pm.faceVerts[size_t(q + 1)];
        const double den = (d1.y() - d2.y()) * (d0.x() - d2.x()) + (d2.x() - d1.x()) * (d0.y() - d2.y()); if (std::abs(den) < 1e-12) continue;
        const int x0 = std::max(0, int(std::floor(std::min({d0.x(), d1.x(), d2.x()})))), x1 = std::min(w - 1, int(std::ceil(std::max({d0.x(), d1.x(), d2.x()}))));
        const int y0 = std::max(0, int(std::floor(std::min({d0.y(), d1.y(), d2.y()})))), y1 = std::min(h - 1, int(std::ceil(std::max({d0.y(), d1.y(), d2.y()}))));
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) { const double px = x + 0.5, py = y + 0.5;
          const double w0 = ((d1.y() - d2.y()) * (px - d2.x()) + (d2.x() - d1.x()) * (py - d2.y())) / den, w1 = ((d2.y() - d0.y()) * (px - d2.x()) + (d0.x() - d2.x()) * (py - d2.y())) / den, w2 = 1 - w0 - w1;
          if (w0 < -0.02 || w1 < -0.02 || w2 < -0.02) continue;
          Vec3 nn = vn[size_t(i0)] * w0 + vn[size_t(i1)] * w1 + vn[size_t(i2)] * w2; if (nn.lengthSq() < 1e-30) continue; nn = nn.normalized();
          const Vec3 pp = Pm.pos[size_t(i0)] * w0 + Pm.pos[size_t(i1)] * w1 + Pm.pos[size_t(i2)] * w2; const size_t t = size_t(y) * size_t(w) + size_t(x);
          G.n[t * 3] = float(nn.x); G.n[t * 3 + 1] = float(nn.y); G.n[t * 3 + 2] = float(nn.z); G.p[t * 3] = float(pp.x); G.p[t * 3 + 1] = float(pp.y); G.p[t * 3 + 2] = float(pp.z); cov[t] = 1; } } } }
  for (int pass = 0; pass < 6; pass++) { std::vector<uchar> nc = cov;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { const size_t t = size_t(y) * size_t(w) + size_t(x); if (cov[t]) continue;
      for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) { const int xx = x + dx, yy = y + dy; if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue; const size_t s2 = size_t(yy) * size_t(w) + size_t(xx);
        if (cov[s2]) { for (int c = 0; c < 3; c++) { G.n[t * 3 + size_t(c)] = G.n[s2 * 3 + size_t(c)]; G.p[t * 3 + size_t(c)] = G.p[s2 * 3 + size_t(c)]; } nc[t] = 1; break; } } }
    cov.swap(nc); }
  return G;
}
}  // namespace hm

namespace hm {
// The Topology room's H over the model with nothing selected, as in the Paint room: hide — the part under the pointer (the polygons
// joined to the one pointed at), hideObj — the whole object, only / onlyObj — everything but that part / that object (the other
// objects through the room's isolation: their own eyes stay as they are), showAll — every hidden polygon, the objects hidden this way
// and the isolation back. false: not taken (components or polygroups are selected, nothing under the pointer) — the keys' older work
bool Viewport::tKeyUnder(const QString& op) {
  if (room() != "topo") return false;
  auto after = [&](const QString& msg) { pickCache_.clear(); staticRev_++; topoRev_++; allWalls_ = true; sceneChanged(false); emit sceneEdited(); emit topoState(); requestUpdate(); ui::toast(msg); };
  if (op == "showAll") {
    int n = 0; for (int id : tHidByKey_) if (Item* it = scene_->item(id); it && !it->visible) { it->visible = true; n++; } tHidByKey_.clear();
    for (auto& [id, it] : scene_->items) if (it.type == ItemType::IMesh && (it.raw.contains("hidF") || !it.raw.value("hidGroups").toArray().isEmpty())) { it.raw.remove("hidF"); it.raw.remove("hidGroups"); n++; }
    if (scene_->iso && !scene_->iso->solo) { scene_->iso.reset(); n++; } tcIso_.reset(); tgIso_.reset();
    after(n ? tr("Everything shown") : tr("Nothing was hidden")); return true;
  }
  if (cam_.view == "uv" || (topoMode != "object" && !tselMeshes().empty())) { tKeyWhy_ = "uv / components selected"; return false; }
  if (const int m = topoMesh(); m >= 0 && !smPickedGroups(m).empty()) { tKeyWhy_ = "polygroups picked"; return false; }
  const auto hit = pickSurface(mouse_); Item* it = hit ? scene_->item(hit->itemId) : nullptr;
  if (!it || it->type != ItemType::IMesh || !it->poly) { tKeyWhy_ = !hit ? QString("nothing under %1,%2").arg(mouse_.x()).arg(mouse_.y()) : QString("item %1 is not a mesh").arg(hit->itemId); return false; }
  const bool whole = op == "hideObj" || op == "onlyObj", only = op == "only" || op == "onlyObj"; const auto B = bsp(*it); const int nf = B->faceCount();
  std::vector<char> part;
  if (!whole) { const int pf = pvPolyFace(*hit, *it); const std::vector<int> map = polyToBase(*it); const int bf = pf < 0 ? -1 : map.empty() ? pf : size_t(pf) < map.size() ? map[size_t(pf)] : -1;
    if (bf < 0 || bf >= nf) return false;
    const auto& parts = partsOf(B); part.assign(size_t(nf), 0); const int p0 = parts[size_t(bf)]; for (int f = 0; f < nf; f++) part[size_t(f)] = parts[size_t(f)] == p0; }
  if (only) { Scene::Isolation iso; iso.keep.insert(it->id); for (int x : meshmods::modsOf(*scene_, it->id)) iso.keep.insert(x); scene_->iso = iso; }
  if (op == "hideObj") { it->visible = false; tHidByKey_.insert(it->id); after(tr("%1 hidden — Ctrl+X shows everything").arg(it->name)); return true; }
  if (op == "onlyObj") { after(tr("Only %1 shown — Ctrl+X shows everything").arg(it->name)); return true; }
  std::vector<char> h = it->raw.value("hidF").toString().isEmpty() ? std::vector<char>(size_t(nf), 0) : topo::smDec(it->raw.value("hidF").toString(), nf); if (int(h.size()) != nf) h.assign(size_t(nf), 0);
  int n = 0; for (int f = 0; f < nf; f++) { if (only) h[size_t(f)] = !part[size_t(f)]; else if (part[size_t(f)]) h[size_t(f)] = 1; n += part[size_t(f)]; }
  it->raw.insert("hidF", topo::smEnc(h));
  after(only ? tr("Only a part of %1 shown (%2 polygons) — Ctrl+X shows everything").arg(it->name).arg(n) : tr("A part of %1 hidden (%2 polygons) — Ctrl+X shows everything").arg(it->name).arg(n));
  return true;
}
}  // namespace hm

namespace hm {
// a brush sized on the screen: how many texels a screen pixel covers where the stroke is — the texture's place a few pixels to the
// right and below the pointer against the one under it (kept as it was where neither is on the same shell)
void Viewport::paintScaleUpdate(const QPointF& m, const QPointF& uv) {
  if (!paintSizeScreen || paintTexSize.isEmpty()) { paintScale_ = 1; return; }
  const double W = paintTexSize.width(), H = paintTexSize.height(), d = 5; std::vector<double> v;
  // (a probe counts only on the same surface: the same object, near the point under the brush in space, facing the same way. On a
  // thin lock the probes beside it land on the lock behind or the next one — far off in the texture: the brush swelled to a blot)
  // The triangle under the brush says what to expect: its texels per scene unit × the size of a pixel there (a surface seen face-on);
  // a probe across a UV seam of the same lock lands close in space but far off in the texture — more than a steep slant explains
  const auto hc = pickSurface(m); const double wpp = hc ? worldPerPixelAt(hc->point) : 0, near = d * wpp * 8;
  double dens = 0; if (hc) paintUvOfHit(*hc, &dens); const double ref = dens * wpp;
  for (const QPointF& o : {QPointF(d, 0), QPointF(0, d), QPointF(-d, 0), QPointF(0, -d)}) {
    std::optional<QPointF> u;
    if (hc) { const auto h = pickSurface(m + o); if (!h || h->itemId != hc->itemId || h->point.distanceTo(hc->point) > near || h->normal.dot(hc->normal) < 0.3) continue; u = paintUvOfHit(*h); }
    else u = paintUvAt(m + o);
    if (!u) continue;
    const double t = std::hypot((u->x() - uv.x()) * W, (u->y() - uv.y()) * H) / d; if (t > 1e-4 && t < 400 && (ref <= 0 || (t < ref * 5 && t > ref * 0.15))) v.push_back(t); }
  if (v.empty() && ref > 1e-4 && ref < 400) v.push_back(ref);   // (no probe on the same surface — a lock seen edge-on: the triangle's own density)
  // (a probe that lands across a UV seam is far away in the texture: the two nearest probes say how dense the texels are here — the
  // brush does not swell beside a seam)
  if (!v.empty()) { std::sort(v.begin(), v.end()); paintScale_ = float(v.size() == 1 ? v[0] : (v[0] + v[1]) / 2); }
}
double Viewport::paintRingRadiusPx() const { return std::max(1.5, (paintSizeScreen || paintProj ? paintRingPx : paintRingPx / std::max(1e-3f, paintScale_)) / 2.0); }
// the brush's ring over the model, at its size on the screen
void Viewport::paintRingOverlay(std::vector<Overlay>& out) const {
  if (paintRemote_ && !inViewport_) { std::vector<std::pair<double, double>> c2; const double rr = std::max(3.0, paintRemoteR_); for (int i = 0; i < 40; i++) { const double a = 6.283185307 * i / 40; c2.push_back({paintRemote_->x() + std::cos(a) * rr, paintRemote_->y() + std::sin(a) * rr}); }
    out.push_back({c2, QColor(80, 220, 255), true}); return; }   // (the pointer over the texture: only its place on the model)
  if (!paintRing || !inViewport_ || paintRingPx <= 0 || drag_ == Drag::Orbit || drag_ == Drag::Pan || drag_ == Drag::Dolly) return;
  const double r = paintRingRadiusPx();
  const QPointF at = paintAdj_ ? paintAdjAt_ : paintLast_;
  const QColor ringC = paintThrough ? QColor(255, 176, 48) : QColor(0xff, 0xff, 0xff);   // (Through: amber and doubled)
  auto screenRing = [&](double rr, const QColor& col) { std::vector<std::pair<double, double>> c; const int n = 48; for (int i = 0; i < n; i++) { const double a = 6.283185307 * i / n; c.push_back({at.x() + std::cos(a) * rr, at.y() + std::sin(a) * rr}); } out.push_back({c, col, true}); };
  // (the ring lies on the surface under the pointer: a circle of the brush's radius in the plane of the polygon there, as the
  // camera sees it — it shows how the surface turns; the reach on the screen stays as a dim circle round it. Off the model, while
  // the size is dragged — the flat ring)
  std::optional<RayHit> h; if (!paintAdj_ && cam_.view != "uv") h = pickSurface(at);
  bool laid = false;
  if (h) { Vec3 nrm = h->normal; const Vec3 toEye = cam_.ortho() ? cam_.dir() * -1.0 : (cam_.pos - h->point); if (nrm.dot(toEye) < 0) nrm = nrm * -1.0;
    const double nl = nrm.length(); if (nl > 1e-9) { nrm = nrm * (1 / nl);
      const Vec3 ref = std::abs(nrm.dot(cam_.upVec())) < 0.9 ? cam_.upVec() : cam_.right(); Vec3 u = ref - nrm * ref.dot(nrm); const double ul = u.length();
      if (ul > 1e-9) { u = u * (1 / ul); const Vec3 v = Vec3{nrm.y * u.z - nrm.z * u.y, nrm.z * u.x - nrm.x * u.z, nrm.x * u.y - nrm.y * u.x};
        const double R = r * worldPerPixelAt(h->point); const Projector P = projector(); const Vec3 lift = nrm * (R * 0.02);   // (a hair above the surface)
        auto surfRing = [&](double k, const QColor& col) { std::vector<std::pair<double, double>> c; const int n = 48;
          for (int i = 0; i < n; i++) { const double a = 6.283185307 * i / n; const ScreenPt q = P.toScreen(h->point + lift + (u * std::cos(a) + v * std::sin(a)) * (R * k)); if (q.z >= 1) return; c.push_back({q.x, q.y}); }
          out.push_back({c, col, true}); };
        surfRing(1, ringC); if (paintThrough) surfRing(0.8, ringC);
        screenRing(r, QColor(96, 96, 96)); laid = true; } } }
  if (!laid) { screenRing(r, ringC); if (paintThrough) screenRing(r * 0.8, ringC); }
  if ((paintDrag_ || paintProjDrag_) && steadyOn_) out.push_back({{{steady_.x(), steady_.y()}, {paintLast_.x(), paintLast_.y()}}, QColor(148, 102, 190), false});   // (Steady Stroke: the string)
  if (paintRemote_) { std::vector<std::pair<double, double>> c2; const double rr = std::max(3.0, paintRemoteR_); for (int i = 0; i < 40; i++) { const double a = 6.283185307 * i / 40; c2.push_back({paintRemote_->x() + std::cos(a) * rr, paintRemote_->y() + std::sin(a) * rr}); }
    out.push_back({c2, QColor(80, 220, 255), true}); }   // (the texture pointer's place on the model)
}

// the polygons a click means for the Fill tool and the like: the polygon, the part joined to it, its polygroup, the whole object
std::vector<int> Viewport::pvFacesOfKind(const PvFace& h, const QString& kind) const {
  const Item* it = scene_->item(h.mesh); const auto B = it ? bsp(*it) : nullptr; if (!B || h.face < 0 || h.face >= B->faceCount()) return {};
  std::vector<int> fs;
  if (kind == "part") { const auto& parts = partsOf(B); const int p0 = parts[size_t(h.face)]; for (int f = 0; f < B->faceCount(); f++) if (parts[size_t(f)] == p0) fs.push_back(f); }
  else if (kind == "group" && int(B->faceGroup.size()) == B->faceCount()) { const int g = B->faceGroup[size_t(h.face)]; for (int f = 0; f < B->faceCount(); f++) if (B->faceGroup[size_t(f)] == g) fs.push_back(f); }
  else if (kind == "object") { for (int f = 0; f < B->faceCount(); f++) fs.push_back(f); }
  else fs.push_back(h.face);
  return fs;
}
// those polygons in the texture of the UV set (and on the screen — whether seen or not): the ones the set holds
std::vector<Viewport::PvProj> Viewport::pvFaceUvs(int mesh, const std::vector<int>& faces, const QString& set) const {
  std::vector<PvProj> out; const Item* it = scene_->item(mesh); const auto Bs = it ? bsp(*it) : nullptr; if (!Bs || !it->poly) return out; const PolyMesh& B = *Bs; const auto& pos = it->poly->pos;
  if (B.cornerUV.size() != B.faceVerts.size() || B.cornerUV.empty() || pos.size() < B.pos.size()) return out;
  int ks = -1; if (!set.isEmpty()) { for (size_t i = 0; i < B.uvSetNames.size(); i++) if (B.uvSetNames[i] == set) ks = int(i); if (ks < 0) return out; }
  const bool hasOwn = ks >= 0 && size_t(ks) < B.setUV.size() && B.setUV[size_t(ks)].size() == B.faceVerts.size(); const Projector P = projector();
  const std::vector<char> blk = pvBlocked(*it);
  for (int f : faces) { if (f < 0 || f >= B.faceCount() || (!blk.empty() && blk[size_t(f)])) continue; if (ks >= 0 && !(size_t(f) < B.faceSets.size() && (B.faceSets[size_t(f)] >> ks & 1u))) continue;
    const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; if (e - s < 3) continue;
    const bool own = hasOwn && !(size_t(f) < B.faceLive.size() && B.faceLive[size_t(f)] == ks); const auto& UV = own ? B.setUV[size_t(ks)] : B.cornerUV;
    double cu = 0, cv = 0; for (int k = s; k < e; k++) { cu += UV[size_t(k)][0]; cv += UV[size_t(k)][1]; } const double ou = std::floor(cu / (e - s)), ov = std::floor(cv / (e - s));
    PvProj p; p.mesh = mesh; p.face = f;
    for (int k = s; k < e; k++) { const double u = UV[size_t(k)][0] - ou, v = UV[size_t(k)][1] - ov; p.uv.push_back(QPointF(u, B.flipY ? 1 - v : v)); const ScreenPt q = P.toScreen(pos[size_t(B.faceVerts[size_t(k)])]); p.scr.push_back(QPointF(q.x, q.y)); }
    out.push_back(std::move(p)); }
  return out;
}
}  // namespace hm
