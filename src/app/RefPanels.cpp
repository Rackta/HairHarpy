#include "app/RefPanels.h"
#include "app/RefBoard.h"
#include "paint/PaintDoc.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace hm {

bool refImageFromFile(const QString& file, QString* dataUrl, int* w, int* h) {
  QImageReader r(file); r.setAutoTransform(true); QImage img = r.read();
  if (img.isNull()) return false;
  double k = std::min(1.0, 2048.0 / std::max(img.width(), img.height()));
  if (k < 1) img = img.scaled(std::max(1, int(std::lround(img.width() * k))), std::max(1, int(std::lround(img.height() * k))), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  QString ext = QFileInfo(file).suffix().toLower(); bool png = img.hasAlphaChannel() || ext == "png" || ext == "gif" || ext == "webp" || ext == "svg";
  QByteArray b; QBuffer buf(&b); buf.open(QIODevice::WriteOnly); img.save(&buf, png ? "PNG" : "JPEG", png ? -1 : 92);
  *dataUrl = QString("data:image/%1;base64,").arg(png ? "png" : "jpeg") + QString::fromLatin1(b.toBase64());
  *w = img.width(); *h = img.height(); return true;
}

void refPlaneSetCanvas(RefPlane& p, int canvas, Scene& sc) {
  auto f = sc.refSketches.find(canvas); if (f == sc.refSketches.end()) return;
  paint::Document d; d.unpackJson(f->second.data); const double a = d.height() > 0 ? double(d.width()) / d.height() : 1;
  if (p.follows() && p.hasLoc) { Vec3 t, s; Quat q; m4Decompose(p.loc, t, q, s); s.x = std::abs(s.y) * a * (s.x < 0 ? -1 : 1); p.loc = m4Compose(t, q, s); }
  else p.scl.x = std::abs(p.scl.y) * a * (p.scl.x < 0 ? -1 : 1);
  p.canvas = canvas;
}

namespace {

std::function<void(int, bool)> g_openCanvas;   // (✏ Canvas: the canvas mode for a plane)
std::function<void(const QString&)> g_newSheet;   // (＋: a new screen-sized canvas for a view)
std::function<void()> g_changed;                   // (the project changed outside the history: the title's mark)

class RefMenu : public QFrame {
public:
  RefMenu(QWidget* parent, Viewport* vp, Scene* sc, RefBoard* board) : QFrame(parent, Qt::Popup), vp_(vp), sc_(sc), board_(board) {
    setAttribute(Qt::WA_DeleteOnClose); setObjectName("refMenu");
    setStyleSheet("#refMenu{background:#262626;border:1px solid #555;border-radius:6px} QLabel[h5=\"true\"]{color:#9a9a9a;font-size:11px;margin-top:6px}"
                  " QFrame[rp=\"true\"]{border:1px solid transparent;border-radius:4px} QFrame[rp=\"true\"]:hover{background:#2e2e2e} QFrame[rp=\"true\"][act=\"true\"]{border-color:#4a7fd6;background:#2c3a52}");
    v_ = new QVBoxLayout(this); v_->setContentsMargins(10, 8, 10, 10); v_->setSpacing(4); setFixedWidth(320);
    build();
  }
  void rebuild() { QTimer::singleShot(0, this, [this] { QWidget* c = content_; content_ = nullptr; if (c) c->deleteLater(); build(); adjustSize(); }); }
private:
  QLabel* h5(const QString& t) { auto* l = new QLabel(t); l->setProperty("h5", true); return l; }
  QPushButton* b(const QString& lab, const QString& tip, std::function<void()> fn, bool on = false) {
    auto* x = new QPushButton(lab); x->setToolTip(tip); x->setFocusPolicy(Qt::NoFocus); if (on) { x->setCheckable(true); x->setChecked(true); }
    QObject::connect(x, &QPushButton::clicked, this, [this, fn] { fn(); rebuild(); }); return x;
  }
  RefPlane* active() { return vp_->refPlane(vp_->activeRefPlane()); }
  void edited(bool commitNow = true) { vp_->refPlanesEdited(commitNow); }
  // the menu: a canvas for a view (＋ and its views), the project's canvases — each with ✏ Draw right on its row — and the chosen one's
  // few settings. The canvas is not moved, turned or scaled here any more: its layers are (in the canvas's own tools)
  QPushButton* ib(const QString& icon, const QString& text, const QString& tip, std::function<void()> fn, int side = 28) {
    auto* x = new QPushButton(text); x->setIcon(ui::icon(icon)); x->setIconSize(QSize(side - 6, side - 6)); x->setMinimumHeight(side); x->setToolTip(tip); x->setFocusPolicy(Qt::NoFocus);
    QObject::connect(x, &QPushButton::clicked, this, [this, fn] { fn(); rebuild(); }); return x;
  }
  void build() {
    content_ = new QWidget; auto* v = new QVBoxLayout(content_); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(4); v_->addWidget(content_);
    // ---- a new canvas ----
    auto* t = h5(QObject::tr("New canvas for the view")); t->setStyleSheet("margin-top:0"); v->addWidget(t);
    { auto* r = new QHBoxLayout; r->setSpacing(3); v->addLayout(r);
      for (const char* view : {"front", "right", "left", "back", "top"}) {
        QString vw = view; auto* x = b(refs::viewLabel(vw), QObject::tr("A new canvas the size of your screen for the %1 view — it opens to draw on").arg(refs::viewLabel(vw).toLower()), [this, vw] { close(); if (g_newSheet) g_newSheet(vw); });
        x->setStyleSheet("padding:3px 6px"); r->addWidget(x); }
      r->addStretch(1); }
    // ---- the canvases ----
    v->addWidget(h5(QObject::tr("Canvases")));
    if (sc_->refPlanes.empty()) { auto* m = new QLabel(QObject::tr("No canvases yet — pick a view above: a canvas the size of your screen opens on it to draw.")); m->setProperty("mut", true); m->setWordWrap(true); v->addWidget(m); }
    if (sel_ < 0 || !vp_->refPlane(sel_)) sel_ = sc_->refPlanes.empty() ? -1 : sc_->refPlanes.front().id;
    for (const RefPlane& rp : sc_->refPlanes) {
      const int id = rp.id;
      class Row : public QFrame { public: std::function<void()> click, dbl; protected:
        void mouseReleaseEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) && click) click(); }
        void mouseDoubleClickEvent(QMouseEvent*) override { if (dbl) dbl(); } };
      auto* r = new Row; r->setProperty("rp", true); r->setProperty("act", sel_ == id); r->setCursor(Qt::PointingHandCursor);
      r->setToolTip(QObject::tr("Click — its settings below · double click or ✏ — draw on it"));
      auto* h = new QHBoxLayout(r); h->setContentsMargins(4, 3, 4, 3); h->setSpacing(6);
      auto* th = new QLabel; th->setFixedSize(52, 30); th->setAlignment(Qt::AlignCenter); th->setStyleSheet("background:#1a1a1a;border:1px solid #3a3a3a");
      { auto f = sc_->refSketches.find(rp.canvas); QImage im = f != sc_->refSketches.end() ? f->second.flat : QImage();
        if (!im.isNull()) th->setPixmap(QPixmap::fromImage(im.scaled(50, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation))); }
      h->addWidget(th);
      auto* nm = new QLabel(QString("<b>%1</b><br><span style='color:#8a8a8a'>%2%3</span>").arg(rp.name.toHtmlEscaped(), refs::viewLabel(rp.view),
        rp.canvas >= 0 && refs::canvasUsers(*sc_, rp.canvas) > 1 ? QObject::tr(" · shared") : QString())); h->addWidget(nm, 1);
      auto* draw = new QPushButton; draw->setIcon(ui::icon("rc_brush")); draw->setIconSize(QSize(22, 22)); draw->setFixedSize(30, 28); draw->setFocusPolicy(Qt::NoFocus); draw->setProperty("primary", true);
      draw->setToolTip(QObject::tr("✏ Draw on it: the canvas opens over its view"));
      QObject::connect(draw, &QPushButton::clicked, this, [this, id] { close(); if (g_openCanvas) g_openCanvas(id, false); });
      auto* eye = new QPushButton; eye->setIcon(ui::eyeIcon(rp.visible)); eye->setFlat(true); eye->setFixedWidth(26); eye->setToolTip(QObject::tr("Show / hide"));
      QObject::connect(eye, &QPushButton::clicked, this, [this, id] { RefPlane* p = vp_->refPlane(id); if (!p) return; p->visible = !p->visible; edited(); rebuild(); });
      auto* del = new QPushButton; del->setIcon(ui::icon("rc_delete")); del->setIconSize(QSize(18, 18)); del->setFlat(true); del->setFixedWidth(26); del->setToolTip(QObject::tr("Delete the canvas from this view"));
      QObject::connect(del, &QPushButton::clicked, this, [this, id] { auto& L = sc_->refPlanes; L.erase(std::remove_if(L.begin(), L.end(), [id](const RefPlane& x) { return x.id == id; }), L.end()); if (vp_->activeRefPlane() == id) vp_->setActiveRefPlane(-1); edited(); rebuild(); });
      h->addWidget(draw); h->addWidget(eye); h->addWidget(del);
      r->click = [this, id] { sel_ = id; rebuild(); };
      r->dbl = [this, id] { close(); if (g_openCanvas) g_openCanvas(id, false); };
      v->addWidget(r);
    }
    // ---- the chosen one ----
    RefPlane* rp = vp_->refPlane(sel_);
    if (rp) {
      const int id = rp->id; auto P = [this, id]() { return vp_->refPlane(id); };
      v->addWidget(h5(rp->name));
      { auto* r = new QHBoxLayout; r->setSpacing(4); v->addLayout(r);
        auto* d = ib("rc_brush", QObject::tr("Draw"), QObject::tr("Open the canvas over its view: brushes, layers, selection — the model stays seen and locked; ✔ Done (Esc) comes back here"), [this, id] { close(); if (g_openCanvas) g_openCanvas(id, false); }, 32);
        d->setProperty("primary", true); r->addWidget(d, 1);
        r->addWidget(ib("rc_library", QObject::tr("Insert image…"), QObject::tr("Open the canvas and put pictures onto new layers (moved, scaled, turned there like any layer)"), [this, id] { close(); if (g_openCanvas) g_openCanvas(id, true); }, 32), 1); }
      { auto* fr = new QHBoxLayout; v->addLayout(fr); fr->addWidget(new QLabel(QObject::tr("Opacity")));
        auto* op = new QSlider(Qt::Horizontal); op->setRange(2, 100); op->setValue(int(std::lround(rp->opacity * 100))); fr->addWidget(op, 1);
        auto* ov = new QLabel(QString("%1%").arg(op->value())); ov->setFixedWidth(34); fr->addWidget(ov);
        QObject::connect(op, &QSlider::valueChanged, this, [this, P, ov](int x) { if (RefPlane* p = P()) { p->opacity = x / 100.0; ov->setText(QString("%1%").arg(x)); vp_->refPlanesEdited(false); } });
        QObject::connect(op, &QSlider::sliderReleased, this, [this] { edited(); }); }
      { auto* r = new QHBoxLayout; r->setSpacing(4); v->addLayout(r);
        auto* f = ib("rc_front", QObject::tr("In front"), QObject::tr("The canvas over the model"), [this, P] { if (RefPlane* p = P()) { p->over = true; edited(); } }); f->setCheckable(true); f->setChecked(rp->over);
        auto* bh = ib("rc_behind", QObject::tr("Behind"), QObject::tr("The canvas behind the model: the model covers it"), [this, P] { if (RefPlane* p = P()) { p->over = false; edited(); } }); bh->setCheckable(true); bh->setChecked(!rp->over);
        r->addWidget(f, 1); r->addWidget(bh, 1); }
      { auto* c = new QCheckBox(QObject::tr("Only in the %1 view").arg(refs::viewLabel(rp->view))); c->setToolTip(QObject::tr("Shown only in its own view (a front canvas — only in the Front view)")); c->setChecked(rp->onlyView); v->addWidget(c);
        QObject::connect(c, &QCheckBox::toggled, this, [this, P](bool x) { if (RefPlane* p = P()) { p->onlyView = x; edited(); } }); }
      if (rp->canvas >= 0) {
        auto* cr = new QHBoxLayout; cr->setSpacing(4); v->addLayout(cr); auto* cn = new QLabel; cn->setPixmap(ui::image("rc_library").scaled(22, 22, Qt::KeepAspectRatio, Qt::SmoothTransformation)); cn->setToolTip(QObject::tr("The canvas it shows")); cr->addWidget(cn);
        auto* combo = new QComboBox; combo->setFocusPolicy(Qt::NoFocus); combo->setToolTip(QObject::tr("The canvas this view shows: pick another of the project's"));
        for (auto& [cid, sk] : sc_->refSketches) { const int u = refs::canvasUsers(*sc_, cid); combo->addItem(QString("%1 (%2×%3)%4").arg(sk.name).arg(sk.flat.width()).arg(sk.flat.height()).arg(u > 1 ? QObject::tr(" · %1 views").arg(u) : QString()), cid); }
        combo->setCurrentIndex(std::max(0, combo->findData(rp->canvas))); cr->addWidget(combo, 1);
        QObject::connect(combo, &QComboBox::activated, this, [this, P, combo](int) { if (RefPlane* p = P()) { refPlaneSetCanvas(*p, combo->currentData().toInt(), *sc_); edited(); rebuild(); } });   // (its proportions follow, as in the canvas mode)
        if (refs::canvasUsers(*sc_, rp->canvas) > 1) { auto* sep = ib("rc_canvas_copy", QString(), QObject::tr("Make separate: this view gets its own copy of the canvas with all its layers"), [this, P] { if (RefPlane* p = P()) { p->canvas = refs::copyCanvas(*sc_, p->canvas); edited(); } }); sep->setFixedWidth(34); cr->addWidget(sep); }
      }
    }
    // ---- the anchors and the board ----
    if (!sc_->refMarkers.empty()) {                          // (📍 the anchors: put on a canvas, seen on every view)
      auto* mr = new QHBoxLayout; v->addLayout(mr);
      auto* mi = new QLabel; mi->setPixmap(ui::image("rc_marker").scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation)); mr->addWidget(mi);
      auto* ml = new QLabel(QObject::tr("Anchors: %1").arg(sc_->refMarkers.size())); ml->setProperty("mut", true); mr->addWidget(ml, 1);
      auto* ms = new QCheckBox(QObject::tr("Show")); ms->setChecked(sc_->refMarkersShown); mr->addWidget(ms);
      // (the anchors are the project's, outside the history: a change marks it unsaved, as in the canvas)
      QObject::connect(ms, &QCheckBox::toggled, this, [this](bool on) { sc_->refMarkersShown = on; sc_->refMarkRev++; vp_->requestUpdate(); if (g_changed) g_changed(); });
      mr->addWidget(b(QObject::tr("✖ All"), QObject::tr("Delete all the anchors"), [this] { sc_->refMarkers.clear(); sc_->refMarkRev++; vp_->requestUpdate(); if (g_changed) g_changed(); }));
    }
    { auto* r1 = new QHBoxLayout; v->addLayout(r1);
      r1->addWidget(b(board_->isOpen() ? QObject::tr("🗔 Hide the reference board") : QObject::tr("🗔 Reference board"), QObject::tr("A free window over the viewport with your reference images (like PureRef)"), [this] { board_->showBoard(!board_->isOpen()); }, board_->isOpen()));
      if (board_->isLocked()) r1->addWidget(b(QObject::tr("🔒 Unlock the board"), {}, [this] { board_->setLocked(false); }));
      r1->addStretch(1); }
  }
  Viewport* vp_; Scene* sc_; RefBoard* board_; QVBoxLayout* v_; QWidget* content_ = nullptr; bool choose_ = false; static inline bool shared_ = true; static inline int sel_ = -1;   // (the canvas whose settings are shown)
};
QPointer<RefMenu> g_menu;

}  // namespace

void openRefMenu(QWidget* parent, Viewport* vp, Scene* sc, RefBoard* board, const QPoint& global) {
  if (g_menu) g_menu->close();
  g_menu = new RefMenu(parent, vp, sc, board); g_menu->adjustSize(); g_menu->move(global); g_menu->show();
}
void setRefCanvasOpener(std::function<void(int, bool)> open, std::function<void(const QString&)> newSheet) { g_openCanvas = std::move(open); g_newSheet = std::move(newSheet); }
void closeRefMenu() { if (g_menu) g_menu->close(); }
bool refMenuOpen() { return g_menu && g_menu->isVisible(); }
bool refMenuShot(const QString& png) { if (!g_menu) return false; QApplication::processEvents(); g_menu->adjustSize(); return g_menu->grab().save(png); }
void setRefMenuChanged(std::function<void()> changed) { g_changed = std::move(changed); }

}  // namespace hm