// The Topology room's Fill Hole (the hole nearest the pointer — or those the selection touches — closed with
// polygons, the filling shown first: LMB fills, RMB turns it to the next variant) and Select N-gons. The filling's
// kinds: quads (a grid over the hole; an odd hole — one triangle first), quads + triangles (rows from both ends), a fan
// of triangles to a middle vertex, triangles without a middle vertex, one n-gon.
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace hm {

using tedit::MeshEd; using tedit::ek;

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }

// a hole's filling: indices below n — the hole's own vertices (in its order), from n — the new points (pts)
struct Fill { std::vector<Vec3> pts; std::vector<std::vector<int>> faces; };

// the hole laid flat on its own plane (Newell's normal: the hole runs anticlockwise there) — where its shape is judged:
// which corners point in, which cuts across it stay inside, which triangles are ears
struct Flat {
  std::vector<double> x, y; int n = 0;
  explicit Flat(const std::vector<Vec3>& L) {
    n = int(L.size()); Vec3 nn;
    for (int k = 0; k < n; k++) { const Vec3& a = L[size_t(k)]; const Vec3& b = L[size_t((k + 1) % n)]; nn.x += (a.y - b.y) * (a.z + b.z); nn.y += (a.z - b.z) * (a.x + b.x); nn.z += (a.x - b.x) * (a.y + b.y); }
    nn = nn.lengthSq() > 0 ? nn.normalized() : Vec3(0, 0, 1);
    const Vec3 u = (std::abs(nn.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0)).cross(nn).normalized(), v = nn.cross(u);
    for (auto& p : L) { x.push_back(p.dot(u)); y.push_back(p.dot(v)); }
  }
  void add(double px, double py) { x.push_back(px); y.push_back(py); }   // (a point made on a cut across the hole)
  double cross(int a, int b, int c) const { return (x[size_t(b)] - x[size_t(a)]) * (y[size_t(c)] - y[size_t(a)]) - (y[size_t(b)] - y[size_t(a)]) * (x[size_t(c)] - x[size_t(a)]); }
  double dist(int a, int b) const { return std::hypot(x[size_t(a)] - x[size_t(b)], y[size_t(a)] - y[size_t(b)]); }
  double angle(int a, int b, int c) const {                      // (the inner angle at b, degrees)
    const double ux = x[size_t(a)] - x[size_t(b)], uy = y[size_t(a)] - y[size_t(b)], vx = x[size_t(c)] - x[size_t(b)], vy = y[size_t(c)] - y[size_t(b)];
    return std::abs(std::atan2(ux * vy - uy * vx, ux * vx + uy * vy)) * 180 / M_PI; }
};
double eps2(const Flat& F, const std::vector<int>& P) { double s = 0; for (size_t i = 0; i < P.size(); i++) s += F.dist(P[i], P[(i + 1) % P.size()]); return 1e-9 * (s / std::max<size_t>(1, P.size())) * (s / std::max<size_t>(1, P.size())); }
// (a corner that turns inward — by more than 8°: a slightly bowed rim of a curved hole laid flat is not one)
bool reflexAt(const Flat& F, const std::vector<int>& P, size_t i, double e) { const size_t n = P.size(); const int a = P[(i + n - 1) % n], b = P[i], c = P[(i + 1) % n];
  return F.cross(a, b, c) < -e && 180 - F.angle(a, b, c) > 8; }
// (p inside the triangle or on one of its sides — a vertex on the side of an ear would leave a T-junction: two edges in line)
bool inTri(const Flat& F, int a, int b, int c, int p) {
  if (F.dist(p, a) < 1e-12 || F.dist(p, b) < 1e-12 || F.dist(p, c) < 1e-12) return false;
  const double e = 1e-7 * std::abs(F.cross(a, b, c)); return F.cross(a, b, p) >= -e && F.cross(b, c, p) >= -e && F.cross(c, a, p) >= -e; }
bool segsCross(const Flat& F, int a, int b, int c, int d) {
  const double d1 = F.cross(a, b, c), d2 = F.cross(a, b, d), d3 = F.cross(c, d, a), d4 = F.cross(c, d, b);
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}
bool insideP(const Flat& F, const std::vector<int>& P, double px, double py) {
  bool in = false; for (size_t i = 0, j = P.size() - 1; i < P.size(); j = i++) { const double xi = F.x[size_t(P[i])], yi = F.y[size_t(P[i])], xj = F.x[size_t(P[j])], yj = F.y[size_t(P[j])];
    if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in; }
  return in;
}
// a cut between P[i] and P[j] across the inside: crosses no side, its middle inside
bool chordOK(const Flat& F, const std::vector<int>& P, size_t i, size_t j) {
  const size_t n = P.size(); const int a = P[i], b = P[j];
  for (size_t k = 0; k < n; k++) { const int c = P[k], d = P[(k + 1) % n]; if (c == a || c == b || d == a || d == b) continue; if (segsCross(F, a, b, c, d)) return false; }
  return insideP(F, P, (F.x[size_t(a)] + F.x[size_t(b)]) / 2, (F.y[size_t(a)] + F.y[size_t(b)]) / 2);
}
// triangles by ears (the best-shaped ear first — its smallest angle the largest); start: where the walk begins
std::vector<std::vector<int>> earClip(const Flat& F, std::vector<int> V, int start) {
  std::vector<std::vector<int>> out; if (V.size() < 3) return out;
  std::rotate(V.begin(), V.begin() + long(((start % int(V.size())) + int(V.size())) % int(V.size())), V.end());
  const double e = eps2(F, V);
  while (V.size() > 3) {
    const size_t n = V.size(); long best = -1; double bq = -1;
    for (size_t i = 0; i < n; i++) {
      const int a = V[(i + n - 1) % n], b = V[i], c = V[(i + 1) % n]; if (F.cross(a, b, c) <= e) continue;
      bool clean = true; for (size_t k = 0; k < n && clean; k++) { const int p = V[k]; if (p == a || p == b || p == c) continue; if (inTri(F, a, b, c, p)) clean = false; }
      if (!clean) continue;
      const double q = std::min({F.angle(c, a, b), F.angle(a, b, c), F.angle(b, c, a)}); if (q > bq + 1e-9) { bq = q; best = long(i); }
    }
    if (best < 0) for (size_t i = 0; i < n; i++) if (F.cross(V[(i + n - 1) % n], V[i], V[(i + 1) % n]) > 0) { best = long(i); break; }   // (a degenerate stretch: any convex corner)
    if (best < 0) best = 0;
    const size_t i = size_t(best); out.push_back({V[(i + n - 1) % n], V[i], V[(i + 1) % n]}); V.erase(V.begin() + long(i));
  }
  out.push_back(V);
  return out;
}
// the hole cut into convex pieces along cuts from its inward corners (even: each piece an even number of corners —
// for quads); the cut that carries on a side of the corner straight across, ends on another inward corner and is
// shortest first; a long cut gets a vertex every side's length (the pieces' sides then match — rectangles), the new
// points added to L and F
std::vector<std::vector<int>> convexParts(Flat& F, std::vector<Vec3>& L, const std::vector<int>& P, bool even, int depth = 0) {
  const size_t n = P.size(); if (n <= 4 || depth > 64) return {P};
  const double e = eps2(F, P); double mean = 0; for (size_t i = 0; i < n; i++) mean += F.dist(P[i], P[(i + 1) % n]); mean /= double(n);
  struct C { double score; size_t i, j; }; std::vector<C> cs;
  for (size_t i = 0; i < n; i++) {
    if (!reflexAt(F, P, i, e)) continue;
    for (size_t j = 0; j < n; j++) {
      if (j == i || (j + 1) % n == i || (i + 1) % n == j) continue;
      const size_t s1 = (j + n - i) % n + 1, s2 = n - s1 + 2;
      if (s1 < 3 || s2 < 3) continue; if (even && (s1 % 2) != (s2 % 2)) continue;
      if (!chordOK(F, P, i, j)) continue;
      // (the cut that carries on one of the corner's two sides straight across first — the pieces then rectangles)
      const int pi = P[(i + n - 1) % n], ci = P[i], ni = P[(i + 1) % n], cj = P[j];
      auto ang = [&](double ux, double uy, double vx, double vy) { return std::abs(std::atan2(ux * vy - uy * vx, ux * vx + uy * vy)) * 180 / M_PI; };
      const double cx = F.x[size_t(cj)] - F.x[size_t(ci)], cy = F.y[size_t(cj)] - F.y[size_t(ci)];
      const double dev = std::min(ang(F.x[size_t(ci)] - F.x[size_t(pi)], F.y[size_t(ci)] - F.y[size_t(pi)], cx, cy), ang(F.x[size_t(ci)] - F.x[size_t(ni)], F.y[size_t(ci)] - F.y[size_t(ni)], cx, cy));
      cs.push_back({dev / 10 + F.dist(P[i], P[j]) / mean - (reflexAt(F, P, j, e) ? 1.5 : 0), i, j});
    }
  }
  if (cs.empty()) return {P};
  const C b = *std::min_element(cs.begin(), cs.end(), [](const C& x, const C& y) { return x.score < y.score; });
  const size_t s1 = (b.j + n - b.i) % n + 1;
  int k = std::max(1, int(std::lround(F.dist(P[b.i], P[b.j]) / mean)));
  if (even && (s1 + size_t(k) - 1) % 2) k = k > 1 && std::abs(F.dist(P[b.i], P[b.j]) / mean - (k - 1)) < std::abs(F.dist(P[b.i], P[b.j]) / mean - (k + 1)) ? k - 1 : k + 1;
  std::vector<int> cut;                                           // (the cut's own vertices, from i to j)
  for (int m = 1; m < k; m++) { const double t = double(m) / k; const int pi = P[b.i], pj = P[b.j];
    L.push_back(L[size_t(pi)] + (L[size_t(pj)] - L[size_t(pi)]) * t); F.add(F.x[size_t(pi)] + (F.x[size_t(pj)] - F.x[size_t(pi)]) * t, F.y[size_t(pi)] + (F.y[size_t(pj)] - F.y[size_t(pi)]) * t); cut.push_back(int(L.size()) - 1); }
  std::vector<int> A, B; for (size_t q = b.i;; q = (q + 1) % n) { A.push_back(P[q]); if (q == b.j) break; } for (auto it = cut.rbegin(); it != cut.rend(); ++it) A.push_back(*it);
  for (size_t q = b.j;; q = (q + 1) % n) { B.push_back(P[q]); if (q == b.i) break; } B.insert(B.end(), cut.begin(), cut.end());
  if ((even && (A.size() < 4 || B.size() < 4)) || A.size() >= n || B.size() >= n) return {P};   // (each piece smaller — else no cut)
  auto ra = convexParts(F, L, A, even, depth + 1), rb = convexParts(F, L, B, even, depth + 1); ra.insert(ra.end(), rb.begin(), rb.end()); return ra;
}
// an even piece as a grid of quads: its four corners (where it turns most) and its sides; opposite sides of the same
// length — an exact grid, else the sides shared out evenly; the inside — the blend of its four sides (on the surface
// the hole's rim describes). variant: which corner it starts from
void coons(Fill& Fo, const std::vector<Vec3>& L, const Flat& F, const std::vector<int>& P, int variant, int n0) {
  const int N = int(P.size());
  if (N == 4) { Fo.faces.push_back(P); return; }
  if (N < 4 || N % 2) { auto t = earClip(F, P, 0); Fo.faces.insert(Fo.faces.end(), t.begin(), t.end()); return; }
  std::vector<std::pair<double, int>> turn; for (int i = 0; i < N; i++) turn.push_back({180 - F.angle(P[size_t((i + N - 1) % N)], P[size_t(i)], P[size_t((i + 1) % N)]), i});
  std::sort(turn.begin(), turn.end(), std::greater<>());
  std::vector<int> cor; for (int k = 0; k < 4 && k < N; k++) cor.push_back(turn[size_t(k)].second); std::sort(cor.begin(), cor.end());
  int start = turn[0].second, a = std::max(1, N / 4), b = N / 2 - a;
  if (cor.size() == 4 && turn[3].first > 30) {                   // (four real corners — not a round hole: its sides)
    int s[4]; for (int k = 0; k < 4; k++) s[k] = (cor[size_t((k + 1) % 4)] - cor[size_t(k)] + N) % N;
    const int k = ((variant % 4) + 4) % 4; start = cor[size_t(k)];
    a = std::clamp((s[k] + s[(k + 2) % 4] + 1) / 2, 1, N / 2 - 1); b = N / 2 - a;   // (unequal opposite sides: their middle — a corner moves by a vertex)
  } else start = (start + variant) % N;                           // (round: a grid of a quarter of it each way)
  std::vector<int> idx; for (int k = 0; k < N; k++) idx.push_back(P[size_t((start + k) % N)]);
  auto Lp = [&](int k) { return L[size_t(idx[size_t(((k % N) + N) % N)])]; };
  auto bnd = [&](int i, int j) -> int {
    if (j == 0) return idx[size_t(i)];
    if (i == a) return idx[size_t(a + j)];
    if (j == b) return idx[size_t((a + b + (a - i)) % N)];
    return idx[size_t((2 * a + b + (b - j)) % N)]; };
  const Vec3 P00 = Lp(0), P10 = Lp(a), P11 = Lp(a + b), P01 = Lp(2 * a + b);
  std::map<std::pair<int, int>, int> G;
  for (int i = 0; i <= a; i++) for (int j = 0; j <= b; j++) {
    if (i == 0 || i == a || j == 0 || j == b) { G[{i, j}] = bnd(i, j); continue; }
    const double u = double(i) / a, v = double(j) / b;
    const Vec3 Bt = Lp(i), Tp = Lp(a + b + (a - i)), R = Lp(a + j), Le = Lp(2 * a + b + (b - j));
    Fo.pts.push_back(Bt * (1 - v) + Tp * v + Le * (1 - u) + R * u - (P00 * ((1 - u) * (1 - v)) + P10 * (u * (1 - v)) + P01 * ((1 - u) * v) + P11 * (u * v)));
    G[{i, j}] = n0 + int(Fo.pts.size()) - 1;
  }
  for (int i = 0; i < a; i++) for (int j = 0; j < b; j++) Fo.faces.push_back({G[{i, j}], G[{i + 1, j}], G[{i + 1, j + 1}], G[{i, j + 1}]});
}

// a hole that is a curved rectangle: four clear corners (each turning over 45°, the rest under 35°), its opposite
// sides as long — a grid over the whole of it (a hole on a curved surface laid flat may look as if it turned in)
bool gridHole(const Flat& F, const std::vector<int>& P);
// how far a piece is from an exact grid: its four corners' opposite sides' difference (-1: not four clear corners)
int gridMismatch(const Flat& F, const std::vector<int>& P) {
  const int N = int(P.size()); if (N < 4) return -1;
  std::vector<std::pair<double, int>> turn; for (int i = 0; i < N; i++) turn.push_back({180 - F.angle(P[size_t((i + N - 1) % N)], P[size_t(i)], P[size_t((i + 1) % N)]), i});
  std::sort(turn.begin(), turn.end(), std::greater<>()); if (turn[3].first < 30) return -1;
  std::vector<int> c; for (int k = 0; k < 4; k++) c.push_back(turn[size_t(k)].second); std::sort(c.begin(), c.end());
  int s[4]; for (int k = 0; k < 4; k++) s[k] = (c[size_t((k + 1) % 4)] - c[size_t(k)] + N) % N;
  return std::abs(s[0] - s[2]) + std::abs(s[1] - s[3]);
}
// an odd piece: one triangle off — where what is left lies as an exact grid if there is such a place, else the
// best-shaped ear (variant: the next such place) → the triangle; P loses its corner
std::vector<int> cutEar(const Flat& F, std::vector<int>& P, int off) {
  const int n = int(P.size()); const double e = eps2(F, P);
  std::vector<std::pair<double, int>> cand;
  for (int i = 0; i < n; i++) { const int a = P[size_t((i + n - 1) % n)], b = P[size_t(i)], c = P[size_t((i + 1) % n)]; if (F.cross(a, b, c) <= e) continue;
    bool clean = true; for (int k : P) if (k != a && k != b && k != c && inTri(F, a, b, c, k)) { clean = false; break; } if (!clean) continue;
    std::vector<int> rest = P; rest.erase(rest.begin() + i);
    // (a clean grid first: what is left an exact grid, else the nearest to one; the triangle on a side, not across a corner)
    const int mis = gridMismatch(F, rest); const bool corner = 180 - F.angle(a, b, c) > 45;
    cand.push_back({(gridHole(F, rest) ? 1000 : 0) + (mis >= 0 ? 300 - 60 * std::min(mis, 5) : 0) - (corner ? 80 : 0) + std::min({F.angle(c, a, b), F.angle(a, b, c), F.angle(b, c, a)}), i}); }
  if (cand.empty()) cand.push_back({0, 0});
  std::stable_sort(cand.begin(), cand.end(), [](auto& x, auto& y) { return x.first > y.first; });
  size_t pick = 0; { size_t same = 1; while (same < cand.size() && cand[same].first >= 1000 == cand[0].first >= 1000 && std::abs(cand[same].first - cand[0].first) < 1e-6) same++; pick = size_t(((off % int(same)) + int(same)) % int(same)); }
  const int i = cand[pick].second; std::vector<int> tri{P[size_t((i + n - 1) % n)], P[size_t(i)], P[size_t((i + 1) % n)]}; P.erase(P.begin() + i);
  return tri;
}

bool gridHole(const Flat& F, const std::vector<int>& P) {
  const int N = int(P.size()); if (N < 4 || N % 2) return false;
  std::vector<std::pair<double, int>> turn; for (int i = 0; i < N; i++) turn.push_back({180 - F.angle(P[size_t((i + N - 1) % N)], P[size_t(i)], P[size_t((i + 1) % N)]), i});
  std::sort(turn.begin(), turn.end(), std::greater<>());
  if (turn[3].first < 45 || (N > 4 && turn[4].first > 35)) return false;
  std::vector<int> c; for (int k = 0; k < 4; k++) c.push_back(turn[size_t(k)].second); std::sort(c.begin(), c.end());
  int s[4]; for (int k = 0; k < 4; k++) s[k] = (c[size_t((k + 1) % 4)] - c[size_t(k)] + N) % N;
  return s[0] == s[2] && s[1] == s[3];
}

// a hole one polygon wide (a gap along a band — its two long sides as many vertices, a short side at each end): the
// rungs across it, vertex to vertex, all about as short as its ends — one row of quads between them (laid flat such a
// band, bent round a head, turns in and out and the grid would go wrong); -1: not such a hole, else where the first end starts
int stripAt(const std::vector<Vec3>& L) {
  const int n = int(L.size()); if (n < 8 || n % 2) return -1;
  const int h = n / 2; int best = -1; double bs = 1e300;
  for (int k = 0; k < n; k++) {                                   // (the end k+1 → k; the other end h further round)
    const double e0 = L[size_t(k)].distanceTo(L[size_t((k + 1) % n)]), e1 = L[size_t((k + h) % n)].distanceTo(L[size_t((k + h + 1) % n)]);
    const double cap = 1.6 * std::max(e0, e1); double sum = 0, side = 0; bool ok = cap > 0;
    for (int i = 0; i < h && ok; i++) { const double r = L[size_t((k + 1 + i) % n)].distanceTo(L[size_t(((k - i) % n + n) % n)]); if (r > cap) ok = false; sum += r; }
    for (int i = 0; i + 1 < h; i++) side += L[size_t((k + 1 + i) % n)].distanceTo(L[size_t((k + 2 + i) % n)]);
    if (ok && side > 2.5 * std::max(e0, e1) && sum < bs) { bs = sum; best = k; } }
  return best;
}

Fill fillHole(const std::vector<Vec3>& L0, const QString& mode, int off) {
  Fill Fo; const int n = int(L0.size()); if (n < 3) return Fo;
  if (mode == "quads" || mode == "quadtri") if (const int k = stripAt(L0); k >= 0) {   // (a band's gap: one row of quads)
    for (int i = 0; i + 1 < n / 2; i++) Fo.faces.push_back({(k + 1 + i) % n, (k + 2 + i) % n, ((k - i - 1) % n + n) % n, ((k - i) % n + n) % n});
    return Fo; }
  std::vector<Vec3> L = L0; Flat F(L); std::vector<int> P(static_cast<size_t>(n)); std::iota(P.begin(), P.end(), 0);
  // (the points the cuts across the hole made: the filling's first new points, in order)
  auto parts = [&](bool even) { auto r = convexParts(F, L, P, even); for (size_t k = size_t(n); k < L.size(); k++) Fo.pts.push_back(L[k]); return r; };
  if (mode == "ngon" || n == 3) { Fo.faces.push_back(P); return Fo; }
  if (mode == "tris") { Fo.faces = earClip(F, P, off); return Fo; }
  if (mode == "fan") {                                           // (a centre for each convex piece — one for a convex hole)
    for (auto& part : parts(false)) {
      if (part.size() == 3) { Fo.faces.push_back(part); continue; }
      Vec3 c; for (int k : part) c += L[size_t(k)]; Fo.pts.push_back(c * (1.0 / double(part.size()))); const int ci = n + int(Fo.pts.size()) - 1;
      for (size_t k = 0; k < part.size(); k++) Fo.faces.push_back({part[k], part[(k + 1) % part.size()], ci});
    }
    return Fo;
  }
  if (mode == "quadtri") {                                       // (convex pieces, each a grid — an odd one with a triangle first: triangles only where quads can't be)
    if (gridHole(F, P)) { coons(Fo, L, F, P, off, n); return Fo; }
    for (auto& part : parts(false)) {
      if (part.size() <= 4) { Fo.faces.push_back(part); continue; }
      if (part.size() % 2) Fo.faces.push_back(cutEar(F, part, off));
      coons(Fo, L, F, part, off, n);
    }
    return Fo;
  }
  // quads: an odd hole — first the best ear off (one triangle); then convex even pieces, each a grid
  if (n % 2) Fo.faces.push_back(cutEar(F, P, off));
  if (gridHole(F, P)) { coons(Fo, L, F, P, off, n); return Fo; }
  for (auto& part : parts(true)) coons(Fo, L, F, part, off, n);
  return Fo;
}

// the mesh's holes: its rim's closed loops, each run against the polygons beside it (the filling agrees) — the rim
// taken side by side, each side the way back along its polygon; where the rim passes a vertex twice (two holes
// touching at a corner — a bow tie), each loop closed by the shortest way back: the holes apart, each one fillable
std::vector<std::vector<int>> holesOf(const topo::TTopo& T) {
  std::map<int, std::vector<std::pair<int, int>>> out;            // (vertex → (next vertex, side) the way round the holes)
  std::vector<int> sides; std::map<int, std::pair<int, int>> dirOf;   // (dirOf: each side's way round its hole)
  for (int e = 0; e < T.ne; e++) { if (T.ef1[size_t(e)] >= 0) continue; const int f = T.ef0[size_t(e)], a0 = T.ea[size_t(e)], b0 = T.eb[size_t(e)]; int a = a0, b = b0;
    for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int k2 = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
      if (T.fv[size_t(k)] == a0 && T.fv[size_t(k2)] == b0) { a = b0; b = a0; break; } }   // (the polygon runs a0 → b0: the hole b0 → a0)
    out[a].push_back({b, e}); out[b].push_back({a, e}); dirOf[e] = {a, b}; sides.push_back(e); }   // (both ways: the polygons beside a hole may be turned unevenly)
  std::set<int> used; std::vector<std::vector<int>> res;
  // (the loop through side s: the shortest way back from its end to its start along the sides not taken yet)
  auto cycleFrom = [&](int s) { std::vector<int> L; const int u = dirOf[s].first, v = dirOf[s].second;
    std::map<int, std::pair<int, int>> from; std::vector<int> q{v}; from[v] = {-1, -1}; bool found = false;
    for (size_t i = 0; i < q.size() && !found; i++) { const int x = q[i]; auto it = out.find(x); if (it == out.end()) continue;
      for (auto& [y, e] : it->second) { if (e == s || used.count(e) || from.count(y)) continue; from[y] = {x, e}; if (y == u) { found = true; break; } q.push_back(y); } }
    if (!found) return L;
    for (int x = u; x != v; x = from[x].first) L.push_back(x);
    L.push_back(v); std::reverse(L.begin(), L.end()); std::rotate(L.begin(), L.end() - 1, L.end()); return L; };
  auto take = [&](const std::vector<int>& L) {                  // (its sides taken; turned the way most of them run)
    std::vector<int> o = L; int agree = 0;
    for (size_t i = 0; i < o.size(); i++) { const int a = o[i], b = o[(i + 1) % o.size()];
      for (auto& [y, e] : out[a]) if (y == b && !used.count(e)) { used.insert(e); if (dirOf[e].first == a) agree++; break; } }
    if (agree * 2 < int(o.size())) std::reverse(o.begin(), o.end()); if (o.size() >= 3) res.push_back(o); };
  // (first the plain holes — every corner on two sides of the rim; then, where holes touch at a corner, the shortest
  // loop first, again and again: each hole its own loop)
  for (int s : sides) { if (used.count(s)) continue; auto L = cycleFrom(s); if (L.empty()) continue;
    bool plain = true; for (int x : L) if (out[x].size() != 2) plain = false; if (plain) take(L); }
  for (bool more = true; more;) { more = false; std::vector<int> best;
    for (int s : sides) { if (used.count(s)) continue; auto L = cycleFrom(s); if (!L.empty() && (best.empty() || L.size() < best.size())) best = L; }
    if (!best.empty()) { take(best); more = true; } }
  return res;
}

// Subdivision: so many loops inside the hole before it is filled (rows of quads from the rim inward), each loop the
// rim drawn towards the middle; a hole not every corner of which sees its middle — cut into convex pieces first (as
// the quads are), each piece with loops of its own round its own middle (a thin part keeps its width); then the
// innermost loop filled in the chosen way
Fill fillWithLoops(const std::vector<Vec3>& L, const QString& mode, int off, int S, int depth = 0) {
  if (S <= 0 || L.size() < 3) return fillHole(L, mode, off);
  const int n = int(L.size()); Flat F(L);
  // (seen from the middle: every side turns the same way round it)
  double cx = 0, cy = 0; for (int i = 0; i < n; i++) { cx += F.x[size_t(i)]; cy += F.y[size_t(i)]; } cx /= n; cy /= n;
  bool star = true; for (int i = 0; i < n && star; i++) { const int j = (i + 1) % n;
    if ((F.x[size_t(j)] - F.x[size_t(i)]) * (cy - F.y[size_t(i)]) - (F.y[size_t(j)] - F.y[size_t(i)]) * (cx - F.x[size_t(i)]) <= 0) star = false; }
  if (!star && depth < 8) {
    std::vector<Vec3> pool = L; std::vector<int> P(static_cast<size_t>(n)); std::iota(P.begin(), P.end(), 0);
    auto parts = convexParts(F, pool, P, mode == "quads" || mode == "quadtri");
    if (parts.size() > 1) {
      Fill out; for (size_t k = size_t(n); k < pool.size(); k++) out.pts.push_back(pool[k]);   // (the cuts' points: pool index = the filling's index)
      for (auto& part : parts) {
        std::vector<Vec3> Lp; for (int k : part) Lp.push_back(pool[size_t(k)]);
        Fill f = fillWithLoops(Lp, mode, off, S, depth + 1);
        const int m = int(part.size()), base = n + int(out.pts.size());
        out.pts.insert(out.pts.end(), f.pts.begin(), f.pts.end());
        for (auto& face : f.faces) { std::vector<int> g; for (int k : face) g.push_back(k < m ? part[size_t(k)] : base + (k - m)); out.faces.push_back(g); }
      }
      return out;
    }
  }
  // (a narrow piece — its shorter side a few edges long: no more loops than fit across it, a strip one edge wide none)
  { std::vector<std::pair<double, int>> turn; for (int i = 0; i < n; i++) turn.push_back({180 - F.angle((i + n - 1) % n, i, (i + 1) % n), i});
    std::sort(turn.begin(), turn.end(), std::greater<>());
    if (n >= 4 && turn[3].first > 30) { std::vector<int> cor; for (int k = 0; k < 4; k++) cor.push_back(turn[size_t(k)].second); std::sort(cor.begin(), cor.end());
      int side = n; for (int k = 0; k < 4; k++) side = std::min(side, (cor[size_t((k + 1) % 4)] - cor[size_t(k)] + n) % n);
      S = std::min(S, std::max(0, side - 1)); if (S <= 0) return fillHole(L, mode, off); } }
  Vec3 c; for (auto& p : L) c += p; c = c * (1.0 / n);
  std::vector<std::vector<Vec3>> rings;
  for (int r = 1; r <= S; r++) { std::vector<Vec3> R; for (int i = 0; i < n; i++) R.push_back(L[size_t(i)] + (c - L[size_t(i)]) * (double(r) / (S + 1))); rings.push_back(R); }
  Fill inner = fillHole(rings.back(), mode, off);
  Fill out;
  for (auto& R : rings) out.pts.insert(out.pts.end(), R.begin(), R.end());
  out.pts.insert(out.pts.end(), inner.pts.begin(), inner.pts.end());
  auto ringIdx = [&](int r, int i) { i = ((i % n) + n) % n; return r == 0 ? i : n + (r - 1) * n + i; };
  for (int r = 1; r <= S; r++) for (int i = 0; i < n; i++) out.faces.push_back({ringIdx(r - 1, i), ringIdx(r - 1, i + 1), ringIdx(r, i + 1), ringIdx(r, i)});
  for (auto& f : inner.faces) { std::vector<int> g; for (int k : f) g.push_back(k < n ? ringIdx(S, k) : n + S * n + (k - n)); out.faces.push_back(g); }
  return out;
}


// Edge Flow: the new vertices moved so the patch carries on the shape round it (its bending — not a flat plane): a
// smooth fairing (each vertex's Laplacian evened with its neighbours', the rim and the polygons outside held), blended
// with the flat place by the amount
void edgeFlow(MeshEd& E, const std::vector<int>& free, double amount) {
  if (free.empty() || amount <= 0) return;
  std::map<int, std::set<int>> adj;
  for (auto& F : E.f) { const size_t n = F.v.size(); for (size_t k = 0; k < n; k++) { const int a = F.v[k], b = F.v[(k + 1) % n]; adj[a].insert(b); adj[b].insert(a); } }
  std::set<int> fr(free.begin(), free.end()), ring1 = fr; for (int v : free) for (int w : adj[v]) ring1.insert(w);
  std::vector<Vec3> P = E.pos; const std::vector<Vec3> P0 = E.pos;
  std::map<int, Vec3> U;
  const int iters = 200 + int(std::sqrt(double(free.size())) * 40);
  for (int it = 0; it < iters; it++) {
    for (int v : ring1) { const auto& nb = adj[v]; if (nb.empty()) { U[v] = Vec3(); continue; } Vec3 a; for (int w : nb) a += P[size_t(w)]; U[v] = a * (1.0 / double(nb.size())) - P[size_t(v)]; }
    for (int v : free) { const auto& nb = adj[v]; if (nb.empty()) continue; Vec3 a; for (int w : nb) { auto u = U.find(w); if (u != U.end()) a += u->second; }
      const Vec3 U2 = a * (1.0 / double(nb.size())) - U[v]; P[size_t(v)] = P[size_t(v)] - U2 * 0.25; }
  }
  for (int v : free) E.pos[size_t(v)] = P0[size_t(v)] + (P[size_t(v)] - P0[size_t(v)]) * std::clamp(amount, 0.0, 1.0);
}
}  // namespace

QString Viewport::tSelectNgons(bool add) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) { ui::toast(tr("Select a mesh first")); return "!"; }
  if (topoHeavy(m)) { ui::toast(tr("A high-poly mesh — Object mode only")); return "!"; }
  if (topoMode != "poly") setTopoMode("poly", true);
  auto T = topo::ttopo(baseSp(*it)); std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (T->faceSize(f) > 4) fs.push_back(f);
  tselApply(m, fs, add ? "add" : "set", 'f');
  const QString msg = fs.empty() ? tr("No n-gons — every polygon has three or four sides") : tr("N-gons: %1").arg(fs.size()); ui::toast(msg); return msg;
}

void Viewport::fillSnapPts(int m, std::vector<Vec3>& pts) const {
  if (!tset.snap || pts.empty()) return;
  const Item* it = scene_->item(m); if (!it) return;
  std::vector<std::shared_ptr<const topo::Bvh>> tg;
  for (auto& [id, t] : scene_->items) if (id != m && t.type == ItemType::IMesh && t.poly && scene_->isVisible(t)) if (auto b = bvhOf(id, true)) tg.push_back(b);
  if (tg.empty()) return;
  const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse();
  for (auto& p : pts) { const Vec3 w = M.apply(p); double bd = 1e300; Vec3 best = w;
    for (auto& b : tg) { double d; if (auto q = b->closest(w, nullptr, &d); q && d < bd) { bd = d; best = *q; } }
    p = Mi.apply(best); }
}

// the holes aimed at filled in E: the kind, the loops of Subdivision, the Edge Flow, then Snap → the new polygons
std::vector<int> Viewport::fillBuild(int m, MeshEd& E, QJsonArray* loops) {
  std::vector<int> made, nvAll;
  for (auto& L : tt_.fillLoops) {
    std::vector<Vec3> P; for (int v : L) P.push_back(E.pos[size_t(v)]);
    Fill F = fillWithLoops(P, ttools.fillMode, tt_.fillOff, ttools.fillSub);
    std::vector<int> nv; for (auto& p : F.pts) nv.push_back(E.addVert(p));
    for (auto& f : F.faces) { std::vector<int> vs; for (int k : f) vs.push_back(k < int(L.size()) ? L[size_t(k)] : nv[size_t(k - int(L.size()))]); made.push_back(E.addFace(vs)); }
    nvAll.insert(nvAll.end(), nv.begin(), nv.end());
    if (loops) { QJsonArray l; for (int v : L) l.append(v); loops->append(l); }
  }
  edgeFlow(E, nvAll, ttools.fillFlow);
  if (tset.snap) { std::vector<Vec3> q; for (int v : nvAll) q.push_back(E.pos[size_t(v)]); fillSnapPts(m, q); for (size_t i = 0; i < nvAll.size(); i++) E.pos[size_t(nvAll[i])] = q[i]; }
  return made;
}

// the hole(s) aimed at: those the selection touches, else the one nearest the pointer — their filling shown
void Viewport::fillPreview(const QPointF& mp, const Vec3* aim) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; std::vector<std::vector<int>> was = tt_.fillLoops; tt_.fillLoops.clear();
  if (!it || topoHeavy(m)) { tt_.fillPrev.clear(); ttTouch(); return; }
  auto T = topo::ttopo(baseSp(*it)); const PolyMesh* B = baseOf(*it);
  if (tt_.fillB != B) { tt_.fillB = B; tt_.fillAll = holesOf(*T); tt_.fillKey.clear(); }
  // (only a picked rim — edges or vertices — chooses the holes: picked polygons (the patch just made among them) do
  // not, else every hole they touch would be filled at once; then the one at the pointer)
  std::set<int> sv; if (tsel_.id == m && (topoMode == "edge" || topoMode == "vert")) { auto vs = tSelVerts(m); sv.insert(vs.begin(), vs.end()); }
  for (auto& L : tt_.fillAll) for (int v : L) if (sv.count(v)) { tt_.fillLoops.push_back(L); break; }
  // (the edges picked closing a loop of their own — not one of the holes found, a side of it shared by more polygons:
  // that loop filled, as it is picked)
  if (tsel_.id == m && topoMode == "edge" && tsel_.e.size() >= 3) { std::vector<std::vector<int>> own;
    std::vector<tedit::EK> es; for (int e : tsel_.e) if (e < T->ne) es.push_back(ek(T->ea[size_t(e)], T->eb[size_t(e)]));
    for (auto& c : tedit::edgeChains(es)) if (c.closed && c.v.size() >= 3) own.push_back(c.v);
    if (!own.empty()) tt_.fillLoops = own; }
  if (tt_.fillAll.empty() && tt_.fillLoops.empty()) { tt_.fillPrev.clear(); ui::hud(tr("No holes in this mesh"), 900); ttTouch(); return; }
  const auto W = ttWorld(m);
  if (aim) { tt_.fillLoops.clear(); double bd = 1e300; int bi = -1;
    for (size_t h = 0; h < tt_.fillAll.size(); h++) for (int v : tt_.fillAll[h]) { const double d = W[size_t(v)].distanceTo(*aim); if (d < bd) { bd = d; bi = int(h); } }
    if (bi >= 0) tt_.fillLoops.push_back(tt_.fillAll[size_t(bi)]); }
  if (tt_.fillLoops.empty()) {
    Projector P = projector(); double bd = 1e300; int bi = -1;
    for (size_t h = 0; h < tt_.fillAll.size(); h++) { const auto& L = tt_.fillAll[h];
      for (size_t k = 0; k < L.size(); k++) { ScreenPt a = P.toScreen(W[size_t(L[k])]), b = P.toScreen(W[size_t(L[(k + 1) % L.size()])]); if (a.z >= 1 || b.z >= 1) continue;
        const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? std::clamp(((mp.x() - a.x) * dx + (mp.y() - a.y) * dy) / l2, 0.0, 1.0) : 0;
        const double d = std::hypot(mp.x() - a.x - t * dx, mp.y() - a.y - t * dy); if (d < bd) { bd = d; bi = int(h); } } }
    if (bi >= 0) tt_.fillLoops.push_back(tt_.fillAll[size_t(bi)]);
  }
  // (made again only when what it shows changed: the holes, the kind, the variant, the loops, the flow, Snap)
  QString key = ttools.fillMode + QString("|%1|%2|%3|%4|").arg(tt_.fillOff).arg(ttools.fillSub).arg(ttools.fillFlow).arg(tset.snap);
  for (auto& L : tt_.fillLoops) key += QString::number(L.size()) + ":" + QString::number(L[0]) + ",";
  if (key == tt_.fillKey && was == tt_.fillLoops) return;
  tt_.fillKey = key; tt_.fillPrev.clear();
  MeshEd E = MeshEd::from(*B); const size_t f0 = E.f.size(); fillBuild(m, E, nullptr);
  const Affine M = meshmods::meshMatrix(*scene_, *it);
  for (size_t fi = f0; fi < E.f.size(); fi++) { std::vector<Vec3> q; for (int v : E.f[fi].v) q.push_back(M.apply(E.pos[size_t(v)])); tt_.fillPrev.push_back(q); }
  ttTouch();
}

// Edge Flow / Subdivision changed: the patch just made — made again (while it is the selection), else the preview
void Viewport::fillAdjustShow() {
  const Item* li = tt_.lastM >= 0 ? scene_->item(tt_.lastM) : nullptr;
  const bool patch = tt_.lastOp == "fill" && li && baseSp(*li) == tt_.lastAfter && tsel_.id == tt_.lastM && tsel_.f == tt_.fillSelAfter;
  if (patch) modelReapply(); else { tt_.fillKey.clear(); fillPreview(tt_.press); }
}

QString Viewport::fillApply() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  if (tt_.fillLoops.empty() || tt_.fillB != baseOf(*it)) fillPreview(tt_.mouse);
  if (tt_.fillLoops.empty()) { ui::toast(tr("No hole to fill — point near one (or select its rim)")); return "!"; }
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); QJsonArray loops;
  const std::vector<int> made = fillBuild(m, E, &loops);
  tt_.lastSel = tsel_; tt_.lastOp = "fill"; tt_.lastArgs = QJsonObject{{"loops", loops}, {"off", tt_.fillOff}};
  const int nh = int(tt_.fillLoops.size()); tt_.fillLoops.clear(); tt_.fillPrev.clear(); tt_.fillB = nullptr; tt_.fillKey.clear();
  const QString r = ttDone(m, E, B, tr("%1 hole%2 filled: %3 polygon%4 — Subdivision / Edge Flow change it while it stays selected").arg(nh).arg(nh == 1 ? "" : "s").arg(made.size()).arg(made.size() == 1 ? "" : "s"), 'f', made);
  tt_.fillSelAfter = tsel_.f;                                    // (the patch: re-made by the settings while this stays the selection)
  return r;
}

}  // namespace hm

namespace hm {
// (tests: the active mesh's holes — their sizes and middles, the biggest first)
QString Viewport::fillHolesInfo() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "no mesh";
  auto T = topo::ttopo(baseSp(*it)); auto H = holesOf(*T); const auto W = ttWorld(m);
  std::set<int> sv; if (tsel_.id == m) { auto vs = tSelVerts(m, QChar(0x66).toLatin1()); sv.insert(vs.begin(), vs.end()); }   // (its vertices on the selected polygons: k of n)
  std::sort(H.begin(), H.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
  QStringList out; for (auto& L : H) { int ks = 0; for (int v : L) ks += int(sv.count(v)); Vec3 c; for (int v : L) c += W[size_t(L[0] >= 0 ? v : 0)]; c = c * (1.0 / double(L.size()));
    out << QString("%1 edges (sel %5) @ %2,%3,%4").arg(L.size()).arg(c.x, 0, 'f', 2).arg(c.y, 0, 'f', 2).arg(c.z, 0, 'f', 2).arg(ks); }
  Box3 b = it->poly->bounds(); return QString("%1 holes; box %2..%3 · ").arg(H.size()).arg(QString("%1,%2,%3").arg(b.min.x, 0, 'f', 1).arg(b.min.y, 0, 'f', 1).arg(b.min.z, 0, 'f', 1)).arg(QString("%1,%2,%3").arg(b.max.x, 0, 'f', 1).arg(b.max.y, 0, 'f', 1).arg(b.max.z, 0, 'f', 1)) + out.join(" | ");
}
}  // namespace hm

namespace hm {
QString Viewport::testHoleLoops(int m) {
  const Item* it = scene_->item(m); if (!it) return "no mesh"; auto T = topo::ttopo(baseSp(*it)); QStringList o;
  for (auto& L : holesOf(*T)) { QStringList vs; for (int v : L) vs << QString::number(v); o << "[" + vs.join(",") + "]"; }
  QStringList rim, nm; std::map<std::pair<int, int>, int> cnt; const PolyMesh& B = *baseSp(*it); for (int f = 0; f < B.faceCount(); f++) { const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) { int a = B.faceVerts[size_t(k)], b = B.faceVerts[size_t(k + 1 < e ? k + 1 : s)]; cnt[{std::min(a, b), std::max(a, b)}]++; } }
  for (auto& [e, c] : cnt) { if (c == 1) rim << QString("%1-%2").arg(e.first).arg(e.second); else if (c > 2) nm << QString("%1-%2x%3").arg(e.first).arg(e.second).arg(c); }
  QStringList fs; for (int f = 0; f < B.faceCount() && f < 40; f++) { QStringList vs; for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) vs << QString::number(B.faceVerts[size_t(k)]); fs << "(" + vs.join(",") + ")"; }
  return QString("%1 holes: ").arg(o.size()) + o.join(" ") + " · rim " + rim.join(" ") + " · non-manifold " + nm.join(" ") + " · faces " + fs.join(" ");
}
}  // namespace hm
