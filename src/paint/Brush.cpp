#include "paint/Brush.h"
#include "paint/Abr.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include "paint/StrokeTrace.h"
#include <QJsonDocument>

namespace hm::paint {

namespace {
struct ModeInfo { BrushMode m; const char* key; const char* name; int target; };
const ModeInfo kModes[] = {{BrushMode::Paint, "paint", "Paint", 0}, {BrushMode::Erase, "erase", "Erase", 0}, {BrushMode::Raise, "raise", "Raise (bump)", 1},
  {BrushMode::Lower, "lower", "Lower (bump)", 1}, {BrushMode::Flatten, "flatten", "Flatten (bump)", 1}, {BrushMode::Flow, "flow", "Flow direction", 2},
  {BrushMode::MaskReveal, "reveal", "Mask: reveal", 3}, {BrushMode::MaskHide, "hide", "Mask: hide", 3},
  {BrushMode::Blur, "blur", "Blur", 0}, {BrushMode::Smudge, "smudge", "Smudge", 0}, {BrushMode::Clone, "clone", "Stamp", 0}, {BrushMode::Patch, "patch", "Patch", 0}};
inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
}  // namespace

QString brushModeName(BrushMode m) { for (auto& i : kModes) if (i.m == m) return QString::fromLatin1(i.name); return "Paint"; }
QString brushModeKey(BrushMode m) { for (auto& i : kModes) if (i.m == m) return QString::fromLatin1(i.key); return "paint"; }
BrushMode brushModeFromKey(const QString& k) { for (auto& i : kModes) if (k == QLatin1String(i.key)) return i.m; return BrushMode::Paint; }
int brushTarget(BrushMode m) { for (auto& i : kModes) if (i.m == m) return i.target; return 0; }

// ---- the pressure curve ----
float Curve::at(float x) const {
  x = clamp01(x); if (pts.size() < 2) return x;
  const size_t n = pts.size(); if (x <= pts.front().x()) return clamp01(float(pts.front().y())); if (x >= pts.back().x()) return clamp01(float(pts.back().y()));
  // (monotone cubic, Fritsch-Carlson: no overshoot between the points, the curve never turns back)
  std::vector<double> d(n - 1), m(n);
  for (size_t i = 0; i + 1 < n; i++) { const double dx = std::max(1e-6, pts[i + 1].x() - pts[i].x()); d[i] = (pts[i + 1].y() - pts[i].y()) / dx; }
  m[0] = d[0]; m[n - 1] = d[n - 2]; for (size_t i = 1; i + 1 < n; i++) m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
  for (size_t i = 0; i + 1 < n; i++) { if (std::abs(d[i]) < 1e-9) { m[i] = m[i + 1] = 0; continue; } const double a = m[i] / d[i], b = m[i + 1] / d[i], h = a * a + b * b; if (h > 9) { const double t = 3 / std::sqrt(h); m[i] = t * a * d[i]; m[i + 1] = t * b * d[i]; } }
  size_t k = 0; while (k + 2 < n && x > pts[k + 1].x()) k++;
  const double h = std::max(1e-6, pts[k + 1].x() - pts[k].x()), t = (x - pts[k].x()) / h, t2 = t * t, t3 = t2 * t;
  const double y = (2 * t3 - 3 * t2 + 1) * pts[k].y() + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * pts[k + 1].y() + (t3 - t2) * h * m[k + 1];
  return clamp01(float(y));
}
QJsonArray Curve::toJson() const { QJsonArray a; for (const QPointF& p : pts) a.append(QJsonArray{p.x(), p.y()}); return a; }
Curve Curve::fromJson(const QJsonValue& v) {
  Curve c; const QJsonArray a = v.toArray(); if (a.size() < 2) return c;
  c.pts.clear(); for (const QJsonValue& p : a) { const QJsonArray q = p.toArray(); if (q.size() == 2) c.pts.push_back(QPointF(std::clamp(q[0].toDouble(), 0.0, 1.0), std::clamp(q[1].toDouble(), 0.0, 1.0))); }
  std::sort(c.pts.begin(), c.pts.end(), [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
  if (c.pts.size() < 2) c = Curve(); return c;
}

// ---- the made alphas ----
QString tipKindName(TipKind k) { switch (k) { case TipKind::Round: return "Round"; case TipKind::Cylinder: return "Cylinder"; case TipKind::Square: return "Square"; case TipKind::Gradient: return "Gradient"; default: return "Picture"; } }
QImage proceduralTip(TipKind k, float soft, float flat, float core, int px) {
  px = std::clamp(px, 8, 512); QImage im(px, px, QImage::Format_Grayscale8); im.fill(0);
  soft = clamp01(soft); flat = clamp01(flat); core = clamp01(core);
  auto smooth = [](float t) { t = clamp01(t); return t * t * (3 - 2 * t); };
  for (int y = 0; y < px; y++) { uchar* row = im.scanLine(y); const float v = (y + 0.5f) / px * 2 - 1;   // (-1...1 across)
    for (int x = 0; x < px; x++) { const float u = (x + 0.5f) / px * 2 - 1; float a = 0;   // (-1...1 along)
      if (k == TipKind::Cylinder) {
        // across: flat in the middle (flat: its share of the half-width), falling to the sides over the rest - softly (soft), or as a
        // round cylinder's shading (soft 0); along: full, its ends faded a little so the dabs join without seams
        const float r = std::abs(v), f = flat * 0.95f; float c = r <= f ? 1.f : 1.f - (r - f) / std::max(1e-3f, 1 - f);
        c = soft > 0.001f ? std::pow(clamp01(c), 0.5f + soft * 1.5f) : std::sqrt(std::max(0.f, 1 - r * r));
        c = clamp01(c) * (1 - core * 0.5f) + (core * 0.5f) * clamp01(1 - r * 3);   // (core: a brighter line along the middle)
        const float e = smooth((1 - std::abs(u)) / 0.18f); a = c * e; }
      else if (k == TipKind::Square) { const float e = std::max(std::abs(u), std::abs(v)); a = soft < 0.01f ? (e < 0.98f ? 1.f : 0.f) : 1 - smooth((e - (1 - soft * 0.5f)) / std::max(1e-3f, soft * 0.5f)); }
      else if (k == TipKind::Gradient) { a = clamp01(1 - (v + 1) / 2) * smooth((1 - std::abs(u)) / 0.1f); }   // (full at one side, nothing at the other)
      else { const float r = std::sqrt(u * u + v * v); a = r > 1 ? 0.f : 1 - smooth((r - (1 - soft)) / std::max(1e-3f, soft)); }
      row[x] = uchar(clamp01(a) * 255 + 0.5f); } }
  return im;
}

QJsonObject Brush::toJson() const {
  QJsonObject o; o["name"] = name; if (!set.isEmpty()) o["set"] = set; o["mode"] = brushModeKey(mode); o["size"] = size; o["spacing"] = spacing; o["hardness"] = hardness; o["opacity"] = opacity; o["flow"] = flow;
  o["strength"] = strength; o["angle"] = angle; o["follow"] = followStroke; o["sizeJ"] = sizeJitter; o["angleJ"] = angleJitter; o["scatter"] = scatter;
  o["pSize"] = pressureSize; o["pFlow"] = pressureFlow; if (smoothing > 0) o["smooth"] = smoothing; o["sblur"] = shiftBlur; o["smode"] = shiftMode;
  o["pOpa"] = pressureOpacity; o["pSizeMin"] = pSizeMin; o["pOpaMin"] = pOpacityMin; o["pFlowMin"] = pFlowMin;
  if (pSizeSrc) o["pSizeSrc"] = pSizeSrc; if (pOpacitySrc) o["pOpaSrc"] = pOpacitySrc; if (pFlowSrc) o["pFlowSrc"] = pFlowSrc;
  if (!pSizeCurve.linear()) o["pSizeC"] = pSizeCurve.toJson(); if (!pOpacityCurve.linear()) o["pOpaC"] = pOpacityCurve.toJson(); if (!pFlowCurve.linear()) o["pFlowC"] = pFlowCurve.toJson();
  if (hueJ > 0) o["hueJ"] = hueJ; if (satJ > 0) o["satJ"] = satJ; if (briJ > 0) o["briJ"] = briJ; if (!colourPerDab) o["colStroke"] = true; if (wet > 0) { o["wet"] = wet; o["load"] = load; }
  if (tipKind != TipKind::Image) o["tipKind"] = int(tipKind); o["tipSoft"] = tipSoft; o["tipFlat"] = tipFlat; o["tipCore"] = tipCore; o["orient"] = orient; return o;
}
Brush Brush::fromJson(const QJsonObject& o) {
  Brush b; b.name = o.value("name").toString(b.name); b.set = o.value("set").toString(); b.mode = brushModeFromKey(o.value("mode").toString()); b.size = float(o.value("size").toDouble(b.size)); b.spacing = float(o.value("spacing").toDouble(b.spacing));
  b.hardness = float(o.value("hardness").toDouble(b.hardness)); b.opacity = float(o.value("opacity").toDouble(1)); b.flow = float(o.value("flow").toDouble(b.flow)); b.strength = float(o.value("strength").toDouble(b.strength));
  b.angle = float(o.value("angle").toDouble()); b.followStroke = o.value("follow").toBool(); b.sizeJitter = float(o.value("sizeJ").toDouble()); b.angleJitter = float(o.value("angleJ").toDouble()); b.smoothing = float(o.value("smooth").toDouble());
  b.scatter = float(o.value("scatter").toDouble()); b.pressureSize = o.value("pSize").toBool(true); b.pressureFlow = o.value("pFlow").toBool(); b.shiftBlur = float(o.value("sblur").toDouble(0.5)); b.shiftMode = o.contains("smode") ? std::clamp(o.value("smode").toInt(1), 0, 2) : (o.value("salong").toBool(true) ? 1 : 0); b.shiftAlong = b.shiftMode == 1;
  b.pressureOpacity = o.value("pOpa").toBool(); b.pSizeMin = float(o.value("pSizeMin").toDouble(0.03)); b.pOpacityMin = float(o.value("pOpaMin").toDouble(0)); b.pFlowMin = float(o.value("pFlowMin").toDouble(0));
  b.pSizeSrc = o.value("pSizeSrc").toInt(); b.pOpacitySrc = o.value("pOpaSrc").toInt(); b.pFlowSrc = o.value("pFlowSrc").toInt();
  b.pSizeCurve = Curve::fromJson(o.value("pSizeC")); b.pOpacityCurve = Curve::fromJson(o.value("pOpaC")); b.pFlowCurve = Curve::fromJson(o.value("pFlowC"));
  b.hueJ = float(o.value("hueJ").toDouble()); b.satJ = float(o.value("satJ").toDouble()); b.briJ = float(o.value("briJ").toDouble()); b.colourPerDab = !o.value("colStroke").toBool();
  b.wet = float(o.value("wet").toDouble()); b.load = float(o.value("load").toDouble(0.5));
  b.tipKind = TipKind(std::clamp(o.value("tipKind").toInt(0), 0, 4)); b.tipSoft = float(o.value("tipSoft").toDouble(0.6)); b.tipFlat = float(o.value("tipFlat").toDouble(0.25)); b.tipCore = float(o.value("tipCore").toDouble(0));
  b.orient = o.contains("orient") ? std::clamp(o.value("orient").toInt(), 0, 2) : (b.followStroke ? 1 : 0); b.followStroke = b.orient == 1; return b;
}

QImage Brush::preview(const QSize& s, const QColor& ink, bool pen) const {
  QImage img(s, QImage::Format_ARGB32); img.fill(QColor(36, 36, 36));
  Brush b = *this; b.mode = BrushMode::Paint; b.size = std::min<float>(size, s.height() * 0.8f); if (!pen) b.pressureSize = false;
  strace::Mute mute; Stroke st(img, b, ink); if (!st.valid()) return img;
  const float tilt0 = g_penTilt; const int n = 60; const double pad = s.height() * 0.45;
  for (int i = 0; i <= n; i++) { const double t = double(i) / n; const QPointF p(pad + t * (s.width() - 2 * pad), s.height() * (0.5 + 0.2 * std::sin(t * 6.2831853)));
    const float pr = pen ? float(0.05 + 0.95 * std::sin(t * 3.14159265)) : 1.f; g_penTilt = pr;   // (pressed — and tilted — harder to the stroke's middle)
    if (!i) st.begin(p, pr); else st.move(p, pr); }
  g_penTilt = tilt0; return img;
}
QImage Brush::preview(const QSize& s) const {
  QImage img(s, QImage::Format_ARGB32); img.fill(QColor(36, 36, 36));
  Brush b = *this; b.mode = BrushMode::Paint; b.size = std::min<float>(size, s.height() * 0.7f); b.pressureSize = false; b.opacity = 1;
  strace::Mute mute; Stroke st(img, b, QColor(230, 230, 230)); if (!st.valid()) return img;
  const int n = 40; const double pad = s.height() * 0.45;
  for (int i = 0; i <= n; i++) { const double t = double(i) / n; const QPointF p(pad + t * (s.width() - 2 * pad), s.height() * (0.5 + 0.22 * std::sin(t * 6.2831853)));
    if (!i) st.begin(p); else st.move(p); }
  return img;
}

// ---- the stroke ----

Stroke::Stroke(QImage& target, const Brush& b, const QColor& color) : t_(&target), b_(b), color_(color) {
  static const QImage::Format F[] = {QImage::Format_ARGB32, QImage::Format_Grayscale16, QImage::Format_RGBA8888, QImage::Format_Grayscale8};
  ok_ = !target.isNull() && target.format() == F[brushTarget(b.mode)];
  if (!ok_) return;
  // (neither filled here: a tile of each is written when the stroke first reaches it — the memory of an 8K layer is asked for, not touched)
  snap_ = QImage(target.size(), target.format()); cov_ = QImage(target.size(), QImage::Format_Grayscale8);
  if (snap_.isNull() || cov_.isNull()) { ok_ = false; return; }
  tw_ = (target.width() + TILE - 1) / TILE; th_ = (target.height() + TILE - 1) / TILE; seen_.assign(size_t(tw_) * size_t(th_), 0);
  if (b_.tipKind != TipKind::Image && b_.tipKind != TipKind::Round) b_.tip = proceduralTip(b_.tipKind, b_.tipSoft, b_.tipFlat, b_.tipCore, 128);   // (the made alpha at a size the dabs read smoothly)
  if (b_.tipKind == TipKind::Round) b_.tip = {};
  ink_[0] = float(color_.redF()); ink_[1] = float(color_.greenF()); ink_[2] = float(color_.blueF());
}

void Stroke::reach(int x0, int y0, int x1, int y1) {
  const int bpp = t_->depth() / 8; const QImage& src = *t_;        // (read as const: the target is not detached by looking)
  for (int ty = y0 / TILE; ty <= y1 / TILE; ty++) for (int tx = x0 / TILE; tx <= x1 / TILE; tx++) {
    char& s = seen_[size_t(ty) * size_t(tw_) + size_t(tx)]; if (s) continue; s = 1; touched_.push_back(ty * tw_ + tx);
    const int px = tx * TILE, py = ty * TILE, w = std::min(TILE, src.width() - px), h = std::min(TILE, src.height() - py);
    for (int y = py; y < py + h; y++) { std::memcpy(snap_.scanLine(y) + size_t(px) * bpp, src.constScanLine(y) + size_t(px) * bpp, size_t(w) * bpp); std::memset(cov_.scanLine(y) + px, 0, size_t(w)); }
  }
}

std::vector<Stroke::Tile> Stroke::takeBefore() const {
  std::vector<Tile> out; out.reserve(touched_.size());
  for (int t : touched_) { const int px = (t % tw_) * TILE, py = (t / tw_) * TILE; out.push_back({QPoint(px, py), snap_.copy(px, py, std::min(TILE, snap_.width() - px), std::min(TILE, snap_.height() - py))}); }
  return out;
}

void Stroke::begin(const QPointF& p, float pressure) {
  if (!ok_) return;
  if (strace::on()) { QJsonObject bj = b_.toJson(); bj.remove("tip"); bj.remove("tipPng");   // (the stroke trace: the brush as it was for this stroke)
    strace::line("stroke", {{"x", p.x()}, {"y", p.y()}, {"p", pressure}, {"texW", t_->width()}, {"texH", t_->height()}, {"sizeScale", sizeScale}, {"tipW", b_.tip.width()}, {"tipH", b_.tip.height()}},
                 "\"color\":\"" + color_.name(QColor::HexArgb).toUtf8() + "\",\"brush\":" + QJsonDocument(bj).toJson(QJsonDocument::Compact)); }
  last_ = p; lastP_ = pressure; started_ = true; carry_ = 0; trail_ = p; dirKnown_ = false;
  if (!b_.colourPerDab && (b_.hueJ > 0 || b_.satJ > 0 || b_.briJ > 0)) {   // (the colour's jitter once for the whole stroke)
    float h, s, v, a; color_.getHsvF(&h, &s, &v, &a); if (h < 0) h = 0;
    h = std::fmod(h + (rnd() - 0.5f) * b_.hueJ + 1.f, 1.f); s = clamp01(s + (rnd() - 0.5f) * b_.satJ); v = clamp01(v + (rnd() - 0.5f) * b_.briJ);
    const QColor c = QColor::fromHsvF(h, s, v); ink_[0] = float(c.redF()); ink_[1] = float(c.greenF()); ink_[2] = float(c.blueF()); }
  dab(p, pressure);
}

void Stroke::move(const QPointF& p, float pressure) {
  if (!ok_) return; if (!started_) { begin(p, pressure); return; }
  if (strace::on()) strace::line("pt", {{"x", p.x()}, {"y", p.y()}, {"p", pressure}});   // (the stroke trace: the point the stroke went on to)
  const QPointF d = p - last_; const double len = std::hypot(d.x(), d.y()); if (len < 1e-6) return;
  const QPointF u = d / len;
  // (the stroke's direction — for an alpha turned along it: taken from a point a rope's length behind (a third of the brush), dragged along
  // after the pen. A hand that slows, shakes or stops does not turn it: the old way turned it at every tiny move, and a cylinder spun
  // round on the spot into a ragged blob)
  { const double R = std::max(3.0, double(b_.size) * sizeScale * 0.35); const QPointF t = p - trail_; const double tl = std::hypot(t.x(), t.y());
    if (tl >= R) { dir_ = t / tl; trail_ = p - dir_ * R; dirKnown_ = true; }
    else if (!dirKnown_ && tl > 2) dir_ = t / tl; }   // (the stroke's very start: the way it set off)
  const double step = std::max(1.0, double(b_.size) * sizeScale * b_.spacing);
  double pos = step - carry_;
  while (pos <= len) { const float k = float(pos / len); dab(last_ + u * pos, lastP_ + (pressure - lastP_) * k); pos += step; }
  carry_ = len - (pos - step); last_ = p; lastP_ = pressure;
}

void Stroke::dab(const QPointF& c, float pressure) {
  // (the pen's pressure: the least it leaves (min) plus the rest by the curve - min 0.03 is Photoshop's taper to a point, 0.15 - 3D-Coat's)
  const float pz = b_.pressureSize ? b_.pSizeMin + (1 - b_.pSizeMin) * b_.pSizeCurve.at(b_.pSizeSrc == 1 ? g_penTilt : pressure) : 1.f;
  float size = b_.size * sizeScale * std::max(0.01f, pz); if (b_.sizeJitter > 0) size *= 1 - b_.sizeJitter * rnd() * 0.8f;
  size = std::max(size, 1.f);
  QPointF at = c; if (b_.scatter > 0) at += QPointF((rnd() - 0.5f) * 2, (rnd() - 0.5f) * 2) * (b_.scatter * size);
  float ang0 = b_.angle + (b_.angleJitter > 0 ? (rnd() - 0.5f) * 360 * b_.angleJitter : 0.f);
  if (b_.orient == 1 || (b_.orient == 0 && b_.followStroke)) ang0 += float(std::atan2(dir_.y(), dir_.x()) * 57.29578);   // (along the stroke)
  else if (b_.orient == 2) { const float s = strandAngle ? strandAngle(c) : std::numeric_limits<float>::quiet_NaN(); ang0 += std::isnan(s) ? float(std::atan2(dir_.y(), dir_.x()) * 57.29578) : s; }   // (along the strand; not known - along the stroke)
  const QColor col = dabColour(at, size);
  dabAt(at, size, ang0, pressure, col);
  if (b_.mode == BrushMode::Clone || b_.mode == BrushMode::Patch) return;   // (the Stamp: no mirrored dabs — its symmetric copy is the source itself)
  if (mirrors) for (const QPointF& m : mirrors(at)) if (std::hypot(m.x() - at.x(), m.y() - at.y()) > 0.5) dabAt(m, size, ang0, pressure, col);
  if (mirror) { const QPointF m = mirror(at); if (std::hypot(m.x() - at.x(), m.y() - at.y()) > 0.5) dabAt(m, size, 180 - ang0, pressure, col); }   // (Symmetry: the mirrored dab, same stroke)
}

QColor Stroke::dabColour(const QPointF& c, float size) {
  if (brushTarget(b_.mode) != 0 || b_.mode == BrushMode::Erase || b_.mode == BrushMode::Blur || b_.mode == BrushMode::Smudge || b_.mode == BrushMode::Clone || b_.mode == BrushMode::Patch) return color_;
  if (b_.wet > 0.001f) {                                       // (wet: the colour under the dab's middle, as it was before the stroke, mixed in)
    const int r = std::max(1, int(size * 0.2f));
    const int W1 = t_->width() - 1, H1 = t_->height() - 1;   // (the same clamp as the samples below: a dab off the texture reads its edge, which is kept first)
    reach(std::clamp(int(c.x()) - r, 0, W1), std::clamp(int(c.y()) - r, 0, H1), std::clamp(int(c.x()) + r, 0, W1), std::clamp(int(c.y()) + r, 0, H1));
    float R = 0, G = 0, B = 0, A = 0;
    for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) { const int x = std::clamp(int(c.x()) + ox * r, 0, t_->width() - 1), y = std::clamp(int(c.y()) + oy * r, 0, t_->height() - 1);
      const QRgb p = reinterpret_cast<const QRgb*>(snap_.constScanLine(y))[x]; const float a = qAlpha(p) / 255.f; R += qRed(p) / 255.f * a; G += qGreen(p) / 255.f * a; B += qBlue(p) / 255.f * a; A += a; }
    if (A > 0.05f) { const float k = clamp01(b_.wet) * clamp01(b_.load); ink_[0] += (R / A - ink_[0]) * k; ink_[1] += (G / A - ink_[1]) * k; ink_[2] += (B / A - ink_[2]) * k; } }
  QColor out = QColor::fromRgbF(clamp01(ink_[0]), clamp01(ink_[1]), clamp01(ink_[2]));
  if (b_.colourPerDab && (b_.hueJ > 0 || b_.satJ > 0 || b_.briJ > 0)) {
    float h, s, v, a; out.getHsvF(&h, &s, &v, &a); if (h < 0) h = 0;
    h = std::fmod(h + (rnd() - 0.5f) * b_.hueJ + 1.f, 1.f); s = clamp01(s + (rnd() - 0.5f) * b_.satJ); v = clamp01(v + (rnd() - 0.5f) * b_.briJ); out = QColor::fromHsvF(h, s, v); }
  return out;
}

void Stroke::dabAt(const QPointF& at, float size, float ang, float pressure, const QColor& col) {
  const float ca = std::cos(ang * 0.0174533f), sa = std::sin(ang * 0.0174533f);
  const bool tip = !b_.tip.isNull(); const int tw = tip ? b_.tip.width() : 1, th = tip ? b_.tip.height() : 1; const float tm = float(std::max(tw, th));
  const float hx = size * 0.5f * (tip ? tw / tm : 1.f), hy = size * 0.5f * (tip ? th / tm : 1.f), R = std::hypot(hx, hy);
  const float flow = b_.flow * (b_.pressureFlow ? b_.pFlowMin + (1 - b_.pFlowMin) * b_.pFlowCurve.at(b_.pFlowSrc == 1 ? g_penTilt : pressure) : 1.f);
  const float opaP = b_.pressureOpacity ? b_.pOpacityMin + (1 - b_.pOpacityMin) * b_.pOpacityCurve.at(b_.pOpacitySrc == 1 ? g_penTilt : pressure) : 1.f;   // (the pen's pressure on the opacity: this dab's own ceiling)
  if (strace::on()) strace::line("dab", {{"x", at.x()}, {"y", at.y()}, {"size", size}, {"ang", ang}, {"p", pressure}, {"tilt", g_penTilt}, {"flow", flow}, {"opa", b_.opacity * opaP}}, "\"c\":\"" + col.name().toUtf8() + "\"");   // (the stroke trace: every dab laid)
  const int x0 = std::max(0, int(std::floor(at.x() - R))), x1 = std::min(t_->width() - 1, int(std::ceil(at.x() + R)));
  const int y0 = std::max(0, int(std::floor(at.y() - R))), y1 = std::min(t_->height() - 1, int(std::ceil(at.y() + R)));
  if (x0 > x1 || y0 > y1) return;
  const float hard = std::min(b_.hardness, 0.995f);
  // (a round tip's edge: soft by its hardness, and never narrower than one pixel — a hard brush's edge antialiased, not a stair of pixels;
  // the band centred on the radius, half a pixel in and half out)
  const float px = 1.f / std::max(0.5f, std::min(hx, hy)), outer = 1 + px * 0.5f, inner = outer - std::max(1 - hard, px), lim = tip ? 1.f : outer;
  const int tgt = brushTarget(b_.mode);
  const float fx = float(dir_.x()) * 0.5f + 0.5f, fy = float(-dir_.y()) * 0.5f + 0.5f;   // (the flow's direction in UV: y up)
  const bool soften = tgt == 0 && (b_.mode == BrushMode::Blur || b_.mode == BrushMode::Smudge);
  const bool clone = tgt == 0 && b_.mode == BrushMode::Clone, patch = b_.mode == BrushMode::Patch;
  std::optional<QTransform> cT; if (clone) { if (!cloneAt || cloneSrc.isNull()) return; cT = cloneAt(at); if (!cT) return; }   // (this dab's source: none — not laid)
  // (the source between its texels, premultiplied — r g b in 0…255 × alpha, a in 0…1; false — off the picture)
  auto srcAt = [&](double sx, double sy, float& r, float& g, float& b, float& a) {
    sx -= 0.5; sy -= 0.5; if (sx < -0.5 || sy < -0.5 || sx > cloneSrc.width() - 0.5 || sy > cloneSrc.height() - 0.5) return false;
    const int ix = std::clamp(int(std::floor(sx)), 0, cloneSrc.width() - 1), iy = std::clamp(int(std::floor(sy)), 0, cloneSrc.height() - 1), jx = std::min(ix + 1, cloneSrc.width() - 1), jy = std::min(iy + 1, cloneSrc.height() - 1);
    const float fx = float(std::clamp(sx - ix, 0.0, 1.0)), fy = float(std::clamp(sy - iy, 0.0, 1.0));
    const QRgb* r0 = reinterpret_cast<const QRgb*>(cloneSrc.constScanLine(iy)); const QRgb* r1 = reinterpret_cast<const QRgb*>(cloneSrc.constScanLine(jy));
    const QRgb q[4] = {r0[ix], r0[jx], r1[ix], r1[jx]}; const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    r = g = b = a = 0; for (int k = 0; k < 4; k++) { const float pa = qAlpha(q[k]) / 255.f * w[k]; r += qRed(q[k]) * pa; g += qGreen(q[k]) * pa; b += qBlue(q[k]) * pa; a += pa; } return true; };
  // (Blur / Smudge read the picture as it was around the dab too: those tiles kept first — as far as the farthest sample: the blur along
  //  the strand half the brush, a strong Smudge up to 0.8 of it behind each texel)
  const int pad = soften ? int(std::ceil(size * std::max(0.5f, 0.4f * std::max(0.1f, b_.strength * 2)))) + 2 : 0;
  reach(std::max(0, x0 - pad), std::max(0, y0 - pad), std::min(t_->width() - 1, x1 + pad), std::min(t_->height() - 1, y1 + pad));
  // the picture as it was at a point (premultiplied; clamped to its edge)
  // (Blur along the strand: the dab's direction there, once a dab — the samples on a line down the lock, half the brush either way)
  float alX = 0, alY = 0; bool along = false;
  if (soften && b_.mode == BrushMode::Blur && blurAlong && strandAngle) { const float sa0 = strandAngle(at); if (!std::isnan(sa0)) { alX = std::cos(sa0 * 0.0174533f); alY = std::sin(sa0 * 0.0174533f); along = true; } }
  // (oil: what lies under the dab now — the picture as painted so far — mixed into the brush's load; the first dab fills it)
  const bool oil = soften && b_.mode == BrushMode::Smudge && oilCarry;
  if (oil) { float r = 0, g = 0, bl = 0, a = 0; int n = 0; const int st = std::max(1, int(size / 8));
    for (int y = std::max(y0, 0); y <= std::min(y1, t_->height() - 1); y += st) { const QRgb* row = reinterpret_cast<const QRgb*>(t_->constScanLine(y));
      for (int x = std::max(x0, 0); x <= std::min(x1, t_->width() - 1); x += st) { const float dx = x + 0.5f - float(at.x()), dy = y + 0.5f - float(at.y()); if (dx * dx + dy * dy > size * size * 0.25f) continue;
        const QRgb p = row[x]; const float pa = qAlpha(p) / 255.f; r += qRed(p) * pa; g += qGreen(p) * pa; bl += qBlue(p) * pa; a += pa; n++; } }
    if (n) { r /= n; g /= n; bl /= n; a /= n;
      const float pick = loaded_ ? 0.12f + 0.3f * (1 - clamp01(softenK)) : 1.f;   // (a stronger Shift setting: the load keeps its colour longer)
      load_[0] += (r - load_[0]) * pick; load_[1] += (g - load_[1]) * pick; load_[2] += (bl - load_[2]) * pick; load_[3] += (a - load_[3]) * pick; loaded_ = true; } }
  auto was = [&](int sx, int sy, float& r, float& g, float& b, float& a) { sx = std::clamp(sx, 0, t_->width() - 1); sy = std::clamp(sy, 0, t_->height() - 1);
    const QRgb p = reinterpret_cast<const QRgb*>(snap_.constScanLine(sy))[sx]; const float pa = qAlpha(p) / 255.f; r += qRed(p) * pa; g += qGreen(p) * pa; b += qBlue(p) * pa; a += pa; };
  for (int y = y0; y <= y1; y++) {
    uchar* cv = cov_.scanLine(y);
    const uchar* srow = snap_.constScanLine(y); uchar* drow = nullptr;   // (the rows once — not asked of the image at every pixel; the target's only when this row is written)
    for (int x = x0; x <= x1; x++) {
      const float dx = x + 0.5f - float(at.x()), dy = y + 0.5f - float(at.y());
      const float u = (dx * ca + dy * sa) / hx, v = (-dx * sa + dy * ca) / hy;
      if (u < -lim || u > lim || v < -lim || v > lim) continue;
      float m;
      if (!tip) { const float r = std::sqrt(u * u + v * v); if (r >= outer) continue; m = r <= inner ? 1.f : (outer - r) / (outer - inner); if (1 - hard > px) m = m * m * (3 - 2 * m); }
      else {
        const float fxp = (u * 0.5f + 0.5f) * (tw - 1), fyp = (v * 0.5f + 0.5f) * (th - 1); const int ix = int(fxp), iy = int(fyp); const float wx = fxp - ix, wy = fyp - iy;
        const int ix1 = std::min(ix + 1, tw - 1), iy1 = std::min(iy + 1, th - 1);
        auto T = [&](int px, int py) { return b_.tip.constScanLine(py)[px] / 255.f; };
        m = (T(ix, iy) * (1 - wx) + T(ix1, iy) * wx) * (1 - wy) + (T(ix, iy1) * (1 - wx) + T(ix1, iy1) * wx) * wy;
      }
      // (the stroke's coverage moves toward this dab's own value at the flow's rate and never past it — as Photoshop's and Krita's
      // strokes: the dabs of one stroke don't pile up, so a soft or antialiased edge stays soft however close the dabs are; a low flow builds up)
      m *= opaP; if (m <= 0.002f) continue;
      if (clipIds) { const quint32 id = reinterpret_cast<const quint32*>(clipIds->constScanLine(y))[x] & 0xFFFFFFu; bool ok = false; for (quint32 a : clipAllow) if (a == id) { ok = true; break; } if (!ok) continue; }   // (outside the stroke's island)
      const float c0 = cv[x] / 255.f; if (m <= c0) continue;
      const float c1 = c0 + (m - c0) * flow; cv[x] = uchar(c1 * 255 + 0.5f);
      if (patch) continue;                                       // (Patch: only marked — filled by the owner from round the mark)
      const float k = clamp01(c1 * b_.opacity) * (soften ? clamp01(softenK) : 1.f);
      if (!drow) drow = t_->scanLine(y);
      if (tgt == 0) { const QRgb s = reinterpret_cast<const QRgb*>(srow)[x]; QRgb& d = reinterpret_cast<QRgb*>(drow)[x];
        if (b_.mode == BrushMode::Erase) d = qRgba(qRed(s), qGreen(s), qBlue(s), int(qAlpha(s) * (1 - k) + 0.5f));
        else if (clone) {                                         // (the Stamp: the source's texel at this one's place, mixed in as the stroke covers it)
          float r, g, bl, a; const QPointF sp = cT->map(QPointF(x + 0.5, y + 0.5)); if (!srcAt(sp.x(), sp.y(), r, g, bl, a)) continue;
          const float sa0 = qAlpha(s) / 255.f, pa = sa0 * (1 - k) + a * k, na = lockAlpha ? sa0 : pa;
          auto mix = [&](int sc, float ink) { return int(clamp01((sc * sa0 * (1 - k) + ink * k) / std::max(pa, 1e-5f) / 255.f) * 255 + 0.5f); };
          d = (na <= 0.002f || pa <= 0.002f) ? (lockAlpha ? s : qRgba(qRed(s), qGreen(s), qBlue(s), 0)) : qRgba(mix(qRed(s), r), mix(qGreen(s), g), mix(qBlue(s), bl), int(na * 255 + 0.5f)); }
        else if (soften) {                                       // (the picture as it was: around this pixel — Blur; behind it along the stroke — Smudge)
          float r = 0, g = 0, bl = 0, a = 0; int n = 0;
          if (oil) { if (!loaded_) { r = qRed(s) * (qAlpha(s) / 255.f); g = qGreen(s) * (qAlpha(s) / 255.f); bl = qBlue(s) * (qAlpha(s) / 255.f); a = qAlpha(s) / 255.f; } else { r = load_[0]; g = load_[1]; bl = load_[2]; a = load_[3]; } n = 1; }
          else if (b_.mode == BrushMode::Blur && along) { const float L = size * 0.5f; for (int t = -6; t <= 6; t++) { was(int(std::lround(x + alX * L * t / 6)), int(std::lround(y + alY * L * t / 6)), r, g, bl, a); n++; } }
          else if (b_.mode == BrushMode::Blur) { const int rad = std::max(1, int(size * 0.14f)); for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) { was(x + ox * rad, y + oy * rad, r, g, bl, a); was(x + ox * rad / 2, y + oy * rad / 2, r, g, bl, a); n += 2; } }
          else { const float L = size * 0.4f * std::max(0.1f, b_.strength * 2); for (int t = 1; t <= 5; t++) { was(int(x - dir_.x() * L * t / 5), int(y - dir_.y() * L * t / 5), r, g, bl, a); n++; } }
          r /= n; g /= n; bl /= n; a /= n;
          const float sa0 = qAlpha(s) / 255.f, pa = sa0 * (1 - k) + a * k, na = lockAlpha ? sa0 : pa;   // (mixed premultiplied: an empty texel brings no colour)
          auto mix = [&](int sc, float ink) { return int(clamp01((sc * sa0 * (1 - k) + ink * k) / std::max(pa, 1e-5f) / 255.f) * 255 + 0.5f); };
          d = (na <= 0.002f || pa <= 0.002f) ? s : qRgba(mix(qRed(s), r), mix(qGreen(s), g), mix(qBlue(s), bl), int(na * 255 + 0.5f)); }
        else if (lockAlpha) { const int sa = qAlpha(s); if (sa > 0) d = qRgba(int(qRed(s) + (col.red() - qRed(s)) * k + 0.5f), int(qGreen(s) + (col.green() - qGreen(s)) * k + 0.5f), int(qBlue(s) + (col.blue() - qBlue(s)) * k + 0.5f), sa); }
        else { const float sa0 = qAlpha(s) / 255.f, na = sa0 + (1 - sa0) * k;
          auto ch = [&](int sc, int cc) { return int(clamp01((sc * sa0 * (1 - k) + cc * k) / std::max(na, 1e-5f) / 255.f) * 255 + 0.5f); };
          d = qRgba(ch(qRed(s), col.red()), ch(qGreen(s), col.green()), ch(qBlue(s), col.blue()), int(na * 255 + 0.5f)); } }
      else if (tgt == 1) { const float s = reinterpret_cast<const quint16*>(srow)[x] / 65535.f; float n;
        if (b_.mode == BrushMode::Raise) n = s + k * b_.strength * 0.5f; else if (b_.mode == BrushMode::Lower) n = s - k * b_.strength * 0.5f; else n = s + (0.5f - s) * k;
        reinterpret_cast<quint16*>(drow)[x] = quint16(clamp01(n) * 65535 + 0.5f); }
      else if (tgt == 2) { const uchar* s = srow + x * 4; uchar* d = drow + x * 4;
        d[0] = uchar(clamp01(s[0] / 255.f + (fx - s[0] / 255.f) * k) * 255 + 0.5f); d[1] = uchar(clamp01(s[1] / 255.f + (fy - s[1] / 255.f) * k) * 255 + 0.5f); d[2] = 128;
        d[3] = uchar(clamp01(s[3] / 255.f + (1 - s[3] / 255.f) * k) * 255 + 0.5f); }
      else { const float s = srow[x] / 255.f, g = maskInk ? qGray(color_.rgb()) / 255.f : 1.f; drow[x] = uchar(clamp01(b_.mode == BrushMode::MaskReveal ? s + (g - s) * k : s * (1 - k)) * 255 + 0.5f); }
    }
  }
  dirty_ = dirty_.united(QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
}

// ---- the library ----

QString BrushLibrary::folder() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Brushes"; }

void BrushLibrary::addBuiltIn() {
  auto add = [&](const char* n, float hard, float spacing, float flow) { Brush b; b.name = n; b.set = "Basic"; b.hardness = hard; b.spacing = spacing; b.flow = flow; brushes.push_back(b); };
  add("Soft Round", 0.0f, 0.1f, 0.5f); add("Hard Round", 0.95f, 0.08f, 1.0f); add("Airbrush", 0.0f, 0.04f, 0.15f);
  Brush h; h.name = "Bump Raise"; h.set = "Basic"; h.mode = BrushMode::Raise; h.hardness = 0.3f; h.flow = 0.4f; brushes.push_back(h);
  Brush f; f.name = "Flow Direction"; f.set = "Basic"; f.mode = BrushMode::Flow; f.hardness = 0.6f; f.flow = 0.8f; f.size = 90; brushes.push_back(f);
  builtin = int(brushes.size());
}

QString BrushLibrary::presetFolder() { return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/HairHarpy Brushes"; }

namespace {
void readList(const QString& dir, std::vector<Brush>& out) {
  QFile f(dir + "/brushes.json"); if (!f.open(QIODevice::ReadOnly)) return;
  for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).array()) {
    const QJsonObject o = v.toObject(); Brush b = Brush::fromJson(o); const QString png = o.value("tip").toString();
    if (!png.isEmpty()) b.tip = QImage(dir + "/" + png).convertToFormat(QImage::Format_Grayscale8);
    out.push_back(b);
  }
}
}  // namespace

void BrushLibrary::load() {
  brushes.clear(); addBuiltIn();
  readList(presetFolder(), brushes); presets = int(brushes.size()) - builtin;   // (the user's preset pack, if there is one)
  readList(folder(), brushes);
}

void BrushLibrary::save() const {
  QDir().mkpath(folder()); QJsonArray a; int n = 0;
  for (const QString& f : QDir(folder()).entryList({"tip_*.png"}, QDir::Files)) QFile::remove(folder() + "/" + f);   // (the old tips: written again below)
  for (size_t i = size_t(fixed()); i < brushes.size(); i++) {   // (the built-in ones are made anew, the presets are read from their own folder)
    const Brush& b = brushes[i]; QJsonObject o = b.toJson();
    if (!b.tip.isNull()) { const QString png = QString("tip_%1.png").arg(n++); b.tip.save(folder() + "/" + png); o["tip"] = png; }
    a.push_back(o);
  }
  QFile f(folder() + "/brushes.json"); if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

QStringList BrushLibrary::sets() const {
  QStringList o; for (const Brush& b : brushes) { const QString n = setOf(b); if (!o.contains(n)) o << n; } return o;
}

int BrushLibrary::importAbr(const QString& file, QString* error, const QString& set) {
  QFile f(file); if (!f.open(QIODevice::ReadOnly)) { if (error) *error = "the file can't be opened"; return -1; }
  QString err; const auto list = readAbr(f.readAll(), &err);
  if (list.empty()) { if (error) *error = err.isEmpty() ? "no brushes in the file" : err; return -1; }
  const QString base = QFileInfo(file).completeBaseName(); int n = 0, added = 0;
  for (const AbrBrush& a : list) {
    { const QString nm = a.name; bool dup = false; for (const Brush& o : brushes) if (!nm.isEmpty() && o.name == nm && o.tip.size() == a.tip.size()) { dup = true; break; } if (dup) continue; }   // (the same brush from two files: once)
    Brush b; b.name = a.name.isEmpty() ? QString("%1 %2").arg(base).arg(++n) : a.name; b.set = set.isEmpty() ? base : set; b.tip = a.tip; b.spacing = a.spacing > 0 ? std::min(a.spacing, 1.f) : 0.25f; b.hardness = a.hardness >= 0 ? a.hardness : 1;
    b.size = a.diameter > 0 ? std::clamp(a.diameter, 1.f, 2048.f) : a.tip.isNull() ? 64.f : float(std::clamp(std::max(a.tip.width(), a.tip.height()), 16, 512));
    b.flow = 1; b.angle = a.angle; b.sizeJitter = a.sizeJitter; b.angleJitter = a.angleJitter; b.scatter = a.scatter; b.pressureSize = a.pressureSize || b.pressureSize; brushes.push_back(b); added++;
  }
  save(); return added;   // (the brushes added — the ones already kept are not counted again)
}

}  // namespace hm::paint
