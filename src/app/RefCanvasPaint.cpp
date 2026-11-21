// The reference canvas's drawing tools besides the brush (RefCanvas.h): the selection (rectangle, ellipse, lasso, polygonal
// lasso, magic wand) as a mask the other tools keep inside, the fill (the bucket), the shapes, and ✥ Move's "all layers" — the
// whole canvas shifted in the view.
#include "app/PaintCanvas.h"
#include "app/PaintSession.h"
#include "app/RefCanvas.h"
#include "paint/PaintDoc.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QPainter>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace hm {

using namespace paint;

namespace {
void pushUndo(PaintSession* s, const Layer* l, bool mask = false) {
  s->pushUndo({l->id, mask, mask ? l->mask.copy() : l->image.copy()});
}
}  // namespace

// ================= the selection =================

void RefCanvas::setSelection(const QPainterPath& path0, int op) {
  const int W = s_->doc.width(), H = s_->doc.height(); if (W == 0) return;
  QPainterPath path = path0; path.setFillRule(Qt::WindingFill);
  if (op == 0 || selMask_.isNull()) { if (op == 2) return; selMask_ = QImage(W, H, QImage::Format_Alpha8); selMask_.fill(0); }
  { QPainter p(&selMask_); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen);
    if (op == 2) p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.fillPath(path, QColor(0, 0, 0, 255)); }
  outlineSel();
}
// the marching ants' path: the edges between the selected pixels (half covered or more) and the rest, in canvas pixels
void RefCanvas::outlineSel() {
  selPath_ = {};
  if (selMask_.isNull()) return;
  const int W = selMask_.width(), H = selMask_.height(); int x0 = W, y0 = H, x1 = -1, y1 = -1;
  for (int y = 0; y < H; y++) { const uchar* r = selMask_.constScanLine(y); for (int x = 0; x < W; x++) if (r[x] >= 128) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); } }
  if (x1 < 0) { deselect(); return; }
  auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && selMask_.constScanLine(y)[x] >= 128; };
  for (int y = y0; y <= y1 + 1; y++) { int run = -1;                 // (across: between row y − 1 and row y)
    for (int x = x0; x <= x1 + 1; x++) { const bool e = x <= x1 && in(x, y - 1) != in(x, y);
      if (e && run < 0) run = x; else if (!e && run >= 0) { selPath_.moveTo(run, y); selPath_.lineTo(x, y); run = -1; } } }
  for (int x = x0; x <= x1 + 1; x++) { int run = -1;                 // (down: between column x − 1 and column x)
    for (int y = y0; y <= y1 + 1; y++) { const bool e = y <= y1 && in(x - 1, y) != in(x, y);
      if (e && run < 0) run = y; else if (!e && run >= 0) { selPath_.moveTo(x, run); selPath_.lineTo(x, y); run = -1; } } }
  selBox_ = QRect(QPoint(x0, y0), QPoint(x1, y1));
  if (!ants_->isActive()) ants_->start();
  canvas_->update(); syncInfo();
}
void RefCanvas::deselect() { selMask_ = {}; selPath_ = {}; selBox_ = {}; poly_.clear(); polyOn_ = false; if (ants_) ants_->stop(); canvas_->update(); syncInfo(); }
void RefCanvas::selectAll() { QPainterPath p; p.addRect(QRectF(0, 0, s_->doc.width(), s_->doc.height())); setSelection(p, 0); }
void RefCanvas::invertSel() {
  if (!hasSel()) { selectAll(); return; }
  for (int y = 0; y < selMask_.height(); y++) { uchar* r = selMask_.scanLine(y); for (int x = 0; x < selMask_.width(); x++) r[x] = uchar(255 - r[x]); }
  outlineSel();
}

// the pixels outside the selection put back as they were before (the edge's soft pixels mixed by its coverage)
void RefCanvas::clipToSel(QImage& img, const QImage& before, const QRect& r0) const {
  if (selMask_.isNull() || before.isNull() || img.size() != selMask_.size() || before.size() != img.size() || before.format() != img.format()) return;
  const QRect r = r0.intersected(img.rect()); if (r.isEmpty()) return;
  const int bpp = img.depth() / 8;
  for (int y = r.top(); y <= r.bottom(); y++) {
    const uchar* m = selMask_.constScanLine(y); const uchar* b = before.constScanLine(y); uchar* o = img.scanLine(y);
    for (int x = r.left(); x <= r.right(); x++) {
      const int k = m[x]; if (k == 255) continue;
      if (bpp == 2) { const quint16 bv = reinterpret_cast<const quint16*>(b)[x]; quint16& ov = reinterpret_cast<quint16*>(o)[x]; ov = k == 0 ? bv : quint16(bv + (int(ov) - int(bv)) * k / 255); continue; }
      for (int c = 0; c < bpp; c++) { const int i = x * bpp + c; o[i] = k == 0 ? b[i] : uchar(b[i] + (int(o[i]) - int(b[i])) * k / 255); }
    }
  }
}

// Delete: what is selected cleared from the active layer (its mask when that is painted on)
void RefCanvas::clearSelected() {
  Layer* l = s_->active(); if (!hasSel() || !l || l->kind != LayerKind::Paint) { ui::toast(tr("Select something on a paint layer first")); return; }
  if (l->locked) { ui::toast(tr("The layer is locked")); return; }
  const bool mask = s_->editMask && !l->mask.isNull(); QImage& img = mask ? l->mask : l->image;
  pushUndo(s_, l, mask);
  const QImage before = img.copy(); { QPainter p(&img); p.setCompositionMode(QPainter::CompositionMode_Source); p.fillRect(img.rect(), mask ? QColor(0, 0, 0) : QColor(0, 0, 0, 0)); }
  clipToSel(img, before, img.rect());                         // (outside the selection: put back)
  emit s_->pixelsChanged(selBox_.adjusted(-2, -2, 2, 2));
}

// Move / Transform with something selected: that part of the active layer lifted, floating (the selection is let go)
bool RefCanvas::floatSel() {
  Layer* l = s_->active();
  if (!hasSel() || !l || l->kind != LayerKind::Paint || l->channel != Channel::Color || l->image.isNull()) return false;
  if (l->locked) { ui::toast(tr("The layer is locked")); return false; }
  const QRect r = selBox_.adjusted(-1, -1, 1, 1).intersected(l->image.rect()); if (r.width() < 1 || r.height() < 1) return false;
  pushUndo(s_, l);
  QImage piece = l->image.copy(r).convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < r.height(); y++) { const uchar* m = selMask_.constScanLine(r.y() + y) + r.x(); QRgb* p = reinterpret_cast<QRgb*>(piece.scanLine(y)); QRgb* q = reinterpret_cast<QRgb*>(l->image.scanLine(r.y() + y)) + r.x();
    for (int x = 0; x < r.width(); x++) { const int k = m[x]; p[x] = qRgba(qRed(p[x]), qGreen(p[x]), qBlue(p[x]), qAlpha(p[x]) * k / 255); q[x] = qRgba(qRed(q[x]), qGreen(q[x]), qBlue(q[x]), qAlpha(q[x]) * (255 - k) / 255); } }
  fl_ = {}; fl_.on = true; fl_.layer = l->id; fl_.img = piece; fl_.cx = r.x() + r.width() / 2.0; fl_.cy = r.y() + r.height() / 2.0; flBegin();
  selMask_ = {}; selPath_ = {}; selBox_ = {}; ants_->stop();
  emit s_->pixelsChanged(r); return true;
}

void RefCanvas::polyClose() {
  if (poly_.size() >= 3) { QPainterPath p; p.addPolygon(poly_); p.closeSubpath(); setSelection(p, polyOp_); }
  poly_.clear(); polyOn_ = false; canvas_->update();
}

bool RefCanvas::selInput(int phase, const QPointF& t, Qt::KeyboardModifiers mods) {
  const double z = std::max(1e-6, canvas_->zoom());
  if (selKind_ == "poly") {                                   // (corners by clicks; the mouse's drag does nothing more)
    if (phase != 0) return true;
    if (!polyOn_) { polyOn_ = true; poly_.clear(); polyOp_ = (mods & Qt::ShiftModifier) ? 1 : (mods & Qt::AltModifier) ? 2 : selOp_; poly_ << t; canvas_->update(); return true; }
    if (poly_.size() >= 3 && std::hypot(t.x() - poly_.first().x(), t.y() - poly_.first().y()) * z < 9) { polyClose(); return true; }
    if (std::hypot(t.x() - poly_.last().x(), t.y() - poly_.last().y()) * z < 3) { if (poly_.size() >= 3) polyClose(); return true; }   // (a double click: the same point again — closed)
    poly_ << t; canvas_->update(); return true;
  }
  if (selKind_ == "wand") {
    if (phase != 0) return true;
    QRect box; const std::vector<uchar> m = matchAt(QPoint(int(std::floor(t.x())), int(std::floor(t.y()))), wandTol_, wandContig_, wandAll_, box);
    if (m.empty()) return true;
    const int W = s_->doc.width(), op = (mods & Qt::ShiftModifier) ? 1 : (mods & Qt::AltModifier) ? 2 : selOp_;
    QImage hit(s_->doc.width(), s_->doc.height(), QImage::Format_Alpha8); hit.fill(0);
    for (int y = box.top(); y <= box.bottom(); y++) { uchar* o = hit.scanLine(y); const uchar* q = m.data() + size_t(y) * W; for (int x = box.left(); x <= box.right(); x++) o[x] = q[x]; }
    if (op == 0 || selMask_.isNull()) { if (op == 2) return true; selMask_ = hit; }
    else { QPainter p(&selMask_); p.setCompositionMode(op == 2 ? QPainter::CompositionMode_DestinationOut : QPainter::CompositionMode_SourceOver); p.drawImage(0, 0, hit); }
    outlineSel();
    return true;
  }
  if (phase == 0) { selDrag_ = true; sel0_ = sel1_ = t; selOpNow_ = (mods & Qt::ShiftModifier) ? 1 : (mods & Qt::AltModifier) ? 2 : selOp_; lasso_.clear(); lasso_ << t; canvas_->update(); return true; }
  if (!selDrag_) return true;
  sel1_ = t; if (selKind_ == "lasso" && (lasso_.isEmpty() || std::hypot(t.x() - lasso_.last().x(), t.y() - lasso_.last().y()) * z > 1.5)) lasso_ << t;
  if (phase == 2) {
    selDrag_ = false; QPainterPath p; const QRectF r = QRectF(sel0_, sel1_).normalized();
    if (selKind_ == "lasso") { if (lasso_.size() >= 3) { p.addPolygon(lasso_); p.closeSubpath(); } }
    else if (r.width() * z >= 2 && r.height() * z >= 2) { if (selKind_ == "ellipse") p.addEllipse(r); else p.addRect(r); }
    if (p.isEmpty()) { if (selOpNow_ == 0) deselect(); }      // (a click without a drag: nothing selected any more)
    else setSelection(p, selOpNow_);
    lasso_.clear();
  }
  canvas_->update(); return true;
}

void RefCanvas::drawSel(QPainter& p) {
  const QTransform M = canvas_->mapping(); p.save(); p.setRenderHint(QPainter::Antialiasing); p.setBrush(Qt::NoBrush);
  auto ants = [&](const QPainterPath& path) {
    p.setPen(QPen(QColor(0, 0, 0, 220), 1)); p.drawPath(path);
    QPen w(QColor(255, 255, 255, 235), 1, Qt::CustomDashLine); w.setDashPattern({4, 4}); w.setDashOffset(antsOff_); p.setPen(w); p.drawPath(path); };
  if (!selPath_.isEmpty()) ants(M.map(selPath_));
  if (selDrag_) {
    QPainterPath d; const QRectF r = QRectF(sel0_, sel1_).normalized();
    if (selKind_ == "lasso") d.addPolygon(lasso_); else if (selKind_ == "ellipse") d.addEllipse(r); else d.addRect(r);
    ants(M.map(d));
  }
  if (polyOn_ && !poly_.isEmpty()) {                          // (the corners so far, a line on to the pointer)
    QPolygonF w = M.map(poly_); const QPointF cur = canvas_->mapFromGlobal(QCursor::pos()); QPainterPath d; d.addPolygon(w); d.lineTo(cur);
    ants(d); p.setPen(QPen(QColor(0, 0, 0), 1)); p.setBrush(QColor(148, 102, 190));
    for (int i = 0; i < w.size(); i++) p.drawRect(QRectF(w[i] - QPointF(3, 3), QSizeF(6, 6)));
    if (w.size() >= 3 && std::hypot(cur.x() - w.first().x(), cur.y() - w.first().y()) < 9) { p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor(255, 255, 255), 1.5)); p.drawEllipse(w.first(), 7, 7); }
  }
  if (shaping_) {                                             // (the shape being drawn, as it will be laid down)
    const QPainterPath sp = M.map(shapePath()); QColor c = s_->color; c.setAlphaF(float(std::clamp(shapeOp_, 0.05, 1.0)));
    const double wpx = std::max(1.0, shapeW_ * canvas_->zoom());
    if (shapeFilled_ && shapeKind_ != "line") { p.setPen(Qt::NoPen); p.setBrush(c); p.drawPath(sp); }
    else { QPen pe(c, wpx, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); p.setPen(pe); p.setBrush(Qt::NoBrush); p.drawPath(sp); }
  }
  p.restore();
}

// ================= the bucket and the wand: what matches the seed =================

std::vector<uchar> RefCanvas::matchAt(QPoint sd, int tol, bool contig, bool all, QRect& box) const {
  const int W = s_->doc.width(), H = s_->doc.height(); if (sd.x() < 0 || sd.y() < 0 || sd.x() >= W || sd.y() >= H) return {};
  const Layer* l = s_->doc.layer(s_->layer);
  QImage src = all || !l || l->kind != LayerKind::Paint || l->channel != Channel::Color ? s_->doc.compose(Channel::Color) : l->image;
  if (src.format() != QImage::Format_ARGB32) src = src.convertToFormat(QImage::Format_ARGB32);
  const QRgb seed = src.pixel(sd);
  auto same = [&](QRgb c) {
    if (qAlpha(seed) == 0 && qAlpha(c) == 0) return true;
    return std::max({std::abs(qRed(c) - qRed(seed)), std::abs(qGreen(c) - qGreen(seed)), std::abs(qBlue(c) - qBlue(seed)), std::abs(qAlpha(c) - qAlpha(seed))}) <= tol; };
  std::vector<uchar> m(size_t(W) * H, 0); int x0 = W, y0 = H, x1 = -1, y1 = -1;
  auto mark = [&](int x, int y) { m[size_t(y) * W + x] = 255; x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); };
  if (!contig) { for (int y = 0; y < H; y++) { const QRgb* r = reinterpret_cast<const QRgb*>(src.constScanLine(y)); for (int x = 0; x < W; x++) if (same(r[x])) mark(x, y); } }
  else {                                                      // (spans: each row filled out to its ends, the rows above and below pushed)
    std::vector<QPoint> st{sd};
    while (!st.empty()) {
      const QPoint q = st.back(); st.pop_back(); const int y = q.y(); const QRgb* r = reinterpret_cast<const QRgb*>(src.constScanLine(y)); uchar* row = m.data() + size_t(y) * W;
      if (row[q.x()] || !same(r[q.x()])) continue;
      int a = q.x(), b = q.x(); while (a > 0 && !row[a - 1] && same(r[a - 1])) a--; while (b < W - 1 && !row[b + 1] && same(r[b + 1])) b++;
      for (int x = a; x <= b; x++) mark(x, y);
      for (int ny : {y - 1, y + 1}) { if (ny < 0 || ny >= H) continue; const QRgb* rr = reinterpret_cast<const QRgb*>(src.constScanLine(ny)); const uchar* mr = m.data() + size_t(ny) * W; bool in = false;
        for (int x = a; x <= b; x++) { const bool ok = !mr[x] && same(rr[x]); if (ok && !in) st.push_back(QPoint(x, ny)); in = ok; } }
    }
  }
  if (x1 < 0) return {};
  box = QRect(QPoint(x0, y0), QPoint(x1, y1)); return m;
}

void RefCanvas::fillAt(const QPointF& t) {
  Layer* l = s_->active();
  if (!l || l->kind != LayerKind::Paint || l->channel != Channel::Color || l->image.isNull()) { ui::toast(tr("Select a colour paint layer to fill")); return; }
  if (l->locked) { ui::toast(tr("The layer is locked")); return; }
  const QPoint sd(int(std::floor(t.x())), int(std::floor(t.y())));
  if (hasSel() && (sd.x() < 0 || sd.y() < 0 || sd.x() >= selMask_.width() || sd.y() >= selMask_.height() || selMask_.constScanLine(sd.y())[sd.x()] == 0)) { ui::toast(tr("Click inside the selection")); return; }
  QRect box; std::vector<uchar> m = matchAt(sd, fillTol_, fillContig_, fillAll_, box); if (m.empty()) return;
  const int W = s_->doc.width(), H = s_->doc.height();
  { std::vector<uchar> g = m; const QRect b = box.adjusted(-1, -1, 1, 1).intersected(QRect(0, 0, W, H));   // (one pixel more round it: no light fringe left at a soft edge)
    for (int y = b.top(); y <= b.bottom(); y++) for (int x = b.left(); x <= b.right(); x++) { if (m[size_t(y) * W + x]) continue;
      for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { const int xx = x + dx, yy = y + dy; if (xx >= 0 && yy >= 0 && xx < W && yy < H && m[size_t(yy) * W + xx]) { g[size_t(y) * W + x] = 255; goto next; } }
      next:; }
    m.swap(g); box = b; }
  pushUndo(s_, l); addRecent(s_->color);
  const QColor c = s_->color; const float op = float(std::clamp(fillOp_, 0.0, 1.0));
  for (int y = box.top(); y <= box.bottom(); y++) {
    QRgb* r = reinterpret_cast<QRgb*>(l->image.scanLine(y)); const uchar* mr = m.data() + size_t(y) * W; const uchar* sm = hasSel() ? selMask_.constScanLine(y) : nullptr;
    for (int x = box.left(); x <= box.right(); x++) {
      float a = mr[x] / 255.f * op; if (sm) a *= sm[x] / 255.f; if (a <= 0) continue;
      const float da = qAlpha(r[x]) / 255.f, oa = a + da * (1 - a); if (oa <= 0) continue;
      auto mix = [&](int s, int d) { return int(std::lround((s * a + d * da * (1 - a)) / oa)); };
      r[x] = qRgba(mix(c.red(), qRed(r[x])), mix(c.green(), qGreen(r[x])), mix(c.blue(), qBlue(r[x])), int(std::lround(oa * 255)));
    }
  }
  emit s_->pixelsChanged(box);
}

// Alt+Backspace / Ctrl+Backspace: what is selected (nothing selected — the whole layer) laid over with a colour
void RefCanvas::fillSelection(const QColor& c) {
  Layer* l = s_->active();
  if (!l || l->kind != LayerKind::Paint || l->channel != Channel::Color || l->image.isNull()) { ui::toast(tr("Select a colour paint layer to fill")); return; }
  if (l->locked) { ui::toast(tr("The layer is locked")); return; }
  if (fl_.on) applyFloat();
  pushUndo(s_, l); addRecent(c);
  const QRect box = hasSel() ? selBox_.adjusted(-1, -1, 1, 1).intersected(l->image.rect()) : l->image.rect();
  for (int y = box.top(); y <= box.bottom(); y++) {
    QRgb* r = reinterpret_cast<QRgb*>(l->image.scanLine(y)); const uchar* sm = hasSel() ? selMask_.constScanLine(y) : nullptr;
    for (int x = box.left(); x <= box.right(); x++) {
      const float a = sm ? sm[x] / 255.f : 1.f; if (a <= 0) continue;
      const float da = qAlpha(r[x]) / 255.f, oa = a + da * (1 - a); if (oa <= 0) continue;
      auto mix = [&](int s, int d) { return int(std::lround((s * a + d * da * (1 - a)) / oa)); };
      r[x] = qRgba(mix(c.red(), qRed(r[x])), mix(c.green(), qGreen(r[x])), mix(c.blue(), qBlue(r[x])), int(std::lround(oa * 255)));
    }
  }
  emit s_->pixelsChanged(box);
  ui::toast(hasSel() ? tr("The selection filled") : tr("The layer filled (nothing selected)"));
}

// ================= shapes =================

QPainterPath RefCanvas::shapePath() const {
  QPointF a = sh0_, b = sh1_; const bool sq = shMods_ & Qt::ShiftModifier; QPainterPath p;
  if (shapeKind_ == "line") {
    if (sq) { const double d = std::hypot(b.x() - a.x(), b.y() - a.y()), ang = std::round(std::atan2(b.y() - a.y(), b.x() - a.x()) / (M_PI / 4)) * (M_PI / 4); b = a + QPointF(std::cos(ang), std::sin(ang)) * d; }
    p.moveTo(a); p.lineTo(b); return p;
  }
  if (sq) { const double s = std::max(std::abs(b.x() - a.x()), std::abs(b.y() - a.y())); b = a + QPointF(b.x() >= a.x() ? s : -s, b.y() >= a.y() ? s : -s); }
  const QRectF r = QRectF(a, b).normalized();
  if (shapeKind_ == "ellipse") p.addEllipse(r); else p.addRect(r);
  return p;
}

bool RefCanvas::shapeInput(int phase, const QPointF& t, Qt::KeyboardModifiers mods) {
  if (phase == 0) {
    Layer* l = s_->active();
    if (!l || l->kind != LayerKind::Paint || l->channel != Channel::Color || l->image.isNull()) { ui::toast(tr("Select a colour paint layer to draw on")); return true; }
    if (l->locked) { ui::toast(tr("The layer is locked")); return true; }
    shaping_ = true; sh0_ = sh1_ = t; shMods_ = mods; canvas_->update(); return true;
  }
  if (!shaping_) return true;
  sh1_ = t; shMods_ = mods;
  if (phase == 2) {
    shaping_ = false; Layer* l = s_->active(); const QPainterPath sp = shapePath();
    if (l && !l->locked && sp.boundingRect().width() + sp.boundingRect().height() > 1) {
      pushUndo(s_, l); addRecent(s_->color); const QImage before = l->image.copy();
      QColor c = s_->color; c.setAlphaF(float(std::clamp(shapeOp_, 0.0, 1.0)));
      { QPainter p(&l->image); p.setRenderHint(QPainter::Antialiasing);
        if (shapeFilled_ && shapeKind_ != "line") { p.setPen(Qt::NoPen); p.setBrush(c); } else { p.setPen(QPen(c, shapeW_, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush); }
        p.drawPath(sp); }
      const QRect r = sp.boundingRect().toAlignedRect().adjusted(-int(shapeW_) - 2, -int(shapeW_) - 2, int(shapeW_) + 2, int(shapeW_) + 2);
      if (hasSel()) clipToSel(l->image, before, r);
      emit s_->pixelsChanged(r);
    }
  }
  canvas_->update(); return true;
}

// ================= ✥ Move: all layers — the canvas moved in the view =================

// the plane shifted by d in the world (an ortho plane following the camera: in the camera's frame, now and where it goes back to on leaving)
void RefCanvas::shiftPlane(const Vec3& d) {
  RefPlane* p = plane(); if (!p) return;
  if (p->follows()) {
    const M4 T = m4Compose(d, Quat(), Vec3(1, 1, 1));
    auto moved = [&](const Camera& cam, const M4& loc) { const M4 F = refs::frame(*p, cam); return m4Mul(m4Mul(m4Inverse(F), m4Mul(T, F)), loc); };
    p->loc = moved(vp_->camera(), p->loc); if (hasLoc_) savedLoc_ = moved(hasCam_ ? savedCam_ : vp_->camera(), savedLoc_);
  } else p->pos = p->pos + d;
  offD_ = offD_ + d; vp_->refPlanesEdited(false); followView();
}

bool RefCanvas::moveAllInput(int phase, const QPointF& t) {
  const QPointF w = canvas_->texToWidget(t);
  if (phase == 0) {
    M4 M; Vec3 n; if (!frame(M, n, mvW_, mvH_)) return true;
    mvMap0_ = canvas_->mapping(); mvW0_ = w; mvM0_ = M; mvDone_ = Vec3(); return true;
  }
  bool ok = false; const QTransform inv = mvMap0_.inverted(&ok); if (!ok) return true;
  const QPointF d = inv.map(w) - inv.map(mvW0_);
  const Vec3 want = m4Apply(mvM0_, {d.x() / mvW_, -d.y() / mvH_, 0}) - m4Apply(mvM0_, {0, 0, 0});
  shiftPlane(want - mvDone_); mvDone_ = want;
  if (phase == 2) { vp_->refPlanesEdited(true); if (changed) changed(); }
  return true;
}

}  // namespace hm
