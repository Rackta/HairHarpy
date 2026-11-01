#include "ui/Pie.h"
#include <QApplication>
#include <QDateTime>
#include <QPainter>
#include <QPointer>
#include "ui/Keymap.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <cmath>

namespace hm::ui {

QPointer<Pie> Pie::cur_;

namespace {
const double PI = 3.14159265358979323846;
const int GAP = 5, PADX = 16, PADY = 6, OFF = 26, RING_W = 74;
QFont itemFont() { QFont f("Segoe UI"); f.setPixelSize(12); return f; }
QFont smallFont() { QFont f("Segoe UI"); f.setPixelSize(11); return f; }
}  // namespace

Pie* Pie::current() { return cur_; }
void Pie::closeCurrent() { if (cur_) cur_->finish(true); }

Pie* Pie::open(QWidget* window, const QPoint& global, const PieSpec& spec, bool hold, const QString& holdKey, std::function<void()> onCancel) {
  closeCurrent();
  Pie* p = new Pie(window, spec);
  p->hold_ = hold; p->holdKey_ = holdKey; p->onCancel_ = std::move(onCancel);
  QRect g = window->window()->frameGeometry();
  p->setGeometry(g);
  p->c_ = p->mapFromGlobal(global);
  // (the ring kept inside the window)
  p->layoutItems();
  if (!spec.ring.empty()) {
    int m = int(p->ringR_) + 60; QPoint c = p->c_;
    c.setX(std::clamp(c.x(), m, std::max(m, g.width() - m))); c.setY(std::clamp(c.y(), m, std::max(m, g.height() - m)));
    if (c != p->c_) { p->c_ = c; p->layoutItems(); }
  }
  cur_ = p;
  p->show(); p->raise(); p->activateWindow(); p->setFocus();
  // (the item already under the pointer wakes only once the pointer comes onto it anew)
  return p;
}

Pie::Pie(QWidget* window, const PieSpec& spec) : QWidget(window, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), spec_(spec) {
  setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_DeleteOnClose);
  setMouseTracking(true); setFocusPolicy(Qt::StrongFocus);
  dwell_ = new QTimer(this); dwell_->setSingleShot(true); dwell_->setInterval(220);
  connect(dwell_, &QTimer::timeout, this, [this] { if (hot_ >= 0 && btns_[size_t(hot_)].it->dwell) btns_[size_t(hot_)].it->dwell(); });
}

void Pie::layoutItems() {
  btns_.clear();
  QFontMetrics fm(itemFont());
  auto itemSize = [&](const PieItem& it) { QString t = it.label + (it.sub.empty() ? "" : " ▸"); return QSize(fm.horizontalAdvance(t) + 2 * PADX + 2, fm.height() + 2 * PADY + 2); };
  // the blocks above and below the pointer (all items of a block as wide as its widest)
  auto block = [&](const std::vector<PieItem>& list, bool top) {
    if (list.empty()) return;
    int w = 0, h = 0; for (auto& it : list) { QSize s = itemSize(it); w = std::max(w, s.width()); h = s.height(); }
    int n = int(list.size()), total = n * h + (n - 1) * GAP;
    int y0 = top ? c_.y() - OFF - total : c_.y() + OFF;
    for (int i = 0; i < n; i++) {
      Btn b; b.r = QRect(c_.x() - w / 2, y0 + i * (h + GAP), w, h); b.it = &list[size_t(i)];
      int idx = int(btns_.size()); btns_.push_back(b);
      // (its sub-list: above it, centred)
      if (!list[size_t(i)].sub.empty()) {
        const auto& sub = list[size_t(i)].sub; int sw = 0; for (auto& s : sub) sw = std::max(sw, itemSize(s).width());
        int sn = int(sub.size()), sh = h, st = sn * sh + (sn - 1) * 4;
        for (int k = 0; k < sn; k++) { Btn s; s.r = QRect(b.r.center().x() - sw / 2, b.r.top() - 6 - st + k * (sh + 4), sw, sh); s.it = &sub[size_t(k)]; s.level = 1; s.parent = idx; btns_.push_back(s); }
      }
    }
  };
  block(spec_.top, true); block(spec_.bottom, false);
  if (!spec_.ring.empty()) {
    int n = int(spec_.ring.size()); bool arc = spec_.arc1 != spec_.arc0; double span = arc ? spec_.arc1 - spec_.arc0 : 2 * PI;
    const bool fixed = !arc && spec_.ringAng.size() == spec_.ring.size();   // (eight places, as the pie's editor laid them)
    ringR_ = arc ? std::max(120.0, n * 120 / span) : std::max(92.0, (fixed ? 8 : n) * 92 / (2 * PI)); ringStep_ = fixed ? 2 * PI / 8 : span / n;
    if (spec_.bare) ringR_ *= 1.12;                           // (the bigger pictures: a wider ring)
    if (!spec_.inner.empty()) ringR_ = std::max(ringR_, 250.0);   // (two rings: room for the inner one)
    const int ic = ringIcon();
    QFontMetrics sf(smallFont());
    for (int i = 0; i < n; i++) {
      const PieItem& it = spec_.ring[size_t(i)];
      double a = fixed ? spec_.ringAng[size_t(i)] : arc ? spec_.arc0 + (i + 0.5) * span / n : -PI / 2 + i * 2 * PI / n;
      QPoint p = c_ + QPoint(int(std::lround(std::cos(a) * ringR_)), int(std::lround(std::sin(a) * ringR_)));
      QSize s = it.icon.isEmpty() ? QSize(fm.horizontalAdvance(it.label) + 28, fm.height() + 2 * PADY + 2) : QSize(RING_W, 4 + ic + 4 + 2 + sf.height() + 5);
      Btn b; b.r = QRect(p.x() - s.width() / 2, p.y() - s.height() / 2, s.width(), s.height()); b.it = &it; b.ring = true; b.ang = a;
      btns_.push_back(b);
    }
    // the inner ring (the settings): pills round the centre, from the top
    const int ni = int(spec_.inner.size());
    if (ni) {
      innerR_ = ringR_ * 0.47; innerStep_ = 2 * PI / ni;
      for (int i = 0; i < ni; i++) {
        const PieItem& it = spec_.inner[size_t(i)]; double a = -PI / 2 + i * innerStep_;
        QPoint p = c_ + QPoint(int(std::lround(std::cos(a) * innerR_)), int(std::lround(std::sin(a) * innerR_)));
        QSize s(fm.horizontalAdvance(it.label) + 24, fm.height() + 2 * PADY + 2);
        Btn b; b.r = QRect(p.x() - s.width() / 2, p.y() - s.height() / 2, s.width(), s.height()); b.it = &it; b.ring = true; b.inner = true; b.ang = a;
        btns_.push_back(b);
      }
    }
  }
  update();
}

int Pie::hit(const QPoint& p) const {
  // (an open sub-list first: it lies over other items)
  for (int i = int(btns_.size()) - 1; i >= 0; i--) {
    const Btn& b = btns_[size_t(i)];
    if (b.level == 1 && openSub_ != b.parent) continue;
    if (b.r.contains(p)) return i;
  }
  return -1;
}

// the ring item in the pointer's direction from the centre (more than 30 px away), within half a step
int Pie::ringPick(const QPoint& p) const {
  QPoint d = p - c_; const double r = std::hypot(d.x(), d.y()); if (r < 30) return -1;
  const bool inner = innerR_ > 0 && r < (innerR_ + ringR_) / 2;   // (two rings: the nearer one by the distance)
  double a = std::atan2(d.y(), d.x()), bd = (inner ? innerStep_ : ringStep_) / 2 + 1e-6; int best = -1;
  for (size_t i = 0; i < btns_.size(); i++) {
    if (!btns_[i].ring || btns_[i].inner != inner) continue;
    double dd = std::abs(std::fmod(std::fmod(a - btns_[i].ang, 2 * PI) + 3 * PI, 2 * PI) - PI);
    if (dd <= bd) { bd = dd; best = int(i); }
  }
  return best;
}

void Pie::setHot(int i) {
  if (i == hot_) return;
  int o = hot_; hot_ = i; dwell_->stop();
  if (o >= 0 && btns_[size_t(o)].it->hot) btns_[size_t(o)].it->hot(false);
  // (the sub-list stays open while the pointer is on its item or on one of its own)
  if (i >= 0) { const Btn& b = btns_[size_t(i)]; openSub_ = b.level == 1 ? b.parent : (!b.it->sub.empty() ? i : -1); }
  else openSub_ = -1;
  if (i >= 0) { if (btns_[size_t(i)].it->hot) btns_[size_t(i)].it->hot(true); if (btns_[size_t(i)].it->dwell) dwell_->start(); }
  update();
}

void Pie::run(int i) {
  if (i < 0) return;
  std::function<void()> act = btns_[size_t(i)].it->act;
  if (!act) return;
  if (btns_[size_t(i)].it->keep && spec_.rebuild) {            // (a setting: switched, the menu redrawn where it is)
    act(); auto rb = spec_.rebuild; auto onEnd = spec_.onEnd; spec_ = rb(); spec_.rebuild = rb; if (!spec_.onEnd) spec_.onEnd = onEnd;
    hot_ = -1; openSub_ = -1; dwell_->stop(); layoutItems(); return;
  }
  finish(false);
  act();
}

void Pie::finish(bool cancelled) {
  if (cur_ != this) return;
  cur_ = nullptr; dwell_->stop();
  if (hot_ >= 0 && btns_[size_t(hot_)].it->hot) btns_[size_t(hot_)].it->hot(false);
  if (spec_.onEnd) spec_.onEnd();
  auto cb = onCancel_;
  close();
  if (cancelled && cb) cb();
}

void Pie::mouseMoveEvent(QMouseEvent* e) { int h = hit(e->position().toPoint()); setHot(h >= 0 ? h : ringPick(e->position().toPoint())); }

void Pie::mousePressEvent(QMouseEvent* e) {
  QPoint p = e->position().toPoint(); int h = hit(p);
  if (h < 0 && e->button() == Qt::LeftButton) h = ringPick(p);
  if (h >= 0 && btns_[size_t(h)].it->act && e->button() == Qt::LeftButton) { run(h); return; }
  if (h >= 0) return;                                          // (an item with a sub-list only)
  finish(true);
}

void Pie::mouseReleaseEvent(QMouseEvent* e) {
  if (!hold_ || e->button() != Qt::RightButton) return;
  hold_ = false;
  QPoint p = e->position().toPoint(); int h = hit(p); if (h < 0) h = ringPick(p);
  if (h >= 0 && btns_[size_t(h)].it->act) run(h);             // released over an item → run it; otherwise it stays open
}

std::function<void(const QString&)> Pie::faqHook;

void Pie::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) { finish(true); return; }
  if ((e->key() == Qt::Key_QuoteLeft || e->key() == Qt::Key_AsciiTilde) && faqHook) {   // (~: the help of the item under the pointer)
    int h = hot_ >= 0 ? hot_ : ringPick(mapFromGlobal(QCursor::pos()));
    if (h >= 0) { const QString q = btns_[size_t(h)].it->label + " | " + btns_[size_t(h)].it->tip; auto fh = faqHook; finish(true); fh(q); }
    return;
  }
  if (Keymap::codeOf(e) == holdKey_ && !holdKey_.isEmpty()) return;
  if (Keymap::comboOf(e) == Keymap::get().key("toolPie") && !e->isAutoRepeat()) { finish(true); return; }
}

void Pie::keyReleaseEvent(QKeyEvent* e) { if (!e->isAutoRepeat()) keyReleased(Keymap::codeOf(e)); }

void Pie::keyReleased(const QString& code) {
  if (holdKey_.isEmpty() || code != holdKey_) return;
  holdKey_.clear();
  int h = ringPick(mapFromGlobal(QCursor::pos()));
  if (h >= 0 && btns_[size_t(h)].it->act) run(h);             // released towards a tool → take it; else the ring stays
}

void Pie::paintEvent(QPaintEvent*) {
  QPainter g(this); g.setRenderHint(QPainter::Antialiasing);
  // the ring's disk, the centre
  if (!spec_.ring.empty() && spec_.arc0 == spec_.arc1) {
    double r = ringR_ + 48; g.setPen(QPen(QColor(255, 255, 255, 20), 1)); g.setBrush(QColor(20, 20, 20, 115));
    g.drawEllipse(QPointF(c_), r, r);
    if (innerR_ > 0) { g.setPen(QPen(QColor(255, 255, 255, 26), 1)); g.setBrush(QColor(0, 0, 0, 40)); g.drawEllipse(QPointF(c_), innerR_ + 30, innerR_ + 30); }   // (the settings' ring)
  }
  g.setPen(QPen(QColor(0xbb, 0xbb, 0xbb), 2)); g.setBrush(QColor(0, 0, 0, 90)); g.drawEllipse(QPointF(c_), 12, 12);
  // the title over the top block
  if (!spec_.title.isEmpty() && !spec_.top.empty()) {
    int top = INT_MAX; for (auto& b : btns_) if (b.level == 0 && !b.ring && b.r.bottom() < c_.y()) top = std::min(top, b.r.top());
    QFont f = smallFont(); f.setPixelSize(10); g.setFont(f); QFontMetrics fm(f);
    QRect r(c_.x() - 300, top - 5 - fm.height(), 600, fm.height());
    g.setPen(QColor(0, 0, 0)); g.drawText(r.translated(0, 1), Qt::AlignCenter, spec_.title);
    g.setPen(QColor(0x99, 0x99, 0x99)); g.drawText(r, Qt::AlignCenter, spec_.title);
  }
  auto drawBtn = [&](const Btn& b, int i) {
    const PieItem& it = *b.it; bool hot = i == hot_ || (i == openSub_);
    QColor bg(0x35, 0x35, 0x35), bd(0x5a, 0x5a, 0x5a);
    if (hot) { if (it.danger) { bg = QColor(0x9a, 0x3a, 0x3a); bd = QColor(0xd0, 0x60, 0x60); } else { bg = QColor(0x4a, 0x6f, 0xb0); bd = QColor(0x7a, 0x9f, 0xe0); } }
    if (b.ring && it.cur && !hot) bd = QColor(0xcb, 0xbf, 0xb3);
    bool iconItem = b.ring && !it.icon.isEmpty();
    double rad = iconItem ? 12 : b.r.height() / 2.0;
    QRectF rf = QRectF(b.r).adjusted(0.5, 0.5, -0.5, -0.5);
    if (iconItem && spec_.bare) {                              // (no box: only the pointed-at one lit, the one in use outlined)
      if (hot) { QColor h = bg; h.setAlpha(110); g.setPen(Qt::NoPen); g.setBrush(h); g.drawRoundedRect(rf, rad, rad); }
      else if (it.cur) { g.setPen(QPen(bd, 2)); g.setBrush(Qt::NoBrush); g.drawRoundedRect(rf, rad, rad); }
    } else {
      g.setPen(Qt::NoPen); g.setBrush(QColor(0, 0, 0, 90)); g.drawRoundedRect(rf.translated(0, 2), rad, rad);
      g.setPen(QPen(bd, b.ring && it.cur ? 2 : 1)); g.setBrush(bg); g.drawRoundedRect(rf, rad, rad);
    }
    g.setPen(QColor(0xe6, 0xe6, 0xe6));
    if (iconItem) {
      QPixmap pm = image(it.icon);
      const int ic = ringIcon(); QRect ir(b.r.center().x() - ic / 2, b.r.top() + 4 + 2, ic, ic);
      if (!pm.isNull()) g.drawPixmap(ir, pm);
      const int wide = spec_.bare ? 24 : 0;                   // (no box: the label may run past its edges)
      g.setFont(smallFont()); g.drawText(QRect(b.r.left() - wide, b.r.top() + 4 + ic + 4 + 2, b.r.width() + 2 * wide, b.r.height() - ic - 10), Qt::AlignHCenter | Qt::AlignTop, it.label);
    } else {
      g.setFont(itemFont());
      QString t = it.label;
      if (!it.sub.empty()) {
        QFontMetrics fm(itemFont()); int tw = fm.horizontalAdvance(t), aw = fm.horizontalAdvance(" ▸");
        int x0 = b.r.center().x() - (tw + aw) / 2;
        g.drawText(QRect(x0, b.r.top(), tw, b.r.height()), Qt::AlignVCenter | Qt::AlignLeft, t);
        g.setPen(QColor(0xaa, 0xaa, 0xaa)); g.drawText(QRect(x0 + tw, b.r.top(), aw + 2, b.r.height()), Qt::AlignVCenter | Qt::AlignLeft, " ▸");
      } else g.drawText(b.r, Qt::AlignCenter, t);
    }
  };
  for (size_t i = 0; i < btns_.size(); i++) if (btns_[i].level == 0) drawBtn(btns_[i], int(i));
  for (size_t i = 0; i < btns_.size(); i++) if (btns_[i].level == 1 && btns_[i].parent == openSub_) drawBtn(btns_[i], int(i));
}

// ---- toast / HUD ----
namespace {
QPointer<QWidget> toastHost_, hudHost_;
// (the overlays: windows of the program's own window — above it only, not over the other programs)
QWidget* ownerWin() { return hudHost_ ? hudHost_->window() : toastHost_ ? toastHost_->window() : nullptr; }
constexpr Qt::WindowFlags OVERLAY = Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus;
QLabel* toastLbl() {
  static QPointer<QLabel> l;
  if (!l) {
    l = new QLabel(ownerWin(), OVERLAY);
    l->setAttribute(Qt::WA_ShowWithoutActivating); l->setAttribute(Qt::WA_TransparentForMouseEvents);
    l->setStyleSheet("QLabel{background:#333;border:1px solid #555;padding:7px 14px;border-radius:5px;color:#cfcfcf}");
    l->setWordWrap(true); l->setMaximumWidth(900);
  }
  return l;
}
QLabel* hudLbl() {
  static QPointer<QLabel> l;
  if (!l) {
    l = new QLabel(ownerWin(), OVERLAY);
    l->setAttribute(Qt::WA_ShowWithoutActivating); l->setAttribute(Qt::WA_TransparentForMouseEvents); l->setAttribute(Qt::WA_TranslucentBackground);
    l->setStyleSheet("QLabel{background:rgba(0,0,0,150);padding:4px 10px;border-radius:4px;color:#cfcfcf}");
  }
  return l;
}
}  // namespace

void setOverlayHost(QWidget* toastHost, QWidget* hudHost) {
  toastHost_ = toastHost; hudHost_ = hudHost;
  static bool hooked = false; if (hooked) return; hooked = true;
  QObject::connect(qApp, &QGuiApplication::applicationStateChanged, [](Qt::ApplicationState s) { if (s == Qt::ApplicationActive) return; toastLbl()->hide(); hudLbl()->hide(); });   // (another program in front: the hints gone)
}

namespace {
// the curl (busyIcon: polylines and dots in a 640 × 200 box from 60, 85) drawn into box: grey, filled blue up to p
void drawCurl(QPainter& g, const QRectF& box, double p) {
  static const std::vector<std::vector<QPointF>> L = {{{86, 180}, {140, 130}, {195, 110}, {262, 108}, {305, 108}, {405, 140}, {450, 148}, {495, 138}, {523, 124}}, {{262, 108}, {350, 140}, {415, 175}, {462, 205}, {493, 201}},
    {{96, 209}, {140, 170}, {208, 152}, {245, 140}, {300, 145}, {360, 165}, {415, 200}}, {{208, 152}, {290, 170}, {340, 205}, {390, 235}, {440, 252}, {500, 254}, {564, 237}},
    {{415, 200}, {450, 225}, {500, 235}, {564, 237}, {610, 205}, {664, 121}}, {{493, 201}, {565, 185}, {615, 160}, {664, 121}}};
  static const QPointF D[9] = {{86, 180}, {96, 209}, {262, 108}, {208, 152}, {415, 200}, {493, 201}, {523, 124}, {664, 121}, {564, 237}};
  const double k = std::min(box.width() / 640, box.height() / 200);
  auto draw = [&](const QColor& c) {
    g.save(); g.translate(box.left(), box.top()); g.scale(k, k); g.translate(-60, -85);
    g.setPen(QPen(c, 10, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); g.setBrush(Qt::NoBrush);
    for (auto& pl : L) g.drawPolyline(pl.data(), int(pl.size()));
    g.setPen(Qt::NoPen); g.setBrush(c); for (auto& d : D) g.drawEllipse(d, 13, 13); g.restore(); };
  draw(QColor(0x34, 0x44, 0x4d));
  g.save(); g.setClipRect(QRectF(box.left(), box.top() - 4, 640 * k * std::clamp(p, 0.0, 1.0), box.height() + 8)); draw(QColor(0x3e, 0xc6, 0xf2)); g.restore();
}
}  // namespace

namespace {
// the curl and its label under it — as wide as the label; no way to close it (it goes when the work is done)
class BusyWidget : public QWidget {
public:
  BusyWidget() : QWidget(ownerWin(), OVERLAY) {
    setAttribute(Qt::WA_ShowWithoutActivating); setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_TranslucentBackground); setFixedSize(100, 48);
    creep_.setInterval(90); QObject::connect(&creep_, &QTimer::timeout, this, [this] { if (!set) { p += (0.9 - p) * 0.04; update(); } });   // (no progress of its own — while the program runs (a save in the background): creeping toward 0.9)
  }
  QString label; double p = 0; int depth = 0; bool set = false;
  void setLabel(const QString& l) { label = l; QFont f = font(); f.setPointSizeF(7.5); setFixedSize(std::max(100, QFontMetrics(f).horizontalAdvance(label + "…") + 10), 48); }
protected:
  void showEvent(QShowEvent*) override { creep_.start(); }
  void hideEvent(QHideEvent*) override { creep_.stop(); }
  void paintEvent(QPaintEvent*) override {
    QPainter g(this); g.setRenderHint(QPainter::Antialiasing);
    drawCurl(g, QRectF(width() - 96, 2, 92, 92.0 * 200 / 640), p);
    QFont f = font(); f.setPointSizeF(7.5); g.setFont(f); g.setPen(QColor(0x8f, 0xd6, 0xf2));
    g.drawText(QRectF(0, 32, width() - 4, 14), Qt::AlignRight | Qt::AlignVCenter, label + "…");
  }
private:
  QTimer creep_;
};
BusyWidget* busyW() { static QPointer<BusyWidget> w; if (!w) w = new BusyWidget; return w; }
// its place: set in the Inspector (a share of the viewport's free room), else the viewport's bottom right
void busyPlace(BusyWidget* w) {
  if (!toastHost_) return; const QRect g(toastHost_->mapToGlobal(QPoint(0, 0)), toastHost_->size());
  const QPointF f = busyPosHook() ? busyPosHook()() : QPointF(-1, -1);
  if (f.x() < 0) { w->move(g.right() - 128 - w->width(), g.bottom() - 12 - w->height()); return; }
  w->move(g.left() + int(f.x() * std::max(0, g.width() - w->width())), g.top() + int(f.y() * std::max(0, g.height() - w->height())));
}
void busyShow(const QString& label, double p, bool set) {
  BusyWidget* w = busyW(); w->setLabel(label); w->p = std::clamp(p, 0.0, 1.0); w->set = set; busyPlace(w); w->show(); w->raise(); w->repaint();
}
}  // namespace

std::function<QPointF()>& busyPosHook() { static std::function<QPointF()> h; return h; }
QSize busySize() { return QSize(150, 48); }
BusyScope::BusyScope(const QString& label) : t0_(QDateTime::currentMSecsSinceEpoch()) {
  busyW()->depth++; busyShow(label, 0.1, false);
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);   // (painted before the task holds the program)
}
BusyScope::~BusyScope() { BusyWidget* w = busyW(); if (--w->depth <= 0) { w->depth = 0; w->hide(); } }
void BusyScope::progress(double p) { BusyWidget* w = busyW(); w->p = std::clamp(p, 0.0, 1.0); w->set = true; w->repaint(); QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); }
void busyBegin(const QString& label) { busyW()->depth++; busyShow(label, 0.1, false); }
void busyEnd() { BusyWidget* w = busyW(); if (--w->depth <= 0) { w->depth = 0; w->hide(); } }

// ---- the inspector thinking: the window dimmed, Mara, the bar, the message ----
namespace {
class ThinkWidget : public QWidget {
public:
  ThinkWidget() : QWidget(ownerWin(), Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus) {
    setAttribute(Qt::WA_ShowWithoutActivating); setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_TransparentForMouseEvents);
    anim_.setInterval(140); QObject::connect(&anim_, &QTimer::timeout, this, [this] { update(); }); }   // (Mara at her board, while the program can paint)
  QString msg; double p = 0; int depth = 0; bool set = false; qint64 t0 = 0;   // (set: p given — otherwise it creeps on, no share shown)
protected:
  void showEvent(QShowEvent*) override { anim_.start(); }
  void hideEvent(QHideEvent*) override { anim_.stop(); }
  void paintEvent(QPaintEvent*) override {
    QPainter g(this); g.setRenderHint(QPainter::Antialiasing); g.setRenderHint(QPainter::SmoothPixmapTransform);
    g.fillRect(rect(), QColor(0, 0, 0, 70));                                   // (dimmed a little)
    const QRect pnl(width() / 2 - 200, height() / 2 - 92, 400, 184);   // (Mara left; the title, the message (three lines at most), the curl under them — apart)
    g.setPen(QPen(QColor(0x55, 0x55, 0x5c), 1)); g.setBrush(QColor(0x26, 0x26, 0x28, 245)); g.drawRoundedRect(pnl, 10, 10);
    static const std::vector<QPixmap> frames = [] { std::vector<QPixmap> v; for (int i = 1; i <= 5; i++) v.push_back(ui::image(QString("maranspector_%1").arg(i))); return v; }();
    const QPixmap& mara = frames[size_t((QDateTime::currentMSecsSinceEpoch() / 140) % 5)];
    const QRect mr(pnl.left() + 14, pnl.top() + (pnl.height() - 132) / 2, 132, 132); if (!mara.isNull()) g.drawPixmap(mr, mara);
    const QRect tx(mr.right() + 16, pnl.top() + 18, pnl.right() - mr.right() - 30, 22);
    QFont f = font(); f.setPixelSize(16); f.setBold(true); g.setFont(f); g.setPen(QColor(0xe8, 0xe8, 0xec)); g.drawText(tx, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, QObject::tr("Let me think…"));
    f.setPixelSize(11); f.setBold(false); g.setFont(f); g.setPen(QColor(0x9a, 0x9a, 0xa4));
    const QRect mt(tx.left(), tx.bottom() + 6, tx.width(), 46); g.drawText(mt, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, msg);
    // (the curl filling up as it goes, the share under it)
    const double cw = std::min<double>(tx.width(), (pnl.bottom() - 18 - (mt.bottom() + 10)) * 640.0 / 200), ch = cw * 200 / 640; const QRectF curl(tx.left(), mt.bottom() + 10, cw, ch);
    drawCurl(g, curl, set ? p : 0.9 * (1 - std::exp(-(QDateTime::currentMSecsSinceEpoch() - t0) / 4000.0)));
    if (set) { f.setPixelSize(10); g.setFont(f); g.setPen(QColor(0x8f, 0xd6, 0xf2)); g.drawText(QRectF(tx.left(), pnl.bottom() - 18, tx.width(), 14), Qt::AlignRight | Qt::AlignVCenter, QString("%1%").arg(int(std::lround(std::clamp(p, 0.0, 1.0) * 100)))); }
  }
private:
  QTimer anim_;
};
ThinkWidget* thinkW() { static QPointer<ThinkWidget> w; if (!w) w = new ThinkWidget; return w; }
void thinkShow(const QString& msg, double p, bool set) {
  ThinkWidget* w = thinkW(); if (!w->isVisible()) w->t0 = QDateTime::currentMSecsSinceEpoch(); w->msg = msg; w->p = p; w->set = set;
  if (QWidget* o = ownerWin()) w->setGeometry(QRect(o->mapToGlobal(QPoint(0, 0)), o->size()));
  w->show(); w->raise(); w->repaint();   // (painted at once — no events taken: it may be shown from inside a frame)
}
}  // namespace

// (a computation: the waiting window, its message under «Let me think…», filling as it goes)
void thinkBegin(const QString& msg) { thinkW()->depth++; thinkShow(msg, 0, false); QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); }
void thinkEnd() { ThinkWidget* w = thinkW(); if (--w->depth <= 0) { w->depth = 0; w->hide(); } }
ThinkScope::ThinkScope(const QString& msg, int delayMs) : t0_(QDateTime::currentMSecsSinceEpoch()), delay_(delayMs), msg_(msg) { thinkW()->depth++; }
ThinkScope::~ThinkScope() { thinkEnd(); }
void ThinkScope::now() { shown_ = true; thinkShow(msg_, 0, false); QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); }
void ThinkScope::progress(double p) {
  if (!shown_ && QDateTime::currentMSecsSinceEpoch() - t0_ < delay_) return;
  static qint64 last = 0; const qint64 t = QDateTime::currentMSecsSinceEpoch(); if (shown_ && t - last < 60) return; last = t;   // (painted a few times a second — not the work's speed)
  shown_ = true; thinkShow(msg_, p, true);
}
void ThinkScope::progress(double p, const QString& stage) { if (!stage.isEmpty()) msg_ = stage; progress(p); }

static QString lastToast_;
QString lastToast() { return lastToast_; }
std::function<void(const QString&)>& toastHook() { static std::function<void(const QString&)> h; return h; }
std::function<void(const QString&)>& stepHook() { static std::function<void(const QString&)> h; return h; }
void toast(const QString& msg) { lastToast_ = msg; if (toastHook()) toastHook()(msg); if (stepHook()) stepHook()(msg);
  QLabel* l = toastLbl(); l->setText(msg); l->adjustSize();
  if (toastHost_) {
    QRect g(toastHost_->mapToGlobal(QPoint(0, 0)), toastHost_->size());
    l->move(g.center().x() - l->width() / 2, g.bottom() - 40 - l->height());
  }
  l->show(); l->raise();
  static QTimer* t = nullptr;
  if (!t) { t = new QTimer; t->setSingleShot(true); QObject::connect(t, &QTimer::timeout, [] { toastLbl()->hide(); }); }
  t->start(std::max(2200, int(msg.size()) * 35));
}

void hud(const QString& msg) {
  QLabel* l = hudLbl();
  if (msg.isEmpty()) { l->hide(); return; }
  l->setText(msg); l->adjustSize();
  if (hudHost_) { QRect g(hudHost_->mapToGlobal(QPoint(0, 0)), hudHost_->size()); l->move(g.center().x() - l->width() / 2, g.top() + 10); }
  l->show(); l->raise();
}
void hud(const QString& msg, int ms) {
  hud(msg);
  QTimer::singleShot(ms, hudLbl(), [msg] { if (hudLbl()->text() == msg) hudLbl()->hide(); });
}

}  // namespace hm::ui

namespace hm::ui {
QImage thinkSample(const QString& msg, double p, const QSize& size) {
  ThinkWidget w; w.msg = msg; w.p = p; w.resize(size); QImage img(size, QImage::Format_ARGB32_Premultiplied); img.fill(QColor(0x30, 0x30, 0x34)); w.render(&img); return img;
}
}  // namespace hm::ui
