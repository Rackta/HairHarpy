#include "paint/PaintDoc.h"
#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cmath>

#include <QRegularExpression>
#include <QStringList>
#include <cstring>

namespace hm::paint {

namespace {
struct BlendInfo { Blend b; const char* key; const char* name; };
const BlendInfo kBlends[] = {{Blend::Normal, "normal", "Normal"}, {Blend::Multiply, "multiply", "Multiply"}, {Blend::Screen, "screen", "Screen"},
  {Blend::Overlay, "overlay", "Overlay"}, {Blend::Add, "add", "Add"}, {Blend::Subtract, "subtract", "Subtract"},
  {Blend::SoftLight, "softlight", "Soft Light"}, {Blend::Darken, "darken", "Darken"}, {Blend::Lighten, "lighten", "Lighten"},
  {Blend::ColorBurn, "colorburn", "Color Burn"}, {Blend::LinearBurn, "linearburn", "Linear Burn"}, {Blend::ColorDodge, "colordodge", "Color Dodge"},
  {Blend::HardLight, "hardlight", "Hard Light"}, {Blend::VividLight, "vividlight", "Vivid Light"}, {Blend::LinearLight, "linearlight", "Linear Light"}, {Blend::PinLight, "pinlight", "Pin Light"},
  {Blend::Difference, "difference", "Difference"}, {Blend::Exclusion, "exclusion", "Exclusion"}, {Blend::Divide, "divide", "Divide"},
  {Blend::Hue, "hue", "Hue"}, {Blend::Saturation, "saturation", "Saturation"}, {Blend::Color, "color", "Color"}, {Blend::Luminosity, "luminosity", "Luminosity"}};
struct AdjInfo { AdjustKind a; const char* key; const char* name; };
const AdjInfo kAdj[] = {{AdjustKind::BrightContrast, "bc", "Brightness / Contrast"}, {AdjustKind::HueSat, "hs", "Hue / Saturation"},
  {AdjustKind::Levels, "levels", "Levels"}, {AdjustKind::Invert, "invert", "Invert"}, {AdjustKind::Threshold, "threshold", "Threshold"},
  {AdjustKind::Posterize, "posterize", "Posterize"}, {AdjustKind::GradientMap, "gradmap", "Gradient Map"}, {AdjustKind::Curves, "curves", "Curves"}};

inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
}  // namespace

const std::vector<MapInfo>& maps() {
  static const std::vector<MapInfo> M = {
    {Channel::Color, "color", "Base colour", "BaseColor", Storage::Colour, false, qRgba(128, 128, 128, 0), "The lit colour of the surface — what every toon shader starts from. Its alpha can carry the cut-out (hair tips, lashes)."},
    {Channel::Shade, "shade", "Shade colour", "ShadeColor", Storage::Colour, false, qRgba(128, 128, 128, 0), "The colour of the same surface in shadow (Guilty Gear's SSS map, MToon / UTS shade colour, 1st shade): painted, not darkened by a formula — skin goes warm, cloth goes cool."},
    {Channel::ShadowMask, "shadow", "Shadow mask", "ShadowMask", Storage::Colour, true, qRgb(128, 128, 128), "Where the light turns to shadow sooner or later (ILM green, the light map's AO, MToon's shading shift): darker — always in shadow (under the chin, creases), lighter — hard to shade. 50% gray — the light decides."},
    {Channel::Specular, "spec", "Highlight mask", "Specular", Storage::Colour, true, qRgb(0, 0, 0), "Where a highlight may show and how strong (ILM red / blue, the light map's specular): black — none. Metal, eyes, lips, the hair's band."},
    {Channel::Rim, "rim", "Rim mask", "RimMask", Storage::Colour, true, qRgb(255, 255, 255), "Where the rim light shows (MToon's rim multiply, UTS / lilToon rim mask): black — no rim (inside the mouth, under the hair)."},
    {Channel::Outline, "outline", "Outline width", "OutlineWidth", Storage::Colour, true, qRgb(255, 255, 255), "The outline's thickness over the mesh (UTS outline width map, MToon outline width multiply, Guilty Gear's vertex alpha): black — no line (the lips' inside, eyelashes), white — the full width."},
    {Channel::Emission, "emission", "Emission", "Emission", Storage::Colour, false, qRgba(0, 0, 0, 0), "What glows by itself: eyes' sparks, lamps, magic. Black — nothing."},
    {Channel::Opacity, "opacity", "Opacity", "Opacity", Storage::Colour, true, qRgb(255, 255, 255), "A cut-out of its own, when it is not kept in the colour's alpha: white — solid, black — gone."},
    {Channel::MatId, "matid", "Material ID", "ID", Storage::Colour, false, qRgba(0, 0, 0, 0), "Flat colours that tell the surfaces apart (skin / cloth / metal / hair): the shader picks a shadow ramp or a material by them (the light map's alpha in Genshin), other programs select by them."},
    {Channel::Height, "height", "Normal (painted as a bump)", "Normal", Storage::Relief, true, qRgb(128, 128, 128), "The relief painted as a bump — raise and lower, 50% gray is flat — and turned into the normal map (with the baked normals where the Bake mask lets them through). Folds, seams, stitches."},
    {Channel::BakeMask, "bakemask", "Bake mask", "BakeMask", Storage::Colour, true, qRgb(0, 0, 0), "Where the normals come from the high-poly: paint white over the folds of the cloth and only they are baked and used — the rest of the model keeps its own smooth normals."},
    {Channel::Flow, "flow", "Flow (hair highlight)", "Flow", Storage::Direction, false, qRgba(128, 128, 128, 0), "The direction the anisotropic highlight runs across — the stroke's own direction is written. The hair's band of light, satin."},
  };
  return M;
}
const MapInfo& mapInfo(Channel ch) { for (const MapInfo& m : maps()) if (m.ch == ch) return m; return maps().front(); }
Channel channelFromKey(const QString& key, bool* ok) { for (const MapInfo& m : maps()) if (key == QLatin1String(m.key)) { if (ok) *ok = true; return m.ch; } if (ok) *ok = false; return Channel::Color; }
QImage::Format formatOf(Channel ch) { const Storage s = mapInfo(ch).storage; return s == Storage::Colour ? QImage::Format_ARGB32 : s == Storage::Relief ? QImage::Format_Grayscale16 : QImage::Format_RGBA8888; }

namespace {
float blendCh(Blend b, float d, float s) {
  switch (b) {
    case Blend::Multiply: return d * s;
    case Blend::Screen: return 1 - (1 - d) * (1 - s);
    case Blend::Overlay: return d < 0.5f ? 2 * d * s : 1 - 2 * (1 - d) * (1 - s);
    case Blend::Add: return clamp01(d + s);
    case Blend::Subtract: return clamp01(d - s);
    case Blend::SoftLight: return s < 0.5f ? d - (1 - 2 * s) * d * (1 - d) : d + (2 * s - 1) * (std::sqrt(d) - d);
    case Blend::Darken: return std::min(d, s);
    case Blend::Lighten: return std::max(d, s);
    case Blend::ColorBurn: return d >= 1 ? 1 : s <= 0 ? 0 : 1 - std::min(1.f, (1 - d) / s);
    case Blend::LinearBurn: return clamp01(d + s - 1);
    case Blend::ColorDodge: return d <= 0 ? 0 : s >= 1 ? 1 : std::min(1.f, d / (1 - s));
    case Blend::HardLight: return s < 0.5f ? 2 * d * s : 1 - 2 * (1 - d) * (1 - s);
    case Blend::VividLight: return s < 0.5f ? blendCh(Blend::ColorBurn, d, 2 * s) : blendCh(Blend::ColorDodge, d, 2 * s - 1);
    case Blend::LinearLight: return clamp01(d + 2 * s - 1);
    case Blend::PinLight: return s < 0.5f ? std::min(d, 2 * s) : std::max(d, 2 * s - 1);
    case Blend::Difference: return std::abs(d - s);
    case Blend::Exclusion: return d + s - 2 * d * s;
    case Blend::Divide: return s <= 0 ? 1 : std::min(1.f, d / s);
    default: return s;
  }
}
// the modes that work on a colour as a whole (the W3C compositing formulas: the tone, the saturation and the lightness told apart)
inline float lumOf(const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
inline float satOf(const float c[3]) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }
void setLum(float c[3], float l) {
  const float d = l - lumOf(c); for (int i = 0; i < 3; i++) c[i] += d;
  const float L = lumOf(c), n = std::min({c[0], c[1], c[2]}), x = std::max({c[0], c[1], c[2]});
  if (n < 0) for (int i = 0; i < 3; i++) c[i] = L + (c[i] - L) * L / std::max(1e-6f, L - n);
  if (x > 1) for (int i = 0; i < 3; i++) c[i] = L + (c[i] - L) * (1 - L) / std::max(1e-6f, x - L);
}
void setSat(float c[3], float s) {
  int mx = 0, mn = 0; for (int i = 1; i < 3; i++) { if (c[i] > c[mx]) mx = i; if (c[i] < c[mn]) mn = i; } if (mx == mn) { c[0] = c[1] = c[2] = 0; return; }
  const int md = 3 - mx - mn; const float lo = c[mn], hi = c[mx];
  c[md] = (c[md] - lo) * s / (hi - lo); c[mx] = s; c[mn] = 0;
}
void blendPx(Blend b, const float d[3], const float s[3], float o[3]) {
  switch (b) {
    case Blend::Hue: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; setSat(o, satOf(d)); setLum(o, lumOf(d)); break;
    case Blend::Saturation: o[0] = d[0]; o[1] = d[1]; o[2] = d[2]; setSat(o, satOf(s)); setLum(o, lumOf(d)); break;
    case Blend::Color: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; setLum(o, lumOf(d)); break;
    case Blend::Luminosity: o[0] = d[0]; o[1] = d[1]; o[2] = d[2]; setLum(o, lumOf(s)); break;
    default: for (int i = 0; i < 3; i++) o[i] = blendCh(b, d[i], s[i]);
  }
}

void rgb2hsl(float r, float g, float b, float& h, float& s, float& l) {
  const float mx = std::max({r, g, b}), mn = std::min({r, g, b}); l = (mx + mn) / 2; const float d = mx - mn;
  if (d < 1e-6f) { h = 0; s = 0; return; }
  s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
  h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4; h /= 6;
}
float hue2c(float p, float q, float t) { if (t < 0) t += 1; if (t > 1) t -= 1; if (t < 1.f / 6) return p + (q - p) * 6 * t; if (t < 0.5f) return q; if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6; return p; }
void hsl2rgb(float h, float s, float l, float& r, float& g, float& b) {
  if (s < 1e-6f) { r = g = b = l; return; }
  const float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q; r = hue2c(p, q, h + 1.f / 3); g = hue2c(p, q, h); b = hue2c(p, q, h - 1.f / 3);
}
}  // namespace

const std::vector<Blend>& blendOrder() {
  static const std::vector<Blend> o = {Blend::Normal, Blend::Darken, Blend::Multiply, Blend::ColorBurn, Blend::LinearBurn, Blend::Subtract, Blend::Lighten, Blend::Screen, Blend::ColorDodge, Blend::Add,
    Blend::Overlay, Blend::SoftLight, Blend::HardLight, Blend::VividLight, Blend::LinearLight, Blend::PinLight, Blend::Difference, Blend::Exclusion, Blend::Divide, Blend::Hue, Blend::Saturation, Blend::Color, Blend::Luminosity};
  return o;
}
float Layer::toneAt(const std::vector<QPointF>& P, float x) {
  x = clamp01(x); const size_t n = P.size(); if (n < 2) return x;
  if (x <= P.front().x()) return clamp01(float(P.front().y())); if (x >= P.back().x()) return clamp01(float(P.back().y()));
  // (monotone cubic, Fritsch–Carlson: through every point, no overshoot between them)
  std::vector<double> d(n - 1), m(n);
  for (size_t i = 0; i + 1 < n; i++) d[i] = (P[i + 1].y() - P[i].y()) / std::max(1e-6, P[i + 1].x() - P[i].x());
  m[0] = d[0]; m[n - 1] = d[n - 2]; for (size_t i = 1; i + 1 < n; i++) m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
  for (size_t i = 0; i + 1 < n; i++) { if (std::abs(d[i]) < 1e-9) { m[i] = m[i + 1] = 0; continue; } const double a = m[i] / d[i], b = m[i + 1] / d[i], h = a * a + b * b; if (h > 9) { const double t = 3 / std::sqrt(h); m[i] = t * a * d[i]; m[i + 1] = t * b * d[i]; } }
  size_t k = 0; while (k + 2 < n && x > P[k + 1].x()) k++;
  const double h = std::max(1e-6, P[k + 1].x() - P[k].x()), t = (x - P[k].x()) / h, t2 = t * t, t3 = t2 * t;
  return clamp01(float((2 * t3 - 3 * t2 + 1) * P[k].y() + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * P[k + 1].y() + (t3 - t2) * h * m[k + 1]));
}
std::vector<float> Document::gradientLut(const std::vector<std::pair<float, QColor>>& stopsIn) {
  std::vector<std::pair<float, QColor>> st = stopsIn; if (st.empty()) st = {{0.f, QColor(0, 0, 0)}, {1.f, QColor(255, 255, 255)}};
  std::stable_sort(st.begin(), st.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<float> lut(256 * 3);
  for (int i = 0; i < 256; i++) { const float t = i / 255.f; size_t k = 0; while (k + 1 < st.size() && st[k + 1].first <= t) k++;
    QColor c = st[k].second;
    if (t < st.front().first) c = st.front().second;
    else if (k + 1 < st.size()) { const float span = st[k + 1].first - st[k].first, u = span > 1e-6f ? (t - st[k].first) / span : 0; const QColor &a = st[k].second, &b = st[k + 1].second;
      lut[size_t(i) * 3] = float(a.redF() + (b.redF() - a.redF()) * u); lut[size_t(i) * 3 + 1] = float(a.greenF() + (b.greenF() - a.greenF()) * u); lut[size_t(i) * 3 + 2] = float(a.blueF() + (b.blueF() - a.blueF()) * u); continue; }
    lut[size_t(i) * 3] = float(c.redF()); lut[size_t(i) * 3 + 1] = float(c.greenF()); lut[size_t(i) * 3 + 2] = float(c.blueF()); }
  return lut;
}
QString blendName(Blend b) { for (auto& i : kBlends) if (i.b == b) return QString::fromLatin1(i.name); return "Normal"; }
QString blendKey(Blend b) { for (auto& i : kBlends) if (i.b == b) return QString::fromLatin1(i.key); return "normal"; }
Blend blendFromKey(const QString& k) { for (auto& i : kBlends) if (k == QLatin1String(i.key)) return i.b; return Blend::Normal; }
QString adjustName(AdjustKind a) { for (auto& i : kAdj) if (i.a == a) return QString::fromLatin1(i.name); return "Adjustment"; }
QString adjustKey(AdjustKind a) { for (auto& i : kAdj) if (i.a == a) return QString::fromLatin1(i.key); return "bc"; }
AdjustKind adjustFromKey(const QString& k) { for (auto& i : kAdj) if (k == QLatin1String(i.key)) return i.a; return AdjustKind::BrightContrast; }

void Document::adjustDefaults(AdjustKind k, float& p1, float& p2, float& p3) {
  p1 = p2 = p3 = 0;
  switch (k) {
    case AdjustKind::Levels: p1 = 0; p2 = 1; p3 = 1; break;
    case AdjustKind::Threshold: p1 = 0.5f; break;
    case AdjustKind::Posterize: p1 = 4; break;
    default: break;
  }
}

void Document::reset(int w, int h) { w_ = w; h_ = h; layers_.clear(); nextId_ = 1; saved_.clear(); set_.clear(); sets_.clear(); dir_.clear(); loaded_.clear(); }

Layer* Document::layer(int id) { for (auto& l : layers_) if (l.id == id) return &l; return nullptr; }
const Layer* Document::layer(int id) const { for (auto& l : layers_) if (l.id == id) return &l; return nullptr; }
int Document::indexOf(int id) const { for (size_t i = 0; i < layers_.size(); i++) if (layers_[i].id == id) return int(i); return -1; }

QImage Document::blank(Channel ch) const {
  QImage im;
  if (mapInfo(ch).storage == Storage::Colour) { im = QImage(w_, h_, QImage::Format_ARGB32); im.fill(Qt::transparent); }
  else if (ch == Channel::Height) { im = QImage(w_, h_, QImage::Format_Grayscale16); for (int y = 0; y < h_; y++) { quint16* p = reinterpret_cast<quint16*>(im.scanLine(y)); std::fill(p, p + w_, quint16(32768)); } }
  else { im = QImage(w_, h_, QImage::Format_RGBA8888); im.fill(QColor(128, 128, 128, 0)); }
  return im;
}

Layer& Document::push(Layer l, int above) {
  l.id = nextId_++;
  size_t at = layers_.size();
  if (above >= 0) { const int i = indexOf(above); if (i >= 0) { at = size_t(i) + 1; l.parent = layers_[i].parent; } }
  return *layers_.insert(layers_.begin() + at, std::move(l));
}

// a name not taken yet: "<base><sep><n>" with the smallest n free — a layer deleted gives its number back (Layer 3 deleted: the next
// new one is Layer 3 again, not Layer 4)
QString Document::freeName(const QString& base, const QString& sep) const {
  for (int n = 1;; n++) { const QString name = base + sep + QString::number(n); bool taken = false; for (const Layer& l : layers_) if (l.name == name) { taken = true; break; } if (!taken) return name; }
}
// a name from where the layer lies and what it is: the layers under it in its group give the stem — what their names begin with alike
// (Sleeves + SleevesColor → Sleeves), or the nearest named one's name; its blend or kind gives the tag (Overlay → Over, a Gradient Map →
// GM); then _n, the first free: SleevesOver_1
QString Document::smartName(int id) const {
  const int at = indexOf(id); if (at < 0) return {}; const Layer& me = layers_[size_t(at)];
  static const QRegularExpression plain("^(Layer|Fill|Group|Bump|Flow|Paint) ?\\d*$");   // (a name the program gave: it says nothing)
  QStringList under;
  for (int i = at - 1; i >= 0 && under.size() < 2; i--) { const Layer& l = layers_[size_t(i)]; if (l.parent != me.parent || (l.kind != LayerKind::Group && l.channel != me.channel)) continue;
    if (l.name.isEmpty() || plain.match(l.name).hasMatch()) continue; under << l.name; }
  QString stem;
  if (under.size() == 2) { int n = 0; while (n < under[0].size() && n < under[1].size() && under[0][n] == under[1][n]) n++; if (n >= 3) stem = under[0].left(n); }
  if (stem.isEmpty() && !under.isEmpty()) stem = under[0];
  if (stem.isEmpty()) stem = set_.isEmpty() ? QStringLiteral("Layer") : set_;      // (nothing named under it: the UV set painted)
  stem.remove(QRegularExpression("[\\s_\\-]*\\d*$"));          // (no number at its end)
  { QString s; bool up = false; for (const QChar c : stem) { if (c.isSpace()) { up = true; continue; } s += up ? c.toUpper() : c; up = false; } stem = s; }   // (its words joined: Model texture → ModelTexture)
  for (const char* t : {"Over", "Soft", "Hard", "Mult", "Screen", "Add", "Color", "Hue", "Sat", "Lum", "GM", "HS", "BC", "Levels", "Fill", "Grp", "Paint"}) if (stem.size() > int(std::strlen(t)) + 2 && stem.endsWith(QLatin1String(t))) { stem.chop(int(std::strlen(t))); break; }
  QString tag;
  if (me.kind == LayerKind::Group) tag = "Grp";
  else if (me.kind == LayerKind::Adjust) tag = me.adjust == AdjustKind::GradientMap ? "GM" : me.adjust == AdjustKind::HueSat ? "HS" : me.adjust == AdjustKind::BrightContrast ? "BC" : me.adjust == AdjustKind::Levels ? "Levels" : "Adj";
  else switch (me.blend) {
    case Blend::Overlay: tag = "Over"; break; case Blend::SoftLight: tag = "Soft"; break; case Blend::HardLight: tag = "Hard"; break; case Blend::Multiply: tag = "Mult"; break;
    case Blend::Screen: tag = "Screen"; break; case Blend::Add: tag = "Add"; break; case Blend::Color: tag = "Color"; break; case Blend::Hue: tag = "Hue"; break;
    case Blend::Saturation: tag = "Sat"; break; case Blend::Luminosity: tag = "Lum"; break; case Blend::Normal: tag = me.kind == LayerKind::Fill ? "Fill" : "Paint"; break;
    default: tag = blendName(me.blend).remove(' '); break; }
  for (int n = 1;; n++) { const QString name = stem + tag + "_" + QString::number(n); bool taken = false; for (const Layer& l : layers_) if (l.id != id && l.name == name) { taken = true; break; } if (!taken) return name; }
}
// the topmost shown paint layer of the map that has paint at a texel (its mask counted) — what a click on the model means; −1: none
int Document::layerAt(Channel ch, int x, int y) const {
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return -1;
  for (int i = int(layers_.size()) - 1; i >= 0; i--) { const Layer& l = layers_[size_t(i)];
    if (l.kind != LayerKind::Paint || l.channel != ch || !l.visible || l.image.isNull() || l.image.format() != QImage::Format_ARGB32) continue;
    bool shown = true; for (int p = l.parent; p >= 0;) { const Layer* g = layer(p); if (!g) break; if (!g->visible) { shown = false; break; } p = g->parent; } if (!shown) continue;
    int a = qAlpha(reinterpret_cast<const QRgb*>(l.image.constScanLine(y))[x]); if (l.maskOn && !l.mask.isNull()) a = a * l.mask.constScanLine(y)[x] / 255;
    if (a * l.opacity >= 8) return l.id; }
  return -1;
}
// a paint layer with nothing painted in the UV set shown (no pixels at all, or every one of them clear) — asked often: kept by the picture's key
bool Document::isBlank(const Layer& l) const {
  if (l.kind != LayerKind::Paint) return false; if (l.image.isNull()) return true;
  if (l.image.format() != QImage::Format_ARGB32) return false;
  const qint64 key = l.image.cacheKey(); auto f = blank_.find(l.id); if (f != blank_.end() && f->second.first == key) return f->second.second;
  bool any = false; for (int y = 0; y < l.image.height() && !any; y++) { const QRgb* p = reinterpret_cast<const QRgb*>(l.image.constScanLine(y)); for (int x = 0; x < l.image.width(); x++) if (qAlpha(p[x])) { any = true; break; } }
  blank_[l.id] = {key, !any}; return !any;
}
// one layer shown alone (solo): it, the groups it lies in, what lies in it
bool Document::soloLets(const Layer& l) const {
  if (solo < 0 || l.id == solo) return true;
  for (int p = l.parent; p >= 0;) { if (p == solo) return true; const Layer* g = layer(p); p = g ? g->parent : -1; }
  const Layer* s = layer(solo); for (int p = s ? s->parent : -1; p >= 0;) { if (p == l.id) return true; const Layer* g = layer(p); p = g ? g->parent : -1; }
  return false;
}

int Document::addPaint(Channel ch, const QString& name, int above) {
  Layer l; l.kind = LayerKind::Paint; l.channel = ch; l.image = blank(ch);
  if (ch == Channel::Height) l.blend = Blend::Add;
  l.name = name.isEmpty() ? freeName(QString(ch == Channel::Color ? "Layer" : ch == Channel::Height ? "Bump" : ch == Channel::Flow ? "Flow" : mapInfo(ch).name)) : name;
  return push(std::move(l), above).id;
}
int Document::addFill(Channel ch, const QColor& c, const QString& name, int above) {
  Layer l; l.kind = LayerKind::Fill; l.channel = ch; l.fill = c; if (ch == Channel::Height) l.blend = Blend::Add;
  l.name = name.isEmpty() ? freeName("Fill") : name;
  return push(std::move(l), above).id;
}
int Document::addAdjust(AdjustKind k, const QString& name, int above, Channel ch) {
  Layer l; l.kind = LayerKind::Adjust; l.channel = mapInfo(ch).storage == Storage::Colour ? ch : Channel::Color; l.adjust = k; adjustDefaults(k, l.p1, l.p2, l.p3);
  l.name = name.isEmpty() ? adjustName(k) : name;
  return push(std::move(l), above).id;
}
int Document::addGroup(const QString& name, int above) {
  Layer l; l.kind = LayerKind::Group; l.name = name.isEmpty() ? freeName("Group") : name;
  return push(std::move(l), above).id;
}

int Document::duplicate(int id) {
  const Layer* s = layer(id); if (!s || s->kind == LayerKind::Group) return -1;
  Layer l = *s; l.name += " copy"; if (!l.image.isNull()) l.image = l.image.copy(); if (!l.mask.isNull()) l.mask = l.mask.copy();
  for (auto& [k, p] : l.other) { if (!p.image.isNull()) p.image = p.image.copy(); if (!p.mask.isNull()) p.mask = p.mask.copy(); }
  return push(std::move(l), id).id;
}

void Document::remove(int id) {
  std::vector<int> kill{id};
  for (size_t i = 0; i < kill.size(); i++) for (auto& l : layers_) if (l.parent == kill[i]) kill.push_back(l.id);
  layers_.erase(std::remove_if(layers_.begin(), layers_.end(), [&](const Layer& l) { return std::find(kill.begin(), kill.end(), l.id) != kill.end(); }), layers_.end());
  if (solo >= 0 && !layer(solo)) solo = -1;   // (the layer shown alone is gone: everything is shown again, not nothing)
}

void Document::restoreLayers(std::vector<Layer> ls) {
  // (a UV set read from the folder since the list was kept: a layer in both keeps those pixels — the kept list has none there)
  for (Layer& l : ls) if (const Layer* now = layer(l.id)) for (const auto& [k, p] : now->other) if (k != set_ && !l.other.count(k)) l.other.emplace(k, p);
  layers_ = std::move(ls);
  if (solo >= 0 && !layer(solo)) solo = -1;
  cacheTiles_.clear(); cacheLayer_ = -1;
}

void Document::move(int id, int delta) {
  int i = indexOf(id); if (i < 0 || !delta) return;
  const int par = layers_[i].parent, dir = delta > 0 ? 1 : -1;
  for (int n = 0; n < std::abs(delta); n++) {
    int j = i + dir; while (j >= 0 && j < int(layers_.size()) && layers_[j].parent != par) j += dir;   // (the neighbour on the same level)
    if (j < 0 || j >= int(layers_.size())) break;
    std::swap(layers_[i], layers_[j]); i = j;
  }
}

bool Document::mergeDown(int id, int* into) {
  const int i = indexOf(id); if (i <= 0) return false;
  Layer& top = layers_[i]; int j = i - 1; while (j >= 0 && layers_[j].parent != top.parent) j--;
  if (j < 0) return false;
  Layer& low = layers_[j];
  if (top.kind != LayerKind::Paint || low.kind != LayerKind::Paint || top.channel != low.channel || mapInfo(top.channel).storage != Storage::Colour || top.image.isNull() || low.image.isNull()) return false;
  // (the upper picture b — by its mask m, when it has one — onto the lower one a, in place: a is the layer's own image, so
  // what is merged stays in it. It was a copy before, and the merged picture was dropped with it)
  auto onto = [&top](QImage& a, const QImage& b, const QImage& m) {
    const int W = std::min(a.width(), b.width()), H = std::min(a.height(), b.height());
    const bool masked = top.maskOn && !m.isNull() && m.width() >= W && m.height() >= H;
    for (int y = 0; y < H; y++) { QRgb* d = reinterpret_cast<QRgb*>(a.scanLine(y)); const QRgb* s = reinterpret_cast<const QRgb*>(b.constScanLine(y)); const uchar* mk = masked ? m.constScanLine(y) : nullptr;
      for (int x = 0; x < W; x++) {
        float sa = qAlpha(s[x]) / 255.f * top.opacity; if (mk) sa *= mk[x] / 255.f;
        const float da = qAlpha(d[x]) / 255.f, oa = sa + da * (1 - sa); if (oa < 1e-6f) continue;
        float c[3] = {qRed(s[x]) / 255.f, qGreen(s[x]) / 255.f, qBlue(s[x]) / 255.f}, dc[3] = {qRed(d[x]) / 255.f, qGreen(d[x]) / 255.f, qBlue(d[x]) / 255.f}, o[3];
        float bl[3]; blendPx(top.blend, dc, c, bl);
        for (int k = 0; k < 3; k++) o[k] = ((1 - sa) * da * dc[k] + sa * ((1 - da) * c[k] + da * bl[k])) / oa;
        d[x] = qRgba(int(o[0] * 255 + 0.5f), int(o[1] * 255 + 0.5f), int(o[2] * 255 + 0.5f), int(oa * 255 + 0.5f));
      } } };
  onto(low.image, top.image, top.mask);
  for (auto& [set, px] : top.other) {     // (the other UV sets' pixels of the upper layer: merged the same way, not thrown away with it)
    if (px.image.isNull()) continue;
    Layer::Pixels& lp = low.other[set];
    if (lp.image.isNull()) { lp.image = QImage(px.image.size(), px.image.format()); lp.image.fill(Qt::transparent); }
    if (lp.image.format() == px.image.format()) onto(lp.image, px.image, px.mask);
  }
  if (into) *into = low.id;
  if (solo == top.id) solo = -1;
  layers_.erase(layers_.begin() + i);
  return true;
}

void Document::addMask(int id, bool white) {
  Layer* l = layer(id); if (!l || !l->mask.isNull()) return;
  l->mask = QImage(w_, h_, QImage::Format_Grayscale8); l->mask.fill(white ? 255 : 0); l->maskOn = true;
}
void Document::removeMask(int id) { if (Layer* l = layer(id)) l->mask = QImage(); }
void Document::invertMask(int id) { if (Layer* l = layer(id); l && !l->mask.isNull()) l->mask.invertPixels(); }
void Document::applyMask(int id) {
  Layer* l = layer(id); if (!l || l->mask.isNull() || l->kind != LayerKind::Paint || mapInfo(l->channel).storage != Storage::Colour || l->image.isNull()) return;
  for (int y = 0; y < h_; y++) { QRgb* d = reinterpret_cast<QRgb*>(l->image.scanLine(y)); const uchar* m = l->mask.constScanLine(y);
    for (int x = 0; x < w_; x++) d[x] = qRgba(qRed(d[x]), qGreen(d[x]), qBlue(d[x]), qAlpha(d[x]) * m[x] / 255); }
  l->mask = QImage();
}

bool Document::rasterize(int id) {
  Layer* l = layer(id); if (!l || l->kind != LayerKind::Fill || mapInfo(l->channel).storage != Storage::Colour) return false;
  QStringList sets = l->onlySets; if (sets.isEmpty()) { sets << set_; for (const SetInfo& x : sets_) if (!sets.contains(x.key)) sets << x.key; }
  const int wasSolo = solo; const float op = l->opacity; const bool vis = l->visible; const Channel ch = l->channel;
  solo = id; l->opacity = 1; l->visible = true;
  std::map<QString, QImage> px; for (const QString& st : sets) { const QImage im = composeSet(st, ch); if (!im.isNull()) px[st] = im.convertToFormat(formatOf(ch)); }
  l = layer(id); solo = wasSolo; l->opacity = op; l->visible = vis;
  l->kind = LayerKind::Paint; l->gradLive = false; l->gradT.clear(); l->spheres.clear(); l->onlySets.clear(); l->mask = QImage(); l->maskOn = true;
  for (auto& [k, p] : l->other) p.mask = QImage();
  for (auto& [st, im] : px) { if (st == set_) l->image = im; else l->other[st].image = im; }
  if (l->image.isNull()) l->image = blank(ch);
  return true;
}

// ---- composition ----

void Document::composeScope(Channel ch, int parent, const QRect& r, std::vector<Px>& out, int startAt, int stopAt) const {
  const int W = r.width(), H = r.height();
  // (clipped layers: the alpha of the last layer that is not clipped — what the clipped ones above it are shown through)
  std::vector<float> baseA; for (const Layer& l : layers_) if (l.parent == parent && l.clip && l.visible && (l.kind == LayerKind::Group || l.channel == ch)) { baseA.assign(size_t(W) * H, 0.f); break; }
  bool started = startAt < 0;
  for (const Layer& l : layers_) {
    if (l.id == stopAt) break;
    if (!started) { if (l.id != startAt) continue; started = true; }
    if (l.parent != parent || !l.visible || !soloLets(l)) continue;
    if (!l.onlySets.isEmpty() && !l.onlySets.contains(set_)) continue;   // (a layer of other UV sets)
    const bool grp = l.kind == LayerKind::Group, adj = l.kind == LayerKind::Adjust;
    if (!grp && l.channel != ch) continue;                       // (an adjustment too: it changes the map it was made in)
    const bool masked = l.maskOn && !l.mask.isNull();
    const float fr = l.fill.redF(), fg = l.fill.greenF(), fb = l.fill.blueF();   // (a fill layer's colour: once, not every pixel)
    const std::vector<float> lut = adj && l.adjust == AdjustKind::GradientMap ? gradientLut(l.grad) : std::vector<float>();
    std::vector<float> clut;                                    // (Curves: 3 × 1024 — a channel's curve, then the common one)
    if (adj && l.adjust == AdjustKind::Curves) { clut.resize(3 * 1024); for (int c = 0; c < 3; c++) for (int i = 0; i < 1024; i++) clut[size_t(c) * 1024 + i] = Layer::toneAt(l.tones[0], Layer::toneAt(l.tones[size_t(c) + 1], i / 1023.f)); }
    // (a live gradient: its colours by 256 steps, read through the texels' places between its points — none known: nothing shown)
    const bool gl = l.kind == LayerKind::Fill && l.gradLive && ch != Channel::Height && ch != Channel::Flow;
    const bool gsph = gl && l.liveKind == QLatin1String("spheres");
    const QImage* gt = nullptr; if (gl) { const auto f = l.gradT.find(set_); if (f == l.gradT.end()) continue; gt = gsph ? &f->second.rgba : &f->second.t; if (gt->size() != QSize(w_, h_)) continue; }
    const std::vector<float> glut = gl ? gradientLut(l.grad.empty() ? std::vector<std::pair<float, QColor>>{{0.f, l.fill}, {1.f, QColor(0, 0, 0)}} : l.grad) : std::vector<float>();
    std::vector<Px> sub; if (grp) { sub.assign(size_t(W) * H, ch == Channel::Height ? Px{0.5f, 0.5f, 0.5f, 0} : Px{0, 0, 0, 0}); composeScope(ch, l.id, r, sub); }
    for (int y = 0; y < H; y++) {
      const int iy = r.y() + y;
      const QRgb* c32 = (l.kind == LayerKind::Paint && ch != Channel::Height && !l.image.isNull()) ? reinterpret_cast<const QRgb*>(l.image.constScanLine(iy)) : nullptr;
      const uchar* px8 = (l.kind == LayerKind::Paint && ch == Channel::Flow && !l.image.isNull()) ? l.image.constScanLine(iy) : nullptr;
      const quint16* h16 = (l.kind == LayerKind::Paint && ch == Channel::Height && !l.image.isNull()) ? reinterpret_cast<const quint16*>(l.image.constScanLine(iy)) : nullptr;
      const uchar* mk = masked ? l.mask.constScanLine(iy) : nullptr;
      for (int x = 0; x < W; x++) {
        const int ix = r.x() + x; Px& d = out[size_t(y) * W + x];
        const float m = (mk ? mk[ix] / 255.f : 1.f) * l.opacity;
        if (ch == Channel::Height) {
          float delta = 0;
          if (grp) delta = sub[size_t(y) * W + x].r - 0.5f;
          else if (l.kind == LayerKind::Fill) delta = l.fillHeight;
          else if (h16) delta = (int(h16[ix]) - 32768) / 32768.f;
          delta *= m; d.r = clamp01(d.r + (l.blend == Blend::Subtract ? -delta : delta)); continue;
        }
        if (ch == Channel::Flow) {
          float sx, sy, sa;
          if (grp) { const Px& s = sub[size_t(y) * W + x]; sx = s.r; sy = s.g; sa = s.a * m; }
          else if (l.kind == LayerKind::Fill) { sx = l.fill.redF() * 2 - 1; sy = l.fill.greenF() * 2 - 1; sa = m; }
          else if (px8) { const uchar* p = px8 + ix * 4; sx = p[0] / 127.5f - 1; sy = p[1] / 127.5f - 1; sa = p[3] / 255.f * m; }
          else continue;
          d.r = d.r * (1 - sa) + sx * sa; d.g = d.g * (1 - sa) + sy * sa; d.a = sa + d.a * (1 - sa); continue;
        }
        // colour
        if (adj) {
          float c[3] = {d.r, d.g, d.b}, o[3] = {d.r, d.g, d.b};
          switch (l.adjust) {
            case AdjustKind::BrightContrast: { const float k = l.p2 >= 0 ? 1 / (1 - l.p2 * 0.999f) : 1 + l.p2; for (int i = 0; i < 3; i++) o[i] = clamp01((c[i] - 0.5f) * k + 0.5f + l.p1 * 0.5f); break; }
            case AdjustKind::HueSat: { float h, s, li; rgb2hsl(c[0], c[1], c[2], h, s, li); h += l.p1 / 360; h -= std::floor(h);
              s = clamp01(l.p2 >= 0 ? s * (1 + l.p2 * 2) : s * (1 + l.p2)); li = clamp01(l.p3 >= 0 ? li + (1 - li) * l.p3 : li * (1 + l.p3)); hsl2rgb(h, s, li, o[0], o[1], o[2]); break; }
            case AdjustKind::Levels: { const float span = std::max(l.p2 - l.p1, 1e-4f), ig = 1 / std::max(l.p3, 0.05f); for (int i = 0; i < 3; i++) o[i] = std::pow(clamp01((c[i] - l.p1) / span), ig); break; }
            case AdjustKind::Invert: for (int i = 0; i < 3; i++) o[i] = 1 - c[i]; break;
            case AdjustKind::Threshold: { const float v = (0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]) >= l.p1 ? 1.f : 0.f; o[0] = o[1] = o[2] = v; break; }
            case AdjustKind::Posterize: { const float n = std::max(2.f, std::round(l.p1)) - 1; for (int i = 0; i < 3; i++) o[i] = std::round(c[i] * n) / n; break; }
            case AdjustKind::Curves: for (int i = 0; i < 3; i++) o[i] = clut[size_t(i) * 1024 + size_t(std::clamp(int(c[i] * 1023 + 0.5f), 0, 1023))]; break;
            case AdjustKind::GradientMap: { const int li = std::clamp(int((0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]) * 255 + 0.5f), 0, 255); o[0] = lut[size_t(li) * 3]; o[1] = lut[size_t(li) * 3 + 1]; o[2] = lut[size_t(li) * 3 + 2]; break; }
          }
          const float ma = l.clip && !baseA.empty() ? m * baseA[size_t(y) * W + x] : m;   // (clipped: only where the layer under it has something)
          d.r += (o[0] - d.r) * ma; d.g += (o[1] - d.g) * ma; d.b += (o[2] - d.b) * ma; continue;
        }
        float sr, sg, sb, sa;
        if (grp) { const Px& s = sub[size_t(y) * W + x]; sr = s.r; sg = s.g; sb = s.b; sa = s.a * m; }
        else if (gsph) { const QRgb p = reinterpret_cast<const QRgb*>(gt->constScanLine(iy))[ix];
          if (!qAlpha(p)) { if (!baseA.empty() && !l.clip) baseA[size_t(y) * W + x] = 0; continue; }
          sr = qRed(p) / 255.f; sg = qGreen(p) / 255.f; sb = qBlue(p) / 255.f; sa = qAlpha(p) / 255.f * m; }
        else if (gl) { const int v = reinterpret_cast<const quint16*>(gt->constScanLine(iy))[ix];
          if (!v) { if (!baseA.empty() && !l.clip) baseA[size_t(y) * W + x] = 0; continue; }   // (no surface under the texel)
          const size_t li = size_t(std::clamp((v - 1) * 255 / 65534, 0, 255)) * 3; sr = glut[li]; sg = glut[li + 1]; sb = glut[li + 2]; sa = m; }
        else if (l.kind == LayerKind::Fill) { sr = fr; sg = fg; sb = fb; sa = m; }
        else if (c32) { const QRgb p = c32[ix]; sr = qRed(p) / 255.f; sg = qGreen(p) / 255.f; sb = qBlue(p) / 255.f; sa = qAlpha(p) / 255.f * m; }
        else { if (!baseA.empty() && !l.clip) baseA[size_t(y) * W + x] = 0; continue; }
        if (!baseA.empty()) { float& ba = baseA[size_t(y) * W + x]; if (l.clip) sa *= ba; else ba = sa; }
        if (sa <= 0) continue;
        const float oa = sa + d.a * (1 - sa), da = d.a;
        const float dc[3] = {d.r, d.g, d.b}, sc[3] = {sr, sg, sb}; float bl[3]; blendPx(l.blend, dc, sc, bl);
        d.r = ((1 - sa) * da * d.r + sa * ((1 - da) * sr + da * bl[0])) / oa;
        d.g = ((1 - sa) * da * d.g + sa * ((1 - da) * sg + da * bl[1])) / oa;
        d.b = ((1 - sa) * da * d.b + sa * ((1 - da) * sb + da * bl[2])) / oa;
        d.a = oa;
      }
    }
  }
}

void Document::readyGradients(Channel ch) const {
  for (const Layer& l : layers_) {
    if (!l.visible || l.kind != LayerKind::Fill || !l.gradLive || l.channel != ch || (!l.onlySets.isEmpty() && !l.onlySets.contains(set_))) continue;
    Layer::GradT& G = l.gradT[set_]; const qint64 rev = geomRevOf ? geomRevOf(set_) : 0;
    if (l.liveKind == QLatin1String("spheres")) {                // (the spheres: their colours over the texels, made again when any of them or the geometry changed)
      quint64 key = 1469598103934665603ULL ^ quint64(rev); auto mix = [&](const void* p, size_t n) { const uchar* b = static_cast<const uchar*>(p); for (size_t i = 0; i < n; i++) { key ^= b[i]; key *= 1099511628211ULL; } };
      for (const Layer::Sphere& sp : l.spheres) { mix(sp.c, sizeof sp.c); mix(&sp.r, sizeof sp.r); mix(&sp.hard, sizeof sp.hard); const QRgb cc = sp.col.rgb(); mix(&cc, sizeof cc); }
      mix(&w_, sizeof w_); mix(&h_, sizeof h_);
      if (G.rgba.size() == QSize(w_, h_) && G.skey == key) continue;
      QImage o(w_, h_, QImage::Format_ARGB32); o.fill(0);
      const float *P = nullptr, *N = nullptr;
      if (!l.spheres.empty() && positionsOf && positionsOf(set_, w_, h_, P, N) && P && N) {
        struct S { float x, y, z, r2, r, h; float cr, cg, cb; }; std::vector<S> ss;
        for (const Layer::Sphere& sp : l.spheres) if (sp.r > 0) ss.push_back({sp.c[0], sp.c[1], sp.c[2], sp.r * sp.r, sp.r, std::clamp(sp.hard, 0.f, 0.999f), float(sp.col.redF()), float(sp.col.greenF()), float(sp.col.blueF())});
        auto rows = [&](int y0, int y1) { for (int y = y0; y < y1; y++) { QRgb* out = reinterpret_cast<QRgb*>(o.scanLine(y));
          for (int x = 0; x < w_; x++) { const size_t q = (size_t(y) * size_t(w_) + size_t(x)) * 3; if (N[q] == 0 && N[q + 1] == 0 && N[q + 2] == 0) continue;
            float keep = 1, W = 0, R = 0, Gc = 0, B = 0;
            for (const S& e : ss) { const float dx = P[q] - e.x, dy = P[q + 1] - e.y, dz = P[q + 2] - e.z, d2 = dx * dx + dy * dy + dz * dz; if (d2 >= e.r2) continue;
              const float d = std::sqrt(d2) / e.r; float w = d <= e.h ? 1.f : 1.f - (d - e.h) / (1.f - e.h); w = w * w * (3 - 2 * w);   // (full to the hardness, then a smooth fall to the radius)
              keep *= 1 - w; W += w; R += e.cr * w; Gc += e.cg * w; B += e.cb * w; }
            if (W <= 0) continue; const float a = 1 - keep;
            out[x] = qRgba(int(R / W * 255 + 0.5f), int(Gc / W * 255 + 0.5f), int(B / W * 255 + 0.5f), int(std::clamp(a, 0.f, 1.f) * 255 + 0.5f)); } } };
        const int nt = std::clamp(int(std::thread::hardware_concurrency()), 1, 12); std::vector<std::thread> ts; const int step = (h_ + nt - 1) / nt;
        for (int k = 1; k < nt; k++) ts.emplace_back(rows, std::min(h_, k * step), std::min(h_, (k + 1) * step)); rows(0, std::min(h_, step)); for (auto& th : ts) th.join(); }
      G.rgba = o; G.skey = key; continue; }
    if (l.liveKind == QLatin1String("curve")) {                 // (along a drawn curve: the nearest place on it, by its length — only where the mask lets it show)
      quint64 key = 1469598103934665603ULL ^ quint64(rev); auto mix = [&](const void* p, size_t n) { const uchar* b = static_cast<const uchar*>(p); for (size_t i = 0; i < n; i++) { key ^= b[i]; key *= 1099511628211ULL; } };
      for (const auto& c : l.curve) mix(c.data(), sizeof(float) * 3); mix(&w_, sizeof w_); mix(&h_, sizeof h_); const qint64 mk = l.mask.isNull() ? 0 : l.mask.cacheKey(); mix(&mk, sizeof mk);
      if (G.t.size() == QSize(w_, h_) && G.skey == key) continue;
      const float *P = nullptr, *N = nullptr;
      if (l.curve.size() < 2 || !positionsOf || !positionsOf(set_, w_, h_, P, N) || !P || !N) { G.t = QImage(); G.skey = key; continue; }
      std::vector<float> cum(l.curve.size(), 0.f); for (size_t i = 1; i < l.curve.size(); i++) { const auto& a = l.curve[i - 1]; const auto& b = l.curve[i]; cum[i] = cum[i - 1] + std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2])); }
      const float total = std::max(cum.back(), 1e-9f);
      const QImage* M = !l.mask.isNull() && l.mask.size() == QSize(w_, h_) && l.mask.format() == QImage::Format_Grayscale8 ? &l.mask : nullptr;
      QImage t(w_, h_, QImage::Format_Grayscale16);
      auto rows = [&](int y0, int y1) { for (int y = y0; y < y1; y++) { quint16* o = reinterpret_cast<quint16*>(t.scanLine(y)); const uchar* mr = M ? M->constScanLine(y) : nullptr;
        for (int x = 0; x < w_; x++) { const size_t q = (size_t(y) * size_t(w_) + size_t(x)) * 3;
          if ((N[q] == 0 && N[q + 1] == 0 && N[q + 2] == 0) || (mr && !mr[x])) { o[x] = 0; continue; }
          float best = 1e30f, at = 0;
          for (size_t i = 1; i < l.curve.size(); i++) { const auto& a = l.curve[i - 1]; const auto& b = l.curve[i];
            const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2], L2 = dx * dx + dy * dy + dz * dz;
            float k = L2 > 1e-12f ? ((P[q] - a[0]) * dx + (P[q + 1] - a[1]) * dy + (P[q + 2] - a[2]) * dz) / L2 : 0.f; k = std::clamp(k, 0.f, 1.f);
            const float ex = a[0] + dx * k - P[q], ey = a[1] + dy * k - P[q + 1], ez = a[2] + dz * k - P[q + 2], d2 = ex * ex + ey * ey + ez * ez;
            if (d2 < best) { best = d2; at = cum[i - 1] + (cum[i] - cum[i - 1]) * k; } }
          o[x] = quint16(1 + int(std::clamp(at / total, 0.f, 1.f) * 65534 + 0.5f)); } } };
      const int nt = std::clamp(int(std::thread::hardware_concurrency()), 1, 12); std::vector<std::thread> ts; const int step = (h_ + nt - 1) / nt;
      for (int k = 1; k < nt; k++) ts.emplace_back(rows, std::min(h_, k * step), std::min(h_, (k + 1) * step)); rows(0, std::min(h_, step)); for (auto& th : ts) th.join();
      G.t = t; G.skey = key; G.rev = rev; continue; }
    if (G.t.size() == QSize(w_, h_) && G.rev == rev && std::equal(l.g0, l.g0 + 3, G.a) && std::equal(l.g1, l.g1 + 3, G.b)) continue;
    const float *P = nullptr, *N = nullptr;
    if (!positionsOf || !positionsOf(set_, w_, h_, P, N) || !P || !N) { G.t = QImage(); continue; }
    QImage t(w_, h_, QImage::Format_Grayscale16);
    const float dx = l.g1[0] - l.g0[0], dy = l.g1[1] - l.g0[1], dz = l.g1[2] - l.g0[2], L2 = std::max(dx * dx + dy * dy + dz * dz, 1e-12f);
    auto rows = [&](int y0, int y1) { for (int y = y0; y < y1; y++) { quint16* o = reinterpret_cast<quint16*>(t.scanLine(y));
      for (int x = 0; x < w_; x++) { const size_t q = (size_t(y) * size_t(w_) + size_t(x)) * 3;
        if (N[q] == 0 && N[q + 1] == 0 && N[q + 2] == 0) { o[x] = 0; continue; }
        const float k = std::clamp(((P[q] - l.g0[0]) * dx + (P[q + 1] - l.g0[1]) * dy + (P[q + 2] - l.g0[2]) * dz) / L2, 0.f, 1.f);
        o[x] = quint16(1 + int(k * 65534 + 0.5f)); } } };
    const int nt = std::clamp(int(std::thread::hardware_concurrency()), 1, 12); std::vector<std::thread> ts; const int step = (h_ + nt - 1) / nt;
    for (int k = 1; k < nt; k++) ts.emplace_back(rows, std::min(h_, k * step), std::min(h_, (k + 1) * step)); rows(0, std::min(h_, step)); for (auto& th : ts) th.join();
    G.t = t; G.rev = rev; std::copy(l.g0, l.g0 + 3, G.a); std::copy(l.g1, l.g1 + 3, G.b);
  }
}

void Document::cacheBelow(int layerId) {
  cacheTiles_.clear(); cacheLayer_ = -1;
  const Layer* l = layer(layerId); if (!l || l->kind != LayerKind::Paint || l->parent != -1 || solo >= 0) return;
  for (const Layer& x : layers_) if (x.parent == -1 && x.clip) return;   // (clipped layers read what lies under them layer by layer: not kept)
  cacheLayer_ = layerId; cacheCh_ = l->channel; cacheSet_ = set_;
}

QImage Document::compose(Channel ch, const QRect& rect) const {
  const QRect r = rect.isNull() ? QRect(0, 0, w_, h_) : rect.intersected(QRect(0, 0, w_, h_));
  if (r.isEmpty()) return {};
  readyGradients(ch);                                         // (here, once, before the bands go to the threads)
  // (a stroke going: the mix under the layer painted, by tiles — made the first time a tile is asked for, before the threads below)
  const bool cached = cacheLayer_ >= 0 && ch == cacheCh_ && set_ == cacheSet_ && !rect.isNull() && layer(cacheLayer_);
  constexpr int T = 64; auto keyOf = [&](int tx, int ty) { return (qint64(tx) << 32) | qint64(ty); };
  if (cached) for (int ty = r.top() / T; ty <= r.bottom() / T; ty++) for (int tx = r.left() / T; tx <= r.right() / T; tx++) {
    auto& t = cacheTiles_[keyOf(tx, ty)]; if (!t.empty()) continue;
    const QRect tr = QRect(tx * T, ty * T, T, T).intersected(QRect(0, 0, w_, h_)); t.assign(size_t(tr.width()) * tr.height(), ch == Channel::Height ? Px{0.5f, 0.5f, 0.5f, 0} : Px{0, 0, 0, 0});
    composeScope(ch, -1, tr, t, -1, cacheLayer_); }
  const Storage stg = mapInfo(ch).storage;
  QImage out(r.size(), formatOf(ch));
  uchar* const base = out.bits(); const qsizetype bpl = out.bytesPerLine();   // (the rows by hand: the bands below write them from several threads)
  // a band of rows composed into its own buffer and written out — the bands are independent: a big picture is split over the cores
  // (a band goes strip by strip — about a million pixels of working floats at a time, 16 MB a thread: a whole 8K map composed in one
  // piece took a gigabyte of them, and as much again for every group)
  const int strip = std::clamp(int((1 << 20) / std::max(1, r.width())), 8, 1024);
  auto band = [&](int b0, int b1) { for (int y0 = b0; y0 < b1; y0 += strip) { const int y1 = std::min(b1, y0 + strip);
    const QRect br(r.x(), r.y() + y0, r.width(), y1 - y0); if (br.height() <= 0) return;
    std::vector<Px> buf(size_t(br.width()) * br.height(), ch == Channel::Height ? Px{0.5f, 0.5f, 0.5f, 0} : Px{0, 0, 0, 0});
    if (cached) { for (int y = 0; y < br.height(); y++) { const int gy = br.y() + y; for (int x = 0; x < br.width(); x++) { const int gx = br.x() + x;   // (the kept mix under the layer, then the layer and those above)
          const auto f = cacheTiles_.find(keyOf(gx / T, gy / T)); if (f == cacheTiles_.end()) continue; const int tw = std::min(T, w_ - (gx / T) * T);
          buf[size_t(y) * br.width() + x] = f->second[size_t(gy % T) * tw + (gx % T)]; } }
      composeScope(ch, -1, br, buf, cacheLayer_); }
    else composeScope(ch, -1, br, buf);
    for (int y = 0; y < br.height(); y++) {
      uchar* line = base + bpl * (y0 + y); const Px* src = buf.data() + size_t(y) * br.width();
      for (int x = 0; x < br.width(); x++) {
        const Px& p = src[x];
        if (stg == Storage::Colour) reinterpret_cast<QRgb*>(line)[x] = qRgba(int(clamp01(p.r) * 255 + 0.5f), int(clamp01(p.g) * 255 + 0.5f), int(clamp01(p.b) * 255 + 0.5f), int(clamp01(p.a) * 255 + 0.5f));
        else if (ch == Channel::Height) reinterpret_cast<quint16*>(line)[x] = quint16(clamp01(p.r) * 65535 + 0.5f);
        else { uchar* q = line + x * 4; q[0] = uchar(clamp01(p.r * 0.5f + 0.5f) * 255 + 0.5f); q[1] = uchar(clamp01(p.g * 0.5f + 0.5f) * 255 + 0.5f); q[2] = 128; q[3] = uchar(clamp01(p.a) * 255 + 0.5f); }
      }
    }
  } };
  const int H = r.height(), n = qint64(r.width()) * H >= 512 * 512 ? std::clamp(int(std::thread::hardware_concurrency()), 1, 12) : 1;
  if (n <= 1) band(0, H);
  else { std::vector<std::thread> ts; const int step = (H + n - 1) / n; for (int i = 0; i < n; i++) ts.emplace_back(band, i * step, std::min(H, (i + 1) * step)); for (auto& t : ts) t.join(); }
  return out;
}

// ---- file ----

QJsonObject Document::toJson() const {
  QJsonObject o; o["w"] = w_; o["h"] = h_; o["next"] = nextId_; QJsonArray a;
  for (const Layer& l : layers_) {
    QJsonObject j; j["id"] = l.id; j["name"] = l.name; j["kind"] = int(l.kind); j["ch"] = int(l.channel); j["blend"] = blendKey(l.blend);
    j["op"] = l.opacity; j["vis"] = l.visible; j["lock"] = l.locked; j["parent"] = l.parent; j["maskOn"] = l.maskOn; if (l.clip) j["clip"] = true; if (l.lockAlpha) j["lockA"] = true; j["hasMask"] = !l.mask.isNull();
    j["fill"] = l.fill.name(); j["fillH"] = l.fillHeight; j["adj"] = adjustKey(l.adjust); j["p1"] = l.p1; j["p2"] = l.p2; j["p3"] = l.p3;
    if (!l.grad.empty()) { QJsonArray g; for (const auto& [at, c] : l.grad) g.append(QJsonArray{double(at), c.name()}); j["grad"] = g; }
    if (!l.fx.isEmpty()) j["fx"] = l.fx;
    { QJsonArray tn; bool any = false; for (const auto& pts : l.tones) { QJsonArray a; for (const QPointF& q : pts) a.append(QJsonArray{q.x(), q.y()}); any |= !pts.empty(); tn.append(a); } if (any) j["tones"] = tn; }
    if (l.gradLive) { j["gLive"] = true; j["g0"] = QJsonArray{l.g0[0], l.g0[1], l.g0[2]}; j["g1"] = QJsonArray{l.g1[0], l.g1[1], l.g1[2]}; j["liveKind"] = l.liveKind;
      if (!l.curve.empty()) { QJsonArray ca; for (const auto& c : l.curve) ca.append(QJsonArray{c[0], c[1], c[2]}); j["curve"] = ca; }
      QJsonArray sa; for (const Layer::Sphere& sp : l.spheres) sa.append(QJsonArray{sp.c[0], sp.c[1], sp.c[2], sp.r, sp.hard, sp.col.name(), sp.link}); if (!sa.isEmpty()) j["spheres"] = sa; }
    if (!l.onlySets.isEmpty()) j["onlySets"] = QJsonArray::fromStringList(l.onlySets);
    a.push_back(j);
  }
  o["layers"] = a;
  if (!set_.isEmpty() || !sets_.empty()) { o["set"] = set_; QJsonArray ss; for (const SetInfo& x : sets_) ss.push_back(QJsonObject{{"key", x.key}, {"w", x.w}, {"h", x.h}}); o["sets"] = ss; }
  return o;
}

bool Document::fromJson(const QJsonObject& o) {
  reset(o.value("w").toInt(2048), o.value("h").toInt(2048)); nextId_ = o.value("next").toInt(1);
  for (const QJsonValue& v : o.value("layers").toArray()) {
    const QJsonObject j = v.toObject(); Layer l; l.id = j.value("id").toInt(); l.name = j.value("name").toString(); l.kind = LayerKind(std::clamp(j.value("kind").toInt(), 0, 3)); l.channel = Channel(std::clamp(j.value("ch").toInt(), 0, int(Channel::Count) - 1));
    l.blend = blendFromKey(j.value("blend").toString()); l.opacity = float(j.value("op").toDouble(1)); l.visible = j.value("vis").toBool(true); l.locked = false;   // (no Locked any more — the author, 17.11: kept flags of older files no longer stop the brush)
    l.parent = j.value("parent").toInt(-1); l.maskOn = j.value("maskOn").toBool(true); l.clip = j.value("clip").toBool(); l.lockAlpha = false; l.fill = QColor(j.value("fill").toString("#808080")); l.fillHeight = float(j.value("fillH").toDouble());   // (lockAlpha: no lock of the transparency any more — the author, 17.11)
    l.adjust = adjustFromKey(j.value("adj").toString()); l.p1 = float(j.value("p1").toDouble()); l.p2 = float(j.value("p2").toDouble()); l.p3 = float(j.value("p3").toDouble());
    for (const QJsonValue& g : j.value("grad").toArray()) { const QJsonArray a = g.toArray(); if (a.size() == 2) l.grad.push_back({float(a[0].toDouble()), QColor(a[1].toString())}); }
    l.fx = j.value("fx").toObject();
    { const QJsonArray tn = j.value("tones").toArray(); for (int c = 0; c < 4 && c < tn.size(); c++) for (const QJsonValue& q : tn[c].toArray()) { const QJsonArray a = q.toArray(); if (a.size() == 2) l.tones[size_t(c)].push_back(QPointF(a[0].toDouble(), a[1].toDouble())); } }
    l.gradLive = j.value("gLive").toBool(); { const QJsonArray a0 = j.value("g0").toArray(), a1 = j.value("g1").toArray(); for (int k = 0; k < 3 && k < a0.size() && k < a1.size(); k++) { l.g0[k] = float(a0[k].toDouble()); l.g1[k] = float(a1[k].toDouble()); } }
    for (const QJsonValue& x : j.value("onlySets").toArray()) l.onlySets << x.toString();
    l.liveKind = j.value("liveKind").toString("linear");
    for (const QJsonValue& x : j.value("curve").toArray()) { const QJsonArray a = x.toArray(); if (a.size() == 3) l.curve.push_back({float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble())}); }
    for (const QJsonValue& x : j.value("spheres").toArray()) { const QJsonArray a = x.toArray(); if (a.size() < 7) continue; Layer::Sphere sp;
      for (int k = 0; k < 3; k++) sp.c[k] = float(a[k].toDouble()); sp.r = float(a[3].toDouble()); sp.hard = float(a[4].toDouble()); sp.col = QColor(a[5].toString()); sp.link = a[6].toInt(); l.spheres.push_back(sp); }
    if (l.kind == LayerKind::Paint) l.image = blank(l.channel);
    if (j.value("hasMask").toBool()) { l.mask = QImage(w_, h_, QImage::Format_Grayscale8); l.mask.fill(255); }
    layers_.push_back(std::move(l)); nextId_ = std::max(nextId_, layers_.back().id + 1);
  }
  set_ = o.value("set").toString();
  for (const QJsonValue& v : o.value("sets").toArray()) { const QJsonObject j = v.toObject(); sets_.push_back({j.value("key").toString(), j.value("w").toInt(), j.value("h").toInt()}); }
  return true;
}

// ---- the UV sets ----

void Document::noteSet(const QString& key, int w, int h) { for (SetInfo& x : sets_) if (x.key == key) { x.w = w; x.h = h; return; } sets_.push_back({key, w, h}); }

void Document::setActiveSet(const QString& key, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (key != set_) {
    if (w_ > 0) noteSet(set_, w_, h_);
    if (!dir_.isEmpty() && !loaded_[key]) loadSet(key, true);       // (its pixels in the project's folder: read once, into the parked places)
    for (Layer& l : layers_) {
      if (!l.image.isNull() || !l.mask.isNull()) { Layer::Pixels& p = l.other[set_]; p.image = std::move(l.image); p.mask = std::move(l.mask); }
      l.image = {}; l.mask = {};
      if (auto f = l.other.find(key); f != l.other.end()) { l.image = std::move(f->second.image); l.mask = std::move(f->second.mask); l.other.erase(f); }
    }
    set_ = key;
  }
  // (the set's texture was given another size since: its layers follow)
  // (every picture not of that size — not only "when the size differs from the set shown before": a set resized to that very size kept its old pictures, read past their end)
  for (Layer& l : layers_) {
    if (!l.image.isNull() && l.image.size() != QSize(w, h)) l.image = l.image.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(formatOf(l.channel));
    if (!l.mask.isNull() && l.mask.size() != QSize(w, h)) l.mask = l.mask.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
  }
  w_ = w; h_ = h; noteSet(key, w, h); loaded_[key] = true;
}

QSize Document::setSize(const QString& key) const { if (key == set_) return QSize(w_, h_); for (const SetInfo& x : sets_) if (x.key == key) return QSize(x.w, x.h); return {}; }
void Document::declareSet(const QString& key, int w, int h) { if (key == set_ || w <= 0 || h <= 0) return; for (const SetInfo& x : sets_) if (x.key == key) return; sets_.push_back({key, w, h}); }
QImage* Document::pixelsIn(int id, const QString& set, bool make) {
  Layer* l = layer(id); if (!l || l->kind != LayerKind::Paint) return nullptr;
  if (set == set_) { if (l->image.isNull() && make) ensurePixels(id); return l->image.isNull() ? nullptr : &l->image; }
  const QSize sz = setSize(set); if (sz.isEmpty()) return nullptr;
  if (!dir_.isEmpty() && !loaded_[set]) loadSet(set, true);       // (its pixels in the project's folder: read once)
  loaded_[set] = true;
  Layer::Pixels& p = l->other[set];
  if (!p.image.isNull() && p.image.size() != sz) p.image = p.image.scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(formatOf(l->channel));
  if (p.image.isNull()) { if (!make || mapInfo(l->channel).storage != Storage::Colour) return nullptr; p.image = QImage(sz, QImage::Format_ARGB32); p.image.fill(Qt::transparent); }
  return &p.image;
}
void Document::ensureLoaded(const QString& set) { if (set == set_ || dir_.isEmpty() || loaded_[set]) return; loadSet(set, true); }
bool Document::hasPixelsIn(const QString& set) const {
  for (const Layer& l : layers_) { if (set == set_) { if (!l.image.isNull()) return true; continue; } const auto f = l.other.find(set); if (f != l.other.end() && !f->second.image.isNull()) return true; }
  return false;
}
// (the set shown for the time of the composing only: its pictures change places with the shown one's and back — no pixel is copied)
QImage Document::composeSet(const QString& set, Channel ch, const QRect& r) {
  if (set == set_) return compose(ch, r); const QSize sz = setSize(set); if (sz.isEmpty()) return {};
  const QString cur = set_; const int cw = w_, chh = h_;
  setActiveSet(set, sz.width(), sz.height()); QImage c = compose(ch, r); setActiveSet(cur, cw, chh);
  return c;
}

void Document::ensurePixels(int id) { Layer* l = layer(id); if (l && l->kind == LayerKind::Paint && l->image.isNull() && w_ > 0) l->image = blank(l->channel); }

bool Document::hasLayers(Channel ch) const { for (const Layer& l : layers_) if (l.visible && l.channel == ch && (l.kind == LayerKind::Fill || (l.kind == LayerKind::Paint && !l.image.isNull()))) return true; return false; }

QImage Document::flatten(Channel ch, bool keepAlpha) const {
  const MapInfo& mi = mapInfo(ch); QImage c = compose(ch); if (c.isNull() || mi.storage != Storage::Colour) return c;
  const int gr = qRed(mi.ground), gg = qGreen(mi.ground), gb = qBlue(mi.ground);
  if (keepAlpha && !mi.gray) return c;
  QImage out(c.size(), mi.gray ? QImage::Format_Grayscale8 : QImage::Format_RGB888);
  for (int y = 0; y < c.height(); y++) { const QRgb* p = reinterpret_cast<const QRgb*>(c.constScanLine(y)); uchar* o = out.scanLine(y);
    for (int x = 0; x < c.width(); x++) { const int a = qAlpha(p[x]); const int r = (qRed(p[x]) * a + gr * (255 - a) + 127) / 255, g = (qGreen(p[x]) * a + gg * (255 - a) + 127) / 255, b = (qBlue(p[x]) * a + gb * (255 - a) + 127) / 255;
      if (mi.gray) o[x] = uchar((r * 299 + g * 587 + b * 114 + 500) / 1000); else { o[x * 3] = uchar(r); o[x * 3 + 1] = uchar(g); o[x * 3 + 2] = uchar(b); } } }
  return out;
}

namespace {
// jobs spread over the cores (each a picture of its own: nothing shared between them)
// (bytesEach — what one job holds while it works: as many at once as fit in about 1.5 GB, so a set of 8K layers is not all in
// memory a second time while it is written or read)
template <class F> void inParallel(size_t n, qint64 bytesEach, F fn) {
  if (n <= 1) { if (n) fn(size_t(0)); return; }
  std::atomic<size_t> next{0}; auto work = [&] { for (;;) { const size_t i = next.fetch_add(1); if (i >= n) return; fn(i); } };
  const size_t room = size_t(std::clamp<qint64>((qint64(3) << 29) / std::max<qint64>(bytesEach, 1), 2, 8));
  const size_t nt = std::min({n, room, std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 8)});
  std::vector<std::thread> ts; for (size_t i = 1; i < nt; i++) ts.emplace_back(work); work(); for (auto& t : ts) t.join();
}
// (written beside the file and put in its place when whole: a save cut short leaves the layer as it was saved before. PNG at a light
// compression — a big layer is written several times faster, a little larger)
bool savePng(const QImage& im, const QString& path) {
  QSaveFile f(path); if (!f.open(QIODevice::WriteOnly)) return false;
  if (!im.save(&f, "PNG", 80)) { f.cancelWriting(); return false; }
  return f.commit();
}
}  // namespace

// a layer's file: layer_<id>.png in the project's single texture, layer_<id>@<set>.png in a named UV set (the name made safe for a file)
QString Document::fileOf(const QString& kind, int id, const QString& set) {
  return set.isEmpty() ? QString("%1_%2.png").arg(kind).arg(id) : QString("%1_%2@%3.png").arg(kind).arg(id).arg(QString::fromLatin1(QUrl::toPercentEncoding(set)));
}

// only what changed since it was last written to (or read from) this folder: a layer is known by its picture's key, which Qt
// changes at every edit — a set of twenty 8K layers with one of them painted on writes that one. Every UV set's pixels are written;
// the files of layers that are no longer in the list are removed (a deleted layer's megabytes don't stay in the project for good)
// In three steps, so the slow one can run off the UI thread while painting goes on: savePlan — what has to be written (the pictures
// held as they are now: a layer painted on meanwhile parts from its copy by itself); saveWrite — the PNGs made and written (any
// thread); saveFinish — what was written noted, the files no layer owns removed (the thread of the document again)
Document::SavePlan Document::savePlan(const QString& dir) const {
  SavePlan p; p.dir = dir; p.set = set_; QDir d(dir); if (!d.mkpath(".")) return p; p.ok = true;
  Document* self = const_cast<Document*>(this);
  p.sameDir = !dir_.isEmpty() && QDir(dir_) == d;
  if (!dir_.isEmpty() && !p.sameDir) for (const SetInfo& x : sets_) if (x.key != set_ && !self->loaded_[x.key]) self->loadSet(x.key, true);   // (saved elsewhere: the sets not looked at yet go along)
  p.bytesEach = qint64(w_) * h_ * 4; p.loaded = loaded_;   // (the sets read by now: one first read while the files are being written keeps its files — they are not in keep)
  auto want = [&](const QImage& im, const QString& name) { p.keep.insert(name); const QString path = d.filePath(name); auto f = saved_.find(path);
    if (f != saved_.end() && f->second == im.cacheKey() && QFileInfo::exists(path)) return; p.jobs.push_back({im, path, im.cacheKey()}); };
  for (const Layer& l : layers_) {
    if (l.kind == LayerKind::Paint && !l.image.isNull()) want(l.image, fileOf("layer", l.id, set_));
    if (!l.mask.isNull()) want(l.mask, fileOf("mask", l.id, set_));
    for (const auto& [k, px] : l.other) { if (l.kind == LayerKind::Paint && !px.image.isNull()) want(px.image, fileOf("layer", l.id, k)); if (!px.mask.isNull()) want(px.mask, fileOf("mask", l.id, k)); }
  }
  return p;
}
std::vector<char> Document::saveWrite(const SavePlan& p) {
  std::vector<char> ok(p.jobs.size(), 0);
  inParallel(p.jobs.size(), p.bytesEach, [&](size_t i) { ok[i] = savePng(p.jobs[i].im, p.jobs[i].path); });
  return ok;
}
bool Document::saveFinish(const SavePlan& p, const std::vector<char>& ok) const {
  if (!p.ok) return false; Document* self = const_cast<Document*>(this); QDir d(p.dir);
  bool all = true; for (size_t i = 0; i < p.jobs.size(); i++) { if (i < ok.size() && ok[i]) saved_[p.jobs[i].path] = p.jobs[i].key; else all = false; }
  // (a set not read from this folder yet keeps its files; of the others — what no layer owns goes)
  static const QRegularExpression re("^(layer|mask)_(\\d+)(@(.*))?\\.png$");
  if (all) for (const QString& f : d.entryList({"layer_*.png", "mask_*.png"}, QDir::Files)) { const auto m = re.match(f); if (!m.hasMatch() || p.keep.contains(f)) continue;
    const QString set = QUrl::fromPercentEncoding(m.captured(4).toLatin1()); const auto lf = p.loaded.find(set);
    if (set == p.set || !p.sameDir || (lf != p.loaded.end() && lf->second)) QFile::remove(d.filePath(f)); }
  if (p.mine) { self->dir_ = p.dir; self->loaded_[p.set] = true; }   // (the project's own folder: where the other sets are read from. A copy written elsewhere — an autosave — changes nothing of that)
  return all;
}
bool Document::saveTo(const QString& dir) const { const SavePlan p = savePlan(dir); return saveFinish(p, saveWrite(p)); }

void Document::loadSet(const QString& key, bool parked) {
  loaded_[key] = true; if (dir_.isEmpty()) return; const QDir d(dir_);
  QSize sz(w_, h_); if (parked) { sz = QSize(); for (const SetInfo& x : sets_) if (x.key == key) sz = QSize(x.w, x.h); }
  struct Job { Layer* l; bool mask; QString path; qint64 key = 0; QImage out; };
  std::vector<Job> jobs;
  for (Layer& l : layers_) {
    if (l.kind == LayerKind::Paint && QFileInfo::exists(d.filePath(fileOf("layer", l.id, key)))) jobs.push_back({&l, false, d.filePath(fileOf("layer", l.id, key))});
    if (QFileInfo::exists(d.filePath(fileOf("mask", l.id, key)))) jobs.push_back({&l, true, d.filePath(fileOf("mask", l.id, key))});
  }
  // (each put into its layer as it is read — the blank picture it replaces let go at once, not when all are in)
  inParallel(jobs.size(), qint64(std::max(1, sz.width())) * std::max(1, sz.height()) * 4, [&](size_t i) { Job& j = jobs[i]; QImage im(j.path); if (im.isNull() || (sz.isValid() && im.size() != sz)) return;
    im = im.convertToFormat(j.mask ? QImage::Format_Grayscale8 : formatOf(j.l->channel)); j.key = im.cacheKey();
    if (parked) j.out = std::move(im); else (j.mask ? j.l->mask : j.l->image) = std::move(im); });
  for (Job& j : jobs) { if (!j.key) continue; saved_[j.path] = j.key;   // (as in the file: not written again until it changes)
    if (parked) { Layer::Pixels& p = j.l->other[key]; (j.mask ? p.mask : p.image) = std::move(j.out); } }
}

bool Document::loadFrom(const QString& dir) {
  const QDir d(dir); if (!d.exists()) return false;
  // (a paint layer has pixels only in the sets it was painted in: what has no file here stays empty until the brush comes)
  for (Layer& l : layers_) if (l.kind == LayerKind::Paint) l.image = {};
  dir_ = dir; loaded_.clear(); loadSet(set_, false);
  return true;
}

}  // namespace hm::paint

namespace hm::paint {

// the document with its pixels in one record (a reference plane's ✏ drawing is kept inside the project file): the layers' records
// and each paint layer / mask as a PNG in base64
static QString pngB64(const QImage& im) { QByteArray b; QBuffer buf(&b); buf.open(QIODevice::WriteOnly); im.save(&buf, "PNG"); return QString::fromLatin1(b.toBase64()); }
QJsonObject Document::packJson() const {
  QJsonObject o = toJson(), px;
  for (const Layer& l : layers_) {
    if (l.kind == LayerKind::Paint && !l.image.isNull()) px[QString::number(l.id)] = pngB64(l.image);
    if (!l.mask.isNull()) px["m" + QString::number(l.id)] = pngB64(l.mask);
  }
  o["px"] = px; return o;
}
bool Document::unpackJson(const QJsonObject& o) {
  if (!o.contains("layers")) return false;
  fromJson(o); const QJsonObject px = o.value("px").toObject();
  auto img = [&](const QString& k) { return QImage::fromData(QByteArray::fromBase64(px.value(k).toString().toLatin1()), "PNG"); };
  for (Layer& l : layers_) {
    if (l.kind == LayerKind::Paint) { QImage im = img(QString::number(l.id));
      if (!im.isNull() && im.size() == QSize(w_, h_)) l.image = im.convertToFormat(formatOf(l.channel)); }
    if (!l.mask.isNull()) { QImage m = img("m" + QString::number(l.id)); if (!m.isNull() && m.size() == QSize(w_, h_)) l.mask = m.convertToFormat(QImage::Format_Grayscale8); }
  }
  return true;
}

}  // namespace hm::paint
