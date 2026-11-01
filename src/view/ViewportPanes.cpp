// Split screen (the HTML version's panes): up to 4 windows in one viewport, each with its own camera; the window under the
// pointer is the active one (its camera is cam_ — every pick, drag and navigation goes through it). Also the camera
// helpers that go with it: Shift-snapping to the standard views while orbiting, the view axes' "look from this axis".
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QDateTime>
#include <QSettings>
#include <QTimer>
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
  return v == "uv" ? "UV Editor" : v == "persp" ? "Perspective" : v == "ortho" ? "Orthographic" : v == "front" ? "Front (ortho)" : v == "right" ? "Right (ortho)" : v == "left" ? "Left (ortho)" : v == "back" ? "Back (ortho)" : "Top (ortho)";
}
Quat slerp(Quat a, Quat b, double t) {
  double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
  if (d > 0.9995) return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}.normalized();
  double th = std::acos(d), s = std::sin(th), wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
  return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.normalized();
}
}  // namespace

std::vector<int> Viewport::shownPanes() const {
  if (!split_) return {soloPane_};
  std::vector<int> r; for (size_t i = 0; i < layouts().at(layout_).size(); i++) r.push_back(int(i)); return r;
}
QRect Viewport::paneRect(int i) const {
  const int W = std::max(1, width()), H = std::max(1, height());
  if (!split_) return QRect(0, 0, W, H);
  const auto& L = layouts().at(layout_); if (i < 0 || size_t(i) >= L.size()) return QRect(0, 0, W, H);
  auto [x, y, w, h] = L[size_t(i)];
  int X = int(std::lround(x * W)), Y = int(std::lround(y * H)), X2 = int(std::lround((x + w) * W)), Y2 = int(std::lround((y + h) * H));
  return QRect(X, Y, std::max(1, X2 - X), std::max(1, Y2 - Y));
}
QString Viewport::paneLabel(int i) const {
  QString iso;
  if (scene_->iso) {
    const Item* g = scene_->iso->group >= 0 ? scene_->item(scene_->iso->group) : nullptr;
    QString kept; if (!g) for (int id : scene_->iso->keep) if (const Item* k = scene_->item(id); k && !k->raw.value("tref").toBool()) { kept = kept.isEmpty() ? k->name : kept + ", " + k->name; if (kept.size() > 40) { kept += "…"; break; } }
    iso = scene_->iso->solo ? tr(" · Solo: %1").arg(g ? g->name : kept) : g ? tr(" · Isolated group: %1").arg(g->name) : scene_->iso->head ? tr(" · Isolated (+ head)") : tr(" · Isolated");
  }
  return tr(VIEW_LABEL(paneCamera(i).view)) + iso;
}

void Viewport::activatePane(int i) {
  if (i < 0 || i > 3 || i == act_) return;
  panes_[size_t(act_)] = cam_; cam_ = panes_[size_t(i)]; act_ = i;
  QRect r = paneRect(i); double dpr = devicePixelRatio();
  cam_.width = std::max(1, int(r.width() * dpr)); cam_.height = std::max(1, int(r.height() * dpr));
  emit viewChanged(cam_.view); overlayRev_++; requestUpdate();
}
void Viewport::choosePane(const QPointF& p) {
  if (!split_) return;
  for (int i : shownPanes()) if (paneRect(i).contains(p.toPoint())) { activatePane(i); return; }
}
void Viewport::setSplit(bool on) {
  if (on == split_) return;
  if (!on) soloPane_ = act_;
  split_ = on;
  if (on) { auto s = shownPanes(); if (std::find(s.begin(), s.end(), act_) == s.end()) activatePane(s[0]); }
  else if (act_ != soloPane_) activatePane(soloPane_);
  saveSplit(); staticRev_++; overlayRev_++; requestUpdate(); emit viewChanged(cam_.view);
}
void Viewport::setSplitLayout(const QString& l) {
  if (!layouts().count(l)) return;
  layout_ = l;
  if (split_) { auto s = shownPanes(); if (std::find(s.begin(), s.end(), act_) == s.end()) activatePane(s[0]); }
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
  if (pane >= 0 && pane < 4 && pane != act_) activatePane(pane);
  soloPane_ = act_; split_ = false;
  saveSplit(); overlayRev_++; requestUpdate(); emit viewChanged(cam_.view);
}
QPoint Viewport::toGlobal(const QPointF& m) const { return mapToGlobal((m + paneOrigin()).toPoint()); }
QPointF Viewport::fromGlobal(const QPoint& g) const { return QPointF(mapFromGlobal(g)) - paneOrigin(); }

void Viewport::saveSplit() const {
  QSettings st; st.setValue("split/on", split_); st.setValue("split/layout", layout_); st.setValue("split/solo", soloPane_);
  // (the UV Editor not kept: its window saved as it was before it, a split it made — single again)
  QStringList v; for (int i = 0; i < 4; i++) { const QString w = paneCamera(i).view; v << (w != "uv" ? w : i == uvWasPane_ && !uvWasView_.isEmpty() && uvWasView_ != "uv" ? uvWasView_ : QString("persp")); } st.setValue("split/views", v);
  if (uvPaneShown() && uvWasPane_ >= 0 && !uvWasSplit_) { st.setValue("split/on", false); st.setValue("split/solo", uvWasPane_ == act_ ? (act_ == 0 ? 1 : 0) : act_); }
}
void Viewport::setFloor(const Floor& f) {
  floor = f; QSettings st; st.setValue("floor/dy", f.dy); st.setValue("floor/op", f.op); st.setValue("floor/ax", f.ax); st.setValue("floor/zero", f.zero);
  staticRev_++; requestUpdate();
}

void Viewport::loadSplit() {
  { QSettings st; floor.dy = st.value("floor/dy", 0.0).toDouble(); floor.op = st.value("floor/op", 1.0).toDouble(); floor.ax = st.value("floor/ax", 0.6).toDouble(); floor.zero = st.value("floor/zero", false).toBool(); }
  QSettings st; split_ = st.value("split/on", false).toBool(); layout_ = st.value("split/layout", "4").toString(); if (!layouts().count(layout_)) layout_ = "4";
  soloPane_ = std::clamp(st.value("split/solo", 0).toInt(), 0, 3);
  QStringList v = st.value("split/views").toStringList();
  for (int i = 0; i < 4; i++) panes_[size_t(i)].view = i < v.size() ? v[i] : QString(i == 0 ? "persp" : i == 1 ? "front" : i == 2 ? "right" : "top");
  uvParked_.clear(); for (int i = 0; i < 4; i++) if (panes_[size_t(i)].view == "uv") panes_[size_t(i)].view = "persp";   // (a UV Editor of another run not brought back)
  act_ = split_ ? 0 : soloPane_; cam_.view = panes_[size_t(act_)].view;
}

// standard directions (pivot → camera) for Shift-snapping: 4 sides, 4 three-quarter views at eye level and from 30°
// above / below, top and bottom (88°, so the camera never looks exactly straight down)
void Viewport::snapToNearestView() {
  struct SV { QString n; Vec3 e; }; static std::vector<SV> L;
  if (L.empty()) {
    const char* names[8] = {"Front", "3/4 front-right", "Right", "3/4 back-right", "Back", "3/4 back-left", "Left", "3/4 front-left"};
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
  const char* names[3][2] = {{"Right", "Left"}, {"Top", "Bottom"}, {"Front", "Back"}}; QString nm = names[k][s > 0 ? 0 : 1];
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
