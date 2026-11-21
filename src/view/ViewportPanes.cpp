// Split screen (the HTML version's panes): up to 4 windows in one viewport, each with its own camera; the window under the
// pointer is the active one (its camera is cam_ — every pick, drag and navigation goes through it). Also the camera
// helpers that go with it: Shift-snapping to the standard views while orbiting, the view axes' "look from this axis".
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QDateTime>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <cmath>
#include <map>
#include <memory>

namespace hm {

namespace {
// layouts: x, y, w, h of each window (fractions of the viewport)
const std::map<QString, std::vector<std::array<double, 4>>>& layouts() {
  static const std::map<QString, std::vector<std::array<double, 4>>> L = {
    {"2", {{0, 0, 0.5, 1}, {0.5, 0, 0.5, 1}}},
    {"2+1", {{0, 0, 0.5, 0.5}, {0.5, 0, 0.5, 0.5}, {0, 0.5, 1, 0.5}}},
    {"1+2", {{0, 0, 1, 0.5}, {0, 0.5, 0.5, 0.5}, {0.5, 0.5, 0.5, 0.5}}},
    {"4", {{0, 0, 0.5, 0.5}, {0.5, 0, 0.5, 0.5}, {0, 0.5, 0.5, 0.5}, {0.5, 0.5, 0.5, 0.5}}}};
  return L;
}
const char* VIEW_LABEL(const QString& v) {
  return v == "uv" ? "UV Editor" : v == "persp" ? "Perspective" : v == "ortho" ? "Orthographic" : v == "front" ? "Front (ortho)" : v == "right" ? "Left (ortho)" : v == "left" ? "Right (ortho)" : v == "back" ? "Back (ortho)" : "Top (ortho)";
}
Quat slerp(Quat a, Quat b, double t) {
  double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
  if (d > 0.9995) return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}.normalized();
  double th = std::acos(d), s = std::sin(th), wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
  return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.normalized();
}
}  // namespace

std::vector<int> Viewport::detachedPanes() const { std::vector<int> r; for (int i = 4 - detN_; i < 4; i++) r.push_back(i); return r; }
std::vector<int> Viewport::shownPanes() const {
  if (!split_) return {soloPane_};
  std::vector<int> r; for (size_t i = 0; i < layouts().at(layout_).size(); i++) r.push_back(int(i)); return r;
}
QRect Viewport::paneRect(int i) const {
  if (paneDetached(i)) {                                         // (the view window's: beside the viewport, far off — one, or two side by side / one over the other)
    const int w = std::max(1, detSize_.width()), h = std::max(1, detSize_.height()), k = i - (4 - detN_), x0 = detachedOrigin().x();
    if (detN_ == 1) return QRect(x0, 0, w, h);
    if (detStack_) { const int h2 = h / 2; return QRect(x0, k ? h2 : 0, w, std::max(1, k ? h - h2 : h2)); }
    const int w2 = w / 2; return QRect(x0 + (k ? w2 : 0), 0, std::max(1, k ? w - w2 : w2), h);
  }
  const int W = std::max(1, width()), H = std::max(1, height());
  if (!split_) return QRect(0, 0, W, H);
  const auto& L = layouts().at(layout_); if (i < 0 || size_t(i) >= L.size()) return QRect(0, 0, W, H);
  auto [x, y, w, h] = L[size_t(i)];
  int X = int(std::lround(x * W)), Y = int(std::lround(y * H)), X2 = int(std::lround((x + w) * W)), Y2 = int(std::lround((y + h) * H));
  return QRect(X, Y, std::max(1, X2 - X), std::max(1, Y2 - Y));
}
QString Viewport::paneLabel(int i) const {
  QString iso;
  QString info;                                                  // (the UV Editor: how full the tile is, the texel density of all the meshes in it and of the picked polygons)
  const bool uvInfoOn = paneCamera(i).view == "uv" && room() == "topo" && tset.uv;   // (computed when the UV or the selection changed — not at every repaint of the labels)
  const QByteArray ikey = QByteArray::number(uvRev_) + "|" + QByteArray::number(topoRev_) + "|" + QByteArray::number(staticRev_) + "|" + QByteArray::number(tsel_.ver) + "|" + QByteArray::number(topoMesh()) + "|" + QByteArray::number(int(sel_.items.size())) + topoMode.toLatin1();
  if (uvInfoOn && ikey == uvInfoKey_) info = uvInfoVal_;
  else if (uvInfoOn) { auto* self = const_cast<Viewport*>(this); const int am = self->topoMesh();
    std::vector<uv::Covered> cs; double au = 0, a3 = 0, su = 0, s3 = 0; UvTex tex = self->uvTex(am); std::vector<int> ids = self->uvMeshes(); if (am >= 0 && std::find(ids.begin(), ids.end(), am) == ids.end()) ids.push_back(am);
    for (int id : ids) { const Item* it = scene_->item(id); if (!it || !it->poly) continue; const PolyMesh& B = *(it->base ? it->base : it->poly); if (!uv::has(B)) continue;
      std::set<int> sel; if (self->topoMode != "object") { if (id == am && self->tsel_.id == am) for (int f : self->tSelFaces(am)) sel.insert(f); else if (auto x = self->tselX_.find(id); x != self->tselX_.end()) sel.insert(x->second.f.begin(), x->second.f.end()); }
      const auto hid = self->uvHidden(*it, B); cs.push_back({&B, hid});   // (only what the editor shows: the set worked in — the head's own set lies in its own tile; counted with it, the tile came out 112% full, 2026-11-10)
      for (int f = 0; f < B.faceCount(); f++) { if (!hid.empty() && size_t(f) < hid.size() && hid[size_t(f)]) continue; const double a = std::abs(uv::faceArea(B, f)), g = uv::faceArea3(B, f); au += a; a3 += g; if (sel.count(f)) { su += a; s3 += g; } } }
    if (a3 > 0) { const double cov = uv::tileCoverage(cs); info = tr("  ·  tile %1% filled").arg(cov * 100, 0, 'f', cov < 0.1 ? 1 : 0);   /* (each place once: stacked shells and what lies off the tile add nothing) */
      if (au > 0) info += tr("  ·  texel %1 px/cm").arg(std::sqrt(au * tex.w * tex.h / a3), 0, 'f', 1);
      if (su > 0 && s3 > 0) info += tr("  ·  selected %1 px/cm").arg(std::sqrt(su * tex.w * tex.h / s3), 0, 'f', 1); } uvInfoKey_ = ikey; uvInfoVal_ = info; }
  if (scene_->iso) {
    const Item* g = scene_->iso->group >= 0 ? scene_->item(scene_->iso->group) : nullptr;
    QString kept; if (!g) for (int id : scene_->iso->keep) if (const Item* k = scene_->item(id); k && !k->raw.value("tref").toBool()) { kept = kept.isEmpty() ? k->name : kept + ", " + k->name; if (kept.size() > 40) { kept += "…"; break; } }
    iso = scene_->iso->solo ? tr(" · Solo: %1").arg(g ? g->name : kept) : g ? tr(" · Isolated group: %1").arg(g->name) : scene_->iso->head ? tr(" · Isolated (+ HEAD)") : tr(" · Isolated");
  }
  QString pv;                                                    // (the Paint room: the window's visibility set, its camera following the main view's)
  if (room() == "paint") { const PaneVis& v = paneVis_[size_t(i & 3)]; if (paneHid(i)) pv += tr(" · Set: %1").arg(v.set); if (v.sync && i != mainPane()) pv += tr(" · camera follows the main view"); }
  return tr(VIEW_LABEL(paneCamera(i).view)) + iso + pv + info;
}

void Viewport::activatePane(int i) {
  if (i < 0 || i > 3 || i == act_) return;
  panes_[size_t(act_)] = cam_; cam_ = panes_[size_t(i)]; paneFloors_[size_t(act_)] = floor; floor = paneFloors_[size_t(i)]; act_ = i;
  QRect r = paneRect(i); double dpr = devicePixelRatio();
  cam_.width = std::max(1, int(r.width() * dpr)); cam_.height = std::max(1, int(r.height() * dpr));
  emit viewChanged(cam_.view); overlayRev_++; requestUpdate();
}
void Viewport::choosePane(const QPointF& p) {
  for (int i : detachedPanes()) if (paneRect(i).contains(p.toPoint())) { activatePane(i); return; }
  if (paneDetached(act_) && QRect(0, 0, width(), height()).contains(p.toPoint()) && !split_) { activatePane(soloPane_); return; }   // (back from the view window)
  if (!split_) return;
  for (int i : shownPanes()) if (paneRect(i).contains(p.toPoint())) { activatePane(i); return; }
}
void Viewport::setSplit(bool on) {
  if (on == split_) return;
  if (!on) soloPane_ = paneDetached(act_) ? mainPane() : act_;
  if (on && int(layouts().at(layout_).size()) > 4 - detN_) layout_ = detN_ >= 2 ? "2" : "2+1";   // (the view window holds the last windows)
  split_ = on;
  if (on) { auto s = shownPanes(); if (std::find(s.begin(), s.end(), act_) == s.end() && !paneDetached(act_)) activatePane(s[0]); }
  else if (act_ != soloPane_ && !paneDetached(act_)) activatePane(soloPane_);
  saveSplit(); staticRev_++; overlayRev_++; requestUpdate(); emit viewChanged(cam_.view);
}
void Viewport::setSplitLayout(const QString& l) {
  if (!layouts().count(l)) return;
  if (int(layouts().at(l).size()) > 4 - detN_) { ui::toast(tr("The view window holds %1 of the four views — move them into the main viewport (Tab over that window) for this layout").arg(detN_)); return; }
  layout_ = l;
  if (split_) { auto s = shownPanes(); if (std::find(s.begin(), s.end(), act_) == s.end() && !paneDetached(act_)) activatePane(s[0]); }
  saveSplit(); overlayRev_++; requestUpdate();
}
void Viewport::setPaneView(int i, const QString& v) {
  if (i == act_) cam_.setView(v); else panes_[size_t(i)].setView(v);
  saveSplit(); overlayRev_++; requestUpdate(); if (i == act_) emit viewChanged(v);
}
void Viewport::swapPanes(int i, int j) {
  if (i == j) return;
  panes_[size_t(act_)] = cam_; std::swap(panes_[size_t(i)], panes_[size_t(j)]); cam_ = panes_[size_t(act_)];
  saveSplit(); overlayRev_++; requestUpdate();
}
void Viewport::backToSingle(int pane) {
  if (paneDetached(pane) || (pane < 0 && paneDetached(act_))) pane = mainPane();   // (a view of the view window stays there)
  if (pane >= 0 && pane < 4 && pane != act_) activatePane(pane);
  soloPane_ = act_; split_ = false;
  saveSplit(); overlayRev_++; requestUpdate(); emit viewChanged(cam_.view);
}
Viewport::PaneState Viewport::paneState() const {
  PaneState s; for (int i = 0; i < 4; i++) s.cams[size_t(i)] = paneCamera(i); s.act = act_; s.solo = soloPane_; s.split = split_; s.layout = layout_; return s;
}
void Viewport::setPaneState(const PaneState& s) {
  if (s.act != act_) { paneFloors_[size_t(act_)] = floor; floor = paneFloors_[size_t(s.act & 3)]; }
  for (int i = 0; i < 4; i++) panes_[size_t(i)] = s.cams[size_t(i)];
  act_ = s.act & 3; soloPane_ = s.solo; split_ = s.split; if (layouts().count(s.layout)) layout_ = s.layout; cam_ = panes_[size_t(act_)];
  const QRect r = paneRect(act_); const double dpr = devicePixelRatio(); cam_.width = std::max(1, int(r.width() * dpr)); cam_.height = std::max(1, int(r.height() * dpr));
  saveSplit(); staticRev_++; overlayRev_++; requestUpdate(); emit viewChanged(cam_.view);
}
QPoint Viewport::toGlobal(const QPointF& m) const { if (paneDetached(act_) && detHost_) return detHost_->mapToGlobal((m + paneOrigin() - detachedOrigin()).toPoint()); return mapToGlobal((m + paneOrigin()).toPoint()); }
QPointF Viewport::fromGlobal(const QPoint& g) const { if (paneDetached(act_) && detHost_) return QPointF(detHost_->mapFromGlobal(g) + detachedOrigin()) - paneOrigin(); return QPointF(mapFromGlobal(g)) - paneOrigin(); }

void Viewport::saveSplit() const {
  QSettings st; st.setValue("split/on", split_); st.setValue("split/layout", layout_); st.setValue("split/solo", soloPane_);
  { QStringList vs, sy; for (const PaneVis& v : paneVis_) { vs << v.set; sy << (v.sync ? "1" : "0"); } st.setValue("split/visSets", vs); st.setValue("split/camSync", sy); }
  // (the UV Editor not kept: its window saved as it was before it, a split it made — single again)
  QStringList v; for (int i = 0; i < 4; i++) { const QString w = paneCamera(i).view; v << (w != "uv" ? w : i == uvWasPane_ && !uvWasView_.isEmpty() && uvWasView_ != "uv" ? uvWasView_ : QString("persp")); } st.setValue("split/views", v);
  if (uvPaneShown() && uvWasPane_ >= 0 && !uvWasSplit_) { st.setValue("split/on", false); st.setValue("split/solo", uvWasPane_ == act_ ? (act_ == 0 ? 1 : 0) : act_); }
}
void Viewport::setPaneFloor(int i, const Floor& f) {
  i &= 3; if (i == act_) floor = f; else paneFloors_[size_t(i)] = f;
  QSettings st; const QString k = QString("floor/p%1/").arg(i); st.setValue(k + "dy", f.dy); st.setValue(k + "op", f.op); st.setValue(k + "ax", f.ax); st.setValue(k + "fit", !f.zero);   // (fit: under the model — the old "zero" key, written with every change while off was the default, not read)
  staticRev_++; requestUpdate();
}
void Viewport::setFloor(const Floor& f) { setPaneFloor(act_, f); }

// a new project: one perspective window, the four panes with their start-up views, every floor at zero with the default look
// (each window keeps the view the user gave it — 21.11: a window set to Right came back Left after every restart, the empty project made
//  at start-up put the start-up views back. atStart: the split and its layout as they were left too)
void Viewport::resetPanes(bool atStart) {
  panes_[size_t(act_)] = cam_;
  if (!atStart) { act_ = 0; split_ = false; soloPane_ = 0; layout_ = "4"; } else act_ = split_ ? 0 : soloPane_;
  const char* names[4] = {"persp", "front", "right", "top"};
  for (int i = 0; i < 4; i++) { QString v = panes_[size_t(i)].view; if (v.isEmpty() || v == "uv") v = names[i]; panes_[size_t(i)].setView(v); if (!atStart) paneFloors_[size_t(i)] = Floor(); }
  cam_ = panes_[size_t(act_)]; floor = paneFloors_[size_t(act_)];
  // (atStart: the windows' grids and axes as the user left them — 21.11: a grid turned off came back on with every start)
  if (!atStart) { QSettings st; for (int i = 0; i < 4; i++) { const QString k = QString("floor/p%1/").arg(i); st.remove(k + "dy"); st.remove(k + "op"); st.remove(k + "ax"); st.remove(k + "zero"); st.remove(k + "fit"); }
    st.remove("floor/dy"); st.remove("floor/op"); st.remove("floor/ax"); st.remove("floor/zero"); }
  saveSplit(); staticRev_++; overlayRev_++; emit viewChanged(cam_.view); requestUpdate();
}

void Viewport::loadSplit() {
  { QSettings st;   // (each pane its own; a pane never set takes the one common setting of the earlier versions)
    for (int i = 0; i < 4; i++) { const QString k = QString("floor/p%1/").arg(i); Floor& f = paneFloors_[size_t(i)];
      f.dy = st.value(k + "dy", st.value("floor/dy", 0.0)).toDouble(); f.op = st.value(k + "op", st.value("floor/op", 1.0)).toDouble(); f.ax = st.value(k + "ax", st.value("floor/ax", 0.6)).toDouble(); f.zero = !st.value(k + "fit", false).toBool(); } }   // (at zero unless the user put it under the model)
  QSettings st; split_ = st.value("split/on", false).toBool(); layout_ = st.value("split/layout", "4").toString(); if (!layouts().count(layout_)) layout_ = "4";
  soloPane_ = std::clamp(st.value("split/solo", 0).toInt(), 0, 3);
  QStringList v = st.value("split/views").toStringList();
  { const QStringList vs = st.value("split/visSets").toStringList(), sy = st.value("split/camSync").toStringList(); for (int i = 0; i < 4; i++) { paneVis_[size_t(i)].set = vs.value(i); paneVis_[size_t(i)].sync = sy.value(i) == "1"; } }
  for (int i = 0; i < 4; i++) panes_[size_t(i)].view = i < v.size() ? v[i] : QString(i == 0 ? "persp" : i == 1 ? "front" : i == 2 ? "right" : "top");
  uvParked_.clear(); for (int i = 0; i < 4; i++) if (panes_[size_t(i)].view == "uv") panes_[size_t(i)].view = "persp";   // (a UV Editor of another run not brought back)
  act_ = split_ ? 0 : soloPane_; cam_.view = panes_[size_t(act_)].view; floor = paneFloors_[size_t(act_)];
}

// standard directions (pivot → camera) for Shift-snapping: 4 sides, 4 three-quarter views at eye level and from 30°
// above / below, top and bottom (88°, so the camera never looks exactly straight down)
void Viewport::snapToNearestView() {
  struct SV { QString n; Vec3 e; }; static std::vector<SV> L;
  if (L.empty()) {
    const char* names[8] = {"Front", "3/4 front-left", "Left", "3/4 back-left", "Back", "3/4 back-right", "Right", "3/4 front-right"};   // (the side views named for the side of the model seen: the camera on +X ("right") looks at its left side — Left)
    auto dir = [](double az, double el) { return Vec3(std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el)); };
    for (int k = 0; k < 8; k++) { double az = k * M_PI / 4; L.push_back({names[k], dir(az, 0)});
      if (k % 2) { L.push_back({QString(names[k]) + ", from above", dir(az, M_PI / 6)}); L.push_back({QString(names[k]) + ", from below", dir(az, -M_PI / 6)}); } }
    L.push_back({"Top", dir(0, 88 * M_PI / 180)}); L.push_back({"Bottom", dir(0, -88 * M_PI / 180)});
  }
  Vec3 eye = cam_.dir() * -1; const SV* best = &L[0]; double bd = -2;
  for (const SV& v : L) { double d = v.e.dot(eye); if (d > bd) { bd = d; best = &v; } }
  double dist = cam_.pos.distanceTo(cam_.pivot);
  cam_.pos = cam_.pivot + best->e * dist; cam_.up = {0, 1, 0}; cam_.lookAt(cam_.pivot);
  ui::hud(tr("View: %1").arg(best->n));
}

// the view axes: the active view turns to look from that axis (smoothly, round the pivot; again — from the other side).
// A fixed view becomes the free perspective first
void Viewport::viewFromAxis(int k, int s) {
  if (cam_.view != "persp" && cam_.view != "ortho") { cam_.setView("persp"); emit viewChanged("persp"); }
  Vec3 p = cam_.pivot; double dist = std::max(cam_.pos.distanceTo(p), cam_.sceneScale * 0.05);
  Vec3 e(k == 0 ? s : 0, k == 1 ? s : 0, k == 2 ? s : 0);
  if ((cam_.dir() * -1).dot(e) > 0.999) { e = e * -1; s = -s; }   // (already looking from there: the other side)
  if (k == 1) { Vec3 az = cam_.dir() * -1; az.y = 0; if (az.length() < 1e-4) az = {0, 0, 1}; e = (e + az.normalized() * 0.0015).normalized(); }   // (a hair off vertical: stays level)
  Vec3 to = p + e * dist; Quat q1 = Quat::lookRotation((p - to).normalized(), {0, 1, 0});
  Quat q0 = cam_.quat; double d0 = cam_.pos.distanceTo(p);
  const char* names[3][2] = {{"Left", "Right"}, {"Top", "Bottom"}, {"Front", "Back"}}; QString nm = names[k][s > 0 ? 0 : 1];   // (the side views named for the side of the model seen: the camera on +X ("right") looks at its left side — Left)
  if (!navAnim_) { navAnim_ = new QTimer(this); navAnim_->setInterval(16); }
  navAnim_->disconnect(); auto t0 = std::make_shared<qint64>(QDateTime::currentMSecsSinceEpoch()); const int pane = act_;
  connect(navAnim_, &QTimer::timeout, this, [this, t0, q0, q1, d0, dist, p, to, nm, pane] {
    if (act_ != pane) { navAnim_->stop(); return; }
    double t = std::min(1.0, (QDateTime::currentMSecsSinceEpoch() - *t0) / 220.0), u = t * t * (3 - 2 * t);
    Quat q = slerp(q0, q1, u); cam_.quat = q; cam_.pos = p + q.rotate({0, 0, d0 + (dist - d0) * u});
    if (t >= 1) { cam_.pos = to; cam_.quat = q1; cam_.up = {0, 1, 0}; navAnim_->stop(); ui::hud(tr("View: %1").arg(nm)); QTimer::singleShot(900, [] { ui::hud({}); }); }
    requestUpdate();
  });
  navAnim_->start();
}

}  // namespace hm
