// The reference canvas's split (RefCanvas.h): the viewport's own layouts (2 · 2+1 · 1+2 · 4), each window a fixed view (front, back,
// the sides, top — no perspective) with its own canvas, all at one scale and height — a lock drawn in the front and the side view at
// once, its places seen in all of them, as a drawing in several views. Each window's view is chosen in its corner. Only a click
// switches the view drawn in: the pointer moving over another one changes nothing.
#include "app/PaintCanvas.h"
#include "app/PaintSession.h"
#include "app/RefCanvas.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QComboBox>
#include <QPushButton>
#include <QSettings>
#include <algorithm>
#include <cmath>

namespace hm {

using namespace paint;

namespace {
const char* kViews[] = {"front", "right", "left", "back", "top"};   // (the order shown: Front, Left, Right, Back, Top — the side views named for the model's side seen)
QStringList defaultViews(const QString& V) {                         // (the other windows' views round the one drawn in)
  QStringList o{V};
  const QStringList pref = V == "left" || V == "right" ? QStringList{"front", V == "right" ? "left" : "right", "back", "top"} : V == "top" ? QStringList{"front", "right", "left", "back"} : QStringList{"right", "left", "top", V == "front" ? "back" : "front"};
  for (const QString& v : pref) if (!o.contains(v)) o << v;
  return o;
}
}  // namespace

// ▦ Grid: every shown window's grid (the floor and the walls) and its world axes on or off together — off when any is on
void RefCanvas::toggleGrid() {
  const auto shown = vp_->shownPanes(); bool any = false; for (int i : shown) if (vp_->paneFloor(i).op > 0.001 || vp_->paneFloor(i).ax > 0.001) any = true;
  for (int i : shown) { auto F = vp_->paneFloor(i);
    if (any) { if (F.op > 0.001) gridOp_[i] = F.op; if (F.ax > 0.001) axesOp_[i] = F.ax; F.op = 0; F.ax = 0; }
    else { auto g = gridOp_.find(i); F.op = g != gridOp_.end() && g->second > 0.001 ? g->second : 1; auto a = axesOp_.find(i); F.ax = a != axesOp_.end() && a->second > 0.001 ? a->second : 0.6; }
    vp_->setPaneFloor(i, F); }
  syncGridB(); ui::hud(any ? tr("Grid off") : tr("Grid on"), 700);
}
void RefCanvas::syncGridB() { if (!gridB_) return; bool any = false; for (int i : vp_->shownPanes()) if (vp_->paneFloor(i).op > 0.001 || vp_->paneFloor(i).ax > 0.001) any = true; QSignalBlocker b(gridB_); gridB_->setChecked(any); }

// (the windows as they were before the canvas opened: taken once, put back on leaving)
void RefCanvas::capturePanes() { if (!hasPanes_ && !open_) { savedPanes_ = vp_->paneState(); hasPanes_ = true; } }

int RefCanvas::paneAt(const QPointF& w) const {
  for (int i : vp_->shownPanes()) if (QRectF(vp_->paneRect(i)).contains(w)) return i;
  return -1;
}

// a window turned to a view: one scale with the rest, its height level with them; the canvas it shows — one already drawn in that view, else a new one
void RefCanvas::applyPaneView(int pane, const QString& v, const Camera& ref) {
  const double dpr = vp_->devicePixelRatio();
  vp_->setPaneView(pane, v);
  Camera& c = vp_->paneCam(pane); const QRect r = vp_->paneRect(pane); c.width = std::max(1, int(r.width() * dpr)); c.height = std::max(1, int(r.height() * dpr));
  c.zoom = ref.zoom;
  if (v != "top" && ref.view != "top") { const double dy = ref.pos.y - c.pos.y; c.pos.y += dy; c.pivot.y += dy; }
  int pid = -1;
  for (const RefPlane& p : sc_->refPlanes) if (p.view == v && p.canvas >= 0 && sc_->refSketches.count(p.canvas)) { if (pid < 0 || p.sheet) pid = p.id; if (p.sheet) break; }
  if (pid < 0) { pid = makeSheet(v, vp_->paneCamera(pane)); madeHere_.push_back(pid); }
  for (RefPlane& p : sc_->refPlanes) if (p.id == pid) p.visible = true;
  panePlane_[pane] = pid;
}

void RefCanvas::setSplit(bool on) {
  if (on == splitOn_) { if (splitB_) splitB_->setChecked(on); return; }
  if (on) {
    const RefPlane* rp = plane();
    if (!rp || rp->follows()) { ui::toast(tr("The split works with a front, back, left, right or top canvas")); splitB_->setChecked(false); return; }
    if (fl_.on) applyFloat();
    composeNow(true);
    const QString V = rp->view, layout = splitLayout_->currentData().toString();
    QStringList views = QSettings().value("refcanvas/splitViews").toStringList();
    if (views.size() < 4 || views.value(0) != V) views = defaultViews(V);
    const int myPlane = id_; const Camera cur = vp_->camera(); const double dpr = vp_->devicePixelRatio();
    vp_->setSplitLayout(layout); vp_->setSplit(true); vp_->activatePane(0);
    { Camera& c = vp_->paneCam(0); c = cur; const QRect r = vp_->paneRect(0); c.width = std::max(1, int(r.width() * dpr)); c.height = std::max(1, int(r.height() * dpr)); }
    panePlane_.clear(); panePlane_[0] = myPlane;
    for (int i : vp_->shownPanes()) if (i != 0) applyPaneView(i, views.value(i, "front"), cur);
    splitOn_ = true; vp_->refPlanesEdited(true);
  } else {
    if (fl_.on) applyFloat();
    composeNow(true); vp_->backToSingle(vp_->activePane()); splitOn_ = false; panePlane_.clear(); overOther_ = false; canvas_->setCursor(canvas_->restCursor);
  }
  splitB_->setChecked(splitOn_); syncGridB(); placePaneBoxes(); followView(); syncPlaneUi(); syncInfo(); canvas_->update(); vp_->requestUpdate();
}

// (the canvas drawn in kept open as it is — its layers, its undo — while another one is drawn in)
void RefCanvas::stashCurrent() {
  if (fl_.on) applyFloat();
  poly_.clear(); polyOn_ = false; shaping_ = selDrag_ = false; deselect();
  composeNow(true);
  if (cid_ >= 0) { Stash st; st.doc = std::move(s_->doc); st.undo = std::move(s_->undo); st.redo = std::move(s_->redo); st.layer = s_->layer; st.placed = std::move(placed_); st.dirty = dirty_;
    stash_[cid_] = std::move(st); s_->undo.clear(); s_->redo.clear(); placed_.clear(); }
  cid_ = -1; dirty_ = false; s_->editMask = false;
}
void RefCanvas::takeUp(int planeId) {
  int cid = -1; for (const RefPlane& r : sc_->refPlanes) if (r.id == planeId) cid = r.canvas;
  id_ = planeId; if (cid < 0) return;
  if (auto g = stash_.find(cid); g != stash_.end()) {
    s_->doc = std::move(g->second.doc); s_->undo = std::move(g->second.undo); s_->redo = std::move(g->second.redo); s_->layer = g->second.layer; placed_ = std::move(g->second.placed); const bool d = g->second.dirty; stash_.erase(g);
    emit s_->layersChanged(); emit s_->pixelsChanged({}); emit s_->activeChanged();   // (before cid_: taking it up is not a change)
    cid_ = cid; dirty_ = d; fillLibrary();
  } else loadCanvas(cid);
  overOther_ = false; canvas_->setCursor(canvas_->restCursor);
  followView(); syncPlaneUi(); syncLayerProps(); syncInfo(); canvas_->update(); vp_->requestUpdate();
}

// the view drawn in switched (a click in it)
void RefCanvas::setActiveSide(int pane) {
  if (!splitOn_ || pane == vp_->activePane()) return;
  auto f = panePlane_.find(pane); if (f == panePlane_.end()) return;
  offOf_[id_] = offD_; rollOf_[vp_->activePane()] = {roll_, rollRaw_};   // (the plane's own offset and the pane's own turn kept — "back where it was opened" and "⟲ 0°" act on the one drawn in)
  stashCurrent(); vp_->activatePane(pane); takeUp(f->second); placePaneBoxes();
  { auto o = offOf_.find(id_); offD_ = o != offOf_.end() ? o->second : Vec3();
    auto r = rollOf_.find(pane); roll_ = r != rollOf_.end() ? r->second.first : 0; rollRaw_ = r != rollOf_.end() ? r->second.second : 0;
    if (rotB_) rotB_->setText(tr("⟲ %1°").arg(int(std::lround(std::remainder(roll_, 360.0))))); }
}

// a window's view chosen in its corner (the window drawn in: its canvas changes with it)
void RefCanvas::setPaneViewTo(int pane, const QString& v) {
  if (!splitOn_) return;
  const bool act = pane == vp_->activePane(); const Camera ref = vp_->paneCamera(pane);
  if (act) stashCurrent();
  applyPaneView(pane, v, ref);
  if (act) takeUp(panePlane_[pane]);
  QStringList views = QSettings().value("refcanvas/splitViews").toStringList(); while (views.size() < 4) views << "front";
  for (auto& [p, id] : panePlane_) for (const RefPlane& r : sc_->refPlanes) if (r.id == id) views[p] = r.view;
  QSettings().setValue("refcanvas/splitViews", views);
  vp_->refPlanesEdited(true); placePaneBoxes(); canvas_->update(); vp_->requestUpdate();
}

// each window's view chooser over its corner (where the viewport's own label is)
void RefCanvas::placePaneBoxes() {
  const auto shown = vp_->shownPanes();
  for (int i = 0; i < 4; i++) {
    const bool want = splitOn_ && std::find(shown.begin(), shown.end(), i) != shown.end();
    auto f = paneBox_.find(i);
    if (!want) { if (f != paneBox_.end()) f->second->hide(); continue; }
    QComboBox* b = f != paneBox_.end() ? f->second : nullptr;
    if (!b) { b = new QComboBox(this); b->setFocusPolicy(Qt::NoFocus); b->setToolTip(tr("This window's view (its own canvas)"));
      for (const char* v : kViews) b->addItem(refs::viewLabel(v), QString(v));
      connect(b, &QComboBox::activated, this, [this, i, b](int) { setPaneViewTo(i, b->currentData().toString()); canvas_->setFocus(); });
      paneBox_[i] = b; }
    QString v; for (const RefPlane& r : sc_->refPlanes) if (r.id == panePlane_[i]) v = r.view;
    { QSignalBlocker bl(b); b->setCurrentIndex(std::max(0, b->findData(v))); }
    b->setStyleSheet(i == vp_->activePane() ? "QComboBox{border:1px solid #9466be}" : QString());
    QRect r = vp_->paneRect(i); const int th = top_->sizeHint().height(); b->adjustSize();
    b->move(r.left() + 6, std::max(r.top() + 6, th + 6)); b->show(); b->raise();
  }
}

}  // namespace hm
