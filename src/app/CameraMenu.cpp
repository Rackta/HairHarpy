#include "app/CameraMenu.h"
#include "app/Overlays.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QCheckBox>
#include <QMenu>
#include <QWidgetAction>
#include <QDoubleSpinBox>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace hm {

namespace {
const char* VIEWS[] = {"persp", "ortho", "front", "right", "left", "back", "top"};
QString viewName(const QString& v) {
  return v == "uv" ? QObject::tr("UV Editor") : v == "persp" ? QObject::tr("Perspective") : v == "ortho" ? QObject::tr("Orthographic") : v == "front" ? QObject::tr("Front (ortho)") : v == "right" ? QObject::tr("Right (ortho)")
       : v == "left" ? QObject::tr("Left (ortho)") : v == "back" ? QObject::tr("Back (ortho)") : QObject::tr("Top (ortho)");
}
const std::map<QString, std::vector<std::array<double, 4>>>& layouts() {
  static const std::map<QString, std::vector<std::array<double, 4>>> L = {
    {"2", {{0, 0, 0.5, 1}, {0.5, 0, 0.5, 1}}}, {"2+1", {{0, 0, 0.5, 0.5}, {0.5, 0, 0.5, 0.5}, {0, 0.5, 1, 0.5}}},
    {"1+2", {{0, 0, 1, 0.5}, {0, 0.5, 0.5, 0.5}, {0.5, 0.5, 0.5, 0.5}}}, {"4", {{0, 0, 0.5, 0.5}, {0.5, 0, 0.5, 0.5}, {0, 0.5, 0.5, 0.5}, {0.5, 0.5, 0.5, 0.5}}}};
  return L;
}

// a chip / a window of the miniature: click, drag (its text), drop. The drag is the menu's own — a label follows the
// pointer, the window under it lit, the release drops — not the system's (on Windows its loop starves the viewport's
// drawing and the whole program stalls while it lasts)
// a view's grid switch: "#", a click shows / hides it, a double click opens its step
class GridBtn : public QPushButton {
public:
  std::function<void()> dbl;
  GridBtn() : QPushButton("#") { setCheckable(true); setFixedSize(22, 22); setFocusPolicy(Qt::NoFocus); setStyleSheet("QPushButton{padding:0;font-weight:600} QPushButton:checked{background:#3d5f99;color:#fff}"); }
protected:
  void mouseDoubleClickEvent(QMouseEvent* e) override { QPushButton::mouseDoubleClickEvent(e); if (dbl) QTimer::singleShot(0, this, [this] { dbl(); }); }
};
class Chip : public QLabel {
public:
  std::function<void()> click; std::function<void(const QString&)> drop; QString dragText;
  explicit Chip(const QString& t, QWidget* parent = nullptr) : QLabel(t, parent) { setCursor(Qt::PointingHandCursor); setAlignment(Qt::AlignCenter); }
  ~Chip() override { if (ghost_) ghost_->deleteLater(); }
protected:
  void mousePressEvent(QMouseEvent* e) override { press_ = e->position().toPoint(); dragged_ = false; }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (dragText.isEmpty() || !(e->buttons() & Qt::LeftButton)) return;
    if (!dragged_ && (e->position().toPoint() - press_).manhattanLength() < 6) return;
    const QPoint g = e->globalPosition().toPoint();
    if (!dragged_) { dragged_ = true; setCursor(Qt::ClosedHandCursor);
      ghost_ = new QLabel(text(), nullptr, Qt::ToolTip | Qt::FramelessWindowHint); ghost_->setAttribute(Qt::WA_TransparentForMouseEvents); ghost_->setAttribute(Qt::WA_ShowWithoutActivating);
      ghost_->setStyleSheet("background:#3d4b66;color:#fff;border:1px solid #6f9fe8;border-radius:4px;padding:3px 8px"); ghost_->adjustSize(); ghost_->show(); }
    ghost_->move(g + QPoint(12, 10));
    Chip* t = target(g); if (t != over_) { lit(over_, false); over_ = t; lit(over_, true); }
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (!dragged_) { if (rect().contains(e->position().toPoint()) && click) click(); return; }
    dragged_ = false; setCursor(Qt::PointingHandCursor); if (ghost_) { ghost_->deleteLater(); ghost_ = nullptr; }
    Chip* t = target(e->globalPosition().toPoint()); lit(over_, false); over_ = nullptr;
    if (t && t->drop) { auto fn = t->drop; const QString d = dragText; QTimer::singleShot(0, [fn, d] { fn(d); }); }   // (it may rebuild the menu — this chip with it)
  }
private:
  // the chip under the pointer that takes a drop (not this one)
  Chip* target(const QPoint& g) const { for (QWidget* w = QApplication::widgetAt(g); w; w = w->parentWidget()) if (auto* c = dynamic_cast<Chip*>(w)) return c != this && c->drop ? c : nullptr; return nullptr; }
  static void lit(Chip* c, bool on) { if (!c) return; c->setProperty("over", on); c->style()->unpolish(c); c->style()->polish(c); }
  QPoint press_; bool dragged_ = false; QLabel* ghost_ = nullptr; QPointer<Chip> over_;
};

class CamMenu : public QFrame {
public:
  CamMenu(QWidget* parent, Viewport* vp) : QFrame(parent, Qt::Popup), vp_(vp) {
    setAttribute(Qt::WA_DeleteOnClose); setObjectName("camMenu");
    setStyleSheet("#camMenu{background:#262626;border:1px solid #555;border-radius:6px} QLabel[h5=\"true\"]{color:#9a9a9a;font-size:11px;margin-top:6px}"
                  " Chip, QLabel[chip=\"true\"]{background:#333;border:1px solid #4a4a4a;border-radius:10px;padding:2px 8px;color:#ccc}"
                  " QLabel[cur=\"true\"]{background:#2c3a52;border-color:#6a9ae0;color:#fff} QLabel[over=\"true\"]{border-color:#e0a030}"
                  " QLabel[slot=\"true\"]{background:#1e1e1e;border:1px solid #4a4a4a;border-radius:3px;color:#bbb;font-size:11px} QLabel[slot=\"true\"][act=\"true\"]{border-color:#4a7fd6;color:#fff}");
    v_ = new QVBoxLayout(this); v_->setContentsMargins(10, 8, 10, 10); v_->setSpacing(4);
    build();
  }
  void rebuild() { QTimer::singleShot(0, this, [this] { QLayoutItem* it; while ((it = v_->takeAt(0))) { if (it->widget()) it->widget()->deleteLater(); else if (it->layout()) clearLayout(it->layout()); delete it; } build(); adjustSize(); }); }
private:
  static void clearLayout(QLayout* l) { QLayoutItem* it; while ((it = l->takeAt(0))) { if (it->widget()) it->widget()->deleteLater(); else if (it->layout()) clearLayout(it->layout()); delete it; } }
  QLabel* h5(const QString& t) { auto* l = new QLabel(t); l->setProperty("h5", true); return l; }
  QPushButton* b(const QString& lab, const QString& tip, std::function<void()> fn, bool on = false) {
    auto* x = new QPushButton(lab); x->setToolTip(tip); x->setCheckable(on); x->setChecked(on); x->setFocusPolicy(Qt::NoFocus);
    QObject::connect(x, &QPushButton::clicked, this, [this, fn] { fn(); rebuild(); }); return x;
  }
  void build() {
    const Camera& c = vp_->camera(); auto shown = vp_->shownPanes();
    int idx = int(std::find(shown.begin(), shown.end(), vp_->activePane()) - shown.begin()) + 1;
    auto* t = h5(vp_->split() ? QObject::tr("Active view — window %1").arg(idx) : QObject::tr("View")); t->setStyleSheet("margin-top:0"); v_->addWidget(t);
    auto* r1 = new QHBoxLayout; v_->addLayout(r1);
    r1->addWidget(b(QObject::tr("↺ Reset camera"), QObject::tr("Upright camera framing the whole model"), [this] { vp_->camera().reset(); vp_->liveView(); }));
    r1->addWidget(b(QObject::tr("Perspective"), QObject::tr("Perspective projection (free camera)"), [this] { vp_->setViewMode("persp"); }, c.view == "persp"));
    r1->addWidget(b(QObject::tr("Orthographic"), QObject::tr("Orthographic projection, free camera (orbits like the perspective one)"), [this] { vp_->setViewMode("ortho"); }, c.view == "ortho"));
    auto* r1b = new QHBoxLayout; v_->addLayout(r1b);
    r1b->addWidget(b(QObject::tr("⧉ Copy position"), QObject::tr("Copy where the camera is and where it looks (position, target, rotation, projection) to the clipboard — paste it next to a screenshot"),
                     [this] { QApplication::clipboard()->setText(camPosText(vp_)); ui::toast(QObject::tr("Camera position copied — paste it next to the screenshot")); }));
    r1b->addWidget(b(QObject::tr("⎘ Paste position"), QObject::tr("Put the camera where a copied position says (from the clipboard)"), [this] {
      QString s = QApplication::clipboard()->text();
      if (!s.contains("HMC-CAM")) {
        QDialog d(parentWidget()); d.setWindowTitle(QObject::tr("Paste a camera position (HMC-CAM …):")); auto* l = new QVBoxLayout(&d); auto* te = new QPlainTextEdit; l->addWidget(te);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); l->addWidget(bb);
        QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept); QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted) return; s = te->toPlainText();
      }
      if (applyCamPos(vp_, s)) ui::toast(QObject::tr("Camera moved to the pasted position"));
    }));
    v_->addWidget(h5(QObject::tr("Views — click to use, drag into a split window")));
    auto* r2 = new QGridLayout; r2->setSpacing(4); v_->addLayout(r2); int k = 0;
    for (const char* vv : VIEWS) {
      QString view = vv; auto* ch = new Chip(viewName(view)); ch->setProperty("chip", true); ch->setProperty("cur", c.view == view); ch->dragText = "view:" + view;
      ch->click = [this, view] { vp_->setViewMode(view); rebuild(); };
      // (beside the view: its grid — the floor for the perspective and the top, a wall for the side views; a click shows /
      // hides it, a double click — its step; the element snap takes its nodes)
      auto* cell = new QWidget; auto* ch2 = new QHBoxLayout(cell); ch2->setContentsMargins(0, 0, 0, 0); ch2->setSpacing(2); ch2->addWidget(ch, 1);
      if (view != "ortho") {
        auto* gb = new GridBtn; gb->setChecked(vp_->viewGrid(view).on);
        gb->setToolTip((view == "persp" || view == "top" ? QObject::tr("The floor's grid of this view") : QObject::tr("The wall's grid of this view (through the origin)")) + QObject::tr(" — a click: shown / hidden; a double click: its step"));
        QObject::connect(gb, &QPushButton::toggled, this, [this, view](bool x) { auto G = vp_->viewGrid(view); G.on = x; vp_->setViewGrid(view, G); });
        gb->dbl = [this, view, gb] {
          QMenu m; auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(8, 4, 8, 4);
          h->addWidget(new QLabel(QObject::tr("%1 — grid step, cm").arg(viewName(view))));
          auto* st = new QDoubleSpinBox; st->setDecimals(3); st->setRange(0.001, 1000); st->setValue(vp_->viewGrid(view).step); h->addWidget(st);
          QObject::connect(st, &QDoubleSpinBox::valueChanged, this, [this, view](double x) { auto G = vp_->viewGrid(view); G.step = x; G.on = true; vp_->setViewGrid(view, G); });
          auto* wa = new QWidgetAction(&m); wa->setDefaultWidget(w); m.addAction(wa); st->setFocus(); m.exec(gb->mapToGlobal(QPoint(0, gb->height())));
          gb->setChecked(vp_->viewGrid(view).on); };
        ch2->addWidget(gb); }
      r2->addWidget(cell, k / 4, k % 4); k++;
    }
    if (vp_->room() == "topo") {                                // (the Topology room: the UV Editor — the active mesh's UV in this window)
      auto* ch = new Chip(viewName("uv")); ch->setProperty("chip", true); ch->setProperty("cur", c.view == "uv"); ch->dragText = "view:uv";
      ch->setToolTip(QObject::tr("The UV Editor: the selected mesh's UV in this window (the UV mode on: picked and moved there by the mode). Drag it into a split window"));
      ch->click = [this] { vp_->setViewMode("uv"); rebuild(); };
      r2->addWidget(ch, k / 4, k % 4); k++;
    }
    v_->addWidget(h5(QObject::tr("Split screen")));
    auto* r3 = new QHBoxLayout; v_->addLayout(r3);
    auto* on = new QCheckBox(QObject::tr("On")); on->setChecked(vp_->split()); r3->addWidget(on);
    QObject::connect(on, &QCheckBox::toggled, this, [this](bool x) { vp_->setSplit(x); rebuild(); });
    for (auto [key, lab, tip] : {std::tuple<const char*, const char*, const char*>{"2", "▯▯", "Two side by side"}, {"2+1", "⊤ 2+1", "Two on top, one below"}, {"1+2", "⊥ 1+2", "One on top, two below"}, {"4", "⊞ 4", "Four windows"}}) {
      QString k2 = key; r3->addWidget(b(QObject::tr(lab), QObject::tr(tip), [this, k2] { vp_->setSplitLayout(k2); }, vp_->splitLayout() == k2));
    }
    r3->addStretch(1);
    // the layout in miniature
    auto* box = new QWidget; box->setFixedSize(300, 150); box->setStyleSheet("background:#1a1a1a;border-radius:4px");
    const auto& L = layouts().at(vp_->splitLayout());
    for (size_t i = 0; i < L.size(); i++) {
      auto [x, y, w, h] = L[i]; int pi = int(i);
      auto* s = new Chip(QString("%1: %2").arg(i + 1).arg(viewName(vp_->paneCamera(pi).view)), box); s->setProperty("slot", true); s->setProperty("act", vp_->split() && pi == vp_->activePane());
      s->setGeometry(int(x * 300) + 3, int(y * 150) + 3, int(w * 300) - 6, int(h * 150) - 6); s->dragText = QString("slot:%1").arg(i); s->setWordWrap(true);
      s->drop = [this, pi](const QString& d) {
        if (d.startsWith("view:")) vp_->setPaneView(pi, d.mid(5));
        else if (d.startsWith("slot:")) { int j = d.mid(5).toInt(); if (j != pi) vp_->swapPanes(pi, j); }
        rebuild();
      };
      s->click = [this, pi] { if (vp_->split()) { vp_->activatePane(pi); rebuild(); } };
    }
    v_->addWidget(box);
    auto* note = new QLabel(QObject::tr("The window under the pointer is the active one: navigation, F, Reset and the views apply to it.")); note->setWordWrap(true); note->setStyleSheet("color:#8a8a8a;font-size:11px"); note->setMaximumWidth(300);
    v_->addWidget(note);
  }
  Viewport* vp_; QVBoxLayout* v_;
};
QPointer<CamMenu> g_menu;
}  // namespace

void openCameraMenu(QWidget* parent, Viewport* vp, const QPoint& global) {
  if (g_menu) g_menu->close();
  g_menu = new CamMenu(parent, vp); g_menu->adjustSize(); g_menu->move(global); g_menu->show();
}
void closeCameraMenu() { if (g_menu) g_menu->close(); }
bool cameraMenuOpen() { return g_menu && g_menu->isVisible(); }
bool cameraMenuShot(const QString& png) { if (!g_menu) return false; QApplication::processEvents(); return g_menu->grab().save(png); }   // (checks)

// "HMC-CAM {…}": view, position, target (the orbit pivot), rotation (quaternion), direction, up, fov / zoom — rounded
QString camPosText(const Viewport* vp) {
  const Camera& c = const_cast<Viewport*>(vp)->camera();
  auto r = [](double v, int d) { double k = std::pow(10.0, d); return std::round(v * k) / k; };
  auto a3 = [&](const Vec3& v, int d) { return QJsonArray{r(v.x, d), r(v.y, d), r(v.z, d)}; };
  QJsonObject o{{"view", c.view}, {"pos", a3(c.pos, 3)}, {"target", a3(c.pivot, 3)}, {"quat", QJsonArray{r(c.quat.x, 5), r(c.quat.y, 5), r(c.quat.z, 5), r(c.quat.w, 5)}},
                {"dir", a3(c.dir(), 4)}, {"up", a3(c.upVec(), 4)}};
  if (!c.ortho()) o.insert("fov", r(c.fov, 3)); else o.insert("zoom", r(c.zoom, 3));
  return "HMC-CAM " + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
bool applyCamPos(Viewport* vp, const QString& text) {
  auto m = QRegularExpression("HMC-CAM\\s*(\\{.*\\})", QRegularExpression::DotMatchesEverythingOption).match(text);
  if (!m.hasMatch()) { ui::toast(QObject::tr("No camera position there (it starts with HMC-CAM)")); return false; }
  QJsonObject o = QJsonDocument::fromJson(m.captured(1).toUtf8()).object();
  if (o.isEmpty()) { ui::toast(QObject::tr("The camera position is damaged")); return false; }
  Camera& c = vp->camera();
  QString v = o.value("view").toString(); if (!v.isEmpty() && v != c.view) vp->setViewMode(v);
  auto vec = [](const QJsonValue& x) { QJsonArray a = x.toArray(); return Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()); };
  if (o.value("pos").isArray()) c.pos = vec(o.value("pos"));
  if (o.value("quat").isArray()) { QJsonArray q = o.value("quat").toArray(); c.quat = Quat{q[0].toDouble(), q[1].toDouble(), q[2].toDouble(), q[3].toDouble()}.normalized(); }
  else if (o.value("target").isArray()) c.lookAt(vec(o.value("target")));
  if (o.value("target").isArray()) c.pivot = vec(o.value("target"));
  if (!c.ortho() && o.value("fov").isDouble()) c.fov = o.value("fov").toDouble();
  if (c.ortho() && o.value("zoom").isDouble()) c.zoom = o.value("zoom").toDouble();
  c.up = c.view == "top" ? c.quat.rotate({0, 1, 0}) : Vec3(0, 1, 0);
  vp->liveView();
  return true;
}

// ---------- the overlay: pane labels, the view axes ----------
class ViewOverlay : public QObject {
public:
  ViewOverlay(QWidget* container, Viewport* vp) : QObject(container), host_(container), vp_(vp) {
    labels_ = new Labels(this); axes_ = new Axes(this); credits_ = new Credits(this); fps_ = new Fps(this); stats_ = new Stats(this);
    auto* t = new QTimer(this); t->setInterval(33); QObject::connect(t, &QTimer::timeout, this, [this] { sync(); }); t->start();
  }
  void sync() {
    QWidget* w = host_->window();
    bool show = host_->isVisible() && !w->isMinimized() && w->isVisible();
    QRect g(host_->mapToGlobal(QPoint(0, 0)), host_->size());
    if (!show) { labels_->hide(); axes_->hide(); credits_->hide(); fps_->hide(); stats_->hide(); return; }
    stats_->sync(g);
    credits_->sync(g.adjusted(0, 0, 0, stats_->isVisible() && overlays::cfg().statsPos.x() < 0 ? -(stats_->height() + 6) : 0));   // (the credits above the model's card, while it is in its corner)
    fps_->sync(g);
    QString sig = QString("%1,%2,%3,%4|").arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height());
    for (int i : vp_->shownPanes()) { QRect r = vp_->paneRect(i); sig += QString("%1:%2,%3,%4,%5:%6;").arg(i).arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(vp_->paneLabel(i)); }
    for (auto& l : vp_->viewLabels()) sig += QString("%1,%2,%3;").arg(int(l.p.x())).arg(int(l.p.y())).arg(l.text);
    const Quat& q = vp_->camera().quat; QString qs = QString("%1,%2,%3,%4").arg(q.x, 0, 'f', 4).arg(q.y, 0, 'f', 4).arg(q.z, 0, 'f', 4).arg(q.w, 0, 'f', 4);
    if (sig != sig_) { sig_ = sig; labels_->setGeometry(g); labels_->syncMask(); labels_->update(); if (!labels_->isVisible()) labels_->show(); }
    if (vp_->camera().view == "uv") { axes_->hide(); return; }   // (the UV Editor: no 3D axes)
    QRect ar(g.right() - 112 - 6, g.bottom() - 112 - 6, 112, 112);
    if (ar != axes_->geometry()) axes_->setGeometry(ar);
    if (qs != qsig_ || !axes_->isVisible()) { qsig_ = qs; axes_->update(); if (!axes_->isVisible()) axes_->show(); }
  }
private:
  // each window's label (its view, the isolation), top left
  class Labels : public QWidget {
  public:
    explicit Labels(ViewOverlay* o) : QWidget(o->host_->window(), Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint), o_(o) {
      setObjectName("viewLabels"); setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    // (off Windows: the window only where it paints — the size of the viewport and over it, it must not stand in the mouse's
    // way even where the window system doesn't let the input through)
    void syncMask() {
#ifndef Q_OS_WIN
      QFont f = font(); f.setPointSizeF(8.5); const QFontMetrics fm(f); QRegion r;
      auto panes = o_->vp_->shownPanes();
      for (int i : panes) { QRect pr = o_->vp_->paneRect(i), tr = fm.boundingRect(o_->vp_->paneLabel(i)).adjusted(-6, -3, 6, 3); tr.moveTopLeft(QPoint(pr.x() + 10, pr.y() + (panes.size() > 1 ? 6 : 8))); r += tr.adjusted(-2, -2, 2, 2); }
      f.setBold(true); const QFontMetrics fb(f);
      for (auto& l : o_->vp_->viewLabels()) { QRectF tr = QRectF(fb.boundingRect(l.text)).adjusted(-5, -2, 5, 2); tr.moveCenter(l.p); r += tr.toAlignedRect().adjusted(-2, -2, 2, 2); }
      setMask(r.isEmpty() ? QRegion(0, 0, 1, 1) : r);   // (an empty mask is no mask: the whole window)
#endif
    }
  protected:
    void paintEvent(QPaintEvent*) override {
      QPainter p(this); p.setRenderHint(QPainter::Antialiasing); QFont f = font(); f.setPointSizeF(8.5); p.setFont(f);
      auto panes = o_->vp_->shownPanes();
      for (int i : panes) {
        QRect r = o_->vp_->paneRect(i); QString t = o_->vp_->paneLabel(i);
        QRect tr = p.fontMetrics().boundingRect(t).adjusted(-6, -3, 6, 3); tr.moveTopLeft(QPoint(r.x() + 10, r.y() + (panes.size() > 1 ? 6 : 8)));
        p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 110)); p.drawRoundedRect(tr, 3, 3);
        p.setPen(QColor(0xb0, 0xb0, 0xb0)); p.drawText(tr, Qt::AlignCenter, t);
      }
      QFont b = f; b.setBold(true); p.setFont(b);   // (the active tool's words on the view: PatchTool — each curve's edges and mode)
      for (auto& l : o_->vp_->viewLabels()) {
        QRectF tr = QRectF(p.fontMetrics().boundingRect(l.text)).adjusted(-5, -2, 5, 2); tr.moveCenter(l.p);
        p.setPen(Qt::NoPen); p.setBrush(QColor(0x18, 0x10, 0x20, 190)); p.drawRoundedRect(tr, 3, 3);
        p.setPen(l.c); p.drawText(tr, Qt::AlignCenter, l.text);
      }
    }
  private:
    ViewOverlay* o_;
  };
  // the view axes: X / Y / Z as the camera sees them — the positive ends labelled, the negative ones dim, the nearer drawn
  // over the farther; a click on an end — the view from that axis
  class Axes : public QWidget {
  public:
    explicit Axes(ViewOverlay* o) : QWidget(o->host_->window(), Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint), o_(o) {
      setObjectName("viewAxes"); setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setMouseTracking(true);
    }
  protected:
    struct End { int k, s; QColor col; double x, y, z; };
    std::vector<End> ends() const {
      const Quat qi = o_->vp_->camera().quat.conj(); const double C = 56, R = 36; std::vector<End> l;
      const QColor cols[3] = {QColor("#e8474f"), QColor("#4cc94a"), QColor("#4f6ff0")};
      for (int k = 0; k < 3; k++) for (int s : {1, -1}) { Vec3 d(k == 0 ? s : 0, k == 1 ? s : 0, k == 2 ? s : 0); Vec3 v = qi.rotate(d); l.push_back({k, s, cols[k], C + v.x * R, C - v.y * R, v.z}); }
      std::sort(l.begin(), l.end(), [](const End& a, const End& b) { return a.z < b.z; });
      return l;
    }
    void paintEvent(QPaintEvent*) override {
      QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
      p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, hover_ ? 90 : 55)); p.drawEllipse(QPointF(56, 56), 54, 54);
      auto l = ends();
      for (auto& a : l) if (a.s > 0) { QColor c = a.col; c.setAlphaF(a.z < -0.2 ? 0.55 : 1); p.setPen(QPen(c, 2.5, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(56, 56), QPointF(a.x, a.y)); }
      QFont f = font(); f.setBold(true); f.setPointSizeF(7.5); p.setFont(f);
      for (auto& a : l) {
        double r = a.s > 0 ? 9 : 7; QColor c = a.col; if (a.s < 0) c.setAlphaF(0.55);
        p.setPen(Qt::NoPen); p.setBrush(c); p.drawRoundedRect(QRectF(a.x - r, a.y - r, r * 2, r * 2), 3, 3);
        if (a.s > 0) { p.setPen(QColor(0x10, 0x10, 0x10)); p.drawText(QRectF(a.x - r, a.y - r, r * 2, r * 2), Qt::AlignCenter, QString("XYZ"[a.k])); }
      }
    }
    void mousePressEvent(QMouseEvent* e) override {
      if (e->button() != Qt::LeftButton) return;
      auto l = ends(); const End* best = nullptr;
      for (auto it = l.rbegin(); it != l.rend(); ++it) { double r = it->s > 0 ? 9 : 7; if (std::abs(e->position().x() - it->x) <= r + 1 && std::abs(e->position().y() - it->y) <= r + 1) { best = &*it; break; } }
      if (best) o_->vp_->viewFromAxis(best->k, best->s);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
      auto l = ends(); QString tip;
      for (auto& a : l) { double r = a.s > 0 ? 9 : 7; if (std::abs(e->position().x() - a.x) <= r + 1 && std::abs(e->position().y() - a.y) <= r + 1) tip = QObject::tr("View from %1%2").arg(a.s > 0 ? "+" : "−").arg(QString("XYZ"[a.k])); }
      setToolTip(tip);
    }
    void enterEvent(QEnterEvent*) override { hover_ = true; update(); }
    void leaveEvent(QEvent*) override { hover_ = false; update(); }
  private:
    ViewOverlay* o_; bool hover_ = false;
  };
  // the credits of the open project (e.g. the author of an example's concept art): a card at the left edge, a link to the
  // post; ✕ folds it to a small tab (click — open again). Taken from the file only (credits [{ role, name, url, link }])
  class Credits : public QFrame {
  public:
    explicit Credits(ViewOverlay* o) : QFrame(o->host_->window(), Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint), o_(o) {
      setAttribute(Qt::WA_ShowWithoutActivating); setObjectName("credits");
      setStyleSheet("#credits{background:rgba(30,30,30,235);border:1px solid #4a4a4a;border-left:3px solid #5a8dee;border-radius:5px} QLabel{color:#ddd}");
      v_ = new QVBoxLayout(this); v_->setContentsMargins(10, 7, 26, 7); v_->setSpacing(1);
      x_ = new QPushButton("✕", this); x_->setFlat(true); x_->setFixedSize(18, 16); x_->setToolTip(QObject::tr("Fold")); x_->setStyleSheet("QPushButton{border:none;color:#888;font-size:10px} QPushButton:hover{color:#fff}");
      QObject::connect(x_, &QPushButton::clicked, this, [this] { min_ = true; build(); });
    }
    void sync(const QRect& host) {
      QJsonArray L = o_->vp_->scene()->fileRaw.value("credits").toArray(); QString sig = QString::fromUtf8(QJsonDocument(L).toJson(QJsonDocument::Compact));
      if (sig != sig_) { sig_ = sig; min_ = false; build(); }
      if (L.isEmpty()) { hide(); return; }
      adjustSize(); QPoint pos(host.left() + 10, host.bottom() - 10 - height());
      if (this->pos() != pos) move(pos);
      if (!isVisible()) show();
      x_->move(width() - 21, 3); x_->setVisible(!min_);
    }
  protected:
    void mousePressEvent(QMouseEvent*) override { if (min_) { min_ = false; build(); } }
  private:
    void build() {
      QLayoutItem* it; while ((it = v_->takeAt(0))) { if (it->widget()) it->widget()->deleteLater(); delete it; }
      QJsonArray L = o_->vp_->scene()->fileRaw.value("credits").toArray();
      if (min_) {
        QStringList n; for (const QJsonValue& c : L) n << c.toObject().value("name").toString();
        auto* t = new QLabel("© " + n.join(", ")); t->setStyleSheet("color:#bbb;font-size:11px"); v_->setContentsMargins(8, 3, 8, 3); v_->addWidget(t); setCursor(Qt::PointingHandCursor); return;
      }
      v_->setContentsMargins(10, 7, 26, 7); unsetCursor();
      for (int i = 0; i < L.size(); i++) {
        QJsonObject c = L[i].toObject(); if (c.value("name").toString().isEmpty()) continue;
        if (i) v_->addSpacing(6);
        if (!c.value("role").toString().isEmpty()) { auto* r = new QLabel(c.value("role").toString().toUpper()); r->setStyleSheet("color:#8a8a8a;font-size:10px"); v_->addWidget(r); }
        auto* n = new QLabel(c.value("name").toString()); n->setStyleSheet("color:#fff;font-weight:600;font-size:12px"); v_->addWidget(n);
        QString url = c.value("url").toString();
        if (url.startsWith("https://")) { auto* a = new QLabel(QString("<a href=\"%1\" style=\"color:#8fb4ff;text-decoration:none\">%2 ↗</a>").arg(url.toHtmlEscaped(), (c.value("link").toString(QObject::tr("Open the post"))).toHtmlEscaped()));
          a->setOpenExternalLinks(true); a->setToolTip(url); a->setStyleSheet("font-size:11px"); v_->addWidget(a); }
      }
    }
    ViewOverlay* o_; QVBoxLayout* v_; QPushButton* x_; QString sig_; bool min_ = false;
  };
  // an info card: drawn by the Inspector's settings (overlays::cfg), dragged anywhere inside the viewport (its place kept
  // as a fraction of the free room — the viewport resized, it keeps its side); the default corner until dragged
  class Card : public QWidget {
  public:
    Card(ViewOverlay* o, QPointF overlays::Cfg::*pos) : QWidget(o->host_->window(), Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint), o_(o), pos_(pos) {
      setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setCursor(Qt::OpenHandCursor);
      setToolTip(QObject::tr("Drag — move it (inside the viewport). Its look: the Inspector (the button by the FAQ)"));
    }
    // where it goes: the kept place, else the corner
    void placeIn(const QRect& host, QPoint corner) {
      const QPointF f = overlays::cfg().*pos_; QPoint p = corner;
      if (f.x() >= 0) p = QPoint(host.left() + int(std::lround(f.x() * std::max(0, host.width() - width()))), host.top() + int(std::lround(f.y() * std::max(0, host.height() - height()))));
      p.setX(std::clamp(p.x(), host.left(), std::max(host.left(), host.right() - width()))); p.setY(std::clamp(p.y(), host.top(), std::max(host.top(), host.bottom() - height())));
      if (!drag_ && pos() != p) move(p);
      host_ = host;
    }
  protected:
    void mousePressEvent(QMouseEvent* e) override { if (e->button() != Qt::LeftButton) return; drag_ = true; grab_ = e->globalPosition().toPoint() - pos(); setCursor(Qt::ClosedHandCursor); }
    void mouseMoveEvent(QMouseEvent* e) override {
      if (!drag_) return; QPoint p = e->globalPosition().toPoint() - grab_;
      p.setX(std::clamp(p.x(), host_.left(), std::max(host_.left(), host_.right() - width()))); p.setY(std::clamp(p.y(), host_.top(), std::max(host_.top(), host_.bottom() - height())));
      move(p);
    }
    void mouseReleaseEvent(QMouseEvent*) override {
      if (!drag_) return; drag_ = false; setCursor(Qt::OpenHandCursor);
      const double fx = host_.width() > width() ? double(x() - host_.left()) / (host_.width() - width()) : 0, fy = host_.height() > height() ? double(y() - host_.top()) / (host_.height() - height()) : 0;
      overlays::cfg().*pos_ = QPointF(std::clamp(fx, 0.0, 1.0), std::clamp(fy, 0.0, 1.0)); overlays::save();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { overlays::cfg().*pos_ = QPointF(-1, -1); overlays::save(); }   // (a double click — back to its corner)
    ViewOverlay* o_; QPointF overlays::Cfg::*pos_; QRect host_; QPoint grab_; bool drag_ = false;
  };
  // the frame counter (top right by default): frames a second while the view is drawn (the camera moving, a drag) and the
  // time between them; resting — a dash
  class Fps : public Card {
  public:
    explicit Fps(ViewOverlay* o) : Card(o, &overlays::Cfg::fpsPos) {}
    void sync(const QRect& host) {
      const auto& C = overlays::cfg(); if (!C.fps) { hide(); return; }
      const qint64 now = QDateTime::currentMSecsSinceEpoch();
      if (now - shown_ >= 250 || rev_ != overlays::revision()) { shown_ = now;
        const double f = o_->vp_->fps();                        // (the time between frames — what the eye gets; resting: none)
        const double cost = o_->vp_->lastFrameMs;   // (resting: what the last frame cost to draw — drawn only when something changes, so no rate to show)
        QString t = f > 0 ? QString("%1 FPS · %2 ms").arg(int(std::lround(f))).arg(1000.0 / f, 0, 'f', 1) : cost > 0 ? QString("rest · %1 ms").arg(cost, 0, 'f', 1) : QString("— FPS");
        QColor c = f > 0 ? (f >= 50 ? QColor(0x7f, 0xe0, 0x7f) : f >= 25 ? QColor(0xe8, 0xc0, 0x50) : QColor(0xe8, 0x60, 0x50))
                         : cost <= 0 ? QColor(0x9a, 0x9a, 0x9a) : cost < 20 ? QColor(0x9a, 0xc8, 0x9a) : cost < 40 ? QColor(0xe8, 0xc0, 0x50) : QColor(0xe8, 0x60, 0x50);
        if (t != text_ || c != col_ || rev_ != overlays::revision()) { rev_ = overlays::revision(); text_ = t; col_ = c; QSize s = overlays::fpsSize(C.fpsS, "000 FPS · 00.0 ms"); if (s != size()) resize(s); update(); } }
      placeIn(host, QPoint(host.right() - width() - 8, host.top() + 8));
      if (!isVisible()) show();
    }
  protected:
    void paintEvent(QPaintEvent*) override { QPainter p(this); overlays::paintFps(p, rect(), overlays::cfg().fpsS, text_, col_); }
  private:
    QString text_; QColor col_; qint64 shown_ = 0; int rev_ = 0;
  };
  // the model's numbers (bottom left by default): the selected meshes (none: the scene) — vertices, edges, polygons,
  // quads, tris, n-gons, the triangles they make; in a column or a row
  class Stats : public Card {
  public:
    explicit Stats(ViewOverlay* o) : Card(o, &overlays::Cfg::statsPos) {}
    void sync(const QRect& host) {
      const auto& C = overlays::cfg(); if (!C.stats) { hide(); return; }
      const qint64 now = QDateTime::currentMSecsSinceEpoch();
      if (now - shown_ >= 250 || rev_ != overlays::revision()) { shown_ = now;
        QLocale L; auto n = [&](long long v) { return L.toString(v); };
        QStringList rows; if (o_->vp_->room() == "hair") { auto S = o_->vp_->convertSummary(); auto a = [&](long long v) { return QString("≈ ") + L.toString(v); };   // (the Hair room: in its place, what Convert would make of the selected cocoons)
          rows = S.locks ? QStringList{QObject::tr("Convert · %1 lock%2").arg(S.locks).arg(S.locks == 1 ? "" : "s"), QObject::tr("Polygons\t%1").arg(a(S.polys)), QObject::tr("Triangles\t%1").arg(a(S.tris)), QObject::tr("Vertices\t%1").arg(a(S.verts))}
                         : QStringList{QObject::tr("Convert · select cocoons")}; }
        else { auto S = o_->vp_->modelStats();
        rows = QStringList{S.all ? QObject::tr("Scene · %1 mesh%2").arg(S.meshes).arg(S.meshes == 1 ? "" : "es") : QObject::tr("Selected · %1 mesh%2").arg(S.meshes).arg(S.meshes == 1 ? "" : "es"),
                         QObject::tr("Vertices\t%1").arg(n(S.verts)), QObject::tr("Edges\t%1").arg(S.edges < 0 ? QString("…") : n(S.edges)), QObject::tr("Polygons\t%1").arg(n(S.faces)),
                         QObject::tr("Quads\t%1").arg(n(S.quads)), QObject::tr("Tris\t%1").arg(n(S.tris)), QObject::tr("N-gons\t%1").arg(n(S.ngons)), QObject::tr("Triangulated\t%1").arg(n(S.triangulated)), QObject::tr("Picked\t%1").arg(o_->vp_->pickedInfo())}; }
        if (rows != rows_ || rev_ != overlays::revision()) { rev_ = overlays::revision(); rows_ = rows; QSize s = overlays::statsSize(C.statsS, rows_, C.stats == 1); if (s != size()) resize(s); update(); }
      }
      placeIn(host, QPoint(host.left() + 10, host.bottom() - 10 - height()));
      if (!isVisible()) show();
    }
  protected:
    void paintEvent(QPaintEvent*) override { QPainter p(this); overlays::paintStats(p, rect(), overlays::cfg().statsS, rows_, overlays::cfg().stats == 1); }
  private:
    QStringList rows_; qint64 shown_ = 0; int rev_ = 0;
  };
  QWidget* host_; Viewport* vp_; Labels* labels_; Axes* axes_; Credits* credits_; Fps* fps_; Stats* stats_; QString sig_, qsig_;
};

ViewOverlay* makeViewOverlay(QWidget* container, Viewport* vp) { return new ViewOverlay(container, vp); }

}  // namespace hm
