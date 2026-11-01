#include "app/KeyCastWindow.h"
#include "app/Overlays.h"
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>

namespace hm {

namespace {
const QColor kOrange(255, 138, 40);
constexpr int kHist = 5;                 // (the combinations kept above the big row)
constexpr qint64 kDimMs = 2500;          // (nothing pressed this long: the big row dims)
bool isMod(int k) { return k == Qt::Key_Control || k == Qt::Key_Shift || k == Qt::Key_Alt || k == Qt::Key_Meta || k == Qt::Key_AltGr; }
}

KeyCastWindow::KeyCastWindow(QWidget* mainWindow)
    : QWidget(mainWindow, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput), main_(mainWindow) {   // (never caught by the mouse: placed in the Inspector only)
  setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setFocusPolicy(Qt::NoFocus);
  setAttribute(Qt::WA_TransparentForMouseEvents);
  tick_ = new QTimer(this); tick_->setInterval(100); connect(tick_, &QTimer::timeout, this, [this] { if (idle_.isValid() && idle_.elapsed() < kDimMs + 400) update(); });
  scale_ = std::clamp(QSettings().value("keycast/scale", 1.0).toDouble(), 0.5, 2.5); bgOp_ = std::clamp(QSettings().value("keycast/bgOpacity", 0.88).toDouble(), 0.0, 1.0);
  idle_.start(); fit();
}

KeyCastWindow::~KeyCastWindow() { qApp->removeEventFilter(this); }

void KeyCastWindow::open() {
  qApp->removeEventFilter(this); qApp->installEventFilter(this); tick_->start();
  place(); show(); raise();
}
// at the share of the viewport's free room set in the Inspector — else its bottom left
void KeyCastWindow::place() {
  QWidget* h = host_ ? host_ : main_; const QRect g(h->mapToGlobal(QPoint(0, 0)), h->size());
  const QPointF f = overlays::cfg().keyPos;
  const QPoint p = f.x() < 0 ? QPoint(g.left() + 20, g.bottom() - height() - 60)
                             : QPoint(g.left() + int(f.x() * std::max(0, g.width() - width())), g.top() + int(f.y() * std::max(0, g.height() - height())));
  move(p);
}

// ---- what is pressed ----
bool KeyCastWindow::eventFilter(QObject* o, QEvent* e) {
  const QEvent::Type t = e->type();
  if ((o == main_ || o == host_) && (t == QEvent::Move || t == QEvent::Resize) && isVisible()) { place(); return false; }   // (the program moved: it goes along)
  if (t != QEvent::KeyPress && t != QEvent::KeyRelease && t != QEvent::MouseButtonPress && t != QEvent::MouseButtonRelease && t != QEvent::MouseButtonDblClick && t != QEvent::Wheel) return false;
  if (o == this || o == windowHandle()) return false;               // (its own: dragged)
  auto* ie = static_cast<QInputEvent*>(e);
  int code = 0;
  if (t == QEvent::KeyPress || t == QEvent::KeyRelease) code = static_cast<QKeyEvent*>(e)->key();
  else if (t == QEvent::Wheel) code = static_cast<QWheelEvent*>(e)->angleDelta().y() > 0 ? 1 : -1;
  else code = int(static_cast<QMouseEvent*>(e)->button());
  // (the same press reaches the filter once per receiver — the window, the widget, its parents: seen once)
  const int tt = t == QEvent::MouseButtonDblClick ? int(QEvent::MouseButtonPress) : int(t);
  if (ie->timestamp() != 0 && ie->timestamp() == lastStamp_ && tt == lastType_ && code == lastCode_) return false;
  lastStamp_ = ie->timestamp(); lastType_ = tt; lastCode_ = code;
  if (t == QEvent::KeyPress || t == QEvent::KeyRelease) {
    auto* k = static_cast<QKeyEvent*>(e); if (k->isAutoRepeat()) return false;
    onKey(k->key(), k->nativeVirtualKey(), k->modifiers(), t == QEvent::KeyPress);
  } else if (t == QEvent::Wheel) {
    auto* w = static_cast<QWheelEvent*>(e); const int dy = w->angleDelta().y(); if (dy == 0) return false;
    onMouse(dy > 0 ? WUp : WDown, w->modifiers(), true);
  } else {
    auto* m = static_cast<QMouseEvent*>(e);
    const int bit = m->button() == Qt::LeftButton ? L : m->button() == Qt::RightButton ? R : m->button() == Qt::MiddleButton ? M
                  : m->button() == Qt::BackButton ? X1 : m->button() == Qt::ForwardButton ? X2 : 0;
    if (bit) onMouse(bit, m->modifiers(), t != QEvent::MouseButtonRelease);
  }
  return false;
}

void KeyCastWindow::testKey(int key, int mods, bool press) { onKey(key, 0, Qt::KeyboardModifiers(mods), press); }
void KeyCastWindow::testMouse(int button, int mods, bool press, int wheel) {
  if (wheel) { onMouse(wheel > 0 ? WUp : WDown, Qt::KeyboardModifiers(mods), true); return; }
  onMouse(button == 2 ? R : button == 3 ? M : L, Qt::KeyboardModifiers(mods), press);
}

KeyCastWindow::Chord KeyCastWindow::modTokens(Qt::KeyboardModifiers m) const {
  Chord c;
  if (m & Qt::ControlModifier) c.push_back({"Ctrl"});
  if (m & Qt::ShiftModifier) c.push_back({"Shift"});
  if (m & Qt::AltModifier) c.push_back({"Alt"});
  if (m & Qt::MetaModifier) c.push_back({"Win"});
  return c;
}

QString KeyCastWindow::keyName(int key, quint32 vk) {
  // (letters and digits by the key's place — the same in any layout, as the hotkeys are)
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return QString(QChar(char(vk)));
  switch (key) {
    case Qt::Key_Space: return "Space"; case Qt::Key_Escape: return "Esc"; case Qt::Key_Return: case Qt::Key_Enter: return "Enter";
    case Qt::Key_Tab: case Qt::Key_Backtab: return "Tab"; case Qt::Key_Backspace: return "Backspace"; case Qt::Key_Delete: return "Del";
    case Qt::Key_Insert: return "Ins"; case Qt::Key_Home: return "Home"; case Qt::Key_End: return "End";
    case Qt::Key_PageUp: return "PgUp"; case Qt::Key_PageDown: return "PgDn"; case Qt::Key_CapsLock: return "Caps";
    case Qt::Key_Left: return QString::fromUtf8("←"); case Qt::Key_Right: return QString::fromUtf8("→");
    case Qt::Key_Up: return QString::fromUtf8("↑"); case Qt::Key_Down: return QString::fromUtf8("↓");
    case Qt::Key_Control: return "Ctrl"; case Qt::Key_Shift: return "Shift"; case Qt::Key_Alt: case Qt::Key_AltGr: return "Alt"; case Qt::Key_Meta: return "Win";
    default: break;
  }
  QString s = QKeySequence(key).toString(QKeySequence::NativeText);
  return s.isEmpty() ? QString("?") : s.toUpper();
}

void KeyCastWindow::onKey(int key, quint32 vk, Qt::KeyboardModifiers mods, bool press) {
  idle_.restart();
  if (!press) {                                                  // (modifiers let go with nothing else pressed: the combination before shown again)
    if (modsOnly_ && !held_) { cur_ = modTokens(mods & ~(key == Qt::Key_Control ? Qt::ControlModifier : key == Qt::Key_Shift ? Qt::ShiftModifier : key == Qt::Key_Alt ? Qt::AltModifier : key == Qt::Key_Meta ? Qt::MetaModifier : Qt::NoModifier));
      if (cur_.empty()) { cur_ = before_; count_ = beforeCount_; modsOnly_ = false; } fit(); }
    update(); return; }
  if (isMod(key)) {                                              // (only modifiers yet: shown, not a combination)
    Qt::KeyboardModifiers m = mods | (key == Qt::Key_Control ? Qt::ControlModifier : key == Qt::Key_Shift ? Qt::ShiftModifier : key == Qt::Key_Meta ? Qt::MetaModifier : Qt::AltModifier);
    if (!modsOnly_) { before_ = cur_; beforeCount_ = count_; }
    cur_ = modTokens(m); modsOnly_ = true; fit(); update(); return;
  }
  Chord c = modTokens(mods); c.push_back({keyName(key, vk)}); commit(c);
}

void KeyCastWindow::onMouse(int bit, Qt::KeyboardModifiers mods, bool press) {
  idle_.restart();
  if (bit == WUp || bit == WDown) { Chord c = modTokens(mods); c.push_back({{}, bit}); commit(c); return; }
  if (!press) { held_ &= ~bit; update(); return; }
  if (held_ && !modsOnly_ && !cur_.empty() && cur_.back().mouse && !(cur_.back().mouse & (WUp | WDown))) {   // (another button while one is held: lit beside it)
    held_ |= bit; cur_.back().mouse |= bit; fit(); update(); return; }
  held_ |= bit;
  Chord c = modTokens(mods); c.push_back({{}, bit}); commit(c);
}

void KeyCastWindow::commit(const Chord& c) {
  if (modsOnly_) { cur_ = before_; count_ = beforeCount_; }       // (the modifiers were the start of this one)
  if (c == cur_) count_++;
  else { if (!cur_.empty()) { hist_.push_back(cur_); if (int(hist_.size()) > kHist) hist_.erase(hist_.begin()); } cur_ = c; count_ = 1; }
  modsOnly_ = false; fit(); update();
}

// ---- the look ----
// (s: the scale — 1 the big row, 0.55 the ones above it)
int KeyCastWindow::chordWidth(const Chord& c, double s) const {
  QFont f = font(); f.setPixelSize(int(std::lround(19 * s))); f.setWeight(QFont::DemiBold); const QFontMetrics fm(f);
  int w = 0;
  for (size_t i = 0; i < c.size(); i++) {
    if (i) w += int(std::lround(22 * s));
    w += c[i].mouse ? int(std::lround(34 * s)) : std::max(int(std::lround(42 * s)), fm.horizontalAdvance(c[i].text) + int(std::lround(24 * s)));
  }
  return w;
}

// a mouse at (x, y) — `bits` lit: its buttons, the wheel (pressed / rolled up or down), the side buttons
static void drawMouse(QPainter& p, double x, double y, double s, int bits, double a) {
  const double w = 34 * s, h = 48 * s, r = 16 * s, split = 20 * s;
  QPainterPath body; body.addRoundedRect(QRectF(x, y, w, h), r, r);
  QColor line(230, 232, 240); line.setAlphaF(a); QColor fill(42, 47, 61); fill.setAlphaF(a); QColor lit = kOrange; lit.setAlphaF(a);
  p.setPen(Qt::NoPen); p.setBrush(fill); p.drawPath(body);
  auto litPart = [&](const QRectF& clip) { p.save(); p.setClipRect(clip); p.setBrush(lit); p.drawPath(body); p.restore(); };
  if (bits & 1) litPart(QRectF(x, y, w / 2, split));             // L
  if (bits & 2) litPart(QRectF(x + w / 2, y, w / 2, split));     // R
  QPen pen(line, std::max(1.5, 2.6 * s)); pen.setJoinStyle(Qt::RoundJoin); pen.setCapStyle(Qt::RoundCap); p.setPen(pen); p.setBrush(Qt::NoBrush);
  p.drawPath(body); p.drawLine(QPointF(x, y + split), QPointF(x + w, y + split)); p.drawLine(QPointF(x + w / 2, y), QPointF(x + w / 2, y + split));
  // the wheel: lit when pressed (the middle button) or rolled; the arrow — which way
  const QRectF wh(x + w / 2 - 3.6 * s, y + 4 * s, 7.2 * s, 12 * s);
  const bool wheel = bits & (4 | 8 | 16);
  p.setBrush(wheel ? lit : fill); p.drawRoundedRect(wh, 3.6 * s, 3.6 * s);
  if (bits & (8 | 16)) {
    const bool up = bits & 8; const double cx = x + w / 2, ay = up ? y - 7 * s : y + split + 9 * s, d = up ? -1 : 1;
    QPainterPath ar; ar.moveTo(cx, ay + d * 6 * s); ar.lineTo(cx - 6 * s, ay - d * 1 * s); ar.lineTo(cx + 6 * s, ay - d * 1 * s); ar.closeSubpath();
    p.setPen(Qt::NoPen); p.setBrush(lit); p.drawPath(ar); p.setPen(pen);
  }
  if (bits & (32 | 64)) { p.setPen(Qt::NoPen); p.setBrush(lit); p.drawRoundedRect(QRectF(x - 4 * s, y + (bits & 64 ? 22 : 30) * s, 5 * s, 7 * s), 2 * s, 2 * s); }
}

int KeyCastWindow::drawChord(QPainter& p, const Chord& c, int x, int y, double s, double a) const {
  QFont f = font(); f.setPixelSize(int(std::lround(19 * s))); f.setWeight(QFont::DemiBold); p.setFont(f); const QFontMetrics fm(f);
  const double capH = 40 * s;
  for (size_t i = 0; i < c.size(); i++) {
    if (i) { QColor pc = kOrange; pc.setAlphaF(a); p.setPen(pc); p.drawText(QRectF(x, y, 22 * s, capH), Qt::AlignCenter, "+"); x += int(std::lround(22 * s)); }
    if (c[i].mouse) { drawMouse(p, x, y + (capH - 48 * s) / 2, s, c[i].mouse, a); x += int(std::lround(34 * s)); continue; }
    const int w = std::max(int(std::lround(42 * s)), fm.horizontalAdvance(c[i].text) + int(std::lround(24 * s)));
    QColor sh(10, 12, 18); sh.setAlphaF(a); QColor cap(46, 52, 68); cap.setAlphaF(a); QColor bd(96, 108, 140); bd.setAlphaF(a); QColor tx(240, 242, 248); tx.setAlphaF(a);
    p.setPen(Qt::NoPen); p.setBrush(sh); p.drawRoundedRect(QRectF(x, y + 3 * s, w, capH), 7 * s, 7 * s);   // (its side: a key cap)
    p.setPen(QPen(bd, std::max(1.0, 1.4 * s))); p.setBrush(cap); p.drawRoundedRect(QRectF(x, y, w, capH - 1 * s), 7 * s, 7 * s);
    p.setPen(tx); p.drawText(QRectF(x, y, w, capH - 2 * s), Qt::AlignCenter, c[i].text);
    x += w;
  }
  return x;
}

void KeyCastWindow::fit() {
  const double k = scale_;
  int w = int(220 * k);
  if (!cur_.empty()) w = std::max(w, chordWidth(cur_, k) + int(32 * k) + (count_ > 1 ? int(54 * k) : 0));
  int hw = 0; for (const Chord& c : hist_) hw += chordWidth(c, 0.55 * k) + int(14 * k);
  w = std::max(w, std::min(hw + int(24 * k), int(640 * k)));
  const int h = int(std::lround(130 * k)), bottom = y() + height();
  setFixedSize(w, h);
  (void)bottom; if (isVisible()) place();   // (a new size: at its place again)
}

void KeyCastWindow::setLook(double scale, double bgOpacity) {
  scale_ = std::clamp(scale, 0.5, 2.5); bgOp_ = std::clamp(bgOpacity, 0.0, 1.0);
  QSettings().setValue("keycast/scale", scale_); QSettings().setValue("keycast/bgOpacity", bgOp_);
  fit(); update();
}

void KeyCastWindow::paintEvent(QPaintEvent*) {
  QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::TextAntialiasing);
  const double k = scale_, hs = 0.55 * k;
  // the background: its opacity set (0 — the keys alone over the program; the frame then only on hover)
  p.setPen(QPen(QColor(58, 69, 104, int(255 * std::min(1.0, hover_ ? std::max(0.6, bgOp_ * 1.5) : bgOp_ * 1.5))), 1)); p.setBrush(QColor(18, 21, 30, std::max(1, int(std::lround(255 * bgOp_)))));   // (never quite 0: still caught by the mouse, dragged)
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12 * k, 12 * k);
  // the ones before: small, the oldest faintest, the newest on the right
  { const int gap = int(14 * k), pad = int(12 * k); int total = 0; for (const Chord& c : hist_) total += chordWidth(c, hs) + gap;
    int x = std::max(pad, width() - pad - total);
    for (size_t i = 0; i < hist_.size(); i++) {
      if (x + chordWidth(hist_[i], hs) < pad) { x += chordWidth(hist_[i], hs) + gap; continue; }
      x = drawChord(p, hist_[i], x, int(14 * k), hs, 0.3 + 0.5 * double(i + 1) / hist_.size()) + gap; } }
  if (cur_.empty()) { p.setPen(QColor(150, 156, 176)); QFont f = font(); f.setPixelSize(std::max(8, int(13 * k))); p.setFont(f);
    p.drawText(QRectF(rect()).adjusted(0, int(30 * k), 0, 0), Qt::AlignCenter, tr("Press a key or a mouse button")); }
  else {
    const bool live = held_ || modsOnly_ || idle_.elapsed() < kDimMs;
    const double a = live ? 1.0 : std::max(0.4, 1.0 - double(idle_.elapsed() - kDimMs) / 400.0 * 0.6);
    const int y = int(62 * k), x = drawChord(p, cur_, int(16 * k), y, k, a);
    if (count_ > 1) { QColor c = kOrange; c.setAlphaF(a); p.setPen(c); QFont f = font(); f.setPixelSize(std::max(8, int(18 * k))); f.setWeight(QFont::Bold); p.setFont(f);
      p.drawText(QRectF(x + 10 * k, y, 50 * k, 40 * k), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("×%1").arg(count_)); }
  }
  if (hover_) { p.setPen(QColor(200, 205, 220)); QFont f = font(); f.setPixelSize(16); p.setFont(f); p.drawText(QRectF(width() - 26, 2, 22, 22), Qt::AlignCenter, QString::fromUtf8("×")); }
}

// ---- dragged; × closes ----
void KeyCastWindow::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  if (QRect(width() - 28, 0, 28, 26).contains(e->position().toPoint())) { hide(); qApp->removeEventFilter(this); tick_->stop(); emit closed(); return; }
  drag_ = true; grab_ = e->globalPosition().toPoint() - pos();
}
void KeyCastWindow::mouseMoveEvent(QMouseEvent* e) {
  if (drag_) move(e->globalPosition().toPoint() - grab_);
  setCursor(QRect(width() - 28, 0, 28, 26).contains(e->position().toPoint()) ? Qt::PointingHandCursor : Qt::SizeAllCursor);
}
void KeyCastWindow::mouseReleaseEvent(QMouseEvent*) {
  if (!drag_) return; drag_ = false;
  const QPoint d = pos() - main_->mapToGlobal(QPoint(0, 0));
  QSettings().setValue("keycast/pos", QJsonDocument(QJsonObject{{"dx", d.x()}, {"dy", d.y()}}).toJson(QJsonDocument::Compact));
}
void KeyCastWindow::enterEvent(QEnterEvent*) { hover_ = true; update(); }
void KeyCastWindow::leaveEvent(QEvent*) { hover_ = false; update(); }

}  // namespace hm
