#include "scene/TopoEdit.h"
#include "scene/Braid.h"
#include "scene/Edit.h"
#include "scene/Network.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QObject>
#include <algorithm>
#include <cmath>
#include <map>
#include <functional>
#include <numeric>
#include <set>

namespace hm::braid {

namespace {
const double PI = 3.14159265358979323846;
double ease(double s) { s = std::clamp(s, 0.0, 1.0); return s * s * (3 - 2 * s); }
double frac(double x) { return x - std::floor(x); }
double num(const QJsonObject& p, const char* k, double def) { const QJsonValue v = p.value(k); return v.isDouble() ? v.toDouble() : def; }

std::vector<Vec3> resample(const std::vector<Vec3>& pts, int n) {
  std::vector<double> L{0}; for (size_t i = 1; i < pts.size(); i++) L.push_back(L.back() + pts[i].distanceTo(pts[i - 1]));
  const double tot = L.back(); std::vector<Vec3> out; size_t j = 0;
  for (int i = 0; i < n; i++) { const double d = tot * i / (n - 1); while (j + 2 < pts.size() && L[j + 1] < d) j++;
    const double sl = L[j + 1] - L[j], t = sl > 0 ? std::clamp((d - L[j]) / sl, 0.0, 1.0) : 0; out.push_back(pts[j].lerp(pts[j + 1], t)); }
  return out;
}
Vec3 perp(const Vec3& v, const Vec3& t) { return v - t * v.dot(t); }
Vec3 rotateFromTo(const Vec3& a, const Vec3& b, const Vec3& v) {
  double r = a.dot(b) + 1; Quat q;
  if (r < 2.220446049250313e-16) { r = 0; q = std::abs(a.x) > std::abs(a.z) ? Quat{-a.y, a.x, 0, r} : Quat{0, -a.z, a.y, r}; }
  else { const Vec3 c = a.cross(b); q = Quat{c.x, c.y, c.z, r}; }
  return q.normalized().rotate(v);
}
struct Frame { Vec3 p, T, N, B; };
// frames along the curve as the Tube modifier makes them: the start normal outwards from the model's vertical axis, then
// carried along without twisting; turned by rot round the curve
std::vector<Frame> framesAlong(const std::vector<Vec3>& P, const Vec3& center, double rot) {
  const size_t n = P.size(); std::vector<Vec3> T(n);
  for (size_t i = 0; i < n; i++) { const Vec3 t = P[std::min(i + 1, n - 1)] - P[i ? i - 1 : 0]; T[i] = t.lengthSq() > 1e-20 ? t.normalized() : Vec3(0, -1, 0); }
  const Vec3 h(P[0].x - center.x, 0, P[0].z - center.z); Vec3 N = perp(h, T[0]);
  if (h.lengthSq() < 1e-12 || N.lengthSq() < 0.09 * h.lengthSq()) N = perp(Vec3(0, 1, 0), T[0]);
  if (N.lengthSq() < 1e-12) N = perp(Vec3(1, 0, 0), T[0]);
  N = N.normalized(); std::vector<Frame> out; out.reserve(n); const double c = std::cos(rot * PI / 180), s = std::sin(rot * PI / 180);
  for (size_t i = 0; i < n; i++) {
    if (i) { N = rotateFromTo(T[i - 1], T[i], N); N = perp(N, T[i]); if (N.lengthSq() < 1e-12) N = perp(Vec3(0, 1, 0), T[i]); N = N.normalized(); }
    const Vec3 B = N.cross(T[i]).normalized(); out.push_back({P[i], T[i], N * c + B * s, B * c - N * s}); }
  return out;
}
tube::Profile defaultProfile() { tube::Profile p; for (int k = 0; k < 8; k++) p.push_back({std::cos(k * PI / 4 - PI / 2), std::sin(k * PI / 4 - PI / 2) * 0.72}); return p; }
QString oldEnd(const QString& k) { return k == "cut" ? "separate" : k == "ring" || k == "band" ? "shared" : k == "point" ? "point" : k; }   // (saves before the rework)

// the strands' places across the braid (u) and in its depth (v) along it (s 0…1 — the weave's own, density applied), before
// the ends gather them
struct Weave {
  int N = 3, K = 8; QString pat; double g = 1, r = 0.5, h = 0.5, depth = 1, cross = 8;
  std::vector<std::vector<int>> laneAt;                  // [step][strand] — its lane before that step
  struct Ev { int s; double tau, amp; };
  std::vector<Ev> ev;
  void make() {
    laneAt.assign(size_t(K) + 1, std::vector<int>(size_t(N), 0)); std::vector<int> ord(static_cast<size_t>(N)); std::iota(ord.begin(), ord.end(), 0);
    auto record = [&](int k) { for (int l = 0; l < N; l++) laneAt[size_t(k)][size_t(ord[size_t(l)])] = l; };
    for (int k = 0; k < K; k++) {
      record(k); if (pat == "rope") continue;
      const bool left = k % 2 == 0; const int c = std::max(1, N / 2), a = left ? 0 : N - 1, b = left ? c : N - 1 - c, m = ord[size_t(a)];
      for (int i = 0; i < c; i++) {                        // (the outer strand over / under the ones it passes, towards the middle)
        const int lane = left ? 1 + i : N - 2 - i, q = ord[size_t(lane)];
        const double sign = pat == "fishtail" ? 1 : (pat == "dutch" ? -1 : 1) * (i % 2 ? -1 : 1), tau = (k + (i + 0.5) / c) / K;
        ev.push_back({m, tau, sign * h}); ev.push_back({q, tau, -sign * h}); }
      ord.erase(ord.begin() + a); ord.insert(ord.begin() + b, m);
    }
    record(K);
  }
  int phases() const { return N % 2 ? N : N + 1; }
  double theta(int i, double s) const {                  // (classic / Dutch: the strand's wave phase)
    const double periods = std::max(0.25, double(K) * std::max(1, N / 2) / (N * (N - 1)));
    return 2 * PI * periods * std::clamp(s, 0.0, 1.0) + 2 * PI * i / phases();
  }
  std::array<double, 2> at(int i, double s) const {
    if (pat == "rope") { const double R = N >= 3 ? r / std::sin(PI / N) : r, f = PI * cross * s + 2 * PI * i / N; return {R * std::cos(f), R * std::sin(f) * depth}; }
    if (pat == "classic" || pat == "dutch") {
      // (each strand a smooth wave across and twice as fast in depth: where two meet across, they are apart in depth —
      // no stops, no kinks; even counts spread over one phase more, so no two run opposite and meet in the middle)
      double minSin = 1;
      for (int a = 0; a < N; a++) for (int b = a + 1; b < N; b++) minSin = std::min(minSin, std::abs(std::sin(2 * PI * (b - a) / phases())));
      const double th = theta(i, s), A = (N - 1) / 2.0 * g, hz = h / std::max(minSin, 0.2) * (pat == "dutch" ? -1 : 1);
      return {A * std::sin(th), hz * std::sin(2 * th) * depth};
    }
    // (the lane through its places at the steps' starts, smoothly: Catmull-Rom — no stop at each step)
    const double x = std::clamp(s, 0.0, 1.0) * K; const int k = std::clamp(int(std::floor(x)), 0, K - 1); const double f = x - k, f2 = f * f, f3 = f2 * f;
    auto L = [&](int j) { return double(laneAt[size_t(std::clamp(j, 0, K))][size_t(i)]); };
    const double p0 = L(k - 1), p1 = L(k), p2 = L(k + 1), p3 = L(k + 2);
    const double lane = 0.5 * (2 * p1 + (p2 - p0) * f + (2 * p0 - 5 * p1 + 4 * p2 - p3) * f2 + (3 * p1 - p0 - 3 * p2 + p3) * f3);
    double v = 0; const double w = 0.55 / K;
    for (auto& q : ev) if (q.s == i) { const double d = (s - q.tau) / w; if (std::abs(d) < 1) v += q.amp * (std::cos(PI * d) + 1) / 2; }
    return {(lane - (N - 1) / 2.0) * g, v * depth};
  }
  // a stylized lobe: the strand's thickness × along one lobe — thick where it comes out on top, tapering to a point
  // where it goes under (a triangle seen from outside), thin while hidden
  double lobe(int i, double s) const {
    double ph;
    if (pat == "rope") ph = frac((PI * cross * s + 2 * PI * i / N) / (2 * PI));
    else if (pat == "classic" || pat == "dutch") { ph = frac(theta(i, s) / PI); if (pat == "dutch") ph = frac(ph + 0.5); }
    else ph = frac(s * K * 0.5 + double(i) / N);
    const double a = 0.12, m = 0.45;
    return ph < a ? m + (1 - m) * ease(ph / a) : ph < 0.5 ? 1 - (1 - m) * (ph - a) / (0.5 - a) : m;
  }
};

// everything a braid is made from: its frames along the curve, the weave, the shape along it, the ends
struct Ctx {
  Params P; int N = 3, K = 8; double width = 1, len = 1, r = 0.3; Weave W; std::vector<Frame> F; std::vector<tube::Profile> prof;
  std::vector<double> warp;                              // (the weave's place along it: density applied)
  double ringR() const { return N >= 3 ? r / std::sin(PI / N) * 0.97 : N == 2 ? r : 0; }
  Frame frameAt(double t) const {
    const double x = std::clamp(t, 0.0, 1.0) * double(F.size() - 1); const size_t i = std::min(size_t(std::floor(x)), F.size() - 2); const double f = x - double(i);
    const Frame& a = F[i]; const Frame& b = F[i + 1];
    Frame o; o.p = a.p.lerp(b.p, f); o.T = a.T.lerp(b.T, f).normalized(); o.N = perp(a.N.lerp(b.N, f), o.T).normalized(); o.B = o.N.cross(o.T).normalized(); return o;
  }
  // a start / middle / end setting at t: straight between them, or rounded
  double key(const double k[3], double t) const {
    t = std::clamp(t, 0.0, 1.0); const double h0 = t < 0.5 ? t / 0.5 : (t - 0.5) / 0.5, h = h0 + (ease(h0) - h0) * std::clamp(P.round, 0.0, 1.0);
    return t < 0.5 ? k[0] + (k[1] - k[0]) * h : k[1] + (k[2] - k[1]) * h;
  }
  void makeWarp() {
    warp.assign(257, 0.0); for (int i = 1; i <= 256; i++) warp[size_t(i)] = warp[size_t(i - 1)] + std::max(0.05, key(P.dk, (i - 0.5) / 256));
    const double tot = warp.back(); for (double& x : warp) x /= tot;
  }
  double warpAt(double t) const { const double x = std::clamp(t, 0.0, 1.0) * 256; const size_t i = std::min(size_t(std::floor(x)), size_t(255)); return warp[i] + (warp[i + 1] - warp[i]) * (x - double(i)); }
  // strand i at t: across, in depth, its thickness ×
  std::array<double, 3> local(int i, double t) const {
    const double s = warpAt(t); auto q = W.at(i, s); const double wk = std::max(0.05, key(P.wk, t)); q[0] *= wk; if (W.pat == "rope") q[1] *= wk;
    double rs = std::max(0.02, key(P.tk, t));
    if (P.sharp > 0) rs *= 1 + (W.lobe(i, s) - 1) * std::clamp(P.sharp, 0.0, 1.0);
    for (int e = 0; e < 2; e++) {
      const QString kind = e ? P.end1 : P.end0; if (kind == "separate") continue; const double te = e ? 1 - t : t, conv = std::max(0.005, P.conv);
      const double ang = 2 * PI * i / N + PI / 2, R = kind == "point" ? 0 : ringR(), w = ease(1 - te / conv);
      q = {q[0] + (R * std::cos(ang) - q[0]) * w, q[1] + (R * std::sin(ang) - q[1]) * w};
      if (kind == "point") { const double x = std::clamp(te / (conv * 0.6), 0.0, 1.0), cone = x, dome = std::sqrt(x * (2 - x)); rs *= cone + (dome - cone) * std::clamp(e ? P.tip1 : P.tip0, 0.0, 1.0); }
    }
    return {q[0], q[1], rs};
  }
  Vec3 world(const Frame& f, double u, double v) const { return f.p + f.B * u + f.N * v; }

  // the ends shaped by their connectors (their outlines round the end: NA angles from +across). Shared — one ring round
  // the gathered strands (C its outline now, R0 as made, A how far it sits off the end's plane, round it); Separate — a
  // ring round each strand's end (moved: the strand's end goes with it; reshaped: so is the end)
  static constexpr int NA = 72;
  struct StrandEnd { bool on = false; std::array<double, 2> shift{0, 0}; double ax = 0; std::vector<double> C, R0; };
  struct EndShape { QString kind; std::vector<double> C, R0, A; bool on = false; std::vector<StrandEnd> st; };
  EndShape ends[2];
  static double polar(const std::vector<double>& R, double th) {
    if (R.empty()) return 0; double x = std::fmod(th, 2 * PI); if (x < 0) x += 2 * PI; x = x / (2 * PI) * double(R.size());
    const size_t i = size_t(std::floor(x)) % R.size(), j = (i + 1) % R.size(); const double f = x - std::floor(x); return R[i] + (R[j] - R[i]) * f;
  }
  double endWeight(int e, double t) const { const double te = e ? 1 - t : t; return ease(1 - te / std::max(0.005, P.conv)); }
  double axialAt(double th, double t) const { double a = 0; for (int e = 0; e < 2; e++) if (ends[e].on && !ends[e].A.empty()) a += polar(ends[e].A, th) * endWeight(e, t); return a; }
  double axialStrand(int i, double t) const { double a = 0; for (int e = 0; e < 2; e++) if (size_t(i) < ends[e].st.size() && ends[e].st[size_t(i)].on) a += ends[e].st[size_t(i)].ax * endWeight(e, t); return a; }
  // strand i's piece of a gathered ring end: a slice of C, point j of n round its rim
  std::array<double, 2> sectorPoint(int e, int i, int j, int n) const {
    const double mid = 2 * PI * i / N + PI / 2, half = PI / N; std::vector<std::array<double, 2>> loop{{0, 0}};
    for (int k = 0; k <= 16; k++) { const double th = mid - half + 2 * half * k / 16, rr = polar(ends[e].C, th); loop.push_back({rr * std::cos(th), rr * std::sin(th)}); }
    std::vector<double> L{0}; for (size_t k = 0; k < loop.size(); k++) { const auto& a = loop[k]; const auto& b = loop[(k + 1) % loop.size()]; L.push_back(L.back() + std::hypot(b[0] - a[0], b[1] - a[1])); }
    // (one point at the slice's centre, the others evenly along its rim, both its ends among them — the slices meet with no gap)
    const int rank = int(std::lround(std::fmod(double(j) / n + 0.5, 1.0) * n)) % n; if (rank == 0) return {0, 0};
    const double d = L[1] + double(rank - 1) / std::max(1, n - 2) * (L[17] - L[1]); size_t k = size_t(std::upper_bound(L.begin(), L.end(), d - 1e-12) - L.begin()); k = std::clamp<size_t>(k, 1, loop.size());
    const auto& a = loop[k - 1]; const auto& b = loop[k % loop.size()]; const double s = L[k] - L[k - 1] > 0 ? (d - L[k - 1]) / (L[k] - L[k - 1]) : 0;
    return {a[0] + (b[0] - a[0]) * s, a[1] + (b[1] - a[1]) * s};
  }
  std::array<double, 2> morph(int i, int j, int n, std::array<double, 2> p, const std::array<double, 2>& c, double t) const {
    for (int e = 0; e < 2; e++) { const double w = endWeight(e, t); if (w <= 0) continue;
      if (ends[e].kind == "shared" && ends[e].on) { const auto q = sectorPoint(e, i, j, n); p = {p[0] + (q[0] - p[0]) * w, p[1] + (q[1] - p[1]) * w}; }
      else if (ends[e].kind == "separate" && size_t(i) < ends[e].st.size() && ends[e].st[size_t(i)].on) {
        const auto& S = ends[e].st[size_t(i)]; const double cx = c[0] + S.shift[0] * w, cy = c[1] + S.shift[1] * w;
        p = {p[0] + S.shift[0] * w, p[1] + S.shift[1] * w};
        const double th = std::atan2(p[1] - cy, p[0] - cx), rp = std::hypot(p[0] - cx, p[1] - cy), r0 = polar(S.R0, th); if (r0 < 1e-12) continue;   // (reshaped: most at the rim)
        const double s = std::pow(std::clamp(rp / r0, 0.0, 1.0), 2), d = (polar(S.C, th) - r0) * w * s; p = {p[0] + std::cos(th) * d, p[1] + std::sin(th) * d}; } }
    return p;
  }
  // the strands' cuts across the braid at t (stretched where they run slanted — not when each tube gets rings across
  // itself; squashed where they press into each other; shaped by the ends' connectors)
  std::vector<std::vector<std::array<double, 2>>> cuts(double t, bool withMorph, double dt = 0.004, bool stretch = true) const {
    std::vector<std::vector<std::array<double, 2>>> polys; std::vector<std::array<double, 2>> cen; std::vector<double> rad;
    for (int i = 0; i < N; i++) {
      auto q = local(i, t), q0 = local(i, std::max(0.0, t - dt)), q1 = local(i, std::min(1.0, t + dt)); const double dl = len * (std::min(1.0, t + dt) - std::max(0.0, t - dt));
      const double su = stretch ? std::min(3.0, std::sqrt(1 + std::pow((q1[0] - q0[0]) / std::max(dl, 1e-12), 2))) : 1, sv = stretch ? std::min(3.0, std::sqrt(1 + std::pow((q1[1] - q0[1]) / std::max(dl, 1e-12), 2))) : 1, rr = r * q[2];
      const auto& pr = prof[size_t(i)]; std::vector<std::array<double, 2>> poly; double m = 0;
      for (size_t j = 0; j < pr.size(); j++) { poly.push_back({q[0] + pr[j][0] * rr * su, q[1] + pr[j][1] * rr * sv}); m = std::max(m, std::hypot(pr[j][0] * rr * su, pr[j][1] * rr * sv)); }
      polys.push_back(std::move(poly)); cen.push_back({q[0], q[1]}); rad.push_back(m); }
    // (two strands pressed into each other: each flattened against the plane between them — as much as Squash says —
    // and spread sideways along it, as a soft strand pressed flat does)
    if (P.squash > 0) { const double sq = std::clamp(P.squash, 0.0, 1.0); auto orig = polys;
      auto press = [&](int i, double nx, double ny, double a) {   // (strand i against the plane a from its middle, facing n)
        const double tx = -ny, ty = nx, ri = std::max(rad[size_t(i)], 1e-12);
        for (size_t j = 0; j < polys[size_t(i)].size(); j++) { const auto& o = orig[size_t(i)][j]; const double ox = o[0] - cen[size_t(i)][0], oy = o[1] - cen[size_t(i)][1], x = ox * nx + oy * ny - a; if (x <= 0) continue;
          const double side = std::clamp((ox * tx + oy * ty) / ri, -1.0, 1.0) * x * sq * 0.6;
          polys[size_t(i)][j][0] += -nx * x * sq + tx * side; polys[size_t(i)][j][1] += -ny * x * sq + ty * side; } };
      for (int i = 0; i < N; i++) for (int k = i + 1; k < N; k++) {
        const double dx = cen[size_t(k)][0] - cen[size_t(i)][0], dy = cen[size_t(k)][1] - cen[size_t(i)][1], d = std::hypot(dx, dy), ri = rad[size_t(i)], rk = rad[size_t(k)];
        if (d < 1e-12 || d >= ri + rk || ri + rk < 1e-12) continue;
        const double a = d * ri / (ri + rk); press(i, dx / d, dy / d, a); press(k, -dx / d, -dy / d, d - a); } }
    if (withMorph) for (int i = 0; i < N; i++) { auto& poly = polys[size_t(i)]; const int n = int(poly.size()); for (int j = 0; j < n; j++) poly[size_t(j)] = morph(i, j, n, poly[size_t(j)], cen[size_t(i)], t); }
    return polys;
  }
  // their outer outline from the middle: on each of A rays the farthest crossing (its strand; −1 none)
  static void rays(const std::vector<std::vector<std::array<double, 2>>>& polys, int A, double th0, std::vector<double>& R, std::vector<int>& O) {
    R.assign(size_t(A), 0); O.assign(size_t(A), -1);
    for (int a = 0; a < A; a++) { const double th = th0 + 2 * PI * a / A, dx = std::cos(th), dy = std::sin(th);
      for (size_t i = 0; i < polys.size(); i++) { const auto& Q = polys[i];
        for (size_t e = 0; e < Q.size(); e++) { const auto& p = Q[e]; const auto& q = Q[(e + 1) % Q.size()]; const double ex = q[0] - p[0], ey = q[1] - p[1], den = dx * ey - dy * ex; if (std::abs(den) < 1e-18) continue;
          const double tt = (p[0] * ey - p[1] * ex) / den, s = (p[0] * dy - p[1] * dx) / den; if (s < -1e-9 || s > 1 + 1e-9 || tt < 0) continue; if (tt > R[size_t(a)]) { R[size_t(a)] = tt; O[size_t(a)] = int(i); } } } }
  }
  // (a direction the outline misses: between its neighbours; false — it misses most)
  static bool fillMisses(std::vector<double>& C, const std::vector<int>& O) {
    int hit = 0; for (int o : O) hit += o >= 0; if (hit < NA / 2) return false;
    for (int a = 0; a < NA; a++) if (O[size_t(a)] < 0) { int l = a, r = a; while (O[size_t(l)] < 0) l = (l + NA - 1) % NA; while (O[size_t(r)] < 0) r = (r + 1) % NA;
      const int dl = (a - l + NA) % NA, dr = (r - a + NA) % NA; C[size_t(a)] = (C[size_t(l)] * dr + C[size_t(r)] * dl) / std::max(1, dl + dr); }
    return true;
  }
  // the outline a connector is made on: the gathered ring; a strand's own cut round its middle
  std::vector<double> ringOutline(int e) const { return std::vector<double>(NA, ringR() + r * local(0, e)[2]); }
  std::vector<double> strandOutline(int e, int i) const {
    auto polys = cuts(e, false); const auto q = local(i, e); std::vector<std::array<double, 2>> poly; for (auto& p : polys[size_t(i)]) poly.push_back({p[0] - q[0], p[1] - q[1]});
    std::vector<double> R; std::vector<int> O; rays({poly}, NA, 0, R, O); return R;
  }
};
std::array<double, 2> rcOf(const Item& k, const std::array<double, 2>& def, const char* key = "rc") { const QJsonArray a = k.raw.value(key).toArray(); return a.size() == 2 ? std::array<double, 2>{a[0].toDouble(), a[1].toDouble()} : def; }
std::vector<double> arr(const Item& k, const char* key) { std::vector<double> o; const QJsonArray a = k.raw.value(key).toArray(); if (a.size() == Ctx::NA) for (const QJsonValue& x : a) o.push_back(x.toDouble()); return o; }
std::optional<Ctx> context(const Scene& sc, const CurveCache& cc, const Item& b, const Vec3& center, int samples) {
  const int src = b.raw.value("src").toInt(-1); auto ci = cc.find(src); if (ci == cc.end() || ci->second.p.size() < 2) return std::nullopt;
  Ctx c; c.P = paramsOf(b); c.N = std::clamp(c.P.strands, 2, 16); c.K = std::max(1, int(std::lround(c.P.cross)));
  const auto& S = ci->second.p; c.len = 0; for (size_t i = 1; i < S.size(); i++) c.len += S[i].distanceTo(S[i - 1]); if (!(c.len > 0)) return std::nullopt;
  c.width = c.P.width > 0 ? c.P.width : tube::autoTubeRadius(sc, cc, src) * 3;
  const double g = c.width / c.N; c.r = g * 0.72 * std::max(0.05, c.P.size);
  c.W.N = c.N; c.W.K = c.K; c.W.pat = c.P.pattern; c.W.g = g; c.W.r = c.r; c.W.h = c.r * (1.05 - 0.45 * std::clamp(c.P.tight, 0.0, 1.0)); c.W.depth = c.P.depth; c.W.cross = c.P.cross; c.W.make();
  c.makeWarp();
  c.F = framesAlong(resample(S, std::max(samples, 8)), center, c.P.rot); c.prof = profilesOf(b, c.N);
  for (int e = 0; e < 2; e++) { c.ends[e].kind = e ? c.P.end1 : c.P.end0; c.ends[e].st.resize(size_t(c.N)); }
  // (the connectors as they are now — in the end's own frame)
  for (const QJsonValue& x : b.raw.value("conns").toArray()) {
    const Item* k = sc.item(x.toInt(-1)); if (!k || k->type != ItemType::Curve) continue; const int e = k->raw.value("end").toInt(-1), slot = k->raw.value("strand").toInt(-1); if (e != 0 && e != 1) continue;
    auto ki = cc.find(k->id); if (ki == cc.end() || ki->second.p.size() < 3) continue;
    auto& E = c.ends[e]; const Frame f = c.frameAt(e); std::vector<std::array<double, 2>> poly; std::vector<double> axl;
    for (auto& p : ki->second.p) { const Vec3 d = p - f.p; poly.push_back({d.dot(f.B), d.dot(f.N)}); axl.push_back(d.dot(f.T)); }
    if (slot < 0 && E.kind == "shared") {
      std::vector<int> O; Ctx::rays({poly}, Ctx::NA, 0, E.C, O); if (!Ctx::fillMisses(E.C, O)) continue;   // (the end's middle far outside it: left as it is)
      std::vector<std::pair<double, double>> ta; for (size_t q = 0; q < poly.size(); q++) ta.push_back({std::atan2(poly[q][1], poly[q][0]), axl[q]});   // (its offset along the braid, round it)
      std::sort(ta.begin(), ta.end()); E.A.assign(Ctx::NA, 0.0);
      for (int a = 0; a < Ctx::NA; a++) { double th = 2 * PI * a / Ctx::NA; if (th > PI) th -= 2 * PI; size_t j = size_t(std::lower_bound(ta.begin(), ta.end(), std::make_pair(th, -1e300)) - ta.begin());
        const auto& p1 = ta[j % ta.size()]; const auto& p0 = ta[(j + ta.size() - 1) % ta.size()]; double span = p1.first - p0.first; if (span <= 0) span += 2 * PI; double off = th - p0.first; if (off < 0) off += 2 * PI;
        E.A[size_t(a)] = p0.second + (p1.second - p0.second) * std::clamp(off / std::max(span, 1e-12), 0.0, 1.0); }
      E.R0 = arr(*k, "rest"); if (E.R0.empty()) E.R0 = E.C; E.on = true;
    } else if (slot >= 0 && slot < c.N && E.kind == "separate") {
      // (moved: its middle off where it was at rest — the strand's end goes as far; its outline round that)
      auto& St = E.st[size_t(slot)]; double cx = 0, cy = 0, ax = 0; int n = 0;
      for (size_t q = 0; q < poly.size(); q++) { cx += poly[q][0]; cy += poly[q][1]; ax += axl[q]; n++; }
      if (!n) continue; cx /= n; cy /= n; ax /= n;
      const auto q0 = c.local(slot, e); const auto rc = rcOf(*k, {q0[0], q0[1]}), c0 = rcOf(*k, rc, "c0");
      St.shift = {cx - c0[0], cy - c0[1]};
      for (auto& p : poly) { p[0] -= rc[0] + St.shift[0]; p[1] -= rc[1] + St.shift[1]; }
      std::vector<int> O; Ctx::rays({poly}, Ctx::NA, 0, St.C, O); if (!Ctx::fillMisses(St.C, O)) continue;
      St.ax = ax; St.R0 = arr(*k, "rest"); if (St.R0.empty()) St.R0 = St.C; St.on = true;
    }
  }
  return c;
}
void placeAfter(Scene& sc, int id, int after) {
  Item* it = sc.item(id); std::vector<int>& a = it->parent >= 0 && sc.item(it->parent) ? sc.item(it->parent)->children : sc.roots;
  a.erase(std::remove(a.begin(), a.end(), id), a.end()); auto at = std::find(a.begin(), a.end(), after); a.insert(at == a.end() ? a.end() : at + 1, id);
}
// a mesh being put together: polygons with their corners' UVs, polygroups
struct Out {
  PolyMesh m;
  int vert(const Vec3& p) { m.pos.push_back(p); return int(m.pos.size()) - 1; }
  void face(std::vector<int> v, std::vector<std::array<float, 2>> uv, int g) {
    std::vector<int> f; std::vector<std::array<float, 2>> fu; for (size_t k = 0; k < v.size(); k++) { if (!f.empty() && f.back() == v[k]) continue; f.push_back(v[k]); fu.push_back(uv[k]); }
    while (f.size() > 1 && f.front() == f.back()) { f.pop_back(); fu.pop_back(); } if (f.size() < 3) return;
    m.addFace(f.data(), int(f.size())); m.cornerUV.insert(m.cornerUV.end(), fu.begin(), fu.end()); m.faceGroup.push_back(g); }
  int group(const QString& n) { m.groupNames.push_back(n); return int(m.groupNames.size()) - 1; }
  // the polygons from f0 on turned to face away from `inside(face centre)` if most of them face it
  void orientOut(int f0, const std::function<Vec3(const Vec3&)>& inside) {
    double s = 0; for (int f = f0; f < m.faceCount(); f++) { Vec3 c, n; const int a = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
      for (int k = a; k < e; k++) { const Vec3& p = m.pos[size_t(m.faceVerts[size_t(k)])]; const Vec3& q = m.pos[size_t(m.faceVerts[size_t(a + (k - a + 1) % (e - a))])]; n += p.cross(q); c += p; }
      c = c * (1.0 / (e - a)); s += n.dot(c - inside(c)); }
    if (s >= 0) return;
    for (int f = f0; f < m.faceCount(); f++) { const int a = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
      std::reverse(m.faceVerts.begin() + a, m.faceVerts.begin() + e); std::reverse(m.cornerUV.begin() + a, m.cornerUV.begin() + e); }
  }
};
bool collapsed(const std::vector<Vec3>& ring, double eps) { Vec3 c; for (auto& p : ring) c += p; c = c * (1.0 / ring.size()); for (auto& p : ring) if (p.distanceTo(c) > eps) return false; return true; }
}  // namespace

Params paramsOf(const Item& b) {
  const QJsonObject p = b.raw.value("p").toObject(); Params o;
  o.strands = int(num(p, "strands", o.strands)); o.pattern = p.value("pattern").toString(o.pattern); o.cross = num(p, "cross", o.cross); o.width = num(p, "width", 0);
  o.depth = num(p, "depth", o.depth); o.size = num(p, "size", o.size); o.tight = num(p, "tight", o.tight); o.rot = num(p, "rot", 0);
  for (int k = 0; k < 3; k++) { o.wk[k] = num(p, qPrintable(QString("w%1").arg(k)), 1); o.dk[k] = num(p, qPrintable(QString("d%1").arg(k)), 1); o.tk[k] = num(p, qPrintable(QString("t%1").arg(k)), 1); }
  if (!p.contains("t0")) { o.tk[0] = num(p, "r0", 1); o.tk[2] = num(p, "r1", 1); o.tk[1] = 1; }   // (saves before the rework: the thickness at the start / end)
  o.round = num(p, "round", 1); o.sharp = num(p, "sharp", 0); o.squash = num(p, "squash", 0);
  o.end0 = oldEnd(p.value("end0").toString(o.end0)); o.end1 = oldEnd(p.value("end1").toString(o.end1)); o.conv = num(p, "conv", o.conv); o.tip0 = num(p, "tip0", o.tip0); o.tip1 = num(p, "tip1", o.tip1);
  o.cocoon = p.value("cocoon").toString(o.cocoon); o.prev = int(num(p, "prev", o.prev));
  o.caps = p.value("caps").toBool(false); o.shared = p.value("shared").toBool(true);
  o.lockAnchors = int(num(p, "lockAnchors", o.lockAnchors)); o.lockTip = p.value("lockTip").toBool(true); o.lockR = num(p, "lockR", 1);
  return o;
}
std::vector<tube::Profile> profilesOf(const Item& b, int n) {
  const QJsonArray a = b.raw.value("profs").toArray(); const bool shared = b.raw.value("p").toObject().value("shared").toBool(true);
  std::vector<tube::Profile> out;
  for (int i = 0; i < n; i++) { tube::Profile p = a.isEmpty() ? tube::Profile{} : tube::profileOf(a[shared || i >= a.size() ? 0 : i]); if (p.size() < 3) p = defaultProfile(); out.push_back(p); }
  return out;
}
std::vector<int> braids(const Scene& sc) {
  std::vector<const Item*> v; for (auto& [id, it] : sc.items) if (it.type == ItemType::Braid) v.push_back(&it);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->order < b->order; }); std::vector<int> out; for (auto* it : v) out.push_back(it->id); return out;
}
int braidOfCurve(const Scene& sc, int curve) { for (int b : braids(sc)) if (sc.item(b)->raw.value("src").toInt(-1) == curve) return b; return -1; }
int braidOfItem(const Scene& sc, int id) {
  const Item* it = sc.item(id); if (!it) return -1;
  if (it->type == ItemType::Braid) return id;
  if (it->type == ItemType::Group && it->raw.value("braidGroup").toBool()) { for (int c : it->children) if (const Item* x = sc.item(c); x && x->type == ItemType::Braid) return c; return -1; }
  if (it->type == ItemType::Curve) { const int b = it->raw.value("braidOf").toInt(-1); if (const Item* x = sc.item(b); x && x->type == ItemType::Braid) return b; return braidOfCurve(sc, id); }
  return -1;
}

// ---------- the strands ----------
std::optional<Built> build(const Scene& sc, const CurveCache& cc, const Item& b, const Vec3& center, int rpc) {
  const Params P0 = paramsOf(b); const int K = std::max(1, int(std::lround(P0.cross)));
  const int NR = std::clamp(K * (rpc > 0 ? rpc : std::clamp(P0.prev, 2, 40)) + 1, 24, 2400);
  auto c = context(sc, cc, b, center, NR); if (!c) return std::nullopt;
  Built out; out.width = c->width; out.len = c->len; out.each = c->P.cocoon == "each";
  // the shared cocoon's levels: planes across the braid along its curve; on each, every strand's cut — so the rings of all
  // the tubes lie on the same levels
  out.strands.resize(size_t(c->N));
  for (int k = 0; k < NR; k++) {
    const double t = double(k) / (NR - 1); const Frame f = c->frameAt(t); auto polys = c->cuts(t, true, 0.5 / (NR - 1), !out.each);
    double rmax = 0; for (auto& q : polys) for (auto& p : q) rmax = std::max(rmax, std::hypot(p[0], p[1]));
    out.lv.push_back({f.p, f.N, f.B, f.T, rmax});
    for (int i = 0; i < c->N; i++) { const double ax = c->axialStrand(i, t); std::vector<Vec3> ring; Vec3 m;
      for (auto& p : polys[size_t(i)]) { ring.push_back(c->world(f, p[0], p[1]) + f.T * (c->axialAt(std::atan2(p[1], p[0]), t) + ax)); m += ring.back(); }
      auto& S = out.strands[size_t(i)]; S.mid.push_back(m * (1.0 / double(ring.size()))); S.rings.push_back(std::move(ring)); S.r.push_back(c->r * c->local(i, t)[2]); } }
  // a cocoon round each tube: each ring turned from across the braid to across the tube itself (round its middle)
  if (out.each) for (auto& S : out.strands) { const size_t n = S.mid.size();
    for (size_t k = 0; k < n; k++) { Vec3 T = S.mid[std::min(k + 1, n - 1)] - S.mid[k ? k - 1 : 0]; if (T.lengthSq() < 1e-20) continue; T = T.normalized();
      for (auto& p : S.rings[k]) p = S.mid[k] + rotateFromTo(out.lv[k].T, T, p - S.mid[k]); } }
  return out;
}

PolyMesh tubesMesh(const Scene& sc, const CurveCache& cc, const Item& b, const Vec3& center) {
  Out o; auto B = build(sc, cc, b, center); if (!B) return {};   // (as the preview shows it)
  const Params P = paramsOf(b); const auto prof = profilesOf(b, int(B->strands.size())); const double eps = B->width * 1e-6;
  for (size_t i = 0; i < B->strands.size(); i++) {
    const auto& S = B->strands[i]; const auto& pr = prof[i]; const int n = int(pr.size()), NR = int(S.rings.size()); const int g = o.group(QString("Strand %1").arg(i + 1)), f0 = o.m.faceCount();
    // (glued profile points — one vertex, its edges along the strand hard)
    std::vector<int> cols; std::vector<char> hard;
    for (int j = 0; j < n; j++) { if (tube::samePt(pr[size_t(j)], pr[size_t((j + n - 1) % n)]) && !cols.empty()) { hard.back() = 1; continue; } cols.push_back(j); hard.push_back(0); }
    if (tube::samePt(pr[size_t(cols.front())], pr[size_t(n - 1)]) && cols.size() < size_t(n)) hard.front() = 1;
    const int nu = int(cols.size()); double rmax = 0; for (double r : S.r) rmax = std::max(rmax, r);
    std::vector<std::vector<int>> V(static_cast<size_t>(NR));
    for (int k = 0; k < NR; k++) { std::vector<Vec3> ring; for (int j : cols) ring.push_back(S.rings[size_t(k)][size_t(j)]);
      if (collapsed(ring, eps)) { Vec3 c; for (auto& p : ring) c += p; V[size_t(k)].assign(size_t(nu), o.vert(c * (1.0 / nu))); }
      else for (auto& p : ring) V[size_t(k)].push_back(o.vert(p)); }
    const float vs = float(B->len / (2 * PI * std::max(rmax, 1e-9)));
    for (int k = 0; k + 1 < NR; k++) { const float v0 = float(k) / (NR - 1) * vs, v1 = float(k + 1) / (NR - 1) * vs;
      for (int q = 0; q < nu; q++) { const int q1 = (q + 1) % nu; const float u0 = float(q) / nu, u1 = float(q + 1) / nu;
        o.face({V[size_t(k)][size_t(q)], V[size_t(k)][size_t(q1)], V[size_t(k + 1)][size_t(q1)], V[size_t(k + 1)][size_t(q)]}, {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}, g); } }
    for (int q = 0; q < nu; q++) if (hard[size_t(q)]) for (int k = 0; k + 1 < NR; k++) if (V[size_t(k)][size_t(q)] != V[size_t(k + 1)][size_t(q)]) o.m.hardEdges.push_back({V[size_t(k)][size_t(q)], V[size_t(k + 1)][size_t(q)]});
    if (P.caps) for (int e = 0; e < 2; e++) { const auto& R = V[size_t(e ? NR - 1 : 0)]; if (std::set<int>(R.begin(), R.end()).size() < 3) continue;
      std::vector<int> f(R.begin(), R.end()); std::vector<std::array<float, 2>> uv; for (int q = 0; q < nu; q++) uv.push_back({0.5f + 0.5f * float(pr[size_t(cols[size_t(q)])][0]), 0.5f + 0.5f * float(pr[size_t(cols[size_t(q)])][1])});
      if (!e) { std::reverse(f.begin(), f.end()); std::reverse(uv.begin(), uv.end()); } o.face(f, uv, g); }
    // (outward: away from the strand's centre line)
    o.orientOut(f0, [&](const Vec3& p) { size_t bi = 0; double bd = 1e300; for (size_t k = 0; k < S.mid.size(); k++) { const double d = (S.mid[k] - p).lengthSq(); if (d < bd) { bd = d; bi = k; } } return S.mid[bi]; });
  }
  return o.m;
}

// ---------- made, turned into meshes / locks, their connectors ----------
QString createBraids(Scene& sc, const CurveCache& cc, Selection& sel, const Vec3& center) {
  std::vector<int> cs;
  for (int id : sel.items) { const Item* it = sc.item(id); if (!it || it->type != ItemType::Curve || it->ring >= 0 || !it->conn.isEmpty()) continue; auto ci = cc.find(id); if (ci == cc.end() || ci->second.p.size() < 2) continue;
    if (braidOfCurve(sc, id) < 0 && std::find(cs.begin(), cs.end(), id) == cs.end()) cs.push_back(id); }
  if (cs.empty()) return QObject::tr("!Select a curve (one without a braid yet), then press Create braid");
  std::vector<int> made;
  for (int cid : cs) {
    const Item* cv = sc.item(cid); edit::nextNumber(sc, ItemType::Braid);
    int num = 1; { const QRegularExpression re("^Braid (\\d+)\\b"); for (auto& [bid, bi] : sc.items) if (bi.type == ItemType::Braid) if (auto m = re.match(bi.name); m.hasMatch()) num = std::max(num, m.captured(1).toInt() + 1); }   // (its name: "Braid N (curve)")
    const int par = cv->parent; const QString cname = cv->name;
    // (its own folder, as a crown's: the braid first, its curve, its end connectors)
    edit::nextNumber(sc, ItemType::Group);
    Item& g = sc.addItem(ItemType::Group, par, QObject::tr("Braid %1").arg(num)); g.expanded = true; const int gid = g.id;
    g.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "auto"}, {"conv", QJsonValue()}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false},
                        {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}, {"braidGroup", true}};
    placeAfter(sc, gid, cid); sc.removeFromParent(cid); sc.item(cid)->parent = gid; sc.item(gid)->children.push_back(cid);
    Item& b = sc.addItem(ItemType::Braid, gid, QObject::tr("Braid %1 (%2)").arg(num).arg(cname)); const int id = b.id;
    const double w = tube::autoTubeRadius(sc, cc, cid) * 3;
    b.raw.insert("src", cid);
    b.raw.insert("p", QJsonObject{{"strands", 3}, {"pattern", "classic"}, {"cross", 12}, {"width", w}, {"depth", 1}, {"size", 1}, {"tight", 0.3}, {"rot", 0},
                                  {"w0", 1}, {"w1", 1}, {"w2", 1}, {"d0", 1}, {"d1", 1}, {"d2", 1}, {"t0", 1}, {"t1", 1}, {"t2", 1}, {"round", 1}, {"sharp", 0}, {"squash", 0},
                                  {"end0", "separate"}, {"end1", "point"}, {"conv", 0.08}, {"tip0", 0.5}, {"tip1", 0.5}, {"cocoon", "shared"}, {"prev", 10}, {"caps", false}, {"shared", true},
                                  {"lockAnchors", 3}, {"lockTip", true}, {"lockR", 1}});
    b.raw.insert("profs", QJsonArray{tube::profileJson(defaultProfile())}); b.raw.insert("conns", QJsonArray());
    { auto& ch = sc.item(gid)->children; ch.erase(std::remove(ch.begin(), ch.end(), id), ch.end()); ch.insert(ch.begin(), id); }
    updateConnectors(sc, cc, id, center);
    made.push_back(id);
  }
  sc.reindex(); sel.items = made; sel.anchors.clear();
  return made.size() == 1 ? QObject::tr("Braid made on %1 — its settings in the Braid panel; move a strand's end ring and the tube follows").arg(sc.item(cs[0])->name)
                          : QObject::tr("%1 braids made").arg(made.size());
}

QString makeMesh(Scene& sc, const CurveCache& cc, Selection& sel, int id, const Vec3& center) {
  const Item* b = sc.item(id); if (!b || b->type != ItemType::Braid) return QObject::tr("!Select a braid");
  PolyMesh m = tubesMesh(sc, cc, *b, center);
  if (m.faceCount() <= 0) return QObject::tr("!The braid's curve is gone — nothing to build");
  tedit::makeManifold(m);                                        // (no edge of three polygons, no parts touching at a vertex only)
  const QString name = b->name + QObject::tr(" · tubes"); const int parent = b->parent; const int faces = m.faceCount();
  auto B = std::make_shared<PolyMesh>(std::move(m));
  Item& it = sc.addItem(ItemType::IMesh, parent, name); it.raw = QJsonObject{{"src", QJsonObject{{"ext", "pmesh"}, {"live", true}}}}; it.base = B; it.poly = B;
  placeAfter(sc, it.id, id); sel.items = {it.id}; sel.anchors.clear();
  return QObject::tr("Separate tubes: %1 polygons (%2)").arg(faces).arg(name);
}

QString toLocks(Scene& sc, const CurveCache& cc, Selection& sel, int id, const Vec3& center) {
  const Item* b = sc.item(id); if (!b || b->type != ItemType::Braid) return QObject::tr("!Select a braid");
  const Params P = paramsOf(*b); auto B = build(sc, cc, *b, center, 8); if (!B || B->strands.empty()) return QObject::tr("!The braid's curve is gone — nothing to convert");
  const auto prof = profilesOf(*b, int(B->strands.size())); const QString bname = b->name.section(" (", 0, 0);
  const int top = b->parent; const Item* tg = sc.item(top); const int gp = tg && tg->raw.value("braidGroup").toBool() ? tg->parent : top;
  // (the new group takes the conversion settings of a hair group that has them)
  QJsonValue conv; for (auto& [gid, g] : sc.items) if (g.type == ItemType::Group && g.raw.value("conv").isObject() && !g.raw.value("guidesGroup").toBool() && !g.raw.value("meshesGroup").toBool() && !g.raw.value("braidGroup").toBool()) { conv = g.raw.value("conv"); break; }
  edit::nextNumber(sc, ItemType::Group);
  Item& g = sc.addItem(ItemType::Group, gp, QObject::tr("%1 locks").arg(bname)); g.expanded = true; const int gid = g.id;
  QJsonObject cv = conv.toObject(); cv.insert("step", projectStep(sc));
  g.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "auto"}, {"conv", cv}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false},
                      {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}};
  if (top >= 0) placeAfter(sc, gid, top);
  const int K = std::max(1, int(std::lround(P.cross))), nA = std::clamp(K * std::max(1, P.lockAnchors) + 1, 4, 400);
  std::vector<int> curves;
  for (size_t i = 0; i < B->strands.size(); i++) {
    const auto& S = B->strands[i]; const int NR = int(S.r.size()); if (S.mid.size() < 2) continue;
    auto rAt = [&](double t) { return S.r[size_t(std::clamp(int(std::lround(t * (NR - 1))), 0, NR - 1))]; };
    const double rmid = rAt(0.5), tipLen = std::clamp(P.conv * 1.5, 0.02, 0.6), lr = std::max(0.05, P.lockR);
    const bool collapse = P.lockTip && P.end1 == "point";
    const double r0 = std::max(P.end0 == "point" ? rAt(tipLen) : rAt(0), rmid * 0.05) * lr, r2 = std::max(collapse || P.end1 == "point" ? rAt(1 - tipLen) : rAt(1), rmid * 0.05) * lr;
    Item& c = edit::makeUserCurve(sc, gid); const int cid = c.id; c.name = QObject::tr("%1 strand %2").arg(bname).arg(i + 1);
    std::vector<int> as; for (const Vec3& p : resample(S.mid, nA)) as.push_back(sc.addAnchor(p).id); sc.item(cid)->anchors = as;
    const int num = edit::nextNumber(sc, ItemType::Tube);
    Item& t = sc.addItem(ItemType::Tube, gid, QString("Modifier %1 (%2)").arg(num).arg(sc.item(cid)->name));
    t.raw = QJsonObject{{"src", cid}, {"profile", tube::profileJson(prof[i])},
                        {"p", QJsonObject{{"r0", r0}, {"r1", rmid * lr}, {"r2", r2}, {"rRef", rmid * lr}, {"rot0", P.rot}, {"rot1", P.rot}, {"rot2", P.rot}, {"collapse", collapse}, {"tipLen", tipLen}}}};
    curves.push_back(cid);
  }
  if (tg && tg->raw.value("braidGroup").toBool()) sc.item(top)->visible = false; else sc.item(id)->visible = false;
  sc.reindex(); sel.items = curves; sel.anchors.clear();
  return QObject::tr("%1 locks made (a curve with a Tube modifier each, in %2) — set the modifiers up, then Convert them as usual").arg(curves.size()).arg(sc.item(gid)->name);
}

std::vector<int> syncConnectors(Scene& sc, const CurveCache& cc, const Vec3& center) {
  std::vector<int> moved;
  for (int id : braids(sc)) {
    Item* b = sc.item(id); const QJsonArray conns = b->raw.value("conns").toArray(); if (conns.isEmpty()) continue;
    auto c = context(sc, cc, *b, center, 200); if (!c) continue;
    QJsonArray fr = b->raw.value("frames").toArray(); bool changed = false;
    for (int e = 0; e < 2; e++) {
      const Frame f = c->frameAt(e); const QJsonArray o = fr.size() == 2 ? fr[e].toArray() : QJsonArray();
      const QJsonArray now{f.p.x, f.p.y, f.p.z, f.N.x, f.N.y, f.N.z, f.B.x, f.B.y, f.B.z, f.T.x, f.T.y, f.T.z};
      if (o.size() != 12) { changed = true; continue; }
      double diff = 0; for (int k = 0; k < 12; k++) diff = std::max(diff, std::abs(o[k].toDouble() - now[k].toDouble()) * (k < 3 ? 1 / std::max(c->width, 1e-9) : 1));
      if (diff < 1e-7) continue;
      changed = true;
      const Vec3 p0(o[0].toDouble(), o[1].toDouble(), o[2].toDouble()), N0(o[3].toDouble(), o[4].toDouble(), o[5].toDouble()), B0(o[6].toDouble(), o[7].toDouble(), o[8].toDouble()), T0(o[9].toDouble(), o[10].toDouble(), o[11].toDouble());
      for (const QJsonValue& x : conns) { const Item* k = sc.item(x.toInt(-1)); if (!k || k->raw.value("end").toInt(-1) != e) continue;
        for (int a : k->anchors) { Anchor* A = sc.anchor(a); if (!A) continue; const Vec3 d = A->pos - p0; const double u = d.dot(B0), v = d.dot(N0), z = d.dot(T0);
          A->pos = f.p + f.B * u + f.N * v + f.T * z; moved.push_back(a); } }
    }
    if (changed) { QJsonArray nf; for (int e = 0; e < 2; e++) { const Frame f = c->frameAt(e); nf.append(QJsonArray{f.p.x, f.p.y, f.p.z, f.N.x, f.N.y, f.N.z, f.B.x, f.B.y, f.B.z, f.T.x, f.T.y, f.T.z}); } b->raw.insert("frames", nf); }
  }
  return moved;
}

// the ends' connectors as the end kinds want them: Shared — a ring on the gathered strands' rim; Separate — a ring round
// each strand's end (its cut's outline); Point — none. Made when missing, removed when not wanted; otherwise reshaped:
// each keeps where it was moved to and how far off its outline its anchors were, on the new outline
QString updateConnectors(Scene& sc, const CurveCache& cc, int id, const Vec3& center, bool reset) {
  Item* b = sc.item(id); if (!b || b->type != ItemType::Braid) return "!";
  b->raw.insert("connVer", 3);   // (looked at by this version — even if there is nothing to build)
  auto c = context(sc, cc, *b, center, 200); if (!c) return "!";
  sc.reindex();
  auto kill = [&](int kid) { std::vector<int> s, a; Network net(sc, cc, s, a); net.deleteItem(kid); net.pruneAnchors(); };
  std::map<std::pair<int, int>, int> have; std::vector<int> drop;
  for (const QJsonValue& x : b->raw.value("conns").toArray()) {
    const Item* k = sc.item(x.toInt(-1)); if (!k || k->type != ItemType::Curve || k->raw.value("braidOf").toInt(-1) != id) continue;
    const std::pair<int, int> key{k->raw.value("end").toInt(-1), k->raw.value("strand").toInt(-1)};
    if ((key.first != 0 && key.first != 1) || have.count(key)) drop.push_back(k->id); else have[key] = k->id; }
  std::vector<std::pair<int, int>> want;
  for (int e = 0; e < 2; e++) { const QString kind = e ? c->P.end1 : c->P.end0; if (kind == "shared") want.push_back({e, -1}); else if (kind == "separate") for (int i = 0; i < c->N; i++) want.push_back({e, i}); }
  for (auto& [key, kid] : have) if (std::find(want.begin(), want.end(), key) == want.end()) drop.push_back(kid);
  for (int kid : drop) if (sc.item(kid)) kill(kid);
  QJsonArray conns;
  for (auto [e, slot] : want) {
    const Frame f = c->frameAt(e); std::array<double, 2> rc{0, 0}; std::vector<double> G;
    if (slot < 0) G = c->ringOutline(e); else { const auto q = c->local(slot, e); rc = {q[0], q[1]}; G = c->strandOutline(e, slot); }
    auto at = [&](double x, double y, double ax) { return f.p + f.B * x + f.N * y + f.T * ax; };
    auto local2 = [&](const Vec3& p) { const Vec3 d = p - f.p; return std::array<double, 3>{d.dot(f.B), d.dot(f.N), d.dot(f.T)}; };
    auto hk = have.find({e, slot}); int kid = hk != have.end() && sc.item(hk->second) ? hk->second : -1;
    std::array<double, 2> sh{0, 0};                      // (a strand's ring moved: how far off its rest place — kept)
    if (kid >= 0) {
      Item* k = sc.item(kid); std::vector<double> G0 = arr(*k, "outline"); if (G0.empty()) G0 = G; const auto rc0 = rcOf(*k, rc), c00 = rcOf(*k, rc0, "c0");
      double cx = 0, cy = 0; int n = 0; for (const Vec3& p : sampleCurveFull(sc, *k).p) { const auto l = local2(p); cx += l[0]; cy += l[1]; n++; }   // (the curve's middle — as the end reads it)
      if (n && slot >= 0 && !reset) sh = {cx / n - c00[0], cy / n - c00[1]};
      const double ox = slot < 0 ? 0 : rc0[0] + sh[0], oy = slot < 0 ? 0 : rc0[1] + sh[1];
      for (int a : k->anchors) { Anchor* A = sc.anchor(a); if (!A) continue; const auto l = local2(A->pos); const double u = l[0] - ox, v = l[1] - oy, th = std::atan2(v, u), r = std::hypot(u, v);
        const double g0 = Ctx::polar(G0, th), rn = (g0 > 1e-12 && !reset ? r / g0 : 1) * Ctx::polar(G, th); A->pos = at(rc[0] + sh[0] + rn * std::cos(th), rc[1] + sh[1] + rn * std::sin(th), reset ? 0 : l[2]); }
    } else {
      b = sc.item(id); const QString base = b->name.section(" (", 0, 0), en = e ? QObject::tr("end") : QObject::tr("start");
      Item& nk = sc.addItem(ItemType::Curve, b->parent, slot < 0 ? QString("%1 %2").arg(base, en) : QString("%1 %2 %3").arg(base, en).arg(slot + 1)); kid = nk.id;
      std::vector<int> as; for (int m = 0; m < 8; m++) { const double th = 2 * PI * m / 8, r = Ctx::polar(G, th); as.push_back(sc.addAnchor(at(rc[0] + r * std::cos(th), rc[1] + r * std::sin(th), 0)).id); }
      Item* x = sc.item(kid); x->anchors = as; x->closed = true; x->conn = "braid"; x->raw.insert("closed", true); x->raw.insert("conn", "braid"); x->raw.insert("braidOf", id); x->raw.insert("end", e); x->raw.insert("strand", slot);
    }
    // (its rest shape: its anchors put back on the outline round the end's own middle — the curve through them is what
    // the end is shaped against; their middle there, c0, is where its moves are counted from)
    QJsonArray rest; std::array<double, 2> c0{0, 0};
    { Item* x = sc.item(kid); std::vector<Vec3> keep;
      for (int a : x->anchors) { Anchor* A = sc.anchor(a); keep.push_back(A->pos); const auto l = local2(A->pos); const double th = std::atan2(l[1] - rc[1] - sh[1], l[0] - rc[0] - sh[0]), r = Ctx::polar(G, th);
        A->pos = at(rc[0] + r * std::cos(th), rc[1] + r * std::sin(th), l[2]); }
      const CurveSampling S = sampleCurveFull(sc, *x); std::vector<std::array<double, 2>> poly; for (auto& p : S.p) { const auto l = local2(p); poly.push_back({l[0] - rc[0], l[1] - rc[1]}); c0[0] += l[0]; c0[1] += l[1]; }
      c0[0] /= double(std::max<size_t>(1, S.p.size())); c0[1] /= double(std::max<size_t>(1, S.p.size()));
      std::vector<double> R0; std::vector<int> O; Ctx::rays({poly}, Ctx::NA, 0, R0, O); for (double v : R0) rest.append(v);
      for (size_t i = 0; i < x->anchors.size(); i++) sc.anchor(x->anchors[i])->pos = keep[i]; }
    Item* x = sc.item(kid); QJsonArray g; for (double v : G) g.append(v);
    x->raw.insert("outline", g); x->raw.insert("rest", rest); x->raw.insert("rc", QJsonArray{rc[0], rc[1]}); x->raw.insert("c0", QJsonArray{c0[0], c0[1]});
    conns.append(kid);
  }
  b = sc.item(id); b->raw.insert("conns", conns); b->raw.insert("connVer", 3);
  for (const char* k : {"outline0", "outline1", "rest0", "rest1", "connKind0", "connKind1", "connN0", "connN1"}) b->raw.remove(k);   // (the earlier version's)
  QJsonArray fr; for (int e = 0; e < 2; e++) { const Frame f = c->frameAt(e); fr.append(QJsonArray{f.p.x, f.p.y, f.p.z, f.N.x, f.N.y, f.N.z, f.B.x, f.B.y, f.B.z, f.T.x, f.T.y, f.T.z}); }
  b->raw.insert("frames", fr); sc.reindex();
  return QString();
}

// (tests) how well each end follows its connectors: the end's outline against the connector's, round it
QString endReport(const Scene& sc, const CurveCache& cc, const Item& b, const Vec3& center) {
  auto c = context(sc, cc, b, center, 200); if (!c) return "no braid";
  QString out;
  for (int e = 0; e < 2; e++) {
    const auto& E = c->ends[e];
    if (E.kind == "shared") {
      if (!E.on) { out += QString(" end%1 shared: no connector;").arg(e); continue; }
      std::vector<double> R; std::vector<int> O; Ctx::rays(c->cuts(e, true), Ctx::NA, 0, R, O);
      double dmax = 0, dmean = 0; for (int a = 0; a < Ctx::NA; a++) { const double d = std::abs(R[size_t(a)] - E.C[size_t(a)]); dmax = std::max(dmax, d); dmean += d; }
      out += QString(" end%1 shared: outline vs connector — mean %2%, max %3% of the width;").arg(e).arg(dmean / Ctx::NA / c->width * 100, 0, 'f', 1).arg(dmax / c->width * 100, 0, 'f', 1);
    } else if (E.kind == "separate") {
      const auto polys = c->cuts(e, true); int on = 0; double worst = 0, shift = 0;
      for (int i = 0; i < c->N; i++) { const auto& S = E.st[size_t(i)]; if (!S.on) continue; on++;
        const auto q = c->local(i, e); std::vector<std::array<double, 2>> poly; for (auto& p : polys[size_t(i)]) poly.push_back({p[0] - q[0] - S.shift[0], p[1] - q[1] - S.shift[1]});
        std::vector<double> R; std::vector<int> O; Ctx::rays({poly}, Ctx::NA, 0, R, O);
        for (int a = 0; a < Ctx::NA; a++) worst = std::max(worst, std::abs(R[size_t(a)] - S.C[size_t(a)]));
        shift = std::max(shift, std::hypot(S.shift[0], S.shift[1])); }
      out += QString(" end%1 separate: %2/%3 connectors, max outline miss %4%, max move %5% of the width;").arg(e).arg(on).arg(c->N).arg(worst / c->width * 100, 0, 'f', 1).arg(shift / c->width * 100, 0, 'f', 1);
    } else out += QString(" end%1 point;").arg(e);
  }
  return out;
}

}  // namespace hm::braid
