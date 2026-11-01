// The Lock Extrude tool (the Hair room): a click picks a flat lock — its chain of curves (Shift adds one more); then a
// drag extrudes the picked locks into volumes (scene/LockExtrude): LMB ←→ — the offset away from the head / toward it,
// ↑↓ — the side walls' segments; RMB ←→ — how far from the root it swells and how much it narrows, ↑↓ — the tip's taper.
// While the tool and the selection stay, the drags go on editing the same extrusion; another tool or another object
// leaves it as it is.
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QMouseEvent>
#include <algorithm>
#include <cmath>

namespace hm {

void Viewport::lockExReset() { lx_ = {}; }

bool Viewport::lockExBuild() {
  const auto ss = lockex::strips(*scene_, sel_.items);
  if (ss.empty()) { ui::toast(tr("Pick a flat lock — curves joined by walls (a volume lock is one already)")); return false; }
  const lockex::Params keep = lx_.P; lx_ = {}; lx_.P = keep; lx_.selKey = sel_.items;
  for (auto& st : ss) {
    // (the head's side: the middle of the head's vertices near the lock — the scene's middle may be far from the head)
    Vec3 c; int n = 0; Box3 lb; for (int id : st) if (const Item* x = scene_->item(id)) for (int a : x->anchors) { c += scene_->anchor(a)->pos; n++; lb.expand(scene_->anchor(a)->pos); }
    c = c / double(std::max(1, n)); const double R = std::max(1e-6, lb.size().length() * 1.5);
    Vec3 hc; int hn = 0; for (auto& [id, it] : scene_->items) if ((it.headTag || it.type == ItemType::Head) && it.poly && scene_->isVisible(it)) for (const Vec3& p : it.poly->pos) if (p.distanceTo(c) < R) { hc += p; hn++; }
    const Vec3 head = hn ? hc / double(hn) : cam_.sceneCenter;
    auto B = lockex::create(*scene_, hair_.curves, st, head, lx_.P.segs); if (!B.copy.empty()) lx_.built.push_back(std::move(B)); }
  if (lx_.built.empty()) { ui::toast(tr("These locks can't be extruded")); return false; }
  lx_.on = true; reloadHair(); return true;
}

// the extrusion placed for lx_.P: the sides made again when the segments changed, the anchors moved
void Viewport::lockExUpdate() {
  bool topo = false; for (auto& B : lx_.built) if (lockex::setSegs(*scene_, hair_.curves, B, lx_.P.segs)) topo = true;
  std::set<int> touched;
  for (auto& B : lx_.built) { lockex::apply(*scene_, B, lx_.P); touched.insert(B.orig.begin(), B.orig.end()); for (auto& L : B.copy) touched.insert(L.id); for (auto& L : B.mids) touched.insert(L.id); }
  scene_->revision++;
  if (topo) reloadHair(); else markCurves(touched);
}

void Viewport::lockExSet(const lockex::Params& P) {
  lx_.P = P; if (!lx_.on) return;
  lockExUpdate(); commitLive(true);
}

bool Viewport::lockExPress(QMouseEvent* e) {
  const QPointF m = e->position(); const bool shift = e->modifiers() & Qt::ShiftModifier;
  if (e->button() != Qt::LeftButton && e->button() != Qt::RightButton) return false;
  if (e->button() == Qt::LeftButton) {
    // (a lock under the pointer not picked yet: picked — its chain of curves; Shift — added)
    int hit = -1;
    if (auto h = pickSurface(m); h && scene_->item(h->itemId) && scene_->item(h->itemId)->type == ItemType::Surface) hit = h->itemId;
    else if (auto ch = pickCurve(m, true, true)) hit = ch->id;
    if (hit >= 0) {
      bool own = false;   // (the extrusion's own curves and walls: a drag on them edits it)
      for (auto& B : lx_.built) {
        for (int c : B.orig) if (c == hit) own = true;
        for (auto& L : B.copy) if (L.id == hit) own = true;
        for (auto& L : B.mids) if (L.id == hit) own = true;
        for (int w : B.backWalls) if (w == hit) own = true;
        for (int w : B.sideWalls) if (w == hit) own = true; }
      const auto st = lockex::stripOf(*scene_, hit);
      bool picked = !st.empty(); for (int c : st) if (std::find(sel_.items.begin(), sel_.items.end(), c) == sel_.items.end()) picked = false;
      if (!own && !picked) {
        if (st.empty()) { ui::toast(tr("Not a flat lock — curves joined by walls in a row")); return true; }
        lockExReset();
        if (shift) { for (int c : st) if (std::find(sel_.items.begin(), sel_.items.end(), c) == sel_.items.end()) sel_.items.push_back(c); } else sel_.items = st;
        sel_.anchors.clear(); onSelChange();
        ui::hud(tr("Lock Extrude: drag — LMB ←→ extrude, ↑↓ segments · RMB ←→ from the root, ↑↓ the tip"), 2500);
        return true;
      }
    }
  }
  if (sel_.items.empty()) { if (e->button() == Qt::LeftButton) ui::toast(tr("Click a flat lock to extrude it")); return true; }
  lxStart_ = m; lxP0_ = lx_.P; lxRmb_ = e->button() == Qt::RightButton; lxMoved_ = false; drag_ = Drag::LockEx;
  return true;
}

void Viewport::lockExMove(const QPointF& m) {
  const double dx = m.x() - lxStart_.x(), dy = m.y() - lxStart_.y();
  if (!lxMoved_ && std::hypot(dx, dy) < 4) return;
  // (the extrusion kept from the last drag: anything of it gone from the scene — undone, deleted — made anew)
  if (lx_.on) { bool ok = true; for (auto& B : lx_.built) { for (int c : B.orig) if (!scene_->item(c)) ok = false; for (auto& L : B.copy) if (!scene_->item(L.id)) ok = false;
      for (auto& L : B.mids) if (!scene_->item(L.id)) ok = false; for (int w : B.backWalls) if (!scene_->item(w)) ok = false; for (int w : B.sideWalls) if (!scene_->item(w)) ok = false; }
    if (!ok) { const lockex::Params keep = lx_.P; lockExReset(); lx_.P = keep; } }
  if (!lx_.on) { lx_.P = lxP0_; if (!lockExBuild()) { drag_ = Drag::None; return; } }
  lxMoved_ = true; lockex::Params P = lxP0_;
  if (!lxRmb_) { P.d = lxP0_.d + dx / 250; P.segs = std::clamp(lxP0_.segs + int(std::lround(-dy / 25)), 1, 16); }
  else { P.root = std::clamp(lxP0_.root + dx / 300, 0.0, 1.0); P.tip = std::clamp(lxP0_.tip - dy / 300, 0.02, 1.0); }
  lx_.P = P; lockExUpdate();
  ui::hud(lxRmb_ ? tr("From the root: %1% · tip taper: %2%").arg(int(std::lround(P.root * 100))).arg(int(std::lround(P.tip * 100)))
                 : tr("Extrude: %1%2% of the width %3 · side segments: %4").arg(P.d >= 0 ? "+" : "−").arg(int(std::lround(std::abs(P.d) * 100))).arg(P.d >= 0 ? tr("away from the head") : tr("toward the head")).arg(P.segs));
  emit brushChanged();
}

void Viewport::lockExRelease() {
  ui::hud({});
  if (!lxMoved_) return;
  beforeFrame(); stepLabel_ = tr("Lock Extrude"); commit(true); emit sceneEdited(); emit brushChanged();
}

}  // namespace hm
