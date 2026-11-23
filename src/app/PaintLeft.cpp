#include "app/PaintLeft.h"
#include "app/ToonUi.h"
#include <QAction>
#include "app/ColorPicker.h"
#include "app/PaintCanvas.h"
#include "app/PaintPanels.h"
#include "app/PaintProj.h"
#include "app/PaintPreview.h"
#include "app/ToolPanels.h"
#include "app/PaintSession.h"
#include "app/PaintTools.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QSpinBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QCheckBox>
#include <QRadialGradient>
#include <QListWidget>
#include <QDockWidget>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

namespace hm {

namespace {
// the floating manager's title (drag — the panel moves) and its corner (drag — it grows / shrinks, its bottom-right corner staying)
class DragStrip : public QWidget {
public:
  std::function<void(const QPoint&)> dragged; bool grip = false;
  explicit DragStrip(bool isGrip) : grip(isGrip) { setCursor(isGrip ? Qt::SizeFDiagCursor : Qt::SizeAllCursor); if (isGrip) setFixedSize(16, 16); else setFixedHeight(20); setAttribute(Qt::WA_StyledBackground); }
protected:
  void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) { at_ = e->globalPosition().toPoint(); on_ = true; } }
  void mouseMoveEvent(QMouseEvent* e) override { if (!on_) return; const QPoint g = e->globalPosition().toPoint(); if (dragged) dragged(g - at_); at_ = g; }
  void mouseReleaseEvent(QMouseEvent*) override { on_ = false; }
  void paintEvent(QPaintEvent* e) override { QWidget::paintEvent(e); if (!grip) return; QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(QColor(150, 150, 156), 1.4));
    for (int k = 4; k <= 12; k += 4) p.drawLine(QPointF(1, k), QPointF(k, 1)); }
private:
  QPoint at_; bool on_ = false;
};
}


// ---- a list with an eye and a lock on every row ----
class VisList : public QWidget {
public:
  struct Row { int id = -1; QString key, text, tip, note; bool eye = true, lock = false, sel = false, dim = false, part = false, head = false, icons = true; };
  std::function<void(int, bool)> setEye, setLock;           // (a click, a drag over the rows: this row's eye / lock made so)
  std::function<void(int)> soloEye, soloLock;                // (Alt + click)
  std::function<void(int, Qt::KeyboardModifiers)> clicked;
  std::function<void(int, const QPoint&)> menu;
  explicit VisList(QWidget* parent = nullptr) : QWidget(parent) { setMouseTracking(true); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum); }
  void setRows(std::vector<Row> r) { rows_ = std::move(r); setMinimumHeight(int(rows_.size()) * H + 2); updateGeometry(); update(); }
  const std::vector<Row>& rows() const { return rows_; }
  QSize sizeHint() const override { return QSize(120, int(rows_.size()) * H + 2); }
protected:
  static constexpr int H = 22, EX = 2, LX = 22, TX = 44;
  int rowAt(const QPoint& p) const { const int i = p.y() / H; return p.y() >= 0 && i < int(rows_.size()) ? i : -1; }
  int cellAt(const QPoint& p) const { return p.x() < LX ? 0 : p.x() < TX ? 1 : 2; }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    static const QPixmap eyeOn = ui::eyePix(true), eyeOff = ui::eyePix(false, QColor(0x6a, 0x6a, 0x6a));
    for (int i = 0; i < int(rows_.size()); i++) {
      const Row& r = rows_[size_t(i)]; const QRect rr(0, i * H, width(), H);
      if (r.head) { p.setPen(QColor(0x8a, 0x8f, 0x96)); QFont f = font(); f.setPointSizeF(f.pointSizeF() * 0.82); p.setFont(f); p.drawText(rr.adjusted(6, 4, -4, 0), Qt::AlignLeft | Qt::AlignVCenter, r.text.toUpper()); p.setFont(font()); continue; }
      if (r.sel) p.fillRect(rr.adjusted(0, 1, 0, -1), QColor(0x2f, 0x4a, 0x7a)); else if (i == hover_) p.fillRect(rr.adjusted(0, 1, 0, -1), QColor(0x32, 0x32, 0x34));
      int tx = 6;
      if (r.icons) {
        const QPixmap& e = r.eye ? eyeOn : eyeOff; const QSizeF es = e.deviceIndependentSize();
        p.setOpacity(r.dim ? 0.35 : 1); p.drawPixmap(QPointF(EX + (20 - es.width()) / 2, i * H + (H - es.height()) / 2), e); p.setOpacity(1);
        if (r.eye && r.part) { p.setPen(Qt::NoPen); p.setBrush(QColor(0xe0, 0xa0, 0x50)); p.drawEllipse(QPointF(EX + 17, i * H + 6), 2.2, 2.2); }   // (a part of it hidden / locked)
        // the lock: closed and amber — the brush leaves it alone; open and dim — free
        const QPointF c(LX + 10, i * H + H / 2.0 + 1); const QColor lc = r.lock ? QColor(0xe0, 0xa0, 0x50) : QColor(0x66, 0x66, 0x66);
        p.setPen(QPen(lc, 1.5)); p.setBrush(Qt::NoBrush); QPainterPath sh;
        if (r.lock) { sh.moveTo(c + QPointF(-3, -1)); sh.lineTo(c + QPointF(-3, -4)); sh.arcTo(QRectF(c + QPointF(-3, -7.5), QSizeF(6, 6)), 180, -180); sh.lineTo(c + QPointF(3, -1)); }
        else { sh.moveTo(c + QPointF(-3, -1)); sh.lineTo(c + QPointF(-3, -4)); sh.arcTo(QRectF(c + QPointF(-3, -7.5), QSizeF(6, 6)), 180, -150); }
        p.drawPath(sh); p.setPen(Qt::NoPen); p.setBrush(lc); p.drawRoundedRect(QRectF(c + QPointF(-4.5, -1), QSizeF(9, 6.5)), 1.2, 1.2);
        tx = TX + 2;
      }
      p.setPen(r.dim || !r.eye ? QColor(0x80, 0x80, 0x80) : QColor(0xdd, 0xdd, 0xdd));
      const QString note = r.note.isEmpty() ? QString() : "  " + r.note; const int nw = note.isEmpty() ? 0 : fontMetrics().horizontalAdvance(note) + 4;
      p.drawText(QRect(tx, i * H, width() - tx - 4 - nw, H), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(r.text, Qt::ElideRight, width() - tx - 4 - nw));
      if (nw) { p.setPen(QColor(0x80, 0x86, 0x8e)); p.drawText(QRect(width() - nw - 2, i * H, nw, H), Qt::AlignRight | Qt::AlignVCenter, note); }
    }
  }
  void apply(int i, int cell, bool on) { if (cell == 0 && setEye) setEye(i, on); if (cell == 1 && setLock) setLock(i, on); }
  void mousePressEvent(QMouseEvent* e) override {
    const QPoint m = e->position().toPoint(); const int i = rowAt(m); if (i < 0 || rows_[size_t(i)].head) return;
    const Row r = rows_[size_t(i)]; const int c = r.icons ? cellAt(m) : 2;
    if (e->button() == Qt::RightButton) { if (menu) menu(i, e->globalPosition().toPoint()); return; }
    if (e->button() != Qt::LeftButton) return;
    if (c == 2) { if (clicked) clicked(i, e->modifiers()); return; }
    if (e->modifiers() & Qt::AltModifier) { if (c == 0 && soloEye) soloEye(i); if (c == 1 && soloLock) soloLock(i); return; }
    drag_ = c; dragOn_ = c == 0 ? !r.eye : !r.lock; last_ = i; apply(i, c, dragOn_);
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    const QPoint m = e->position().toPoint(); const int i = rowAt(m);
    if (i != hover_) { hover_ = i; update(); }
    if (drag_ < 0 || i < 0 || i == last_ || rows_[size_t(i)].head || !rows_[size_t(i)].icons) return;
    last_ = i; const Row& r = rows_[size_t(i)]; if ((drag_ == 0 ? r.eye : r.lock) != dragOn_) apply(i, drag_, dragOn_);
  }
  void mouseReleaseEvent(QMouseEvent*) override { drag_ = -1; last_ = -1; }
  void leaveEvent(QEvent*) override { if (hover_ >= 0) { hover_ = -1; update(); } }
  bool event(QEvent* e) override {
    if (e->type() == QEvent::ToolTip) { auto* h = static_cast<QHelpEvent*>(e); const int i = rowAt(h->pos());
      if (i >= 0 && !rows_[size_t(i)].head) { const Row& r = rows_[size_t(i)]; const int c = r.icons ? cellAt(h->pos()) : 2;
        QToolTip::showText(h->globalPos(), c == 0 ? QObject::tr("Shown / hidden in the Paint room (the other rooms don't see it). Drag over the eyes — the same for every row passed · Alt + click — only this one shown; again — all")
                           : c == 1 ? QObject::tr("Locked: the brush leaves it alone. Drag over the locks — the same for every row passed · Alt + click — only this one free, the rest locked; again — all free") : r.tip, this); return true; } }
    return QWidget::event(e);
  }
private:
  std::vector<Row> rows_; int hover_ = -1, drag_ = -1, last_ = -1; bool dragOn_ = false;
};

namespace {
QScrollArea* scrolled(QWidget* w) { auto* a = new QScrollArea; a->setWidget(w); a->setWidgetResizable(true); a->setFrameShape(QFrame::NoFrame); a->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); a->setFocusPolicy(Qt::NoFocus); return a; }
QLabel* title(const QString& t) { auto* l = new QLabel(t); l->setObjectName("plHead"); return l; }
QPushButton* flat(const QString& t, const QString& tip) { auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); b->setProperty("plBtn", true); return b; }
QPushButton* ico(QPushButton* b, const char* name, int px = 18) { b->setIcon(ui::icon(name)); b->setIconSize(QSize(px, px)); return b; }   // (an icon on a settings button; with no words on it — the icon alone)

// how a painting tool is laid (PaintSession::drawMode): the mode's icon, its name, what it does
struct DrawModeInfo { const char* key; const char* icon; QString name, tip; };
std::vector<DrawModeInfo> drawModes() {
  return {{"stroke", "pt_mode_stroke", QObject::tr("Stroke"), QObject::tr("By hand: the brush follows the pen")},
          {"polyline", "pt_mode_line", QObject::tr("Line"), QObject::tr("A line through clicked points, as wide as the brush: Enter or a click on the last point lays it, Backspace takes a point off, Esc drops it. On the model the points lie on the surface — turn the camera between them, drag a point to move it")},
          {"lasso", "pt_mode_lasso", QObject::tr("Lasso"), QObject::tr("Outline a region by hand: everything inside takes the tool's effect at once — painted, erased, blurred, recoloured, frozen. Ctrl — the opposite (the brush erases, the mask is freed)")},
          {"rect", "pt_mode_rect", QObject::tr("Rectangle"), QObject::tr("Drag a rectangle (Shift — a square): what is inside takes the tool's effect at once")},
          {"ellipse", "pt_mode_ellipse", QObject::tr("Ellipse"), QObject::tr("Drag an ellipse (Shift — a circle): what is inside takes the tool's effect at once")},
          {"faces", "pt_mode_faces", QObject::tr("Polygons"), QObject::tr("Whole polygons of the mesh: drag — every polygon the pen passes over takes the tool's effect, edge to edge; a click — the one under it")}};
}

// The drawing modes' menu — as 3DCoat's E panel: it is there only while the pointer is over it (or over the button it hangs from) and
// gone the moment the pointer leaves; a click picks the mode. It never takes the focus: the keys stay the viewport's
class ModeFly : public QFrame {
public:
  QRect anchor;                         // (the button it hangs from, on the screen: the pointer there keeps it too)
  bool hold = false;                    // (checks: it stays whatever the pointer does)
  struct Extra { QString key, icon, name, tip; bool on; };
  ModeFly(QWidget* owner, const QString& cur, std::function<void(const QString&)> pick, const std::vector<Extra>& extras = {}, std::function<void(const QString&)> pickExtra = {}) : QFrame(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus) {
    setAttribute(Qt::WA_ShowWithoutActivating); setAttribute(Qt::WA_DeleteOnClose); setAttribute(Qt::WA_StyledBackground); setObjectName("modeFly");
    setStyleSheet("#modeFly{background:#262626;border:1px solid #5c5c5c} #modeFly QLabel{color:#8a8f96;font-size:10px;letter-spacing:1px;padding:0 2px}"
                  " #modeFly QToolButton{background:transparent;border:1px solid transparent;border-radius:6px;color:#d6d6d6;font-size:11px;padding:3px 0 2px 0}"
                  " #modeFly QToolButton:hover{background:#323232;border:1px solid #92776d} #modeFly QToolButton:checked{background:#3d2f52;border:1px solid #9466be}");
    auto* v = new QVBoxLayout(this); v->setContentsMargins(6, 5, 6, 6); v->setSpacing(4);
    v->addWidget(new QLabel(QObject::tr("DRAWING MODE")));
    auto* g = new QGridLayout; g->setSpacing(3); v->addLayout(g); int n = 0;
    for (const DrawModeInfo& m : drawModes()) {
      auto* b = new QToolButton; b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon); b->setIcon(ui::icon(m.icon)); b->setIconSize(QSize(40, 40)); b->setText(m.name); b->setToolTip(m.tip);
      b->setFixedSize(80, 68); b->setCheckable(true); b->setChecked(cur == m.key); b->setFocusPolicy(Qt::NoFocus); g->addWidget(b, n / 3, n % 3); n++;
      const QString key = m.key; QObject::connect(b, &QToolButton::clicked, this, [this, key, pick] { const auto f = pick; close(); f(key); }); }
    if (!extras.empty()) {                   // (the brush with a picture: Stencil, Strip — on / off, a picture chosen when turned on)
      v->addWidget(new QLabel(QObject::tr("WITH A PICTURE"))); auto* g2 = new QGridLayout; g2->setSpacing(3); v->addLayout(g2); int k = 0;
      for (const Extra& x : extras) { auto* b = new QToolButton; b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon); b->setIcon(ui::icon(x.icon)); b->setIconSize(QSize(40, 40)); b->setText(x.name); b->setToolTip(x.tip);
        b->setFixedSize(80, 68); b->setCheckable(true); b->setChecked(x.on); b->setFocusPolicy(Qt::NoFocus); g2->addWidget(b, k / 3, k % 3); k++;
        const QString key = x.key; QObject::connect(b, &QToolButton::clicked, this, [this, key, pickExtra] { const auto f = pickExtra; close(); if (f) f(key); }); } }
    timer_.setInterval(30);
    QObject::connect(&timer_, &QTimer::timeout, this, [this] {
      if (hold) return; const QPoint p = QCursor::pos();
      if (geometry().adjusted(-2, -2, 2, 2).contains(p) || anchor.contains(p) || QApplication::mouseButtons() != Qt::NoButton) return;
      close(); });
  }
  void showAt(QPoint at) {              // (kept on the screen it opens on)
    adjustSize(); if (QScreen* sc = QGuiApplication::screenAt(QCursor::pos())) { const QRect a = sc->availableGeometry(); at.setX(std::clamp(at.x(), a.left(), std::max(a.left(), a.right() - width()))); at.setY(std::clamp(at.y(), a.top(), std::max(a.top(), a.bottom() - height()))); }
    move(at); show(); timer_.start(); }
private:
  QTimer timer_;
};

// (a settings button that tells when the pointer comes over it: the drawing modes' menu opens under it)
class HoverBtn : public QPushButton {
public:
  using QPushButton::QPushButton; std::function<void()> entered;
protected:
  void enterEvent(QEnterEvent* e) override { QPushButton::enterEvent(e); if (entered) entered(); }
};

// (the view's roll on one button: a drag along it turns the view either way — half a degree a pixel, Shift: in 15° steps; a click
// with no drag sets the view upright)
class RollBtn : public QPushButton {
public:
  using QPushButton::QPushButton; std::function<void(double)> roll; std::function<void()> upright;
protected:
  void mousePressEvent(QMouseEvent* e) override { if (e->button() != Qt::LeftButton) return; at_ = e->globalPosition().x(); done_ = 0; moved_ = false; setDown(true); }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (!(e->buttons() & Qt::LeftButton)) return; const double dx = e->globalPosition().x() - at_; if (!moved_ && std::abs(dx) < 3) return; moved_ = true;
    const double raw = dx * 0.5, to = (e->modifiers() & Qt::ShiftModifier) ? std::round(raw / 15) * 15 : raw;
    if (std::abs(to - done_) > 1e-6 && roll) { roll(to - done_); done_ = to; } }
  void mouseReleaseEvent(QMouseEvent* e) override { if (e->button() != Qt::LeftButton) return; setDown(false); if (!moved_ && upright) upright(); }
private:
  double at_ = 0, done_ = 0; bool moved_ = false;
};
}  // namespace

void PaintLeft::modesMenu(bool atPointer, bool hold) {
  if (modeFly_) { modeFly_->close(); modeFly_ = nullptr; }
  const QString t = s_->tool;
  if (!(t == "brush" || t == "eraser" || t == "blur" || t == "smudge" || t == "recolor" || t == "stamp")) { if (atPointer) ui::toast(QObject::tr("The drawing modes belong to the painting tools: the brush, the eraser, Blur, Smudge, Recolor, the Stamp")); return; }
  std::vector<ModeFly::Extra> ex;
  if (proj_ && (t == "brush" || t == "eraser")) {
    ex.push_back({"stencil", "pt_stencil", QObject::tr("Stencil"), QObject::tr("Stencil: a gray picture hung over the viewport — the brush paints through its light parts, the dark ones hold it back. It stays on the screen while the camera moves: line the model up under it. On — a click turns it off"), !proj_->stencilFile.isEmpty()});
    ex.push_back({"strip", "pt_strip", QObject::tr("Strip"), QObject::tr("Strip: a pattern repeated along the stroke in place of the brush's dabs — a seam, a chain, a braid: the picture's height across the stroke (the brush's size), its width along it. On — a click turns it off"), !proj_->stripFile.isEmpty()}); }
  auto* f = new ModeFly(window(), s_->drawMode, [this](const QString& key) {
    modeFlyOff_.start(); tools_->cancel(); s_->drawMode = key; s_->saveDrawPrefs(); vp_->paintOverlayChanged(); syncOpts(); }, ex, [this](const QString& key) {
    modeFlyOff_.start(); const QString imgs = QObject::tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tga)");
    if (key == "stencil") { if (!proj_->stencilFile.isEmpty()) proj_->setStencil({}); else { const QString f = QFileDialog::getOpenFileName(window(), QObject::tr("Stencil"), {}, imgs); if (!f.isEmpty() && !proj_->setStencil(f)) ui::toast(QObject::tr("The picture could not be read")); } }
    if (key == "strip") { if (!proj_->stripFile.isEmpty()) proj_->setStrip({}); else { const QString f = QFileDialog::getOpenFileName(window(), QObject::tr("Strip"), {}, imgs); if (!f.isEmpty() && !proj_->setStrip(f)) ui::toast(QObject::tr("The picture could not be read")); } }
    syncOpts(); });
  f->hold = hold; f->adjustSize(); QPoint at;
  if (atPointer || !modeBtn_ || !modeBtn_->isVisible()) at = QCursor::pos() - QPoint(f->width() / 2, f->height() / 2);
  else { f->anchor = QRect(modeBtn_->mapToGlobal(QPoint(0, 0)), modeBtn_->size()); at = QPoint(f->anchor.left(), f->anchor.top() + f->anchor.height() - 1); }
  modeFly_ = f; f->showAt(at);
}

PaintLeft::PaintLeft(PaintSession* s, Viewport* vp, Scene* sc, QWidget* parent) : QWidget(parent), s_(s), vp_(vp), sc_(sc) {
  setObjectName("paintLeft"); setAttribute(Qt::WA_StyledBackground); setFixedWidth(600);   // (three columns over the manager — the author's sketch, 17.11)
  setStyleSheet("#paintLeft{background:#262626;border-right:1px solid #3a3a3a} #plHead{color:#8a8f96;font-size:10px;letter-spacing:1px;padding:3px 6px 2px 6px}"
                " #plCol{border-right:1px solid #3a3a3a} #plStrip{background:#222224;border-right:1px solid #3a3a3a} QPushButton[plBtn=\"true\"]{padding:2px 5px;font-size:11px} QPushButton[plBtn=\"true\"][lit=\"true\"]{background:#3d2f52;border:1px solid #9466be} #plNote{color:#8a8f96;font-size:11px;padding:0 6px}"
                " QPushButton[tool=\"true\"]:checked{background:#3d2f52;border:1px solid #9466be} QSplitter::handle{background:#3a3a3a}");
  sheet_ = styleSheet();
  auto* v = new QVBoxLayout(this); v->setContentsMargins(0, 0, 1, 0); v->setSpacing(0);
  auto* split = new QSplitter(Qt::Vertical); split->setChildrenCollapsible(false); split->setHandleWidth(3); v->addWidget(split); split_ = split;

  // ---- the manager window: the modes and their tools on its left, the view (the texture / the model) in its middle, the objects and
  // the visibility sets on its right ----
  hist_ = new VisHistory(vp);
  mgr_ = new QWidget; mgr_->setObjectName("plMgr"); auto* mh = new QHBoxLayout(mgr_); mh->setContentsMargins(0, 0, 0, 0); mh->setSpacing(0);
  auto* strip = new QWidget; strip->setObjectName("plStrip"); strip->setAttribute(Qt::WA_StyledBackground); strip->setFixedWidth(40);
  auto* sv = new QVBoxLayout(strip); sv->setContentsMargins(3, 4, 3, 4); sv->setSpacing(3);
  auto sbtn = [&](QVBoxLayout* l, const char* icon, const QString& tip, bool check) { auto* b = new QPushButton; b->setProperty("tool", true); b->setFixedSize(34, 30); b->setFocusPolicy(Qt::NoFocus); b->setToolTip(tip);
    b->setIcon(ui::icon(icon)); b->setIconSize(QSize(24, 24)); b->setCheckable(check); l->addWidget(b, 0, Qt::AlignHCenter); return b; };
  mode2d_ = sbtn(sv, "pm_tex2d", QObject::tr("The texture in 2D: the UV set chosen under this button — painted here as on the model (wheel — zoom, MMB / Space — pan, R — turn)"), true);
  setBtn_ = sbtn(sv, "uv_set", QObject::tr("The UV set shown in 2D"), false);
  { auto* m = new QMenu(setBtn_); QObject::connect(m, &QMenu::aboutToShow, this, [this, m] { m->clear();
      auto add = [&](const QString& name, const QString& key, int w, int h) { QAction* a = m->addAction(QString("%1 · %2×%3").arg(name).arg(w).arg(h)); a->setCheckable(true); a->setChecked(s_->doc.activeSet() == key);
        QObject::connect(a, &QAction::triggered, this, [this, key, w, h] { s_->setSet(key, w, h); setMgrMode(0); }); };
      int w0 = s_->bakeS.size, h0 = s_->bakeS.size; for (const auto& x : s_->doc.sets()) if (x.key.isEmpty()) { w0 = x.w; h0 = x.h; }
      add(QObject::tr("Single texture"), QString(), w0, h0);
      for (const QString& n : vp_->uvSetNamesAll()) { const auto t = vp_->uvSetProps(n); add(n, n, t.w, t.h); } });
    QObject::connect(setBtn_, &QPushButton::clicked, this, [this, m] { m->popup(setBtn_->mapToGlobal(QPoint(setBtn_->width(), 0))); }); }
  modeModel_ = sbtn(sv, "pm_model", QObject::tr("The model's preview (as the Visible Model plugin): nothing is painted there — parts are chosen and hidden with the tools under this button.\n"
                                               "H — hide · Shift+H — only it · Ctrl+H — the object · Ctrl+X — everything · F — frame. Ctrl+Z over this window takes back the hiding, not the stroke"), true);
  QObject::connect(mode2d_, &QPushButton::clicked, this, [this] { setMgrMode(0); });
  QObject::connect(modeModel_, &QPushButton::clicked, this, [this] { setMgrMode(1); });
  selBox_ = new QWidget; auto* selL = new QVBoxLayout(selBox_); selL->setContentsMargins(0, 2, 0, 0); selL->setSpacing(3);
  for (auto [k, icon, tip] : {std::tuple{"face", "tsel_cursor", QObject::tr("Polygon: a click — the polygon under it; held — painted over")},
                              std::tuple{"loop", "tsel_loop", QObject::tr("Loop across the strand: the ring of polygons round the lock at the side nearest the pointer — a lock's band picked in one click")},
                              std::tuple{"ring", "tsel_ring", QObject::tr("Ring: the row of polygons along the strand")},
                              std::tuple{"fill", "tsel_fill", QObject::tr("Fill: the polygons joined to it up to a UV seam or the selection's border")},
                              std::tuple{"part", "tsel_shell", QObject::tr("Part: every polygon joined to it — a whole lock")},
                              std::tuple{"rect", "tsel_rect", QObject::tr("Rectangle: the polygons seen in it")},
                              std::tuple{"lasso", "tsel_lasso", QObject::tr("Lasso: the polygons seen inside the outline")}}) {
    auto* b = sbtn(selL, icon, tip + QObject::tr("\nShift — add, Ctrl — take off · LMB on the empty ground — turn the view"), true); const QString key = k;
    QObject::connect(b, &QPushButton::clicked, this, [this, key] { setMgrKind(key); }); kindBtns_.push_back({key, b}); }
  { auto* vb = sbtn(selL, "pm_vis", QObject::tr("Hide, show, the preview's settings"), false); auto* m = new QMenu(vb); m->setToolTipsVisible(true);
    QObject::connect(m, &QMenu::aboutToShow, this, [this, m] { m->clear(); const bool sel = !vp_->pvSel.empty();
      auto it = [&](const QString& t, const QString& op, bool on) { QAction* a = m->addAction(t); a->setEnabled(on); QObject::connect(a, &QAction::triggered, this, [this, op] { hist_->begin(); vp_->pvKey(op); hist_->end(); }); };
      it(QObject::tr("Hide the selected\tH"), "hide", sel); it(QObject::tr("Only the selected shown\tShift+H"), "only", sel); it(QObject::tr("Show the selected hidden ones\tH over a ghost"), "show", sel);
      it(QObject::tr("Show everything\tCtrl+X"), "showAll", true);
      QObject::connect(m->addAction(QObject::tr("Deselect\tCtrl+D")), &QAction::triggered, this, [this] { vp_->pvSelClear(); });
      m->addSeparator();
      QAction* g = m->addAction(QObject::tr("The hidden as ghosts")); g->setCheckable(true); g->setChecked(preview_->ghosts); g->setToolTip(QObject::tr("The hidden parts drawn faint — a click picks them, H shows them again"));
      QObject::connect(g, &QAction::toggled, this, [this](bool on) { preview_->ghosts = on; QSettings().setValue("paint/mgrGhosts", on); preview_->refresh(); preview_->update(); });
      QAction* f = m->addAction(QObject::tr("Follow the main view's angle")); f->setCheckable(true); f->setChecked(preview_->follow);
      QObject::connect(f, &QAction::toggled, this, [this](bool on) { preview_->follow = on; if (on) preview_->takeMainView(); preview_->update(); });
      QObject::connect(m->addAction(QObject::tr("The main view's angle now")), &QAction::triggered, this, [this] { preview_->takeMainView(); });
      QObject::connect(m->addAction(QObject::tr("Frame the shown\tF")), &QAction::triggered, this, [this] { preview_->frameShown(); }); });
    QObject::connect(vb, &QPushButton::clicked, this, [vb, m] { m->popup(vb->mapToGlobal(QPoint(vb->width(), 0))); }); }
  sv->addWidget(selBox_); sv->addStretch(1);
  floatBtn_ = sbtn(sv, "pm_float", QObject::tr("The manager as a panel of its own over the viewport's bottom right — drag its title to move it, its top-left corner to make it bigger; again — back into the left column"), true);
  QObject::connect(floatBtn_, &QPushButton::clicked, this, [this](bool on) { setMgrFloat(on); });
  undoBtn_ = sbtn(sv, "pm_undo", {}, false); redoBtn_ = sbtn(sv, "pm_redo", {}, false); undoBtn_->hide(); redoBtn_->hide();   // (not shown: Ctrl+Z / Ctrl+Shift+Z over the window — the author, 17.11)
  mgr_->setProperty("help", QStringList{QObject::tr("Over this window Ctrl+Z takes back the last hiding / showing / selection, Ctrl+Shift+Z — again; the strokes are taken back by Ctrl+Z over the model or the texture"),
    QObject::tr("The model's preview: H — hide the selected · Shift+H — only them · Ctrl+X — show everything · F — frame · RMB / Alt + LMB / LMB on the empty ground — turn round the point under the pointer · MMB — move · the wheel — zoom"),
    QObject::tr("The texture: the wheel — zoom · MMB / Space — move · R held + drag — turn · Shift+R — upright")});
  QObject::connect(undoBtn_, &QPushButton::clicked, this, [this] { visUndo(false); });
  QObject::connect(redoBtn_, &QPushButton::clicked, this, [this] { visUndo(true); });
  mh->addWidget(strip);
  auto* hs = new QSplitter(Qt::Horizontal); hs->setChildrenCollapsible(false); hs->setHandleWidth(2); mh->addWidget(hs, 1);
  stack_ = new QStackedWidget; stack_->setMinimumWidth(140); hs->addWidget(stack_);
  // (the lists: objects over the visibility sets)
  auto* lists = new QSplitter(Qt::Vertical); lists->setChildrenCollapsible(false); lists->setHandleWidth(2); lists->setMinimumWidth(110);
  auto column = [&](const QString& head, VisList*& list) {
    auto* w = new QWidget; w->setObjectName("plCol"); w->setAttribute(Qt::WA_StyledBackground); auto* l = new QVBoxLayout(w); l->setContentsMargins(0, 0, 0, 2); l->setSpacing(1);
    l->addWidget(title(head)); list = new VisList; auto* holder = new QWidget; auto* hl = new QVBoxLayout(holder); hl->setContentsMargins(0, 0, 0, 0); hl->addWidget(list); hl->addStretch(1);
    l->addWidget(scrolled(holder), 1); lists->addWidget(w); return l; };
  { auto* l = column(QObject::tr("OBJECTS"), objs_);
    objNote_ = new QLabel; objNote_->setObjectName("plNote"); objNote_->setWordWrap(true); l->addWidget(objNote_);
    auto* row = new QHBoxLayout; row->setContentsMargins(4, 0, 4, 0); row->setSpacing(3);
    auto* des = flat(QObject::tr("Deselect all"), QObject::tr("No object selected: the brush paints on every object (but the locked ones). With objects selected only they are painted"));
    QObject::connect(des, &QPushButton::clicked, this, [this] { vp_->setSelectedItems({}); });
    noPaintBtn_ = flat("NoPainted", QObject::tr("The objects left out of the Paint room by the NoPainted label (RMB on a mesh or in the outliner) — pick one to bring it back")); noPaintBtn_->hide();
    QObject::connect(noPaintBtn_, &QPushButton::clicked, this, [this] { QMenu m(this);
      for (const auto& [id, it] : sc_->items) if (it.raw.value("noPaint").toBool()) { const int i = id; QObject::connect(m.addAction(QObject::tr("Bring back: %1").arg(it.name)), &QAction::triggered, this, [this, i] { vp_->pvSetNoPaint({i}, false); }); }
      m.exec(QCursor::pos()); });
    row->addWidget(des, 1); row->addWidget(noPaintBtn_); l->addLayout(row); }
  { auto* l = column(QObject::tr("VISIBILITY SETS"), sets_);
    auto* row = new QHBoxLayout; row->setContentsMargins(4, 0, 4, 0);
    auto* add = flat(QObject::tr("＋ Set"), QObject::tr("A new visibility set of what is shown now (the objects and polygons hidden in this room stay hidden in it). A click on a set's name — only it shown"));
    QObject::connect(add, &QPushButton::clicked, this, [this] { ui::toast(QObject::tr("Visibility set %1 made").arg(vp_->visSetSave(QString()))); });
    row->addWidget(add, 1); l->addLayout(row); }
  lists->setSizes({190, 150}); hs->addWidget(lists); hs->setStretchFactor(0, 1); hs->setStretchFactor(1, 0); hs->setSizes({250, 140});
  { const QByteArray st = QSettings().value("paint/mgrSplit").toByteArray(); if (!st.isEmpty()) hs->restoreState(st);
    QObject::connect(hs, &QSplitter::splitterMoved, this, [hs] { QSettings().setValue("paint/mgrSplit", hs->saveState()); }); }
  // (the manager goes under the tools — setMgrFloat at the end puts it there, or into its own panel)

  // the objects
  objs_->clicked = [this](int i, Qt::KeyboardModifiers m) { vp_->selectItem(objs_->rows()[size_t(i)].id, bool(m & (Qt::ShiftModifier | Qt::ControlModifier))); };
  objs_->setEye = [this](int i, bool on) { const auto r = objs_->rows()[size_t(i)];
    if (on && r.dim) { vp_->runEdit([id = r.id](Scene& sc, Selection&, const CurveCache&) { for (Item* x = sc.item(id); x; x = x->parent >= 0 ? sc.item(x->parent) : nullptr) x->visible = true; sc.revision++; return QString(); }, false); vp_->sceneChanged(false); }
    vp_->pvSetShown(actOn(r.id), on); };
  objs_->setLock = [this](int i, bool on) { vp_->pvSetLocked(actOn(objs_->rows()[size_t(i)].id), on); };
  objs_->soloEye = [this](int i) { vp_->pvSolo(actOn(objs_->rows()[size_t(i)].id), false); };
  objs_->soloLock = [this](int i) { vp_->pvSolo(actOn(objs_->rows()[size_t(i)].id), true); };
  objs_->menu = [this](int i, const QPoint& g) { const std::vector<int> ids = actOn(objs_->rows()[size_t(i)].id); QMenu m(this);
    QObject::connect(m.addAction(QObject::tr("NoPainted — leave out of the Paint room")), &QAction::triggered, this, [this, ids] { vp_->pvSetNoPaint(ids, true); });
    m.addSeparator();
    QObject::connect(m.addAction(QObject::tr("Only this shown (Alt + click on the eye)")), &QAction::triggered, this, [this, ids] { vp_->pvSolo(ids, false); });
    QObject::connect(m.addAction(QObject::tr("Show everything (Ctrl+X)")), &QAction::triggered, this, [this] { vp_->pvKey("showAll"); });
    m.exec(g); };
  // the visibility sets
  sets_->clicked = [this](int i, Qt::KeyboardModifiers) { const auto r = sets_->rows()[size_t(i)]; vp_->pvSetApply(r.key); ui::toast(r.key == "default" ? QObject::tr("Default: everything shown") : QObject::tr("Only %1 shown").arg(r.text)); };
  sets_->setEye = [this](int i, bool on) { vp_->pvSetToggle(sets_->rows()[size_t(i)].key, false, on); };
  sets_->setLock = [this](int i, bool on) { vp_->pvSetToggle(sets_->rows()[size_t(i)].key, true, on); };
  sets_->soloEye = [this](int i) { vp_->pvSetApply(sets_->rows()[size_t(i)].key); };
  sets_->soloLock = [this](int i) { vp_->pvSetSoloLock(sets_->rows()[size_t(i)].key); };
  sets_->menu = [this](int i, const QPoint& g) { const auto r = sets_->rows()[size_t(i)]; if (!r.key.startsWith("set:")) return; const QString n = r.key.mid(4); QMenu m(this);
    QObject::connect(m.addAction(QObject::tr("Update: what is shown now")), &QAction::triggered, this, [this, n] { vp_->visSetSave(n); ui::toast(QObject::tr("Visibility set %1 updated").arg(n)); });
    QObject::connect(m.addAction(QObject::tr("Rename…")), &QAction::triggered, this, [this, n] { bool ok = false; const QString t = QInputDialog::getText(this, QObject::tr("Visibility set"), QObject::tr("Name"), QLineEdit::Normal, n, &ok); if (ok) vp_->visSetRename(n, t); });
    QObject::connect(m.addAction(QObject::tr("Delete")), &QAction::triggered, this, [this, n] { vp_->visSetDelete(n); });
    m.exec(g); };

  // ---- the texture page: the UV set's name, the islands, fit, a window of its own, over the whole viewport ----
  canvasHost_ = new QWidget; hostLay_ = new QVBoxLayout(canvasHost_); hostLay_->setContentsMargins(0, 0, 0, 0); hostLay_->setSpacing(0);
  auto* bar = new QHBoxLayout; bar->setContentsMargins(4, 3, 4, 3); bar->setSpacing(4);
  setLabel_ = new QLabel; setLabel_->setObjectName("plNote"); setLabel_->setToolTip(QObject::tr("The UV set shown here — chosen with the button under the 2D one on the left. One list of layers serves every set — each set its own pixels and its own size. On the model the brush paints wherever it is put down: a polygon of another set under it — that set is shown here by itself"));
  auto* fit = flat(QObject::tr("Fit"), QObject::tr("Fit the texture to the view (F over it). R held + drag — turn the texture to any angle, Shift+R — upright; each UV set keeps its turn"));
  fullBtn_ = flat(QString::fromUtf8("⛶"), QObject::tr("Open the texture over the whole viewport — to paint in 2D; again — back into this panel")); fullBtn_->setCheckable(true);
  // (the islands over the texture; the texture in a window of its own — a second screen)
  auto* ovB = flat(QString::fromUtf8("◫"), QObject::tr("The UV islands over the texture: the dark ground outside them, their outlines, the polygons' wire"));
  // (the model's wire over the texture: a button of its own — one click on, one off — 2026-11-22, the user's request)
  auto* wireB = flat(QString(), QObject::tr("The model's wire over the texture: its polygons as they lie in the UV")); wireB->setIcon(ui::icon("disp_wire")); wireB->setIconSize(QSize(18, 18));
  wireB->setCheckable(true); wireB->setChecked(s_->ovWire);
  { auto* menu = new QMenu(ovB); QAction* wireA = nullptr;
    auto add = [&](const QString& t, bool PaintSession::*m, const char* key) { auto* a = menu->addAction(t); a->setCheckable(true); a->setChecked(s_->*m);
      QObject::connect(a, &QAction::toggled, this, [this, m, key](bool on) { s_->*m = on; QSettings().setValue(key, on); canvas_->update(); }); return a; };
    add(QObject::tr("Dark ground outside the islands"), &PaintSession::ovDark, "paint/ovDark"); add(QObject::tr("Island outlines"), &PaintSession::ovEdge, "paint/ovEdge"); wireA = add(QObject::tr("Polygon wire"), &PaintSession::ovWire, "paint/ovWire");
    QObject::connect(wireA, &QAction::toggled, wireB, [wireB](bool on) { QSignalBlocker b(wireB); wireB->setChecked(on); });
    QObject::connect(wireB, &QPushButton::toggled, wireA, [wireA](bool on) { wireA->setChecked(on); });
    QObject::connect(ovB, &QPushButton::clicked, this, [ovB, menu] { menu->popup(ovB->mapToGlobal(QPoint(0, ovB->height()))); }); }
  auto* winB = flat(QString::fromUtf8("⧉"), QObject::tr("The texture in a window of its own — move it to a second screen; it paints the same layers"));
  QObject::connect(winB, &QPushButton::clicked, this, [this] { auto* w = makePaintCanvasWindow(s_, window(), canvas_); w->setAttribute(Qt::WA_DeleteOnClose); w->show(); w->raise(); });
  bar->addWidget(setLabel_, 1); bar->addWidget(wireB); bar->addWidget(ovB); bar->addWidget(fit); bar->addWidget(winB); bar->addWidget(fullBtn_); hostLay_->addLayout(bar);
  canvas_ = new PaintCanvas(s, canvasHost_); canvas_->setMinimumSize(120, 120); hostLay_->addWidget(canvas_, 1);
  s->strokeGoing = [c = QPointer<PaintCanvas>(canvas_)] { return c && c->painting(); };   // (files back from the editor wait for the stroke's end)
  QObject::connect(fit, &QPushButton::clicked, canvas_, &PaintCanvas::fit);
  QObject::connect(fullBtn_, &QPushButton::clicked, this, [this](bool on) { setFull(on); });
  QObject::connect(s, &PaintSession::viewChanged, this, [this] { fillSets(); });
  QObject::connect(s, &PaintSession::throughChanged, this, [this] { syncOpts(); });   // (Y or the top bar's button: the shapes' Through button follows)
  stack_->addWidget(canvasHost_);
  // ---- the model page ----
  preview_ = new PaintPreview(vp, sc, hist_); preview_->ghosts = QSettings().value("paint/mgrGhosts", true).toBool();
  preview_->stateChanged = [this] { syncMgr(); };
  stack_->addWidget(preview_);
  QObject::connect(vp, &Viewport::paintVisChanged, this, [this] { hist_->changed(); syncMgr(); preview_->refresh(); });
  setMgrKind(QSettings().value("paint/mgrKind", "part").toString());
  setMgrMode(QSettings().value("paint/mgrMode", 0).toInt());

  // ---- the tools, their settings, the brush in hand ----
  tools_ = new PaintTools(s, vp, canvas_);
  tools_->fillSoft = QSettings().value("paint/fillSoft", 3.0).toFloat(); s_->freezeEdgePx = QSettings().value("paint/freezeEdgePx", 4.0).toFloat();
  tools_->askText = [this](const QString& t) { bool ok = false; const QString r = QInputDialog::getText(window(), t, QObject::tr("The text (drawn with the brush's colour and opacity)"), QLineEdit::Normal, {}, &ok); return ok ? r : QString(); };
  auto* tools = new QWidget; auto* th = new QHBoxLayout(tools); th->setContentsMargins(4, 4, 0, 0); th->setSpacing(4);
  auto* tcw = new QVBoxLayout; auto* tc = new QGridLayout; tc->setSpacing(3); tcw->addLayout(tc); tcw->addStretch(1); int tn = 0;
  auto place = [&](QPushButton* b) { tc->addWidget(b, tn, 0); tn++; };   // (one column — the author, 17.11)
  // (the shader mode under the tools, apart from them — the column's empty height: a click — on / off, a double click — Materials & Shader)
  tcw->insertSpacing(1, 18); tcw->insertWidget(2, new ToonButton(30), 0, Qt::AlignLeft);
  auto mk = [&](const QString& icon, const QString& glyph, const QString& tip) {
    auto* b = new QPushButton; b->setProperty("tool", true); b->setFixedSize(36, 36); b->setFocusPolicy(Qt::NoFocus); b->setToolTip(tip);
    if (!icon.isEmpty()) { b->setIcon(ui::icon(icon)); b->setIconSize(QSize(30, 30)); } else { b->setText(glyph); b->setStyleSheet("font-size:17px"); }
    place(b); return b; };
  auto tool = [&](const QString& id, const QString& icon, const QString& glyph, const QString& tip) { auto* b = mk(icon, glyph, tip); b->setCheckable(true); toolBtns_.push_back({id, b}); QObject::connect(b, &QPushButton::clicked, this, [this, id] { setTool(id); }); };
  tool("brush", "rc_brush", {}, QObject::tr("Brush: paints into the active layer — on the model and on the texture. Hold Ctrl — it erases. Shift + click — a straight stroke from where the last one ended. Alt + RMB drag / [ ] — its size"));
  tool("eraser", "rc_eraser", {}, QObject::tr("Eraser (E — the brush ↔ the eraser, over the model and over the texture): the same brush, erasing"));
  tool("blur", "pt_blur", {}, QObject::tr("Blur: the colours under the brush run together — soft passages between two tones. The brush's size, flow and hardness are its own"));
  tool("smudge", "pt_smudge", {}, QObject::tr("Smudge: the colour pulled along the stroke — its reach by the brush's Bump strength"));
  tool("stamp", "pt_stamp", {}, QObject::tr("Stamp: carries the texture from one place to another under the brush — the mirrored side (symmetric copy), a point set with Ctrl + click (from an anchor), or fills a spot with the colours round it (patch)"));
  tool("recolor", "pt_recolor", {}, QObject::tr("Recolor: changes the colours already painted, never their place — the brush's colour given to them with their light and dark kept (Hue & saturation), lighter / darker, more / less saturated, sharper. Which — in the tool's settings; how strongly — the brush's opacity. A texel passed twice in one stroke is changed once"));
  tool("fill", "rc_fill", {}, QObject::tr("Fill: a click on the model fills the polygon, its part, its polygroup, the whole object, its UV island, a closed contour of the freeze mask or the whole layer (the tool's settings) with the colour at the brush's opacity; Ctrl + click erases it. Gradient: drag — from the colour to the second colour along the drag. With the freeze mask on (❄) the click freezes instead. Locked, hidden and frozen texels stay"));
  tool("piece", "pt_piece", {}, QObject::tr("Piece: on the texture (the 2D window) outline a piece of the active layer with a lasso — it floats: drag it to move, drag outside it to turn (Shift — 15° steps), drag a corner to scale; Enter lays it, Esc drops it. A drawing carried from one UV island onto another by hand; its settings — cut, a new layer, flips"));
  tool("text", "pt_text", {}, QObject::tr("Text: click where it starts, type it — drawn into the active layer with the brush's colour. On the model it lies as it is read on the screen"));
  tool("select", "rc_select", {}, QObject::tr("Select polygons: click — a polygon (the part it belongs to, its polygroup: the tool's settings), a held button paints the selection; Shift — add, Ctrl — take off. H then hides the selected, Shift+H — everything else; they can be locked"));
  tool("toposym", "pt_toposym", {}, QObject::tr("Topological symmetry: mark two neighbouring polygons across the model's middle line — a click: the red one, Ctrl + click: the blue one. Then copy the layer from one side onto the other (the tool's settings): the twin of every polygon is found through the mesh itself — the pose and uneven halves don't matter"));
  { auto* fz = mk("pt_freeze", {}, QObject::tr("Freeze mask: on — the painting tools (in any of their modes: stroke, line, lasso, rectangle, ellipse, polygons) and Fill work on the mask instead of the layer: light blue stripes; Ctrl / the eraser takes it off. Where the mask is, nothing can be painted (a soft edge — partly). Its settings: the tool's panel")); fz->setCheckable(true); freezeBtn_ = fz;
    QObject::connect(fz, &QPushButton::clicked, this, [this](bool on) { s_->freezePaint = on; emit s_->brushChanged(); syncOpts(); }); }
  { // (the view's roll — one button: drag on it and the view turns either way, a click sets it upright; over the model — R held + drag)
    auto* rb = new RollBtn; rb->setProperty("tool", true); rb->setFixedSize(36, 36); rb->setFocusPolicy(Qt::NoFocus); rb->setIcon(ui::icon("pt_view_roll")); rb->setIconSize(QSize(30, 30)); rb->setCursor(Qt::SizeHorCursor);
    rb->setToolTip(QObject::tr("Roll the view: drag on this button left / right — the view turns round its own axis, either way (Shift — in 15° steps): the model is painted at the angle the hand likes. A click — upright again.\nOver the model: R held + drag — the same; Shift+R — upright"));
    rb->roll = [this](double d) { vp_->rollView(d); }; rb->upright = [this] { vp_->rollView(0); }; place(rb);
    auto* rs = mk("pt_view_reset", {}, QObject::tr("Reset the camera: back to where a new project has it")); QObject::connect(rs, &QPushButton::clicked, this, [this] { vp_->resetView(); }); }
  toolsW_ = new QWidget; toolsW_->setLayout(tcw); toolsW_->setMaximumWidth(46); th->addWidget(toolsW_);
  auto* right = new QWidget; auto* rv = new QVBoxLayout(right); rv->setContentsMargins(0, 0, 0, 0); rv->setSpacing(2);
  opts_ = new QWidget; optLay_ = new QVBoxLayout(opts_); optLay_->setContentsMargins(4, 0, 6, 4); optLay_->setSpacing(4); rv->addWidget(opts_);
  QWidget* brushList = nullptr; QWidget* settings = makePaintBrushPanel(s, &brushList);   // (three columns: the tools' buttons | the tool's list (the brushes) | its settings)
  { toolList_ = new QStackedWidget; toolList_->addWidget(brushList);
    auto mkList = [&](const std::vector<std::tuple<QString, QIcon, QString, QString>>& items) { auto* lw = new QListWidget; lw->setIconSize(QSize(26, 26)); lw->setFocusPolicy(Qt::NoFocus); lw->setSpacing(1);
      lw->setStyleSheet("QListWidget{background:#232325;border:none} QListWidget::item{padding:3px 2px;color:#d8d8dc} QListWidget::item:selected{background:#3d2f52;border:1px solid #9466be}");
      for (const auto& [k, ic, name, tip] : items) { auto* it = new QListWidgetItem(ic, name); it->setData(Qt::UserRole, k); it->setToolTip(tip); lw->addItem(it); } toolList_->addWidget(lw); return lw; };
    QPixmap sp(52, 52); sp.fill(Qt::transparent); { QPainter q(&sp); q.setRenderHint(QPainter::Antialiasing);   // (the spheres' picture: three soft balls)
      for (auto [x, y, r, c] : {std::tuple{18.0, 30.0, 14.0, QColor(0xc8, 0xa8, 0xff)}, std::tuple{34.0, 22.0, 11.0, QColor(0x8f, 0x6a, 0xd0)}, std::tuple{34.0, 38.0, 9.0, QColor(0xe8, 0xe0, 0xff)}}) {
        QRadialGradient g(QPointF(x - r * 0.3, y - r * 0.3), r * 1.2); g.setColorAt(0, c.lighter(130)); g.setColorAt(1, c.darker(160)); q.setPen(Qt::NoPen); q.setBrush(g); q.drawEllipse(QPointF(x, y), r, r); } }
    QPixmap cv(52, 52); cv.fill(Qt::transparent); { QPainter q(&cv); q.setRenderHint(QPainter::Antialiasing);   // (the curve gradient's picture: a curved band dark → light)
      QPainterPath path; path.moveTo(10, 44); path.cubicTo(8, 22, 40, 30, 42, 8);
      for (int i = 0; i < 24; i++) { const double t = i / 23.0; const QPointF p = path.pointAtPercent(t); const int v = int(70 + 170 * t); q.setPen(Qt::NoPen); q.setBrush(QColor(v - 30, v - 40, v)); q.drawEllipse(p, 5.5, 5.5); } }
    fillList_ = mkList({{"fill", ui::icon("rc_fill"), QObject::tr("Fill"), QObject::tr("A click fills what the settings say — the polygon, its part, the island, a colour like it… — with the colour")},
                        {"gradient", ui::icon("pt_gradient"), QObject::tr("Gradient"), QObject::tr("Drag over the model: from the colour where the drag begins to the second colour where it ends — a live gradient layer, its ends moved later by dragging them")},
                        {"curve", QIcon(cv), QObject::tr("Along a curve"), QObject::tr("A strand gradient: select the strands (or not — the ones the curve passes over), draw a curve on the model from the root to the tip — the colour → the second colour along it, live: its stops, blend and opacity in the Layers panel; draw again over the active one — it runs along the new curve")},
                        {"spheres", QIcon(sp), QObject::tr("Spheres"), QObject::tr("Colour spheres: a click on the model — a sphere of the colour, a drag from a sphere moves it, Ctrl + click removes it; each its own radius and hardness (the Layers panel). A spheres layer is made if the active one is not")}});
    QObject::connect(fillList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { const QString k = it->data(Qt::UserRole).toString();
      tools_->fillGradient = k == "gradient"; tools_->fillSpheres = k == "spheres"; tools_->fillCurve = k == "curve"; tools_->cancel(); syncOpts(); });
    selList_ = mkList({{"face", ui::icon("tsel_cursor"), QObject::tr("Polygon"), QObject::tr("A click — the polygon under it; held — painted over")}, {"loop", ui::icon("tsel_loop"), QObject::tr("Loop"), QObject::tr("The ring of polygons round the lock (across the strand)")},
                       {"ring", ui::icon("tsel_ring"), QObject::tr("Ring"), QObject::tr("The row of polygons along the strand")}, {"fill", ui::icon("tsel_fill"), QObject::tr("Fill"), QObject::tr("The polygons joined to it up to a UV seam or the selection's border")},
                       {"part", ui::icon("tsel_shell"), QObject::tr("Part"), QObject::tr("Every polygon joined to it — a whole lock")}, {"group", ui::icon("rc_group_new"), QObject::tr("Polygroup"), QObject::tr("Its polygroup")},
                       {"rect", ui::icon("tsel_rect"), QObject::tr("Rectangle"), QObject::tr("The polygons seen in a dragged rectangle")}, {"lasso", ui::icon("tsel_lasso"), QObject::tr("Lasso"), QObject::tr("The polygons seen inside a drawn outline")}});
    QObject::connect(selList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { tools_->selKind = it->data(Qt::UserRole).toString(); const QString k = tools_->selKind;
      vp_->pvHKind = (k == "loop" || k == "ring" || k == "fill" || k == "group") ? k : "part"; vp_->pvHoverAt({}, {}); });
    noList_ = new QWidget; toolList_->addWidget(noList_);
    toolList_->setMinimumWidth(140); toolList_->resize(184, toolList_->height()); brushListW_ = toolList_; toolList_->setObjectName("plCol"); toolList_->setAttribute(Qt::WA_StyledBackground); th->addWidget(toolList_); }
  brushPanel_ = scrolled(settings); rv->addWidget(brushPanel_, 1); rv->addStretch(0); th->addWidget(right, 1); settingsW_ = right;   // (no brush: the tool's settings stay at the top)
  split->addWidget(tools);
  split->setSizes({440, 400});

  // the texture over the whole viewport: a frameless window there, the same canvas moved into it
  fullWin_ = new QWidget(window(), Qt::Tool | Qt::FramelessWindowHint); fullWin_->setObjectName("plFull"); fullWin_->setStyleSheet("#plFull{background:#1e1e20;border:1px solid #555}");
  fullLay_ = new QVBoxLayout(fullWin_); fullLay_->setContentsMargins(1, 1, 1, 1); fullLay_->setSpacing(0);
  { auto* fb = new QHBoxLayout; fb->setContentsMargins(6, 3, 6, 3); auto* l = new QLabel(QObject::tr("Texture — LMB paint · Ctrl erase · Alt pick a colour · wheel zoom · MMB / Space pan · [ ] size")); l->setObjectName("plNote");
    auto* back = flat(QObject::tr("⤡ Back into the panel"), QObject::tr("The texture back into the left column, the model in the viewport again")); QObject::connect(back, &QPushButton::clicked, this, [this] { setFull(false); });
    fb->addWidget(l, 1); fb->addWidget(back); fullLay_->addLayout(fb); }
  fullWin_->hide();
  // the manager's own panel over the viewport: a title to drag, the manager, a corner to size it by
  floatWin_ = new QWidget(window(), Qt::Tool | Qt::FramelessWindowHint); floatWin_->setObjectName("plFloat"); floatWin_->setStyleSheet("#plFloat{background:#262626;border:1px solid #5a5a5e} #plFloatBar{background:#2e2e31;border-bottom:1px solid #3a3a3a}");
  floatLay_ = new QVBoxLayout(floatWin_); floatLay_->setContentsMargins(1, 1, 1, 1); floatLay_->setSpacing(0);
  { auto* bar = new QWidget; bar->setObjectName("plFloatBar"); bar->setAttribute(Qt::WA_StyledBackground); auto* bh = new QHBoxLayout(bar); bh->setContentsMargins(0, 0, 4, 0); bh->setSpacing(4);
    auto* grip = new DragStrip(true); grip->setToolTip(QObject::tr("Drag — the panel bigger or smaller (its bottom-right corner stays)"));
    grip->dragged = [this](const QPoint& d) { fW_ -= d.x(); fH_ -= d.y(); placeFloat(); saveFloat(); };
    auto* title = new DragStrip(false); title->setToolTip(QObject::tr("Drag — move the panel"));
    { auto* tl = new QHBoxLayout(title); tl->setContentsMargins(4, 0, 0, 0); auto* l = new QLabel(QObject::tr("MANAGER")); l->setObjectName("plHead"); l->setAttribute(Qt::WA_TransparentForMouseEvents); tl->addWidget(l); tl->addStretch(1); }
    title->dragged = [this](const QPoint& d) { fRight_ -= d.x(); fBottom_ -= d.y(); placeFloat(); saveFloat(); };
    auto* back = flat(QObject::tr("⤡ Into the column"), QObject::tr("The manager back into the left column"));
    QObject::connect(back, &QPushButton::clicked, this, [this] { setMgrFloat(false); });
    bh->addWidget(grip); bh->addWidget(title, 1); bh->addWidget(back); floatLay_->addWidget(bar); }
  floatWin_->hide();
  { const QList<QVariant> g = QSettings().value("paint/mgrFloatGeo").toList(); if (g.size() == 4) { fRight_ = g[0].toInt(); fBottom_ = g[1].toInt(); fW_ = g[2].toInt(); fH_ = g[3].toInt(); } }

  QObject::connect(vp, &Viewport::paintVisChanged, this, [this] { later(); });
  QObject::connect(vp, &Viewport::sceneEdited, this, [this] { later(); });
  QObject::connect(vp, &Viewport::selectionChanged, this, [this](const std::vector<int>&) { later(); });
  QObject::connect(vp, &Viewport::roomChanged, this, [this](const QString& r) { if (r != "paint") setFull(false); later(); });
  canvas_->eraserKey = [this] { setTool(s_->tool == "eraser" ? "brush" : "eraser"); };
  QObject::connect(s, &PaintSession::brushChanged, this, [this] { tools_->relive(); });   // (the brush's colour changed: the last fill, still live, takes it)
  QObject::connect(vp, &Viewport::paintVisChanged, this, [this] { if (s_->tool == "select") syncOpts(); });
  setTool(s->tool); fillSets(); refresh();
  setMgrFloat(QSettings().value("paint/mgrFloat2", false).toBool());   // (by default under the tools, across the left area — the author's sketch of 17.11; its own panel by the button)
}

// the selected polygons into the freeze mask (or everything but them): their texels in the set painted, a little past their edges
void PaintLeft::freezeSelection(bool rest) {
  if (vp_->pvSel.empty()) { ui::toast(QObject::tr("Select polygons first")); return; }
  s_->ensureDoc(); QImage* fz = s_->freezeOf(true); if (!fz) return;
  PaintSession::Undo u; u.layer = -2; u.img = *fz; s_->pushUndo(std::move(u));   // (Ctrl+Z puts the mask back)
  QImage sel(fz->size(), QImage::Format_Grayscale8); sel.fill(0); const int W = sel.width(), H = sel.height(); int n = 0;
  { QPainter p(&sel); p.setRenderHint(QPainter::Antialiasing, false); p.setPen(QPen(Qt::white, 3)); p.setBrush(Qt::white);
    for (const auto& [m, fs] : vp_->pvSel) for (const auto& f : vp_->pvFaceUvs(m, std::vector<int>(fs.begin(), fs.end()), s_->doc.activeSet())) {
      QPolygonF poly; for (const QPointF& q : f.uv) poly << QPointF(q.x() * W, q.y() * H); p.drawPolygon(poly); n++; } }
  for (int y = 0; y < H; y++) { uchar* d = fz->scanLine(y); const uchar* s = sel.constScanLine(y); for (int x = 0; x < W; x++) d[x] = std::max(d[x], rest ? uchar(255 - s[x]) : s[x]); }
  s_->freezeShown = true; emit s_->pixelsChanged({}); syncOpts();
  ui::toast(rest ? QObject::tr("Everything but %1 polygons frozen — Ctrl+Z / the mask's Clear undo it").arg(n) : QObject::tr("%1 polygons frozen — Ctrl+Z / the mask's Clear undo it").arg(n));
}

void PaintLeft::later() { if (pending_) return; pending_ = true; QTimer::singleShot(0, this, [this] { pending_ = false; refresh(); }); }

void PaintLeft::setTool(const QString& t) {
  if (t != s_->tool) s_->rememberTool();                      // (the tool put down keeps its brush; the one taken gets back its own)
  tools_->setTool(t); vp_->paintRing = t == "brush" || t == "eraser" || t == "blur" || t == "smudge" || t == "recolor" || t == "stamp"; vp_->paintOverlayChanged();
  s_->recallTool(t);
  for (auto& [id, b] : toolBtns_) { QSignalBlocker k(b); b->setChecked(id == t); }
  emit s_->brushChanged(); syncOpts();
}

// the tool's own settings, over the brush's
void PaintLeft::syncToolList() {
  if (!toolList_) return; const QString t = s_->tool; const bool painting = t == "brush" || t == "eraser" || t == "blur" || t == "smudge" || t == "recolor" || t == "stamp";
  QWidget* page = painting ? toolList_->widget(0) : t == "fill" ? static_cast<QWidget*>(fillList_) : t == "select" ? static_cast<QWidget*>(selList_) : noList_;
  toolList_->setCurrentWidget(page); if (brushPanel_) brushPanel_->setVisible(painting);
  const QString fk = tools_->fillSpheres ? "spheres" : tools_->fillCurve ? "curve" : tools_->fillGradient ? "gradient" : "fill";
  for (int i = 0; i < fillList_->count(); i++) if (fillList_->item(i)->data(Qt::UserRole).toString() == fk) { QSignalBlocker b(fillList_); fillList_->setCurrentRow(i); }
  for (int i = 0; i < selList_->count(); i++) if (selList_->item(i)->data(Qt::UserRole).toString() == tools_->selKind) { QSignalBlocker b(selList_); selList_->setCurrentRow(i); }
  for (QWidget* x = toolList_; x; x = x->parentWidget()) if (auto* d = qobject_cast<QDockWidget*>(x)) {   // (the column's title: what it holds now)
    d->setWindowTitle(painting ? QObject::tr("Brushes") : t == "fill" ? QObject::tr("Fill") : t == "select" ? QObject::tr("Selection") : QObject::tr("Presets")); break; }
}
void PaintLeft::syncOpts() {
  while (QLayoutItem* it = optLay_->takeAt(0)) { if (QWidget* w = it->widget()) { w->hide(); w->deleteLater(); } else if (QLayout* l = it->layout()) { while (QLayoutItem* j = l->takeAt(0)) { if (j->widget()) { j->widget()->hide(); j->widget()->deleteLater(); } delete j; } } delete it; }
  const QString t = s_->tool;
  opts_->setProperty("help", QStringList());     // (the tool's notes: under the "?" of its panel, not in it — the author, 17.11)
  auto note = [&](const QString& x) { QStringList h = opts_->property("help").toStringList(); h << x; opts_->setProperty("help", h); return static_cast<QLabel*>(nullptr); };
  auto row = [&] { auto* h = new QHBoxLayout; h->setSpacing(3); optLay_->addLayout(h); return h; };
  // (the settings' buttons are compact icons with no words on them — what each does is in its tip; commands of one kind open as a menu)
  auto ib = [&](QHBoxLayout* h, const char* icon, const QString& tip, std::function<void()> f) { auto* b = flat({}, tip); ico(b, icon, 20); if (f) QObject::connect(b, &QPushButton::clicked, this, f); h->addWidget(b); return b; };
  auto tg = [&](QHBoxLayout* h, const char* icon, const QString& tip, bool on, std::function<void()> f) { auto* b = ib(h, icon, tip, f); b->setCheckable(true); b->setChecked(on); return b; };
  auto mn = [&](QHBoxLayout* h, const char* icon, const QString& tip, bool lit = false) { auto* b = ib(h, icon, tip, nullptr); if (lit) b->setProperty("lit", true); auto* m = new QMenu(b); m->setToolTipsVisible(true); b->setMenu(m); return m; };
  auto item = [this](QMenu* m, const QString& name, const QString& tip, std::function<void()> f, const char* icon = nullptr) { QAction* a = m->addAction(name); a->setToolTip(tip); if (icon) a->setIcon(ui::icon(icon)); QObject::connect(a, &QAction::triggered, this, f); return a; };
  const bool painting = t == "brush" || t == "eraser" || t == "blur" || t == "smudge" || t == "recolor" || t == "stamp";
  if (painting) {                                               // (how the tool is laid — the same switch for every painting tool, and for the freeze mask painted with it)
    // (the mode's own button: its menu opens under it when the pointer comes over it and goes when the pointer leaves)
    const QString cur = s_->drawMode; const auto modes = drawModes(); const DrawModeInfo* mi = &modes[0]; for (const DrawModeInfo& m : modes) if (cur == m.key) mi = &m;
    { auto* mb = new HoverBtn; mb->setObjectName("modePlate"); mb->setFocusPolicy(Qt::NoFocus); mb->setIcon(ui::icon(mi->icon)); mb->setIconSize(QSize(20, 20));
      QStringList on; if (proj_ && (t == "brush" || t == "eraser")) { if (!proj_->stencilFile.isEmpty()) on << QObject::tr("Stencil"); if (!proj_->stripFile.isEmpty()) on << QObject::tr("Strip"); }
      mb->setText("  " + QObject::tr("Drawing mode: %1").arg(mi->name) + (on.isEmpty() ? QString() : "  ·  " + on.join(" · ")) + QString::fromUtf8("  ▾"));
      mb->setMinimumHeight(30); mb->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      mb->setStyleSheet("#modePlate{background:#2e2e31;border:1px solid #45454a;border-radius:4px;color:#e0e0e4;text-align:left;padding:3px 8px;font-weight:bold} #modePlate:hover{border-color:#9466be;background:#34303b}");
      mb->setToolTip(QObject::tr("The drawing mode: %1 — %2\n\nThe same for the brush, the eraser, Blur, Smudge, Recolor and the freeze mask painted with them. Point at it — the modes' menu rolls out under it").arg(mi->name, mi->tip));
      mb->entered = [this] { if (!modeFly_ && (!modeFlyOff_.isValid() || modeFlyOff_.elapsed() > 350)) modesMenu(false); };
      QObject::connect(mb, &QPushButton::clicked, this, [this] { if (!modeFly_) modesMenu(false); }); optLay_->addWidget(mb); modeBtn_ = mb; }
    auto* h = row();
    auto flip = [this](bool PaintSession::* field) { return std::function<void()>([this, field] { s_->*field = !(s_->*field); s_->saveDrawPrefs(); tools_->cancel(); syncOpts(); }); };
    if (cur == "polyline") {
      tg(h, "pt_smooth", QObject::tr("Smooth: on — a curve through the points; off — straight from point to point"), s_->lineSmooth, flip(&PaintSession::lineSmooth));
      tg(h, "pt_flat", QObject::tr("Flat: off — the points lie on the model's surface and the line follows it, round a sleeve too: the camera can be turned between two points. On — the points lie on the screen and the line is carried onto the model from this view, as it is seen"), s_->drawFlat, flip(&PaintSession::drawFlat));
      ib(h, "pt_ok", QObject::tr("Lay the line: the points so far laid with the tool in hand (Enter)"), [this] { tools_->layCurve(); });
      ib(h, "pt_cancel", QObject::tr("Drop the line's points (Esc)"), [this] { tools_->cancel(); }); }
    if (cur == "lasso" || cur == "rect" || cur == "ellipse") {
      tg(h, "pt_through", QObject::tr("Through (Y): on the model, off — only what the camera sees inside the outline; on — what is hidden behind it too (the far side of a sleeve). The same switch as the brush's Through in the top bar"), s_->drawThrough, [this] { s_->setThrough(!s_->drawThrough); tools_->cancel(); syncOpts(); });
      if (cur != "lasso") tg(h, "rc_shape_outline", QObject::tr("Outline only: on — the shape's outline as a stroke of the tool; off — the whole shape"), s_->shapeOutline, flip(&PaintSession::shapeOutline)); }
    if (proj_ && (t == "brush" || t == "eraser") && !proj_->stencilFile.isEmpty()) {   // (the stencil on: tiled or once — chosen in the Drawing mode's menu)
      auto* tb = tg(h, "pt_stencil", QObject::tr("Stencil tiled: the picture repeated over the whole viewport; off — once, nothing painted outside it"), proj_->stTile, [this] { proj_->stTile = !proj_->stTile; proj_->sync(); syncOpts(); }); (void)tb; }
    h->addStretch(1);
    if (cur == "polyline") {
      auto sl = [&](const QString& name, double lo, double hi, double val, const QString& tip, float PaintSession::* field) { auto* r = new SliderRow(name, lo, hi, 1, val * 100, [](double x) { return QString::number(int(std::lround(x))) + " %"; }); r->setToolTip(tip); optLay_->addWidget(r); QObject::connect(r, &SliderRow::changed, this, [this, field](double x) { s_->*field = float(x / 100); s_->saveDrawPrefs(); }); };
      sl(QObject::tr("Line: start width"), 0, 100, s_->lineStart, QObject::tr("The line's width where it begins, of the brush's size: under 100 % it grows from that to the full width"), &PaintSession::lineStart);
      sl(QObject::tr("Line: end width"), 0, 100, s_->lineEnd, QObject::tr("The line's width where it ends: under 100 % it narrows down to that"), &PaintSession::lineEnd);
      sl(QObject::tr("Line: taper length"), 1, 50, s_->lineTaper, QObject::tr("How much of the line's length each end takes to get from its width to the full one: 50 % — the line swells to its middle and thins again"), &PaintSession::lineTaper); }
    if (const bool stOn = proj_ && (t == "brush" || t == "eraser") && !proj_->stencilFile.isEmpty(); stOn) {
      auto sl = [&](const QString& name, double lo, double hi, double step, double val, std::function<QString(double)> fmt, std::function<void(double)> set) { auto* r = new SliderRow(name, lo, hi, step, val, fmt); optLay_->addWidget(r); QObject::connect(r, &SliderRow::changed, this, [this, set](double x) { set(x); proj_->sync(); }); };
      sl(QObject::tr("Stencil scale"), 0.05, 6, 0.01, proj_->stScale, [](double x) { return QString::number(x, 'f', 2); }, [this](double x) { proj_->stScale = x; });
      sl(QObject::tr("Stencil angle"), -180, 180, 1, proj_->stAngle, [](double x) { return QString::number(int(x)) + QString::fromUtf8("°"); }, [this](double x) { proj_->stAngle = x; });
      sl(QObject::tr("Stencil X"), -1, 1, 0.01, proj_->stX, [](double x) { return QString::number(x, 'f', 2); }, [this](double x) { proj_->stX = x; });
      sl(QObject::tr("Stencil Y"), -1, 1, 0.01, proj_->stY, [](double x) { return QString::number(x, 'f', 2); }, [this](double x) { proj_->stY = x; }); }
  }
  if (t == "recolor") { auto* h = row(); auto* c = new QComboBox; c->setFocusPolicy(Qt::NoFocus);
    for (auto [o, n] : {std::pair{paint::RegionOp::Hue, QObject::tr("Hue & saturation — the brush's colour, the light and dark kept")}, std::pair{paint::RegionOp::Lighten, QObject::tr("Lighten")}, std::pair{paint::RegionOp::Darken, QObject::tr("Darken")},
                        std::pair{paint::RegionOp::Saturate, QObject::tr("Saturate")}, std::pair{paint::RegionOp::Desaturate, QObject::tr("Desaturate (towards gray)")}, std::pair{paint::RegionOp::Sharpen, QObject::tr("Sharpen")}}) c->addItem(n, paint::regionOpKey(o));
    c->setCurrentIndex(std::max(0, c->findData(paint::regionOpKey(s_->recolorOp)))); c->setToolTip(QObject::tr("What Recolor does to the colours under it; how strongly — the brush's opacity (2–15 % for a gentle pass)"));
    QObject::connect(c, &QComboBox::activated, this, [this, c](int) { s_->recolorOp = paint::regionOpFromKey(c->currentData().toString()); s_->saveDrawPrefs(); }); h->addWidget(c, 1); }
  if (t == "blur") note(QObject::tr("Blur: stroke over the border of two tones — they run together"));
  if (t == "smudge") note(QObject::tr("Smudge: pulls the colour along the stroke (Bump strength — how far)"));
  if (t == "stamp") { auto* h = row(); auto* c = new QComboBox; c->setFocusPolicy(Qt::NoFocus);
    c->addItem(QObject::tr("Symmetric copy")); c->addItem(QObject::tr("From an anchor")); c->addItem(QObject::tr("Patch"));
    c->setCurrentIndex(std::clamp(s_->stampMode, 0, 2)); c->setToolTip(QObject::tr("What the Stamp lays under the brush"));
    QObject::connect(c, &QComboBox::activated, this, [this](int i) { s_->stampMode = i; QSettings().setValue("paint/stampMode", i); syncOpts(); }); h->addWidget(c, 1);
    if (s_->stampMode != 2) { auto* src = new QComboBox; src->setFocusPolicy(Qt::NoFocus); src->addItem(QObject::tr("Active layer")); src->addItem(QObject::tr("All layers")); src->setCurrentIndex(s_->stampAll ? 1 : 0);
      src->setToolTip(QObject::tr("Where the copy is taken from: the active layer alone, or every layer as it is seen (as 3DCoat's Through All Layers) — laid into the active layer"));
      QObject::connect(src, &QComboBox::activated, this, [this](int i) { s_->stampAll = i == 1; QSettings().setValue("paint/stampAll", s_->stampAll); }); h->addWidget(src, 1); }
    if (s_->stampMode == 1 && s_->stampAnchor) ib(h, "pt_cancel", QObject::tr("The anchor taken off — Ctrl + click sets a new one"), [this] { s_->stampAnchor.reset(); s_->stampOff.reset(); syncOpts(); });
    note(s_->stampMode == 0 ? QObject::tr("Symmetric copy: with Symmetry on (Shift+S), paint where the paint should be — what lies on the mirrored side is laid under the brush, turned and flipped as the texture is there. For paint that was laid without symmetry, or came back from Photoshop lopsided")
       : s_->stampMode == 1 ? QObject::tr("From an anchor: Ctrl + click where the texture is to be taken from, then paint elsewhere — the anchor follows the brush at the same offset, stroke after stroke, until a new Ctrl + click (as Photoshop's Clone Stamp, aligned). On the model and on the texture alike; the offset is in the texture, so it is meant within one UV island or between alike islands")
       : QObject::tr("Patch: paint over a spot — it is filled with the colours round it, the nearest weighing most (as Photoshop's healing). Only the UV islands the spot is on lend their colours. A lasso, a rectangle or an ellipse (Drawing mode) patches the whole shape at once")); }
  if (t == "fill" && tools_->fillCurve) { auto* sl = new SliderRow(QObject::tr("Opacity"), 0, 1, 0.01, s_->brush.opacity, [](double x) { return QString::number(int(std::lround(x * 100))) + " %"; }); optLay_->addWidget(sl);
    QObject::connect(sl, &SliderRow::changed, this, [this](double x) { s_->brush.opacity = float(x); s_->rememberTool(); emit s_->brushChanged(); });
    note(QObject::tr("Along a curve: select the strands in the manager (or none — the strands the curve passes over), then draw a curve on the model from the root to the tip. A live layer: the colour at the curve's start → the second colour at its end; stops, blend and opacity in the Layers panel. Drawn again with that layer active — it runs along the new curve")); }
  if (t == "fill" && !tools_->fillSpheres && !tools_->fillCurve) { auto* h = row(); auto* c = new QComboBox; c->setFocusPolicy(Qt::NoFocus);
    for (auto [k, n] : {std::pair{"face", QObject::tr("Polygon")}, std::pair{"part", QObject::tr("Part (joined polygons)")}, std::pair{"group", QObject::tr("Polygroup")}, std::pair{"object", QObject::tr("Object")}, std::pair{"island", QObject::tr("UV island")},
                        std::pair{"colour", QObject::tr("Similar colour")}, std::pair{"contour", QObject::tr("Closed contour (up to the mask's lines)")}, std::pair{"layer", QObject::tr("Whole layer")}}) c->addItem(n, QString(k));
    c->setCurrentIndex(std::max(0, c->findData(tools_->fillKind))); c->setToolTip(QObject::tr("What a click fills"));
    QObject::connect(c, &QComboBox::activated, this, [this, c](int) { tools_->fillKind = c->currentData().toString(); syncOpts(); }); h->addWidget(c, 1);
    tg(h, "rc_fill_layer", QObject::tr("As a layer: on — the click makes a fill layer over the active one: one flat colour through a mask of what was clicked. Its colour can be changed in the Layers panel at any time"), tools_->fillAsLayer, [this] { tools_->fillAsLayer = !tools_->fillAsLayer; syncOpts(); });
    auto sl = [&](const QString& name, double lo, double hi, double step, double val, std::function<QString(double)> fmt, const QString& tip, std::function<void(double)> set) { auto* r = new SliderRow(name, lo, hi, step, val, fmt); r->setToolTip(tip); optLay_->addWidget(r); QObject::connect(r, &SliderRow::changed, this, set); return r; };
    auto pc = [](double x) { return QString::number(int(std::lround(x * 100))) + " %"; };
    sl(QObject::tr("Opacity"), 0, 1, 0.01, s_->brush.opacity, pc, QObject::tr("How much of the colour the fill lays down"), [this](double x) { s_->brush.opacity = float(x); s_->rememberTool(); emit s_->brushChanged(); });   // (the bar's Opacity follows; the live fill is recoloured through brushChanged)
    sl(QObject::tr("Smooth edge"), 0, 32, 0.5, tools_->fillSoft, [](double x) { return QString::number(x, 'f', 1) + " px"; }, QObject::tr("The pixel staircase along the fill's edge rounded off over this many texels — the edge stays crisp (not feathered); on the paint and on the freeze mask alike. 0 — the polygons' own edge. It never spills onto a UV island the fill did not touch"),
       [this](double x) { tools_->fillSoft = float(x); QSettings().setValue("paint/fillSoft", x); });
    if (tools_->fillKind == "colour") {
      sl(QObject::tr("Colour tolerance"), 0, 1, 0.01, tools_->fillTol, pc, QObject::tr("How far a texel's colour may be from the clicked one and still be filled: 0 — the very same colour"), [this](double x) { tools_->fillTol = float(x); });
      auto* h2 = row(); auto* cg = new QCheckBox(QObject::tr("Joined only")); cg->setChecked(tools_->fillContig); cg->setFocusPolicy(Qt::NoFocus); cg->setToolTip(QObject::tr("On — only the texels joined to the clicked one; off — every texel of a like colour in the UV set"));
      QObject::connect(cg, &QCheckBox::toggled, this, [this](bool on) { tools_->fillContig = on; }); h2->addWidget(cg);
      auto* al = new QCheckBox(QObject::tr("All layers")); al->setChecked(tools_->fillAllLayers); al->setFocusPolicy(Qt::NoFocus); al->setToolTip(QObject::tr("On — the colour as every layer shows it together; off — the active layer's own"));
      QObject::connect(al, &QCheckBox::toggled, this, [this](bool on) { tools_->fillAllLayers = on; }); h2->addWidget(al); h2->addStretch(1); }
    if (!s_->freezePaint && !tools_->fillGradient && !tools_->fillAsLayer) note(QObject::tr("The last fill stays live: pick another colour (the palette, V) or opacity — it is recoloured at once, until something else is done")); }
  if (t == "fill" && tools_->fillSpheres) { auto* sl = new SliderRow(QObject::tr("Opacity"), 0, 1, 0.01, s_->brush.opacity, [](double x) { return QString::number(int(std::lround(x * 100))) + " %"; }); optLay_->addWidget(sl);
    QObject::connect(sl, &SliderRow::changed, this, [this](double x) { s_->brush.opacity = float(x); s_->rememberTool(); emit s_->brushChanged(); });
    note(QObject::tr("Spheres: a click on the model — a sphere of the colour, a drag from a sphere — it moves, Ctrl + click — removed; their radius, hardness and links — in the Layers panel")); }
  if (t == "piece") { note(QObject::tr("On the texture: lasso a piece of the active layer · drag — move · outside — turn · a corner — scale · Enter — lay · Esc — drop")); auto* h = row();
    tg(h, "pt_cut", QObject::tr("Cut: on — the place the piece was taken from is cleared when it is laid; off — it stays: a copy"), tools_->pieceCut, [this] { tools_->pieceCut = !tools_->pieceCut; syncOpts(); });
    tg(h, "rc_layer_new", QObject::tr("New layer: on — the piece is laid onto a new layer above the one it came from"), tools_->pieceNewLayer, [this] { tools_->pieceNewLayer = !tools_->pieceNewLayer; syncOpts(); });
    ib(h, "rc_flip_h", QObject::tr("The floating piece mirrored left ↔ right"), [this] { tools_->pieceFlip(true); }); ib(h, "rc_flip_v", QObject::tr("The floating piece mirrored top ↔ bottom"), [this] { tools_->pieceFlip(false); });
    ib(h, "pt_ok", QObject::tr("Lay the piece into the layer where it floats (Enter)"), [this] { tools_->pieceApply(); }); ib(h, "pt_cancel", QObject::tr("Drop the floating piece — nothing changes (Esc)"), [this] { tools_->pieceDrop(); }); h->addStretch(1); }
  if (t == "text") { { auto* sl = new SliderRow(QObject::tr("Opacity"), 0, 1, 0.01, s_->brush.opacity, [](double x) { return QString::number(int(std::lround(x * 100))) + " %"; }); optLay_->addWidget(sl);
      QObject::connect(sl, &SliderRow::changed, this, [this](double x) { s_->brush.opacity = float(x); s_->rememberTool(); emit s_->brushChanged(); }); }
    auto* h = row(); h->addWidget(new QLabel(QObject::tr("Height")));
    auto* sp = new QSpinBox; sp->setRange(6, 2000); sp->setValue(tools_->textSize); sp->setSuffix(" px"); sp->setToolTip(QObject::tr("The letters' height: screen pixels on the model, texture pixels on the texture")); QObject::connect(sp, &QSpinBox::valueChanged, this, [this](int v) { tools_->textSize = v; }); h->addWidget(sp);
    auto* fc = new QFontComboBox; fc->setFocusPolicy(Qt::ClickFocus); if (!tools_->textFont.isEmpty()) fc->setCurrentFont(QFont(tools_->textFont)); QObject::connect(fc, &QFontComboBox::currentFontChanged, this, [this](const QFont& f) { tools_->textFont = f.family(); }); h->addWidget(fc, 1); }
  if (t == "select") { auto* h = row();
    ib(h, "rc_sel_none", QObject::tr("Deselect: no polygon selected"), [this] { vp_->pvSelClear(); });
    { QMenu* m = mn(h, "rc_lock", QObject::tr("Lock: the brush leaves locked polygons alone"));
      item(m, QObject::tr("Lock the selected polygons"), {}, [this] { vp_->pvSelLock(true); }); item(m, QObject::tr("Unlock the selected polygons"), {}, [this] { vp_->pvSelLock(false); }); }
    { QMenu* m = mn(h, "pt_freeze", QObject::tr("Freeze by the selection: polygons' texels into the freeze mask — the brush leaves them alone"));
      item(m, QObject::tr("Freeze the selected"), QObject::tr("The selected polygons' texels into the freeze mask: the brush leaves them alone"), [this] { freezeSelection(false); });
      item(m, QObject::tr("Freeze everything else"), QObject::tr("Everything but the selected polygons frozen: only they can be painted"), [this] { freezeSelection(true); });
      m->addSeparator();
      item(m, QObject::tr("Mark as the scalp"), QObject::tr("The selected polygons are the scalp — the cap the locks grow from: the brush's \"Near the scalp only\" and the Scalp shadow layer measure from it"), [this] { const int n = vp_->pvSetScalp(true); ui::toast(QObject::tr("The scalp: %1 polygons").arg(n)); });
      item(m, QObject::tr("Unmark as the scalp"), QObject::tr("The selected polygons are no longer the scalp"), [this] { const int n = vp_->pvSetScalp(false); ui::toast(QObject::tr("The scalp: %1 polygons").arg(n)); }); }
    h->addStretch(1);
    { auto* l = new QLabel(QObject::tr("%1 polygons selected").arg(vp_->pvSelCount())); l->setObjectName("plNote"); optLay_->addWidget(l); }
    note(QObject::tr("Selected polygons: H — hide them · Shift+H — only them · Ctrl+X — show everything")); }
  if (t == "toposym") { note(QObject::tr("Click — the red polygon · Ctrl + click — the blue one. Two neighbours across the middle line: the two halves of a part. Or the same polygon of two alike parts (a copy, a mirror copy): the paint is carried from one part onto the other"));
    { auto* h0 = row(); auto* c = new QComboBox; c->setFocusPolicy(Qt::NoFocus); c->addItem(QObject::tr("Two parts: whichever way fits")); c->addItem(QObject::tr("Two parts: mirrored")); c->addItem(QObject::tr("Two parts: the same way round")); c->setCurrentIndex(std::clamp(vp_->pvTsWay, 0, 2));
      c->setToolTip(QObject::tr("For two polygons marked apart, on two alike parts: how the blue part is taken to lie against the red one — a mirror copy (a left and a right sleeve), a plain copy (the same pattern the same way round on both), or whichever pairs the most polygons into the most alike shape")); QObject::connect(c, &QComboBox::activated, this, [this](int i) { vp_->pvTsWay = i; }); h0->addWidget(c, 1); }
    auto* h = row();
    { QMenu* m = mn(h, "pt_transfer", QObject::tr("Copy the active layer across: from one marked side onto the other"));
      item(m, QObject::tr("Red → Blue"), QObject::tr("The active layer's red side copied onto the blue one"), [this] { ui::toast(tools_->topoCopy(0)); });
      item(m, QObject::tr("Blue → Red"), QObject::tr("The active layer's blue side copied onto the red one"), [this] { ui::toast(tools_->topoCopy(1)); });
      item(m, QObject::tr("Swap the two sides"), QObject::tr("The two sides of the active layer swapped"), [this] { ui::toast(tools_->topoCopy(2)); }); }
    tg(h, "pt_freeze", QObject::tr("With the freeze mask: on — the mask is carried across with the layer. While the mask itself is painted (the snowflake in the tools) the copy is the mask's alone"), tools_->topoMask, [this] { tools_->topoMask = !tools_->topoMask; syncOpts(); });
    ib(h, "pt_cancel", QObject::tr("The marks taken off"), [this] { vp_->pvTs = {}; vp_->paintOverlayChanged(); });
    h->addStretch(1); }
  // (the freeze mask: its state on the row — the hard brush, the stripes shown — and one menu of everything that is done with it)
  const QStringList kept = s_->freezeNames();
  if (s_->freezePaint || s_->freezeAny() || !kept.isEmpty()) {
    const bool any = s_->freezeAny();
    auto* h = row(); auto* l = new QLabel(s_->freezePaint ? QObject::tr("❄ Painting the freeze mask") : any ? QObject::tr("❄ Freeze mask") : QObject::tr("❄ Kept masks: %1").arg(kept.size())); l->setObjectName("plNote"); h->addWidget(l, 1);
    tg(h, "pt_hardtip", QObject::tr("Hard brush: on — the mask is painted with a hard round tip in full, whatever the pen presses: a texel is frozen or it is not. Off — the brush as it is set: a soft mask that holds the paint back partly"), s_->freezeHard, [this] { s_->freezeHard = !s_->freezeHard; s_->saveFreezePrefs(); syncOpts(); });
    tg(h, "pt_eye", QObject::tr("Show: the mask's stripes shown / hidden (it holds either way)"), s_->freezeShown, [this] { s_->freezeShown = !s_->freezeShown; emit s_->pixelsChanged({}); });
    if (any) { auto* er = new SliderRow(QObject::tr("Edge"), 1, 32, 0.5, s_->freezeEdgePx, [](double x) { return QString::number(x, 'f', 1) + " px"; });   // (how far the mask's edge commands reach: smooth, expand, contract, border)
      er->setToolTip(QObject::tr("How far the mask's edge commands (the menu ▾) reach: smoothing the staircase, expanding, contracting, the border band")); optLay_->addWidget(er);
      QObject::connect(er, &SliderRow::changed, this, [this](double x) { s_->freezeEdgePx = float(x); QSettings().setValue("paint/freezeEdgePx", x); }); }
    QMenu* m = mn(h, "rc_mask", QObject::tr("The freeze mask's commands: clear, invert, make solid, take from a layer, keep under a name — and the active layer's paint inside or outside the mask erased, lifted onto a new layer, adjusted"));
    item(m, QObject::tr("Clear"), QObject::tr("Nothing frozen in this UV set"), [this] { s_->freezeClear(); syncOpts(); })->setEnabled(any);
    item(m, QObject::tr("Invert"), QObject::tr("The frozen free, the free frozen"), [this] { s_->freezeInvert(); syncOpts(); });
    m->addSeparator();
    item(m, QObject::tr("Edge: smooth the staircase"), QObject::tr("The pixel staircase along the mask's edge rounded off over the Edge width — the edge stays crisp, not feathered"), [this] { s_->freezeEdge(2); syncOpts(); })->setEnabled(any);
    item(m, QObject::tr("Edge: expand"), QObject::tr("The frozen area grown by the Edge width"), [this] { s_->freezeEdge(0); syncOpts(); })->setEnabled(any);
    item(m, QObject::tr("Edge: contract"), QObject::tr("The frozen area shrunk by the Edge width"), [this] { s_->freezeEdge(1); syncOpts(); })->setEnabled(any);
    item(m, QObject::tr("Edge: only the border"), QObject::tr("Only a band along the edge stays frozen, the Edge width either side — to paint just along the border"), [this] { s_->freezeEdge(3); syncOpts(); })->setEnabled(any);
    m->addSeparator();
    item(m, QObject::tr("To 100 %"), QObject::tr("Everything frozen at all becomes fully frozen: a mask painted softly or in several passes stops letting paint through. Its contour is rounded and its edge given a width — the two sliders under the row; with both at 0 the edge is a hard staircase"), [this] { s_->freezeSolid(); ui::toast(QObject::tr("The mask is solid now — Ctrl+Z takes it back")); }, "pt_solid")->setEnabled(any);
    m->addSeparator();
    const QString needL = QObject::tr("Select a colour layer with something painted on it");
    item(m, QObject::tr("From the layer's paint"), QObject::tr("What the active layer has painted — frozen (added to the mask)"), [this, needL] { if (!s_->freezeFromLayer(true)) ui::toast(needL); else syncOpts(); }, "pt_from_layer");
    item(m, QObject::tr("From what the layer leaves empty"), QObject::tr("What the active layer leaves empty — frozen: only the painted part can be changed"), [this, needL] { if (!s_->freezeFromLayer(false)) ui::toast(needL); else syncOpts(); });
    m->addSeparator();
    item(m, QObject::tr("Keep as…"), QObject::tr("The mask as it is now kept under a name: an outline drawn once is not drawn again. The kept masks are saved with the project"), [this] {
      bool ok = false; const QString n = QInputDialog::getText(window(), QObject::tr("Keep the mask"), QObject::tr("The mask's name"), QLineEdit::Normal, QObject::tr("Mask %1").arg(s_->freezeNames().size() + 1), &ok); if (!ok || n.trimmed().isEmpty()) return;
      s_->freezeSave(n); syncOpts(); }, "pt_keep")->setEnabled(any);
    for (const QString& n : kept) { QMenu* k = m->addMenu(n); k->setToolTipsVisible(true);
      item(k, QObject::tr("Use"), QObject::tr("The kept mask in place of the one there is"), [this, n] { if (s_->freezeLoad(n, 0)) syncOpts(); });
      item(k, QObject::tr("Add"), QObject::tr("The kept mask added to the one there is"), [this, n] { if (s_->freezeLoad(n, 1)) syncOpts(); });
      item(k, QObject::tr("Subtract"), QObject::tr("The kept mask taken off the one there is"), [this, n] { if (s_->freezeLoad(n, 2)) syncOpts(); });
      k->addSeparator(); item(k, QObject::tr("Forget"), QObject::tr("The kept mask forgotten"), [this, n] { s_->freezeDrop(n); syncOpts(); }); }
    if (any) {                                                  // (the mask as a region of the active layer: what is frozen, or what is left free)
      m->addSeparator(); const QString need = QObject::tr("Select a colour paint layer that has something in that region");
      for (bool free : {false, true}) { QMenu* r = m->addMenu(free ? QObject::tr("Layer in the free part") : QObject::tr("Layer in the frozen part")); r->setToolTipsVisible(true);
        item(r, QObject::tr("Erase"), QObject::tr("The active layer's paint in the region erased — a soft mask erases softly (Ctrl+Z takes it back)"), [this, need, free] { if (!s_->regionErase(free)) ui::toast(need); }, "rc_eraser");
        item(r, QObject::tr("Copy onto a new layer"), QObject::tr("The active layer's paint in the region onto a new layer above it"), [this, need, free] { if (s_->regionLift(free, false) < 0) ui::toast(need); }, "rc_layer_dup");
        item(r, QObject::tr("Cut onto a new layer"), QObject::tr("The active layer's paint in the region moved onto a new layer above it"), [this, need, free] { if (s_->regionLift(free, true) < 0) ui::toast(need); }, "pt_cut");
        r->addSeparator();
        for (paint::AdjustKind k : {paint::AdjustKind::HueSat, paint::AdjustKind::GradientMap, paint::AdjustKind::BrightContrast, paint::AdjustKind::Levels, paint::AdjustKind::Curves, paint::AdjustKind::Invert, paint::AdjustKind::Posterize})
          item(r, paint::adjustName(k), QObject::tr("An adjustment layer over the active one that works in the region alone (Hue / Saturation with Saturation −100 % — gray). It stays editable — its settings are in the Layers panel"), [this, k, free] {
            if (s_->regionAdjust(k, free) < 0) ui::toast(QObject::tr("Select a colour layer first")); else ui::toast(QObject::tr("%1 over the layer, in the region alone — its settings: the Layers panel").arg(paint::adjustName(k))); }); } }
    if (s_->freezePaint || any) {
      auto sl = [&](const QString& name, double hi, double val, const QString& tip, std::function<void(double)> set) { auto* r = new SliderRow(name, 0, hi, 0.5, val, [](double x) { return x < 0.25 ? QObject::tr("off") : QString::number(x, 'f', 1) + " px"; }); r->setToolTip(tip); optLay_->addWidget(r); QObject::connect(r, &SliderRow::changed, this, [this, set](double x) { set(x); s_->saveFreezePrefs(); }); };
      sl(QObject::tr("To 100 %: contour"), 12, s_->freezeSmooth, QObject::tr("How much the contour is rounded when the mask is made solid: the stairs of a hard edge go, and so does anything thinner than this"), [this](double x) { s_->freezeSmooth = float(x); });
      sl(QObject::tr("To 100 %: edge"), 12, s_->freezeSoft, QObject::tr("The width of the passage from frozen to free, in texels: 0 — a hard edge, 1–2 — a clean antialiased one, more — a soft one"), [this](double x) { s_->freezeSoft = float(x); }); } }
  opts_->setVisible(optLay_->count() > 0); syncToolList();
  { QSignalBlocker b(freezeBtn_); freezeBtn_->setChecked(s_->freezePaint); }
}

std::vector<int> PaintLeft::actOn(int id) const {
  if (!vp_->isItemSelected(id)) return {id};
  std::vector<int> out; for (int x : vp_->selection().items) if (const Item* it = sc_->item(x); it && vp_->pvIsMesh(*it)) out.push_back(x);
  return out.empty() ? std::vector<int>{id} : out;
}

void PaintLeft::fillSets() {
  const QString k = s_->doc.activeSet(); QString t;
  if (k.isEmpty()) t = s_->doc.width() ? QObject::tr("Single texture · %1×%2").arg(s_->doc.width()).arg(s_->doc.height()) : QObject::tr("Single texture");
  else { const auto p = vp_->uvSetProps(k); t = QString("%1 · %2×%3").arg(k).arg(p.w).arg(p.h); }
  if (setLabel_) setLabel_->setText(t);
  if (setBtn_) setBtn_->setToolTip(QObject::tr("The UV set shown in 2D: %1 — click to choose another").arg(t));
}

// ---------- the manager window ----------
void PaintLeft::setMgrMode(int m) {
  mgrMode_ = std::clamp(m, 0, 1); QSettings().setValue("paint/mgrMode", mgrMode_);
  if (stack_) stack_->setCurrentIndex(mgrMode_);
  if (mgrMode_ == 1 && preview_) preview_->refresh();
  syncMgr();
}
void PaintLeft::setMgrKind(const QString& k0) {
  QString k = k0; bool ok = false; for (auto& [id, b] : kindBtns_) ok |= id == k; if (!ok) k = "part";
  if (preview_) preview_->setKind(k); QSettings().setValue("paint/mgrKind", k); syncMgr();
}
void PaintLeft::syncMgr() {
  if (!mgr_) return;
  { QSignalBlocker a(mode2d_), b(modeModel_); mode2d_->setChecked(mgrMode_ == 0); modeModel_->setChecked(mgrMode_ == 1); }
  selBox_->setVisible(mgrMode_ == 1); if (setBtn_) setBtn_->setVisible(mgrMode_ == 0);
  for (auto& [id, b] : kindBtns_) { QSignalBlocker k(b); b->setChecked(preview_ && preview_->kind == id); }
  const int nu = hist_ ? hist_->undoCount() : 0, nr = hist_ ? hist_->redoCount() : 0;
  undoBtn_->setEnabled(nu > 0); redoBtn_->setEnabled(nr > 0);
  undoBtn_->setToolTip(QObject::tr("Take back the last hiding / showing / selection (Ctrl+Z over this window) — %1 step(s). The strokes are not touched: Ctrl+Z over the model or the texture takes them back").arg(nu));
  redoBtn_->setToolTip(QObject::tr("Again what was taken back (Ctrl+Shift+Z over this window) — %1 step(s)").arg(nr));
}
bool PaintLeft::overManager() const {
  if (!mgr_ || !mgr_->isVisible() || sc_->room != "paint") return false; const QPoint g = QCursor::pos();
  if (!QRect(mgr_->mapToGlobal(QPoint(0, 0)), mgr_->size()).contains(g)) return false;
  if (canvas_->isVisible() && QRect(canvas_->mapToGlobal(QPoint(0, 0)), canvas_->size()).contains(g)) return false;   // (over the texture painted in it: the stroke's undo)
  return true;
}
bool PaintLeft::visUndo(bool redo) {
  const bool ok = redo ? hist_->redo() : hist_->undo(); syncMgr();
  ui::hud(ok ? (redo ? QObject::tr("Visibility: again") : QObject::tr("Visibility: taken back")) : (redo ? QObject::tr("Nothing to do again in the visibility") : QObject::tr("Nothing to take back in the visibility")), 700);
  return ok;
}
void PaintLeft::historyReset() { if (hist_) hist_->reset(); syncMgr(); }
bool PaintLeft::previewKey(const QString& op) { return mgrMode_ == 1 && preview_ && preview_->key(op); }
bool PaintLeft::previewFrame() { if (mgrMode_ != 1 || !preview_ || !preview_->pointerOver()) return false; preview_->frameShown(); return true; }

void PaintLeft::refresh() {
  if (sc_->room != "paint") return;
  // the objects
  std::vector<VisList::Row> rows; int nSel = 0;
  for (int id : vp_->pvMeshes()) { const Item& it = *sc_->item(id); VisList::Row r; r.id = id; r.text = it.name; r.dim = !sc_->isVisibleBase(it);
    r.eye = vp_->pvShown(id) && !r.dim; r.lock = vp_->pvLocked(id); r.part = vp_->pvPartHidden(id); r.sel = vp_->isItemSelected(id); nSel += r.sel;
    r.tip = it.name + (r.dim ? QObject::tr("\nHidden in the scene (the outliner's eye in the other rooms) — its eye here shows it") : QString()) + QObject::tr("\nClick — select (Ctrl / Shift — more): only the selected objects are painted · RMB — NoPainted, isolate");
    rows.push_back(r); }
  objs_->setRows(rows);
  objNote_->setText(nSel ? QObject::tr("Painted: the %1 selected").arg(nSel) : QObject::tr("Painted: every object"));
  int np = 0; for (const auto& [id, it] : sc_->items) np += it.raw.value("noPaint").toBool(); noPaintBtn_->setVisible(np > 0); noPaintBtn_->setText(QObject::tr("NoPainted: %1").arg(np));
  // the sets
  rows.clear(); int lastKind = 0;
  for (const auto& x : vp_->pvSets()) {
    if (x.kind != lastKind) { VisList::Row h; h.head = true; h.text = x.kind == 1 ? QObject::tr("UV sets") : QObject::tr("My sets"); rows.push_back(h); lastKind = x.kind; }
    VisList::Row r; r.key = x.key; r.text = x.name; r.eye = x.shown; r.lock = x.locked;
    r.tip = x.kind == 0 ? QObject::tr("Default: everything of the room. Click — everything shown") : x.kind == 1 ? QObject::tr("The polygons of the UV set %1. Click — only them shown").arg(x.name) : QObject::tr("%1: what was shown when the set was made. Click — only that shown · RMB — update, rename, delete").arg(x.name);
    rows.push_back(r); }
  sets_->setRows(rows);
}

void PaintLeft::setDocked() { docked_ = true; mgrFloat_ = false; if (floatBtn_) floatBtn_->hide(); if (floatWin_) floatWin_->hide(); }
void PaintLeft::setMgrFloat(bool on) {
  if (docked_) return;
  mgrFloat_ = on; QSettings().setValue("paint/mgrFloat2", on); if (floatBtn_) { QSignalBlocker b(floatBtn_); floatBtn_->setChecked(on); }
  if (on) { floatLay_->addWidget(mgr_, 1); mgr_->show(); if (isVisible() && sc_->room == "paint") { placeFloat(); floatWin_->show(); floatWin_->raise(); } }
  else { floatWin_->hide(); split_->insertWidget(1, mgr_); mgr_->show(); split_->setSizes({460, 440}); }   // (under the three columns, across their whole width)
}
void PaintLeft::placeFloat() {
  if (!floatWin_ || !viewportRect) return; const QRect r = viewportRect(); if (r.width() < 100 || r.height() < 100) return;
  const int w = std::clamp(fW_, 300, std::max(300, r.width() - 8)), h = std::clamp(fH_, 220, std::max(220, r.height() - 8));
  fW_ = w; fH_ = h; fRight_ = std::clamp(fRight_, 0, std::max(0, r.width() - w)); fBottom_ = std::clamp(fBottom_, 0, std::max(0, r.height() - h));
  floatWin_->setGeometry(r.right() + 1 - fRight_ - w, r.bottom() + 1 - fBottom_ - h, w, h);
}
void PaintLeft::saveFloat() const { QSettings().setValue("paint/mgrFloatGeo", QList<QVariant>{fRight_, fBottom_, fW_, fH_}); }
void PaintLeft::showEvent(QShowEvent* e) { QWidget::showEvent(e); if (window() != this && !window()->property("plFilter").toBool()) { window()->installEventFilter(this); window()->setProperty("plFilter", true); }   /* (the main window's moves: the panel follows) */
  if (mgrFloat_ && floatWin_) { QTimer::singleShot(0, this, [this] { if (!isVisible() || !mgrFloat_) return; placeFloat(); floatWin_->show(); floatWin_->raise(); }); } }
void PaintLeft::hideEvent(QHideEvent* e) { QWidget::hideEvent(e); if (floatWin_) floatWin_->hide(); }
bool PaintLeft::eventFilter(QObject* o, QEvent* e) {
  if (o == window() && mgrFloat_ && floatWin_ && floatWin_->isVisible() && (e->type() == QEvent::Move || e->type() == QEvent::Resize)) QTimer::singleShot(0, this, [this] { placeFloat(); });
  return QWidget::eventFilter(o, e);
}

void PaintLeft::setFull(bool on) {
  if (on == full_) return; full_ = on; { QSignalBlocker b(fullBtn_); fullBtn_->setChecked(on); }
  if (on) { const QRect r = viewportRect ? viewportRect() : QRect(mapToGlobal(QPoint(width(), 0)), QSize(900, 700));
    hostLay_->removeWidget(canvas_); fullLay_->addWidget(canvas_, 1); fullWin_->setGeometry(r); fullWin_->show(); fullWin_->raise(); }
  else { fullLay_->removeWidget(canvas_); hostLay_->addWidget(canvas_, 1); fullWin_->hide(); }
  canvas_->show(); QTimer::singleShot(0, canvas_, [c = canvas_] { c->fit(); });
}

// ======================= the palette =======================
QWidget* makePaintPalettePanel(PaintSession* s) {
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(8, 6, 8, 8); v->setSpacing(5);
  auto* pick = new ColorPicker; pick->setFixedHeight(170); pick->setMode(QSettings().value("paint/pickerMode", "quad").toString()); pick->setColor(s->color); v->addWidget(pick);
  auto* row = new QHBoxLayout; row->setSpacing(4);
  auto* c1 = new QPushButton, *c2 = new QPushButton; for (QPushButton* b : {c1, c2}) { b->setFixedSize(34, 24); b->setFocusPolicy(Qt::NoFocus); }
  c1->setToolTip(QObject::tr("The colour painted with")); c2->setToolTip(QObject::tr("The second colour — click: it becomes the first (X swaps them, D — black and white)"));
  auto* hex = new QLineEdit; hex->setMaxLength(7); hex->setFixedWidth(72); hex->setToolTip(QObject::tr("The colour as #rrggbb"));
  auto* mode = new QComboBox; mode->setFocusPolicy(Qt::NoFocus); for (const QString& k : ColorPicker::modes()) mode->addItem(ColorPicker::modeName(k), k);
  mode->setCurrentIndex(std::max(0, mode->findData(pick->mode()))); mode->setToolTip(QObject::tr("The picker's view"));
  row->addWidget(c1); row->addWidget(c2); row->addWidget(hex); row->addWidget(mode, 1); v->addLayout(row);
  pick->onLoad = [pick, w] { const QString f = QFileDialog::getOpenFileName(w, QObject::tr("A picture to pick colours from"), {}, QObject::tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tga)")); if (!f.isEmpty()) pick->loadImage(f); };
  auto* recent = new SwatchGrid; v->addWidget(recent);
  // the kept colours: ＋ adds the colour in hand, a right click takes one off
  { auto* kl = new QLabel(QObject::tr("Palette")); kl->setProperty("mut", true); v->addWidget(kl); }
  auto* kept = new SwatchGrid; kept->plus = true; v->addWidget(kept);
  auto loadKept = [] { std::vector<QColor> c; for (const QString& n : QSettings().value("paint/palette").toStringList()) if (QColor q(n); q.isValid()) c.push_back(q); return c; };
  auto saveKept = [kept] { QStringList l; for (const QColor& x : kept->colors()) l << x.name(); QSettings().setValue("paint/palette", l); };
  kept->setColors(loadKept());
  kept->added = [kept, s, saveKept] { std::vector<QColor> c = kept->colors(); c.push_back(s->color); kept->setColors(c); saveKept(); };
  kept->removed = [kept, saveKept](int i) { std::vector<QColor> c = kept->colors(); if (i >= 0 && i < int(c.size())) { c.erase(c.begin() + i); kept->setColors(c); saveKept(); } };
  kept->picked = [s](const QColor& c) { s->color = c; emit s->brushChanged(); };
  auto loadRecent = [] { std::vector<QColor> c; for (const QString& n : QSettings().value("paint/recent").toStringList()) if (QColor q(n); q.isValid()) c.push_back(q); return c; };
  recent->setColors(loadRecent());
  auto sw = [](QPushButton* b, const QColor& c) { b->setStyleSheet(QString("QPushButton{background:%1;border:1px solid #666;border-radius:3px}").arg(c.name())); };
  auto sync = [=] { pick->setColor(s->color); sw(c1, s->color); sw(c2, s->color2); if (!hex->hasFocus()) hex->setText(s->color.name()); };
  auto keep = [=](const QColor& c) { std::vector<QColor> cs = recent->colors(); cs.erase(std::remove_if(cs.begin(), cs.end(), [&](const QColor& x) { return x.rgb() == c.rgb(); }), cs.end()); cs.insert(cs.begin(), c); if (cs.size() > 24) cs.resize(24);
    recent->setColors(cs); QStringList l; for (const QColor& x : cs) l << x.name(); QSettings().setValue("paint/recent", l); };
  pick->changed = [=](const QColor& c) { s->color = c; sw(c1, c); if (!hex->hasFocus()) hex->setText(c.name()); emit s->brushChanged(); };
  pick->committed = [=](const QColor& c) { keep(c); };
  recent->picked = [=](const QColor& c) { s->color = c; emit s->brushChanged(); };
  QObject::connect(c2, &QPushButton::clicked, w, [=] { std::swap(s->color, s->color2); emit s->brushChanged(); });
  QObject::connect(hex, &QLineEdit::editingFinished, w, [=] { QString t = hex->text().trimmed(); if (!t.startsWith('#')) t.prepend('#'); if (QColor c(t); c.isValid()) { s->color = c; keep(c); emit s->brushChanged(); } });
  QObject::connect(mode, &QComboBox::activated, w, [=](int) { pick->setMode(mode->currentData().toString()); QSettings().setValue("paint/pickerMode", pick->mode()); });
  QObject::connect(s, &PaintSession::brushChanged, w, sync);
  sync(); v->addStretch(1);   // (the rest of the panel's height left empty below — nothing spread over it)
  return w;
}

}  // namespace hm
