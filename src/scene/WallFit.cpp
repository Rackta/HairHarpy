// Walls fitted to a mesh (WallFit.h). Each wall is built once with Bulge 1 and 2 segments: the middle column shows the
// direction the bulge pushes each row. Along it, from points across the flat row (¼, ½, ¾ of the width), the mesh's
// surface is found (the nearest vertex of a dense mesh, a few steps along the line); the row's bulge is the sine through
// those offsets. The rows' bulges, smoothed, make the profile; Bulge is its peak (limited), the segments — enough for the
// arc to stay within a small share of the mesh's size.
#include "scene/WallFit.h"
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

namespace hm::wallfit {

namespace {

constexpr double PI = 3.14159265358979323846;

struct Grid {
  double cs = 1; Vec3 o; std::vector<std::pair<long long, int>> v; std::unordered_map<long long, std::pair<int, int>> range;
  long long key(long long x, long long y, long long z) const { return (x * 100003LL + y) * 100003LL + z; }
  void cell(const Vec3& p, long long& x, long long& y, long long& z) const { x = (long long)std::floor((p.x - o.x) / cs); y = (long long)std::floor((p.y - o.y) / cs); z = (long long)std::floor((p.z - o.z) / cs); }
  Grid(const std::vector<Vec3>& P, const Box3& bb, double c) : cs(std::max(c, 1e-9)), o(bb.min) {
    v.reserve(P.size()); for (size_t i = 0; i < P.size(); i++) { long long x, y, z; cell(P[i], x, y, z); v.push_back({key(x, y, z), int(i)}); }
    std::sort(v.begin(), v.end()); for (size_t i = 0; i < v.size();) { size_t j = i; while (j < v.size() && v[j].first == v[i].first) j++; range[v[i].first] = {int(i), int(j)}; i = j; } }
  int nearest(const std::vector<Vec3>& P, const Vec3& p, int maxR) const {      // (rings of cells widened till one is found, then one more)
    long long cx, cy, cz; cell(p, cx, cy, cz); int best = -1; double bd = 1e300; int found = -1;
    for (int r = 0; r <= maxR; r++) {
      for (long long x = cx - r; x <= cx + r; x++) for (long long y = cy - r; y <= cy + r; y++) for (long long z = cz - r; z <= cz + r; z++) {
        if (std::max({std::llabs(x - cx), std::llabs(y - cy), std::llabs(z - cz)}) != r) continue;
        auto it = range.find(key(x, y, z)); if (it == range.end()) continue;
        for (int k = it->second.first; k < it->second.second; k++) { const double d = (P[size_t(v[size_t(k)].second)] - p).lengthSq(); if (d < bd) { bd = d; best = v[size_t(k)].second; } } }
      if (best >= 0 && found < 0) found = r; if (found >= 0 && r >= found + 1) break; }
    return best; }
};

}  // namespace

int fitToMesh(Scene& sc, const CurveCache& cc, const std::vector<int>& walls, const PolyMesh& m, double maxBulge) {
  if (walls.empty() || m.pos.size() < 4) return 0;
  const Box3 bb = m.bounds(); const double diag = bb.size().length(); if (!(diag > 0)) return 0;
  const Grid G(m.pos, bb, diag / 160);
  const double tol = 0.0015 * diag;                                                            // (the arc's allowed miss)
  auto onSurface = [&](const Vec3& q0, const Vec3& n, double lim) {                            // (the offset along n to the surface)
    double t = 0;
    for (int it = 0; it < 6; it++) { const Vec3 q = q0 + n * t; const int v = G.nearest(m.pos, q, 8); if (v < 0) return 0.0;
      const double dt = (m.pos[size_t(v)] - q).dot(n); t = std::clamp(t + dt, -lim, lim); if (std::abs(dt) < 1e-4 * diag) break; }
    return t; };
  int fitted = 0;
  for (int sid : walls) {
    Item* sf = sc.item(sid); if (!sf || sf->type != ItemType::Surface) continue;
    const QJsonValue p0 = sf->raw.value("p");                  // (its settings before the probe: put back when it can't be fitted)
    QJsonObject p = p0.toObject(); p.remove("bprof"); p.insert("segs", 2); p.insert("bulge", 1.0); p.insert("flip", false); p.insert("b2on", false);
    sf->raw.insert("p", p);
    const std::set<int> only{sid}; const auto W = buildWalls(sc, cc, true, &only);
    auto restore = [&] { if (p0.isUndefined()) sf->raw.remove("p"); else sf->raw.insert("p", p0); };
    if (W.empty() || W[0].grids.empty()) { restore(); continue; }
    const auto& g = W[0].grids[0]; const auto& P = W[0].pos; const int R = g.R;
    if (g.M != 3 || R < 2) { restore(); continue; }
    std::vector<double> b(size_t(R), 0), wd(size_t(R), 0), u(size_t(R), 0);
    for (int k = 0; k < R; k++) {
      const Vec3 c0 = P[size_t(g.start + k * 3)], c1 = P[size_t(g.start + k * 3 + 1)], c2 = P[size_t(g.start + k * 3 + 2)];
      const Vec3 w = c2 - c0, mid = (c0 + c2) * 0.5; wd[size_t(k)] = w.length();
      if (k) { const Vec3 pm = (P[size_t(g.start + (k - 1) * 3)] + P[size_t(g.start + (k - 1) * 3 + 2)]) * 0.5; u[size_t(k)] = u[size_t(k - 1)] + mid.distanceTo(pm); }
      Vec3 n = c1 - mid; if (wd[size_t(k)] < 1e-9 || n.length() < 1e-6 * wd[size_t(k)]) continue;
      n = n.normalized();
      double num = 0, den = 0;
      for (double s : {0.25, 0.5, 0.75}) { const double sn = std::sin(PI * s); num += onSurface(c0 + w * s, n, 3 * wd[size_t(k)]) * sn; den += sn * sn; }
      b[size_t(k)] = num / (den * wd[size_t(k)]);
    }
    // the narrow rows (near a fork, a pointed end) say little of the bulge — an offset there over a tiny width: they take
    // the nearest wide row's
    { const double mw = *std::max_element(wd.begin(), wd.end()); std::vector<char> ok(size_t(R), 0); bool any = false;
      for (int k = 0; k < R; k++) any |= bool(ok[size_t(k)] = wd[size_t(k)] >= 0.3 * mw);
      if (any) for (int k = 0; k < R; k++) if (!ok[size_t(k)]) { int best = -1; for (int j = 0; j < R; j++) if (ok[size_t(j)] && (best < 0 || std::abs(j - k) < std::abs(best - k))) best = j; b[size_t(k)] = b[size_t(best)]; } }
    // smoothed along the wall (a median of 5, then an average of 3): one stray vertex doesn't make a dent
    std::vector<double> s1(b);
    for (int k = 0; k < R; k++) { std::vector<double> w5; for (int d = -2; d <= 2; d++) w5.push_back(b[size_t(std::clamp(k + d, 0, R - 1))]); std::nth_element(w5.begin(), w5.begin() + 2, w5.end()); s1[size_t(k)] = w5[2]; }
    for (int k = 0; k < R; k++) b[size_t(k)] = (s1[size_t(std::max(k - 1, 0))] + s1[size_t(k)] + s1[size_t(std::min(k + 1, R - 1))]) / 3;
    // the profile: 24 values along the wall's length
    const double L = u[size_t(R - 1)] > 0 ? u[size_t(R - 1)] : 1; constexpr int N = 24; std::vector<double> pr(N);
    for (int i = 0; i < N; i++) { const double x = L * i / (N - 1); int k = 0; while (k + 2 < R && u[size_t(k + 1)] < x) k++;
      const double span = u[size_t(k + 1)] - u[size_t(k)], t = span > 0 ? std::clamp((x - u[size_t(k)]) / span, 0.0, 1.0) : 0; pr[size_t(i)] = b[size_t(k)] * (1 - t) + b[size_t(k + 1)] * t; }
    double peak = 0; for (double x : pr) if (std::abs(x) > std::abs(peak)) peak = x;
    double h = 0; for (int k = 0; k < R; k++) h = std::max(h, std::abs(std::clamp(b[size_t(k)], -maxBulge, maxBulge)) * wd[size_t(k)]);
    p = sf->raw.value("p").toObject();
    if (h < tol || std::abs(peak) < 0.01) { p.insert("segs", 1); p.insert("bulge", 0.0); p.remove("bprof"); sf->raw.insert("p", p); continue; }
    const double B = std::clamp(peak, -maxBulge, maxBulge);
    QJsonArray pa; for (double x : pr) pa.append(std::round(std::clamp(x / B, -1.0, 1.0) * 1000) / 1000);
    int segs = int(std::ceil(PI * std::sqrt(h / (8 * tol)))); segs = std::clamp(segs + (segs & 1), 2, 8);   // (even: a column on the arc's top)
    p.insert("segs", segs); p.insert("bulge", std::round(B * 1000) / 1000); p.insert("bprof", pa);
    p.insert("jfade", 0.15);                                     // (the bulge fading out where the wall closes to a fork: no flaps there)
    sf->raw.insert("p", p); fitted++;
  }
  sc.revision++;
  return fitted;
}

}  // namespace hm::wallfit
