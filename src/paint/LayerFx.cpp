#include "paint/LayerFx.h"
#include <QColor>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <thread>

namespace hm::paint {

namespace {
inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
template <class F> void rows(int h, F f) {
  const int nt = std::clamp(int(std::thread::hardware_concurrency()), 1, 12), step = (h + nt - 1) / nt; std::vector<std::thread> ts;
  for (int k = 1; k < nt; k++) { const int a = k * step, b = std::min(h, (k + 1) * step); if (a < b) ts.emplace_back(f, a, b); }
  f(0, std::min(h, step)); for (auto& t : ts) t.join();
}
}  // namespace

void fxLight(double az, double el, float out[3]) {
  const double a = az * M_PI / 180, e = el * M_PI / 180;
  out[0] = float(std::cos(e) * std::sin(a)); out[1] = float(std::sin(e)); out[2] = float(std::cos(e) * std::cos(a));
}

std::shared_ptr<FxFrame> fxFrame(int w, int h, const float* P, const float* N, const float L[3]) {
  auto F = std::make_shared<FxFrame>(); F->w = w; F->h = h; const size_t n = size_t(w) * h;
  F->surf.assign(n, 0); F->link.assign(n, 0); F->lx.assign(n, 0); F->ly.assign(n, 0); F->nx.assign(n, 0); F->ny.assign(n, 0); F->nz.assign(n, 0);
  if (!P || !N || w <= 0 || h <= 0) return F;
  for (size_t i = 0; i < n; i++) { F->surf[i] = N[i * 3] != 0 || N[i * 3 + 1] != 0 || N[i * 3 + 2] != 0;
    if (F->surf[i]) { const float l = std::max(1e-9f, std::sqrt(N[i * 3] * N[i * 3] + N[i * 3 + 1] * N[i * 3 + 1] + N[i * 3 + 2] * N[i * 3 + 2]));
      F->nx[i] = (signed char)std::lround(N[i * 3] / l * 127); F->ny[i] = (signed char)std::lround(N[i * 3 + 1] / l * 127); F->nz[i] = (signed char)std::lround(N[i * 3 + 2] / l * 127); } }
  auto dist = [&](size_t a, size_t b) { const float dx = P[a * 3] - P[b * 3], dy = P[a * 3 + 1] - P[b * 3 + 1], dz = P[a * 3 + 2] - P[b * 3 + 2]; return std::sqrt(dx * dx + dy * dy + dz * dz); };
  // (a texel's size on the model: the middle of the steps between neighbours — a step much longer is a jump over a seam)
  std::vector<float> st; st.reserve(4096);
  for (size_t i = 0; i + 1 < n && st.size() < 200000; i += 7) if (F->surf[i] && (i + 1) % size_t(w) && F->surf[i + 1]) st.push_back(dist(i, i + 1));
  float m = 0; if (!st.empty()) { std::nth_element(st.begin(), st.begin() + st.size() / 2, st.end()); m = st[st.size() / 2]; }
  const float far = std::max(m * 4, 1e-9f);
  auto near = [&](size_t a, size_t b) { if (!F->surf[a] || !F->surf[b]) return false; if (dist(a, b) > far) return false;
    return N[a * 3] * N[b * 3] + N[a * 3 + 1] * N[b * 3 + 1] + N[a * 3 + 2] * N[b * 3 + 2] > 0.2f; };
  rows(h, [&](int y0, int y1) { for (int y = y0; y < y1; y++) for (int x = 0; x < w; x++) { const size_t i = size_t(y) * w + x; unsigned char b = 0;
    if (x + 1 < w && near(i, i + 1)) b |= 1; if (y + 1 < h && near(i, i + w)) b |= 2; F->link[i] = b; } });
  // (the way toward the light in the texture: the light laid onto the surface, written in the texture's own directions dP/du, dP/dv)
  rows(h, [&](int y0, int y1) { for (int y = y0; y < y1; y++) for (int x = 0; x < w; x++) { const size_t i = size_t(y) * w + x; if (!F->surf[i]) continue;
    float U[3] = {0, 0, 0}, V[3] = {0, 0, 0};
    if (F->link[i] & 1) for (int k = 0; k < 3; k++) U[k] = P[(i + 1) * 3 + k] - P[i * 3 + k]; else if (x > 0 && (F->link[i - 1] & 1)) for (int k = 0; k < 3; k++) U[k] = P[i * 3 + k] - P[(i - 1) * 3 + k]; else continue;
    if (F->link[i] & 2) for (int k = 0; k < 3; k++) V[k] = P[(i + w) * 3 + k] - P[i * 3 + k]; else if (y > 0 && (F->link[i - w] & 2)) for (int k = 0; k < 3; k++) V[k] = P[i * 3 + k] - P[(i - w) * 3 + k]; else continue;
    const float* Nn = N + i * 3; const float nl = Nn[0] * L[0] + Nn[1] * L[1] + Nn[2] * L[2]; const float T[3] = {L[0] - Nn[0] * nl, L[1] - Nn[1] * nl, L[2] - Nn[2] * nl};
    const double uu = U[0] * U[0] + U[1] * U[1] + U[2] * U[2], uv = U[0] * V[0] + U[1] * V[1] + U[2] * V[2], vv = V[0] * V[0] + V[1] * V[1] + V[2] * V[2];
    const double ru = U[0] * T[0] + U[1] * T[1] + U[2] * T[2], rv = V[0] * T[0] + V[1] * T[1] + V[2] * T[2], det = uu * vv - uv * uv; if (std::abs(det) < 1e-30) continue;
    double a = (vv * ru - uv * rv) / det, b = (uu * rv - uv * ru) / det; const double l = std::hypot(a, b); if (l < 1e-12) continue; a /= l; b /= l;
    const double tl = std::sqrt(double(T[0]) * T[0] + double(T[1]) * T[1] + double(T[2]) * T[2]); const double k = std::min(1.0, tl * 4);   // (the light almost along the normal: the direction fades)
    F->lx[i] = (signed char)std::lround(a * 127 * k); F->ly[i] = (signed char)std::lround(b * 127 * k); } });
  return F;
}

QJsonObject fxDefaults(const QString& type) {
  if (type == "shadow") return QJsonObject{{"type", type}, {"src", -1}, {"soft", 3}, {"rays", 16}, {"c1", "#6a5a7a"}, {"az", 30}, {"el", 60}};
  if (type == "inner") return QJsonObject{{"type", type}, {"dist", 4}, {"size", 8}, {"c1", "#4a2e22"}, {"az", 30}, {"el", 55}};
  if (type == "shine") return QJsonObject{{"type", type}, {"rough", 0.45}, {"metal", 0.8}, {"power", 1.5}, {"c1", "#ffd98a"}, {"az", 30}, {"el", 55}, {"vd", QJsonArray{0, 0, 1}}};
  if (type == "bevel") return QJsonObject{{"type", type}, {"size", 8}, {"c1", "#ffffff"}, {"c2", "#000000"}, {"az", 30}, {"el", 55}};
  return QJsonObject{{"type", "stroke"}, {"size", 6}, {"pos", "inside"}, {"c1", "#74674c"}, {"c2", "#2e261c"}, {"grad", false}, {"az", 30}, {"el", 55}};
}
QString fxName(const QString& type) { return type == "shadow" ? QStringLiteral("Object shadow") : type == "inner" ? QStringLiteral("Inner shadow") : type == "bevel" ? QStringLiteral("Bevel") : type == "shine" ? QStringLiteral("Shine") : QStringLiteral("Stroke"); }

QImage fxShineMap(const FxFrame& F, const float L0[3], const float V0[3], float rough) {
  const int w = F.w, h = F.h; QImage m(std::max(1, w), std::max(1, h), QImage::Format_Grayscale8); m.fill(0); if (w <= 0 || h <= 0) return m;
  auto unit = [](const float* a, float* o) { const float l = std::max(1e-6f, std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])); for (int k = 0; k < 3; k++) o[k] = a[k] / l; };
  float L[3], V[3], H[3]; unit(L0, L); unit(V0, V); const float hs[3] = {L[0] + V[0], L[1] + V[1], L[2] + V[2]}; unit(hs, H);
  const float r = std::clamp(rough, 0.02f, 1.f), al2 = (r * r) * (r * r);
  rows(h, [&](int y0, int y1) { for (int y = y0; y < y1; y++) { uchar* o = m.scanLine(y);
    for (int x = 0; x < w; x++) { const size_t i = size_t(y) * w + x; if (!F.surf[i]) continue;
      const float Nx = F.nx[i] / 127.f, Ny = F.ny[i] / 127.f, Nz = F.nz[i] / 127.f;
      const float nl = Nx * L[0] + Ny * L[1] + Nz * L[2], nv = Nx * V[0] + Ny * V[1] + Nz * V[2]; if (nl <= 0 || nv <= 0) continue;
      const float nh = std::max(0.f, Nx * H[0] + Ny * H[1] + Nz * H[2]); const float d = nh * nh * (al2 - 1) + 1; const float D = al2 / std::max(1e-6f, d);
      o[x] = uchar(clamp01(D * std::min(1.f, nl * 2)) * 255 + 0.5f); } } });
  return m;
}

QImage fxRender(const QImage& a8in, const FxFrame& F, const QJsonObject& fx) {
  const int w = F.w, h = F.h; if (w <= 0 || h <= 0) return {};
  const QImage a8 = a8in.size() == QSize(w, h) ? a8in.convertToFormat(QImage::Format_Grayscale8) : a8in.scaled(w, h).convertToFormat(QImage::Format_Grayscale8);
  const size_t n = size_t(w) * h; const float INF = 1e9f;
  if (fx.value("type").toString() == "shine") {
    // the highlight the light gives this surface seen from the view kept in the record (GGX's spread, its peak = 1), metal tinting
    // it with the colour; only where the source has paint (19.11: the author's roughness and metal on the gold parts in 3D-Coat,
    // meant to be baked into the texture through Photoshop — which takes no highlights)
    float L[3]; fxLight(fx.value("az").toDouble(30), fx.value("el").toDouble(55), L);
    const QJsonArray va = fx.value("vd").toArray(); float V[3] = {float(va.at(0).toDouble(0)), float(va.at(1).toDouble(0)), float(va.at(2).toDouble(1))};
    { const float l = std::max(1e-6f, std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2])); for (float& c : V) c /= l; }
    float Hh[3] = {L[0] + V[0], L[1] + V[1], L[2] + V[2]}; { const float l = std::max(1e-6f, std::sqrt(Hh[0] * Hh[0] + Hh[1] * Hh[1] + Hh[2] * Hh[2])); for (float& c : Hh) c /= l; }
    const float r = std::clamp(float(fx.value("rough").toDouble(0.3)), 0.02f, 1.f), al2 = (r * r) * (r * r), metal = clamp01(float(fx.value("metal").toDouble(0.8))), power = std::max(0.f, float(fx.value("power").toDouble(1)));
    const QColor tint(fx.value("c1").toString("#ffd98a")); const float cr = 1 + (float(tint.redF()) - 1) * metal, cg = 1 + (float(tint.greenF()) - 1) * metal, cb = 1 + (float(tint.blueF()) - 1) * metal;
    QImage out(w, h, QImage::Format_ARGB32); out.fill(0);
    rows(h, [&](int y0, int y1) { for (int y = y0; y < y1; y++) { QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y)); const uchar* ar = a8.constScanLine(y);
      for (int x = 0; x < w; x++) { const size_t i = size_t(y) * w + x; if (!F.surf[i] || !ar[x]) continue;
        const float Nx = F.nx[i] / 127.f, Ny = F.ny[i] / 127.f, Nz = F.nz[i] / 127.f;
        const float nl = Nx * L[0] + Ny * L[1] + Nz * L[2], nv = Nx * V[0] + Ny * V[1] + Nz * V[2]; if (nl <= 0 || nv <= 0) continue;
        const float nh = std::max(0.f, Nx * Hh[0] + Ny * Hh[1] + Nz * Hh[2]); const float d = nh * nh * (al2 - 1) + 1; const float D = (al2 / std::max(1e-6f, d)) * (al2 / std::max(1e-6f, d));
        const float a = clamp01(std::sqrt(D) * std::min(1.f, nl * 2) * power) * (ar[x] / 255.f); if (a <= 0.003f) continue;
        o[x] = qRgba(int(cr * 255 + 0.5f), int(cg * 255 + 0.5f), int(cb * 255 + 0.5f), int(a * 255 + 0.5f)); } } });
    return out;
  }
  std::vector<unsigned char> in(n, 0); for (int y = 0; y < h; y++) { const uchar* r = a8.constScanLine(y); for (int x = 0; x < w; x++) in[size_t(y) * w + x] = F.surf[size_t(y) * w + x] && r[x] >= 128; }
  // the distance over the surface to the nearest texel of the other side (chamfer 1 / √2, only along the links)
  auto distTo = [&](bool fromOutside) {
    std::vector<float> d(n, INF);
    for (size_t i = 0; i < n; i++) if (F.surf[i] && (fromOutside ? !in[i] : in[i])) d[i] = 0;
    auto H = [&](int xa, int ya) { return (F.link[size_t(ya) * w + xa] & 1) != 0; };
    auto Vv = [&](int xa, int ya) { return (F.link[size_t(ya) * w + xa] & 2) != 0; };
    // (the texel at x+dx, y+dy joined to x, y on the surface: straight along a link; a diagonal — either way round the corner)
    auto Ld = [&](int x, int y, int dx, int dy) -> bool { const int X = x + dx, Y = y + dy;
      return (H(std::min(x, X), y) && Vv(X, std::min(y, Y))) || (Vv(x, std::min(y, Y)) && H(std::min(x, X), Y)); };
    const float D = 1.41421f;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { const size_t i = size_t(y) * w + x; if (!F.surf[i] || d[i] == 0) continue; float v = d[i];
      if (x > 0 && H(x - 1, y)) v = std::min(v, d[i - 1] + 1); if (y > 0 && Vv(x, y - 1)) v = std::min(v, d[i - w] + 1);
      if (x > 0 && y > 0 && Ld(x, y, -1, -1)) v = std::min(v, d[i - w - 1] + D); if (x + 1 < w && y > 0 && Ld(x, y, 1, -1)) v = std::min(v, d[i - w + 1] + D); d[i] = v; }
    for (int y = h - 1; y >= 0; y--) for (int x = w - 1; x >= 0; x--) { const size_t i = size_t(y) * w + x; if (!F.surf[i] || d[i] == 0) continue; float v = d[i];
      if (x + 1 < w && H(x, y)) v = std::min(v, d[i + 1] + 1); if (y + 1 < h && Vv(x, y)) v = std::min(v, d[i + w] + 1);
      if (x + 1 < w && y + 1 < h && Ld(x, y, 1, 1)) v = std::min(v, d[i + w + 1] + D); if (x > 0 && y + 1 < h && Ld(x, y, -1, 1)) v = std::min(v, d[i + w - 1] + D); d[i] = v; }
    return d; };
  std::vector<float> dIn, dOut; { std::thread t([&] { dOut = distTo(false); }); dIn = distTo(true); t.join(); }   // (both at once)
  const QString type = fx.value("type").toString("stroke");
  const float size = float(std::max(0.5, fx.value("size").toDouble(6))), dist = float(std::max(0.0, fx.value("dist").toDouble(4)));
  const QColor c1(fx.value("c1").toString("#74674c")), c2(fx.value("c2").toString("#2e261c")); const bool grad = fx.value("grad").toBool(); const QString pos = fx.value("pos").toString("inside");
  QImage out(w, h, QImage::Format_ARGB32); out.fill(0);
  rows(h, [&](int y0, int y1) { for (int y = y0; y < y1; y++) { QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y)); const uchar* ar = a8.constScanLine(y);
    for (int x = 0; x < w; x++) { const size_t i = size_t(y) * w + x; if (!F.surf[i]) continue;
      const bool I = in[i]; const float di = dIn[i], dO = dOut[i];
      if ((I ? di : dO) >= INF) continue;                                        // (no edge on this piece of surface)
      // (the edge's outward way: the signed distance rising — read from the neighbours this texel is joined to)
      auto sd = [&](size_t j) { return in[j] ? -dIn[j] : dOut[j]; }; const float s0 = sd(i);
      float gx = 0, gy = 0; const unsigned char lk = F.link[i];
      if ((lk & 1) && x > 0 && (F.link[i - 1] & 1)) gx = (sd(i + 1) - sd(i - 1)) * 0.5f; else if (lk & 1) gx = sd(i + 1) - s0; else if (x > 0 && (F.link[i - 1] & 1)) gx = s0 - sd(i - 1);
      if ((lk & 2) && y > 0 && (F.link[i - w] & 2)) gy = (sd(i + w) - sd(i - w)) * 0.5f; else if (lk & 2) gy = sd(i + w) - s0; else if (y > 0 && (F.link[i - w] & 2)) gy = s0 - sd(i - w);
      const float gl = std::hypot(gx, gy); const float nx = gl > 1e-6f ? gx / gl : 0, ny = gl > 1e-6f ? gy / gl : 0;
      const float lxv = F.lx[i] / 127.f, lyv = F.ly[i] / 127.f; const float face = nx * lxv + ny * lyv;   // (>0: the edge faces the light)
      const float aSrc = ar[x] / 255.f; float a = 0; QColor c = c1;
      if (type == "stroke") {
        const float wIn = pos == "outside" ? 0 : pos == "center" ? size / 2 : size, wOut = pos == "inside" ? 0 : pos == "center" ? size / 2 : size;
        if (I) a = clamp01(wIn - (di - 0.5f)) * aSrc; else a = clamp01(wOut - (dO - 0.5f));
        if (grad) { const float t = clamp01(0.5f - 0.5f * face); c = QColor::fromRgbF(c1.redF() + (c2.redF() - c1.redF()) * t, c1.greenF() + (c2.greenF() - c1.greenF()) * t, c1.blueF() + (c2.blueF() - c1.blueF()) * t); }
      } else if (type == "inner") {
        if (!I) continue; float s = clamp01(1 - (di - 0.5f) / std::max(0.5f, dist + size)); s = s * s * (3 - 2 * s);
        const float wdir = dist > 0 ? clamp01(0.5f + face * std::min(1.f, dist / std::max(0.5f, size)) * 1.5f) : 1.f;   // (offset toward the light: the edges facing it shadowed)
        a = s * wdir * aSrc;
      } else {   // bevel: a band inside the edge; the side facing the light lit, the other darkened
        if (!I) continue; float b = clamp01(1 - (di - 0.5f) / size); b = b * b * (3 - 2 * b);
        a = b * std::abs(face) * aSrc; c = face >= 0 ? c1 : c2;
      }
      if (a <= 0.002f) continue; o[x] = qRgba(c.red(), c.green(), c.blue(), int(clamp01(a) * 255 + 0.5f)); } } });
  return out;
}

}  // namespace hm::paint
