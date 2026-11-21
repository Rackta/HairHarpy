#include <array>
#include <map>
#include <set>
#include "app/PaintTools.h"
#include "app/PaintCanvas.h"
#include "app/PaintSession.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include "view/Picking.h"
#include <QFont>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hm {

using namespace paint;

namespace {
// a smooth line through the points (Catmull-Rom), a point every few units
std::vector<QPointF> spline(const std::vector<QPointF>& p, double step) {
  if (p.size() < 3) return p;
  std::vector<QPointF> o; const int n = int(p.size());
  for (int i = 0; i + 1 < n; i++) { const QPointF p0 = p[size_t(std::max(0, i - 1))], p1 = p[size_t(i)], p2 = p[size_t(i + 1)], p3 = p[size_t(std::min(n - 1, i + 2))];
    const int k = std::max(2, int(std::hypot(p2.x() - p1.x(), p2.y() - p1.y()) / step));
    for (int j = 0; j < k; j++) { const double t = double(j) / k, t2 = t * t, t3 = t2 * t;
      o.push_back(0.5 * ((2 * p1) + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (3 * p1 - p0 - 3 * p2 + p3) * t3)); } }
  o.push_back(p.back()); return o;
}
QPointF snap45(const QPointF& a, const QPointF& b) {
  const QPointF d = b - a; const double L = std::hypot(d.x(), d.y()); if (L < 1e-9) return b;
  const double ang = std::round(std::atan2(d.y(), d.x()) / (3.14159265358979 / 4)) * (3.14159265358979 / 4); return a + QPointF(std::cos(ang), std::sin(ang)) * L;
}
// the active layer to draw into (a paint layer with pixels, not locked), its picture as it was kept for Ctrl+Z
paint::Layer* layerToEdit(PaintSession* s, QImage* before) {
  Layer* l = s->active();
  if (!l || l->kind != LayerKind::Paint) { ui::toast(QObject::tr("Select a paint layer first")); return nullptr; }
  if (l->locked) { ui::toast(QObject::tr("The layer is locked")); return nullptr; }
  if (!l->fx.isEmpty()) { ui::toast(QObject::tr("An effect layer is made again from its source: bake it first (the Layers panel) to paint on it")); return nullptr; }
  if (s->editMask) { ui::toast(QObject::tr("The mask is being painted: this tool works on the layer itself — click the layer's picture in its row first")); return nullptr; }   // (the tool writes the picture, never the mask: refused rather than laid where the user does not look)
  if (l->image.isNull()) s->doc.ensurePixels(l->id);
  if (l->image.isNull()) return nullptr;
  *before = l->image; PaintSession::Undo u; u.layer = l->id; u.img = *before; s->pushUndo(std::move(u));
  return l;
}
}  // namespace

// ---- a filled shape ----
namespace {
bool isLinearLive(const paint::Layer* l) { return l && l->kind == paint::LayerKind::Fill && l->gradLive && l->liveKind != QLatin1String("spheres") && l->liveKind != QLatin1String("curve"); }
bool isCurveLive(const paint::Layer* l) { return l && l->kind == paint::LayerKind::Fill && l->gradLive && l->liveKind == QLatin1String("curve"); }
bool isSpheres(const paint::Layer* l) { return l && l->kind == paint::LayerKind::Fill && l->gradLive && l->liveKind == QLatin1String("spheres"); }
QColor inkOf(PaintSession* s, const paint::Layer& l) { QColor c = s->color; if (mapInfo(l.channel).gray) { const int g = qGray(c.rgb()); c = QColor(g, g, g); } c.setAlphaF(std::clamp(double(s->brush.opacity), 0.0, 1.0)); return c; }
// the polygons' texels as a coverage of the texture (255 — theirs, a texel past their edges too: no gap along a seam); box — their bounds
QImage facesCoverage(const std::vector<Viewport::PvProj>& faces, int W, int H, QRect* box) {
  QImage cov(W, H, QImage::Format_Grayscale8); cov.fill(0); QRectF bb;
  { QPainter p(&cov); p.setRenderHint(QPainter::Antialiasing, false); p.setPen(QPen(Qt::white, 2)); p.setBrush(Qt::white);
    for (const auto& f : faces) { QPolygonF poly; for (const QPointF& q : f.uv) poly << QPointF(q.x() * W, q.y() * H); p.drawPolygon(poly); bb = bb.united(poly.boundingRect()); } }
  if (box) *box = bb.toAlignedRect().adjusted(-2, -2, 2, 2).intersected(cov.rect());
  return cov;
}
}

PaintTools::PaintTools(PaintSession* s, Viewport* vp, PaintCanvas* cv) : s_(s), vp_(vp), cv_(cv) {
  vp_->paintToolInput = [this](int ph, const QPointF& m, Qt::KeyboardModifiers md) { return onView(ph, m, md); };
  vp_->paintToolKey = [this](int k) { return key(k); };
  vp_->paintToolOverlay = [this](std::vector<Viewport::Overlay>& out) {
    auto add = [&](const std::vector<QPointF>& pts, bool closed, const QColor& c = QColor(0xff, 0xff, 0xff)) { if (pts.size() < 2) return; std::vector<std::pair<double, double>> q; for (const QPointF& p : pts) q.push_back({p.x(), p.y()}); out.push_back({q, c, closed}); };
    const QString g = gesture();
    if (vdrag_ && (g == "rect" || g == "ellipse")) add(figure(va_, vb_, g), true);
    if (vdrag_ && g == "lasso") add(lasso_, true);
    if (vdrag_ && g == "fill" && fillGradient && !isSpheres(s_->active())) add({va_, vb_}, false);
    if (vdrag_ && g == "fill" && fillCurve) add(curveScr_, false);
    if (s_->tool == "fill" && isCurveLive(s_->active())) {          // (the active curve gradient's curve on the model: the start ringed)
      std::vector<QPointF> q; for (const auto& c : s_->active()->curve) if (const auto sp = vp_->paintAnchorScreen({Vec3(c[0], c[1], c[2]), {}, -1})) q.push_back(*sp);
      add(q, false, QColor(255, 255, 255, 190));
      if (!q.empty()) { std::vector<QPointF> r; for (int i = 0; i < 20; i++) { const double a = i * 6.283185307179586 / 20; r.push_back(q.front() + QPointF(std::cos(a), std::sin(a)) * 7); } add(r, true); } }
    if (isLinearLive(s_->active()) && (s_->tool == "fill" || s_->gradPick)) {   // (the live gradient's ends: a ring in each end's colour, the start's ring double; the line between)
      const Layer* l = s_->active(); const auto e1 = gradEndScreen(1), e2 = gradEndScreen(2);
      const QColor c1 = l->grad.empty() ? l->fill : l->grad.front().second, c2 = l->grad.empty() ? QColor(0, 0, 0) : l->grad.back().second;
      auto ring = [&](const QPointF& c, double r, const QColor& col) { std::vector<QPointF> q; for (int i = 0; i < 24; i++) { const double t = i * 6.283185307179586 / 24; q.push_back(c + QPointF(std::cos(t), std::sin(t)) * r); } add(q, true, col); };
      if (e1 && e2) add({*e1, *e2}, false, QColor(255, 255, 255, 170));
      if (e1) { ring(*e1, 9, QColor(255, 255, 255)); ring(*e1, 6, c1); ring(*e1, 4, c1); }
      if (e2) { ring(*e2, 9, QColor(255, 255, 255)); ring(*e2, 6, c2); } }
    if (s_->tool == "fill" && isSpheres(s_->active())) {          // (the colour spheres: each a ring where it lies, the picked one amber)
      const Layer* l = s_->active();
      for (size_t i = 0; i < l->spheres.size(); i++) { const auto& sp = l->spheres[i]; const Vec3 c(sp.c[0], sp.c[1], sp.c[2]);
        const auto sc = vp_->paintAnchorScreen({c, {}, -1}); if (!sc) continue; double rr = 0;
        for (const Vec3& ax : {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)}) if (const auto e = vp_->paintAnchorScreen({c + ax * double(sp.r), {}, -1})) rr = std::max(rr, QLineF(*sc, *e).length());
        if (rr < 2) continue; std::vector<QPointF> ring; for (int k = 0; k < 48; k++) { const double t = k * 6.283185307179586 / 48; ring.push_back(*sc + QPointF(std::cos(t), std::sin(t)) * rr); }
        const QColor rc = int(i) == s_->sphereSel ? QColor(0xff, 0xc0, 0x40) : QColor(0xff, 0xff, 0xff); add(ring, true, rc);
        const double ri = rr * std::clamp(double(sp.hard), 0.0, 1.0); if (ri > 2) { std::vector<QPointF> in; for (int k = 0; k < 32; k++) { const double t = k * 6.283185307179586 / 32; in.push_back(*sc + QPointF(std::cos(t), std::sin(t)) * ri); } add(in, true, QColor(rc.red(), rc.green(), rc.blue(), 120)); }
        add({*sc + QPointF(-4, 0), *sc + QPointF(4, 0)}, false, rc); add({*sc + QPointF(0, -4), *sc + QPointF(0, 4)}, false, rc); } }
    if (vdrag_ && g == "select") { if (selKind == "lasso") add(lasso_, true); else if (selKind == "rect") { const QRectF r = QRectF(va_, vb_).normalized(); add({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}, true); } }
    if (vdrag_ && g == "faces") for (const auto& [m, fs] : faces_) for (const auto& f : vp_->pvFaceUvs(m, std::vector<int>(fs.begin(), fs.end()), s_->doc.activeSet())) add(f.scr, true, QColor(0xff, 0xd0, 0x40));   // (the polygons passed over so far)
    if (g == "polyline") {                                       // (the line so far: its points where they are on the screen now — the camera may have turned since)
      if (anch_.empty() && vpts_.empty() && !laid_.empty()) { std::vector<QPointF> lp; for (const auto& a : laid_) if (const auto sp = vp_->paintAnchorScreen(a)) lp.push_back(*sp);   // (the line laid last: drag a point or click on it to change it)
        if (lp.size() >= 2) add(s_->lineSmooth ? spline(lp, 6) : lp, false, QColor(160, 170, 190)); for (const QPointF& c : lp) add({c + QPointF(-3, -3), c + QPointF(3, -3), c + QPointF(3, 3), c + QPointF(-3, 3)}, true, QColor(160, 170, 190)); }
      std::vector<QPointF> p; if (!anch_.empty()) { for (const auto& a : anch_) if (const auto sp = vp_->paintAnchorScreen(a)) p.push_back(*sp); } else p = vpts_;
      if (!p.empty()) { std::vector<QPointF> q = p; if (hoverOn_ && grab_ < 0) q.push_back(vhover_); add(s_->lineSmooth ? spline(q, 6) : q, false);
        for (const QPointF& c : p) add({c + QPointF(-3, -3), c + QPointF(3, -3), c + QPointF(3, 3), c + QPointF(-3, 3)}, true); } } };
  cv_->toolInput = [this](int ph, const QPointF& t, Qt::KeyboardModifiers md) { return onTex(ph, t, md); };
  cv_->drawOver = [this](QPainter& p, const QRectF&) {
    auto draw = [&](const std::vector<QPointF>& pts, bool closed) { if (pts.size() < 2) return; QPolygonF q; for (const QPointF& x : pts) q << cv_->texToWidget(x);
      p.setRenderHint(QPainter::Antialiasing); p.setBrush(Qt::NoBrush);
      for (auto [c, w] : {std::pair{QColor(0, 0, 0, 170), 3.0}, std::pair{QColor(255, 255, 255), 1.0}}) { p.setPen(QPen(c, w)); if (closed) p.drawPolygon(q); else p.drawPolyline(q); } };
    const QString g = gesture();
    if (tdrag_ && (g == "rect" || g == "ellipse")) draw(figure(ta_, tb_, g), true);
    if (tdrag_ && g == "lasso") draw(lasso_, true);
    if (g == "polyline" && !tpts_.empty()) { draw(s_->lineSmooth ? spline(tpts_, 8) : tpts_, false); p.setPen(QPen(Qt::white, 1)); for (const QPointF& c : tpts_) p.drawRect(QRectF(cv_->texToWidget(c) - QPointF(3, 3), QSizeF(6, 6))); }
    if (tdrag_ && g == "piece" && !piece_.on) draw(lasso_, true);
    if (piece_.on) {                                             // (the floating piece as it will lie, its frame and corners)
      const QTransform T = pieceXf(); p.save(); p.setRenderHint(QPainter::SmoothPixmapTransform); p.setTransform(T * cv_->mapping()); p.setOpacity(0.92); p.drawImage(0, 0, piece_.img); p.restore();
      const double w = piece_.img.width(), h = piece_.img.height(); std::vector<QPointF> fr; for (const QPointF& q : {QPointF(0, 0), QPointF(w, 0), QPointF(w, h), QPointF(0, h)}) fr.push_back(T.map(q)); draw(fr, true);
      p.setPen(QPen(Qt::black, 1)); p.setBrush(Qt::white); for (const QPointF& q : fr) p.drawRect(QRectF(cv_->texToWidget(q) - QPointF(4, 4), QSizeF(8, 8))); } };
}

// ---- Piece: a part of the layer lifted, moved by hand, laid again ----
QTransform PaintTools::pieceXf() const {                        // (the piece's own pixels → the texture)
  QTransform t; t.translate(piece_.c.x(), piece_.c.y()); t.rotate(piece_.ang); t.scale(piece_.sc * (piece_.fx ? -1 : 1), piece_.sc * (piece_.fy ? -1 : 1)); t.translate(-piece_.img.width() / 2.0, -piece_.img.height() / 2.0); return t;
}
void PaintTools::pieceDrop() { if (!piece_.on) return; piece_ = {}; cv_->update(); }
void PaintTools::pieceFlip(bool across) { if (!piece_.on) return; (across ? piece_.fx : piece_.fy) ^= true; cv_->update(); }
void PaintTools::pieceTurn(double deg) { if (!piece_.on) return; piece_.ang = std::remainder(piece_.ang + deg, 360.0); cv_->update(); }
void PaintTools::pieceApply() {
  if (!piece_.on) return; const Piece pc = piece_; const QTransform T = pieceXf(); piece_ = {};
  Layer* src = s_->doc.layer(pc.layer);
  if (pieceCut && src && !src->locked && src->image.format() == QImage::Format_ARGB32) {   // (where it was taken from: cleared — its own Ctrl+Z step)
    PaintSession::Undo u; u.layer = src->id; u.img = src->image; s_->pushUndo(std::move(u)); const QImage was = src->image;
    paint::applyRegion(src->image, was, pc.cov, QPoint(0, 0), pc.box, RegionOp::Erase, Qt::white, 1); if (cv_->clip) cv_->clip(src->image, was, pc.box); }
  if (pieceNewLayer && src) { s_->layer = s_->doc.addPaint(src->channel, src->name + QObject::tr(" (piece)"), src->id); emit s_->layersChanged(); emit s_->activeChanged(); }
  else s_->setLayer(pc.layer);
  QImage before; Layer* l = layerToEdit(s_, &before); if (!l) { cv_->update(); return; }
  if (l->image.format() != QImage::Format_ARGB32) { s_->undoStroke(); cv_->update(); return; }
  if (cv_->strokeBegan) cv_->strokeBegan();
  { QPainter p(&l->image); p.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing); p.setTransform(T); p.drawImage(0, 0, pc.img); }
  const QRect box = T.mapRect(QRectF(0, 0, pc.img.width(), pc.img.height())).toAlignedRect().adjusted(-2, -2, 2, 2).intersected(l->image.rect());
  if (cv_->clip) cv_->clip(l->image, before, box);
  emit s_->pixelsChanged({}); cv_->update();
}

// what the left button does now: a painting tool — the way it is laid (PaintSession::drawMode); another tool — itself
QString PaintTools::gesture() const {
  const QString& t = s_->tool;
  if (t == "brush" || t == "eraser" || t == "blur" || t == "smudge" || t == "recolor" || t == "stamp") return s_->drawMode;
  return t;
}
int PaintTools::linePoints() const { return int(std::max({anch_.size(), vpts_.size(), tpts_.size()})); }
// the line's width along it, of the brush's size: from lineStart at its beginning up to full, back down to lineEnd at its end — each
// end's passage takes lineTaper of the line's length, eased
double PaintTools::lineWidthAt(double t) const {
  const double s = std::clamp(double(s_->lineStart), 0.0, 1.0), e = std::clamp(double(s_->lineEnd), 0.0, 1.0), tp = std::clamp(double(s_->lineTaper), 0.01, 0.5);
  auto ease = [](double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3 - 2 * x); };
  double w = 1; if (t < tp) w = std::min(w, s + (1 - s) * ease(t / tp)); if (t > 1 - tp) w = std::min(w, e + (1 - e) * ease((1 - t) / tp));
  return std::max(0.03, w);
}

void PaintTools::setTool(const QString& t) { cancel(); s_->tool = t; cv_->setEraser(t == "eraser"); vp_->paintOverlayChanged(); cv_->update(); }
void PaintTools::cancel() { piece_ = {}; vdrag_ = tdrag_ = hoverOn_ = false; grab_ = -1; vpts_.clear(); tpts_.clear(); anch_.clear(); laid_.clear(); laidUndo_ = -1; laidSteps_ = 0; lasso_.clear(); faces_.clear(); vp_->paintOverlayChanged(); cv_->update(); }

std::vector<QPointF> PaintTools::figure(const QPointF& a, const QPointF& b, const QString& kind) const {
  const QRectF r = QRectF(a, b).normalized();
  if (kind == "ellipse") { std::vector<QPointF> o; const QPointF c = r.center(); const int n = 72; for (int i = 0; i <= n; i++) { const double t = 2 * 3.14159265358979 * i / n; o.push_back(c + QPointF(std::cos(t) * r.width() / 2, std::sin(t) * r.height() / 2)); } return o; }
  return {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(), r.topLeft()};
}

// ---- on the model (window px) ----
bool PaintTools::onView(int ph, const QPointF& m, Qt::KeyboardModifiers md) {
  const QString& t = s_->tool; const QString g = gesture(); const bool shift = md & Qt::ShiftModifier, ctrl = md & Qt::ControlModifier;
  if (s_->gradPick && ph <= 2) {                                 // (a live gradient's point waiting: the click puts it there, whatever the tool)
    if (ph == 2) { const int k = s_->gradPick; s_->gradPick = 0; if (setGradEnd(k, m)) ui::toast(k == 1 ? QObject::tr("The gradient's start is here") : QObject::tr("The gradient's end is here")); else ui::toast(QObject::tr("Click on the model to put the point")); emit s_->layersChanged(); }
    return true; }
  if (g == "stroke") return false;
  if (ph == 4) { if (g == "polyline" && linePoints() > 0) { vhover_ = m; hoverOn_ = true; vp_->paintOverlayChanged(); }
    vp_->pvHoverAt(m, t == "select" ? selKind : QString()); return false; }   // (Loop / Ring / Fill: what a click takes, shown under the pointer)
  if (g == "rect" || g == "ellipse") {                           // (a shape dragged: what the camera sees inside it takes the tool's effect — or its outline as a stroke)
    auto end = [&](const QPointF& p) { if (!shift) return p; const QPointF d = p - va_; const double k = std::max(std::abs(d.x()), std::abs(d.y())); return va_ + QPointF(d.x() < 0 ? -k : k, d.y() < 0 ? -k : k); };
    if (ph == 0) { va_ = vb_ = m; vdrag_ = true; inv_ = ctrl; }
    else if (ph == 1) vb_ = end(m);
    else if (ph == 2 && vdrag_) { vdrag_ = false; vb_ = end(m);
      if (std::hypot(vb_.x() - va_.x(), vb_.y() - va_.y()) > 3) { const auto fig = figure(va_, vb_, g);
        if (s_->shapeOutline) vp_->paintStrokeAlong(fig); else { QRect box; const QImage cov = viewCoverage(fig, &box); if (!cov.isNull()) applyCov(cov, box, inv_); } } }
    vp_->paintOverlayChanged(); return true;
  }
  if (g == "lasso") {                                            // (an outline drawn by hand: the same, for what is seen inside it)
    if (ph == 0) { lasso_ = {m}; vdrag_ = true; inv_ = ctrl; }
    else if (ph == 1 && vdrag_) { if (std::hypot(m.x() - lasso_.back().x(), m.y() - lasso_.back().y()) > 2) lasso_.push_back(m); }
    else if (ph == 2 && vdrag_) { vdrag_ = false; if (lasso_.size() >= 3) { QRect box; const QImage cov = viewCoverage(lasso_, &box); if (!cov.isNull()) applyCov(cov, box, inv_); } lasso_.clear(); }
    vp_->paintOverlayChanged(); return true;
  }
  if (g == "faces") {                                            // (whole polygons of the mesh: every one the pointer passes over — a click: the one under it)
    auto take = [&](const QPointF& p) { const auto h = vp_->pvFaceAt(p); if (h.mesh >= 0) faces_[h.mesh].insert(h.face); };
    if (ph == 0) { faces_.clear(); inv_ = ctrl; vdrag_ = true; va_ = m; take(m); }
    else if (ph == 1 && vdrag_) { const double L = std::hypot(m.x() - va_.x(), m.y() - va_.y()); const int n = std::max(1, int(L / 4)); for (int k = 1; k <= n; k++) take(va_ + (m - va_) * (double(k) / n)); va_ = m; }
    else if (ph == 2 && vdrag_) { vdrag_ = false; std::vector<Viewport::PvProj> all;
      for (const auto& [mesh, fs] : faces_) { const auto f = vp_->pvFaceUvs(mesh, std::vector<int>(fs.begin(), fs.end()), s_->doc.activeSet()); all.insert(all.end(), f.begin(), f.end()); }
      faces_.clear();
      if (all.empty()) ui::toast(QObject::tr("No polygon of the UV set painted under the stroke (or they are locked)"));
      else { s_->ensureDoc(); QRect box; const QImage cov = facesCoverage(all, s_->doc.width(), s_->doc.height(), &box); applyCov(cov, box, inv_); } }
    vp_->paintOverlayChanged(); return true;
  }
  if (g == "polyline") {
    if (!s_->drawFlat) {                                         // (the points lie on the surface: the camera may turn between two of them; a point can be dragged to another place)
      if (ph == 0) { va_ = m; grab_ = -1; grabMoved_ = false; inserted_ = false; double best = 9;
        grabLaid_ = anch_.empty() && vpts_.empty() && !laid_.empty();   // (nothing being made: the line laid last is the one edited)
        std::vector<Viewport::PaintAnchor>& L = grabLaid_ ? laid_ : anch_;
        for (size_t i = 0; i < L.size(); i++) if (const auto sp = vp_->paintAnchorScreen(L[i])) { const double d = std::hypot(sp->x() - m.x(), sp->y() - m.y()); if (d < best) { best = d; grab_ = int(i); } }
        if (grab_ < 0 && L.size() >= 2) {                        // (a click on the line between two points: a point put in there — the line bends through it)
          double bd = 6; int at = -1;
          for (size_t i = 0; i + 1 < L.size(); i++) { const auto a = vp_->paintAnchorScreen(L[i]), b = vp_->paintAnchorScreen(L[i + 1]); if (!a || !b) continue;
            const QPointF ab = *b - *a; const double l2 = ab.x() * ab.x() + ab.y() * ab.y(); if (l2 < 1) continue; const double k = std::clamp(((m.x() - a->x()) * ab.x() + (m.y() - a->y()) * ab.y()) / l2, 0.0, 1.0);
            if (k <= 0.02 || k >= 0.98) continue; const QPointF c = *a + ab * k; const double d = std::hypot(c.x() - m.x(), c.y() - m.y()); if (d < bd) { bd = d; at = int(i) + 1; } }
          if (at > 0) if (const auto a = vp_->paintAnchorAt(m)) { L.insert(L.begin() + at, *a); grab_ = at; inserted_ = true; } }
        if (grab_ < 0) { if (grabLaid_) { laid_.clear(); laidUndo_ = -1; laidSteps_ = 0; grabLaid_ = false; }   // (a click elsewhere: a new line begins, the laid one is let go)
          if (const auto a = vp_->paintAnchorAt(m)) { vpts_.clear(); tpts_.clear(); anch_.push_back(*a); hoverOn_ = false; } else ui::toast(QObject::tr("Click on the model, on a polygon of the UV set painted — the line's points lie on the surface")); } }
      else if (ph == 1 && grab_ >= 0 && (grabMoved_ || std::hypot(m.x() - va_.x(), m.y() - va_.y()) > 3)) { if (const auto a = vp_->paintAnchorAt(m)) { (grabLaid_ ? laid_ : anch_)[size_t(grab_)] = *a; grabMoved_ = true; } }
      else if (ph == 2) { if (grabLaid_ && grab_ >= 0 && (grabMoved_ || inserted_)) relay();   // (the laid line changed: taken back and laid again)
        else if (!grabLaid_ && grab_ >= 0 && !grabMoved_ && !inserted_ && grab_ == int(anch_.size()) - 1 && anch_.size() >= 2) layCurve(); grab_ = -1; grabLaid_ = false; }   // (a click on the last point lays the line)
    } else if (ph == 0) {                                        // (flat: the points lie on the screen, the line is carried onto the model from this view)
      if (!vpts_.empty() && std::hypot(m.x() - vpts_.back().x(), m.y() - vpts_.back().y()) < 7) layCurve(); else { tpts_.clear(); anch_.clear(); vpts_.push_back(m); hoverOn_ = false; } }
    vp_->paintOverlayChanged(); return true;
  }
  if (t == "select" && (selKind == "rect" || selKind == "lasso")) {   // (an outline dragged: the polygons seen inside it)
    if (ph == 0) { selOp_ = ctrl ? 2 : shift ? 1 : 0; va_ = vb_ = m; lasso_ = {m}; vdrag_ = true; }
    else if (ph == 1 && vdrag_ && !lasso_.empty()) { vb_ = m; if (std::hypot(m.x() - lasso_.back().x(), m.y() - lasso_.back().y()) > 2) lasso_.push_back(m); }   // (Esc mid-drag emptied it)
    else if (ph == 2 && vdrag_) { vdrag_ = false;
      if (std::hypot(m.x() - va_.x(), m.y() - va_.y()) < 4) vp_->pvSelPick(m, "face", selOp_);   // (a click: the polygon under it)
      else if (selKind == "rect") { const QRectF r = QRectF(va_, m).normalized(); vp_->pvSelArea(r, [r](const QPointF& p) { return r.contains(p); }, selOp_); }
      else { QPolygonF poly; for (const QPointF& p : lasso_) poly << p; vp_->pvSelArea(poly.boundingRect(), [poly](const QPointF& p) { return poly.containsPoint(p, Qt::OddEvenFill); }, selOp_); } }
    vp_->paintOverlayChanged(); return true;
  }
  if (t == "select") {
    if (ph == 0) { selOp_ = ctrl ? 2 : shift ? 1 : 0; vp_->pvSelPick(m, selKind, selOp_); if (selOp_ == 0) selOp_ = 1; }
    else if (ph == 1 && vp_->pvFaceAt(m).mesh >= 0) vp_->pvSelPick(m, selKind, selOp_);   // (a held button paints the selection)
    return true;
  }
  if (t == "toposym") { if (ph == 0) ui::toast(vp_->pvTsPick(m, ctrl)); return true; }
  if (t == "piece") { if (ph == 0) ui::toast(QObject::tr("A piece is outlined and moved on the texture — the 2D window in the left column (its ⤢ button opens it over the viewport)")); return true; }
  if (t == "fill" && fillCurve && !s_->freezePaint) {             // (Along a curve: drawn on the model, root → tip)
    auto take = [&](const QPointF& p) { if (const auto a = vp_->paintAnchorAt(p)) { curve3_.push_back(a->p); curveScr_.push_back(p); } };
    if (ph == 0) { curve3_.clear(); curveScr_.clear(); vdrag_ = true; take(m); }
    else if (ph == 1 && vdrag_) { if (curveScr_.empty() || std::hypot(m.x() - curveScr_.back().x(), m.y() - curveScr_.back().y()) > 5) take(m); }
    else if (ph == 2 && vdrag_) { vdrag_ = false; take(m); layCurveGradient(); curve3_.clear(); curveScr_.clear(); }
    vp_->paintOverlayChanged(); return true; }
  if (t == "fill") { if (ph == 0) { va_ = vb_ = m; vdrag_ = true; } else if (ph == 1) vb_ = m; else if (ph == 2 && vdrag_) { vdrag_ = false; fillAt(va_, m, ctrl); } vp_->paintOverlayChanged(); return true; }
  if (t == "text") {
    if (ph != 2) return true;
    const auto uv = vp_->paintUvAt(m); if (!uv) { ui::toast(QObject::tr("Click on the model, on a polygon of the UV set painted")); return true; }
    // (the screen's right and down there, in the texture: the text lies on the model as it is read on the screen — on a mirrored shell too)
    const double W = s_->doc.width(), H = s_->doc.height(), d = 6; QPointF ex(1, 0), ey(0, 1);
    const auto ux = vp_->paintUvAt(m + QPointF(d, 0)), uy = vp_->paintUvAt(m + QPointF(0, d));
    if (ux && uy) { ex = QPointF((ux->x() - uv->x()) * W / d, (ux->y() - uv->y()) * H / d); ey = QPointF((uy->x() - uv->x()) * W / d, (uy->y() - uv->y()) * H / d);
      const double lx = std::hypot(ex.x(), ex.y()), ly = std::hypot(ey.x(), ey.y()); if (lx < 1e-6 || ly < 1e-6 || lx > 400 || ly > 400) { ex = QPointF(1, 0); ey = QPointF(0, 1); } }
    text(QPointF(uv->x() * W, uv->y() * H), ex, ey); return true;
  }
  return false;
}
bool PaintTools::key(int k) {
  if (piece_.on) { if (k == Qt::Key_Return || k == Qt::Key_Enter) { pieceApply(); return true; } if (k == Qt::Key_Escape) { pieceDrop(); return true; } }
  if (gesture() != "polyline" || linePoints() == 0) { if (k == Qt::Key_Escape && (vdrag_ || tdrag_ || !laid_.empty())) { cancel(); return true; } return false; }   // (Esc: the laid line let go — no longer editable)
  if (k == Qt::Key_Return || k == Qt::Key_Enter) { layCurve(); return true; }
  if (k == Qt::Key_Escape) { cancel(); return true; }
  if (k == Qt::Key_Backspace) { grab_ = -1; /* (a point held by the mouse may be the one taken away) */ if (!anch_.empty()) anch_.pop_back(); else if (!vpts_.empty()) vpts_.pop_back(); else if (!tpts_.empty()) tpts_.pop_back(); vp_->paintOverlayChanged(); cv_->update(); return true; }
  return false;
}
// the laid line, changed: its strokes taken back while they are still the last steps, then laid again through its points as they are now
void PaintTools::relay() {
  if (laid_.size() < 2) return;
  if (laidUndo_ >= 0 && !s_->undo.empty() && s_->undo.back().id == laidUndo_) for (int k = 0; k < laidSteps_; k++) s_->undoStroke();
  else ui::toast(QObject::tr("Something was painted after the line — the old line stays, the changed one is laid over it"));
  anch_ = laid_; laid_.clear(); layCurve();
}

// the line's points so far laid as a stroke of the tool in hand (into the freeze mask while it is painted)
void PaintTools::layCurve() {
  grab_ = -1;                                                    // (the points move to the laid line: a point held by the mouse no longer indexes them)
  const bool prof = s_->lineStart < 0.995f || s_->lineEnd < 0.995f; const std::function<double(double)> width = prof ? std::function<double(double)>([this](double t) { return lineWidthAt(t); }) : std::function<double(double)>();
  if (anch_.size() >= 2) {
    std::vector<Viewport::PaintAnchor> p = anch_;
    if (s_->lineSmooth && anch_.size() >= 3) { p.clear(); const int n = int(anch_.size());   // (a curve through the points — Catmull-Rom — the normals carried along)
      for (int i = 0; i + 1 < n; i++) { const auto &a0 = anch_[size_t(std::max(0, i - 1))], &a1 = anch_[size_t(i)], &a2 = anch_[size_t(i + 1)], &a3 = anch_[size_t(std::min(n - 1, i + 2))];
        for (int j = 0; j < 8; j++) { const double u = j / 8.0, u2 = u * u, u3 = u2 * u; Viewport::PaintAnchor q = a1;
          q.p = ((a1.p * 2.0) + (a2.p - a0.p) * u + (a0.p * 2.0 - a1.p * 5.0 + a2.p * 4.0 - a3.p) * u2 + (a1.p * 3.0 - a0.p - a2.p * 3.0 + a3.p) * u3) * 0.5; q.n = a1.n * (1 - u) + a2.n * u; p.push_back(q); } }
      p.push_back(anch_.back()); }
    laid_ = anch_; anch_.clear(); hoverOn_ = false; vp_->paintOverlayChanged();
    const int before = s_->undo.empty() ? -1 : s_->undo.back().id; vp_->paintStrokeThrough(p, width);
    laidSteps_ = 0; for (auto it = s_->undo.rbegin(); it != s_->undo.rend() && it->id != before; ++it) laidSteps_++;   // (its steps: a line broken by a gap is several strokes)
    laidUndo_ = s_->undo.empty() ? -1 : s_->undo.back().id; }
  else if (vpts_.size() >= 2) { const std::vector<QPointF> p = s_->lineSmooth ? spline(vpts_, 4) : vpts_; vpts_.clear(); hoverOn_ = false; vp_->paintOverlayChanged(); vp_->paintStrokeAlong(p, width); }
  else if (tpts_.size() >= 2) { const std::vector<QPointF> p = s_->lineSmooth ? spline(tpts_, 8) : tpts_; tpts_.clear(); strokeTex(p); }
  else { if (linePoints() > 0) ui::toast(QObject::tr("A line needs two points or more")); }
  cv_->update();
}

// ---- on the texture (texture px) ----
void PaintTools::strokeTex(const std::vector<QPointF>& pts) {
  const double W = s_->doc.width(), H = s_->doc.height(); if (pts.size() < 2 || W <= 0 || H <= 0) return; bool down = false;
  const bool prof = s_->lineStart < 0.995f || s_->lineEnd < 0.995f; double total = 0, gone = 0; for (size_t i = 0; i + 1 < pts.size(); i++) total += std::hypot(pts[i + 1].x() - pts[i].x(), pts[i + 1].y() - pts[i].y());
  for (size_t i = 0; i + 1 < pts.size(); i++) { const QPointF a = pts[i], b = pts[i + 1]; const double L = std::hypot(b.x() - a.x(), b.y() - a.y()); const int n = std::max(1, int(std::ceil(L / 16.0)));   // (close points: the canvas takes a long jump for a UV seam)
    for (int k = i ? 1 : 0; k <= n; k++) { const QPointF p = a + (b - a) * (double(k) / n); cv_->strokeTex(down ? 1 : 0, QPointF(p.x() / W, p.y() / H), false, prof && total > 1e-9 ? float(lineWidthAt((gone + L * k / n) / total)) : 1.f); down = true; }
    gone += L; }
  cv_->strokeTex(2, {}, false);
}
bool PaintTools::onTex(int ph, const QPointF& tp, Qt::KeyboardModifiers md) {
  const QString& t = s_->tool; const QString g = gesture(); const bool shift = md & Qt::ShiftModifier, ctrl = md & Qt::ControlModifier;
  if (g == "stroke") return false;
  if (g == "rect" || g == "ellipse") {
    auto end = [&](const QPointF& p) { if (!shift) return p; const QPointF d = p - ta_; const double k = std::max(std::abs(d.x()), std::abs(d.y())); return ta_ + QPointF(d.x() < 0 ? -k : k, d.y() < 0 ? -k : k); };
    if (ph == 0) { ta_ = tb_ = tp; tdrag_ = true; inv_ = ctrl; }
    else if (ph == 1) tb_ = end(tp);
    else if (ph == 2 && tdrag_) { tdrag_ = false; tb_ = end(tp);
      if (std::hypot(tb_.x() - ta_.x(), tb_.y() - ta_.y()) > 1) { const auto fig = figure(ta_, tb_, g); if (s_->shapeOutline) strokeTex(fig); else { QRect box; const QImage cov = texCoverage(fig, &box); applyCov(cov, box, inv_); } } }
    return true;
  }
  if (g == "lasso") {
    if (ph == 0) { lasso_ = {tp}; tdrag_ = true; inv_ = ctrl; }
    else if (ph == 1 && tdrag_) { if (std::hypot(tp.x() - lasso_.back().x(), tp.y() - lasso_.back().y()) * cv_->zoom() > 2) lasso_.push_back(tp); }
    else if (ph == 2 && tdrag_) { tdrag_ = false; if (lasso_.size() >= 3) { QRect box; const QImage cov = texCoverage(lasso_, &box); applyCov(cov, box, inv_); } lasso_.clear(); }
    return true;
  }
  if (g == "piece") {
    if (!piece_.on) {                                            // (the piece outlined: what the active layer holds inside the lasso)
      if (ph == 0) { lasso_ = {tp}; tdrag_ = true; }
      else if (ph == 1 && tdrag_) { if (std::hypot(tp.x() - lasso_.back().x(), tp.y() - lasso_.back().y()) * cv_->zoom() > 2) lasso_.push_back(tp); }
      else if (ph == 2 && tdrag_) { tdrag_ = false;
        if (lasso_.size() >= 3) { Layer* l = s_->active();
          if (!l || l->kind != LayerKind::Paint || l->image.isNull() || l->image.format() != QImage::Format_ARGB32) ui::toast(QObject::tr("Select a colour paint layer first — the piece is taken from it"));
          else { QRect box; const QImage cov = texCoverage(lasso_, &box); QImage img = box.isEmpty() ? QImage() : l->image.copy(box); bool any = false;
            for (int y = 0; y < img.height(); y++) { QRgb* p = reinterpret_cast<QRgb*>(img.scanLine(y)); const uchar* c = cov.constScanLine(box.y() + y) + box.x(); for (int x = 0; x < img.width(); x++) { const int a = qAlpha(p[x]) * c[x] / 255; any |= a > 0; p[x] = qRgba(qRed(p[x]), qGreen(p[x]), qBlue(p[x]), a); } }
            if (!any) ui::toast(QObject::tr("The layer has nothing painted inside the outline"));
            else { piece_ = {}; piece_.on = true; piece_.img = img; piece_.cov = cov; piece_.box = box; piece_.layer = l->id; piece_.c = QRectF(box).center();
              ui::toast(QObject::tr("The piece floats: drag it — move · drag outside it — turn · a corner — scale · Enter — lay it · Esc — drop")); } } }
        lasso_.clear(); }
      cv_->update(); return true; }
    // (the piece floats: grabbed inside — moved; by a corner — scaled; outside — turned)
    const double w = piece_.img.width(), h = piece_.img.height();
    if (ph == 0) { const QPointF lp = pieceXf().inverted().map(tp); piece_.grab = tp; piece_.c0 = piece_.c; piece_.ang0 = piece_.ang; piece_.sc0 = piece_.sc;
      const double tol = 9 / std::max(1e-6, cv_->zoom() * piece_.sc); bool corner = false; for (const QPointF& q : {QPointF(0, 0), QPointF(w, 0), QPointF(w, h), QPointF(0, h)}) corner |= std::hypot(lp.x() - q.x(), lp.y() - q.y()) < tol;
      piece_.mode = corner ? 3 : (lp.x() >= 0 && lp.x() <= w && lp.y() >= 0 && lp.y() <= h) ? 1 : 2; }
    else if (ph == 1 && piece_.mode == 1) piece_.c = piece_.c0 + (tp - piece_.grab);
    else if (ph == 1 && piece_.mode == 2) { const double a0 = std::atan2(piece_.grab.y() - piece_.c0.y(), piece_.grab.x() - piece_.c0.x()), a1 = std::atan2(tp.y() - piece_.c0.y(), tp.x() - piece_.c0.x());
      piece_.ang = piece_.ang0 + (a1 - a0) * 180 / 3.14159265358979; if (shift) piece_.ang = std::round(piece_.ang / 15) * 15; }
    else if (ph == 1 && piece_.mode == 3) { const double d0 = std::hypot(piece_.grab.x() - piece_.c0.x(), piece_.grab.y() - piece_.c0.y()), d1 = std::hypot(tp.x() - piece_.c0.x(), tp.y() - piece_.c0.y()); if (d0 > 1e-6) piece_.sc = std::clamp(piece_.sc0 * d1 / d0, 0.02, 50.0); }
    else if (ph == 2) piece_.mode = 0;
    cv_->update(); return true;
  }
  if (g == "polyline") {
    if (ph == 0) { const double close = 7 / std::max(1e-6, cv_->zoom()); if (!tpts_.empty() && std::hypot(tp.x() - tpts_.back().x(), tp.y() - tpts_.back().y()) < close) layCurve(); else { vpts_.clear(); anch_.clear(); tpts_.push_back(tp); vp_->paintOverlayChanged(); } }
    return true;
  }
  if (t == "text") { if (ph == 2) text(tp, QPointF(1, 0), QPointF(0, 1)); return true; }
  if (ph == 0) ui::toast(g == "faces" ? QObject::tr("Whole polygons are painted on the model — the texture has none to pick") : t == "fill" ? QObject::tr("Fill works on the model: click a polygon there") : t == "select" ? QObject::tr("Polygons are selected on the model") : QObject::tr("The red and the blue polygon are marked on the model"));
  return true;
}

// ---- a region: its coverage in the texture, and the tool's effect laid over it at once ----
// an outline on the texture itself
QImage PaintTools::texCoverage(const std::vector<QPointF>& outline, QRect* box) {
  s_->ensureDoc(); const int W = s_->doc.width(), H = s_->doc.height(); if (W <= 0 || H <= 0) return {};
  QImage cov(W, H, QImage::Format_Grayscale8); cov.fill(0); QPolygonF q; for (const QPointF& x : outline) q << x;
  { QPainter p(&cov); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(Qt::white); p.drawPolygon(q); }
  if (box) *box = q.boundingRect().toAlignedRect().adjusted(-1, -1, 1, 1).intersected(cov.rect());
  return cov;
}
// an outline on the screen: every texel of the polygons the camera sees there (with PaintSession::drawThrough — of the ones behind
// them, too) whose place on the screen lies inside the outline
QImage PaintTools::viewCoverage(const std::vector<QPointF>& outline, QRect* box) {
  s_->ensureDoc(); const int W = s_->doc.width(), H = s_->doc.height(); if (W <= 0 || H <= 0) return {};
  QPolygonF shapeP; for (const QPointF& x : outline) shapeP << x; const QRectF sb = shapeP.boundingRect().adjusted(-60, -60, 60, 60);
  const auto faces = vp_->pvFacesIn(sb, s_->doc.activeSet(), true, false, s_->drawThrough);
  if (faces.empty()) { ui::toast(QObject::tr("Nothing of the UV set painted is seen inside the shape")); return {}; }
  QImage cov(W, H, QImage::Format_Grayscale8); cov.fill(0); int n = 0, bx0 = W, by0 = H, bx1 = -1, by1 = -1;
  for (const auto& f : faces) for (size_t i = 1; i + 1 < f.uv.size(); i++) {
    const QPointF d0(f.uv[0].x() * W, f.uv[0].y() * H), d1(f.uv[i].x() * W, f.uv[i].y() * H), d2(f.uv[i + 1].x() * W, f.uv[i + 1].y() * H), s0 = f.scr[0], s1 = f.scr[i], s2 = f.scr[i + 1];
    const double den = (d1.y() - d2.y()) * (d0.x() - d2.x()) + (d2.x() - d1.x()) * (d0.y() - d2.y()); if (std::abs(den) < 1e-9) continue;
    const double longest = std::max({std::hypot(d1.x() - d0.x(), d1.y() - d0.y()), std::hypot(d2.x() - d1.x(), d2.y() - d1.y()), std::hypot(d0.x() - d2.x(), d0.y() - d2.y())}), slack = std::min(0.5, longest / std::abs(den));
    const int x0 = std::max(0, int(std::floor(std::min({d0.x(), d1.x(), d2.x()}))) - 1), x1 = std::min(W - 1, int(std::ceil(std::max({d0.x(), d1.x(), d2.x()}))) + 1);
    const int y0 = std::max(0, int(std::floor(std::min({d0.y(), d1.y(), d2.y()}))) - 1), y1 = std::min(H - 1, int(std::ceil(std::max({d0.y(), d1.y(), d2.y()}))) + 1);
    for (int y = y0; y <= y1; y++) { uchar* row = cov.scanLine(y);
      for (int x = x0; x <= x1; x++) { if (row[x]) continue; const double px = x + 0.5, py = y + 0.5;
        const double w0 = ((d1.y() - d2.y()) * (px - d2.x()) + (d2.x() - d1.x()) * (py - d2.y())) / den, w1 = ((d2.y() - d0.y()) * (px - d2.x()) + (d0.x() - d2.x()) * (py - d2.y())) / den, w2 = 1 - w0 - w1;
        if (w0 < -slack || w1 < -slack || w2 < -slack) continue;
        if (!shapeP.containsPoint(s0 * w0 + s1 * w1 + s2 * w2, Qt::OddEvenFill)) continue;
        row[x] = 255; n++; bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); } } }
  if (!n) { ui::toast(QObject::tr("Nothing of the UV set painted is seen inside the shape")); return {}; }
  if (box) *box = QRect(QPoint(bx0, by0), QPoint(bx1, by1));
  return cov;
}
// the tool in hand over a coverage: the brush paints it with the colour at its opacity (inv — Ctrl: erases), the eraser erases, Blur
// softens, Recolor changes the colours; while the freeze mask is painted — the region is frozen (inv / the eraser: freed)
void PaintTools::applyCov(const QImage& cov, const QRect& box, bool inv) {
  if (cov.isNull() || box.isEmpty()) return; const QString& t = s_->tool;
  if (s_->freezePaint) { s_->freezeAdd(cov.copy(box), box.topLeft(), inv != (t == "eraser")); return; }
  if (t == "smudge") { ui::toast(QObject::tr("Smudge pulls the colour along a stroke — it has nothing to do over a region: pick Stroke or Line")); return; }
  if (t == "stamp" && s_->stampMode != 2) { ui::toast(QObject::tr("The Stamp's copy goes along a stroke — over a shape only the Patch works")); return; }
  if (t == "stamp") {                                          // (the patch over a shape: all of it filled from round it at once)
    QImage before; Layer* l = layerToEdit(s_, &before); if (!l) return;
    if (l->image.format() != QImage::Format_ARGB32) { ui::toast(QObject::tr("The Stamp works on a colour layer — not on a mask, the bump or the flow")); s_->undoStroke(); return; }
    if (cv_->strokeBegan) cv_->strokeBegan();
    const int pad = std::clamp(std::max(box.width(), box.height()) / 2, 16, 512);
    if (!paint::healRegion(l->image, before, cov, box, pad, s_->islands ? s_->islands() : nullptr)) ui::toast(QObject::tr("Nothing round the shape to fill it from — the whole island is inside it"));
    if (cv_->clip) cv_->clip(l->image, before, box);
    emit s_->pixelsChanged(box); return; }
  QImage before; Layer* l = layerToEdit(s_, &before); if (!l) return;
  if (l->image.format() != QImage::Format_ARGB32) { ui::toast(QObject::tr("A region is filled in a colour or a mask map's layer — not in the bump or the flow")); s_->undoStroke(); return; }
  if (cv_->strokeBegan) cv_->strokeBegan();
  const float k = std::clamp(s_->brush.opacity, 0.f, 1.f); RegionOp op = inv ? RegionOp::Erase : RegionOp::Paint;
  if (t == "eraser") op = inv ? RegionOp::Paint : RegionOp::Erase; else if (t == "blur") op = RegionOp::Blur; else if (t == "recolor") op = s_->recolorOp;
  paint::applyRegion(l->image, before, cov, QPoint(0, 0), box, op, inkOf(s_, *l), op == RegionOp::Paint ? 1.f : k);   // (Paint: the opacity is the ink's alpha already)
  if (l->lockAlpha) for (int y = box.top(); y <= box.bottom(); y++) { QRgb* row = reinterpret_cast<QRgb*>(l->image.scanLine(y)); const QRgb* was = reinterpret_cast<const QRgb*>(before.constScanLine(y)); for (int x = box.left(); x <= box.right(); x++) { const int a0 = qAlpha(was[x]); row[x] = a0 ? qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]), a0) : was[x]; } }   // (the layer's transparency kept)
  if (cv_->clip) cv_->clip(l->image, before, box);
  emit s_->pixelsChanged(box);
}

// Fill: the polygon clicked (the part it belongs to, its polygroup, the whole object — or the whole layer) takes the colour at the
// brush's opacity; Gradient: from the brush's colour where the drag began to the second colour where it ended, as seen on the screen
std::optional<QPointF> PaintTools::gradEndScreen(int k) const {
  const Layer* l = s_->active(); if (!isLinearLive(l)) return std::nullopt; const float* p = k == 1 ? l->g0 : l->g1;
  return vp_->paintAnchorScreen({Vec3(p[0], p[1], p[2]), {}, -1});
}
bool PaintTools::setGradEnd(int k, const QPointF& m) {
  Layer* l = s_->active(); if (!isLinearLive(l)) return false; float* p = k == 1 ? l->g0 : l->g1;
  std::optional<Vec3> q; if (const auto a = vp_->paintAnchorAt(m)) q = a->p;
  else { const Ray r = rayAt(vp_->camera(), m.x(), m.y(), vp_->paneW(), vp_->paneH()); q = planeHit(r, Vec3(p[0], p[1], p[2]), r.d * -1.0); }
  if (!q) return false;
  for (int i = 0; i < 3; i++) p[i] = float((*q)[i]);
  s_->edits++; emit s_->pixelsChanged({}); vp_->paintOverlayChanged(); return true;
}

void PaintTools::spheresAt(const QPointF& a, const QPointF& b, bool inv) {
  Layer* l = s_->active(); if (!isSpheres(l)) return;
  const auto A = vp_->paintAnchorAt(a); if (!A) { ui::toast(QObject::tr("Colour spheres: click on the model")); return; }
  auto inside = [&](const Vec3& p) { int best = -1; double bd = 1; for (size_t i = 0; i < l->spheres.size(); i++) { const auto& sp = l->spheres[i];
      const double d = (p - Vec3(sp.c[0], sp.c[1], sp.c[2])).length() / std::max(1e-9f, sp.r); if (d < bd) { bd = d; best = int(i); } } return best; };
  const int hit = inside(A->p); const bool drag = QLineF(a, b).length() > 4;
  if (inv) { if (hit < 0) { ui::toast(QObject::tr("No sphere here to remove")); return; } l->spheres.erase(l->spheres.begin() + hit); s_->sphereSel = std::min(s_->sphereSel, int(l->spheres.size()) - 1); }
  else if (drag && hit >= 0) { const auto B = vp_->paintAnchorAt(b); if (!B) { ui::toast(QObject::tr("End the drag on the model: the sphere goes where it ends")); return; }
    for (int k = 0; k < 3; k++) l->spheres[size_t(hit)].c[k] = float(B->p[k]); s_->sphereSel = hit; }
  else if (!drag && hit >= 0 && hit != s_->sphereSel) s_->sphereSel = hit;                      // (a click into another sphere: it is picked)
  else { Layer::Sphere sp; const Layer::Sphere* like = s_->sphereSel >= 0 && s_->sphereSel < int(l->spheres.size()) ? &l->spheres[size_t(s_->sphereSel)] : nullptr;
    sp.r = like ? like->r : float(0.08 * vp_->pvSceneSize()); sp.hard = like ? like->hard : 0.3f; sp.col = QColor(s_->color.red(), s_->color.green(), s_->color.blue());
    for (int k = 0; k < 3; k++) sp.c[k] = float(A->p[k]); l->spheres.push_back(sp); s_->sphereSel = int(l->spheres.size()) - 1; }
  s_->edits++; vp_->paintOverlayChanged(); emit s_->layersChanged(); emit s_->pixelsChanged({});
}

// the curve drawn: smoothed and spaced evenly; a curve gradient layer made over the selected strands (none selected — the strands the
// curve passes over), or the active curve gradient aimed along it
void PaintTools::layCurveGradient() {
  if (curve3_.size() < 2) { ui::toast(QObject::tr("Draw the curve along the strand on the model, from the root to the tip")); return; }
  std::vector<Vec3> c = curve3_;
  for (int pass = 0; pass < 2; pass++) { std::vector<Vec3> o = c; for (size_t i = 1; i + 1 < c.size(); i++) o[i] = (c[i - 1] + c[i] * 2.0 + c[i + 1]) * 0.25; c = o; }
  double L = 0; for (size_t i = 1; i < c.size(); i++) L += (c[i] - c[i - 1]).length(); if (L < 1e-9) { ui::toast(QObject::tr("The curve is too short")); return; }
  std::vector<std::array<float, 3>> out; const int n = std::clamp(int(c.size()), 2, 48); double acc = 0; size_t j = 1;
  for (int k = 0; k < n; k++) { const double want = L * k / (n - 1);
    while (j + 1 < c.size() && acc + (c[j] - c[j - 1]).length() < want) { acc += (c[j] - c[j - 1]).length(); j++; }
    const double seg = std::max(1e-12, (c[j] - c[j - 1]).length()); const double f = std::clamp((want - acc) / seg, 0.0, 1.0); const Vec3 p = c[j - 1] + (c[j] - c[j - 1]) * f;
    out.push_back({float(p.x), float(p.y), float(p.z)}); }
  if (Layer* g = s_->active(); isCurveLive(g)) { g->curve = out; s_->edits++; emit s_->layersChanged(); emit s_->pixelsChanged({}); ui::toast(QObject::tr("The curve gradient runs along the new curve")); return; }
  const Channel ch = s_->modeChannel(); if (mapInfo(ch).storage != Storage::Colour) { ui::toast(QObject::tr("A curve gradient belongs to a colour or a mask map")); return; }
  s_->ensureDoc(); const int W = s_->doc.width(), H = s_->doc.height(); if (W <= 0 || H <= 0) return;
  std::vector<Viewport::PvProj> faces;                          // (where it lies: the selected polygons — or the strands the curve passed over)
  if (!vp_->pvSel.empty()) { for (const auto& [mesh, fs] : vp_->pvSel) { const auto f = vp_->pvFaceUvs(mesh, std::vector<int>(fs.begin(), fs.end()), s_->doc.activeSet()); faces.insert(faces.end(), f.begin(), f.end()); } }
  else { std::map<int, std::set<int>> parts; for (const QPointF& p : curveScr_) { const auto h = vp_->pvFaceAt(p); if (h.mesh < 0) continue; for (int f : vp_->pvFacesOfKind(h, "part")) parts[h.mesh].insert(f); }
    for (const auto& [mesh, fs] : parts) { const auto f = vp_->pvFaceUvs(mesh, std::vector<int>(fs.begin(), fs.end()), s_->doc.activeSet()); faces.insert(faces.end(), f.begin(), f.end()); } }
  if (faces.empty()) { ui::toast(QObject::tr("No strand of the UV set painted there: select the strands (or draw over them)")); return; }
  QRect box; const QImage cov = facesCoverage(faces, W, H, &box);
  QColor c1 = s_->color, c2 = s_->color2; if (mapInfo(ch).gray) { const int a = qGray(c1.rgb()), b = qGray(c2.rgb()); c1 = QColor(a, a, a); c2 = QColor(b, b, b); }
  const int id = s_->doc.addFill(ch, c1, s_->doc.freeName("Strand gradient"), s_->layer);
  if (Layer* f = s_->doc.layer(id)) { f->gradLive = true; f->liveKind = "curve"; f->curve = out; f->grad = {{0.f, QColor(c1.red(), c1.green(), c1.blue())}, {1.f, QColor(c2.red(), c2.green(), c2.blue())}};
    f->mask = cov; f->maskOn = true; f->opacity = std::clamp(s_->brush.opacity, 0.f, 1.f); if (!s_->doc.activeSet().isEmpty()) f->onlySets = QStringList{s_->doc.activeSet()}; }
  s_->layer = id; s_->edits++; emit s_->layersChanged(); emit s_->activeChanged(); emit s_->pixelsChanged({});
  ui::toast(QObject::tr("A strand gradient: the colour → the second colour along the curve. Its stops, blend and opacity — the Layers panel; draw again — a new curve"));
}
void PaintTools::fillAll() {
  const Layer* l = s_->active(); if (!l || (l->kind != LayerKind::Paint && !s_->freezePaint)) { ui::toast(QObject::tr("Alt+Backspace fills a paint layer: pick one")); return; }
  const QString k = fillKind; const bool g = fillGradient, sp = fillSpheres, al = fillAsLayer, cu = fillCurve; const float so = fillSoft;
  fillKind = "layer"; fillGradient = fillSpheres = fillAsLayer = fillCurve = false; fillSoft = 0;
  fillAt({}, {}, false);
  fillKind = k; fillGradient = g; fillSpheres = sp; fillAsLayer = al; fillCurve = cu; fillSoft = so; live_ = {};
  ui::toast(QObject::tr("Filled wherever it is not frozen"));
}
void PaintTools::fillAt(const QPointF& a, const QPointF& b, bool inv) {
  live_ = {};
  if (fillSpheres && !s_->freezePaint && !isSpheres(s_->active()) && !inv) {   // (the freeze mask painted: no spheres — the click freezes)             // (the Spheres variant on a layer of another kind: a spheres layer made over it first)
    if (mapInfo(s_->modeChannel()).storage != Storage::Colour) { ui::toast(QObject::tr("Colour spheres belong to a colour or a mask map")); return; }
    s_->ensureDoc(); const int id = s_->doc.addFill(s_->modeChannel(), s_->color, s_->doc.freeName("Spheres"), s_->layer);
    if (Layer* f = s_->doc.layer(id)) { f->gradLive = true; f->liveKind = "spheres"; if (!s_->doc.activeSet().isEmpty()) f->onlySets = QStringList{s_->doc.activeSet()}; }
    s_->layer = id; s_->sphereSel = -1; emit s_->layersChanged(); emit s_->activeChanged(); }
  if (!s_->freezePaint && isSpheres(s_->active())) { spheresAt(a, b, inv); return; }
  // (a drag that begins on one of the active live gradient's ends: that end alone goes where the drag ends)
  if (!inv && isLinearLive(s_->active()) && QLineF(a, b).length() > 3) for (int k = 1; k <= 2; k++) if (const auto e = gradEndScreen(k); e && QLineF(*e, a).length() < 12) {
    if (setGradEnd(k, b)) emit s_->layersChanged(); return; }
  if (fillKind == "island" || fillKind == "contour" || fillKind == "colour" || s_->freezePaint || !fillGradient || inv) {   // (a patch of texels, polygons in one colour, or the mask itself: through a coverage — an erase (Ctrl) always, the Gradient variant too)
    s_->ensureDoc(); const int W = s_->doc.width(), H = s_->doc.height(); if (W <= 0 || H <= 0) return; QImage cov; QRect box;
    if (fillKind == "layer") { cov = QImage(W, H, QImage::Format_Grayscale8); cov.fill(255); box = cov.rect(); }
    else if (fillKind == "colour") {                              // (the texels of a colour like the clicked one: joined to it, or all over the layer — as Photoshop's bucket)
      const auto uv = vp_->paintUvAt(a); if (!uv) { ui::toast(QObject::tr("Click on the model, on a polygon of the UV set painted")); return; }
      const QPoint p(std::clamp(int(uv->x() * W), 0, W - 1), std::clamp(int(uv->y() * H), 0, H - 1));
      const Layer* al = s_->active(); QImage src = fillAllLayers || !al || al->kind != LayerKind::Paint ? s_->doc.compose(s_->modeChannel()) : al->image;
      if (src.isNull()) { src = QImage(W, H, QImage::Format_ARGB32); src.fill(Qt::transparent); }
      src = src.convertToFormat(QImage::Format_ARGB32); if (src.size() != QSize(W, H)) src = src.scaled(W, H);
      const paint::IslandMap* im = s_->islands ? s_->islands() : nullptr; const double kx = im && !im->ids.isNull() ? double(im->ids.width()) / W : 1, ky = im && !im->ids.isNull() ? double(im->ids.height()) / H : 1;
      auto inIsland = [&](int x, int y) { return !im || im->ids.isNull() || im->at(int(x * kx), int(y * ky)) != 0; };
      const int tol = int(std::lround(fillTol * 255));
      cov = QImage(W, H, QImage::Format_Grayscale8); cov.fill(0); int x0 = W, y0 = H, x1 = -1, y1 = -1;
      auto mark = [&](int x, int y) { cov.scanLine(y)[x] = 255; x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); };
      auto floodFrom = [&](const QPoint& p) {
        const QRgb c0 = reinterpret_cast<const QRgb*>(src.constScanLine(p.y()))[p.x()];
        auto like = [&](int x, int y) { const QRgb c = reinterpret_cast<const QRgb*>(src.constScanLine(y))[x];
          return std::abs(qRed(c) - qRed(c0)) <= tol && std::abs(qGreen(c) - qGreen(c0)) <= tol && std::abs(qBlue(c) - qBlue(c0)) <= tol && std::abs(qAlpha(c) - qAlpha(c0)) <= tol; };
        if (fillContig) { if (!inIsland(p.x(), p.y())) return false; std::vector<QPoint> st{p};
          while (!st.empty()) { const QPoint q = st.back(); st.pop_back(); if (cov.constScanLine(q.y())[q.x()] || !like(q.x(), q.y()) || !inIsland(q.x(), q.y())) continue; mark(q.x(), q.y());
            if (q.x() > 0) st.push_back({q.x() - 1, q.y()}); if (q.x() + 1 < W) st.push_back({q.x() + 1, q.y()}); if (q.y() > 0) st.push_back({q.x(), q.y() - 1}); if (q.y() + 1 < H) st.push_back({q.x(), q.y() + 1}); } }
        else for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (inIsland(x, y) && like(x, y)) mark(x, y);
        return true; };
      if (!floodFrom(p)) { ui::toast(QObject::tr("Nothing to fill there: outside the UV islands")); return; }
      for (const QPointF& mu : vp_->paintMirrorsUvAt(a)) floodFrom(QPoint(std::clamp(int(mu.x() * W), 0, W - 1), std::clamp(int(mu.y() * H), 0, H - 1)));   // (Symmetry: the mirrored side filled from its own colour)
      if (x1 < 0) { ui::toast(QObject::tr("Nothing to fill there")); return; }
      box = QRect(QPoint(x0, y0), QPoint(x1, y1)); }
    else if (fillKind == "island" || fillKind == "contour") {
      const auto uv = vp_->paintUvAt(a); if (!uv) { ui::toast(QObject::tr("Click on the model, on a polygon of the UV set painted")); return; }
      const QPoint p(std::clamp(int(uv->x() * W), 0, W - 1), std::clamp(int(uv->y() * H), 0, H - 1));
      if (fillKind == "contour" && s_->freezePaint) {              // (the closed contour of the mask itself: its inside frozen — Ctrl: the frozen patch freed)
        if (!s_->freezeFillAt(QPointF(p), inv)) ui::toast(inv ? QObject::tr("Nothing frozen under the click to free") : QObject::tr("The click is on a frozen texel — click inside the contour, on what is still free")); return; }
      const paint::IslandMap* im = s_->islands ? s_->islands() : nullptr;
      QImage none; const QImage* lines = fillKind == "contour" ? s_->freezeOf(false) : nullptr;
      if (!lines) { none = QImage(W, H, QImage::Format_Grayscale8); none.fill(0); lines = &none; }
      cov = paint::floodFree(*lines, p, 128, im, 2, &box);
      if (cov.isNull()) { ui::toast(QObject::tr("Nothing to fill there: a frozen texel, or outside the UV islands")); return; }
      for (const QPointF& mu : vp_->paintMirrorsUvAt(a)) {          // (Symmetry: the mirrored island / contour too)
        QRect b2; const QImage c2 = paint::floodFree(*lines, QPoint(std::clamp(int(mu.x() * W), 0, W - 1), std::clamp(int(mu.y() * H), 0, H - 1)), 128, im, 2, &b2); if (c2.isNull()) continue;
        for (int y = b2.top(); y <= b2.bottom(); y++) { const uchar* q = c2.constScanLine(y); uchar* o = cov.scanLine(y); for (int x = b2.left(); x <= b2.right(); x++) o[x] = std::max(o[x], q[x]); }
        box = box.united(b2); } }
    else { const auto h = vp_->pvFaceAt(a); if (h.mesh < 0) { ui::toast(QObject::tr("Click a polygon of the model")); return; }
      auto faces = vp_->pvFaceUvs(h.mesh, vp_->pvFacesOfKind(h, fillKind), s_->doc.activeSet());
      for (const QPointF& mu : vp_->paintMirrorsUvAt(a)) { const auto h2 = vp_->pvFaceAtUv(mu); if (h2.mesh < 0 || (h2.mesh == h.mesh && h2.face == h.face)) continue;   // (Symmetry: the mirrored figure filled with it — the author, 18.11)
        const auto f2 = vp_->pvFaceUvs(h2.mesh, vp_->pvFacesOfKind(h2, fillKind), s_->doc.activeSet()); faces.insert(faces.end(), f2.begin(), f2.end()); }
      if (faces.empty()) { ui::toast(QObject::tr("Nothing to fill there: not in the UV set painted, or locked")); return; }
      cov = facesCoverage(faces, W, H, &box); }
    if (fillSoft > 0.25f && fillKind != "layer") {                 // (the edge's staircase smoothed — the edge stays crisp — kept on its islands)
      const int pad = int(std::ceil(fillSoft)) + 2; const QRect pb = box.adjusted(-pad, -pad, pad, pad).intersected(QRect(0, 0, W, H));
      const QImage soft = paint::maskEdge(cov.copy(pb), paint::EdgeOp::Smooth, fillSoft, s_->islands ? s_->islands() : nullptr, pb.topLeft(), QSize(W, H));
      QImage full(W, H, QImage::Format_Grayscale8); full.fill(0); for (int y = 0; y < soft.height(); y++) std::memcpy(full.scanLine(pb.top() + y) + pb.left(), soft.constScanLine(y), size_t(soft.width()));
      cov = full; box = pb; }
    if (s_->freezePaint) { s_->freezeAdd(cov.copy(box), box.topLeft(), inv); return; }
    if (fillAsLayer && !inv) {                                   // (a Fill layer over the active one: one flat colour through a mask of what was clicked — changed any time later)
      const Layer* base = s_->active(); const Channel ch = base && base->kind != LayerKind::Group ? base->channel : s_->modeChannel();
      if (mapInfo(ch).storage != Storage::Colour) { ui::toast(QObject::tr("A fill layer of one colour belongs to a colour or a mask map")); return; }
      QColor c = s_->color; if (mapInfo(ch).gray) { const int g = qGray(c.rgb()); c = QColor(g, g, g); }
      const int id = s_->doc.addFill(ch, c, {}, s_->layer);
      if (Layer* f = s_->doc.layer(id)) { f->mask = cov; f->maskOn = true; f->opacity = std::clamp(s_->brush.opacity, 0.f, 1.f); f->name = s_->doc.smartName(id);
        if (!s_->doc.activeSet().isEmpty()) f->onlySets = QStringList{s_->doc.activeSet()}; }   // (its mask is this set's: unmasked in the others it would cover them whole)
      s_->layer = id; emit s_->layersChanged(); emit s_->activeChanged(); emit s_->pixelsChanged({});
      ui::toast(QObject::tr("A fill layer over what was clicked — its colour: the Layers panel, any time")); return; }
    QImage before; Layer* l = layerToEdit(s_, &before); if (!l) return;
    if (l->image.format() != QImage::Format_ARGB32) { ui::toast(QObject::tr("Fill goes into a colour or a mask map's layer — not into the bump or the flow")); s_->undoStroke(); return; }
    if (cv_->strokeBegan) cv_->strokeBegan();
    const QColor ink = inkOf(s_, *l);
    paint::applyRegion(l->image, before, cov, QPoint(0, 0), box, inv ? RegionOp::Erase : RegionOp::Paint, ink, 1);
    if (l->lockAlpha) for (int y = box.top(); y <= box.bottom(); y++) { QRgb* row = reinterpret_cast<QRgb*>(l->image.scanLine(y)); const QRgb* was = reinterpret_cast<const QRgb*>(before.constScanLine(y)); for (int x = box.left(); x <= box.right(); x++) { const int a0 = qAlpha(was[x]); row[x] = a0 ? qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]), a0) : was[x]; } }
    if (cv_->clip) cv_->clip(l->image, before, box);
    if (!inv && !s_->undo.empty()) live_ = {l->id, s_->undo.back().id, cov, box, ink};   // (it stays live: the next colour picked recolours it)
    emit s_->pixelsChanged(box); return;
  }
  // (a live gradient: the drag's two ends taken on the surface — a new Fill layer, or the active live gradient aimed again)
  if (!inv && fillGradient) { Layer* g = s_->active(); const bool reaim = g && g->kind == LayerKind::Fill && g->gradLive && g->liveKind != QLatin1String("spheres");
    if (reaim || fillAsLayer) {
      auto at3 = [&](const QPointF& m) -> std::optional<Vec3> { if (const auto k = vp_->paintAnchorAt(m)) return k->p; return std::nullopt; };
      const auto A = at3(a); std::optional<Vec3> B = at3(b);
      if (A && !B) { const Ray r = rayAt(vp_->camera(), b.x(), b.y(), vp_->paneW(), vp_->paneH()); B = planeHit(r, *A, r.d * -1.0); }   // (the end off the model: on the plane through the start that faces the camera — the 3D-Coat author set it at the shoulders)
      if (!A || !B || (*B - *A).length() < 1e-9) { ui::toast(QObject::tr("A live gradient: begin the drag on the model")); return; }
      if (reaim) { for (int k = 0; k < 3; k++) { g->g0[k] = float((*A)[k]); g->g1[k] = float((*B)[k]); } emit s_->pixelsChanged({}); emit s_->layersChanged(); vp_->paintOverlayChanged(); ui::toast(QObject::tr("The live gradient aimed again: %1").arg(g->name)); return; }
      const Layer* base = s_->active(); const Channel ch = base && base->kind != LayerKind::Group ? base->channel : s_->modeChannel();
      if (mapInfo(ch).storage != Storage::Colour) { ui::toast(QObject::tr("A live gradient belongs to a colour or a mask map")); return; }
      QImage cov;
      if (fillKind != "layer") { const auto h = vp_->pvFaceAt(a); if (h.mesh < 0) { ui::toast(QObject::tr("Begin the drag on the model")); return; }
        const auto fs = vp_->pvFaceUvs(h.mesh, vp_->pvFacesOfKind(h, fillKind), s_->doc.activeSet());
        if (fs.empty()) { ui::toast(QObject::tr("Nothing to fill there: not in the UV set painted, or locked")); return; }
        s_->ensureDoc(); QRect box; cov = facesCoverage(fs, s_->doc.width(), s_->doc.height(), &box); }
      s_->ensureDoc(); QColor c1 = s_->color, c2 = s_->color2; if (mapInfo(ch).gray) { c1 = QColor(qGray(c1.rgb()), qGray(c1.rgb()), qGray(c1.rgb())); c2 = QColor(qGray(c2.rgb()), qGray(c2.rgb()), qGray(c2.rgb())); }
      const int id = s_->doc.addFill(ch, c1, s_->doc.freeName("Gradient"), s_->layer);
      if (Layer* f = s_->doc.layer(id)) { f->gradLive = true; f->grad = {{0.f, QColor(c1.red(), c1.green(), c1.blue())}, {1.f, QColor(c2.red(), c2.green(), c2.blue())}};
        for (int k = 0; k < 3; k++) { f->g0[k] = float((*A)[k]); f->g1[k] = float((*B)[k]); }
        if (!cov.isNull()) { f->mask = cov; f->maskOn = true; }
        if (!s_->doc.activeSet().isEmpty()) f->onlySets = QStringList{s_->doc.activeSet()};
        f->opacity = std::clamp(s_->brush.opacity, 0.f, 1.f); }
      s_->layer = id; emit s_->layersChanged(); emit s_->activeChanged(); emit s_->pixelsChanged({});
      ui::toast(QObject::tr("A live gradient layer: its colours, blend and opacity — the Layers panel, any time; drag again with this tool to move its ends")); return; } }
  std::vector<Viewport::PvProj> faces;
  if (fillKind != "layer") { const auto h = vp_->pvFaceAt(a); if (h.mesh < 0) { ui::toast(QObject::tr("Click a polygon of the model")); return; }
    faces = vp_->pvFaceUvs(h.mesh, vp_->pvFacesOfKind(h, fillKind), s_->doc.activeSet());
    if (faces.empty()) { ui::toast(QObject::tr("Nothing to fill there: not in the UV set painted, or locked")); return; } }
  QImage before; Layer* l = layerToEdit(s_, &before); if (!l) return;
  if (l->image.format() != QImage::Format_ARGB32) { ui::toast(QObject::tr("Fill goes into a colour or a mask map's layer — not into the bump or the flow")); s_->undoStroke(); return; }
  if (cv_->strokeBegan) cv_->strokeBegan();
  QImage& img = l->image; const int W = img.width(), H = img.height(); const QColor c1 = inkOf(s_, *l); QColor c2 = s_->color2; if (mapInfo(l->channel).gray) { const int g = qGray(c2.rgb()); c2 = QColor(g, g, g); }
  const int ia = c1.alpha(); const QPointF ab = b - a; const double L2 = ab.x() * ab.x() + ab.y() * ab.y(); const bool grad = fillGradient && L2 > 16 && fillKind != "layer";
  if (fillKind == "layer") { QPainter p(&img); p.fillRect(img.rect(), c1); }
  else for (const auto& f : faces) for (size_t i = 1; i + 1 < f.uv.size(); i++) {
    const QPointF d0(f.uv[0].x() * W, f.uv[0].y() * H), d1(f.uv[i].x() * W, f.uv[i].y() * H), d2(f.uv[i + 1].x() * W, f.uv[i + 1].y() * H), s0 = f.scr[0], s1 = f.scr[i], s2 = f.scr[i + 1];
    const double den = (d1.y() - d2.y()) * (d0.x() - d2.x()) + (d2.x() - d1.x()) * (d0.y() - d2.y()); if (std::abs(den) < 1e-9) continue;
    const double longest = std::max({std::hypot(d1.x() - d0.x(), d1.y() - d0.y()), std::hypot(d2.x() - d1.x(), d2.y() - d1.y()), std::hypot(d0.x() - d2.x(), d0.y() - d2.y())}), slack = std::min(0.5, 2 * longest / std::abs(den));
    const int x0 = std::max(0, int(std::floor(std::min({d0.x(), d1.x(), d2.x()}))) - 2), x1 = std::min(W - 1, int(std::ceil(std::max({d0.x(), d1.x(), d2.x()}))) + 2);
    const int y0 = std::max(0, int(std::floor(std::min({d0.y(), d1.y(), d2.y()}))) - 2), y1 = std::min(H - 1, int(std::ceil(std::max({d0.y(), d1.y(), d2.y()}))) + 2);
    for (int y = y0; y <= y1; y++) { QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y)); const QRgb* was = reinterpret_cast<const QRgb*>(before.constScanLine(y));
      for (int x = x0; x <= x1; x++) { const double px = x + 0.5, py = y + 0.5;
        const double w0 = ((d1.y() - d2.y()) * (px - d2.x()) + (d2.x() - d1.x()) * (py - d2.y())) / den, w1 = ((d2.y() - d0.y()) * (px - d2.x()) + (d0.x() - d2.x()) * (py - d2.y())) / den, w2 = 1 - w0 - w1;
        if (w0 < -slack || w1 < -slack || w2 < -slack) continue;
        int r = c1.red(), g = c1.green(), bl = c1.blue();
        if (grad) { const QPointF sp = s0 * w0 + s1 * w1 + s2 * w2; const double t = std::clamp(((sp.x() - a.x()) * ab.x() + (sp.y() - a.y()) * ab.y()) / L2, 0.0, 1.0); r = int(r + (c2.red() - r) * t); g = int(g + (c2.green() - g) * t); bl = int(bl + (c2.blue() - bl) * t); }
        const QRgb bq = was[x]; const int ba = qAlpha(bq), oa = ia + ba * (255 - ia) / 255; if (oa <= 0) continue;
        row[x] = qRgba((r * ia + qRed(bq) * ba * (255 - ia) / 255) / oa, (g * ia + qGreen(bq) * ba * (255 - ia) / 255) / oa, (bl * ia + qBlue(bq) * ba * (255 - ia) / 255) / oa, oa); } } }
  if (l->lockAlpha) for (int y = 0; y < H; y++) { QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y)); const QRgb* was = reinterpret_cast<const QRgb*>(before.constScanLine(y)); for (int x = 0; x < W; x++) { const int a0 = qAlpha(was[x]); row[x] = a0 ? qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]), a0) : was[x]; } }   // (the layer's transparency kept)
  if (cv_->clip) cv_->clip(img, before, img.rect());
  emit s_->pixelsChanged({});
}

// the last fill takes the colour the brush has now: the layer put back as it was under it, filled again
void PaintTools::relive() {
  if (live_.layer < 0) return;
  if (s_->tool != "fill" || s_->layer != live_.layer || s_->freezePaint || s_->undo.empty() || s_->undo.back().id != live_.undoId || s_->undo.back().set != s_->doc.activeSet()) { live_ = {}; return; }   // (something else was done since — or another UV set is shown: it is no longer the last)
  Layer* l = s_->active(); PaintSession::Undo* u = s_->undoById(live_.undoId);
  if (!l || !u || u->img.isNull() || u->img.size() != l->image.size() || u->img.format() != l->image.format()) { live_ = {}; return; }
  const QColor ink = inkOf(s_, *l); if (ink == live_.ink) return; live_.ink = ink; const QRect box = live_.box;
  for (int y = box.top(); y <= box.bottom(); y++) std::memcpy(l->image.scanLine(y) + size_t(box.left()) * 4, u->img.constScanLine(y) + size_t(box.left()) * 4, size_t(box.width()) * 4);
  paint::applyRegion(l->image, u->img, live_.cov, QPoint(0, 0), box, RegionOp::Paint, ink, 1);
  if (l->lockAlpha) for (int y = box.top(); y <= box.bottom(); y++) { QRgb* row = reinterpret_cast<QRgb*>(l->image.scanLine(y)); const QRgb* was = reinterpret_cast<const QRgb*>(u->img.constScanLine(y)); for (int x = box.left(); x <= box.right(); x++) { const int a0 = qAlpha(was[x]); row[x] = a0 ? qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]), a0) : was[x]; } }
  if (cv_->clip) cv_->clip(l->image, u->img, box);
  emit s_->pixelsChanged(box);
}

// the text: typed, then drawn into the active layer with the brush's colour — ex / ey: a step right / down of the reader, in texture px
void PaintTools::text(const QPointF& at, const QPointF& ex, const QPointF& ey) {
  const QString s = askText ? askText(QObject::tr("Text")) : QString(); if (s.isEmpty()) return;
  QImage before; Layer* l = layerToEdit(s_, &before); if (!l) return;
  if (l->image.format() != QImage::Format_ARGB32) { ui::toast(QObject::tr("The text is drawn into a colour or a mask map's layer — not into the bump or the flow")); s_->undoStroke(); return; }
  if (cv_->strokeBegan) cv_->strokeBegan();
  QColor c = s_->color; if (mapInfo(l->channel).gray) { const int g = qGray(c.rgb()); c = QColor(g, g, g); } c.setAlphaF(std::clamp(double(s_->brush.opacity), 0.0, 1.0));
  { QPainter p(&l->image); p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    p.setTransform(QTransform(ex.x(), ex.y(), ey.x(), ey.y(), at.x(), at.y()));
    QFont f = textFont.isEmpty() ? QFont() : QFont(textFont); f.setPixelSize(std::max(4, textSize)); QPainterPath path; path.addText(0, 0, f, s);   // (as an outline: it scales cleanly whatever the texture's size)
    p.fillPath(path, c); }
  if (cv_->clip) cv_->clip(l->image, before, l->image.rect());     // (the locked, hidden and frozen texels as they were)
  emit s_->pixelsChanged({});
}

// ---- the topological symmetry: the layer copied from one side onto the other ----
QString PaintTools::topoCopy(int dir) {
  QString err; const auto prs = vp_->pvTsPairs(s_->doc.activeSet(), &err); if (prs.empty()) return err;
  const QString odd = vp_->pvTsOdd_ ? QObject::tr(" — the two sides differ in many places: check that the marked polygons lie on the middle line (Ctrl+Z takes the copy back)") : QString();
  // (the freeze mask painted: the copy is the mask's alone; else the layer's — and, with topoMask, the mask's with it)
  const bool maskOnly = s_->freezePaint; QImage layerWas; Layer* l = nullptr;
  if (!maskOnly) { l = layerToEdit(s_, &layerWas); if (!l) return QObject::tr("No layer to copy in"); if (cv_->strokeBegan) cv_->strokeBegan(); }
  auto carry = [&](QImage& img, const QImage& src) {
  const int W = img.width(), H = img.height(), bpp = img.depth() / 8; const QImage::Format fmt = img.format();
  auto sample = [&](double x, double y, uchar* out) {                // (the picture as it was, between its texels)
    x = std::clamp(x - 0.5, 0.0, W - 1.0); y = std::clamp(y - 0.5, 0.0, H - 1.0); const int x0 = int(x), y0 = int(y), x1 = std::min(W - 1, x0 + 1), y1 = std::min(H - 1, y0 + 1); const double fx = x - x0, fy = y - y0;
    const uchar *r0 = src.constScanLine(y0), *r1 = src.constScanLine(y1);
    if (fmt == QImage::Format_ARGB32) { const QRgb p[4] = {reinterpret_cast<const QRgb*>(r0)[x0], reinterpret_cast<const QRgb*>(r0)[x1], reinterpret_cast<const QRgb*>(r1)[x0], reinterpret_cast<const QRgb*>(r1)[x1]};
      const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy}; double a = 0, r = 0, g = 0, b = 0;
      for (int i = 0; i < 4; i++) { const double ai = qAlpha(p[i]) * w[i]; a += ai; r += qRed(p[i]) * ai; g += qGreen(p[i]) * ai; b += qBlue(p[i]) * ai; }
      const QRgb o = a > 1e-6 ? qRgba(int(r / a + 0.5), int(g / a + 0.5), int(b / a + 0.5), int(a + 0.5)) : 0; std::memcpy(out, &o, 4); }
    else if (fmt == QImage::Format_Grayscale16) { const quint16* a0 = reinterpret_cast<const quint16*>(r0); const quint16* a1 = reinterpret_cast<const quint16*>(r1);
      const quint16 o = quint16(a0[x0] * (1 - fx) * (1 - fy) + a0[x1] * fx * (1 - fy) + a1[x0] * (1 - fx) * fy + a1[x1] * fx * fy + 0.5); std::memcpy(out, &o, 2); }
    else if (fmt == QImage::Format_Grayscale8) *out = uchar(r0[x0] * (1 - fx) * (1 - fy) + r0[x1] * fx * (1 - fy) + r1[x0] * (1 - fx) * fy + r1[x1] * fx * fy + 0.5);   // (the freeze mask)
    else std::memcpy(out, (fy < 0.5 ? r0 : r1) + (fx < 0.5 ? x0 : x1) * bpp, size_t(bpp)); };
  auto raster = [&](const std::vector<QPointF>& D, const std::vector<QPointF>& S) {   // (D's texels take what S holds, corner for corner)
    for (size_t i = 1; i + 1 < D.size() && i + 1 < S.size(); i++) {
      const QPointF d0(D[0].x() * W, D[0].y() * H), d1(D[i].x() * W, D[i].y() * H), d2(D[i + 1].x() * W, D[i + 1].y() * H), s0(S[0].x() * W, S[0].y() * H), s1(S[i].x() * W, S[i].y() * H), s2(S[i + 1].x() * W, S[i + 1].y() * H);
      const double den = (d1.y() - d2.y()) * (d0.x() - d2.x()) + (d2.x() - d1.x()) * (d0.y() - d2.y()); if (std::abs(den) < 1e-9) continue;
      const double longest = std::max({std::hypot(d1.x() - d0.x(), d1.y() - d0.y()), std::hypot(d2.x() - d1.x(), d2.y() - d1.y()), std::hypot(d0.x() - d2.x(), d0.y() - d2.y())});
      const double slack = std::min(0.5, longest / std::abs(den));   // (a texel just outside the triangle is still its own: no gap along the edges)
      const int x0 = std::max(0, int(std::floor(std::min({d0.x(), d1.x(), d2.x()}))) - 1), x1 = std::min(W - 1, int(std::ceil(std::max({d0.x(), d1.x(), d2.x()}))) + 1);
      const int y0 = std::max(0, int(std::floor(std::min({d0.y(), d1.y(), d2.y()}))) - 1), y1 = std::min(H - 1, int(std::ceil(std::max({d0.y(), d1.y(), d2.y()}))) + 1);
      for (int y = y0; y <= y1; y++) { uchar* row = img.scanLine(y);
        for (int x = x0; x <= x1; x++) { const double px = x + 0.5, py = y + 0.5;
          const double w0 = ((d1.y() - d2.y()) * (px - d2.x()) + (d2.x() - d1.x()) * (py - d2.y())) / den, w1 = ((d2.y() - d0.y()) * (px - d2.x()) + (d0.x() - d2.x()) * (py - d2.y())) / den, w2 = 1 - w0 - w1;
          if (w0 < -slack || w1 < -slack || w2 < -slack) continue;
          sample(s0.x() * w0 + s1.x() * w1 + s2.x() * w2, s0.y() * w0 + s1.y() * w1 + s2.y() * w2, row + x * bpp); } } } };
  for (const auto& p : prs) { if (dir == 0 || dir == 2) raster(p.blue, p.red); if (dir == 1 || dir == 2) raster(p.red, p.blue); } };
  if (l) { carry(l->image, layerWas); if (cv_->clip) cv_->clip(l->image, layerWas, l->image.rect()); }
  if ((maskOnly || topoMask) && s_->freezeAny()) { s_->freezeStep(); if (QImage* fz = s_->freezeOf(false)) { const QImage was = *fz; carry(*fz, was); } }   // (its own Ctrl+Z step)
  else if (maskOnly) return QObject::tr("Nothing is frozen: there is no mask to copy across");
  emit s_->pixelsChanged({});
  return (dir == 0 ? QObject::tr("Red copied onto blue: %1 pairs of polygons").arg(prs.size()) : dir == 1 ? QObject::tr("Blue copied onto red: %1 pairs of polygons").arg(prs.size()) : QObject::tr("The two sides swapped: %1 pairs of polygons").arg(prs.size())) + odd;
}

}  // namespace hm
