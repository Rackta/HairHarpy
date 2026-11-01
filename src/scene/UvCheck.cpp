// What is wrong with a tile's UV: polygons turned over, lying over others (a stack on purpose — two alike shells one on
// another — left out), past the tile, nearer each other than the padding (the shells grown by half of it in a raster of
// the texture's pixels: a texel two shells take), shells off the wanted texel density, mirror pairs not mirrored; and
// each polygon's angle distortion for the stretch map.
#include "scene/Uv.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace hm::uv {

namespace {
using P = std::array<double, 2>;
double segDist(const P& p, const P& a, const P& b) { const double dx = b[0] - a[0], dy = b[1] - a[1], L = dx * dx + dy * dy; double t = L > 0 ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / L : 0; t = std::clamp(t, 0.0, 1.0); return std::hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy); }
bool inTri(const P& p, const P& a, const P& b, const P& c) {
  auto cr = [](const P& o, const P& u, const P& v) { return (u[0] - o[0]) * (v[1] - o[1]) - (u[1] - o[1]) * (v[0] - o[0]); };
  const double d1 = cr(a, b, p), d2 = cr(b, c, p), d3 = cr(c, a, p); return (d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0);
}
// each polygon's fan triangles in cells (U · nx, V · ny) — every cell whose middle is in one (grow: within `grow` cells)
template <class F> void raster(const PolyMesh& m, const topo::TTopo& T, int f, int nx, int ny, double grow, F&& hit) {
  const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
  auto at = [&](int k) { return P{m.cornerUV[size_t(k)][0] * nx, m.cornerUV[size_t(k)][1] * ny}; };
  for (int k = s + 1; k + 1 < e; k++) {
    const P a = at(s), b = at(k), c = at(k + 1);
    const int x0 = std::max(0, int(std::floor(std::min({a[0], b[0], c[0]}) - grow))), x1 = std::min(nx - 1, int(std::ceil(std::max({a[0], b[0], c[0]}) + grow)));
    const int y0 = std::max(0, int(std::floor(std::min({a[1], b[1], c[1]}) - grow))), y1 = std::min(ny - 1, int(std::ceil(std::max({a[1], b[1], c[1]}) + grow)));
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) { const P p{x + 0.5, y + 0.5};
      if (inTri(p, a, b, c) || (grow > 0 && std::min({segDist(p, a, b), segDist(p, b, c), segDist(p, c, a)}) <= grow)) hit(x, y); }
  }
}
}  // namespace

CheckReport check(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const CheckOpts& o) {
  CheckReport R; if (!has(m) || faces.empty()) return R;
  const Points Pt = points(m, T); const Shells S = shells(m, T, Pt);
  std::set<int> in(faces.begin(), faces.end());
  // the shells here: area in UV (texels) and 3D, polygon count; the ones alike (a stack when one lies on the other)
  std::map<int, std::array<double, 3>> sh; for (int f : faces) { auto& a = sh[S.of[size_t(f)]]; a[0] += std::abs(faceArea(m, f)) * o.w * o.h; a[1] += faceArea3(m, f); a[2] += 1; }
  auto alike = [&](int a, int b) { const auto &x = sh[a], &y = sh[b]; return x[2] == y[2] && std::abs(x[0] - y[0]) <= 0.03 * std::max(x[0], y[0]); };   // (as Pack takes a stack: as many polygons, one size in UV)
  // turned over
  { const auto fl = flipped(m, T, S); for (int f : fl) if (in.count(f)) R.flipped.push_back(f); }
  // past the tile
  for (int f : faces) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const UV& q = m.cornerUV[size_t(k)]; if (q[0] < 0 || q[0] > 1 || q[1] < 0 || q[1] > 1) { R.outside.push_back(f); break; } }
  // over each other: cells of 4 texels — a cell two polygons take (not a stack's twins)
  { const int nx = std::clamp(o.w / 4, 64, 1024), ny = std::clamp(o.h / 4, 64, 1024); std::vector<int> own(size_t(nx) * size_t(ny), -1); std::set<int> bad; std::set<std::pair<int, int>> stacks;
    for (int f : faces) raster(m, T, f, nx, ny, 0, [&](int x, int y) { int& w = own[size_t(y) * size_t(nx) + size_t(x)]; if (w < 0) { w = f; return; } if (w == f) return;
      const int a = S.of[size_t(w)], b = S.of[size_t(f)]; if (a != b && alike(a, b)) { stacks.insert(std::minmax(a, b)); return; } bad.insert(w); bad.insert(f); });
    R.overlap.assign(bad.begin(), bad.end()); R.stacks = int(stacks.size()); }
  // the padding: the shells grown by half of it in cells of the padding's quarter — a cell two shells take; and the
  // tile's edges (half of it off them)
  if (o.pad > 0) { const double cellPx = std::max(1.0, o.pad / 4); const int nx = std::clamp(int(o.w / cellPx), 64, 2048), ny = std::clamp(int(o.h / cellPx), 64, 2048);
    const double grow = o.pad / 2 / (double(o.w) / nx); std::vector<int> own(size_t(nx) * size_t(ny), -1); std::set<int> bad;
    for (int f : faces) raster(m, T, f, nx, ny, grow, [&](int x, int y) { int& w = own[size_t(y) * size_t(nx) + size_t(x)]; if (w < 0) { w = f; return; }
      const int a = S.of[size_t(w)], b = S.of[size_t(f)]; if (a == b || alike(a, b)) return; bad.insert(w); bad.insert(f); });
    for (int f : faces) { bool edge = false; const double mu = o.pad / 2 / o.w, mv = o.pad / 2 / o.h;
      for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1] && !edge; k++) { const UV& q = m.cornerUV[size_t(k)]; if (q[0] >= 0 && q[0] <= 1 && q[1] >= 0 && q[1] <= 1 && (q[0] < mu || q[0] > 1 - mu || q[1] < mv || q[1] > 1 - mv)) edge = true; }
      if (edge) bad.insert(f); }
    R.padding.assign(bad.begin(), bad.end()); }
  // the texel density (pixels per scene unit): each shell's against the wanted one — none: the shells' median by area
  { std::vector<std::pair<double, double>> td; for (auto& [s, a] : sh) if (a[1] > 0 && a[0] > 0) td.push_back({std::sqrt(a[0] / a[1]), a[1]});
    if (!td.empty()) { std::sort(td.begin(), td.end()); double tot = 0; for (auto& x : td) tot += x.second; double acc = 0; R.tdRef = td.back().first; for (auto& x : td) { acc += x.second; if (acc >= tot / 2) { R.tdRef = x.first; break; } }
      if (o.td > 0) R.tdRef = o.td;
      std::set<int> off; for (auto& [s, a] : sh) if (a[1] > 0 && a[0] > 0 && std::abs(std::sqrt(a[0] / a[1]) / R.tdRef - 1) > o.tdTol) off.insert(s);
      for (int f : faces) if (off.count(S.of[size_t(f)])) R.density.push_back(f); } }
  // mirror pairs: each shell whose every corner's mirror lies in one other shell (and back) — its corners about its
  // middle against its twin's, mirrored; off by more than 1% of its size: not mirror images
  if (o.mc && o.mc->size() == m.cornerUV.size()) {
    std::map<int, int> to; for (int f : faces) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int s = S.of[size_t(f)], p = (*o.mc)[size_t(k)];
      const int g = p >= 0 && in.count(T.cf[size_t(p)]) ? S.of[size_t(T.cf[size_t(p)])] : -1; auto it = to.find(s); if (it == to.end()) to[s] = g; else if (it->second != g) it->second = -1; }
    std::map<int, std::array<double, 4>> ctr;   // (middle u, v; size; corners)
    for (int f : faces) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { auto& c = ctr[S.of[size_t(f)]]; c[0] += m.cornerUV[size_t(k)][0] * o.w; c[1] += m.cornerUV[size_t(k)][1] * o.h; c[3] += 1; }
    for (auto& [s, c] : ctr) { c[0] /= c[3]; c[1] /= c[3]; c[2] = std::sqrt(sh[s][0]); }
    std::set<int> off;
    for (auto& [s, g] : to) { if (g < 0 || g == s || to[g] != s || s > g) continue;
      if (!alike(s, g)) { off.insert(s); off.insert(g); continue; }   // (another size: not mirror images)
      double worst = 0; for (int f : faces) if (S.of[size_t(f)] == s) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int p = (*o.mc)[size_t(k)]; if (p < 0) continue;
        const double ax = m.cornerUV[size_t(k)][0] * o.w - ctr[s][0], ay = m.cornerUV[size_t(k)][1] * o.h - ctr[s][1], bx = m.cornerUV[size_t(p)][0] * o.w - ctr[g][0], by = m.cornerUV[size_t(p)][1] * o.h - ctr[g][1];
        worst = std::max(worst, std::min(std::hypot(ax + bx, ay - by), std::hypot(ax - bx, ay + by))); }   // (mirrored across a vertical line, or a horizontal one)
      if (worst > 0.01 * std::max(ctr[s][2], 1.0)) { off.insert(s); off.insert(g); } }
    for (int f : faces) if (off.count(S.of[size_t(f)])) R.asym.push_back(f);
  }
  return R;
}

std::vector<float> angleStretch(const PolyMesh& m, const topo::TTopo& T, double asp) {
  std::vector<float> out(size_t(T.nf), 1.f); if (!has(m)) return out;
  for (int f = 0; f < T.nf; f++) {
    const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1]; double worst = 1;
    for (int k = s + 1; k + 1 < e; k++) {
      const Vec3 &p0 = m.pos[size_t(T.fv[size_t(s)])], &p1 = m.pos[size_t(T.fv[size_t(k)])], &p2 = m.pos[size_t(T.fv[size_t(k + 1)])];
      const Vec3 e1 = p1 - p0, e2 = p2 - p0; const double l = e1.length(); const Vec3 nn = e1.cross(e2); const double A2 = nn.length(); if (l <= 0 || A2 <= 1e-30) continue;
      const Vec3 x = e1 * (1 / l), y = (nn * (1 / A2)).cross(x); const double q1x = l, q2x = e2.dot(x), q2y = e2.dot(y);
      const UV &u0 = m.cornerUV[size_t(s)], &u1 = m.cornerUV[size_t(k)], &u2 = m.cornerUV[size_t(k + 1)];
      const double du1 = u1[0] - u0[0], dv1 = (u1[1] - u0[1]) * asp, du2 = u2[0] - u0[0], dv2 = (u2[1] - u0[1]) * asp;
      // J = [du1 du2; dv1 dv2] · inverse([q1x q2x; 0 q2y]) — its singular values' ratio
      const double ia = 1 / q1x, ib = -q2x / (q1x * q2y), id = 1 / q2y;
      const double a = du1 * ia, b = du1 * ib + du2 * id, c = dv1 * ia, d = dv1 * ib + dv2 * id;
      const double fro = a * a + b * b + c * c + d * d, det = std::abs(a * d - b * c); if (det <= 1e-30) { worst = 1e3; continue; }
      const double disc = std::sqrt(std::max(0.0, fro * fro / 4 - det * det)), s1 = std::sqrt(fro / 2 + disc), s2 = std::sqrt(std::max(1e-30, fro / 2 - disc));
      worst = std::max(worst, s1 / s2);
    }
    out[size_t(f)] = float(std::min(worst, 1e3));
  }
  return out;
}

}  // namespace hm::uv
