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
// A quad that is not convex from its first corner (the fan from it would have a triangle turned the other way, sticking out): the fan from its second corner instead,
// as the solver's triangulation does. Returns the first corner's offset in the polygon
static int fanFrom(const PolyMesh& m, int s, int e) {
  if (e - s != 4) return 0; auto c = [&](int i) { return m.cornerUV[size_t(s + i)]; };
  auto cr = [&](int a, int b, int d) { return (double(c(b)[0]) - c(a)[0]) * (double(c(d)[1]) - c(a)[1]) - (double(c(d)[0]) - c(a)[0]) * (double(c(b)[1]) - c(a)[1]); };
  return cr(0, 1, 2) * cr(0, 2, 3) < 0 ? 1 : 0;
}
// each polygon's fan triangles in cells (U · nx, V · ny) — every cell whose middle is in one (grow: within `grow` cells)
template <class F> void raster(const PolyMesh& m, const topo::TTopo& T, int f, int nx, int ny, double grow, F&& hit) {
  const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
  auto at = [&](int k) { return P{m.cornerUV[size_t(k)][0] * nx, m.cornerUV[size_t(k)][1] * ny}; };
  const int r = fanFrom(m, s, e), n = e - s; auto cq = [&](int i) { return s + (i + r) % n; };
  for (int k = 1; k + 1 < n; k++) {
    const P a = at(cq(0)), b = at(cq(k)), c = at(cq(k + 1));
    const int x0 = std::max(0, int(std::floor(std::min({a[0], b[0], c[0]}) - grow))), x1 = std::min(nx - 1, int(std::ceil(std::max({a[0], b[0], c[0]}) + grow)));
    const int y0 = std::max(0, int(std::floor(std::min({a[1], b[1], c[1]}) - grow))), y1 = std::min(ny - 1, int(std::ceil(std::max({a[1], b[1], c[1]}) + grow)));
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) { const P p{x + 0.5, y + 0.5};
      if (inTri(p, a, b, c) || (grow > 0 && std::min({segDist(p, a, b), segDist(p, b, c), segDist(p, c, a)}) <= grow)) hit(x, y); }
  }
}
// The polygons of one shell that lie over each other, exactly: its fan triangles tested in pairs (a grid to find the near ones) — a thin fold, a sliver the raster
// cells of the other test are too coarse for, is caught. Two triangles of one polygon, and triangles that only touch, do not count
std::vector<int> selfOverlapFaces(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces) {
  struct Tri { P a, b, c; int f; double x0, y0, x1, y1; }; std::vector<Tri> tr;
  for (int f : faces) { const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
    const int r = fanFrom(m, s, e), n = e - s; auto cq = [&](int i) { return s + (i + r) % n; };
    for (int k = 1; k + 1 < n; k++) { Tri t; t.a = {double(m.cornerUV[size_t(cq(0))][0]), double(m.cornerUV[size_t(cq(0))][1])}; t.b = {double(m.cornerUV[size_t(cq(k))][0]), double(m.cornerUV[size_t(cq(k))][1])}; t.c = {double(m.cornerUV[size_t(cq(k + 1))][0]), double(m.cornerUV[size_t(cq(k + 1))][1])}; t.f = f;
      t.x0 = std::min({t.a[0], t.b[0], t.c[0]}); t.x1 = std::max({t.a[0], t.b[0], t.c[0]}); t.y0 = std::min({t.a[1], t.b[1], t.c[1]}); t.y1 = std::max({t.a[1], t.b[1], t.c[1]}); tr.push_back(t); } }
  std::vector<int> out; if (tr.size() < 2) return out;
  double gx0 = 1e300, gy0 = 1e300, gx1 = -1e300, gy1 = -1e300, avg = 0; for (const Tri& t : tr) { gx0 = std::min(gx0, t.x0); gy0 = std::min(gy0, t.y0); gx1 = std::max(gx1, t.x1); gy1 = std::max(gy1, t.y1); avg += std::max(t.x1 - t.x0, t.y1 - t.y0); }
  avg /= double(tr.size()); const double cell = std::max(avg * 1.5, 1e-9); const int nx = std::clamp(int((gx1 - gx0) / cell) + 1, 1, 512), ny = std::clamp(int((gy1 - gy0) / cell) + 1, 1, 512);
  const double cw = std::max((gx1 - gx0) / nx, 1e-12), ch = std::max((gy1 - gy0) / ny, 1e-12); std::vector<std::vector<int>> grid(size_t(nx) * size_t(ny));
  for (size_t i = 0; i < tr.size(); i++) { const Tri& t = tr[i]; const int ix0 = std::clamp(int((t.x0 - gx0) / cw), 0, nx - 1), ix1 = std::clamp(int((t.x1 - gx0) / cw), 0, nx - 1), iy0 = std::clamp(int((t.y0 - gy0) / ch), 0, ny - 1), iy1 = std::clamp(int((t.y1 - gy0) / ch), 0, ny - 1);
    for (int y = iy0; y <= iy1; y++) for (int x = ix0; x <= ix1; x++) grid[size_t(y) * size_t(nx) + size_t(x)].push_back(int(i)); }
  const double eps = std::max(avg * avg, 1e-18) * 1e-4;                // (the overlap must be more than touching)
  auto sep = [&](const Tri& A, const Tri& B) {                          // (a separating axis among the six edges' normals)
    const P* pa[3] = {&A.a, &A.b, &A.c}; const P* pb[3] = {&B.a, &B.b, &B.c};
    for (int which = 0; which < 2; which++) { const P** q = which ? pb : pa;
      for (int i = 0; i < 3; i++) { const P& p0 = *q[i]; const P& p1 = *q[(i + 1) % 3]; const double nxv = p1[1] - p0[1], nyv = p0[0] - p1[0]; const double nl = std::hypot(nxv, nyv); if (nl < 1e-15) continue;
        double a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300; for (int k = 0; k < 3; k++) { const double da = ((*pa[k])[0] * nxv + (*pa[k])[1] * nyv) / nl, db = ((*pb[k])[0] * nxv + (*pb[k])[1] * nyv) / nl; a0 = std::min(a0, da); a1 = std::max(a1, da); b0 = std::min(b0, db); b1 = std::max(b1, db); }
        if (a1 <= b0 + std::sqrt(eps) * 0.01 || b1 <= a0 + std::sqrt(eps) * 0.01) return true; } }
    return false; };
  std::set<int> bad;
  for (size_t i = 0; i < tr.size(); i++) { const Tri& t = tr[i]; const int ix0 = std::clamp(int((t.x0 - gx0) / cw), 0, nx - 1), ix1 = std::clamp(int((t.x1 - gx0) / cw), 0, nx - 1), iy0 = std::clamp(int((t.y0 - gy0) / ch), 0, ny - 1), iy1 = std::clamp(int((t.y1 - gy0) / ch), 0, ny - 1);
    for (int y = iy0; y <= iy1; y++) for (int x = ix0; x <= ix1; x++) for (int j : grid[size_t(y) * size_t(nx) + size_t(x)]) { if (size_t(j) <= i) continue; const Tri& u = tr[size_t(j)]; if (u.f == t.f) continue;
        if (u.x0 >= t.x1 || t.x0 >= u.x1 || u.y0 >= t.y1 || t.y0 >= u.y1) continue; if (bad.count(t.f) && bad.count(u.f)) continue; if (!sep(t, u)) { bad.insert(t.f); bad.insert(u.f); } } }
  out.assign(bad.begin(), bad.end()); return out;
}

}  // namespace

CheckReport check(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const CheckOpts& o) {
  CheckReport R; if (!has(m) || faces.empty()) return R;
  const Points Pt = points(m, T); const Shells S = shells(m, T, Pt);
  std::set<int> in(faces.begin(), faces.end());
  // the shells here: area in UV (texels) and 3D, polygon count; the ones alike (a stack when one lies on the other)
  std::map<int, std::array<double, 3>> sh; for (int f : faces) { auto& a = sh[S.of[size_t(f)]]; a[0] += std::abs(faceArea(m, f)) * o.w * o.h; a[1] += faceArea3(m, f); a[2] += 1; }
  auto alike = [&](int a, int b) { const auto &x = sh[a], &y = sh[b]; return x[2] == y[2] && std::abs(x[0] - y[0]) <= 0.03 * std::max(x[0], y[0]); };   // (as Pack takes a stack: as many polygons, one size in UV)
  // (a stack: two alike shells whose UV boxes lie on each other — 80% of their union shared, as Pack's stack test — not alike
  // shells that merely cross or touch)
  std::map<int, std::array<double, 4>> shBox; for (int f : faces) { auto it = shBox.try_emplace(S.of[size_t(f)], std::array<double, 4>{1e300, 1e300, -1e300, -1e300}).first; auto& b = it->second;
    for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const UV& q = m.cornerUV[size_t(k)]; b[0] = std::min(b[0], double(q[0])); b[1] = std::min(b[1], double(q[1])); b[2] = std::max(b[2], double(q[0])); b[3] = std::max(b[3], double(q[1])); } }
  auto stacked = [&](int a, int b) { if (!alike(a, b)) return false; const auto &p = shBox[a], &q = shBox[b];
    const double iw = std::min(p[2], q[2]) - std::max(p[0], q[0]), ih = std::min(p[3], q[3]) - std::max(p[1], q[1]); if (iw <= 0 || ih <= 0) return false;
    const double uw = std::max(p[2], q[2]) - std::min(p[0], q[0]), uh = std::max(p[3], q[3]) - std::min(p[1], q[1]); return uw > 0 && uh > 0 && (iw * ih) / (uw * uh) > 0.8; };
  // turned over
  { const auto fl = flipped(m, T, S); for (int f : fl) if (in.count(f)) R.flipped.push_back(f); }
  // past the tile
  for (int f : faces) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const UV& q = m.cornerUV[size_t(k)]; if (q[0] < 0 || q[0] > 1 || q[1] < 0 || q[1] > 1) { R.outside.push_back(f); break; } }
  // over each other: cells of 4 texels — a cell two polygons take (not a stack's twins)
  { const int nx = std::clamp(o.w / 4, 64, 1024), ny = std::clamp(o.h / 4, 64, 1024); std::vector<int> own(size_t(nx) * size_t(ny), -1); std::set<int> bad; std::set<std::pair<int, int>> stacks;
    for (int f : faces) raster(m, T, f, nx, ny, 0, [&](int x, int y) { int& w = own[size_t(y) * size_t(nx) + size_t(x)]; if (w < 0) { w = f; return; } if (w == f) return;
      const int a = S.of[size_t(w)], b = S.of[size_t(f)]; if (a != b && stacked(a, b)) { stacks.insert(std::minmax(a, b)); return; } bad.insert(w); bad.insert(f); });
    { std::map<int, std::vector<int>> byS; for (int f : faces) byS[S.of[size_t(f)]].push_back(f); for (auto& [sh, fl] : byS) for (int f : selfOverlapFaces(m, T, fl)) bad.insert(f); }   // (and, exactly, the polygons of one shell over each other — a thin fold too)
    R.overlap.assign(bad.begin(), bad.end()); R.stacks = int(stacks.size()); }
  // the padding: the shells grown by half of it in cells of the padding's quarter — a cell two shells take; and the
  // tile's edges (half of it off them)
  if (o.pad > 0) { const double cellPx = std::max(1.0, o.pad / 4); const int nx = std::clamp(int(o.w / cellPx), 64, 2048), ny = std::clamp(int(o.h / cellPx), 64, 2048);
    const double grow = o.pad / 2 / (double(o.w) / nx); std::vector<int> own(size_t(nx) * size_t(ny), -1); std::set<int> bad;
    for (int f : faces) raster(m, T, f, nx, ny, grow, [&](int x, int y) { int& w = own[size_t(y) * size_t(nx) + size_t(x)]; if (w < 0) { w = f; return; }
      const int a = S.of[size_t(w)], b = S.of[size_t(f)]; if (a == b || stacked(a, b)) return; bad.insert(w); bad.insert(f); });
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
