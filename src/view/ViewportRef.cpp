// The Convert window's helpers in the viewport: the reference cube (its height becomes the loop step; drag it to move it,
// its top face — the height) and ◎ Pick (a circle at the cursor: a click sets its diameter as the weld distance).
#include "scene/Walls.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <cmath>

namespace hm {

namespace {
QString fmtLen(double v) {
  if (v == 0) return "0.00";
  int e = int(std::floor(std::log10(std::abs(v))));
  if (e < -6 || e >= 3) return QString::number(v, 'e', 2);
  return QString::number(v, 'f', std::max(0, 2 - e));
}
}  // namespace

void Viewport::toggleReference() {
  if (!refCube) {
    double step = convValue("step").toDouble(0); if (!(step > 0)) step = projectStep(*scene_);
    Vec3 base = cam_.pivot;
    for (int id : sel_.items) if (const Item* c = scene_->item(id); c && c->type == ItemType::Curve) { auto it = hair_.curves.find(id); if (it != hair_.curves.end() && !it->second.p.empty()) { base = it->second.p[it->second.p.size() >> 1]; break; } }
    refCube = RefCube{base, std::max(step, cam_.sceneScale * 0.05), step};   // (wide enough to grab)
    sel_.items.clear(); sel_.anchors.clear(); onSelChange();
    overlayRev_++; requestUpdate();
    ui::toast(tr("Move the cube (drag it), drag its top face to set the height, then press Set reference again"));
  } else {
    double h = refCube->height; refCube.reset(); overlayRev_++; requestUpdate();
    setConv("step", h, true);
    ui::toast(tr("Loop step = %1").arg(fmtLen(h)));
  }
  emit brushChanged();
}

// a ray at the cube (an axis-aligned box: base at the bottom centre) → its top face → the height; its body → moved
bool Viewport::refPress(const QPointF& m) {
  if (!refCube) return false;
  const RefCube& c = *refCube; Ray r = ray(m);
  Vec3 lo(c.base.x - c.size / 2, c.base.y, c.base.z - c.size / 2), hi(c.base.x + c.size / 2, c.base.y + c.height, c.base.z + c.size / 2);
  double t0 = -1e300, t1 = 1e300; int axis = -1; bool neg = false;
  for (int k = 0; k < 3; k++) {
    double o = r.o[k], d = r.d[k], a = lo[k], b = hi[k];
    if (std::abs(d) < 1e-15) { if (o < a || o > b) return false; continue; }
    double ta = (a - o) / d, tb = (b - o) / d; bool sw = ta > tb; if (sw) std::swap(ta, tb);
    if (ta > t0) { t0 = ta; axis = k; neg = !sw; }   // (entered through the low face when the ray runs up that axis)
    t1 = std::min(t1, tb);
  }
  if (t0 > t1 || t1 < 0 || axis < 0) return false;
  Vec3 p = r.o + r.d * std::max(t0, 0.0);
  if (axis == 1 && !neg) {                                   // the top face
    Vec3 n = camDir(); n.y = 0; if (n.lengthSq() < 1e-8) n = Vec3(0, 0, 1);
    rd_ = {}; rd_.n = n.normalized(); rd_.p0 = p; rd_.h0 = c.height; drag_ = Drag::RefTop;
  } else { rd_ = {}; rd_.n = camDir(); rd_.start = c.base; rd_.p0 = p; rd_.hit0 = planeHit(ray(m), p, camDir()); drag_ = Drag::RefMove; }
  return true;
}
void Viewport::refMove(const QPointF& m) {
  if (!refCube) return;
  auto h = planeHit(ray(m), rd_.p0, rd_.n); if (!h) return;
  if (drag_ == Drag::RefTop) { refCube->height = std::max(refCube->size * 0.02, rd_.h0 + h->y - rd_.p0.y); ui::hud(tr("Loop step: %1").arg(fmtLen(refCube->height))); emit brushChanged(); }
  else if (rd_.hit0) refCube->base = rd_.start + (*h - *rd_.hit0);
  overlayRev_++; requestUpdate();
}

double Viewport::pickWorld(const QPointF& m) const {
  auto hit = pickSurface(m); return 2 * pickR * worldPerPixelAt(hit ? hit->point : cam_.pivot);
}
void Viewport::setMergePick(bool on) {
  mergePick = on; overlayRev_++; requestUpdate();
  if (on) ui::hud(tr("Weld distance: %1 (circle diameter) — click to set · Ctrl + wheel / Ctrl+RMB drag — size · Esc — cancel").arg(fmtLen(pickWorld(mouse_))));
  else ui::hud({});
  emit brushChanged();
}

}  // namespace hm
