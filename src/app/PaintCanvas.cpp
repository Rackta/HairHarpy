#include "ui/Keymap.h"
#include "app/PaintCanvas.h"
#include "view/Viewport.h"
#include "app/PaintSession.h"
#include "ui/Pie.h"
#include "paint/Region.h"
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QTabletEvent>
#include <QTimer>
#include <QPointer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>

namespace hm {

using namespace paint;

PaintCanvas::PaintCanvas(PaintSession* s, QWidget* parent) : QWidget(parent), s_(s) {
  setMouseTracking(true); setFocusPolicy(Qt::StrongFocus); setMinimumSize(240, 200); setAttribute(Qt::WA_TabletTracking);
  setCursor(Qt::BlankCursor);
  // (the whole map made anew: once per turn of the event loop, however many signals asked for it — a layer added says so three
  // times over, and on an 8K set each is a second of work; a part — a brush's dab — at once)
  auto whole = [this] { if (wholePending_) return; wholePending_ = true; QTimer::singleShot(0, this, [this] { wholePending_ = false; rebuild(); }); };
  connect(s, &PaintSession::pixelsChanged, this, [this, whole](const QRect& r) { if (r.isNull()) { normalDirty_ = true; whole(); return; } if (s_->view == "normalBump" || s_->view.startsWith("bake:")) return; if (!wholePending_) rebuild(r); });
  connect(s, &PaintSession::layersChanged, this, [this, whole] { normalDirty_ = true; whole(); });
  connect(s, &PaintSession::bakeChanged, this, [this, whole] { normalDirty_ = true; whole(); });
  connect(s, &PaintSession::viewChanged, this, [this, whole] { normalDirty_ = true; fitted_ = false;
    // (another UV set shown: its picture made at once, not at the next turn of the event loop — what is drawn on the model meanwhile
    // must not be the set that was shown before)
    if (s_->doc.activeSet() != rotSet_) { rotSet_ = s_->doc.activeSet(); rot_ = rotRaw_ = rotBySet_.value(rotSet_, 0.0); rebuild(); return; }   // (each UV set: its own turn)
    whole(); });
  connect(s, &PaintSession::brushChanged, this, [this] {
    if (lastSize_ >= 0 && std::abs(s_->brush.size - lastSize_) > 1e-3 && ring && isVisible()) {   // (a new size, from a key, a drag or a panel's slider: the ring shown at it for a moment)
      flash_ = true; if (!flashT_) { flashT_ = new QTimer(this); flashT_->setSingleShot(true); connect(flashT_, &QTimer::timeout, this, [this] { flash_ = false; update(); }); } flashT_->start(900); }
    lastSize_ = s_->brush.size; update(); });
  lastSize_ = s_->brush.size;
}

// where the brush's ring is drawn: at the pointer; the pointer away (a panel's slider being dragged) — the middle of what is seen
QPointF PaintCanvas::ringAt() const {
  if (hover_ || sizing_) return hoverAt_;
  return viewCentre ? viewCentre() : QPointF(width() / 2.0, height() / 2.0);
}

// the map, the freeze mask over it: frozen texels under light blue stripes
QImage PaintCanvas::render(const QRect& r) {
  QImage o = renderMap(r); if (o.isNull() || !s_->freezeShown || s_->view.startsWith("bake:")) return o;
  const QImage* fz = s_->freezeOf(false); if (!fz) return o;
  const QRect rr = r.isNull() ? QRect(0, 0, o.width(), o.height()) : r; if (rr.x() < 0 || rr.y() < 0 || fz->width() < rr.x() + o.width() || fz->height() < rr.y() + o.height() || o.format() != QImage::Format_ARGB32_Premultiplied) return o;
  // (the frozen part: a light blue over the map with fine stripes in it, both as strong as the mask is there — a soft mask fades out
  //  evenly, a hard one ends sharply: its edge is seen as it is, no outline drawn that a soft edge has not)
  for (int y = 0; y < o.height(); y++) { const int gy = rr.y() + y; const uchar* m = fz->constScanLine(gy) + rr.x(); QRgb* q = reinterpret_cast<QRgb*>(o.scanLine(y));
    for (int x = 0; x < o.width(); x++) { const int f = m[x]; if (!f) continue; const bool st = (((rr.x() + x) + gy) / 6) & 1; const int k = f * (st ? 185 : 135) / 255, ik = 255 - k;
      q[x] = qRgba((qRed(q[x]) * ik + 90 * k) / 255, (qGreen(q[x]) * ik + 175 * k) / 255, (qBlue(q[x]) * ik + 255 * k) / 255, (qAlpha(q[x]) * ik + 255 * k) / 255); } }
  return o;
}
QImage PaintCanvas::renderMap(const QRect& r) {
  const Document& d = s_->doc; const QString v = s_->view;
  if (v.startsWith("bake:")) {
    const QImage& m = s_->baked.value(v.mid(5)); if (m.isNull()) return {};
    QImage c = m.copy(r.isNull() ? m.rect() : r); return c.format() == QImage::Format_Grayscale16 ? c.convertToFormat(QImage::Format_Grayscale8).convertToFormat(QImage::Format_ARGB32_Premultiplied) : c.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  }
  if (v == "normalBump") {                                      // (the Normal mode's result: the bake through its mask + the painted bump)
    if (normalDirty_ || normalCache_.isNull()) { normalCache_ = s_->normalResult(); normalDirty_ = false; }
    if (normalCache_.isNull()) return {};
    return normalCache_.copy(r.isNull() ? normalCache_.rect() : r).convertToFormat(QImage::Format_ARGB32_Premultiplied);
  }
  if (s_->showMask) if (const Layer* l = s_->active(); l && !l->mask.isNull()) {   // (the mask itself: white shows the layer, black hides it)
    const QImage& m = l->mask; return (r.isNull() ? m : m.copy(r)).convertToFormat(QImage::Format_ARGB32_Premultiplied); }
  const Channel ch = s_->modeChannel(); const MapInfo& mi = mapInfo(ch);
  QImage c = d.compose(ch, r);
  if (ch == Channel::Color) return c.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  if (ch == Channel::Height) return c.convertToFormat(QImage::Format_Grayscale8).convertToFormat(QImage::Format_ARGB32_Premultiplied);
  if (mi.storage == Storage::Colour) {                          // (a map with a ground of its own: shown over it — a mask reads as the shader will read it)
    QImage o(c.size(), QImage::Format_ARGB32_Premultiplied); const int gr = qRed(mi.ground), gg = qGreen(mi.ground), gb = qBlue(mi.ground), ga = mi.gray ? 255 : qAlpha(mi.ground);
    for (int y = 0; y < c.height(); y++) { const QRgb* p = reinterpret_cast<const QRgb*>(c.constScanLine(y)); QRgb* q = reinterpret_cast<QRgb*>(o.scanLine(y));
      for (int x = 0; x < c.width(); x++) { const int a = qAlpha(p[x]), ia = 255 - a; int rr = (qRed(p[x]) * a + gr * ga / 255 * ia) / 255, g2 = (qGreen(p[x]) * a + gg * ga / 255 * ia) / 255, bb = (qBlue(p[x]) * a + gb * ga / 255 * ia) / 255;
        if (mi.gray) rr = g2 = bb = (rr * 299 + g2 * 587 + bb * 114) / 1000;
        q[x] = qRgba(rr, g2, bb, a + ga * ia / 255); } }
    return o;
  }
  QImage o(c.size(), QImage::Format_ARGB32_Premultiplied);   // (flow: the direction as a colour — x red, y green — where it's set, the dark ground elsewhere)
  for (int y = 0; y < c.height(); y++) { const uchar* p = c.constScanLine(y); QRgb* q = reinterpret_cast<QRgb*>(o.scanLine(y));
    for (int x = 0; x < c.width(); x++) { const float a = p[x * 4 + 3] / 255.f; q[x] = qRgb(int(p[x * 4] * a + 24 * (1 - a)), int(p[x * 4 + 1] * a + 24 * (1 - a)), int(128 * a + 24 * (1 - a))); } }
  return o;
}

void PaintCanvas::setMapping(const QTransform& t) { if (mapped_ && t == xf_) return; bool ok = false; const QTransform i = t.inverted(&ok); if (!ok) return; xf_ = t; xfi_ = i; mapped_ = true; update(); }
void PaintCanvas::panBy(const QPointF& d) { if (navPan) navPan(d); else { pan_ += d; autoFit_ = false; update(); } }
// (the brush ring moved: only where it was and is — a window over the whole viewport is not repainted for every mouse move)
void PaintCanvas::hoverUpdate(const QPointF& was) {
  if (!ring) return;
  if (mirror && mirror(toTex(hoverAt_))) { update(); return; }
  const int R = int(std::max(2.0, s_->brush.size * zoom() / 2)) + 6;
  update(QRect(was.toPoint() - QPoint(R, R), QSize(2 * R, 2 * R))); update(QRect(hoverAt_.toPoint() - QPoint(R, R), QSize(2 * R, 2 * R)));
}

void PaintCanvas::rebuild(const QRect& r) {
  if (bare) { update(); return; }
  const Document& d = s_->doc; if (d.width() == 0) { disp_ = {}; update(); return; }
  const bool whole = r.isNull() || disp_.size() != QSize(d.width(), d.height()) || s_->view == "normalBump" || s_->view.startsWith("bake:");
  QImage part = render(whole ? QRect() : r);
  shownDirty_ = whole ? QRect() : r.intersected(QRect(0, 0, d.width(), d.height()));
  if (part.isNull()) { disp_ = {}; shownDirty_ = {}; update(); if (shownChanged) shownChanged(); return; }
  if (whole) { const bool first = disp_.isNull(); disp_ = part; if (first || !fitted_) { fit(); fitted_ = width() > 200 && height() > 150; } }
  else { QPainter p(&disp_); p.setCompositionMode(QPainter::CompositionMode_Source); p.drawImage(r.topLeft(), part); }
  update(); if (shownChanged) shownChanged();
}

void PaintCanvas::strokeTex(int phase, const QPointF& uv, bool invert, float sizeScale, float pressure, bool pen) {
  texScale_ = sizeScale; if (stroke_) stroke_->sizeScale = sizeScale;
  const QPointF t(uv.x() * s_->doc.width(), uv.y() * s_->doc.height()), w = texToWidget(t);
  if (phase == 2) { end(); return; }
  if (phase == 3) { end(); return; }
  if (phase == 0) { penStroke_ = pen; tex3d_ = t; begin(w, pressure, invert, (QApplication::keyboardModifiers() & Qt::ShiftModifier) ? 1 : 0); texScale_ = 1; return; }   // (Shift on the model: the brush blurs)
  // (a move: across a UV seam the texture point jumps — the stroke lifts there and starts anew, no line drawn across the sheet)
  const double jump = std::hypot(t.x() - tex3d_.x(), t.y() - tex3d_.y()); tex3d_ = t;
  if (painting_ && jump > std::max(96.0, double(s_->brush.size) * sizeScale * 4)) end();   // (a brush sized on the screen: its size in the texture here)
  if (!painting_) { penStroke_ = pen; begin(w, pressure, invert); } else move(w, pressure);
  texScale_ = 1;
}

void PaintCanvas::fit() {
  if (navFit) { navFit(); return; }
  if (disp_.isNull()) return;
  const double r = std::abs(std::sin(rot_ * 0.0174533)) + std::abs(std::cos(rot_ * 0.0174533));   // (turned: the square's span grows by |sin|+|cos|)
  zoom_ = std::max(0.05, std::min((width() - 20.0) / (disp_.width() * r), (height() - 20.0) / (disp_.height() * r)));
  pan_ = QPointF(0, 0); const QPointF c = own().map(QPointF(disp_.width() / 2.0, disp_.height() / 2.0)); pan_ = QPointF(width() / 2.0, height() / 2.0) - c; autoFit_ = true; update();
}

void PaintCanvas::resizeEvent(QResizeEvent*) { if (!painting_ && !disp_.isNull() && (!fitted_ || autoFit_)) { fit(); fitted_ = width() > 200 && height() > 150; } }   // (the panel resized: a texture still at its fit fills it again — the author, 16.11)
void PaintCanvas::showEvent(QShowEvent*) { QTimer::singleShot(0, this, [this] { if (!disp_.isNull()) fit(); }); }

void PaintCanvas::paintEvent(QPaintEvent*) {
  QPainter p(this);
  if (bare) {                                                  // (see through it — but not click through: a window's fully clear pixels would let the mouse pass to the viewport)
    p.setCompositionMode(QPainter::CompositionMode_Source); p.fillRect(rect(), QColor(0, 0, 0, 1)); p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const QRectF tex(0, 0, s_->doc.width(), s_->doc.height()); const QPolygonF sheet = mapping().map(QPolygonF(tex));
    p.setPen(QPen(QColor(0, 0, 0, 170), 3)); p.setBrush(Qt::NoBrush); p.drawPolygon(sheet); p.setPen(QPen(QColor(110, 170, 255), 1, Qt::DashLine)); p.drawPolygon(sheet);
    if (drawOver) { p.save(); drawOver(p, sheet.boundingRect()); p.restore(); }
    if (ring && (!ringHere || ringHere(ringAt())) && (sizing_ || flash_ || (hover_ && !panning_ && !zooming_ && !tooling_ && !picking_ && !(QApplication::keyboardModifiers() == Qt::AltModifier)))) {   // (sizing — Alt+RMB / Ctrl+Alt drag: shown though Alt is held)
      const double r = std::max(2.0, s_->brush.size * zoom() / 2); const bool inv = bool((QApplication::keyboardModifiers() & Qt::ControlModifier) || eraserTip_) != eraser_; p.setRenderHint(QPainter::Antialiasing);
      const QColor rim = inv ? QColor(255, 110, 110, 230) : QColor(255, 255, 255, 220);
      auto rg = [&](const QPointF& c, bool dashed) { p.setPen(QPen(QColor(0, 0, 0, 160), 3)); p.drawEllipse(c, r, r); QPen pe(rim, 1); if (dashed) pe.setStyle(Qt::DashLine); p.setPen(pe); p.drawEllipse(c, r, r); };
      const QPointF at = ringAt(); rg(at, false); if (mirror) if (auto m = mirror(toTex(at))) rg(texToWidget(*m), true);
      if (painting_ && s_->brush.smoothing > 0.005) { p.setPen(QPen(QColor(148, 102, 190), 1.5)); p.drawLine(lazy_, hoverAt_); p.setBrush(QColor(148, 102, 190)); p.drawEllipse(lazy_, 3, 3); }   // (the string: the brush behind the pointer)
    }
    return;
  }
  p.fillRect(rect(), QColor(30, 30, 32));
  if (disp_.isNull()) { p.setPen(QColor(150, 150, 150)); p.drawText(rect(), Qt::AlignCenter, tr("Nothing to show: this map is not made yet")); return; }
  const QRectF texR(0, 0, disp_.width(), disp_.height()); const QRectF tr_ = own().mapRect(texR);
  p.save(); p.setTransform(own(), true);                       // (the texture as it lies on the canvas: moved, turned, scaled)
  if (checker_ && (s_->view == "color" || (!mapInfo(s_->modeChannel()).gray && mapInfo(s_->modeChannel()).storage == Storage::Colour))) { const double c = 12 / zoom_; p.save(); p.setClipRect(texR);
    const QRectF vis = own().inverted().mapRect(QRectF(rect())).intersected(texR);   // (only the cells on the screen: a zoomed-in view is a few thousand, not millions)
    const int x0 = std::max(0, int(std::floor(vis.left() / c))), x1 = int(std::ceil(vis.right() / c)), y0 = std::max(0, int(std::floor(vis.top() / c))), y1 = int(std::ceil(vis.bottom() / c));
    if (!vis.isEmpty()) for (int y = y0; y <= y1 && y * c < texR.height(); y++) for (int x = x0; x <= x1 && x * c < texR.width(); x++) p.fillRect(QRectF(x * c, y * c, c, c), ((x + y) & 1) ? QColor(70, 70, 70) : QColor(96, 96, 96)); p.restore(); }
  p.restore();
  if (drawUnder) { p.save(); drawUnder(p, tr_); p.restore(); }
  p.save(); p.setTransform(own(), true);
  p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1); p.drawImage(texR, disp_);
  if (const QImage& ov = overlay(); !ov.isNull()) { p.setRenderHint(QPainter::SmoothPixmapTransform, false); p.drawImage(texR, ov); }   // (the islands: the dark ground, the outlines)
  if (s_->ovWire && uvWire) if (const auto* polys = uvWire()) { p.setPen(QPen(QColor(255, 255, 255, 70), 0)); p.setBrush(Qt::NoBrush); for (const QPolygonF& q : *polys) p.drawPolygon(q); }   // (the polygons' wire)
  p.setPen(QPen(QColor(90, 90, 96), 0)); p.setBrush(Qt::NoBrush); p.drawRect(texR);
  p.restore();
  if (remote_) {                                               // (where the brush is on the model: a cyan ring here)
    const QPointF c = texToWidget(*remote_); const double r = std::max(3.0, remoteR_ * zoom_); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0, 0, 0, 160), 3)); p.drawEllipse(c, r, r); p.setPen(QPen(QColor(80, 220, 255), 1.5)); p.drawEllipse(c, r, r); p.drawLine(c - QPointF(4, 0), c + QPointF(4, 0)); p.drawLine(c - QPointF(0, 4), c + QPointF(0, 4)); }
  if (drawOver) { p.save(); drawOver(p, tr_); p.restore(); }
  if (sizing_ || flash_ || (hover_ && !panning_ && !(toolInput && tooling_))) {   // the brush's outline at its size (red: Ctrl held / the pen's eraser — it erases)
    const double r = std::max(2.0, s_->brush.size * zoom_ / 2); p.setRenderHint(QPainter::Antialiasing);
    const bool inv = (QApplication::keyboardModifiers() & Qt::ControlModifier) || eraserTip_;
    const QColor rim = inv ? QColor(255, 110, 110, 230) : QColor(255, 255, 255, 220);
    auto ring = [&](const QPointF& c, bool dashed) { p.setPen(QPen(QColor(0, 0, 0, 160), 3)); p.drawEllipse(c, r, r); QPen pe(rim, 1); if (dashed) pe.setStyle(Qt::DashLine); p.setPen(pe); p.drawEllipse(c, r, r); };
    const QPointF at = ringAt(); ring(at, false);
    if (mirror) if (auto m = mirror(toTex(at))) { const QPointF w = texToWidget(*m); if (std::hypot(w.x() - at.x(), w.y() - at.y()) > 1) ring(w, true); }   // (Symmetry: where the mirrored dab goes)
  }
  p.setPen(QColor(170, 170, 176)); p.setFont(QFont(font().family(), 8));
  p.drawText(QRect(8, height() - 22, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter, QString("%1×%2   %3%").arg(s_->doc.width()).arg(s_->doc.height()).arg(int(zoom_ * 100)));
}

void PaintCanvas::zoomAt(const QPointF& w, double f) {
  const QPointF t = toTex(w); zoom_ = std::clamp(zoom_ * f, 0.03, 40.0); pan_ += w - own().map(t); autoFit_ = false; update();
}
void PaintCanvas::setRotation(double deg) {
  const QPointF c(width() / 2.0, height() / 2.0); const QPointF t = toTex(c);
  rot_ = std::remainder(deg, 360.0); pan_ += c - own().map(t); rotBySet_[rotSet_] = rot_; autoFit_ = false; update();
  hud(tr("Texture turned %1°").arg(int(std::lround(rot_)))); QTimer::singleShot(700, this, [] { ui::hud({}); });
}
void PaintCanvas::rotateBy(double deg, int snap) {
  if (navRotate) { navRotate(deg, snap != 0); return; }       // (a canvas over the viewport: the view itself turns)
  rotRaw_ += deg; setRotation(rotRaw_);                         // (the texture: turned freely, no angle steps)
}
// the islands' overlay: the dark ground outside them and their outlines — at most 2048 across, drawn scaled over the texture
const QImage& PaintCanvas::overlay() {
  const paint::IslandMap* im = s_->islands ? s_->islands() : nullptr;
  if (!im || im->ids.isNull() || !(s_->ovDark || s_->ovEdge)) { ov_ = {}; ovKey_.clear(); return ov_; }
  const QString key = QString("%1|%2|%3|%4|%5").arg(im->ids.cacheKey()).arg(s_->ovDark).arg(s_->ovEdge).arg(s_->ovColor.rgba()).arg(im->angle.size());
  if (key == ovKey_) return ov_;
  ovKey_ = key; const int W = im->ids.width(), H = im->ids.height(), k = std::max(1, (std::max(W, H) + 2047) / 2048), ow = W / k, oh = H / k;
  ov_ = QImage(ow, oh, QImage::Format_ARGB32_Premultiplied); ov_.fill(Qt::transparent);
  std::vector<int> id(size_t(ow) * size_t(oh)); for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) id[size_t(y) * size_t(ow) + size_t(x)] = im->at(x * k + k / 2, y * k + k / 2);
  const QColor dark = s_->ovColor; const QRgb dk = qPremultiply(dark.rgba()), ed = qPremultiply(qRgba(110, 170, 255, 235));
  for (int y = 0; y < oh; y++) { QRgb* row = reinterpret_cast<QRgb*>(ov_.scanLine(y));
    for (int x = 0; x < ow; x++) { const int v = id[size_t(y) * size_t(ow) + size_t(x)];
      if (!v) { if (s_->ovDark) row[x] = dk; continue; }
      if (s_->ovEdge) { const int l = x > 0 ? id[size_t(y) * size_t(ow) + size_t(x - 1)] : 0, r = x + 1 < ow ? id[size_t(y) * size_t(ow) + size_t(x + 1)] : 0, u = y > 0 ? id[size_t(y - 1) * size_t(ow) + size_t(x)] : 0, d = y + 1 < oh ? id[size_t(y + 1) * size_t(ow) + size_t(x)] : 0;
        if (l != v || r != v || u != v || d != v) row[x] = ed; } } }
  return ov_;
}

void PaintCanvas::wheelEvent(QWheelEvent* e) { if (navZoom) { navZoom(e->position(), e->angleDelta().y() / 120.0); return; } zoomAt(e->position(), std::pow(1.0015, e->angleDelta().y())); }

void PaintCanvas::pick(const QPointF& w) {
  if (!pickInto(s_, toTex(w), mapToGlobal(w.toPoint())) && !picking_) ui::toast(tr("No paint here to take a colour from (outside the UV islands)"));
}

// ---- the colour taken: the loupe ----
bool pickInto(PaintSession* s, const QPointF& tex, const QPoint& global) {
  const auto c = s->pickColor(tex); if (!c) return false;
  s->color = *c; emit s->brushChanged(); showPickLoupe(s, tex, *c, global); return true;
}
void showPickLoupe(PaintSession* s, const QPointF& tex, const QColor& c, const QPoint& global) {
  static QPointer<QLabel> tip; static QTimer* hide = nullptr;
  if (!tip) { tip = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint); tip->setAttribute(Qt::WA_TransparentForMouseEvents); tip->setAttribute(Qt::WA_ShowWithoutActivating); }
  if (!hide) { hide = new QTimer; hide->setSingleShot(true); QObject::connect(hide, &QTimer::timeout, [] { if (tip) tip->hide(); }); }
  const int n = 7, cell = 8, W = n * cell;                     // (7×7 texels round the point, each 8 px; the middle one framed; the colour taken beside)
  QImage pic(W * 2 + 6, W, QImage::Format_ARGB32); pic.fill(QColor(30, 30, 32));
  const int cx = int(std::floor(tex.x())), cy = int(std::floor(tex.y()));
  const QImage px = s->doc.width() > 0 ? s->doc.compose(paint::Channel::Color, QRect(cx - n / 2, cy - n / 2, n, n)).convertToFormat(QImage::Format_ARGB32) : QImage();
  { QPainter p(&pic);
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) { QColor k(40, 40, 44); if (!px.isNull() && x < px.width() && y < px.height()) { const QRgb v = px.pixel(x, y); if (qAlpha(v) > 8) k = QColor(qRed(v), qGreen(v), qBlue(v)); } p.fillRect(x * cell, y * cell, cell, cell, k); }
    p.setPen(QPen(Qt::white, 1)); p.drawRect((n / 2) * cell, (n / 2) * cell, cell - 1, cell - 1);
    p.fillRect(W + 6, 0, W, W, c); p.setPen(QPen(QColor(0, 0, 0, 160), 1)); p.drawRect(W + 6, 0, W - 1, W - 1); }
  tip->setPixmap(QPixmap::fromImage(pic)); tip->adjustSize(); tip->move(global + QPoint(22, 22)); tip->show(); tip->raise(); hide->start(700);
}

void PaintCanvas::begin(const QPointF& w, float pressure, bool invert, int how) {
  if (s_->freezePaint && !s_->view.startsWith("bake:")) {       // (the freeze mask painted: white — frozen; Ctrl / the eraser — taken off)
    s_->ensureDoc(); QImage* fz = s_->freezeOf(true); if (!fz) return;
    Brush b = s_->brush; b.mode = bool(invert || (QApplication::keyboardModifiers() & Qt::ControlModifier)) != eraser_ ? BrushMode::MaskHide : BrushMode::MaskReveal;
    if (s_->freezeHard) {                                       // (the mask's brush: a hard round tip in full, whatever the pen presses — a texel is frozen or it is not)
      b.tip = {}; b.tipKind = TipKind::Image; b.hardness = 1; b.flow = 1; b.opacity = 1; b.pressureSize = b.pressureFlow = b.pressureOpacity = false;
      b.sizeJitter = b.angleJitter = b.scatter = 0; b.spacing = std::min(b.spacing, 0.1f); pressure = 1; }
    stroke_ = std::make_unique<Stroke>(*fz, b, QColor(Qt::white)); stroke_->sizeScale = texScale_;
    if (!stroke_->valid()) { stroke_.reset(); return; }
    if (mirror) stroke_->mirror = [m = mirror](const QPointF& t) { auto r = m(t); return r ? *r : t; }; if (mirrorsN) stroke_->mirrors = mirrorsN;
    PaintSession::Undo u; u.layer = -2; undoId_ = s_->pushUndo(std::move(u)); target_ = fz;
    painting_ = true; dirty_ = {}; lazy_ = w; lastPressure_ = pressure; pLo_ = pHi_ = pressure;
    stroke_->begin(toTex(w), pressure); dirty_ = stroke_->takeDirty(); emit s_->pixelsChanged(dirty_); return;
  }
  // (the Stamp from an anchor: Ctrl + click sets where the texture is taken from — as Blender's clone; Alt is the camera and the colour)
  if (s_->tool == "stamp" && s_->stampMode == 1 && !s_->editMask && (invert || (QApplication::keyboardModifiers() & Qt::ControlModifier))) {
    s_->stampAnchor = toTex(w); s_->stampOff.reset(); update(); ui::toast(tr("Stamp: the anchor is set — paint elsewhere, the texture comes from the anchor")); return; }
  Layer* l = s_->active();
  // (a map with no layer yet — a mode just switched to: the first stroke makes its paint layer, nothing to set up by hand)
  if (!s_->editMask && !s_->view.startsWith("bake:") && (!l || l->channel != s_->modeChannel())) {
    int top = -1; for (const Layer& x : s_->doc.layers()) if (x.channel == s_->modeChannel() && x.kind != LayerKind::Group) top = x.id;
    if (top < 0) { s_->ensureDoc(); s_->layer = s_->doc.addPaint(s_->modeChannel(), {}, -1); emit s_->layersChanged(); emit s_->activeChanged(); l = s_->active(); }
    else { s_->setLayer(top); l = s_->active(); } }   // (the active layer is of another map: the stroke goes into the topmost layer of the map shown, as a mode switch picks it)
  if (!l || (l->kind != LayerKind::Paint && !(s_->editMask && !l->mask.isNull()))) { ui::toast(tr("Select a paint layer to paint on")); return; }
  if (l->locked) { ui::toast(tr("The layer is locked")); return; }
  if (!s_->editMask && !l->fx.isEmpty()) { ui::toast(tr("An effect layer is made again from its source: bake it first (the Layers panel) to paint on it")); return; }   // (what is painted there would be wiped at its next regeneration)
  if (!s_->editMask && l->kind == LayerKind::Paint && l->image.isNull()) s_->doc.ensurePixels(l->id);   // (a layer not painted in this UV set yet: its picture made now)
  QImage* target = &l->image; if (s_->editMask) { if (l->mask.isNull()) { ui::toast(tr("The layer has no mask: add one in the Layers panel")); return; } target = &l->mask; }
  Brush b = s_->brush; b.mode = s_->effectiveMode();
  if (!s_->editMask && l->image.format() == QImage::Format_ARGB32) { if (how == 1 && s_->tool != "blur" && s_->brush.shiftMode == 2) b.mode = BrushMode::Smudge; else if (s_->tool == "blur" || how == 1) b.mode = BrushMode::Blur; else if (s_->tool == "smudge") b.mode = BrushMode::Smudge; }   // (Shift: a blur — or oil, by the brush's Shift mode)   // (the tool in hand — or Shift held: the colours softened, pulled)
  if (s_->tool == "stamp" && how != 1) {                       // (the Stamp: a copy or a patch — a colour layer's)
    if (s_->editMask || l->image.format() != QImage::Format_ARGB32) { ui::toast(tr("The Stamp works on a colour layer — not on a mask, the bump or the flow")); return; }
    b.mode = s_->stampMode == 2 ? BrushMode::Patch : BrushMode::Clone; }
  const bool shiftBlur = how == 1 && s_->tool != "blur";
  if (bool(invert || (QApplication::keyboardModifiers() & Qt::ControlModifier)) != eraser_) {   // (Ctrl held / the pen's eraser end: the brush inverted — paint erases, raise lowers, reveal hides)
    switch (b.mode) { case BrushMode::Paint: b.mode = BrushMode::Erase; break; case BrushMode::Erase: b.mode = BrushMode::Paint; break; case BrushMode::Raise: b.mode = BrushMode::Lower; break;
      case BrushMode::Lower: b.mode = BrushMode::Raise; break; case BrushMode::MaskReveal: b.mode = BrushMode::MaskHide; break; case BrushMode::MaskHide: b.mode = BrushMode::MaskReveal; break; default: break; } }
  // (a gray map — a mask the shader reads as one number: the brush's colour goes in as its gray)
  QColor col = s_->color; if (!s_->editMask && mapInfo(l->channel).gray && mapInfo(l->channel).storage == Storage::Colour) { const int g = qGray(col.rgb()); col = QColor(g, g, g); }
  fx_ = s_->tool == "recolor" && !s_->editMask && how != 1 && l->image.format() == QImage::Format_ARGB32;
  if (s_->tool == "recolor" && !fx_ && how != 1) { ui::toast(tr("Recolor works on a colour layer — not on a mask, the bump or the flow")); return; }
  if (fx_) {                                                  // (Recolor: the stroke lays a coverage, the layer is recoloured under it)
    if (fxCov_.size() != target->size()) { fxCov_ = QImage(target->size(), QImage::Format_Grayscale8); fxCov_.fill(0); }
    fxSnap_.start(*target); fxOp_ = s_->recolorOp; fxInk_ = col; b.mode = BrushMode::MaskReveal;
    stroke_ = std::make_unique<Stroke>(fxCov_, b, QColor(Qt::white)); stroke_->sizeScale = texScale_; }
  else { stroke_ = std::make_unique<Stroke>(*target, b, col); stroke_->sizeScale = texScale_; stroke_->lockAlpha = !s_->editMask && l->lockAlpha; stroke_->maskInk = s_->editMask; }
  if (shiftBlur) { stroke_->softenK = s_->brush.shiftBlur; stroke_->blurAlong = s_->brush.shiftMode == 1; stroke_->oilCarry = s_->brush.shiftMode == 2; }   // (Shift's blur: as strong as the brush says — 3D-Coat's Smoothing; along the strand when set)
  if (const paint::IslandMap* im = s_->islands ? s_->islands() : nullptr; im && !im->ids.isNull() && im->ids.size() == target->size()) {
    stroke_->strandAngle = [im](const QPointF& p) { return im->angleOf(im->at(p)); };   // (the alpha along the strand: its island's long axis)
    if (s_->islandLock) { const QPointF t0 = toTex(w); if (const int id = im->at(t0); id > 0) {   // (the stroke kept in the island it began on — and in its mirrors')
      stroke_->clipIds = &im->ids; stroke_->clipAllow = {quint32(id)};
      if (mirrorsN) for (const QPointF& q : mirrorsN(t0)) if (const int k = im->at(q); k > 0) stroke_->clipAllow.push_back(quint32(k));
      if (mirror) if (const auto q = mirror(t0)) if (const int k = im->at(*q); k > 0) stroke_->clipAllow.push_back(quint32(k)); } } }
  if (mirror) stroke_->mirror = [m = mirror](const QPointF& t) { auto r = m(t); return r ? *r : t; }; if (mirrorsN) stroke_->mirrors = mirrorsN;
  patch_ = false;
  if (b.mode == BrushMode::Clone || b.mode == BrushMode::Patch) {   // (the Stamp: its source — no mirrored dabs of its own)
    stroke_->mirror = {}; stroke_->mirrors = {}; patch_ = b.mode == BrushMode::Patch;
    stroke_->cloneSrc = b.mode == BrushMode::Clone && s_->stampAll ? s_->doc.compose(s_->modeChannel()).convertToFormat(QImage::Format_ARGB32) : *target;   // (the layer: shared, it stays as it was when the stroke writes)
    if (stroke_->cloneSrc.size() != target->size()) stroke_->cloneSrc = stroke_->cloneSrc.scaled(target->size());
    if (b.mode == BrushMode::Clone && s_->stampMode == 0) {
      if (!mirrorOf || !symmetryOn || !symmetryOn()) { ui::toast(tr("Symmetric copy: turn Symmetry on (Shift+S) — the Stamp lays the mirrored side under the brush")); stroke_.reset(); return; }
      const double d = std::max(2.0, double(b.size) * texScale_ * 0.25);
      // (the source of a dab: the mirror of its middle, and how the texture turns and flips there — the mirrors of two points beside it)
      stroke_->cloneAt = [m = mirrorOf, d](const QPointF& p) -> std::optional<QTransform> {
        const auto q = m(p), qx = m(p + QPointF(d, 0)), qy = m(p + QPointF(0, d)); if (!q || !qx || !qy) return std::nullopt;
        const QPointF ax = (*qx - *q) / d, ay = (*qy - *q) / d; const double det = ax.x() * ay.y() - ax.y() * ay.x(); if (std::abs(det) < 0.05 || std::abs(det) > 20) return std::nullopt;   // (across a UV seam: not this dab)
        return QTransform(ax.x(), ax.y(), ay.x(), ay.y(), q->x() - ax.x() * p.x() - ay.x() * p.y(), q->y() - ax.y() * p.x() - ay.y() * p.y()); }; }
    else if (b.mode == BrushMode::Clone) {
      if (!s_->stampAnchor) { ui::toast(tr("Stamp from an anchor: Ctrl + click first where the texture is to be taken from")); stroke_.reset(); return; }
      if (!s_->stampOff) s_->stampOff = *s_->stampAnchor - toTex(w);   // (aligned, as Photoshop's: the offset kept stroke after stroke until a new anchor)
      const QPointF off = *s_->stampOff; stroke_->cloneAt = [off](const QPointF&) { return std::optional<QTransform>(QTransform::fromTranslate(off.x(), off.y())); }; } }
  if (!stroke_->valid()) { ui::toast(tr("The brush writes %1: it does not fit this layer").arg(brushModeName(b.mode))); stroke_.reset(); return; }
  if (strokeBegan) strokeBegan();
  // (the step for Ctrl+Z: empty for now — the stroke keeps the tiles it touches as they were, and they go into it when it ends;
  // the layer is not copied whole at every stroke's start)
  PaintSession::Undo u; u.layer = l->id; u.mask = s_->editMask; undoId_ = s_->pushUndo(std::move(u)); target_ = target;
  painting_ = true; dirty_ = {}; const QPointF t = toTex(w);
  lazy_ = w; lastPressure_ = pressure; pLo_ = pHi_ = pressure;
  if (how == 2 && hasLastEnd_) { stroke_->begin(lastEnd_, pressure); stroke_->move(t, pressure); lastEnd_ = t; } else stroke_->begin(t, pressure);   // (Shift + click: a straight line from where the last stroke ended)
  applied(stroke_->takeDirty());
}
// a part of the stroke was laid: Recolor's effect made from the layer as it was; what the brush may not touch put back
void PaintCanvas::applied(const QRect& r) {
  if (r.isNull() || !target_) return;
  if (fx_) { fxSnap_.reach(*target_, r.adjusted(-4, -4, 4, 4));   // (a little wider: Blur / Sharpen read the texels round each one)
    paint::applyRegion(*target_, fxSnap_.image(), fxCov_, QPoint(0, 0), r, fxOp_, fxInk_, 1); if (clip) clip(*target_, fxSnap_.image(), r); }
  else if (patch_ && stroke_) {                                // (the patch: the marked texels filled from round them — near the dabs now, whole at the end)
    const int pad = std::max(8, int(s_->brush.size * texScale_));
    paint::healRegion(*target_, stroke_->cloneSrc, stroke_->coverage(), r, pad, s_->islands ? s_->islands() : nullptr); if (clip) clip(*target_, stroke_->before(), r); }
  else if (clip) clip(*target_, stroke_->before(), r);
  dirty_ = dirty_.united(r); emit s_->pixelsChanged(r);
}
void PaintCanvas::move(const QPointF& w, float pressure) {
  if (!painting_ || !stroke_) return;
  lastPressure_ = pressure; pLo_ = std::min(pLo_, pressure); pHi_ = std::max(pHi_, pressure); QPointF at = w;
  if (const double R = std::clamp(double(s_->brush.smoothing), 0.0, 1.0) * 2 * double(s_->brush.size) * texScale_ * zoom(); R > 0.5) {   // (Steady Stroke: the brush moves only when the pointer pulls its string — as long as the smoothing's share of 2 × the brush's diameter on the screen)
    const QPointF d = w - lazy_; const double dist = std::hypot(d.x(), d.y()); if (dist <= R) { update(); return; }
    lazy_ += d * ((dist - R) / dist); at = lazy_; }
  lastEnd_ = toTex(at); hasLastEnd_ = true; stroke_->move(lastEnd_, pressure); applied(stroke_->takeDirty());
}
void PaintCanvas::end() {
  if (!painting_) return; painting_ = false;
  if (patch_ && stroke_ && target_ && !dirty_.isEmpty()) {     // (the patch once more over the whole mark: no seams between the parts filled as it went)
    const int pad = std::clamp(std::max(dirty_.width(), dirty_.height()) / 2, 16, 512);
    paint::healRegion(*target_, stroke_->cloneSrc, stroke_->coverage(), dirty_, pad, s_->islands ? s_->islands() : nullptr); if (clip) clip(*target_, stroke_->before(), dirty_); emit s_->pixelsChanged(dirty_); }
  patch_ = false;
  if (stroke_) if (PaintSession::Undo* u = s_->undoById(undoId_)) u->tiles = fx_ ? fxSnap_.tiles() : stroke_->takeBefore();   // (what the stroke changed, as it was: its undo step)
  if (fx_) { fx_ = false; const QRect r = dirty_.intersected(fxCov_.rect()); for (int y = r.top(); y <= r.bottom(); y++) std::memset(fxCov_.scanLine(y) + r.left(), 0, size_t(r.width())); }   // (the coverage empty again for the next stroke)
  ui::devLog("stroke", {{"how", "texture"}, {"tool", s_->tool}, {"size", double(s_->brush.size)}, {"opacity", double(s_->brush.opacity)}, {"pen", penStroke_}, {"x", dirty_.x()}, {"y", dirty_.y()}, {"w", dirty_.width()}, {"h", dirty_.height()}});
  if (strokeEnded) strokeEnded(penStroke_, pLo_, pHi_); stroke_.reset(); target_ = nullptr; normalDirty_ = true; if (s_->view == "normalBump") rebuild();
}

// the keys and modifiers of the painting programs (Photoshop, SAI, Krita), the same for the mouse and the pen:
// Space + drag — move the view · Ctrl+Space + drag — zoom · Alt — the colour under the pen (held: as it moves) · Ctrl+Alt + drag / Alt+RMB — the brush's size
bool PaintCanvas::pressAt(const QPointF& w, Qt::MouseButton b, Qt::KeyboardModifiers mods, float pressure, bool invert) {
  setFocus(); last_ = anchor_ = w; hoverAt_ = w;
  if (pressFilter && pressFilter(w, b, b == Qt::MiddleButton || space_)) { update(); return true; }
  if (b == Qt::LeftButton && (rKey_ || (space_ && (mods & (Qt::ShiftModifier | Qt::AltModifier))))) { rotating_ = true; rotRaw_ = rot_; setCursor(Qt::SizeAllCursor); update(); return true; }   // (R held — Photoshop, Shift+Space — Krita, Alt+Space — SAI)   // (Shift+Space — Krita, Alt+Space — SAI, R held — Photoshop)
  if (b == Qt::LeftButton && space_ && (mods & Qt::ControlModifier)) { zooming_ = true; setCursor(Qt::SizeVerCursor); update(); return true; }
  if (b == Qt::MiddleButton || (b == Qt::LeftButton && space_)) { panning_ = true; setCursor(Qt::ClosedHandCursor); update(); return true; }
  const bool ctrl = mods & Qt::ControlModifier, alt = mods & Qt::AltModifier, shift = mods & Qt::ShiftModifier;
  // (RMB drag: a plain one — size ←→ and opacity ↑↓, as on the model (3D-Coat); Alt+RMB / Ctrl+Alt+LMB — size and hardness, as Photoshop)
  if (ring && ((alt && ((b == Qt::LeftButton && ctrl) || b == Qt::RightButton)) || (b == Qt::RightButton && !ctrl && !shift))) {
    sizing_ = true; adjMode_ = alt ? 2 : 1; size0_ = s_->brush.size; op0_ = s_->brush.opacity; hard0_ = s_->brush.hardness; return true; }
  if (b != Qt::LeftButton) return false;
  if (ctrl && shift && !alt && pickLayer) { pickLayer(toTex(w)); return true; }
  if (toolInput && toolInput(0, toTex(w), mods)) { tooling_ = true; update(); return true; }
  if (alt) { picking_ = true; pick(w); update(); return true; }
  if (shift && !s_->freezePaint) { shiftPending_ = true; shiftAt_ = w; shiftP_ = pressure; shiftInv_ = invert; return true; }   // (blur or a straight line: known once it moves / is let go)
  begin(w, pressure, invert); return true;
}
void PaintCanvas::zoomDrag(const QPointF& w) {
  const double dx = (w.x() - last_.x()) - (w.y() - last_.y()); last_ = w;
  if (navZoom) navZoom(anchor_, dx / 45); else zoomAt(anchor_, std::pow(1.15, dx / 45));
}
void PaintCanvas::moveAt(const QPointF& w, Qt::KeyboardModifiers mods, float pressure) {
  const QPointF was = hoverAt_; hover_ = true; if (!sizing_) hoverAt_ = w;
  if (rotating_) { const QPointF c = viewCentre ? viewCentre() : QPointF(width() / 2.0, height() / 2.0); const double a0 = std::atan2(last_.y() - c.y(), last_.x() - c.x()), a1 = std::atan2(w.y() - c.y(), w.x() - c.x()); double d = (a1 - a0) * 180 / M_PI; if (d > 180) d -= 360; if (d < -180) d += 360; last_ = w; rotateBy(d, (mods & Qt::ShiftModifier && !space_) ? 1 : 0); return; }
  if (zooming_) { zoomDrag(w); return; }
  if (panning_) { panBy(w - last_); last_ = w; return; }
  if (sizing_) { s_->brush.size = std::clamp(size0_ * float(std::pow(1.008, w.x() - anchor_.x())), 1.f, 2048.f);
    const float dy = float(w.y() - anchor_.y());
    if (adjMode_ == 1) hud(tr("Brush size %1").arg(int(std::lround(s_->brush.size))));   // (a plain RMB drag: the size alone — up and down does nothing, so a slanted drag never changes anything else)
    else if (std::abs(dy) > 3) { s_->brush.hardness = std::clamp(hard0_ - dy / 250.f, 0.f, 1.f); s_->brush.tip = {}; hud(tr("Brush: size %1 · hardness %2%").arg(int(std::lround(s_->brush.size))).arg(int(std::lround(s_->brush.hardness * 100)))); }
    else hud(tr("Brush size %1").arg(int(std::lround(s_->brush.size))));
    emit s_->brushChanged(); update(); return; }
  if (shiftPending_) { if (std::hypot(w.x() - shiftAt_.x(), w.y() - shiftAt_.y()) < 3) return;   // (Shift held and dragged: the brush blurs from where it was pressed)
    shiftPending_ = false; begin(shiftAt_, shiftP_, shiftInv_, 1); }
  if (tooling_) { toolInput(1, toTex(w), mods); update(); return; }
  if (picking_) { pick(w); return; }
  if (painting_) move(w, pressure);
  if (hovered && !painting_) hovered(w);
  if (pointerTex) pointerTex(toTex(w));
  if (bare) hoverUpdate(was); else if (mirror || drawOver) update(); else update(QRect(hoverAt_.toPoint() - QPoint(300, 300), QSize(600, 600)));
}
void PaintCanvas::releaseAt(const QPointF& w, Qt::KeyboardModifiers mods) {
  if (shiftPending_) { shiftPending_ = false; begin(shiftAt_, shiftP_, shiftInv_, 2); }   // (Shift + a click: a straight stroke from the last one's end)
  if (painting_ && stroke_ && s_->brush.smoothing > 0.005) {   // (the string's end: the brush catches up with where the pointer was let go)
    lastEnd_ = toTex(w); stroke_->move(lastEnd_, lastPressure_); applied(stroke_->takeDirty()); }
  if (tooling_) { tooling_ = false; toolInput(2, toTex(w), mods); }
  if (panning_ || zooming_ || rotating_) setCursor(rKey_ ? Qt::SizeAllCursor : space_ ? Qt::OpenHandCursor : restCursor);
  if (sizing_) { hud({}); QTimer::singleShot(600, this, [] { ui::hud({}); }); }
  panning_ = zooming_ = rotating_ = picking_ = sizing_ = false; end(); update();
}
void PaintCanvas::hud(const QString& s) { ui::hud(s); }
void PaintCanvas::enterEvent(QEnterEvent*) {
  QWidget* f = QApplication::focusWidget();   // (the pen back over the canvas — over the viewport or in the manager: its keys (R, Space, [ ]) work again — unless a name is being typed in a panel)
  if (!qobject_cast<QLineEdit*>(f) && !qobject_cast<QAbstractSpinBox*>(f)) setFocus();
}

void PaintCanvas::mousePressEvent(QMouseEvent* e) { penStroke_ = false; pressAt(e->position(), e->button(), e->modifiers(), 1.f, e->modifiers() & Qt::ControlModifier); }
void PaintCanvas::mouseMoveEvent(QMouseEvent* e) { moveAt(e->position(), e->modifiers(), 1.f); }
void PaintCanvas::mouseReleaseEvent(QMouseEvent* e) { releaseAt(e->position(), e->modifiers()); }

void PaintCanvas::tabletEvent(QTabletEvent* e) {   // (the pen: pressure into the brush; mouse events from it are not made when this accepts)
  const QPointF w = e->position();
  eraserTip_ = e->pointerType() == QPointingDevice::PointerType::Eraser;   // (the pen turned over: it erases)
  paint::g_penTilt = e->type() == QEvent::TabletRelease ? 1.f : std::clamp(float(std::hypot(e->xTilt(), e->yTilt()) / 60.0), 0.f, 1.f);   // (the tilt: what a pen parameter may follow)
  switch (e->type()) {
    case QEvent::TabletPress: penStroke_ = true; pressAt(w, e->button(), e->modifiers(), float(e->pressure()), eraserTip_); break;   // (the barrel button sent as the middle one: moves the view)
    case QEvent::TabletMove: moveAt(w, e->modifiers(), float(e->pressure())); break;
    case QEvent::TabletRelease: releaseAt(w, e->modifiers()); break;
    default: break;
  }
  e->accept();
}

void PaintCanvas::keyPressEvent(QKeyEvent* e) {
  const int k = ui::Keymap::latinKey(e); const QString code = ui::Keymap::codeOf(e); const bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier, alt = e->modifiers() & Qt::AltModifier;
  auto opacity = [&](int n) { s_->brush.opacity = n / 10.f; emit s_->brushChanged(); hud(tr("Brush opacity %1%").arg(n * 10)); QTimer::singleShot(700, this, [] { ui::hud({}); }); };
  if (e->key() == Qt::Key_Control) { update(); QWidget::keyPressEvent(e); }   // (the cursor turns red: the brush erases while Ctrl is held)
  else if (e->key() == Qt::Key_Alt) { if (ring && !painting_) { setCursor(Qt::CrossCursor); update(); } e->accept(); }   // (the pipette under the pen)
  else if (e->key() == Qt::Key_Space) { if (!e->isAutoRepeat()) { space_ = true; if (!panning_ && !painting_) setCursor(Qt::OpenHandCursor); update(); } }
  else if (k == Qt::Key_R && !ctrl && !alt) { if (shift) { if (!e->isAutoRepeat()) { if (navRotate) { if (navRotateReset) navRotateReset(); } else { rotRaw_ = 0; setRotation(0); } } } else if (!e->isAutoRepeat()) { rKey_ = true; if (!painting_) setCursor(Qt::SizeAllCursor); } }
  else if (painting_ && ctrl && (k == Qt::Key_Z || k == Qt::Key_Y)) {}   // (a stroke going: its undo step is not whole yet — Ctrl+Z waits for the pen to lift)
  else if (k == Qt::Key_Z && ctrl && undoHook && undoHook(shift)) {}
  else if (k == Qt::Key_Y && ctrl && undoHook && undoHook(true)) {}
  else if (k == Qt::Key_Z && ctrl) { if (shift) { if (!s_->redoStroke()) ui::toast(tr("Nothing to redo in the canvas")); } else if (!s_->undoStroke()) ui::toast(tr("Nothing to undo in the canvas")); }
  else if (k == Qt::Key_Y && ctrl) { if (!s_->redoStroke()) ui::toast(tr("Nothing to redo in the canvas")); }
  else if (ctrl && code == "Digit0") fit();
  else if (ctrl && (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal)) { if (navZoom) navZoom(QPointF(width() / 2.0, height() / 2.0), 1); else zoomAt(QPointF(width() / 2.0, height() / 2.0), 1.15); }
  else if (ctrl && e->key() == Qt::Key_Minus) { if (navZoom) navZoom(QPointF(width() / 2.0, height() / 2.0), -1); else zoomAt(QPointF(width() / 2.0, height() / 2.0), 1 / 1.15); }
  else if (!ctrl && !alt && (k == Qt::Key_F || e->key() == Qt::Key_Home)) fit();
  else if (code == "BracketLeft" || code == "BracketRight") { const bool up = code == "BracketRight";
    if (shift) { s_->brush.hardness = std::clamp(s_->brush.hardness + (up ? 0.1f : -0.1f), 0.f, 1.f); hud(tr("Brush hardness %1%").arg(int(std::lround(s_->brush.hardness * 100)))); }
    else { s_->brush.size = up ? std::min(2048.f, s_->brush.size / 0.85f) : std::max(1.f, s_->brush.size * 0.85f); hud(tr("Brush size %1").arg(int(std::lround(s_->brush.size)))); }
    emit s_->brushChanged(); update(); QTimer::singleShot(700, this, [] { ui::hud({}); }); }
  else if (!ctrl && !alt && !shift && k == Qt::Key_E && eraserKey) eraserKey();
  else if (!ctrl && !alt && !shift && k == Qt::Key_E) { eraser_ = !eraser_; update(); ui::toast(eraser_ ? tr("Eraser") : tr("Brush")); }
  else if (!ctrl && !alt && !shift && k == Qt::Key_X) { std::swap(s_->color, s_->color2); emit s_->brushChanged(); }
  else if (!ctrl && !alt && !shift && k == Qt::Key_D) { s_->color = Qt::black; s_->color2 = Qt::white; emit s_->brushChanged(); }
  else if (!ctrl && !alt && !shift && code.size() == 6 && code.startsWith("Digit")) { const int n = code[5].digitValue(); opacity(n == 0 ? 10 : n); }
  else QWidget::keyPressEvent(e);
}
void PaintCanvas::keyReleaseEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Control) update();
  if (ui::Keymap::latinKey(e) == Qt::Key_R) { if (!e->isAutoRepeat()) { rKey_ = false; if (!rotating_) setCursor(space_ ? Qt::OpenHandCursor : restCursor); } return; }
  if (e->key() == Qt::Key_Alt) { if (!panning_) setCursor(space_ ? Qt::OpenHandCursor : restCursor); update(); e->accept(); return; }
  if (e->key() == Qt::Key_Space) { if (!e->isAutoRepeat()) { space_ = false; if (!panning_ && !zooming_) setCursor(restCursor); update(); } return; }
  QWidget::keyReleaseEvent(e);
}

// ---- the window ----

QWidget* makePaintCanvasWindow(PaintSession* s, QWidget* parent, const PaintCanvas* like) {
  auto* w = new QWidget(parent, Qt::Tool); w->setWindowTitle(QObject::tr("Texture")); w->resize(620, 640);
  auto* v = new QVBoxLayout(w); v->setContentsMargins(6, 6, 6, 6); v->setSpacing(6);
  auto* bar = new QHBoxLayout; auto* canvas = new PaintCanvas(s, w);
  if (like) {   // (the room's guards and symmetry: a stroke here keeps off frozen texels, locked / hidden polygons, unselected objects)
    canvas->clip = like->clip; canvas->strokeBegan = like->strokeBegan; canvas->strokeEnded = like->strokeEnded; canvas->mirrorsN = like->mirrorsN;
    canvas->mirrorOf = like->mirrorOf; canvas->symmetryOn = like->symmetryOn; canvas->pickLayer = like->pickLayer; canvas->uvWire = like->uvWire; canvas->eraserKey = like->eraserKey; }
  // (a brush by hand only: the Eraser tool erases; Fill, Select, Text, the lasso / shapes / lines are the room's — refused, not laid as a plain stroke)
  canvas->toolInput = [s, canvas](int ph, const QPointF&, Qt::KeyboardModifiers) {
    const QString& t = s->tool; canvas->setEraser(t == "eraser");
    if ((t == "brush" || t == "eraser" || t == "blur" || t == "smudge" || t == "recolor" || t == "stamp") && s->drawMode == "stroke") return false;
    if (ph == 0) ui::toast(QObject::tr("This window paints with a brush by hand — use this tool or drawing mode on the model or in the room's texture view"));
    return true; };
  auto* views = new QComboBox; views->setToolTip(QObject::tr("The map being painted — the room's mode: each map has its own layers, and the brush writes what that map holds. The Bake's maps show once baked"));
  auto fill = [views, s] {
    const QString was = s->view; QSignalBlocker b(views); views->clear();
    for (const MapInfo& m : maps()) { views->addItem(QObject::tr(m.name), QString::fromLatin1(m.key)); views->setItemData(views->count() - 1, QObject::tr(m.about), Qt::ToolTipRole);
      if (m.ch == Channel::Height) { views->addItem(QObject::tr("Normal map (the result)"), "normalBump"); views->setItemData(views->count() - 1, QObject::tr("The normal map as it will be written: the baked normals where the Bake mask is painted, the painted bump over them"), Qt::ToolTipRole); } }
    for (auto it = s->baked.begin(); it != s->baked.end(); ++it) views->addItem(QObject::tr("Baked: %1").arg(it.key()), "bake:" + it.key());
    int i = views->findData(was); views->setCurrentIndex(i < 0 ? 0 : i); };
  fill();
  QObject::connect(views, &QComboBox::currentIndexChanged, s, [views, s] { s->setView(views->currentData().toString()); });
  QObject::connect(s, &PaintSession::viewChanged, views, fill);
  QObject::connect(s, &PaintSession::bakeChanged, views, fill);
  auto* fitB = new QPushButton(QObject::tr("Fit")); fitB->setToolTip(QObject::tr("Fit the texture to the window (F)")); QObject::connect(fitB, &QPushButton::clicked, canvas, &PaintCanvas::fit);
  auto* hint = new QLabel(QObject::tr("LMB paint · hold Ctrl — erase · Alt+LMB pick colour · wheel zoom · MMB / Space pan · [ ] size · Ctrl+Z undo")); hint->setProperty("mut", true);
  auto* on3d = new QPushButton(QObject::tr("On the model")); on3d->setCheckable(true); on3d->setChecked(s->onModel); on3d->setFocusPolicy(Qt::NoFocus);
  on3d->setToolTip(QObject::tr("The map shown on the meshes in the viewport, and the brush paints right on them: the left button over a mesh of the UV set (Ctrl — erase; Alt and the other buttons move the camera). A mesh needs UVs — unfold it in the Topology room"));
  QObject::connect(on3d, &QPushButton::toggled, s, [s](bool on) { s->onModel = on; emit s->viewChanged(); });
  bar->addWidget(views); bar->addWidget(fitB); bar->addWidget(on3d); bar->addStretch(1); v->addLayout(bar); v->addWidget(canvas, 1); v->addWidget(hint);
  return w;
}

}  // namespace hm
