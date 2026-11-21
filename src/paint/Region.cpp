#include "paint/Region.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hm::paint {

// ---- the freeze mask ----
QImage boxBlur8(const QImage& g, int radius, int passes) {
  if (g.isNull() || radius < 1) return g;
  QImage a = g.convertToFormat(QImage::Format_Grayscale8), b(a.size(), QImage::Format_Grayscale8);
  const int W = a.width(), H = a.height(), n = 2 * radius + 1;
  std::vector<int> col(size_t(std::max(W, H)));
  for (int p = 0; p < passes; p++) {
    for (int y = 0; y < H; y++) {                                  // (along the rows: a → b)
      const uchar* s = a.constScanLine(y); uchar* d = b.scanLine(y); int sum = 0;
      for (int x = -radius; x <= radius; x++) sum += s[std::clamp(x, 0, W - 1)];
      for (int x = 0; x < W; x++) { d[x] = uchar((sum + n / 2) / n); sum += s[std::min(W - 1, x + radius + 1)] - s[std::max(0, x - radius)]; }
    }
    for (int x = 0; x < W; x++) {                                  // (down the columns: b → a)
      int sum = 0; for (int y = -radius; y <= radius; y++) sum += b.constScanLine(std::clamp(y, 0, H - 1))[x];
      for (int y = 0; y < H; y++) { col[size_t(y)] = (sum + n / 2) / n; sum += b.constScanLine(std::min(H - 1, y + radius + 1))[x] - b.constScanLine(std::max(0, y - radius))[x]; }
      for (int y = 0; y < H; y++) a.scanLine(y)[x] = uchar(col[size_t(y)]);
    }
  }
  return a;
}

namespace {
// separable running max / min / mean over a square window (radius r), Grayscale8
QImage passH(const QImage& s, int r, int kind) {   // kind 0 — max, 1 — min, 2 — mean
  QImage d(s.size(), QImage::Format_Grayscale8); const int W = s.width();
  std::vector<int> row(size_t(W));
  for (int y = 0; y < s.height(); y++) { const uchar* a = s.constScanLine(y); uchar* o = d.scanLine(y);
    if (kind == 2) { long acc = 0; for (int x = -r; x <= r; x++) acc += a[std::clamp(x, 0, W - 1)];
      for (int x = 0; x < W; x++) { o[x] = uchar(acc / (2 * r + 1)); acc += a[std::min(W - 1, x + r + 1)] - a[std::max(0, x - r)]; } }
    else for (int x = 0; x < W; x++) { int v = kind == 0 ? 0 : 255; for (int k = std::max(0, x - r); k <= std::min(W - 1, x + r); k++) v = kind == 0 ? std::max(v, int(a[k])) : std::min(v, int(a[k])); o[x] = uchar(v); } }
  return d; }
QImage transposed(const QImage& s) { QImage d(s.height(), s.width(), QImage::Format_Grayscale8); for (int y = 0; y < s.height(); y++) { const uchar* a = s.constScanLine(y); for (int x = 0; x < s.width(); x++) d.scanLine(x)[y] = a[x]; } return d; }
QImage filt(const QImage& s, int r, int kind) { return transposed(passH(transposed(passH(s, r, kind)), r, kind)); }
}  // namespace

QImage maskEdge(const QImage& m0, EdgeOp op, float px, const IslandMap* im, QPoint at, QSize tex) {
  if (m0.isNull()) return m0; const QImage m = m0.convertToFormat(QImage::Format_Grayscale8); const int r = std::max(1, int(std::lround(px)));
  QImage out;
  if (op == EdgeOp::Expand) out = filt(m, r, 0);
  else if (op == EdgeOp::Contract) out = filt(m, r, 1);
  else if (op == EdgeOp::Smooth) {   // (the staircase smoothed, the edge kept crisp: the contour rounded by a blur of about px, then cut at half again with a 1-texel ramp)
    const int q = std::max(1, r / 2); const QImage b = filt(filt(filt(m, q, 2), q, 2), q, 2); const double S = std::max(1.0, 1.6 * r); out = QImage(m.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < m.height(); y++) { const uchar* p = b.constScanLine(y); uchar* o = out.scanLine(y); for (int x = 0; x < m.width(); x++) o[x] = uchar(std::clamp(int(std::lround((p[x] - 127.5) * S + 127.5)), 0, 255)); } }
  else { const QImage a = filt(m, r, 0), b = filt(m, r, 1); out = QImage(m.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < m.height(); y++) { const uchar* p = a.constScanLine(y); const uchar* q = b.constScanLine(y); uchar* o = out.scanLine(y); for (int x = 0; x < m.width(); x++) o[x] = uchar(std::max(0, int(p[x]) - int(q[x]))); } }
  if (im && !im->ids.isNull() && op != EdgeOp::Contract) {   // (what grew stays on the islands the mask was on — no leak onto a neighbouring island)
    const QSize T = tex.isValid() ? tex : m.size(); const double kx = double(im->ids.width()) / T.width(), ky = double(im->ids.height()) / T.height();
    auto idAt = [&](int x, int y) { return im->at(int((x + at.x()) * kx), int((y + at.y()) * ky)); };
    std::vector<char> on(1 << 16, 0); bool any = false;
    for (int y = 0; y < m.height(); y++) { const uchar* a = m.constScanLine(y); for (int x = 0; x < m.width(); x++) if (a[x] > 8) { const int id = idAt(x, y); if (id > 0 && id < int(on.size())) { on[size_t(id)] = 1; any = true; } } }
    if (any) for (int y = 0; y < out.height(); y++) { uchar* o = out.scanLine(y); for (int x = 0; x < out.width(); x++) if (o[x]) { const int id = idAt(x, y); if (id > 0 && (id >= int(on.size()) || !on[size_t(id)])) o[x] = 0; } } }
  return out;
}

bool healRegion(QImage& dst, const QImage& src, const QImage& cov, const QRect& box0, int pad, const IslandMap* im) {
  if (dst.isNull() || src.size() != dst.size() || cov.size() != dst.size() || dst.format() != QImage::Format_ARGB32 || src.format() != QImage::Format_ARGB32) return false;
  const QRect box = box0.intersected(dst.rect()); if (box.isEmpty()) return false;
  const QRect win = box.adjusted(-pad, -pad, pad, pad).intersected(dst.rect()); const int W = win.width(), H = win.height();
  const double kx = im && !im->ids.isNull() ? double(im->ids.width()) / dst.width() : 1, ky = im && !im->ids.isNull() ? double(im->ids.height()) / dst.height() : 1;
  auto idAt = [&](int x, int y) { return im && !im->ids.isNull() ? im->at(int(x * kx), int(y * ky)) : 1; };
  // (the islands the mark lies on: only they lend colours — the next island over is another part of the model)
  std::vector<int> isl; for (int y = box.top(); y <= box.bottom(); y++) { const uchar* c = cov.constScanLine(y); for (int x = box.left(); x <= box.right(); x++) if (c[x]) { const int id = idAt(x, y); if (id > 0 && std::find(isl.begin(), isl.end(), id) == isl.end()) isl.push_back(id); } }
  if (isl.empty()) return false;
  struct Lv { int w, h; std::vector<float> c, n; };   // (premultiplied r g b a, and how many known texels went into each)
  std::vector<Lv> L(1); L[0] = {W, H, std::vector<float>(size_t(W) * H * 4, 0.f), std::vector<float>(size_t(W) * H, 0.f)};
  bool any = false;
  for (int y = 0; y < H; y++) { const QRgb* s = reinterpret_cast<const QRgb*>(src.constScanLine(win.top() + y)); const uchar* c = cov.constScanLine(win.top() + y);
    for (int x = 0; x < W; x++) { const int gx = win.left() + x; if (c[gx]) continue; const int id = idAt(gx, win.top() + y); if (std::find(isl.begin(), isl.end(), id) == isl.end()) continue;
      const QRgb q = s[gx]; const float a = qAlpha(q) / 255.f; float* o = &L[0].c[(size_t(y) * W + x) * 4]; o[0] = qRed(q) * a; o[1] = qGreen(q) * a; o[2] = qBlue(q) * a; o[3] = a; L[0].n[size_t(y) * W + x] = 1; any = true; } }
  if (!any) return false;
  while (L.back().w > 1 || L.back().h > 1) {             // (pushed down: each level the sums of the one below, 2 × 2)
    const Lv& a = L.back(); Lv b{(a.w + 1) / 2, (a.h + 1) / 2, {}, {}}; b.c.assign(size_t(b.w) * b.h * 4, 0.f); b.n.assign(size_t(b.w) * b.h, 0.f);
    for (int y = 0; y < a.h; y++) for (int x = 0; x < a.w; x++) { const size_t i = size_t(y) * a.w + x, j = size_t(y / 2) * b.w + x / 2; b.n[j] += a.n[i]; for (int k = 0; k < 4; k++) b.c[j * 4 + k] += a.c[i * 4 + k]; }
    L.push_back(std::move(b)); }
  // (pulled up: a level's known texels keep their mean, the unknown ones take the coarser level's, between its texels)
  std::vector<float> up; int uw = 0, uh = 0;
  for (int li = int(L.size()) - 1; li >= 0; li--) { Lv& a = L[size_t(li)]; std::vector<float> v(size_t(a.w) * a.h * 4, 0.f);
    for (int y = 0; y < a.h; y++) for (int x = 0; x < a.w; x++) { const size_t i = size_t(y) * a.w + x;
      if (a.n[i] > 0) { for (int k = 0; k < 4; k++) v[i * 4 + k] = a.c[i * 4 + k] / a.n[i]; continue; }
      if (up.empty()) continue;
      const double sx = std::clamp((x + 0.5) / 2 - 0.5, 0.0, uw - 1.0), sy = std::clamp((y + 0.5) / 2 - 0.5, 0.0, uh - 1.0); const int x0 = int(sx), y0 = int(sy), x1 = std::min(uw - 1, x0 + 1), y1 = std::min(uh - 1, y0 + 1); const float fx = float(sx - x0), fy = float(sy - y0);
      for (int k = 0; k < 4; k++) v[i * 4 + k] = (up[(size_t(y0) * uw + x0) * 4 + k] * (1 - fx) + up[(size_t(y0) * uw + x1) * 4 + k] * fx) * (1 - fy) + (up[(size_t(y1) * uw + x0) * 4 + k] * (1 - fx) + up[(size_t(y1) * uw + x1) * 4 + k] * fx) * fy; }
    up.swap(v); uw = a.w; uh = a.h; }
  for (int y = box.top(); y <= box.bottom(); y++) { QRgb* d = reinterpret_cast<QRgb*>(dst.scanLine(y)); const QRgb* s = reinterpret_cast<const QRgb*>(src.constScanLine(y)); const uchar* c = cov.constScanLine(y);
    for (int x = box.left(); x <= box.right(); x++) { if (!c[x] || idAt(x, y) <= 0) continue;
      const float* f = &up[(size_t(y - win.top()) * W + (x - win.left())) * 4]; const float k = c[x] / 255.f; const QRgb q = s[x]; const float sa = qAlpha(q) / 255.f;
      const float a = sa * (1 - k) + f[3] * k; if (a <= 0.002f) { d[x] = qRgba(qRed(q), qGreen(q), qBlue(q), 0); continue; }
      auto ch = [&](int sc, float fc) { return int(std::clamp((sc * sa * (1 - k) + fc * k) / a, 0.f, 255.f) + 0.5f); };
      d[x] = qRgba(ch(qRed(q), f[0]), ch(qGreen(q), f[1]), ch(qBlue(q), f[2]), int(a * 255 + 0.5f)); } }
  return true;
}

QImage maskSolid(const QImage& m, int threshold, float smoothPx, float softPx) {
  if (m.isNull()) return m;
  QImage o = m.convertToFormat(QImage::Format_Grayscale8); const int W = o.width(), H = o.height(), thr = std::clamp(threshold, 1, 255);
  auto binarize = [&](int at) { for (int y = 0; y < H; y++) { uchar* p = o.scanLine(y); for (int x = 0; x < W; x++) p[x] = p[x] >= at ? 255 : 0; } };
  binarize(thr);
  if (smoothPx >= 0.75f) { o = boxBlur8(o, std::max(1, int(std::lround(smoothPx))), 2); binarize(128); }   // (the contour rounded: the stairs gone)
  if (softPx >= 0.75f) { const int passes = softPx < 3 ? 1 : 2; o = boxBlur8(o, std::max(1, int(std::lround(softPx / (2 * passes)))), passes); }   // (its edge: a passage about that wide — a box of radius r run p times spreads a step over 2·r·p texels)
  return o;
}

QImage floodFree(const QImage& m, const QPoint& at, int threshold, const IslandMap* isl, int grow, QRect* box) {
  if (m.isNull() || !m.rect().contains(at) || m.format() != QImage::Format_Grayscale8) return {};
  const int W = m.width(), H = m.height(), thr = std::clamp(threshold, 1, 255);
  const bool useIsl = isl && !isl->ids.isNull() && isl->ids.size() == m.size(); const int id0 = useIsl ? isl->at(at.x(), at.y()) : 0;
  if (useIsl && id0 == 0) return {};
  auto open = [&](int x, int y) { return m.constScanLine(y)[x] < thr && (!useIsl || isl->at(x, y) == id0); };
  if (!open(at.x(), at.y())) return {};
  QImage out(W, H, QImage::Format_Grayscale8); out.fill(0);
  std::vector<QPoint> stack{at}; int x0 = at.x(), x1 = at.x(), y0 = at.y(), y1 = at.y();
  while (!stack.empty()) {                                         // (span by span)
    const QPoint p = stack.back(); stack.pop_back(); const int y = p.y(); uchar* o = out.scanLine(y);
    if (o[p.x()] || !open(p.x(), y)) continue;
    int l = p.x(), r = p.x(); while (l > 0 && !o[l - 1] && open(l - 1, y)) l--; while (r + 1 < W && !o[r + 1] && open(r + 1, y)) r++;
    std::memset(o + l, 255, size_t(r - l + 1)); x0 = std::min(x0, l); x1 = std::max(x1, r); y0 = std::min(y0, y); y1 = std::max(y1, y);
    for (int ny : {y - 1, y + 1}) { if (ny < 0 || ny >= H) continue; const uchar* q = out.constScanLine(ny); bool in = false;
      for (int x = l; x <= r; x++) { const bool ok = !q[x] && open(x, ny); if (ok && !in) stack.push_back(QPoint(x, ny)); in = ok; } }
  }
  QRect b(QPoint(x0, y0), QPoint(x1, y1));
  if (grow > 0) {                                                  // (onto the lines' soft edge — still inside the island)
    b = b.adjusted(-grow, -grow, grow, grow).intersected(out.rect()); const QImage part = out.copy(b); const QImage g = boxBlur8(part, grow, 1);
    for (int y = 0; y < b.height(); y++) { uchar* o = out.scanLine(b.y() + y) + b.x(); const uchar* s = g.constScanLine(y);
      for (int x = 0; x < b.width(); x++) if (s[x] && !o[x] && (!useIsl || isl->at(b.x() + x, b.y() + y) == id0)) o[x] = 255; }
  }
  if (box) *box = b;
  return out;
}

void keepTexels(QImage& target, const QImage& before, int y, int x0, int x1, const uchar* hold) {
  uchar* t = target.scanLine(y); const uchar* b = before.constScanLine(y); const int bpp = target.depth() / 8;
  if (target.format() == QImage::Format_ARGB32) {
    QRgb* tq = reinterpret_cast<QRgb*>(t); const QRgb* bq = reinterpret_cast<const QRgb*>(b);
    for (int x = x0; x <= x1; x++) { const int f = hold[x]; if (!f) continue; if (f == 255) { tq[x] = bq[x]; continue; }
      const int ab = qAlpha(bq[x]), at = qAlpha(tq[x]), wb = ab * f, wt = at * (255 - f), den = wb + wt;   // (premultiplied: each colour by its own density)
      if (den <= 0) { tq[x] = 0; continue; }
      tq[x] = qRgba((qRed(bq[x]) * wb + qRed(tq[x]) * wt + den / 2) / den, (qGreen(bq[x]) * wb + qGreen(tq[x]) * wt + den / 2) / den, (qBlue(bq[x]) * wb + qBlue(tq[x]) * wt + den / 2) / den, (den + 127) / 255); }
    return;
  }
  if (target.format() == QImage::Format_Grayscale16) {
    quint16* tq = reinterpret_cast<quint16*>(t); const quint16* bq = reinterpret_cast<const quint16*>(b);
    for (int x = x0; x <= x1; x++) { const int f = hold[x]; if (!f) continue; tq[x] = f == 255 ? bq[x] : quint16(int(bq[x]) + (int(tq[x]) - int(bq[x])) * (255 - f) / 255); }
    return;
  }
  for (int x = x0; x <= x1; x++) { const int f = hold[x]; if (!f) continue;
    if (f == 255) { std::memcpy(t + x * bpp, b + x * bpp, size_t(bpp)); continue; }
    for (int c = 0; c < bpp; c++) { const int bi = b[x * bpp + c], ti = t[x * bpp + c]; t[x * bpp + c] = uchar(bi + (ti - bi) * (255 - f) / 255); } }
}

// ---- an effect over a region ----
namespace {
struct OpInfo { RegionOp o; const char* key; const char* name; };
const OpInfo kOps[] = {{RegionOp::Paint, "paint", "Paint"}, {RegionOp::Erase, "erase", "Erase"}, {RegionOp::Blur, "blur", "Blur"}, {RegionOp::Sharpen, "sharpen", "Sharpen"},
  {RegionOp::Hue, "hue", "Hue & saturation"}, {RegionOp::Lighten, "lighten", "Lighten"}, {RegionOp::Darken, "darken", "Darken"}, {RegionOp::Saturate, "saturate", "Saturate"}, {RegionOp::Desaturate, "desaturate", "Desaturate"}};

void toHsl(float r, float g, float b, float& h, float& s, float& l) {
  const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn; l = (mx + mn) / 2;
  if (d < 1e-6f) { h = 0; s = 0; return; }
  s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
  h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4; h /= 6;
}
float hue2rgb(float p, float q, float t) { if (t < 0) t += 1; if (t > 1) t -= 1; if (t < 1 / 6.f) return p + (q - p) * 6 * t; if (t < 0.5f) return q; if (t < 2 / 3.f) return p + (q - p) * (2 / 3.f - t) * 6; return p; }
void fromHsl(float h, float s, float l, float& r, float& g, float& b) {
  if (s < 1e-6f) { r = g = b = l; return; }
  const float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q; r = hue2rgb(p, q, h + 1 / 3.f); g = hue2rgb(p, q, h); b = hue2rgb(p, q, h - 1 / 3.f);
}
// the mean of the texels round a point of the layer as it was (premultiplied: an empty texel has no colour to give)
void meanAt(const QImage& src, int cx, int cy, int rad, float& r, float& g, float& b, float& a) {
  const int W = src.width(), H = src.height(); double R = 0, G = 0, B = 0, A = 0; int n = 0;
  for (int y = std::max(0, cy - rad); y <= std::min(H - 1, cy + rad); y++) { const QRgb* p = reinterpret_cast<const QRgb*>(src.constScanLine(y));
    for (int x = std::max(0, cx - rad); x <= std::min(W - 1, cx + rad); x++) { const double al = qAlpha(p[x]); R += qRed(p[x]) * al; G += qGreen(p[x]) * al; B += qBlue(p[x]) * al; A += al; n++; } }
  if (A <= 0 || n == 0) { r = g = b = a = 0; return; }
  r = float(R / A); g = float(G / A); b = float(B / A); a = float(A / n);
}
}  // namespace

QString regionOpKey(RegionOp o) { for (const OpInfo& i : kOps) if (i.o == o) return QLatin1String(i.key); return "paint"; }
QString regionOpName(RegionOp o) { for (const OpInfo& i : kOps) if (i.o == o) return QLatin1String(i.name); return "Paint"; }
RegionOp regionOpFromKey(const QString& k) { for (const OpInfo& i : kOps) if (k == QLatin1String(i.key)) return i.o; return RegionOp::Paint; }

void applyRegion(QImage& layer, const QImage& src, const QImage& cov, const QPoint& covAt, const QRect& r0, RegionOp op, const QColor& ink, float strength) {
  if (layer.format() != QImage::Format_ARGB32 || src.size() != layer.size() || src.format() != layer.format() || cov.isNull()) return;
  const QRect r = r0.intersected(layer.rect()).intersected(QRect(covAt, cov.size())); if (r.isEmpty()) return;
  const float st = std::clamp(strength, 0.f, 1.f); float ih = 0, is = 0, il = 0; toHsl(ink.redF(), ink.greenF(), ink.blueF(), ih, is, il);
  const int ia0 = ink.alpha();
  for (int y = r.top(); y <= r.bottom(); y++) {
    QRgb* d = reinterpret_cast<QRgb*>(layer.scanLine(y)); const QRgb* s = reinterpret_cast<const QRgb*>(src.constScanLine(y)); const uchar* c = cov.constScanLine(y - covAt.y()) - covAt.x();
    for (int x = r.left(); x <= r.right(); x++) {
      const int cv = c[x]; if (!cv) { continue; }
      const QRgb sq = s[x]; const int sa = qAlpha(sq); const float k = cv / 255.f * st;
      if (op == RegionOp::Paint) {                                 // (the colour over what was there, once)
        const int ia = int(std::lround(ia0 * (cv / 255.f) * st)), oa = ia + sa * (255 - ia) / 255; if (oa <= 0) continue;
        d[x] = qRgba((ink.red() * ia + qRed(sq) * sa * (255 - ia) / 255) / oa, (ink.green() * ia + qGreen(sq) * sa * (255 - ia) / 255) / oa, (ink.blue() * ia + qBlue(sq) * sa * (255 - ia) / 255) / oa, oa); continue; }
      if (op == RegionOp::Erase) { d[x] = qRgba(qRed(sq), qGreen(sq), qBlue(sq), int(std::lround(sa * (1 - k)))); continue; }
      if (op == RegionOp::Blur || op == RegionOp::Sharpen) {
        float mr, mg, mb, ma; meanAt(src, x, y, op == RegionOp::Blur ? 3 : 1, mr, mg, mb, ma);
        if (op == RegionOp::Blur) { const float oa = sa + (ma - sa) * k; if (oa <= 0.5f) { d[x] = 0; continue; }   // (premultiplied: the colours run together by their density)
          const float ws = sa * (1 - k), wm = ma * k, den = std::max(1e-6f, ws + wm);
          d[x] = qRgba(int(std::lround((qRed(sq) * ws + mr * wm) / den)), int(std::lround((qGreen(sq) * ws + mg * wm) / den)), int(std::lround((qBlue(sq) * ws + mb * wm) / den)), int(std::lround(oa))); }
        else { if (!sa || ma <= 0) continue; const float a = 2.5f * k;   // (unsharp: away from the mean round it)
          d[x] = qRgba(std::clamp(int(std::lround(qRed(sq) + (qRed(sq) - mr) * a)), 0, 255), std::clamp(int(std::lround(qGreen(sq) + (qGreen(sq) - mg) * a)), 0, 255), std::clamp(int(std::lround(qBlue(sq) + (qBlue(sq) - mb) * a)), 0, 255), sa); }
        continue; }
      if (!sa) continue;                                           // (the recolours: only where something is painted)
      float h, sv, l; toHsl(qRed(sq) / 255.f, qGreen(sq) / 255.f, qBlue(sq) / 255.f, h, sv, l);
      switch (op) {
        case RegionOp::Hue: h = ih; sv = is; break;                // (the ink's tone and saturation, the texel's own lightness — mixed in by k below)
        case RegionOp::Lighten: l = l + (1 - l) * k; break;
        case RegionOp::Darken: l = l * (1 - k); break;
        case RegionOp::Saturate: sv = sv + (1 - sv) * k; break;
        case RegionOp::Desaturate: sv = sv * (1 - k); break;
        default: break; }
      float rr, gg, bb; fromHsl(h, sv, l, rr, gg, bb);
      if (op == RegionOp::Hue) { rr = qRed(sq) / 255.f + (rr - qRed(sq) / 255.f) * k; gg = qGreen(sq) / 255.f + (gg - qGreen(sq) / 255.f) * k; bb = qBlue(sq) / 255.f + (bb - qBlue(sq) / 255.f) * k; }
      d[x] = qRgba(std::clamp(int(std::lround(rr * 255)), 0, 255), std::clamp(int(std::lround(gg * 255)), 0, 255), std::clamp(int(std::lround(bb * 255)), 0, 255), sa);
    }
  }
}

// ---- a picture as it was, tile by tile ----
void TileSnap::start(const QImage& src) {
  tw_ = (src.width() + TILE - 1) / TILE; th_ = (src.height() + TILE - 1) / TILE; seen_.assign(size_t(tw_) * size_t(th_), 0); touched_.clear();
  if (snap_.size() != src.size() || snap_.format() != src.format()) snap_ = QImage(src.size(), src.format());
}
void TileSnap::reach(const QImage& src, const QRect& r0) {
  const QRect r = r0.intersected(src.rect()); if (r.isEmpty() || snap_.size() != src.size()) return; const int bpp = src.depth() / 8;
  for (int ty = r.top() / TILE; ty <= r.bottom() / TILE; ty++) for (int tx = r.left() / TILE; tx <= r.right() / TILE; tx++) {
    const int i = ty * tw_ + tx; if (seen_[size_t(i)]) continue; seen_[size_t(i)] = 1; touched_.push_back(i);
    const int x = tx * TILE, w = std::min(TILE, src.width() - x); for (int y = ty * TILE; y < std::min(src.height(), (ty + 1) * TILE); y++) std::memcpy(snap_.scanLine(y) + size_t(x) * bpp, src.constScanLine(y) + size_t(x) * bpp, size_t(w) * bpp); }
}
std::vector<Stroke::Tile> TileSnap::tiles() const {
  std::vector<Stroke::Tile> out;
  for (int i : touched_) { const QRect tr = QRect((i % tw_) * TILE, (i / tw_) * TILE, TILE, TILE).intersected(snap_.rect()); out.push_back({tr.topLeft(), snap_.copy(tr)}); }
  return out;
}

}  // namespace hm::paint
