#include "app/ViewWindow.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCursor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace hm {

namespace {
const char* VIEWS[] = {"persp", "ortho", "front", "right", "left", "back", "top"};
QString viewName(const QString& v) {
  return v == "persp" ? QObject::tr("Perspective") : v == "ortho" ? QObject::tr("Orthographic") : v == "front" ? QObject::tr("Front") : v == "right" ? QObject::tr("Left")
       : v == "left" ? QObject::tr("Right") : v == "back" ? QObject::tr("Back") : QObject::tr("Top");
}

// the picture of the views, the mouse handed to the viewport (its positions moved to where the views lie for it)
class Surface : public QWidget {
public:
  Surface(QWidget* parent, Viewport* vp) : QWidget(parent), vp_(vp) { setMouseTracking(true); setFocusPolicy(Qt::StrongFocus); setAttribute(Qt::WA_OpaquePaintEvent); setMinimumSize(160, 120); }
  void setImage(const QImage& im) { img_ = im; update(); }
  bool stacked = false;
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.fillRect(rect(), QColor(0x2a, 0x2a, 0x2a));
    if (!img_.isNull()) { if (img_.size() != size() * devicePixelRatioF()) p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(rect(), img_); }
    const auto panes = vp_->detachedPanes(); const QPoint o = vp_->detachedOrigin();
    QFont f = font(); f.setPointSizeF(8.5); p.setFont(f); p.setRenderHint(QPainter::Antialiasing);
    for (int i : panes) {
      const QRect r = vp_->paneRect(i).translated(-o);
      if (panes.size() > 1) { const bool act = i == vp_->activePane(); p.setBrush(Qt::NoBrush); p.setPen(QPen(act ? QColor(0x4a, 0x7f, 0xd6) : QColor(0x44, 0x44, 0x44), act ? 2 : 1)); p.drawRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5)); }
      const QString t = vp_->paneLabel(i); QRect tr = p.fontMetrics().boundingRect(t).adjusted(-6, -3, 6, 3); tr.moveTopLeft(QPoint(r.x() + 10, r.y() + 8));
      p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 110)); p.drawRoundedRect(tr, 3, 3); p.setPen(QColor(0xb0, 0xb0, 0xb0)); p.drawText(tr, Qt::AlignCenter, t);
    }
  }
  void mouse(QMouseEvent* e) { QMouseEvent m(e->type(), e->position() + QPointF(vp_->detachedOrigin()), e->globalPosition(), e->button(), e->buttons(), e->modifiers()); vp_->viewEvent(&m); setCursor(vp_->cursor()); update(); }
  void mousePressEvent(QMouseEvent* e) override { setFocus(); mouse(e); }
  void mouseMoveEvent(QMouseEvent* e) override { mouse(e); }
  void mouseReleaseEvent(QMouseEvent* e) override { mouse(e); }
  void mouseDoubleClickEvent(QMouseEvent* e) override { mouse(e); }
  void wheelEvent(QWheelEvent* e) override { QWheelEvent w(e->position() + QPointF(vp_->detachedOrigin()), e->globalPosition(), e->pixelDelta(), e->angleDelta(), e->buttons(), e->modifiers(), e->phase(), e->inverted()); vp_->viewEvent(&w); }
  void enterEvent(QEnterEvent*) override { vp_->viewHover(true); }
  void leaveEvent(QEvent*) override { vp_->viewHover(false); }
  void resizeEvent(QResizeEvent*) override { vp_->setDetachedLook(size(), stacked); }
private:
  Viewport* vp_; QImage img_;
};

class ViewWin : public QWidget {
public:
  ViewWin(QWidget* mainWindow, Viewport* vp) : QWidget(mainWindow, Qt::Window), vp_(vp) {
    setObjectName("viewWindow"); setWindowTitle(QObject::tr("Views")); setAttribute(Qt::WA_DeleteOnClose);
    auto* l = new QVBoxLayout(this); l->setContentsMargins(0, 0, 0, 0); surf_ = new Surface(this, vp); l->addWidget(surf_);
    QSettings st; surf_->stacked = st.value("viewWindow/stacked", false).toBool();
    const QByteArray g = st.value("viewWindow/geom").toByteArray(); if (g.isEmpty() || !restoreGeometry(g)) resize(720, 560);
    vp_->detachedFrame = [this](const QImage& im) { surf_->setImage(im); };
  }
  ~ViewWin() override { if (vp_) vp_->detachedFrame = nullptr; }
  Surface* surface() const { return surf_; }
  void setStacked(bool on) { surf_->stacked = on; QSettings().setValue("viewWindow/stacked", on); vp_->setDetachedLook(surf_->size(), on); }
  bool migrating = false;
protected:
  void closeEvent(QCloseEvent* e) override { QSettings().setValue("viewWindow/geom", saveGeometry()); if (!migrating && vp_) vp_->setDetached(0); e->accept(); }
private:
  QPointer<Viewport> vp_; Surface* surf_;
};
QPointer<ViewWin> g_win;

// Tab over the view window: its views
class WinMenu : public QFrame {
public:
  WinMenu(ViewWin* win, Viewport* vp) : QFrame(win, Qt::Popup), win_(win), vp_(vp) {
    setAttribute(Qt::WA_DeleteOnClose); setObjectName("viewWinMenu");
    setStyleSheet("#viewWinMenu{background:#262626;border:1px solid #555;border-radius:6px} QLabel[h5=\"true\"]{color:#9a9a9a;font-size:11px;margin-top:6px}");
    v_ = new QVBoxLayout(this); v_->setContentsMargins(10, 8, 10, 10); v_->setSpacing(4); build();
  }
private:
  static void clearLayout(QLayout* l) { QLayoutItem* it; while ((it = l->takeAt(0))) { if (it->widget()) it->widget()->deleteLater(); else if (it->layout()) clearLayout(it->layout()); delete it; } }
  void rebuild() { QTimer::singleShot(0, this, [this] { clearLayout(v_); build(); adjustSize(); }); }
  QLabel* h5(const QString& t) { auto* l = new QLabel(t); l->setProperty("h5", true); return l; }
  QPushButton* b(const QString& lab, const QString& tip, std::function<void()> fn, bool checkable = false, bool on = false) {
    auto* x = new QPushButton(lab); x->setToolTip(tip); x->setCheckable(checkable); x->setChecked(on); x->setFocusPolicy(Qt::NoFocus);
    QObject::connect(x, &QPushButton::clicked, this, [this, fn] { fn(); rebuild(); }); return x;
  }
  void build() {
    if (!win_) return;
    const int n = vp_->detached(); const bool st = vp_->detachedStacked();
    auto* t = h5(QObject::tr("The view window")); t->setStyleSheet("margin-top:0"); v_->addWidget(t);
    auto* r0 = new QHBoxLayout; v_->addLayout(r0);
    r0->addWidget(b(QObject::tr("▯ One view"), QObject::tr("One view in this window"), [this] { vp_->setDetached(1, win_->surface()); }, true, n == 1));
    r0->addWidget(b(QObject::tr("▯▯ Two side by side"), QObject::tr("Two views, side by side"), [this] { vp_->setDetached(2, win_->surface()); win_->setStacked(false); }, true, n == 2 && !st));
    r0->addWidget(b(QObject::tr("▭▭ Two, one over the other"), QObject::tr("Two views, one over the other"), [this] { vp_->setDetached(2, win_->surface()); win_->setStacked(true); }, true, n == 2 && st));
    const auto panes = vp_->detachedPanes(); const bool paint = vp_->room() == "paint"; const QStringList sets = vp_->visSetNames();
    for (size_t k = 0; k < panes.size(); k++) { const int pi = panes[k]; const QString view = vp_->paneCamera(pi).view;
      v_->addWidget(h5(panes.size() > 1 ? QObject::tr("View %1").arg(k + 1) : QObject::tr("The view")));
      auto* r1 = new QHBoxLayout; r1->setSpacing(3); v_->addLayout(r1);
      for (const char* vv : VIEWS) { const QString w = vv; r1->addWidget(b(viewName(w), QObject::tr("This view's projection"), [this, pi, w] { vp_->setPaneView(pi, w); }, true, view == w)); }
      r1->addWidget(b(QObject::tr("↺"), QObject::tr("Reset this view's camera: upright, the whole model in it"), [this, pi] { vp_->paneCam(pi).reset(); vp_->liveView(); }));
      if (paint) {                                              // (the Paint room: what the view shows, its camera with the main view's)
        const Viewport::PaneVis pv = vp_->paneVis(pi); const bool has = bool(vp_->paneHid(pi));
        auto* r2 = new QHBoxLayout; r2->setSpacing(3); v_->addLayout(r2); r2->addWidget(new QLabel(QObject::tr("Shows")));
        r2->addWidget(b(QObject::tr("As the scene"), QObject::tr("What the main view shows — the outliner's eyes"), [this, pi] { auto v = vp_->paneVis(pi); v.set.clear(); vp_->setPaneVis(pi, v); }, true, !has));
        for (const QString& s : sets) r2->addWidget(b(s, QObject::tr("This view shows the visibility set %1").arg(s), [this, pi, s] { auto v = vp_->paneVis(pi); v.set = s; vp_->setPaneVis(pi, v); }, true, has && pv.set == s));
        r2->addStretch(1);
        auto* cb = new QCheckBox(QObject::tr("Camera follows the main view")); cb->setChecked(pv.sync); cb->setFocusPolicy(Qt::NoFocus);
        cb->setToolTip(QObject::tr("This view's camera and the main view's move as one — turn either, both turn"));
        QObject::connect(cb, &QCheckBox::toggled, this, [this, pi](bool on) { auto v = vp_->paneVis(pi); v.sync = on; vp_->setPaneVis(pi, v); });
        v_->addWidget(cb);
      }
    }
    if (paint && sets.isEmpty()) { auto* note = new QLabel(QObject::tr("No visibility sets yet: hide what is in the way in the Paint room (its lists' eyes, H over the model), then ＋ Set under the Visibility sets list.")); note->setWordWrap(true); note->setStyleSheet("color:#8a8a8a;font-size:11px"); note->setMaximumWidth(460); v_->addWidget(note); }
    v_->addWidget(h5(QObject::tr("The window")));
    auto* r3 = new QHBoxLayout; v_->addLayout(r3);
    auto* mg = new QPushButton(QObject::tr("⇤ Move the views into the main viewport")); mg->setFocusPolicy(Qt::NoFocus); mg->setToolTip(QObject::tr("The views of this window become windows of the main viewport's split screen (their cameras and sets kept); this window closes"));
    QObject::connect(mg, &QPushButton::clicked, this, [this] { QPointer<ViewWin> w = win_; close(); if (w) { w->migrating = true; vp_->migrateDetached(); w->close(); } });
    auto* cl = new QPushButton(QObject::tr("✕ Close the window")); cl->setFocusPolicy(Qt::NoFocus); cl->setToolTip(QObject::tr("The window and its views closed"));
    QObject::connect(cl, &QPushButton::clicked, this, [this] { QPointer<ViewWin> w = win_; close(); if (w) w->close(); });
    r3->addWidget(mg); r3->addWidget(cl);
  }
  QPointer<ViewWin> win_; Viewport* vp_; QVBoxLayout* v_;
};
QPointer<WinMenu> g_menu; Viewport* g_vp = nullptr;
}  // namespace

void openViewWindow(QWidget* mainWindow, Viewport* vp) {
  if (g_win) {
    if (vp->detached() >= 2) { ui::toast(QObject::tr("The view window shows two views already — four views in all, the main viewport's with them")); g_win->raise(); return; }
    vp->setDetached(2, g_win->surface()); g_win->raise(); return;
  }
  g_vp = vp; g_win = new ViewWin(mainWindow, vp); vp->setDetached(1, g_win->surface()); g_win->show();
  vp->setDetachedLook(g_win->surface()->size(), g_win->surface()->stacked);
}
void closeViewWindow() { if (g_win) g_win->close(); }
bool viewWindowOpen() { return g_win && g_win->isVisible(); }
bool viewWindowUnderPointer() { return g_win && g_win->isVisible() && g_win->frameGeometry().contains(QCursor::pos()) && (QApplication::activeWindow() == g_win || QApplication::topLevelAt(QCursor::pos()) == g_win); }
void toggleViewWindowMenu(const QPoint& global) {
  if (g_menu) { g_menu->close(); return; }
  if (!g_win) return;
  g_menu = new WinMenu(g_win, g_vp); g_menu->adjustSize(); g_menu->move(global); g_menu->show();
}
bool viewWindowMouse(int type, const QPointF& at, int button, int held, int mods) {
  if (!g_win) return false; QWidget* s = g_win->surface();
  QMouseEvent ev(QEvent::Type(type), at, s->mapToGlobal(at.toPoint()), Qt::MouseButton(button), Qt::MouseButtons(held), Qt::KeyboardModifiers(mods));
  QCoreApplication::sendEvent(s, &ev); return true;
}
bool viewWindowShot(const QString& png, bool menu) {
  QApplication::processEvents();
  if (menu) return g_menu && g_menu->grab().save(png);
  return g_win && g_win->grab().save(png);
}

}  // namespace hm
