#include "app/ViewFrame.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QCursor>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QJsonArray>
#include <array>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace hm {

namespace {
constexpr int kSize = 28, kGap = 4, kMargin = 8;
constexpr double kReach = 100;   // (px round the control where it shows)

class FBtn : public QWidget {
public:
  enum Kind { WireOnly, MeshWire, Grid, Axes, Display, Pdo, Vis };
  FBtn(QWidget* parent, Kind k) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint), kind(k) {
    setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setMouseTracking(true);
    setFixedSize(kSize, kSize); setCursor(Qt::PointingHandCursor);
  }
  Kind kind; bool on = false, dragEnabled = false;
  std::function<void()> click;
  std::function<void(double dx, double dy)> drag;          // (px since the last move, along the one axis the drag took)
  std::function<void(const QPoint&)> context;
  bool busy() const { return down_; }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const QColor ink(0xd8, 0xd8, 0xd8);
    p.setPen(Qt::NoPen); p.setBrush(QColor(0xd0, 0xd0, 0xd0, on ? 95 : 38)); p.drawRoundedRect(QRectF(1, 1, kSize - 2, kSize - 2), 6, 6);
    p.setBrush(Qt::NoBrush); QPen pen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); p.setPen(pen);
    const QPointF c(kSize / 2.0, kSize / 2.0);
    switch (kind) {
      case WireOnly: case MeshWire: {                       // (a cube: wire alone / over its shaded sides)
        QPolygonF hex; for (int i = 0; i < 6; i++) hex << c + QPointF(std::cos(M_PI / 6 + i * M_PI / 3) * 8.5, std::sin(M_PI / 6 + i * M_PI / 3) * 8.5);
        if (kind == MeshWire) { p.setBrush(QColor(0xd8, 0xd8, 0xd8, 70)); p.setPen(Qt::NoPen); p.drawPolygon(hex); p.setBrush(Qt::NoBrush); p.setPen(pen); }
        p.drawPolygon(hex); p.drawLine(c, hex[1]); p.drawLine(c, hex[3]); p.drawLine(c, hex[5]);
        if (kind == WireOnly) { QPen d = pen; d.setStyle(Qt::DotLine); p.setPen(d); p.drawLine(c, hex[1]); }
        break; }
      case Grid: {
        // (a plain grid: three lines each way inside a square)
        const double h = 8; p.drawRect(QRectF(c.x() - h, c.y() - h, 2 * h, 2 * h));
        p.drawLine(QPointF(c.x() - h / 3, c.y() - h), QPointF(c.x() - h / 3, c.y() + h)); p.drawLine(QPointF(c.x() + h / 3, c.y() - h), QPointF(c.x() + h / 3, c.y() + h));
        p.drawLine(QPointF(c.x() - h, c.y() - h / 3), QPointF(c.x() + h, c.y() - h / 3)); p.drawLine(QPointF(c.x() - h, c.y() + h / 3), QPointF(c.x() + h, c.y() + h / 3));
        break; }
      case Axes: { p.drawLine(c + QPointF(-6, 6), c + QPointF(-6, -8)); p.drawLine(c + QPointF(-6, 6), c + QPointF(8, 6)); p.drawLine(c + QPointF(-6, 6), c + QPointF(4, -2));
        p.setBrush(ink); p.drawEllipse(c + QPointF(-6, 6), 1.6, 1.6); break; }
      case Display: { QPainterPath e; e.moveTo(c + QPointF(-9, 0)); e.quadTo(c + QPointF(0, -10), c + QPointF(9, 0)); e.quadTo(c + QPointF(0, 10), c + QPointF(-9, 0)); p.drawPath(e); p.drawEllipse(c, 3.2, 3.2); break; }
      case Vis: {                                              // (an eye over two layers: what this window shows)
        QPainterPath e; e.moveTo(c + QPointF(-8, -3)); e.quadTo(c + QPointF(0, -11), c + QPointF(8, -3)); e.quadTo(c + QPointF(0, 5), c + QPointF(-8, -3)); p.drawPath(e); p.drawEllipse(c + QPointF(0, -3), 2.4, 2.4);
        p.drawLine(c + QPointF(-7, 6), c + QPointF(7, 6)); p.drawLine(c + QPointF(-5, 9.5), c + QPointF(5, 9.5)); break; }
      case Pdo: { QFont f = font(); f.setPixelSize(10); f.setBold(true); p.setFont(f); p.drawText(rect(), Qt::AlignCenter, "PDO"); break; }
    }
  }
  void mousePressEvent(QMouseEvent* e) override { down_ = true; btn_ = e->button(); last_ = e->globalPosition(); acc_ = {}; axis_ = 0; dragging_ = false; }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (!down_ || btn_ != Qt::LeftButton || !dragEnabled) return;
    const QPointF d = e->globalPosition() - last_; last_ = e->globalPosition();
    if (!axis_) { acc_ += d; if (std::abs(acc_.x()) < 4 && std::abs(acc_.y()) < 4) return; axis_ = std::abs(acc_.y()) >= std::abs(acc_.x()) ? 2 : 1; dragging_ = true; if (drag) drag(axis_ == 1 ? acc_.x() : 0, axis_ == 2 ? acc_.y() : 0); return; }
    if (drag) drag(axis_ == 1 ? d.x() : 0, axis_ == 2 ? d.y() : 0);
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (!down_) return; down_ = false;
    if (btn_ == Qt::RightButton) { if (context && rect().contains(e->position().toPoint())) context(e->globalPosition().toPoint()); }
    else if (btn_ == Qt::LeftButton && !dragging_ && rect().contains(e->position().toPoint()) && click) click();
    dragging_ = false;
  }
private:
  bool down_ = false, dragging_ = false; Qt::MouseButton btn_ = Qt::NoButton; QPointF last_, acc_; int axis_ = 0;
};
}  // namespace

// (each pane of the viewport has its own set of controls at its top right corner, and its own grid and axes settings)
struct ViewFrame::Impl {
  struct Set { FBtn *wireOnly = nullptr, *meshWire = nullptr, *grid = nullptr, *axes = nullptr, *room = nullptr, *vis = nullptr; std::vector<FBtn*> all; std::vector<double> alpha; double lastFloorOp = 1, lastAxOp = 0.6; };
  QWidget* host; Viewport* vp; Hooks h;
  Set S[4]; int stripPane = -1;

  // (each view's wire: "paneWire" [[only, wire] × 4]; an older project's one setting for all views — wireOnly / showMeshWire — read as
  // every view's until a view's button is clicked)
  std::array<Viewport::PaneWire, 4> wires() const {
    const QJsonObject v = h.view(); std::array<Viewport::PaneWire, 4> o{};
    const QJsonArray a = v.value("paneWire").toArray();
    for (int p = 0; p < 4; p++) { if (v.contains("paneWire")) { const QJsonArray q = a.at(p).toArray(); o[size_t(p)] = {q.at(0).toBool(false), q.at(1).toBool(false)}; }
      else o[size_t(p)] = {v.value("wireOnly").toBool(false), v.value("showMeshWire").toBool(false) && !v.value("wireOnly").toBool(false)}; }
    return o; }
  void setWire(int p, bool only, bool wire) {
    auto o = wires(); o[size_t(p & 3)] = {only, wire && !only};
    QJsonArray a; for (const auto& w : o) a.append(QJsonArray{w.only, w.wire});
    const QJsonObject v = h.view(); const bool old = !v.contains("paneWire");
    h.setView("paneWire", a);
    if (old) { if (v.value("wireOnly").toBool(false)) h.setView("wireOnly", false); if (v.value("showMeshWire").toBool(false)) h.setView("showMeshWire", false); }   // (the old one-for-all taken over by the views)
  }
  void sync() {
    QWidget* w = host->window();
    const bool vis = host->isVisible() && w->isVisible() && !w->isMinimized() && QApplication::activeWindow() != nullptr && !QApplication::activeModalWidget();   // (a modal window open — the reference canvas, a dialog: nothing of the viewport's frame over it, it could not be clicked)
    const QString room_ = vp->room(); const bool topo = room_ == "topo", hair = room_ == "hair";
    const QJsonObject v = h.view();
    (void)v; const auto pw = wires();
    if (pw != vp->paneWires) { vp->paneWires = pw; vp->requestUpdate(); }
    const bool showRoom = topo, paintRoom = room_ == "paint";
    const QPoint cur = QCursor::pos(); const auto shown = vp->shownPanes();
    for (int p = 0; p < 4; p++) {
      Set& s = S[p]; const bool inUse = std::find(shown.begin(), shown.end(), p) != shown.end() && vp->paneCamera(p).view != "uv";
      const Viewport::Floor& F = vp->paneFloor(p);
      s.wireOnly->on = pw[size_t(p)].only; s.meshWire->on = pw[size_t(p)].wire && !pw[size_t(p)].only; s.grid->on = F.op > 0.001; s.axes->on = F.ax > 0.001;
      s.room->on = topo && vp->tdisp.pdoOn;
      s.vis->on = paintRoom && (vp->paneHid(p) || (vp->paneVis(p).sync && p != vp->mainPane()));
      const QRect pr = vp->paneRect(p); const QRect g(host->mapToGlobal(pr.topLeft()), pr.size());
      int x = g.right() - kMargin - kSize + 1;
      for (size_t i = 0; i < s.all.size(); i++) {
        FBtn* b = s.all[i];
        const bool want = vis && inUse && !(b == s.room && !showRoom) && b != s.vis && !(paintRoom && (b == s.room || b == s.wireOnly));   // (the Paint room: the wire, the floor, the axes — as the other rooms; a window's visibility set is picked in the camera menu, not here)
        if (!want) { s.alpha[i] = 0; if (b->isVisible()) b->hide(); continue; }
        const QRect r(QPoint(x, g.top() + kMargin), QSize(kSize, kSize)); x -= kSize + kGap;
        if (b->geometry() != r) b->setGeometry(r);
        const double dx = std::max({double(r.left() - cur.x()), 0.0, double(cur.x() - r.right())}), dy = std::max({double(r.top() - cur.y()), 0.0, double(cur.y() - r.bottom())});
        const double d = std::hypot(dx, dy);
        double t = d >= kReach ? 0 : 1 - d / kReach; t = t * t * (3 - 2 * t);   // (smooth: full at the control, none at 100 px)
        if (QApplication::mouseButtons() != Qt::NoButton && !b->busy()) t = 0;   // (a stroke or an orbit near the edge: no flashing)
        s.alpha[i] += (t - s.alpha[i]) * 0.4; if (std::abs(t - s.alpha[i]) < 0.01) s.alpha[i] = t;
        if (s.alpha[i] < 0.02 && !b->busy()) { if (b->isVisible()) b->hide(); continue; }
        b->setWindowOpacity(std::clamp(s.alpha[i], 0.0, 1.0) * 0.9); if (!b->isVisible()) b->show(); b->update();
      }
    }
    // (the Hair room's display settings: a panel under the pane's buttons, open while the pointer is on them — or on it)
    if (QWidget* ds = h.strip ? h.strip() : nullptr) {
      ds->setAttribute(Qt::WA_ShowWithoutActivating);
      bool want = false; int at = stripPane;
      if (vis && hair) for (int p : shown) {
        const QRect pr = vp->paneRect(p), g(host->mapToGlobal(pr.topLeft()), pr.size());
        const int bw = 4 * (kSize + kGap);
        const QRect row(QPoint(g.right() - kMargin - bw + kGap, g.top() + kMargin), QSize(bw, kSize));
        const bool onRow = row.adjusted(-10, -10, 10, 10).contains(cur) && (QApplication::mouseButtons() == Qt::NoButton || ds->isVisible());
        const bool onStrip = ds->isVisible() && p == stripPane && ds->geometry().adjusted(-10, -10, 10, 10).contains(cur);
        const bool busy = ds->isVisible() && p == stripPane && QApplication::mouseButtons() != Qt::NoButton;   // (a slider being dragged: the panel stays)
        if (onRow || onStrip || busy) { want = true; at = p; break; }
      }
      if (want) {
        stripPane = at; const QRect pr = vp->paneRect(at), g(host->mapToGlobal(pr.topLeft()), pr.size());
        if (!ds->isVisible() && h.syncStrip) h.syncStrip();
        ds->adjustSize(); ds->move(g.right() - kMargin - ds->width() + 1, g.top() + kMargin + kSize + 6);
        if (!ds->isVisible()) { ds->show(); ds->raise(); }
      } else if (ds->isVisible()) ds->hide();
    }
  }
};

ViewFrame::ViewFrame(QWidget* host, Viewport* vp, Hooks hooks) : QObject(host), d_(new Impl{host, vp, std::move(hooks), {}}) {
  Impl* d = d_; QWidget* win = host->window();
  for (int p = 0; p < 4; p++) {
    Impl::Set& s = d->S[p];
    auto mk = [&](FBtn::Kind k, const QString& tip) { auto* b = new FBtn(win, k); b->setToolTip(tip); s.all.push_back(b); s.alpha.push_back(0); return b; };
    s.wireOnly = mk(FBtn::WireOnly, tr("Wireframe view — this view only: the meshes that have a wire shown as their wire alone (their polygons go fully transparent); Head Collision's objects stay as they are. Off by default; turns the next button off"));
    s.meshWire = mk(FBtn::MeshWire, tr("The wire of the meshes that have it switched on, over their polygons — this view only; not on Head Collision's objects. Off by default; turns the wireframe view off"));
    s.grid = mk(FBtn::Grid, tr("This view's grids, the floor included: click — on / off; hold and drag up / down — opacity, left / right — position. RMB — reset. Each view has its own"));
    s.axes = mk(FBtn::Axes, tr("This view's world axes: click — on / off; hold and drag up / down — opacity. RMB — reset. Each view has its own"));
    s.room = mk(FBtn::Display, tr("Display"));
    s.vis = mk(FBtn::Vis, tr("What this window shows (the Paint room): the scene as it is, or a visibility set — a part alone, to paint it with nothing in the way; its camera with the main view's. The sets are made in the camera menu (Tab)"));
    s.vis->click = [d, p] { Viewport* vp = d->vp; QMenu m(d->host->window()); const Viewport::PaneVis pv = vp->paneVis(p); const bool has = bool(vp->paneHid(p));
      QAction* sc = m.addAction(QObject::tr("As the scene (the main view)")); sc->setCheckable(true); sc->setChecked(!has);
      std::vector<std::pair<QAction*, QString>> sets; for (const QString& n : vp->visSetNames()) { QAction* a = m.addAction(QObject::tr("Set: %1").arg(n)); a->setCheckable(true); a->setChecked(has && pv.set == n); sets.push_back({a, n}); }
      m.addSeparator(); QAction* sy = m.addAction(QObject::tr("Camera follows the main view")); sy->setCheckable(true); sy->setChecked(pv.sync); sy->setEnabled(p != vp->mainPane());
      m.addSeparator(); QAction* nw = m.addAction(QObject::tr("New set of what is shown now"));
      QAction* r = m.exec(QCursor::pos()); if (!r) return; Viewport::PaneVis v = pv;
      if (r == sc) v.set.clear(); else if (r == sy) v.sync = sy->isChecked(); else if (r == nw) { ui::toast(QObject::tr("Visibility set %1 made").arg(vp->visSetSave(QString()))); return; }
      else for (auto& [a, n] : sets) if (a == r) v.set = n;
      vp->setPaneVis(p, v); };
    Impl::Set* ps = &s;
    s.wireOnly->click = [d, p] { const bool on = d->wires()[size_t(p)].only; d->setWire(p, !on, false); };   // (this view only)
    s.meshWire->click = [d, p] { const auto w = d->wires()[size_t(p)]; d->setWire(p, false, !(w.wire && !w.only)); };
    s.grid->dragEnabled = s.axes->dragEnabled = true;
    s.grid->click = [d, p, ps] { auto F = d->vp->paneFloor(p); if (F.op > 0.001) { ps->lastFloorOp = F.op; F.op = 0; } else F.op = ps->lastFloorOp > 0.001 ? ps->lastFloorOp : 1; d->vp->setPaneFloor(p, F); };
    s.grid->drag = [d, p](double dx, double dy) { auto F = d->vp->paneFloor(p);
      if (dy != 0) { F.op = std::clamp(F.op - dy / 150.0, 0.0, 1.0); ui::hud(QObject::tr("Grid opacity %1%").arg(int(std::lround(F.op * 100))), 800); }
      if (dx != 0) { F.dy = std::clamp(F.dy + dx * 0.004, -0.5, 0.5); ui::hud(QObject::tr("Grid position %1").arg(F.dy, 0, 'f', 3), 800); }
      d->vp->setPaneFloor(p, F); };
    s.grid->context = [d, p](const QPoint& at) { QMenu m(d->host->window()); auto F0 = d->vp->paneFloor(p);
      QAction* a = m.addAction(QObject::tr("Opacity to the default")); QAction* b = m.addAction(QObject::tr("Position to the default")); QAction* c = m.addAction(QObject::tr("Both to the default"));
      m.addSeparator(); QAction* z = m.addAction(QObject::tr("Floor under the model (at its lowest point)")); z->setCheckable(true); z->setChecked(!F0.zero);
      z->setToolTip(QObject::tr("Off (the default) — the floor at the world's zero height; on — fitted under the model"));
      QAction* r = m.exec(at); if (!r) return; auto F = d->vp->paneFloor(p);
      if (r == a || r == c) F.op = 1; if (r == b || r == c) F.dy = 0; if (r == z) F.zero = !z->isChecked();
      d->vp->setPaneFloor(p, F); };
    s.axes->click = [d, p, ps] { auto F = d->vp->paneFloor(p); if (F.ax > 0.001) { ps->lastAxOp = F.ax; F.ax = 0; } else F.ax = ps->lastAxOp > 0.001 ? ps->lastAxOp : 0.6; d->vp->setPaneFloor(p, F); };
    s.axes->drag = [d, p](double, double dy) { if (dy == 0) return; auto F = d->vp->paneFloor(p); F.ax = std::clamp(F.ax - dy / 150.0, 0.0, 1.0); ui::hud(QObject::tr("Axes opacity %1%").arg(int(std::lround(F.ax * 100))), 800); d->vp->setPaneFloor(p, F); };
    s.axes->context = [d, p](const QPoint& at) { QMenu m(d->host->window()); QAction* a = m.addAction(QObject::tr("Opacity to the default")); if (m.exec(at) == a) { auto F = d->vp->paneFloor(p); F.ax = 0.6; d->vp->setPaneFloor(p, F); } };
    // (the room's own control: the Hair room's display strip; the Topology room's PDO — RMB there: its display panel)
    FBtn* room = s.room;
    s.room->click = [d, room] {
      if (d->vp->room() == "topo") { d->vp->tdisp.pdoOn = !d->vp->tdisp.pdoOn; d->vp->saveTopoPrefs(); d->vp->topoDisplayChanged(); }
      else if (d->h.openHairDisplay) d->h.openHairDisplay(room->geometry().bottomRight()); };
    s.room->context = [d, room](const QPoint&) { if (d->vp->room() == "topo" && d->h.openTopoDisplay) d->h.openTopoDisplay(room->geometry().bottomRight()); };
  }
  auto* t = new QTimer(this); t->setInterval(33);
  QObject::connect(t, &QTimer::timeout, this, [d] {
    // (the room's control changes with the room)
    const bool topo = d->vp->room() == "topo";
    const FBtn::Kind want = topo ? FBtn::Pdo : FBtn::Display;
    for (auto& s : d->S) if (s.room->kind != want) { s.room->kind = want; s.room->setToolTip(topo ? QObject::tr("PDO on / off: the active geometry drawn nearer the camera so it covers the meshes it lies on. RMB — the display panel (its amount, shading, wire)") : QObject::tr("Display: X-ray, show / hide and opacity of anchors, curves, guides, the walls' edges, NURBS surfaces, modifiers")); }
    d->sync(); });
  t->start();
}

}  // namespace hm
