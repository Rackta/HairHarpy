// The Paint room's views: a window of the split screen (or of the view window) showing a visibility set — the scene with other
// eyes: a small part alone, to paint it with nothing in the way, next to the whole model in the main view — and its camera
// following the main view's. A visibility set is the project's (fileRaw "visSets": name + the items whose eye is off); the main
// view shows the scene as the outliner's eyes say.
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QMouseEvent>
#include <QSettings>
#include <QWheelEvent>
#include <QWidget>

namespace hm {

namespace {
QJsonArray setsOf(const Scene& s) { return s.fileRaw.value("visSets").toArray(); }
int findSet(const QJsonArray& a, const QString& name) { for (int i = 0; i < a.size(); i++) if (a[i].toObject().value("name").toString() == name) return i; return -1; }
}  // namespace

QStringList Viewport::visSetNames() const { QStringList r; for (const QJsonValue& v : setsOf(*scene_)) r << v.toObject().value("name").toString(); return r; }

// the scene's eyes as they are → a set (a new one, or the one of that name made anew); "" — a name of its own
QString Viewport::visSetSave(const QString& name0) {
  QJsonArray a = setsOf(*scene_); QString name = name0.trimmed();
  if (name.isEmpty()) { int k = int(a.size()) + 1; do name = tr("Set %1").arg(k++); while (findSet(a, name) >= 0); }
  // (what the Paint room shows now: its hidden objects, its hidden polygons)
  QJsonArray hid; QJsonObject hf; for (const auto& [id, it] : scene_->items) { if (it.raw.value("pHid").toBool()) hid.append(id); else if (const QString f = it.raw.value("pHidF").toString(); !f.isEmpty()) hf.insert(QString::number(id), f); }
  const QJsonObject o{{"name", name}, {"hid", hid}, {"f", hf}}; const int i = findSet(a, name);
  if (i >= 0) a[i] = o; else a.append(o);
  scene_->fileRaw.insert("visSets", a); changes_++; emit changed(); staticRev_++; overlayRev_++; requestUpdate(); emit paintVisChanged();
  return name;
}
// a set → what the Paint room shows (its own eyes and hidden polygons)
void Viewport::visSetShow(const QString& name) { if (findSet(setsOf(*scene_), name) >= 0) pvSetApply("set:" + name); }
void Viewport::visSetRename(const QString& from, const QString& to0) {
  QJsonArray a = setsOf(*scene_); const int i = findSet(a, from); const QString to = to0.trimmed(); if (i < 0 || to.isEmpty() || to == from) return;
  if (findSet(a, to) >= 0) { ui::toast(tr("A visibility set named %1 is there already").arg(to)); return; }
  QJsonObject o = a[i].toObject(); o.insert("name", to); a[i] = o; scene_->fileRaw.insert("visSets", a);
  for (auto& v : paneVis_) if (v.set == from) v.set = to;
  changes_++; emit changed(); saveSplit(); overlayRev_++; requestUpdate(); emit paintVisChanged();
}
void Viewport::visSetDelete(const QString& name) {
  QJsonArray a = setsOf(*scene_); const int i = findSet(a, name); if (i < 0) return;
  a.removeAt(i); if (a.isEmpty()) scene_->fileRaw.remove("visSets"); else scene_->fileRaw.insert("visSets", a);
  for (auto& v : paneVis_) if (v.set == name) v.set.clear();
  changes_++; emit changed(); saveSplit(); staticRev_++; overlayRev_++; requestUpdate(); emit paintVisChanged();
}

void Viewport::setPaneVis(int i, const PaneVis& v) {
  paneVis_[size_t(i & 3)] = v; saveSplit(); staticRev_++; overlayRev_++; requestUpdate();
}
int Viewport::mainPane() const { const auto s = shownPanes(); return s.empty() ? act_ : s[0]; }

// the items a window hides by its set (nullopt — it shows the scene as it is: no set, a set gone, another room)
std::optional<std::set<int>> Viewport::paneHid(int i) const {
  const QString& nm = paneVis_[size_t(i & 3)].set; if (nm.isEmpty() || room() != "paint") return std::nullopt;
  const QJsonArray a = setsOf(*scene_); const int k = findSet(a, nm); if (k < 0) return std::nullopt;
  std::set<int> hid; for (const QJsonValue& v : a[k].toObject().value("hid").toArray()) hid.insert(v.toInt()); return hid;
}
bool Viewport::shownWith(const std::optional<std::set<int>>& hid, const Item& it) const {
  if (!hid) return scene_->isVisible(it);
  if (!scene_->isVisibleBase(it)) return false;                 // (the scene's own eyes and the NoPainted label hold in every window)
  for (const Item* p = &it; p; p = p->parent >= 0 ? scene_->item(p->parent) : nullptr) if (hid->count(p->id)) return false;
  return true;
}
std::vector<int> Viewport::panesInUse() const { std::vector<int> r = shownPanes(); for (int i : detachedPanes()) r.push_back(i); return r; }
bool Viewport::paneFilters() const { if (room() != "paint") return false; for (int i : panesInUse()) if (!paneVis_[size_t(i & 3)].set.isEmpty()) return true; return false; }
// drawn by any window: what the renderer keeps built
bool Viewport::shownAnywhere(const Item& it) const {
  if (scene_->isVisible(it)) return true; if (!paneFilters()) return false;
  for (int i : panesInUse()) if (const auto h = paneHid(i); h && shownWith(h, it)) return true;
  return false;
}

// the cameras that follow the main view's: the one being moved (the main view, or a window following it) gives the others its own
void Viewport::syncPaneCams() {
  if (room() != "paint") return;
  const auto use = panesInUse(); const int mainP = mainPane();
  auto follows = [&](int i) { return i != mainP && paneVis_[size_t(i & 3)].sync && std::find(use.begin(), use.end(), i) != use.end(); };
  bool any = false; for (int i : use) if (follows(i)) any = true; if (!any) return;
  const int src = act_ == mainP || follows(act_) ? act_ : mainP; const Camera s = paneCamera(src);
  for (int i : use) { if (i == src || !(i == mainP || follows(i))) continue;
    Camera& c = paneCam(i); const int w = c.width, h = c.height;
    if (c.view == s.view && c.pos.distanceTo(s.pos) < 1e-12 && c.pivot.distanceTo(s.pivot) < 1e-12 && c.quat.x == s.quat.x && c.quat.y == s.quat.y && c.quat.z == s.quat.z && c.quat.w == s.quat.w && c.fov == s.fov && c.zoom == s.zoom) continue;
    c = s; c.width = w; c.height = h; overlayRev_++; }
}


// ---------- the view window ----------
void Viewport::fitActiveCam() { const QRect r = paneRect(act_); const double dpr = devicePixelRatio(); cam_.width = std::max(1, int(r.width() * dpr)); cam_.height = std::max(1, int(r.height() * dpr)); }
void Viewport::swapWholePanes(int i, int j) {
  if (i == j || i < 0 || j < 0 || i > 3 || j > 3) return;
  panes_[size_t(act_)] = cam_; paneFloors_[size_t(act_)] = floor;
  std::swap(panes_[size_t(i)], panes_[size_t(j)]); std::swap(paneFloors_[size_t(i)], paneFloors_[size_t(j)]); std::swap(paneVis_[size_t(i)], paneVis_[size_t(j)]);
  if (act_ == i) act_ = j; else if (act_ == j) act_ = i;
  cam_ = panes_[size_t(act_)]; floor = paneFloors_[size_t(act_)];
}
void Viewport::setDetached(int n, QWidget* host) {
  n = std::clamp(n, 0, 2); if (n == detN_) { detHost_ = n ? host : nullptr; return; }
  panes_[size_t(act_)] = cam_; paneFloors_[size_t(act_)] = floor;
  if (n > detN_) {                                               // (room for them: the main viewport keeps the first windows)
    const int room = 4 - n;
    if (split_ && layout_ != "2" && (room < 3 || layout_ == "4")) layout_ = room >= 3 ? "2+1" : "2";
    if (!split_ && soloPane_ >= room) { swapWholePanes(soloPane_, 0); soloPane_ = 0; }
    const int mainP = split_ ? 0 : soloPane_;
    if (detN_ == 1) swapWholePanes(2, 3);                          // (the view there already stays the first, the new one after it)
    const int fresh = detN_ == 1 ? 3 : 4 - n;                       // (a new view: the main view's camera and floor, the scene as it is)
    for (int i = fresh; i < 4; i++) { panes_[size_t(i)] = panes_[size_t(mainP)]; paneFloors_[size_t(i)] = paneFloors_[size_t(mainP)]; paneVis_[size_t(i)] = PaneVis(); }
  } else if (detN_ == 2 && n == 1) swapWholePanes(2, 3);            // (the second view goes, the first stays)
  detN_ = n; detHost_ = n ? host : nullptr;
  if (!paneDetached(act_)) { const auto s = shownPanes(); if (std::find(s.begin(), s.end(), act_) == s.end()) { act_ = s[0]; } }
  cam_ = panes_[size_t(act_)]; floor = paneFloors_[size_t(act_)]; fitActiveCam();
  saveSplit(); staticRev_++; overlayRev_++; requestUpdate(); emit viewChanged(cam_.view);
}
void Viewport::setDetachedLook(const QSize& logical, bool stacked) {
  const QSize s(std::max(16, logical.width()), std::max(16, logical.height())); if (s == detSize_ && stacked == detStack_) return;
  detSize_ = s; detStack_ = stacked; if (paneDetached(act_)) fitActiveCam(); overlayRev_++; requestUpdate();
}
// the view window's views after the main viewport's own, the split screen on with a layout that holds them all
void Viewport::migrateDetached() {
  const int n = detN_; if (!n) return;
  if (!split_ && soloPane_ != 0) { swapWholePanes(soloPane_, 0); soloPane_ = 0; }
  const int m = split_ ? (layout_ == "2" ? 2 : 3) : 1;
  for (int k = 0; k < n; k++) swapWholePanes(m + k, 4 - n + k);
  detN_ = 0; detHost_ = nullptr; split_ = true; layout_ = m + n <= 2 ? "2" : m + n == 3 ? "2+1" : "4";
  cam_ = panes_[size_t(act_)]; floor = paneFloors_[size_t(act_)]; fitActiveCam();
  saveSplit(); staticRev_++; overlayRev_++; requestUpdate(); emit viewChanged(cam_.view);
}
void Viewport::viewHover(bool in) { inViewport_ = in; overlayRev_++; requestUpdate(); }
void Viewport::viewEvent(QEvent* e) {
  switch (e->type()) {
    case QEvent::MouseButtonPress: mousePressEvent(static_cast<QMouseEvent*>(e)); break;
    case QEvent::MouseButtonRelease: mouseReleaseEvent(static_cast<QMouseEvent*>(e)); break;
    case QEvent::MouseMove: mouseMoveEvent(static_cast<QMouseEvent*>(e)); break;
    case QEvent::MouseButtonDblClick: mouseDoubleClickEvent(static_cast<QMouseEvent*>(e)); break;
    case QEvent::Wheel: wheelEvent(static_cast<QWheelEvent*>(e)); break;
    default: break;
  }
}

}  // namespace hm
