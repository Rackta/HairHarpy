// Reference planes in the viewport: the active plane (the 🖼 menu's), its home pane, Ctrl+Alt+drag of its picture. The
// gizmo's part is in ViewportGizmo.cpp; the drawing in Renderer (drawRefPlanes).
#include "ui/Pie.h"
#include "view/Viewport.h"

namespace hm {

RefPlane* Viewport::refPlane(int id) { for (auto& r : scene_->refPlanes) if (r.id == id) return &r; return nullptr; }
int Viewport::activeRefPlane() const { for (auto& r : scene_->refPlanes) if (r.id == refPlane_ && r.visible) return r.id; return -1; }

void Viewport::setActiveRefPlane(int id) {
  refPivEdit_ = false;                                         // (the pivot locks again when the plane is let go)
  if (id >= 0) { sel_ = {}; onSelChange(); }
  refPlane_ = id; refRev_++;
  if (id >= 0 && tool_ != "move" && tool_ != "rotate" && tool_ != "gizmo") setTool("move"); else updateGizmo();
  overlayRev_++; requestUpdate();
}
void Viewport::setRefPivEdit(bool on) {
  refPivEdit_ = on; if (on && tool_ != "move") setTool("move");
  updateGizmo();
  ui::toast(on ? tr("Pivot unlocked: the gizmo moves only the pivot point — put it where to rotate / scale from, then lock it") : tr("Pivot locked: rotate (R) and scale (T) work about it"));
}
void Viewport::refPlanesEdited(bool commitNow) {
  refRev_++; if (!refPlane(refPlane_)) refPlane_ = -1;
  updateGizmo(); requestUpdate();
  if (commitNow) { scene_->revision++; commit(false); }
}

// the plane's own pane: "only in its view" — a shown pane in that view (the active one if it is), else the active pane
const Camera& Viewport::refHomeCamera(const RefPlane& rp) const {
  if (!rp.onlyView || cam_.view == rp.view || (rp.visible && refs::shownIn(rp, cam_))) return cam_;
  for (int i : shownPanes()) if (paneCamera(i).view == rp.view) return paneCamera(i);
  return cam_;
}

// Ctrl+Alt+LMB with a plane active: drag its canvas, whatever is under the pointer — in the plane facing the camera
bool Viewport::rpDragPress(const QPointF& m) {
  RefPlane* rp = refPlane(activeRefPlane()); if (!rp) return false;
  refs::apply(*rp, refHomeCamera(*rp));
  rpd_ = {}; rpd_.id = rp->id; rpd_.n = cam_.dir(); rpd_.ref = rp->pos; rpd_.start = rp->pos; rpd_.startHit = planeHit(ray(m), rpd_.ref, rpd_.n);
  drag_ = Drag::RefPlane; ui::hud(tr("Moving the reference image (Ctrl+Alt+drag)"));
  return true;
}
void Viewport::rpDragMove(const QPointF& m) {
  RefPlane* rp = refPlane(rpd_.id); if (!rp) return;
  auto h = planeHit(ray(m), rpd_.ref, rpd_.n); if (!h || !rpd_.startHit) return;
  rp->pos = rpd_.start + (*h - *rpd_.startHit); refs::capture(*rp, refHomeCamera(*rp));
  rpd_.moved = true; refRev_++; updateGizmo();
}

}  // namespace hm
