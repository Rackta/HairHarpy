#include "app/ColorPicker.h"
#include <QConicalGradient>
#include <QImageReader>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QToolTip>
#include <algorithm>
#include <cmath>

namespace hm {

namespace {
constexpr double kPi = 3.14159265358979323846;
QColor hueColor(double h) { return QColor::fromHsvF(float(std::fmod(std::fmod(h, 360.0) + 360.0, 360.0) / 360.0), 1, 1); }
void marker(QPainter& p, const QPointF& c, double r = 5) { p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor(0, 0, 0, 200), 3)); p.drawEllipse(c, r, r); p.setPen(QPen(Qt::white, 1.5)); p.drawEllipse(c, r, r); }
void strip(QPainter& p, const QRectF& r, double at, bool vertical) {   // (the pointer on a strip / a bar: two little triangles)
  p.setPen(QPen(QColor(0, 0, 0, 220), 1)); p.setBrush(Qt::white);
  if (vertical) { const double y = r.top() + at * r.height(); p.drawPolygon(QPolygonF({QPointF(r.left() - 1, y), QPointF(r.left() - 6, y - 4), QPointF(r.left() - 6, y + 4)})); p.drawPolygon(QPolygonF({QPointF(r.right() + 1, y), QPointF(r.right() + 6, y - 4), QPointF(r.right() + 6, y + 4)})); }
  else { const double x = r.left() + at * r.width(); p.drawRect(QRectF(x - 2, r.top() - 2, 4, r.height() + 4)); }
}
}  // namespace

ColorPicker::ColorPicker(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(170); setCursor(Qt::CrossCursor);
  const QString f = QSettings().value("colorpicker/image").toString(); if (!f.isEmpty()) { picFile_ = f; QImageReader r(f); r.setAutoTransform(true); pic_ = r.read(); }
}
QStringList ColorPicker::modes() { return {"quad", "triangle", "round", "bars", "image"}; }
QString ColorPicker::modeName(const QString& k) {
  if (k == "triangle") return tr("Triangle"); if (k == "round") return tr("Round"); if (k == "bars") return tr("Bars"); if (k == "image") return tr("Image"); return tr("Quad");
}
void ColorPicker::setMode(const QString& k) { if (modes().contains(k) && k != mode_) { mode_ = k; cache_ = {}; update(); } }
void ColorPicker::setColor(const QColor& c0) {
  if (part_ >= 0) return;                         // (being dragged here: the change came from here — its own hue and saturation kept, not read back rounded)
  const QColor c = c0.toHsv(); float hh, ss, vv; c.getHsvF(&hh, &ss, &vv);
  const double nh = hh < 0 ? h_ : hh * 360.0;   // (a gray: its hue is none — the one shown stays)
  if (std::abs(nh - h_) < 1e-6 && std::abs(ss - s_) < 1e-6 && std::abs(vv - v_) < 1e-6) return;
  h_ = nh; s_ = ss; v_ = vv; update();
}
void ColorPicker::setHsv(double h, double s, double v) { h_ = std::clamp(h, 0.0, 360.0); s_ = std::clamp(s, 0.0, 1.0); v_ = std::clamp(v, 0.0, 1.0); update(); if (changed) changed(color()); }
void ColorPicker::loadImage(const QString& file) {
  QImageReader r(file); r.setAutoTransform(true); QImage im = r.read(); if (im.isNull()) return;
  if (std::max(im.width(), im.height()) > 1400) im = im.scaled(1400, 1400, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  pic_ = im.convertToFormat(QImage::Format_ARGB32); picFile_ = file; QSettings().setValue("colorpicker/image", file); update();
}

QRectF ColorPicker::mainRect() const {
  if (mode_ == "quad") return QRectF(1, 1, width() - 32, height() - 2);
  if (mode_ == "round") { const double s = std::min(width() - 34.0, height() - 2.0); return QRectF(1 + (width() - 34 - s) / 2, 1 + (height() - 2 - s) / 2, s, s); }
  const double s = std::min(width() - 2.0, height() - 2.0); return QRectF((width() - s) / 2, (height() - s) / 2, s, s);
}
QRectF ColorPicker::stripRect() const { return QRectF(width() - 22, 1, 16, height() - 2); }
QRectF ColorPicker::picRect() const {
  if (pic_.isNull()) return rect();
  const double k = std::min(double(width()) / pic_.width(), double(height()) / pic_.height()); const QSizeF s(pic_.width() * k, pic_.height() * k);
  return QRectF((width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height());
}

void ColorPicker::paintEvent(QPaintEvent*) {
  QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), QColor(0x26, 0x26, 0x26));
  const QRectF m = mainRect();
  if (mode_ == "quad") {
    const QString key = QString("q%1 %2x%3").arg(int(h_)).arg(int(m.width())).arg(int(m.height()));
    if (cache_.isNull() || cacheKey_ != key) {
      cache_ = QImage(m.size().toSize(), QImage::Format_ARGB32_Premultiplied); QPainter q(&cache_); q.fillRect(cache_.rect(), hueColor(h_));
      QLinearGradient w(0, 0, cache_.width(), 0); w.setColorAt(0, Qt::white); w.setColorAt(1, QColor(255, 255, 255, 0)); q.fillRect(cache_.rect(), w);
      QLinearGradient b(0, 0, 0, cache_.height()); b.setColorAt(0, QColor(0, 0, 0, 0)); b.setColorAt(1, Qt::black); q.fillRect(cache_.rect(), b); cacheKey_ = key; }
    p.drawImage(m.topLeft(), cache_); p.setPen(QColor(0x55, 0x55, 0x55)); p.setBrush(Qt::NoBrush); p.drawRect(m);
    const QRectF st = stripRect(); QLinearGradient g(st.topLeft(), st.bottomLeft()); for (int k = 0; k <= 6; k++) g.setColorAt(k / 6.0, hueColor(k * 60)); p.fillRect(st, g); p.drawRect(st);
    marker(p, QPointF(m.left() + s_ * m.width(), m.top() + (1 - v_) * m.height())); strip(p, st, h_ / 360.0, true);
  } else if (mode_ == "round") {
    const QString key = QString("r%1 %2").arg(int(v_ * 255)).arg(int(m.width()));
    if (cache_.isNull() || cacheKey_ != key) {
      const int S = int(m.width()); cache_ = QImage(S, S, QImage::Format_ARGB32_Premultiplied); cache_.fill(Qt::transparent); const double R = S / 2.0;
      for (int y = 0; y < S; y++) { QRgb* row = reinterpret_cast<QRgb*>(cache_.scanLine(y)); for (int x = 0; x < S; x++) { const double dx = x + 0.5 - R, dy = y + 0.5 - R, d = std::hypot(dx, dy) / R; if (d > 1.0) continue;
        double a = std::atan2(-dy, dx) * 180 / kPi; if (a < 0) a += 360; const double edge = std::clamp((1.0 - d) * R, 0.0, 1.0);
        const QColor c = QColor::fromHsvF(float(a / 360), float(d), float(v_)); row[x] = qPremultiply(qRgba(c.red(), c.green(), c.blue(), int(255 * edge))); } }
      cacheKey_ = key; }
    p.drawImage(m.topLeft(), cache_); p.setPen(QColor(0x55, 0x55, 0x55)); p.setBrush(Qt::NoBrush); p.drawEllipse(m);
    const QRectF st = stripRect(); QLinearGradient g(st.topLeft(), st.bottomLeft()); g.setColorAt(0, QColor::fromHsvF(float(h_ / 360), float(s_), 1)); g.setColorAt(1, Qt::black); p.fillRect(st, g); p.drawRect(st);
    const double a = h_ * kPi / 180, R = m.width() / 2; marker(p, m.center() + QPointF(std::cos(a), -std::sin(a)) * s_ * R); strip(p, st, 1 - v_, true);
  } else if (mode_ == "triangle") {
    const double R = m.width() / 2, rin = R - 16, ri = rin - 4; const QPointF c = m.center();
    QConicalGradient cg(c, 0); for (int k = 0; k <= 6; k++) cg.setColorAt(k / 6.0, hueColor(k * 60));
    p.setPen(QPen(QBrush(cg), 14)); p.setBrush(Qt::NoBrush); p.drawEllipse(c, R - 8, R - 8);
    const double a = h_ * kPi / 180; auto vtx = [&](double off) { return c + QPointF(std::cos(a + off), -std::sin(a + off)) * ri; };
    const QPointF H = vtx(0), W = vtx(2 * kPi / 3), K = vtx(4 * kPi / 3);
    const QString key = QString("t%1 %2").arg(int(h_)).arg(int(m.width()));
    const QRectF bb = QPolygonF({H, W, K}).boundingRect().adjusted(-1, -1, 1, 1);
    if (cache_.isNull() || cacheKey_ != key) {
      cache_ = QImage(bb.size().toSize() + QSize(1, 1), QImage::Format_ARGB32_Premultiplied); cache_.fill(Qt::transparent);
      const double den = (W.y() - K.y()) * (H.x() - K.x()) + (K.x() - W.x()) * (H.y() - K.y());
      for (int y = 0; y < cache_.height(); y++) { QRgb* row = reinterpret_cast<QRgb*>(cache_.scanLine(y)); for (int x = 0; x < cache_.width(); x++) {
        const double px = bb.left() + x + 0.5, py = bb.top() + y + 0.5;
        const double l1 = ((W.y() - K.y()) * (px - K.x()) + (K.x() - W.x()) * (py - K.y())) / den, l2 = ((K.y() - H.y()) * (px - K.x()) + (H.x() - K.x()) * (py - K.y())) / den, l3 = 1 - l1 - l2;
        if (l1 < -0.01 || l2 < -0.01 || l3 < -0.01) continue;
        const double vv = std::clamp(l1 + l2, 0.0, 1.0), ss = vv > 1e-6 ? std::clamp(l1 / vv, 0.0, 1.0) : 0; const QColor q = QColor::fromHsvF(float(h_ / 360), float(ss), float(vv)); row[x] = qRgb(q.red(), q.green(), q.blue()); } }
      cacheKey_ = key; }
    p.drawImage(bb.topLeft(), cache_);
    const double l1 = s_ * v_, l2 = v_ - l1, l3 = 1 - v_; marker(p, H * l1 + W * l2 + K * l3);
    marker(p, c + QPointF(std::cos(a), -std::sin(a)) * (R - 8), 6);
  } else if (mode_ == "bars") {
    const int n = 6; const double gap = 6, bh = (height() - gap * (n - 1)) / n; const QColor col = color();
    const char* lab[n] = {"H", "S", "V", "R", "G", "B"};
    for (int i = 0; i < n; i++) {
      const QRectF r(16, i * (bh + gap), width() - 22, bh); QLinearGradient g(r.topLeft(), r.topRight()); double at = 0;
      if (i == 0) { for (int k = 0; k <= 6; k++) g.setColorAt(k / 6.0, hueColor(k * 60)); at = h_ / 360; }
      else if (i == 1) { g.setColorAt(0, QColor::fromHsvF(float(h_ / 360), 0, float(v_))); g.setColorAt(1, QColor::fromHsvF(float(h_ / 360), 1, float(v_))); at = s_; }
      else if (i == 2) { g.setColorAt(0, Qt::black); g.setColorAt(1, QColor::fromHsvF(float(h_ / 360), float(s_), 1)); at = v_; }
      else { auto ch = [&](int v) { QColor q = col; if (i == 3) q.setRed(v); else if (i == 4) q.setGreen(v); else q.setBlue(v); return q; }; g.setColorAt(0, ch(0)); g.setColorAt(1, ch(255)); at = (i == 3 ? col.red() : i == 4 ? col.green() : col.blue()) / 255.0; }
      p.fillRect(r, g); p.setPen(QColor(0x55, 0x55, 0x55)); p.setBrush(Qt::NoBrush); p.drawRect(r);
      p.setPen(QColor(0xaa, 0xaa, 0xaa)); p.drawText(QRectF(0, r.top(), 14, r.height()), Qt::AlignCenter, lab[i]); strip(p, r, at, false);
    }
  } else {   // image
    if (pic_.isNull()) { p.setPen(QPen(QColor(0x55, 0x55, 0x55), 1, Qt::DashLine)); p.drawRect(rect().adjusted(1, 1, -2, -2)); p.setPen(QColor(0x8a, 0x8a, 0x8a)); p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, tr("Click — load a picture to take colours from\n(right click later — another one)")); }
    else { p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(picRect(), pic_); }
  }
}

void ColorPicker::pick(const QPointF& pt, bool press) {
  const QRectF m = mainRect();
  if (mode_ == "quad") {
    if (press) part_ = stripRect().adjusted(-6, 0, 6, 0).contains(pt) ? 1 : 0;
    if (part_ == 1) setHsv(std::clamp((pt.y() - m.top()) / m.height(), 0.0, 1.0) * 360, s_, v_);
    else setHsv(h_, std::clamp((pt.x() - m.left()) / m.width(), 0.0, 1.0), 1 - std::clamp((pt.y() - m.top()) / m.height(), 0.0, 1.0));
  } else if (mode_ == "round") {
    if (press) part_ = pt.x() > m.right() + 4 ? 1 : 0;
    if (part_ == 1) setHsv(h_, s_, 1 - std::clamp((pt.y() - 1) / (height() - 2.0), 0.0, 1.0));
    else { const QPointF d = pt - m.center(); double a = std::atan2(-d.y(), d.x()) * 180 / kPi; if (a < 0) a += 360; setHsv(a, std::min(1.0, std::hypot(d.x(), d.y()) / (m.width() / 2)), v_); }
  } else if (mode_ == "triangle") {
    const double R = m.width() / 2, rin = R - 16, ri = rin - 4; const QPointF c = m.center(), d = pt - c;
    if (press) part_ = std::hypot(d.x(), d.y()) > rin ? 1 : 0;
    if (part_ == 1) { double a = std::atan2(-d.y(), d.x()) * 180 / kPi; if (a < 0) a += 360; setHsv(a, s_, v_); return; }
    const double a = h_ * kPi / 180; auto vtx = [&](double off) { return c + QPointF(std::cos(a + off), -std::sin(a + off)) * ri; };
    const QPointF H = vtx(0), W = vtx(2 * kPi / 3), K = vtx(4 * kPi / 3);
    const double den = (W.y() - K.y()) * (H.x() - K.x()) + (K.x() - W.x()) * (H.y() - K.y());
    double l1 = ((W.y() - K.y()) * (pt.x() - K.x()) + (K.x() - W.x()) * (pt.y() - K.y())) / den, l2 = ((K.y() - H.y()) * (pt.x() - K.x()) + (H.x() - K.x()) * (pt.y() - K.y())) / den, l3 = 1 - l1 - l2;
    l1 = std::max(0.0, l1); l2 = std::max(0.0, l2); l3 = std::max(0.0, l3); const double sum = l1 + l2 + l3; l1 /= sum; l2 /= sum;   // (outside: onto its nearest side, near enough)
    const double vv = l1 + l2; setHsv(h_, vv > 1e-6 ? l1 / vv : 0, vv);
  } else if (mode_ == "bars") {
    const int n = 6; const double gap = 6, bh = (height() - gap * (n - 1)) / n;
    if (press) part_ = std::clamp(int(pt.y() / (bh + gap)), 0, n - 1);
    const double t = std::clamp((pt.x() - 16) / (width() - 22.0), 0.0, 1.0);
    if (part_ == 0) setHsv(t * 360, s_, v_); else if (part_ == 1) setHsv(h_, t, v_); else if (part_ == 2) setHsv(h_, s_, t);
    else { QColor q = color(); const int v = int(std::lround(t * 255)); if (part_ == 3) q.setRed(v); else if (part_ == 4) q.setGreen(v); else q.setBlue(v);
      float hh, ss, vv; q.getHsvF(&hh, &ss, &vv); setHsv(hh < 0 ? h_ : hh * 360, ss, vv); }
  } else {
    if (pic_.isNull()) return; part_ = 0; const QRectF r = picRect(); if (!r.contains(pt) && press) return;
    const int x = std::clamp(int((pt.x() - r.left()) / r.width() * pic_.width()), 0, pic_.width() - 1), y = std::clamp(int((pt.y() - r.top()) / r.height() * pic_.height()), 0, pic_.height() - 1);
    const QColor c = pic_.pixelColor(x, y); float hh, ss, vv; c.getHsvF(&hh, &ss, &vv); setHsv(hh < 0 ? h_ : hh * 360, ss, vv);
  }
}

void ColorPicker::mousePressEvent(QMouseEvent* e) {
  if (mode_ == "image" && (pic_.isNull() || e->button() == Qt::RightButton)) { if (onLoad) onLoad(); return; }
  if (e->button() != Qt::LeftButton) return;
  pick(e->position(), true);
}
void ColorPicker::mouseMoveEvent(QMouseEvent* e) { if (part_ >= 0 && (e->buttons() & Qt::LeftButton)) pick(e->position(), false); }
void ColorPicker::mouseReleaseEvent(QMouseEvent*) { if (part_ >= 0) { part_ = -1; if (committed) committed(color()); } }

// ---- the swatches ----
int SwatchGrid::heightForWidth(int w) const { const int n = std::max(1, (w + kG) / (kS + kG)); const int rows = std::max(1, (count() + n - 1) / n); return rows * (kS + kG); }
int SwatchGrid::at(const QPoint& p) const { for (int i = 0; i < count(); i++) if (cell(i).contains(p)) return i; return -1; }
void SwatchGrid::paintEvent(QPaintEvent*) {
  QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
  for (int i = 0; i < int(c_.size()); i++) { const QRect r = cell(i); p.setPen(QPen(i == hover_ ? QColor(255, 255, 255) : QColor(0x55, 0x55, 0x55), i == hover_ ? 1.5 : 1)); p.setBrush(c_[size_t(i)]); p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3); }
  if (plus) { const QRect r = cell(int(c_.size())); p.setPen(QPen(int(c_.size()) == hover_ ? QColor(255, 255, 255) : QColor(0x77, 0x77, 0x77), 1, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    p.setPen(QPen(QColor(0x94, 0x66, 0xbe), 2)); p.drawLine(r.center() + QPoint(-5, 0), r.center() + QPoint(5, 0)); p.drawLine(r.center() + QPoint(0, -5), r.center() + QPoint(0, 5)); }
  if (c_.empty() && !plus) { p.setPen(QColor(0x8a, 0x8a, 0x8a)); p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, tr("none yet")); }
}
void SwatchGrid::mousePressEvent(QMouseEvent* e) {
  const int i = at(e->pos()); if (i < 0) return;
  if (i == int(c_.size())) { if (plus && added) added(); return; }
  if (e->button() == Qt::RightButton) { if (removed) removed(i); return; }
  if (picked) picked(c_[size_t(i)]);
}
void SwatchGrid::mouseMoveEvent(QMouseEvent* e) {
  const int i = at(e->pos()); if (i != hover_) { hover_ = i; update();
    if (i >= 0 && i < int(c_.size())) QToolTip::showText(e->globalPosition().toPoint(), c_[size_t(i)].name().toUpper() + (removed ? tr("  ·  right click — remove") : QString()), this);
    else if (i >= 0) QToolTip::showText(e->globalPosition().toPoint(), tr("Keep the current colour"), this); }
}

}  // namespace hm
