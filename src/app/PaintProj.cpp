#include "app/PaintProj.h"
#include "app/PaintCanvas.h"
#include "app/PaintSession.h"
#include "paint/Psd.h"
#include <QFile>
#include "ui/Pie.h"
#include <QJsonArray>
#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <climits>
#include <limits>
#include <thread>

namespace hm {

using namespace paint;

// the stencil shown over the viewport: a see-through window the mouse goes through
class StencilOver : public QWidget {
public:
  StencilOver(QWidget* parent, PaintProj* p) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint), p_(p) {
    setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setAttribute(Qt::WA_TransparentForMouseEvents); }
protected:
  void paintEvent(QPaintEvent*) override {
    if (p_->stencil_.isNull()) return; QPainter g(this); g.setRenderHint(QPainter::SmoothPixmapTransform); g.setOpacity(0.32);
    const QImage& im = p_->stencil_; g.translate(width() / 2.0 + p_->stX * width() / 2.0, height() / 2.0 + p_->stY * height() / 2.0); g.rotate(p_->stAngle); g.scale(p_->stScale, p_->stScale);
    if (p_->stTile) { const int n = int(std::ceil(std::hypot(width(), height()) / (std::min(im.width(), im.height()) * std::max(0.05, p_->stScale)) / 2)) + 1; for (int y = -n; y <= n; y++) for (int x = -n; x <= n; x++) g.drawImage(QPointF(x * im.width() - im.width() / 2.0, y * im.height() - im.height() / 2.0), im); }
    else g.drawImage(QPointF(-im.width() / 2.0, -im.height() / 2.0), im);
  }
private:
  PaintProj* p_;
};

namespace {
QImage grayOf(const QString& file) {                                // a picture as a mask: its lightness (a see-through one: its alpha too)
  QImage im(file); if (im.isNull()) return {}; im = im.convertToFormat(QImage::Format_ARGB32); QImage g(im.size(), QImage::Format_Grayscale8);
  for (int y = 0; y < im.height(); y++) { const QRgb* p = reinterpret_cast<const QRgb*>(im.constScanLine(y)); uchar* q = g.scanLine(y); for (int x = 0; x < im.width(); x++) q[x] = uchar(qGray(p[x]) * qAlpha(p[x]) / 255); }
  return g;
}
float sample8(const QImage& g, double x, double y) {
  x = std::clamp(x - 0.5, 0.0, g.width() - 1.0); y = std::clamp(y - 0.5, 0.0, g.height() - 1.0); const int x0 = int(x), y0 = int(y), x1 = std::min(g.width() - 1, x0 + 1), y1 = std::min(g.height() - 1, y0 + 1); const double fx = x - x0, fy = y - y0;
  const uchar *a = g.constScanLine(y0), *b = g.constScanLine(y1);
  return float((a[x0] * (1 - fx) + a[x1] * fx) * (1 - fy) + (b[x0] * (1 - fx) + b[x1] * fx) * fy) / 255.f;
}
}  // namespace

PaintProj::PaintProj(PaintSession* s, Viewport* vp, PaintCanvas* cv, QWidget* vpBox) : s_(s), vp_(vp), cv_(cv), box_(vpBox) {
  over_ = new StencilOver(vpBox->window(), this); over_->hide();
  flushT_ = new QTimer(over_); flushT_->setSingleShot(true); QObject::connect(flushT_, &QTimer::timeout, over_, [this] { flush(); });
  auto* t = new QTimer(over_); t->setInterval(300); QObject::connect(t, &QTimer::timeout, over_, [this] { sync(); }); t->start();
}
// What the stroke changed is shown - the locked, hidden and frozen texels put back first, by each set's own masks, then the
// picture's owner told where it changed: the canvas composes that part, the model takes the new picture. Not after every move of
// the pointer: once the moves waiting in the queue are laid (a timer of no delay), so a fast hand is not held back by the showing
void PaintProj::flush() {
  if (pend_.empty()) return; const QString act = s_->doc.activeSet(); const std::map<QString, TileMask> pend = std::move(pend_); pend_.clear();
  for (const auto& [name, tm] : pend) { QImage* img = s_->doc.pixelsIn(layer_, name, false); if (!img) continue; const auto st = sets_.find(name);
    for (const QRect& td : tm.runs()) {                          // (only the tiles painted: a stroke over several locks touches islands all over the texture)
      if (st != sets_.end() && st->second.before.size() == img->size()) { if (name == act) { if (cv_->clip) cv_->clip(*img, st->second.before, td); } else if (clipSet) clipSet(name, *img, st->second.before, td); }
      if (name == act) emit s_->pixelsChanged(td); else emit s_->setPixels(name, td); } }
}
void PaintProj::sync() {
  const bool want = !stencil_.isNull() && vp_->room() == "paint" && box_->isVisible() && box_->window()->isActiveWindow();
  if (!want) { if (over_->isVisible()) over_->hide(); return; }
  const QRect r(box_->mapToGlobal(QPoint(0, 0)), box_->size()); if (over_->geometry() != r) over_->setGeometry(r);
  if (!over_->isVisible()) over_->show(); over_->update();
}
bool PaintProj::setStencil(const QString& file) { stencil_ = file.isEmpty() ? QImage() : grayOf(file); stencilFile = stencil_.isNull() ? QString() : file; sync(); return !stencil_.isNull(); }
bool PaintProj::setStrip(const QString& file) { strip_ = file.isEmpty() ? QImage() : grayOf(file); stripFile = strip_.isNull() ? QString() : file; return !strip_.isNull(); }

// how much of the stroke the stencil lets through at a point of the viewport (no stencil: all)
float PaintProj::stencilAt(const QPointF& p) const {
  if (stencil_.isNull()) return 1; const double W = vp_->paneW(), H = vp_->paneH();
  const double dx = p.x() - (W / 2 + stX * W / 2), dy = p.y() - (H / 2 + stY * H / 2), a = -stAngle * 3.14159265358979 / 180, sc = std::max(0.02, stScale);
  double x = (dx * std::cos(a) - dy * std::sin(a)) / sc + stencil_.width() / 2.0, y = (dx * std::sin(a) + dy * std::cos(a)) / sc + stencil_.height() / 2.0;
  if (stTile) { x = std::fmod(x, stencil_.width()); if (x < 0) x += stencil_.width(); y = std::fmod(y, stencil_.height()); if (y < 0) y += stencil_.height(); }
  else if (x < 0 || y < 0 || x >= stencil_.width() || y >= stencil_.height()) return 0;
  return sample8(stencil_, x, y);
}

// a strip: the pattern laid along a piece of the stroke — across it the brush's size, along it repeated
QRect PaintProj::stripSegment(const QPointF& a, const QPointF& b) {
  const double len = std::hypot(b.x() - a.x(), b.y() - a.y()); if (len < 0.5) return {};
  const QPointF d = (b - a) / len; const double half = std::max(1.0, double(s_->brush.size)) / 2, period = std::max(2.0, s_->brush.size * double(strip_.width()) / std::max(1, strip_.height()));
  const int x0 = std::max(0, int(std::floor(std::min(a.x(), b.x()) - half))), x1 = std::min(scr_.width() - 1, int(std::ceil(std::max(a.x(), b.x()) + half)));
  const int y0 = std::max(0, int(std::floor(std::min(a.y(), b.y()) - half))), y1 = std::min(scr_.height() - 1, int(std::ceil(std::max(a.y(), b.y()) + half)));
  const float op = std::clamp(s_->brush.opacity, 0.f, 1.f);
  for (int y = y0; y <= y1; y++) { uchar* row = scr_.scanLine(y);
    for (int x = x0; x <= x1; x++) { const double px = x + 0.5 - a.x(), py = y + 0.5 - a.y(), t = px * d.x() + py * d.y(), lat = -px * d.y() + py * d.x();
      if (t < -0.5 || t > len + 0.5 || std::abs(lat) > half) continue;
      double u = std::fmod((arc_ + t) / period, 1.0); if (u < 0) u += 1;
      const int v = int(sample8(strip_, u * strip_.width(), (lat / (2 * half) + 0.5) * strip_.height()) * op * 255 + 0.5f); if (v > row[x]) row[x] = uchar(v); } }
  arc_ += len;
  return x0 <= x1 && y0 <= y1 ? QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1) : QRect();
}

// would a press on the model now begin a projected stroke (the brush / the eraser on a colour paint layer; not Shift's blur, not
// the freeze mask, not a layer's mask) — such a stroke needs no UV set shown for it
bool PaintProj::would() const {
  const QString& t = s_->tool;
  if (QApplication::keyboardModifiers() & Qt::ShiftModifier) return false;   // (Shift: the brush blurs — in the texture itself, the older way)
  if (!on || s_->freezePaint || s_->editMask || s_->view.startsWith("bake:") || !(t == "brush" || t == "eraser" || t == "line" || t == "curve" || t == "shape")) return false;
  const Layer* l = s_->active();
  return l && l->kind == LayerKind::Paint && !l->locked && l->fx.isEmpty() && l->channel == s_->modeChannel() && mapInfo(l->channel).storage == Storage::Colour;   // (the bump, the flow, a mask, no layer yet: the older way — a live effect layer too: refused there)
}

bool PaintProj::input(int phase, const QPointF& m, bool invert) {
  if (phase == 0) {
    active_ = false; if (!would()) return false;
    Layer* l = s_->active();
    if (l->image.isNull()) s_->doc.ensurePixels(l->id); if (l->image.format() != QImage::Format_ARGB32) return false;
    scr_ = QImage(vp_->paneW(), vp_->paneH(), QImage::Format_Grayscale8); if (scr_.isNull()) return false; scr_.fill(0); prev_ = scr_;
    erase_ = (s_->tool == "eraser") != invert; layer_ = l->id; sets_.clear(); strokeT_.start(); steps_ = 0; texelsAll_ = 0; stepMax_ = 0; box_s_ = QRect();
    if (cv_->strokeBegan) cv_->strokeBegan();
    s_->doc.cacheBelow(l->id);                                   // (the layers under it do not change during the stroke: their mix kept)
    // (Through: how far behind the seen surface the stroke reaches — the brush's radius there, in scene units; a perspective camera's
    // pixel grows with the distance, an orthographic one's does not)
    thr_ = s_->drawThrough; { const Camera& C = vp_->camera(); const double rpx = std::max(2.0, double(s_->brush.size) / 2);
      if (C.ortho()) { thrK_ = 0; thrO_ = rpx * C.worldPerPixel(); } else { thrK_ = rpx * 2 * std::tan(C.fov / 2 * 3.14159265358979 / 180) / std::max(1, vp_->paneH()); thrO_ = 0; } }
    lockId_ = 0;                                                 // (the stroke kept in the island it began on)
    if (s_->islandLock && s_->islands) if (const paint::IslandMap* im = s_->islands(); im && !im->ids.isNull()) if (const auto uv = vp_->paintUvAt(m)) lockId_ = im->at(QPointF(uv->x() * im->ids.width(), uv->y() * im->ids.height()));
    last_ = m; arc_ = 0; active_ = true; QRect d;
    // (the pen's pressure: into the stroke on the screen — its size and flow as the brush says; the mouse and a tool's line press fully)
    if (strip_.isNull()) { Brush b = s_->brush; b.mode = BrushMode::MaskReveal; st_ = std::make_unique<Stroke>(scr_, b, QColor(Qt::white)); if (!st_->valid()) { st_.reset(); active_ = false; return false; }
      if (s_->brush.orient == 2 && s_->islands) st_->strandAngle = [this](const QPointF& p) -> float {   // (along the strand, on the screen: the island's long axis carried through the UV's change across the screen there)
        const paint::IslandMap* im = s_->islands(); if (!im || im->ids.isNull()) return std::numeric_limits<float>::quiet_NaN();
        const auto u0 = vp_->paintUvAt(p), ux = vp_->paintUvAt(p + QPointF(3, 0)), uy = vp_->paintUvAt(p + QPointF(0, 3)); if (!u0 || !ux || !uy) return std::numeric_limits<float>::quiet_NaN();
        const double W = im->ids.width(), H = im->ids.height(); const float a = im->angleOf(im->at(QPointF(u0->x() * W, u0->y() * H))); if (std::isnan(a)) return a;
        const double ja = (ux->x() - u0->x()) * W / 3, jb = (uy->x() - u0->x()) * W / 3, jc = (ux->y() - u0->y()) * H / 3, jd = (uy->y() - u0->y()) * H / 3, det = ja * jd - jb * jc;   // (texture px per screen px)
        if (std::abs(det) < 1e-9) return std::numeric_limits<float>::quiet_NaN();
        const double tx = std::cos(a * 0.0174533), ty = std::sin(a * 0.0174533), sx = (jd * tx - jb * ty) / det, sy = (-jc * tx + ja * ty) / det;   // (the screen vector the island's axis comes from)
        return float(std::atan2(sy, sx) * 57.29578); };
      st_->begin(m, vp_->paintPressureNow()); d = st_->takeDirty(); }
    project(d); return true;
  }
  if (!active_) return false;
  if (phase == 1) { QRect d; if (st_) { st_->move(m, vp_->paintPressureNow()); d = st_->takeDirty(); } else d = stripSegment(last_, m); last_ = m; project(d); return true; }
  // (the stroke is over: one step of the history, whatever UV sets it passed over — the layer as it was in each of them)
  flush();
  { QJsonArray sets; for (auto& [set, st] : sets_) if (st.touched) sets.append(set);   // (the stroke as a whole, for the session's record)
    ui::devLog("stroke", {{"how", "projected"}, {"tool", s_->tool}, {"erase", erase_}, {"size", double(s_->brush.size)}, {"opacity", double(s_->brush.opacity)}, {"layer", layer_}, {"sets", sets}, {"steps", steps_}, {"texels", double(texelsAll_)},
                          {"ms", double(strokeT_.elapsed())}, {"stepMaxMs", double(stepMax_)}, {"x", box_s_.x()}, {"y", box_s_.y()}, {"w", box_s_.width()}, {"h", box_s_.height()}}); }
  { PaintSession::Undo u; u.layer = layer_; for (auto& [set, st] : sets_) if (st.touched) u.multi.push_back({set, st.before}); if (!u.multi.empty()) s_->pushUndo(std::move(u)); }
  sets_.clear(); st_.reset(); active_ = false; scr_ = {}; prev_ = {}; s_->doc.cacheBelow(-1); return true;
}

// the stroke as it lies on the screen now → the textures: every texel of what the camera sees there takes the layer as it was with the
// colour over it by the stroke's strength at the texel's place on the screen (worked out from the layer before the stroke: laid again
// at every step, never piling up). Every UV set the stroke lies over is written — the one shown and the others alike; a texel is
// written where nothing stands in front of it on the screen (Viewport::PvView). Of each polygon only the texels that lie under what
// changed on the screen are gone through; the polygons are shared out among the processor's cores
void PaintProj::project(const QRect& d) {
  stat = QString("dirty %1,%2 %3x%4").arg(d.x()).arg(d.y()).arg(d.width()).arg(d.height());
  if (d.isNull()) return; Layer* l = s_->doc.layer(layer_); if (!l) return;
  QElapsedTimer tm; tm.start();
  const Viewport::PvView& V = vp_->pvView(); V.facesIn(d.adjusted(-3, -3, 3, 3), pick_); if (pick_.empty()) return;
  const qint64 tView = tm.restart();
  const QString act = s_->doc.activeSet();
  QColor ink = s_->color; if (mapInfo(l->channel).gray) { const int g = qGray(ink.rgb()); ink = QColor(g, g, g); }
  const int ir = ink.red(), ig = ink.green(), ib = ink.blue(); const bool lockA = l->lockAlpha, erase = erase_;
  // (a UV set's picture of the layer: asked for when the first of its polygons comes under the stroke — a blank one made where the
  // layer has none in that set; the picture before the stroke kept by the set's name)
  struct Tg { bool tried = false; QImage* img = nullptr; QString name; uchar* bits = nullptr; const uchar* was = nullptr; qsizetype bpl = 0, wbpl = 0; int W = 0, H = 0; const paint::IslandMap* il = nullptr; mutable TileMask tiles; };
  std::vector<Tg> tg(V.sets.size() + 1);
  const paint::IslandMap* isl = lockId_ > 0 && s_->islands ? s_->islands() : nullptr;
  auto target = [&](int si) -> int { const int ti = si < 0 ? int(V.sets.size()) : si; Tg& t = tg[size_t(ti)]; if (t.tried) return ti; t.tried = true; t.name = si < 0 ? act : V.sets[size_t(si)];
    if (t.name != act) { if (lockId_ > 0) return ti;               // (kept in the island it began on: in that island's set)
      if (s_->doc.setSize(t.name).isEmpty()) { const auto p = vp_->uvSetProps(t.name); s_->doc.declareSet(t.name, p.w, p.h); } }
    QImage* im = s_->doc.pixelsIn(layer_, t.name, true); if (!im || im->format() != QImage::Format_ARGB32) return ti;
    SetSt& st = sets_[t.name]; if (st.before.isNull()) st.before = *im;
    if (st.before.size() != im->size()) return ti;
    t.img = im; t.W = im->width(); t.H = im->height(); t.bits = im->bits(); t.bpl = im->bytesPerLine(); t.was = st.before.constBits(); t.wbpl = st.before.bytesPerLine();   // (bits(): its own copy now — the threads below write it)
    t.il = isl && t.name == act && isl->ids.size() == im->size() ? isl : nullptr; t.tiles.reset(t.W, t.H); return ti; };
  const int sx0 = d.left() - 2, sx1 = d.right() + 2, sy0 = d.top() - 2, sy1 = d.bottom() + 2;
  struct Job { int face, tg; }; std::vector<Job> jobs; jobs.reserve(pick_.size());
  for (int fi : pick_) { const Viewport::PvView::F& f = V.faces[size_t(fi)];
    if (f.box.right() < sx0 || f.box.left() > sx1 || f.box.bottom() < sy0 || f.box.top() > sy1) continue;      // (not under what changed)
    const int ti = target(f.set); if (tg[size_t(ti)].img) jobs.push_back({fi, ti}); }
  if (jobs.empty()) return;
  const bool thr = thr_; const double thrK = thrK_, thrO = thrO_;
  const uchar* const scr = scr_.constBits(); const uchar* const prv = prev_.size() == scr_.size() ? prev_.constBits() : nullptr; const qsizetype sbpl = scr_.bytesPerLine(); const int SW = scr_.width(), SH = scr_.height();
  struct Out { std::vector<int> box; qint64 texels = 0; };       // (per thread: where each set's picture was written — x0, y0, x1, y1)
  auto work = [&](std::atomic<size_t>& next, Out& out) {
    out.box.assign(tg.size() * 4, 0); for (size_t i = 0; i < tg.size(); i++) { out.box[i * 4] = out.box[i * 4 + 1] = INT_MAX; out.box[i * 4 + 2] = out.box[i * 4 + 3] = INT_MIN; }
    for (;;) { const size_t ji = next.fetch_add(1); if (ji >= jobs.size()) return;
      const Job& jb = jobs[ji]; const Viewport::PvView::F& f = V.faces[size_t(jb.face)]; const Tg& T = tg[size_t(jb.tg)]; const int W = T.W, H = T.H; int* bx = &out.box[size_t(jb.tg) * 4];
      for (size_t i = 1; i + 1 < f.uv.size(); i++) {
        const QPointF d0(f.uv[0].x() * W, f.uv[0].y() * H), d1(f.uv[i].x() * W, f.uv[i].y() * H), d2(f.uv[i + 1].x() * W, f.uv[i + 1].y() * H), s0 = f.scr[0], s1 = f.scr[i], s2 = f.scr[i + 1];
        const float z0 = f.z[0], z1 = f.z[i], z2 = f.z[i + 1];
        const double den = (d1.y() - d2.y()) * (d0.x() - d2.x()) + (d2.x() - d1.x()) * (d0.y() - d2.y()); if (std::abs(den) < 1e-9) continue;
        const double longest = std::max({std::hypot(d1.x() - d0.x(), d1.y() - d0.y()), std::hypot(d2.x() - d1.x(), d2.y() - d1.y()), std::hypot(d0.x() - d2.x(), d0.y() - d2.y())}), slack = std::min(0.5, 2 * longest / std::abs(den));
        int x0 = std::max(0, int(std::floor(std::min({d0.x(), d1.x(), d2.x()}))) - 2), x1 = std::min(W - 1, int(std::ceil(std::max({d0.x(), d1.x(), d2.x()}))) + 2);
        int y0 = std::max(0, int(std::floor(std::min({d0.y(), d1.y(), d2.y()}))) - 2), y1 = std::min(H - 1, int(std::ceil(std::max({d0.y(), d1.y(), d2.y()}))) + 2);
        // (only the part of the triangle that lies under what changed on the screen: the changed rectangle carried into the texture)
        const double dens = (s1.y() - s2.y()) * (s0.x() - s2.x()) + (s2.x() - s1.x()) * (s0.y() - s2.y());
        if (std::abs(dens) > 1e-9) { double ux0 = 1e30, ux1 = -1e30, uy0 = 1e30, uy1 = -1e30;
          for (int c = 0; c < 4; c++) { const double cx = (c & 1) ? sx1 + 1 : sx0 - 1, cy = (c & 2) ? sy1 + 1 : sy0 - 1;
            const double a0 = ((s1.y() - s2.y()) * (cx - s2.x()) + (s2.x() - s1.x()) * (cy - s2.y())) / dens, a1 = ((s2.y() - s0.y()) * (cx - s2.x()) + (s0.x() - s2.x()) * (cy - s2.y())) / dens, a2 = 1 - a0 - a1;
            const double u = d0.x() * a0 + d1.x() * a1 + d2.x() * a2, v = d0.y() * a0 + d1.y() * a1 + d2.y() * a2; ux0 = std::min(ux0, u); ux1 = std::max(ux1, u); uy0 = std::min(uy0, v); uy1 = std::max(uy1, v); }
          x0 = std::max(x0, int(std::floor(std::max(ux0, -1e9))) - 1); x1 = std::min(x1, int(std::ceil(std::min(ux1, 1e9))) + 1); y0 = std::max(y0, int(std::floor(std::max(uy0, -1e9))) - 1); y1 = std::min(y1, int(std::ceil(std::min(uy1, 1e9))) + 1); }
        if (x0 > x1 || y0 > y1) continue;
        for (int y = y0; y <= y1; y++) { QRgb* row = reinterpret_cast<QRgb*>(T.bits + T.bpl * y); const QRgb* was = reinterpret_cast<const QRgb*>(T.was + T.wbpl * y);
          for (int x = x0; x <= x1; x++) { const double px = x + 0.5, py = y + 0.5;
            const double w0 = ((d1.y() - d2.y()) * (px - d2.x()) + (d2.x() - d1.x()) * (py - d2.y())) / den, w1 = ((d2.y() - d0.y()) * (px - d2.x()) + (d0.x() - d2.x()) * (py - d2.y())) / den, w2 = 1 - w0 - w1;
            if (w0 < -slack || w1 < -slack || w2 < -slack) continue;
            const double spx = s0.x() * w0 + s1.x() * w1 + s2.x() * w2, spy = s0.y() * w0 + s1.y() * w1 + s2.y() * w2; if (spx < 0 || spy < 0 || spx >= SW || spy >= SH) continue;
            // (the stroke's strength there: the screen picture read between its pixels)
            const double fx0 = std::clamp(spx - 0.5, 0.0, SW - 1.0), fy0 = std::clamp(spy - 0.5, 0.0, SH - 1.0); const int ix = int(fx0), iy = int(fy0), ix1 = std::min(SW - 1, ix + 1), iy1 = std::min(SH - 1, iy + 1); const double fx = fx0 - ix, fy = fy0 - iy;
            const uchar *ra = scr + sbpl * iy, *rb = scr + sbpl * iy1;
            float c = float((ra[ix] * (1 - fx) + ra[ix1] * fx) * (1 - fy) + (rb[ix] * (1 - fx) + rb[ix1] * fx) * fy) / 255.f; if (c <= 0.003f) continue;
            if (prv) { const uchar *pa = prv + sbpl * iy, *pb = prv + sbpl * iy1;   // (the stroke there as the texel already holds it: nothing new — not written again)
              if (pa[ix] == ra[ix] && pa[ix1] == ra[ix1] && pb[ix] == rb[ix] && pb[ix1] == rb[ix1]) continue; }
            const QPointF sp(spx, spy);
            const float zt = float(z0 * w0 + z1 * w1 + z2 * w2);
            if (!V.seen(jb.face, sp, zt)) {                    // (something stands in front of it there — Through: still painted while it lies within the brush's reach behind)
              if (!thr) continue; const float f = V.frontAt(sp); if (f < 1e29f && zt > f + float(thrK * f + thrO) + V.eps) continue; }
            c *= stencilAt(sp); if (c <= 0.003f) continue;
            if (T.il && !f.mirror && T.il->at(x, y) != lockId_) continue;   // (another island: the stroke stays in its own)
            const QRgb b = was[x]; const int ba = qAlpha(b); QRgb o;
            if (erase) o = qRgba(qRed(b), qGreen(b), qBlue(b), int(ba * (1 - c) + 0.5f));
            else if (lockA) { if (!ba) continue; o = qRgba(int(qRed(b) + (ir - qRed(b)) * c + 0.5f), int(qGreen(b) + (ig - qGreen(b)) * c + 0.5f), int(qBlue(b) + (ib - qBlue(b)) * c + 0.5f), ba); }
            else { const float a0 = ba / 255.f, na = c + a0 * (1 - c); o = qRgba(int((ir * c + qRed(b) * a0 * (1 - c)) / na + 0.5f), int((ig * c + qGreen(b) * a0 * (1 - c)) / na + 0.5f), int((ib * c + qBlue(b) * a0 * (1 - c)) / na + 0.5f), int(na * 255 + 0.5f)); }
            row[x] = o; out.texels++; T.tiles.t[size_t(y / TileMask::T) * T.tiles.tw + x / TileMask::T] = 1; bx[0] = std::min(bx[0], x); bx[1] = std::min(bx[1], y); bx[2] = std::max(bx[2], x); bx[3] = std::max(bx[3], y); } }
      } } };
  const size_t nt = jobs.size() < 48 ? 1 : std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 12);
  std::vector<Out> outs(nt); std::atomic<size_t> next{0};
  if (nt <= 1) work(next, outs[0]);
  else { std::vector<std::thread> ts; for (size_t i = 1; i < nt; i++) ts.emplace_back([&, i] { work(next, outs[i]); }); work(next, outs[0]); for (auto& t : ts) t.join(); }
  if (prv) { const QRect pr = QRect(sx0, sy0, sx1 - sx0 + 1, sy1 - sy0 + 1).intersected(scr_.rect());   // (what the texture took now)
    for (int y = pr.top(); y <= pr.bottom(); y++) std::memcpy(prev_.scanLine(y) + pr.left(), scr_.constScanLine(y) + pr.left(), size_t(pr.width())); }
  const qint64 tTex = tm.restart(); qint64 texels = 0; for (const Out& o : outs) texels += o.texels;
  stat += QString(" faces %1 texels %2 threads %3 sets").arg(jobs.size()).arg(texels).arg(nt);
  // (the locked, hidden and frozen texels as they were — by each set's own masks; then the picture's owner told where it changed)
  for (size_t i = 0; i < tg.size(); i++) { Tg& t = tg[i]; if (!t.img) continue; int b[4] = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    for (const Out& o : outs) { if (o.box.size() != tg.size() * 4) continue; b[0] = std::min(b[0], o.box[i * 4]); b[1] = std::min(b[1], o.box[i * 4 + 1]); b[2] = std::max(b[2], o.box[i * 4 + 2]); b[3] = std::max(b[3], o.box[i * 4 + 3]); }
    if (b[0] > b[2] || b[1] > b[3]) continue; const QRect td(QPoint(b[0], b[1]), QPoint(b[2], b[3]));
    SetSt& st = sets_[t.name]; st.touched = true; stat += " " + (t.name.isEmpty() ? QString("-") : t.name);
    (void)td; TileMask& pm = pend_[t.name]; if (pm.tw != t.tiles.tw || pm.th != t.tiles.th) pm.reset(t.W, t.H); pm.add(t.tiles); }
  if (!pend_.empty()) flushT_->start(0);
  steps_++; texelsAll_ += texels; stepMax_ = std::max(stepMax_, tView + tTex); box_s_ = box_s_.united(d);
  stat += QString(" · view %1 ms, texels %2 ms, shown %3 ms").arg(tView).arg(tTex).arg(tm.elapsed());
  if (statAll.size() < 16000) statAll += stat + "\n";            // (checks: read and emptied by "pv proj stats" — never grows past this)
}

// ---- the view to an external editor ----
// The picture of the viewport goes to Photoshop as two layers: the texture as it is, no light ("Colour"), and the room's light over
// it ("Light", Multiply — on / off there like 3D-Coat's Lightmap; 18.11: "свет отдельным слоем"). What the camera saw is remembered:
// every polygon with its place on that picture, the depth and the polygon at every pixel. When the file is saved there, what was
// changed in it is carried onto the model — into a new layer "Projection" — from the camera the picture was taken with, wherever the
// camera is by then. A texel takes it only where the camera saw it (nothing in front of it); a polygon seen at a slant takes less
// and less of it and an edge-on one none (as Substance's Angle + Hardness); the pixels at the edge of the silhouette — half the
// background, half the model on the screen — are not carried (the dark rims round a projection, 3D-Coat, 18.11)
namespace {
QImage paneShot(Viewport* vp) {
  QImage shot = vp->grab(); if (shot.isNull()) return {};
  if (vp->colorFormat() == VK_FORMAT_B8G8R8A8_SRGB || vp->colorFormat() == VK_FORMAT_B8G8R8A8_UNORM) shot = shot.rgbSwapped();
  const QRect pr = vp->paneRect(vp->activePane()); const double dpr = vp->devicePixelRatio();
  return shot.copy(QRect(int(pr.x() * dpr), int(pr.y() * dpr), int(pr.width() * dpr), int(pr.height() * dpr))).scaled(pr.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
}
// the light alone: lit ÷ unlit, a channel at a time (white — no change; a highlight brighter than the colour stays white: Multiply can't lighten)
QImage lightOf(const QImage& lit, const QImage& flat) {
  QImage L(flat.size(), QImage::Format_ARGB32);
  for (int y = 0; y < flat.height(); y++) { const QRgb *a = reinterpret_cast<const QRgb*>(lit.constScanLine(y)), *b = reinterpret_cast<const QRgb*>(flat.constScanLine(y)); QRgb* o = reinterpret_cast<QRgb*>(L.scanLine(y));
    for (int x = 0; x < flat.width(); x++) { auto ch = [](int l, int f) { return f <= 2 ? 255 : std::clamp(int(std::lround(255.0 * l / f)), 0, 255); };
      o[x] = qRgba(ch(qRed(a[x]), qRed(b[x])), ch(qGreen(a[x]), qGreen(b[x])), ch(qBlue(a[x]), qBlue(b[x])), 255); } }
  return L;
}
// the highlights alone, for Screen over colour × shade: lit = 1 − (1 − base)(1 − h) → h = 1 − (1 − lit)/(1 − base), where the lit
// picture is brighter than the colour (a Multiply layer can't lighten — the highlights were lost on the way to Photoshop; the
// author, 19.11: "фш не воспринимает блики")
QImage highlightsOf(const QImage& lit, const QImage& flat) {
  QImage Hh(flat.size(), QImage::Format_ARGB32);
  for (int y = 0; y < flat.height(); y++) { const QRgb *a = reinterpret_cast<const QRgb*>(lit.constScanLine(y)), *b = reinterpret_cast<const QRgb*>(flat.constScanLine(y)); QRgb* o = reinterpret_cast<QRgb*>(Hh.scanLine(y));
    for (int x = 0; x < flat.width(); x++) { auto ch = [](int l, int f) { if (l <= f) return 0; if (f >= 253) return 255; return std::clamp(int(std::lround(255.0 * (1.0 - (255.0 - l) / (255.0 - f)))), 0, 255); };
      o[x] = qRgba(ch(qRed(a[x]), qRed(b[x])), ch(qGreen(a[x]), qGreen(b[x])), ch(qBlue(a[x]), qBlue(b[x])), 255); } }
  return Hh;
}
}  // namespace

QString PaintProj::sendView(const QString& saveTo) {
  if (s_->doc.width() <= 0) return QObject::tr("Nothing to paint on yet");
  const QString disp0 = vp_->paintDisp;
  vp_->setPaintDisp("flat"); const QImage flat = paneShot(vp_);
  vp_->setPaintDisp("std"); const QImage lit = paneShot(vp_);
  vp_->setPaintDisp(disp0);
  if (flat.isNull() || lit.isNull()) return QObject::tr("The viewport's picture could not be taken");
  const QString act = s_->doc.activeSet(); sentV_ = vp_->pvView();
  int seen = 0; for (const auto& f : sentV_.faces) seen += !f.mirror && (f.set < 0 ? act.isEmpty() : sentV_.sets[size_t(f.set)] == act);
  if (!seen) { sentV_ = Viewport::PvView(); return QObject::tr("Nothing of the UV set painted is seen in this view"); }
  if (sentV_.W != flat.width() || sentV_.H != flat.height()) { sentV_ = Viewport::PvView(); return QObject::tr("The view changed while it was taken — send it again"); }
  sent_ = flat; sentSet_ = act; QString err;
  { // (how far every pixel lies from an edge: the background, or a surface standing well in front next to it — chamfer distance)
    const int W = sentV_.W, H = sentV_.H; const auto& id = sentV_.id; const auto& z = sentV_.depth; sentEdge_.assign(size_t(W) * H, 1e9f);
    auto jump = [&](size_t a, size_t b) { return id[b] == -1 || z[b] < z[a] - 0.03f * std::abs(z[a]) - sentV_.eps; };
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { const size_t i = size_t(y) * W + x;
      if (id[i] == -1 || x == 0 || y == 0 || x == W - 1 || y == H - 1 || jump(i, i - 1) || jump(i, i + 1) || jump(i, i - W) || jump(i, i + W)) sentEdge_[i] = 0; }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float& d = sentEdge_[size_t(y) * W + x];   // (forward)
      if (x > 0) d = std::min(d, sentEdge_[size_t(y) * W + x - 1] + 1); if (y > 0) { d = std::min(d, sentEdge_[size_t(y - 1) * W + x] + 1);
        if (x > 0) d = std::min(d, sentEdge_[size_t(y - 1) * W + x - 1] + 1.4142f); if (x + 1 < W) d = std::min(d, sentEdge_[size_t(y - 1) * W + x + 1] + 1.4142f); } }
    for (int y = H - 1; y >= 0; y--) for (int x = W - 1; x >= 0; x--) { float& d = sentEdge_[size_t(y) * W + x];   // (backward)
      if (x + 1 < W) d = std::min(d, sentEdge_[size_t(y) * W + x + 1] + 1); if (y + 1 < H) { d = std::min(d, sentEdge_[size_t(y + 1) * W + x] + 1);
        if (x + 1 < W) d = std::min(d, sentEdge_[size_t(y + 1) * W + x + 1] + 1.4142f); if (x > 0) d = std::min(d, sentEdge_[size_t(y + 1) * W + x - 1] + 1.4142f); } } }
  std::vector<PsdLayer> layers(3);
  layers[0].name = "Colour"; layers[0].image = flat.convertToFormat(QImage::Format_ARGB32);
  layers[1].name = "Shade"; layers[1].image = lightOf(lit, flat); layers[1].blend = Blend::Multiply;
  layers[2].name = "Highlights"; layers[2].image = highlightsOf(lit, flat); layers[2].blend = Blend::Screen;
  const QImage merged = lit.convertToFormat(QImage::Format_ARGB32);
  if (!saveTo.isEmpty()) {
    if (saveTo.endsWith(".psd", Qt::CaseInsensitive)) return writePsd(saveTo, flat.width(), flat.height(), layers, merged, &err) ? QString("saved %1 faces %2").arg(saveTo).arg(seen) : "not saved: " + err;
    return flat.save(saveTo) ? QString("saved %1 faces %2").arg(saveTo).arg(seen) : QString("not saved"); }
  const QString path = s_->bridge.workFolder() + "/" + viewFileName(); QFile::remove(path);
  if (!writePsd(path, flat.width(), flat.height(), layers, merged, &err)) { sent_ = {}; sentV_ = Viewport::PvView(); return err; }
  if (!s_->bridge.sendFile(s_->bridge.editor() == BridgeApp::ThreeDCoat ? BridgeApp::Photoshop : s_->bridge.editor(), path, &err)) { sent_ = {}; sentV_ = Viewport::PvView(); return err; }
  return QObject::tr("The view sent to Photoshop: \"Colour\" — the texture without light, \"Shade\" (Multiply) and \"Highlights\" (Screen) — the light over it; to keep a highlight, paint it into Colour or a layer of your own. Paint, save the file — what you changed comes back onto the model as the layer \"Projection\"");
}

QString PaintProj::takeBackFile(const QString& psd) {
  int w = 0, h = 0; std::vector<PsdLayer> in; QString err; if (!readPsd(psd, &w, &h, &in, &err)) return err;
  // (every visible picture layer but the light, composed bottom to top as plain layers — a new layer painted over "Colour" counts too)
  QImage out(w, h, QImage::Format_ARGB32_Premultiplied); out.fill(0); QPainter g(&out);
  for (const PsdLayer& l : in) { if (!l.pixels || !l.visible || (l.name == "Light" || l.name == "Shade" || l.name == "Highlights") || l.image.isNull()) continue; g.setOpacity(std::clamp<double>(l.opacity, 0, 1)); g.drawImage(0, 0, l.image); }
  g.end();
  QImage flat(w, h, QImage::Format_RGB32); flat.fill(Qt::black); QPainter f(&flat); f.drawImage(0, 0, out); f.end();
  return takeBack(flat);
}

QString PaintProj::takeBack(const QImage& img0) {
  if (sent_.isNull() || sentV_.faces.empty()) return QObject::tr("No view was sent");
  if (s_->doc.activeSet() != sentSet_) return QObject::tr("The view was sent from the UV set %1 — show it to take the projection back").arg(sentSet_.isEmpty() ? QObject::tr("Single texture") : sentSet_);
  const QImage back = img0.scaled(sent_.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
  // what was changed: the pixels that differ from the picture sent. A changed pixel comes back whole — its colour is what the editor
  // shows there (a soft stroke's edge is already mixed with what was under it); only a shift of a level or two is not a change. (Till
  // 18.11 a change under 24 levels was dropped and a small one came back faint — a Flow 10 % stroke in Photoshop was lost; that was
  // for a PNG's colour handling, the PSD's "Colour" layer goes and comes back as it is)
  QImage diff(sent_.size(), QImage::Format_Grayscale8); diff.fill(0); bool anyDiff = false;
  for (int y = 0; y < sent_.height(); y++) { const QRgb *a = reinterpret_cast<const QRgb*>(sent_.constScanLine(y)), *b = reinterpret_cast<const QRgb*>(back.constScanLine(y)); uchar* q = diff.scanLine(y);
    for (int x = 0; x < sent_.width(); x++) { const int dd = std::abs(qRed(a[x]) - qRed(b[x])) + std::abs(qGreen(a[x]) - qGreen(b[x])) + std::abs(qBlue(a[x]) - qBlue(b[x])); if (dd > 6) { q[x] = 255; anyDiff = true; } } }
  if (!anyDiff) return QObject::tr("Nothing was changed in the picture");
  const int id = s_->doc.addPaint(Channel::Color, QObject::tr("Projection"), s_->layer); s_->doc.ensurePixels(id); Layer* l = s_->doc.layer(id); if (!l) return QObject::tr("No layer could be made");
  QImage& out = l->image; const int W = out.width(), H = out.height(); const QImage empty = out;
  if (cv_->strokeBegan) cv_->strokeBegan();
  const Viewport::PvView& V = sentV_;
  // (near the edge of the silhouette — the background, or a surface well in front: the pixels there are half of the other (an AI
  // picture's dark rim, the anti-aliasing): nothing within a pixel of it, then fading in over edgePx)
  auto edgeW = [&](const QPointF& sp) -> double {
    const int px = std::clamp(int(std::floor(sp.x())), 0, V.W - 1), py = std::clamp(int(std::floor(sp.y())), 0, V.H - 1);
    const double d = sentEdge_.size() == size_t(V.W) * V.H ? sentEdge_[size_t(py) * V.W + px] : 1e9; if (d <= 1) return 0;
    const double t = std::min(1.0, (d - 1) / std::max(0.5, edgePx - 1)); return t * t * (3 - 2 * t); };
  const double aHi = angleFull, aLo = std::min(angleNone, angleFull - 1e-3);
  qint64 texels = 0, slant = 0, edge = 0, hidden = 0;
  for (size_t fi = 0; fi < V.faces.size(); fi++) { const auto& f = V.faces[fi];
    if (f.mirror || !(f.set < 0 ? sentSet_.isEmpty() : V.sets[size_t(f.set)] == sentSet_)) continue;
    double wa = (f.facing - aLo) / (aHi - aLo); if (wa <= 0) { slant++; continue; } wa = std::min(1.0, wa); wa = wa * wa * (3 - 2 * wa);   // (smooth)
    for (size_t i = 1; i + 1 < f.uv.size(); i++) {
      const QPointF d0(f.uv[0].x() * W, f.uv[0].y() * H), d1(f.uv[i].x() * W, f.uv[i].y() * H), d2(f.uv[i + 1].x() * W, f.uv[i + 1].y() * H), s0 = f.scr[0], s1 = f.scr[i], s2 = f.scr[i + 1];
      const float z0 = f.z[0], z1 = f.z[i], z2 = f.z[i + 1];
      const double den = (d1.y() - d2.y()) * (d0.x() - d2.x()) + (d2.x() - d1.x()) * (d0.y() - d2.y()); if (std::abs(den) < 1e-9) continue;
      const double longest = std::max({std::hypot(d1.x() - d0.x(), d1.y() - d0.y()), std::hypot(d2.x() - d1.x(), d2.y() - d1.y()), std::hypot(d0.x() - d2.x(), d0.y() - d2.y())}), slack = std::min(0.5, 2 * longest / std::abs(den));
      const int x0 = std::max(0, int(std::floor(std::min({d0.x(), d1.x(), d2.x()}))) - 2), x1 = std::min(W - 1, int(std::ceil(std::max({d0.x(), d1.x(), d2.x()}))) + 2);
      const int y0 = std::max(0, int(std::floor(std::min({d0.y(), d1.y(), d2.y()}))) - 2), y1 = std::min(H - 1, int(std::ceil(std::max({d0.y(), d1.y(), d2.y()}))) + 2);
      for (int y = y0; y <= y1; y++) { QRgb* row = nullptr;
        for (int x = x0; x <= x1; x++) { const double px = x + 0.5, py = y + 0.5;
          const double w0 = ((d1.y() - d2.y()) * (px - d2.x()) + (d2.x() - d1.x()) * (py - d2.y())) / den, w1 = ((d2.y() - d0.y()) * (px - d2.x()) + (d0.x() - d2.x()) * (py - d2.y())) / den, w2 = 1 - w0 - w1;
          if (w0 < -slack || w1 < -slack || w2 < -slack) continue;
          const QPointF sp = s0 * w0 + s1 * w1 + s2 * w2; if (sp.x() < 0 || sp.y() < 0 || sp.x() >= back.width() || sp.y() >= back.height()) continue;
          float a = sample8(diff, sp.x(), sp.y()); if (a <= 0.02f) continue;
          const float zt = float(z0 * w0 + z1 * w1 + z2 * w2);
          if (checkSeen && !V.seen(int(fi), sp, zt)) { hidden++; continue; }        // (something stood in front of this texel)
          const double ew = skipEdge ? edgeW(sp) : 1.0; if (ew <= 0) { edge++; continue; }
          a = float(std::min(1.f, a) * wa * ew); if (a <= 0.004f) continue;
          const QRgb c = back.pixel(std::clamp(int(sp.x()), 0, back.width() - 1), std::clamp(int(sp.y()), 0, back.height() - 1));
          if (!row) row = reinterpret_cast<QRgb*>(out.scanLine(y)); const int na = std::max(qAlpha(row[x]), int(a * 255 + 0.5f)); row[x] = qRgba(qRed(c), qGreen(c), qBlue(c), na); texels++; } } } }
  if (cv_->clip) cv_->clip(out, empty, out.rect());
  stat = QString("back texels %1 slantFaces %2 edge %3 hidden %4").arg(texels).arg(slant).arg(edge).arg(hidden);
  s_->layer = id; emit s_->layersChanged(); emit s_->pixelsChanged({});
  return QObject::tr("The changes carried onto the model: the layer \"Projection\"");
}

}  // namespace hm
