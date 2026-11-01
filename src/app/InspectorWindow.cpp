#include "app/InspectorWindow.h"
#include "app/Extras.h"
#include "app/HistoryBar.h"
#include "app/KeyCastWindow.h"
#include "app/Overlays.h"
#include "app/ToolPanels.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace hm {

namespace {
// the look of one info window: a popup at `at` (global) — the background's colour, its opacity; the font's size, weight,
// opacity
void openStyleAt(QWidget* parent, const QPoint& at, overlays::Style* s, std::function<void()> changed) {
  auto* f = new QFrame(parent->window(), Qt::Popup); f->setAttribute(Qt::WA_DeleteOnClose); f->setObjectName("insStyle");
  f->setStyleSheet("#insStyle{background:#1a2030;border:1px solid #3a4568;border-radius:8px} QLabel{color:#c8cbd8}");
  auto* v = new QVBoxLayout(f); v->setContentsMargins(10, 8, 10, 10); v->setSpacing(4);
  auto* cb = new QPushButton; cb->setFixedSize(46, 20); cb->setToolTip(QObject::tr("The background's colour"));
  auto paint = [cb, s] { cb->setStyleSheet(QString("background:%1;border:1px solid #555;border-radius:3px").arg(s->bg.name())); }; paint();
  QObject::connect(cb, &QPushButton::clicked, f, [s, paint, changed] { QColor c = QColorDialog::getColor(s->bg, nullptr, QObject::tr("Background")); if (c.isValid()) { s->bg = c; paint(); changed(); } });
  auto* row = new QWidget; auto* h = new QHBoxLayout(row); h->setContentsMargins(0, 0, 0, 0); h->addWidget(new QLabel(QObject::tr("Background"))); h->addStretch(1); h->addWidget(cb); v->addWidget(row);
  auto slider = [&](const QString& lab, double lo, double hi, double step, double val, std::function<QString(double)> fmt, std::function<void(double)> set) {
    auto* r = new SliderRow(lab, lo, hi, step, val, fmt); QObject::connect(r, &SliderRow::changed, f, [set, changed](double x) { set(x); changed(); }); v->addWidget(r); };
  auto pct = [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; };
  slider(QObject::tr("Background opacity"), 0, 1, 0.01, s->bgOp, pct, [s](double x) { s->bgOp = x; });
  slider(QObject::tr("Font size"), 6, 24, 0.5, s->size, [](double x) { return QString::number(x, 'g', 3); }, [s](double x) { s->size = x; });
  slider(QObject::tr("Font weight"), 100, 900, 100, s->weight, [](double x) { return QString::number(int(x)); }, [s](double x) { s->weight = int(std::lround(x / 100) * 100); });
  slider(QObject::tr("Font opacity"), 0.1, 1, 0.01, s->textOp, pct, [s](double x) { s->textOp = x; });
  if (s == &overlays::cfg().histS) {   // (the History slider: the step count too)
    auto* c = new QCheckBox(QObject::tr("Show the step count (Step 3 of 12)")); c->setStyleSheet("color:#c8cbd8"); c->setChecked(overlays::cfg().histSteps);
    QObject::connect(c, &QCheckBox::toggled, f, [changed](bool on) { overlays::cfg().histSteps = on; changed(); }); v->addWidget(c);
    slider(QObject::tr("Steps shown"), 5, 200, 1, overlays::cfg().histShown, [](double x) { return QString::number(int(x)); }, [](double x) { overlays::cfg().histShown = int(std::lround(x)); });
  }
  f->setMinimumWidth(270); f->adjustSize(); f->move(at); f->show();
}
void openStyle(QWidget* at, overlays::Style* s, std::function<void()> changed) { openStyleAt(at, at->mapToGlobal(QPoint(0, at->height() + 4)), s, changed); }
// the key display's look: a popup under its ⚙ — the background's opacity, the size (the window shown meanwhile: seen as it changes)
void openKeyCastLookAt(QWidget* parent, const QPoint& at, KeyCastWindow* kc) {
  auto* f = new QFrame(parent->window(), Qt::Popup); f->setAttribute(Qt::WA_DeleteOnClose); f->setObjectName("insStyle");
  f->setStyleSheet("#insStyle{background:#1a2030;border:1px solid #3a4568;border-radius:8px} QLabel{color:#c8cbd8}");
  auto* v = new QVBoxLayout(f); v->setContentsMargins(10, 8, 10, 10); v->setSpacing(4);
  auto* op = new SliderRow(QObject::tr("Background opacity"), 0, 1, 0.01, kc->bgOpacity(), [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; });
  auto* sz = new SliderRow(QObject::tr("Size"), 0.5, 2.5, 0.05, kc->scale(), [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; });
  QObject::connect(op, &SliderRow::changed, f, [kc](double x) { kc->setLook(kc->scale(), x); });
  QObject::connect(sz, &SliderRow::changed, f, [kc](double x) { kc->setLook(x, kc->bgOpacity()); });
  v->addWidget(op); v->addWidget(sz);
  if (!kc->isVisible()) { kc->open(); QObject::connect(f, &QObject::destroyed, kc, [kc] { if (!QSettings().value("keycast/on", false).toBool()) { kc->hide(); qApp->removeEventFilter(kc); } }); }
  f->setMinimumWidth(270); f->adjustSize(); f->move(at); f->show();
}
void openKeyCastLook(QWidget* at, KeyCastWindow* kc) { openKeyCastLookAt(at, at->mapToGlobal(QPoint(0, at->height() + 4)), kc); }

// the preview: the viewport in small — the info windows as outlines where they sit (in proportion to the real viewport);
// LMB drag — moved (the viewport's follow), RMB — the window's look, double click — back to its corner
class Preview : public QWidget {
public:
  QWidget* host = nullptr; bool hair = false; std::function<void()> changed; std::function<bool()> histShown;
  std::function<KeyCastWindow*()> keyCast;          // (the key display: in the preview while it is on)
  Preview() { setMinimumHeight(250); setMouseTracking(true); setToolTip(QObject::tr("The viewport in small: drag a window to move it there; RMB — its look; double click — back to its corner")); }
protected:
  struct Box { QRect r; QPointF overlays::Cfg::*pos; overlays::Style overlays::Cfg::*style; std::function<void(QPoint)> look = {}; QString name = {}; };   // (style none: RMB — its own look, or nothing)
  QRect area() const { return rect().adjusted(8, 8, -8, -8); }
  double k() const { const double hw = host ? std::max(1, host->width()) : 1200; return std::min(1.0, area().width() / hw); }   // (the real size → the preview's)
  std::vector<Box> boxes() const {
    const auto& C = overlays::cfg(); const QRect R = area(); const double s = k(); std::vector<Box> out;
    const double hh = host ? std::max(1, host->height()) : 800;   // (the viewport's shape inside the preview)
    vr_ = QRect(R.left(), R.top(), R.width(), int(std::min<double>(R.height(), hh * s)));
    const QRect V = vr_;
    auto put = [&](QSize real, QPointF overlays::Cfg::*pos, overlays::Style overlays::Cfg::*style, QPoint corner) {
      QSize sz(std::max(14, int(real.width() * s)), std::max(8, int(real.height() * s)));
      const QPointF f = C.*pos; QPoint q = corner;
      if (f.x() >= 0) q = QPoint(V.left() + int(f.x() * std::max(0, V.width() - sz.width())), V.top() + int(f.y() * std::max(0, V.height() - sz.height())));
      out.push_back({QRect(q, sz), pos, style});
    };
    if (C.stats) { const QStringList rows = hair ? QStringList{"Convert · 4 locks", "Polygons\t≈ 3,216", "Triangles\t≈ 6,410", "Vertices\t≈ 3,380"} : QStringList{"Scene · 3 meshes", "Vertices\t12,480", "Edges\t24,816", "Polygons\t12,342", "Quads\t12,100", "Tris\t230", "N-gons\t12", "Triangulated\t24,684"};
      QSize real = overlays::statsSize(C.statsS, rows, C.stats == 1); put(real, &overlays::Cfg::statsPos, &overlays::Cfg::statsS, QPoint(V.left() + 6, V.bottom() - int(real.height() * s) - 6)); }
    if (histShown && histShown()) { const QSize real = HistoryBar::sizeFor(host, 78);
      put(real, &overlays::Cfg::histPos, &overlays::Cfg::histS, QPoint(V.left() + (V.width() - int(real.width() * s)) / 2, V.bottom() - int(real.height() * s) - int(14 * s))); }
    if (C.fps) { QSize real = overlays::fpsSize(C.fpsS, "000 FPS · 00.0 ms"); put(real, &overlays::Cfg::fpsPos, &overlays::Cfg::fpsS, QPoint(V.right() - int(real.width() * s) - 6, V.top() + 6)); }
    if (KeyCastWindow* kc = keyCast ? keyCast() : nullptr; kc && kc->isVisible()) { const QSize real = kc->size();   // (the key display: its look on RMB)
      put(real, &overlays::Cfg::keyPos, nullptr, QPoint(V.left() + int(20 * s), V.bottom() - int(real.height() * s) - int(60 * s)));
      out.back().look = [w = const_cast<Preview*>(this), kc](QPoint at) { openKeyCastLookAt(w, at, kc); }; out.back().name = QObject::tr("Keys"); }
    { const QSize real = ui::busySize();   // (the busy curl: always here — it can't be switched off, only placed)
      put(real, &overlays::Cfg::busyPos, nullptr, QPoint(V.right() - int((128 + real.width()) * s), V.bottom() - int((12 + real.height()) * s))); out.back().name = QObject::tr("Busy"); }
    return out;
  }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor(0x3a, 0x45, 0x68)); p.setBrush(QColor(0x23, 0x23, 0x23)); p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    auto bs = boxes();
    p.setPen(QColor(255, 255, 255, 16)); for (int x = vr_.left() + 16; x < vr_.right(); x += 22) p.drawLine(x, vr_.top(), x, vr_.bottom()); for (int y = vr_.top() + 16; y < vr_.bottom(); y += 22) p.drawLine(vr_.left(), y, vr_.right(), y);
    for (size_t i = 0; i < bs.size(); i++) {
      QColor bg = bs[i].style ? (overlays::cfg().*bs[i].style).bg : QColor(0x12, 0x15, 0x1e); bg.setAlphaF(bs[i].style ? std::max(0.35, (overlays::cfg().*bs[i].style).bgOp) : 0.85);
      p.setPen(QPen(int(i) == hot_ || int(i) == drag_ ? QColor(0x8f, 0xb4, 0xff) : QColor(0xe8, 0xe8, 0xe8), 2.5)); p.setBrush(bg);
      p.drawRoundedRect(QRectF(bs[i].r).adjusted(1, 1, -1, -1), 4, 4);
      if (!bs[i].name.isEmpty()) { QFont f = font(); f.setPixelSize(9); p.setFont(f); p.setPen(QColor(0xc8, 0xcb, 0xd8)); p.drawText(bs[i].r, Qt::AlignCenter, bs[i].name); }
    }
    if (bs.empty()) { p.setPen(QColor(0x88, 0x8c, 0x99)); p.drawText(rect(), Qt::AlignCenter, QObject::tr("No info windows on")); }
  }
  int at(const QPoint& q) const { auto bs = boxes(); for (int i = int(bs.size()) - 1; i >= 0; i--) if (bs[size_t(i)].r.adjusted(-3, -3, 3, 3).contains(q)) return i; return -1; }
  void mousePressEvent(QMouseEvent* e) override {
    const int i = at(e->position().toPoint()); if (i < 0) { e->ignore(); return; }
    auto bs = boxes();
    if (e->button() == Qt::RightButton) { const QPoint at = e->globalPosition().toPoint() + QPoint(6, 6); if (bs[size_t(i)].style) openStyleAt(this, at, &(overlays::cfg().*bs[size_t(i)].style), changed); else if (bs[size_t(i)].look) bs[size_t(i)].look(at); return; }
    if (e->button() == Qt::LeftButton) { drag_ = i; grab_ = e->position().toPoint() - bs[size_t(i)].r.topLeft(); dpos_ = bs[size_t(i)].pos; setCursor(Qt::ClosedHandCursor); }
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (drag_ < 0) { int h = at(e->position().toPoint()); if (h != hot_) { hot_ = h; setCursor(h >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor); update(); } e->ignore(); return; }
    auto bs = boxes(); if (drag_ >= int(bs.size())) return; const QSize sz = bs[size_t(drag_)].r.size();
    const QPoint q = e->position().toPoint() - grab_;
    const double fx = vr_.width() > sz.width() ? double(q.x() - vr_.left()) / (vr_.width() - sz.width()) : 0, fy = vr_.height() > sz.height() ? double(q.y() - vr_.top()) / (vr_.height() - sz.height()) : 0;
    overlays::cfg().*dpos_ = QPointF(std::clamp(fx, 0.0, 1.0), std::clamp(fy, 0.0, 1.0)); overlays::save(); update();
    if (KeyCastWindow* kc = keyCast ? keyCast() : nullptr; kc && kc->isVisible()) kc->place();   // (the key display follows at once)   // (the viewport's window follows)
  }
  void mouseReleaseEvent(QMouseEvent* e) override { if (drag_ < 0) { e->ignore(); return; } drag_ = -1; setCursor(Qt::OpenHandCursor); if (changed) changed(); }
  void mouseDoubleClickEvent(QMouseEvent* e) override { const int i = at(e->position().toPoint()); if (i < 0) return; auto bs = boxes(); overlays::cfg().*bs[size_t(i)].pos = QPointF(-1, -1); overlays::save(); if (KeyCastWindow* kc = keyCast ? keyCast() : nullptr; kc && kc->isVisible()) kc->place(); if (changed) changed(); }
  void leaveEvent(QEvent*) override { if (hot_ >= 0) { hot_ = -1; update(); } }
private:
  mutable QRect vr_; int hot_ = -1, drag_ = -1; QPoint grab_; QPointF overlays::Cfg::*dpos_ = nullptr;
};

// Mara: the pointer on her chest — her other picture (on her head she is petted: extras::attachMara)
class MaraHover : public QObject {
public:
  QLabel* l = nullptr; bool chest = false;
  bool eventFilter(QObject*, QEvent* e) override {
    if (e->type() == QEvent::Leave) { if (chest) { chest = false; l->setPixmap(ui::image("mara_insp")); } return false; }
    if (e->type() != QEvent::MouseMove) return false;
    const QPointF p = static_cast<QMouseEvent*>(e)->position(); const double x = p.x() / l->width(), y = p.y() / l->height();
    const bool c = x > 0.15 && x < 0.85 && y >= 0.58;
    if (c != chest) { chest = c; if (c) l->setPixmap(ui::image("mara_insp_2")); else if (y >= 0.58 || x <= 0.12 || x >= 0.8) l->setPixmap(ui::image("mara_insp")); }
    return false;
  }
};
}  // namespace

InspectorWindow::InspectorWindow(QWidget* host, Viewport* vp) : QFrame(host->window(), Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), host_(host), vp_(vp) {
  setAttribute(Qt::WA_TranslucentBackground);                  // (the rounded corners: nothing behind them)
  setObjectName("insWin");
  setStyleSheet("#insBody{background:#111725;border:2px solid #5a8dee;border-radius:16px} #insBody QLabel{color:#c8cbd8}"
                "#insBody QPushButton{background:#1f2638;border:1px solid #3a4568;color:#dde;border-radius:6px;padding:4px 10px}"
                "#insBody QPushButton:checked{background:#3b6fd8;border-color:#5a8dee;color:#fff}"
                "#insBody QPushButton[pic=\"true\"]{padding:2px} #insBody QPushButton[pic=\"true\"]:checked{background:#2b4f9a;border:2px solid #8fb4ff}");
  auto* outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QFrame; body->setObjectName("insBody"); outer->addWidget(body);
  auto* V = new QVBoxLayout(body); V->setContentsMargins(14, 12, 14, 14); V->setSpacing(8);
  auto* top = new QHBoxLayout; top->setSpacing(16); V->addLayout(top);
  // ---- Mara ----
  auto* mara = new QLabel; mara->setFixedSize(150, 150); mara->setScaledContents(true); mara->setPixmap(ui::image("mara_insp")); top->addWidget(mara, 0, Qt::AlignTop);
  { auto* mh = new MaraHover; mh->l = mara; mh->setParent(mara); mara->setMouseTracking(true); mara->installEventFilter(mh); }
  extras::attachMara(mara);                                    // (her head petted — the pat pictures)
  auto* v = new QVBoxLayout; v->setSpacing(6); top->addLayout(v, 1);
  auto* head = new QHBoxLayout; auto* title = new QLabel(tr("Inspector")); title->setStyleSheet("font-weight:600;font-size:14px;color:#fff"); head->addWidget(title); head->addStretch(1);
  auto* x = new QPushButton("×"); x->setFixedSize(26, 22); x->setToolTip(tr("Close")); connect(x, &QPushButton::clicked, this, [this] { hide(); emit closed(); }); head->addWidget(x); v->addLayout(head);
  auto label = [](const QString& t) { auto* l = new QLabel(t); l->setStyleSheet("color:#8a90a8;font-size:10px;letter-spacing:1px;margin-top:2px"); return l; };
  auto pic = [](const char* icon, const QString& tip) { auto* b = new QPushButton; b->setProperty("pic", true); b->setFixedSize(52, 52); b->setIcon(ui::icon(icon)); b->setIconSize(QSize(44, 44)); b->setToolTip(tip); return b; };
  // a slider with its name over it, at the end of a row of buttons
  auto sideSlider = [this](const QString& name, double lo, double hi, double step, double val, std::function<QString(double)> fmt, const QString& tip, std::function<void(double)> set, std::function<void(double)> done = {}) {
    auto* w = new QWidget; w->setFixedWidth(176); auto* l = new QVBoxLayout(w); l->setContentsMargins(6, 0, 0, 0); l->setSpacing(0);
    auto* t = new QLabel(name); t->setStyleSheet("color:#dde;font-size:11px"); l->addWidget(t);
    auto* s = new SliderRow("", lo, hi, step, val, fmt); s->setTip(tip); connect(s, &SliderRow::changed, this, [set](double x) { set(x); });
    if (done) connect(s, &SliderRow::released, this, [done](double x) { done(x); });
    if (auto* q = s->findChild<QSlider*>()) q->setFixedWidth(108);   // (room left for its value)
    l->addWidget(s); w->setToolTip(tip); return w;
  };
  // ---- the Topology room: the highlights on the meshes ----
  topoBox_ = new QWidget; { auto* tv = new QVBoxLayout(topoBox_); tv->setContentsMargins(0, 0, 0, 0); tv->setSpacing(4);
    tv->addWidget(label(tr("HIGHLIGHT")));
    auto* row = new QHBoxLayout; row->setSpacing(6);
    struct H { const char* icon; const char* tip; bool Viewport::Insp::*f; };
    const H hs[] = {{"insp_ngons", "N-gons: the polygons of more than four sides — violet", &Viewport::Insp::ngons},
                    {"insp_open", "Open edges: the mesh's rim (an edge of one polygon) — yellow", &Viewport::Insp::open},
                    {"insp_nonmanifold", "Non-manifold edges: one edge shared by more than two polygons — red", &Viewport::Insp::nonManifold},
                    {"insp_interior", "Interior markers: a mesh's loose pieces left inside it (e.g. leftover polygons inside an open tube) — pink, with a marker seen through", &Viewport::Insp::interior},
                    {"top_hard", "Hard edges: the edges the shading breaks along — sky blue", &Viewport::Insp::hard},
                    {"insp_doubles", "Double walls: polygons lying one on another in one place (e.g. doubled by a mirror) or slid partly over each other (a seam not welded) — hot pink", &Viewport::Insp::doubles},
                    {"insp_nonplanar", "Non-planar polygons: a corner off the polygon's plane — teal (how bent: the slider beside)", &Viewport::Insp::nonPlanar}};
    for (const H& d : hs) {
      auto* b = pic(d.icon, tr(d.tip)); b->setCheckable(true); b->setChecked(vp->insp.*d.f); auto f = d.f;
      connect(b, &QPushButton::toggled, this, [vp, f](bool on) { vp->insp.*f = on; vp->inspChanged(); });
      row->addWidget(b);
    }
    row->addWidget(sideSlider(tr("Non-planar"), 0, 1, 0.01, vp->insp.planarTol, [](double x) { return QString::number(x, 'f', 2); },
                              tr("How bent a polygon must be to be lit as non-planar: 0 — any bend at all, 1 — only those with a corner off their plane by half their longest side"),
                              [vp](double x) { vp->insp.planarTol = x; vp->inspChanged(); }));
    row->addStretch(1); tv->addLayout(row); }
  v->addWidget(topoBox_);
  // ---- the Hair room: the welds' check (its stitch radius), the near anchors, the degenerate curves, the twisted walls ----
  hairBox_ = new QWidget; { auto* hv = new QVBoxLayout(hairBox_); hv->setContentsMargins(0, 0, 0, 0); hv->setSpacing(4);
    hv->addWidget(label(tr("CHECK")));
    auto* row = new QHBoxLayout; row->setSpacing(6);
    auto* run = pic("insp_runcheck", tr("Check connections: the weld places of the selected cocoons' locks (the Inspector's Report — the places, Auto-fix)"));
    connect(run, &QPushButton::clicked, this, [vp] { vp->checkConnections(true); }); row->addWidget(run);
    Scene* sc = vp->scene();
    auto* sliders = new QWidget; auto* sl = new QVBoxLayout(sliders); sl->setContentsMargins(0, 0, 0, 0); sl->setSpacing(2);   // (both sliders in one column, after the buttons)
    sl->addWidget(sideSlider(tr("Stitch radius"), 0.02, 0.5, 0.01, sc->fileRaw.value("lockWeldR").toDouble(0.1), [](double x) { return QString::number(x, 'f', 2); },
                              tr("How far apart two locks' points may be to be stitched, in loop steps (0.1 by default): smaller — fewer places reported, only the really close ones"),
                              [sc](double x) { sc->fileRaw.insert("lockWeldR", x); }, [vp](double x) { vp->setWeldSetting("lockWeldR", x); }));
    struct H { const char* icon; const char* tip; bool Viewport::HInsp::*f; };
    const H hs[] = {{"insp_nearanchors", "Near anchors: anchors of different curves lying practically in one place but not welded — orange", &Viewport::HInsp::near},
                    {"insp_degenerate", "Degenerate curves: fewer than two anchors or no length — red; anchors lying almost one on another — the curve thin red, the place a big yellow mark", &Viewport::HInsp::degen},
                    {"insp_twisted", "Twisted: the walls' quads bent by twisting (the wall turning round its length by over the angle beside within one loop step: the polygons there come out bent) — lime", &Viewport::HInsp::twisted}};
    for (const H& d : hs) {
      auto* b = pic(d.icon, tr(d.tip)); b->setCheckable(true); b->setChecked(vp->hinsp.*d.f); auto f = d.f;
      connect(b, &QPushButton::toggled, this, [vp, f](bool on) { vp->hinsp.*f = on; vp->hinspChanged(); });
      row->addWidget(b);
    }
    sl->addWidget(sideSlider(tr("Twist, °"), 1, 90, 1, std::min(90.0, vp->hinsp.twistDeg), [](double x) { return QString::number(int(x)) + "°"; },
                              tr("Twisted: a wall turning round its length by more than this within one loop step is lit"),
                              [vp](double x) { vp->hinsp.twistDeg = x; vp->hinspChanged(); }));
    row->addWidget(sliders, 0, Qt::AlignTop);
    row->addStretch(1); hv->addLayout(row); }
  v->addWidget(hairBox_);
  // ---- the info windows ----
  v->addWidget(label(tr("WINDOWS")));
  auto* g = new QHBoxLayout; g->setSpacing(6);
  fpsBtn_ = pic("insp_fps", {}); fpsBtn_->setCheckable(true);
  statsBtn_ = pic("insp_stats_col", {}); statsBtn_->setCheckable(true);
  auto* fpsSet = new QPushButton("⚙"); fpsSet->setToolTip(tr("The frame counter's look")); auto* stSet = new QPushButton("⚙"); stSet->setToolTip(tr("The numbers' look"));
  g->addWidget(fpsBtn_); g->addWidget(fpsSet, 0, Qt::AlignBottom);
  g->addSpacing(14); g->addWidget(statsBtn_); g->addWidget(stSet, 0, Qt::AlignBottom);
  histBtn_ = pic("HistorySlider", {}); histBtn_->setCheckable(true); histBtn_->setToolTip(tr("The History slider along the viewport's bottom: every step named, a click goes to it, RMB — delete a step (the later ones stay)"));
  connect(histBtn_, &QPushButton::clicked, this, [this] { if (toggleHistory) toggleHistory(); sync(); });
  auto* histSet = new QPushButton("⚙"); histSet->setToolTip(tr("The History slider's look and whether it shows the step count"));
  g->addSpacing(14); g->addWidget(histBtn_); g->addWidget(histSet, 0, Qt::AlignBottom);
  castBtn_ = pic("insp_keys", tr("Key display: a window showing what is pressed — keys and combinations as key caps, the mouse's buttons and wheel on a mouse picture, repeats as ×N. For recording lessons; placed in the preview below")); castBtn_->setCheckable(true);
  connect(castBtn_, &QPushButton::clicked, this, [this](bool on) { showKeyCast(on); });
  auto* castSet = new QPushButton("⚙"); castSet->setToolTip(tr("The key display's look: the background's opacity, the keys' and the mouse's size"));
  connect(castSet, &QPushButton::clicked, this, [this, castSet] { openKeyCastLook(castSet, keyCast()); });
  g->addSpacing(14); g->addWidget(castBtn_); g->addWidget(castSet, 0, Qt::AlignBottom); g->addStretch(1);
  v->addLayout(g); v->addStretch(1);
  // ---- the preview, the whole width ----
  auto* pv = new Preview; pv->host = host; preview_ = pv; V->addWidget(pv);
  auto changed = [this] { overlays::save(); sync(); if (historyStyled) historyStyled(); };
  pv->changed = changed; pv->histShown = [this] { return historyShown && historyShown(); }; pv->keyCast = [this] { return keyCast(false); };
  connect(fpsBtn_, &QPushButton::clicked, this, [changed] { overlays::cfg().fps = !overlays::cfg().fps; changed(); });
  connect(statsBtn_, &QPushButton::clicked, this, [changed] { int& s = overlays::cfg().stats; s = s == 2 ? 1 : s == 1 ? 0 : 2; changed(); });
  connect(fpsSet, &QPushButton::clicked, this, [fpsSet, changed] { openStyle(fpsSet, &overlays::cfg().fpsS, changed); });
  connect(stSet, &QPushButton::clicked, this, [stSet, changed] { openStyle(stSet, &overlays::cfg().statsS, changed); });
  connect(histSet, &QPushButton::clicked, this, [histSet, changed] { openStyle(histSet, &overlays::cfg().histS, changed); });
  setFixedWidth(760);
  sync();
  if (QSettings().value("keycast/on", false).toBool()) QTimer::singleShot(0, this, [this] { showKeyCast(true); });   // (left open: open again)
}

KeyCastWindow* InspectorWindow::keyCast(bool make) {
  if (!cast_ && make) { cast_ = new KeyCastWindow(host_->window()); cast_->setHost(host_);
    connect(cast_, &KeyCastWindow::closed, this, [this] { QSettings().setValue("keycast/on", false); if (castBtn_) castBtn_->setChecked(false); }); }
  return cast_;
}
void InspectorWindow::showKeyCast(bool on) {
  if (on) keyCast()->open(); else if (cast_) { cast_->hide(); qApp->removeEventFilter(cast_); }
  if (preview_) preview_->update();
  QSettings().setValue("keycast/on", on); if (castBtn_) castBtn_->setChecked(on);
}

void InspectorWindow::sync() {
  const auto& C = overlays::cfg();
  if (histBtn_) histBtn_->setChecked(historyShown && historyShown());
  fpsBtn_->setChecked(C.fps); fpsBtn_->setToolTip(tr("The frame counter in the viewport: %1 — a click switches it").arg(C.fps ? tr("on") : tr("off")));
  statsBtn_->setIcon(ui::icon(C.stats == 1 ? "insp_stats_row" : "insp_stats_col"));   // (off: the column picture, unlit)
  statsBtn_->setToolTip((vp_->room() == "hair" ? tr("The numbers in the viewport — in the Hair room what Convert would make of the selected cocoons: %1 — a click: in a column → in a row → off") : tr("The model's numbers in the viewport: %1 — a click: in a column → in a row → off")).arg(C.stats == 2 ? tr("in a column") : C.stats == 1 ? tr("in a row") : tr("off")));
  statsBtn_->setChecked(C.stats != 0);
  const bool hair = vp_->room() == "hair";                     // (each room its own checks; the summary — the Hair room's)
  topoBox_->setVisible(!hair); hairBox_->setVisible(hair);
  static_cast<Preview*>(preview_)->hair = hair; preview_->update();
  if (isVisible()) QTimer::singleShot(0, this, [this] { place(); });   // (once the other room.s part is gone: its height)
}

// its place: where it was dragged (kept, from the viewport's top right), else the viewport's top right
void InspectorWindow::place() {
  if (!host_) return;
  if (layout()) layout()->activate(); resize(width(), sizeHint().height());   // (the content.s height — another room.s part gone too)
  const QPoint tr = host_->mapToGlobal(QPoint(host_->width(), 0));
  const QJsonObject st = QJsonDocument::fromJson(QSettings().value("inspector/pos").toByteArray()).object();
  QPoint p = st.contains("dx") ? tr + QPoint(st.value("dx").toInt(), st.value("dy").toInt()) : QPoint(tr.x() - width() - 16, tr.y() + 16);
  const QRect g(host_->window()->mapToGlobal(QPoint(0, 0)), host_->window()->size());
  p.setX(std::clamp(p.x(), g.left(), std::max(g.left(), g.right() - width()))); p.setY(std::clamp(p.y(), g.top(), std::max(g.top(), g.bottom() - height())));
  move(p);
}
// dragged by its empty parts (Mara, the labels, the background)
void InspectorWindow::mousePressEvent(QMouseEvent* e) { if (e->button() == Qt::LeftButton) { drag_ = true; grab_ = e->globalPosition().toPoint() - pos(); } }
void InspectorWindow::mouseMoveEvent(QMouseEvent* e) { if (drag_) move(e->globalPosition().toPoint() - grab_); }
void InspectorWindow::mouseReleaseEvent(QMouseEvent*) {
  if (!drag_) return; drag_ = false;
  const QPoint d = pos() - host_->mapToGlobal(QPoint(host_->width(), 0));
  QSettings().setValue("inspector/pos", QJsonDocument(QJsonObject{{"dx", d.x()}, {"dy", d.y()}}).toJson(QJsonDocument::Compact));
}

QPushButton* makeInspectorButton(QWidget* parent) {
  auto* b = new QPushButton(parent); b->setFixedSize(56, 56); b->setIcon(ui::icon("maranspector_1")); b->setIconSize(QSize(52, 52));
  b->setStyleSheet("QPushButton{border:none;background:transparent;padding:0}"); b->setCursor(Qt::PointingHandCursor);
  b->setToolTip(QObject::tr("Inspector: the checks on the meshes / the hair and the viewport's info windows"));
  // (the pointer on it: her frames in turn)
  class Anim : public QObject {
  public:
    QPushButton* b; QTimer t; int i = 0;
    bool eventFilter(QObject*, QEvent* e) override {
      if (e->type() == QEvent::Enter) { i = 0; t.start(); }
      if (e->type() == QEvent::Leave) { t.stop(); b->setIcon(ui::icon("maranspector_1")); }
      return false;
    }
  };
  auto* a = new Anim; a->b = b; a->setParent(b); a->t.setInterval(140);
  QObject::connect(&a->t, &QTimer::timeout, a, [a] { a->i = (a->i + 1) % 5; a->b->setIcon(ui::icon(QString("maranspector_%1").arg(a->i + 1))); });
  b->installEventFilter(a);
  return b;
}

}  // namespace hm
