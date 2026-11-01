// Lobes: the layers' code, the crevices, the fill, the pieces, the quad mesh made of the layers (see Lobes.h)
#include "scene/Lobes.h"
#include <QByteArray>
#include <QObject>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <numeric>
#include <set>

namespace hm::lobes {
using topo::TTopo;

namespace {
// each polygon next to f (across its edges; −1 — the mesh's rim)
template <class F> void eachNb(const TTopo& T, int f, F fn) {
  for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int e = T.ce[size_t(k)]; fn(T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)], e); }
}
Vec3 newell(const TTopo& T, const std::vector<Vec3>& P, int f) {   // (twice the area, along the normal)
  Vec3 n; const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
  for (int k = s; k < e; k++) { const Vec3& a = P[size_t(T.fv[size_t(k)])]; const Vec3& b = P[size_t(T.fv[size_t(k + 1 < e ? k + 1 : s)])];
    n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
  return n;
}
Vec3 faceMid(const TTopo& T, const std::vector<Vec3>& P, int f) {
  Vec3 c; const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1]; for (int k = s; k < e; k++) c += P[size_t(T.fv[size_t(k)])]; return c * (1.0 / std::max(1, e - s));
}
std::vector<double> cumOf(const std::vector<Vec3>& L) { std::vector<double> c(L.size(), 0.0); for (size_t i = 1; i < L.size(); i++) c[i] = c[i - 1] + L[i].distanceTo(L[i - 1]); return c; }
Vec3 atLen(const std::vector<Vec3>& L, const std::vector<double>& cum, double d) {
  if (L.empty()) return {}; if (L.size() == 1) return L[0];
  d = std::clamp(d, 0.0, cum.back()); size_t i = size_t(std::upper_bound(cum.begin(), cum.end(), d) - cum.begin()); i = std::clamp<size_t>(i, 1, L.size() - 1);
  const double seg = cum[i] - cum[i - 1]; return L[i - 1] + (L[i] - L[i - 1]) * (seg > 0 ? (d - cum[i - 1]) / seg : 0.0);
}
Vec3 atFrac(const std::vector<Vec3>& L, double f) { const auto c = cumOf(L); return atLen(L, c, f * (c.empty() ? 0 : c.back())); }
// the two main directions of a spread of points (its plane)
void plane(const std::vector<Vec3>& pts, Vec3& c, Vec3& e1, Vec3& e2) {
  c = {}; for (auto& p : pts) c += p; c = c * (1.0 / std::max<size_t>(1, pts.size()));
  double C[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  for (auto& p : pts) { const Vec3 d = p - c; const double v[3] = {d.x, d.y, d.z}; for (int r = 0; r < 3; r++) for (int q = 0; q < 3; q++) C[r][q] += v[r] * v[q]; }
  auto mul = [&](const Vec3& v) { return Vec3(C[0][0] * v.x + C[0][1] * v.y + C[0][2] * v.z, C[1][0] * v.x + C[1][1] * v.y + C[1][2] * v.z, C[2][0] * v.x + C[2][1] * v.y + C[2][2] * v.z); };
  e1 = Vec3(0.577, 0.577, 0.577); for (int it = 0; it < 50; it++) { const Vec3 n = mul(e1); if (n.length() < 1e-30) break; e1 = n.normalized(); }
  e2 = std::abs(e1.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0); e2 = (e2 - e1 * e2.dot(e1)).normalized();
  for (int it = 0; it < 50; it++) { Vec3 n = mul(e2); n = n - e1 * n.dot(e1); if (n.length() < 1e-30) break; e2 = n.normalized(); }
}
}  // namespace

// ---------- the layers' code: 16 bits a polygon, compressed ----------
QString enc(const std::vector<int>& lab) {
  if (std::all_of(lab.begin(), lab.end(), [](int x) { return x == 0; })) return {};
  QByteArray b(int(lab.size()) * 2, 0);
  for (size_t i = 0; i < lab.size(); i++) { const int v = std::clamp(lab[i], 0, 65535); b[int(i * 2)] = char(v & 255); b[int(i * 2 + 1)] = char(v >> 8); }
  return QString::fromLatin1(qCompress(b, 6).toBase64());
}
std::vector<int> dec(const QString& s, int n) {
  std::vector<int> o(size_t(std::max(0, n)), 0); if (s.isEmpty() || n <= 0) return o;
  const QByteArray b = qUncompress(QByteArray::fromBase64(s.toLatin1())); if (b.size() != n * 2) return o;
  for (int i = 0; i < n; i++) o[size_t(i)] = int(uchar(b[i * 2])) | (int(uchar(b[i * 2 + 1])) << 8);
  return o;
}

// ---------- the crevices ----------
// a vertex's hollowness: its neighbours above its tangent plane (the mean of 2·n·d / |d|² — the curvature along each
// edge), times the mean edge; smoothed over its rings — the fine grooves of a sculpt averaged out, the crevices kept
std::vector<float> concavity(const TTopo& T, const std::vector<Vec3>& P) {
  const int n = T.n; std::vector<Vec3> N(static_cast<size_t>(n));
  for (int f = 0; f < T.nf; f++) { const Vec3 fn = newell(T, P, f); for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) N[size_t(T.fv[size_t(k)])] += fn; }
  for (auto& x : N) x = x.normalized();
  double el = 0; for (int e = 0; e < T.ne; e++) el += P[size_t(T.ea[size_t(e)])].distanceTo(P[size_t(T.eb[size_t(e)])]); el /= std::max(1, T.ne);
  std::vector<double> k(size_t(n), 0.0), k2(size_t(n), 0.0);
  for (int v = 0; v < n; v++) { double s = 0; int c = 0;
    for (int i = T.voff[size_t(v)]; i < T.voff[size_t(v) + 1]; i++) { const int u = T.other(T.vE[size_t(i)], v); const Vec3 d = P[size_t(u)] - P[size_t(v)]; const double L2 = d.lengthSq(); if (L2 > 0) { s += 2 * N[size_t(v)].dot(d) / L2; c++; } }
    k[size_t(v)] = c ? s / c * el : 0; }
  const int rounds = 10;
  for (int r = 0; r < rounds; r++) {
    for (int v = 0; v < n; v++) { double s = k[size_t(v)]; int c = 1; for (int i = T.voff[size_t(v)]; i < T.voff[size_t(v) + 1]; i++) { s += k[size_t(T.other(T.vE[size_t(i)], v))]; c++; } k2[size_t(v)] = s / c; }
    k.swap(k2); }
  std::vector<float> out(size_t(T.nf), 0.f);
  for (int f = 0; f < T.nf; f++) { double s = 0; const int a = T.fo[size_t(f)], b = T.fo[size_t(f) + 1]; for (int q = a; q < b; q++) s += k[size_t(T.fv[size_t(q)])]; out[size_t(f)] = float(s / std::max(1, b - a)); }
  return out;
}
float quantile(std::vector<float> v, double q) {
  if (v.empty()) return 0; const size_t i = size_t(std::clamp(q, 0.0, 1.0) * double(v.size() - 1)); std::nth_element(v.begin(), v.begin() + long(i), v.end()); return v[i];
}

std::vector<char> crevices(const TTopo& T, const std::vector<Vec3>& P, const std::vector<float>& conc, double foldDeg, float concThr) {
  std::vector<char> cr(size_t(T.nf), 0); std::vector<Vec3> N(size_t(T.nf)); for (int f = 0; f < T.nf; f++) N[size_t(f)] = newell(T, P, f).normalized();
  const double c = std::cos(foldDeg * M_PI / 180);
  for (int e = 0; e < T.ne; e++) { const int a = T.ef0[size_t(e)], b = T.ef1[size_t(e)]; if (b >= 0 && N[size_t(a)].dot(N[size_t(b)]) < c) cr[size_t(a)] = cr[size_t(b)] = 1; }
  for (int f = 0; f < T.nf && size_t(f) < conc.size(); f++) if (conc[size_t(f)] >= concThr) cr[size_t(f)] = 1;
  return cr;
}
std::vector<int> fill(const TTopo& T, const std::vector<float>& conc, std::vector<char> cr, const std::vector<int>& lab, int f0, int layer, bool overwrite, int close, size_t maxFaces) {
  std::vector<int> out; if (f0 < 0 || f0 >= T.nf) return out;
  auto free = [&](int f) { return overwrite || lab[size_t(f)] == 0 || lab[size_t(f)] == layer; };
  const std::vector<char> cr0 = cr;
  for (int r = 0; r < close; r++) { std::vector<int> add;   // (the crevices widened: the gaps in them shut)
    for (int f = 0; f < T.nf; f++) if (!cr[size_t(f)]) { bool nb = false; eachNb(T, f, [&](int g, int) { if (g >= 0 && cr[size_t(g)]) nb = true; }); if (nb) add.push_back(f); }
    for (int f : add) cr[size_t(f)] = 1; }
  std::vector<int> depth(size_t(T.nf), -1); std::deque<int> q{f0}; depth[size_t(f0)] = 0;
  while (!q.empty() && out.size() <= maxFaces) { const int f = q.front(); q.pop_front(); out.push_back(f);   // (out of the crevices)
    eachNb(T, f, [&](int g, int) { if (g < 0 || depth[size_t(g)] >= 0 || !free(g) || cr[size_t(g)]) return; depth[size_t(g)] = 0; q.push_back(g); }); }
  if (out.size() > maxFaces) return out;                        // (leaked out: the caller tries a wider closing)
  // (on into the crevice: across the widening and the seam itself, then on up a hollow's slope to its bottom)
  q.assign(out.begin(), out.end());
  while (!q.empty()) { const int f = q.front(); q.pop_front(); const int d = depth[size_t(f)] + 1;
    eachNb(T, f, [&](int g, int) { if (g < 0 || depth[size_t(g)] >= 0 || !free(g) || !cr[size_t(g)]) return;
      if (d > close + 1 && !(cr0[size_t(g)] && conc[size_t(g)] >= conc[size_t(f)])) return;
      depth[size_t(g)] = d; out.push_back(g); q.push_back(g); }); }
  return out;
}

// ---------- each lobe laid onto a square ----------
std::vector<LobeMap> maps(const TTopo& T, const std::vector<Vec3>& P, const std::vector<int>& lab) {
  std::vector<LobeMap> out; const int nf = T.nf; if (int(lab.size()) != nf) return out;
  int K = 0; for (int x : lab) K = std::max(K, x);
  std::vector<int> comp(size_t(nf), -1);
  for (int l = 1; l <= K; l++) {
    // (its largest piece)
    std::vector<int> best; for (int f0 = 0; f0 < nf; f0++) { if (lab[size_t(f0)] != l || comp[size_t(f0)] >= 0) continue; std::vector<int> fs; std::deque<int> q{f0}; comp[size_t(f0)] = f0;
      while (!q.empty()) { const int f = q.front(); q.pop_front(); fs.push_back(f); eachNb(T, f, [&](int g, int) { if (g >= 0 && lab[size_t(g)] == l && comp[size_t(g)] < 0) { comp[size_t(g)] = f0; q.push_back(g); } }); }
      if (fs.size() > best.size()) best.swap(fs); }
    if (best.size() < 8) continue;
    LobeMap M; M.id = l; std::set<int> in(best.begin(), best.end());
    std::map<int, int> vmap; auto lv = [&](int v) { auto it = vmap.find(v); if (it != vmap.end()) return it->second; M.P.push_back(P[size_t(v)]); return vmap[v] = int(M.P.size()) - 1; };
    std::multimap<int, int> nxt;                                  // (its outline: each border edge as its polygon goes round — start → end)
    for (int f : best) { const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
      for (int k = s + 1; k + 1 < e; k++) M.tri.push_back({lv(T.fv[size_t(s)]), lv(T.fv[size_t(k)]), lv(T.fv[size_t(k + 1)])});
      for (int k = s; k < e; k++) { const int ed = T.ce[size_t(k)]; const int g = T.ef0[size_t(ed)] == f ? T.ef1[size_t(ed)] : T.ef0[size_t(ed)];
        if (g < 0 || !in.count(g)) nxt.insert({T.fv[size_t(k)], T.fv[size_t(k + 1 < e ? k + 1 : s)]}); } }
    // (the longest loop of it)
    std::vector<int> B; double bestLen = -1; std::set<std::pair<int, int>> used;
    for (auto& [a0, b0] : nxt) { if (used.count({a0, b0})) continue; std::vector<int> L{a0}; int cur = b0; used.insert({a0, b0}); double len = P[size_t(a0)].distanceTo(P[size_t(b0)]);
      for (int guard = 0; cur != a0 && guard < int(nxt.size()) + 2; guard++) { L.push_back(cur); int nx = -1;
        for (auto [it, end] = nxt.equal_range(cur); it != end; ++it) if (!used.count({cur, it->second})) { nx = it->second; break; }
        if (nx < 0) break; used.insert({cur, nx}); len += P[size_t(cur)].distanceTo(P[size_t(nx)]); cur = nx; }
      if (cur == a0 && len > bestLen) { bestLen = len; B = L; } }
    const int nB = int(B.size()); if (nB < 8) continue;
    std::vector<double> cb(size_t(nB) + 1, 0.0); for (int i = 0; i < nB; i++) cb[size_t(i) + 1] = cb[size_t(i)] + P[size_t(B[size_t(i)])].distanceTo(P[size_t(B[size_t((i + 1) % nB)])]);
    const double tot = cb.back();
    // (its four corners: two sides along the lobe — its long axis — and two ends across it)
    Vec3 axis; { Vec3 cc, e2; plane(M.P, cc, axis, e2); }
    const int NS = std::min(48, nB); std::vector<int> idx(static_cast<size_t>(NS)); std::vector<double> A(static_cast<size_t>(NS)), W(static_cast<size_t>(NS));
    for (int k = 0; k < NS; k++) { const double d = tot * k / NS; const int i = int(std::upper_bound(cb.begin(), cb.end(), d) - cb.begin()) - 1; idx[size_t(k)] = std::clamp(i, 0, nB - 1); }
    for (int k = 0; k < NS; k++) { const int i = idx[size_t(k)], sp = std::max(1, nB / 64); const Vec3 t = (P[size_t(B[size_t((i + sp) % nB)])] - P[size_t(B[size_t((i - sp + nB) % nB)])]).normalized();
      A[size_t(k)] = std::abs(t.dot(axis));
      const int j = idx[size_t((k + 1) % NS)]; W[size_t(k)] = k + 1 < NS ? cb[size_t(j)] - cb[size_t(i)] : tot - cb[size_t(i)] + cb[size_t(j)]; }
    std::vector<double> SA(size_t(NS) + 1, 0.0), SE(size_t(NS) + 1, 0.0);
    for (int k = 0; k < NS; k++) { SA[size_t(k) + 1] = SA[size_t(k)] + A[size_t(k)] * W[size_t(k)]; SE[size_t(k) + 1] = SE[size_t(k)] + (1 - A[size_t(k)]) * W[size_t(k)]; }
    auto sum = [&](const std::vector<double>& Sx, int a, int b) { return a <= b ? Sx[size_t(b)] - Sx[size_t(a)] : Sx[size_t(NS)] - Sx[size_t(a)] + Sx[size_t(b)]; };
    double bs = -1e300; std::array<int, 4> bc{}; bool alongFirst = true;
    for (int i = 0; i < NS; i++) for (int j = i + 1; j < NS; j++) for (int k = j + 1; k < NS; k++) for (int m = k + 1; m < NS; m++) {
      const double s1 = sum(SA, i, j) + sum(SE, j, k) + sum(SA, k, m) + sum(SE, m, i), s2 = sum(SE, i, j) + sum(SA, j, k) + sum(SE, k, m) + sum(SA, m, i);
      if (s1 > bs) { bs = s1; bc = {i, j, k, m}; alongFirst = true; } if (s2 > bs) { bs = s2; bc = {i, j, k, m}; alongFirst = false; } }
    if (!alongFirst) bc = {bc[1], bc[2], bc[3], bc[0]};
    std::array<int, 4> ci{}; for (int q = 0; q < 4; q++) ci[size_t(q)] = idx[size_t(bc[size_t(q)])];
    // (the same way round on every lobe: u = 0 at the higher end — the braid's root)
    auto endY = [&](int a, int b) { double s = 0; int n = 0; for (int i = a; i != b; i = (i + 1) % nB) { s += P[size_t(B[size_t(i)])].y; n++; } return n ? s / n : 0.0; };
    if (endY(ci[1], ci[2]) > endY(ci[3], ci[0])) ci = {ci[2], ci[3], ci[0], ci[1]};
    // (the outline on the square's sides by length; Tutte: each inner vertex the mean of its neighbours)
    const int nv = int(M.P.size()); M.U.assign(size_t(nv), 0.5); M.V.assign(size_t(nv), 0.5); std::vector<char> fixedV(size_t(nv), 0);
    for (int q = 0; q < 4; q++) { const int a = ci[size_t(q)], b = ci[size_t((q + 1) % 4)]; std::vector<std::pair<int, double>> vs; double arc = 0;
      for (int i = a;; i = (i + 1) % nB) { if (i != a) arc += P[size_t(B[size_t((i - 1 + nB) % nB)])].distanceTo(P[size_t(B[size_t(i)])]); vs.push_back({B[size_t(i)], arc}); if (i == b) break; }
      for (auto [v, s] : vs) { const double f = arc > 0 ? s / arc : 0; const int x = vmap[v]; fixedV[size_t(x)] = 1;
        M.U[size_t(x)] = q == 0 ? f : q == 1 ? 1 : q == 2 ? 1 - f : 0; M.V[size_t(x)] = q == 0 ? 0 : q == 1 ? f : q == 2 ? 1 : 1 - f; } }
    // (each edge weighted by the cotangents of the angles facing it — nearly conformal: a long lobe not squeezed in its
    // middle; kept above a floor, so it stays a mean of the neighbours and nothing folds)
    std::vector<std::map<int, double>> wm(static_cast<size_t>(nv));
    for (auto& t : M.tri) for (int k = 0; k < 3; k++) { const int a = t[size_t(k)], b = t[size_t((k + 1) % 3)], c = t[size_t((k + 2) % 3)];
      const Vec3 ca = M.P[size_t(a)] - M.P[size_t(c)], cb = M.P[size_t(b)] - M.P[size_t(c)]; const double cr = ca.cross(cb).length(); const double ct = cr > 1e-20 ? ca.dot(cb) / cr : 0.0;
      wm[size_t(a)][b] += 0.5 * ct; wm[size_t(b)][a] += 0.5 * ct; }
    std::vector<std::vector<std::pair<int, double>>> nb(static_cast<size_t>(nv)); std::vector<double> wsum(size_t(nv), 0.0);
    for (int v = 0; v < nv; v++) for (auto [u, w] : wm[size_t(v)]) { const double ww = std::max(w, 0.05); nb[size_t(v)].push_back({u, ww}); wsum[size_t(v)] += ww; }
    std::vector<int> inn, at(size_t(nv), -1); for (int v = 0; v < nv; v++) if (!fixedV[size_t(v)] && !nb[size_t(v)].empty()) { at[size_t(v)] = int(inn.size()); inn.push_back(v); }
    auto solve = [&](std::vector<double>& X) {
      const size_t n = inn.size(); if (!n) return; std::vector<double> x(n), b(n, 0.0), r(n), p(n), Ap(n);
      for (size_t i = 0; i < n; i++) { x[i] = X[size_t(inn[i])]; for (auto [u, w] : nb[size_t(inn[i])]) if (at[size_t(u)] < 0) b[i] += w * X[size_t(u)]; }
      auto mul = [&](const std::vector<double>& y, std::vector<double>& o2) { for (size_t i = 0; i < n; i++) { double s = wsum[size_t(inn[i])] * y[i]; for (auto [u, w] : nb[size_t(inn[i])]) if (at[size_t(u)] >= 0) s -= w * y[size_t(at[size_t(u)])]; o2[i] = s; } };
      mul(x, Ap); double rr = 0; for (size_t i = 0; i < n; i++) { r[i] = b[i] - Ap[i]; p[i] = r[i]; rr += r[i] * r[i]; }
      for (size_t it = 0; it < 4 * n + 50 && rr > 1e-20; it++) { mul(p, Ap); double pAp = 0; for (size_t i = 0; i < n; i++) pAp += p[i] * Ap[i]; if (pAp <= 0) break;
        const double a = rr / pAp; double r2 = 0; for (size_t i = 0; i < n; i++) { x[i] += a * p[i]; r[i] -= a * Ap[i]; r2 += r[i] * r[i]; }
        const double bta = r2 / rr; rr = r2; for (size_t i = 0; i < n; i++) p[i] = r[i] + bta * p[i]; }
      for (size_t i = 0; i < n; i++) X[size_t(inn[i])] = x[i]; };
    solve(M.U); solve(M.V);
    // (finding things: its triangles in 3D, and on the square by cells)
    std::vector<uint32_t> idx3; for (auto& t : M.tri) for (int k = 0; k < 3; k++) idx3.push_back(uint32_t(t[size_t(k)]));
    M.bvh = std::make_shared<topo::Bvh>(); M.bvh->build(M.P, idx3);
    M.bins.assign(size_t(M.GB * M.GB), {});
    auto binOf = [&](double x) { return std::clamp(int(x * M.GB), 0, M.GB - 1); };
    for (size_t t = 0; t < M.tri.size(); t++) { double u0 = 1e9, u1 = -1e9, v0 = 1e9, v1 = -1e9; for (int k = 0; k < 3; k++) { const int v = M.tri[t][size_t(k)]; u0 = std::min(u0, M.U[size_t(v)]); u1 = std::max(u1, M.U[size_t(v)]); v0 = std::min(v0, M.V[size_t(v)]); v1 = std::max(v1, M.V[size_t(v)]); }
      for (int i = binOf(u0); i <= binOf(u1); i++) for (int j = binOf(v0); j <= binOf(v1); j++) M.bins[size_t(j * M.GB + i)].push_back(int(t)); }
    M.ok = true; out.push_back(std::move(M)); }
  return out;
}

// a point near the lobe → its place on the square (the nearest point of the lobe, the same blend of its triangle)
bool toUV(const LobeMap& M, const Vec3& p, double& u, double& v, double* dist) {
  if (!M.ok || !M.bvh) return false; double d = 0; const auto q = M.bvh->closest(p, nullptr, &d); if (!q) return false; if (dist) *dist = d;
  double bd = 1e300; std::array<double, 3> bw{1, 0, 0}; int bt = -1; const double r = std::max(1e-9, d * 1e-3 + 1e-6);
  M.bvh->sphere(*q, r, [&](int t) { const auto& tr = M.tri[size_t(t)]; const Vec3 &a = M.P[size_t(tr[0])], &b = M.P[size_t(tr[1])], &c = M.P[size_t(tr[2])];
    const Vec3 e0 = b - a, e1 = c - a, w = *q - a; const double d00 = e0.dot(e0), d01 = e0.dot(e1), d11 = e1.dot(e1), d20 = w.dot(e0), d21 = w.dot(e1), den = d00 * d11 - d01 * d01; if (std::abs(den) < 1e-300) return;
    double l1 = (d11 * d20 - d01 * d21) / den, l2 = (d00 * d21 - d01 * d20) / den, l0 = 1 - l1 - l2;
    const double out2 = std::max(0.0, -l0) + std::max(0.0, -l1) + std::max(0.0, -l2); if (out2 >= bd) return;
    bd = out2; l0 = std::max(0.0, l0); l1 = std::max(0.0, l1); l2 = std::max(0.0, l2); const double s = l0 + l1 + l2; bw = {l0 / s, l1 / s, l2 / s}; bt = t; });
  if (bt < 0) return false; const auto& tr = M.tri[size_t(bt)];
  u = M.U[size_t(tr[0])] * bw[0] + M.U[size_t(tr[1])] * bw[1] + M.U[size_t(tr[2])] * bw[2]; v = M.V[size_t(tr[0])] * bw[0] + M.V[size_t(tr[1])] * bw[1] + M.V[size_t(tr[2])] * bw[2];
  return true;
}
// a place on the square → the lobe (the triangle round it there, the same blend of its corners)
Vec3 fromUV(const LobeMap& M, double u, double v) {
  u = std::clamp(u, 0.0, 1.0); v = std::clamp(v, 0.0, 1.0);
  auto binOf = [&](double x) { return std::clamp(int(x * M.GB), 0, M.GB - 1); };
  double best = -1e300; Vec3 bp;
  for (int ring = 0; ring < M.GB && best < -1e-9; ring++) {
    for (int i = binOf(u) - ring; i <= binOf(u) + ring; i++) for (int j = binOf(v) - ring; j <= binOf(v) + ring; j++) {
      if (i < 0 || j < 0 || i >= M.GB || j >= M.GB || (std::abs(i - binOf(u)) != ring && std::abs(j - binOf(v)) != ring)) continue;
      for (int t : M.bins[size_t(j * M.GB + i)]) { const auto& tr = M.tri[size_t(t)]; const double ax = M.U[size_t(tr[0])], ay = M.V[size_t(tr[0])], bx = M.U[size_t(tr[1])], by = M.V[size_t(tr[1])], cx = M.U[size_t(tr[2])], cy = M.V[size_t(tr[2])];
        const double d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy); if (std::abs(d) < 1e-300) continue;
        double l0 = ((by - cy) * (u - cx) + (cx - bx) * (v - cy)) / d, l1 = ((cy - ay) * (u - cx) + (ax - cx) * (v - cy)) / d, l2 = 1 - l0 - l1;
        const double mn = std::min({l0, l1, l2}); if (mn <= best) continue;
        best = mn; l0 = std::max(0.0, l0); l1 = std::max(0.0, l1); l2 = std::max(0.0, l2); const double s = l0 + l1 + l2;
        bp = (M.P[size_t(tr[0])] * l0 + M.P[size_t(tr[1])] * l1 + M.P[size_t(tr[2])] * l2) * (1.0 / s); } } }
  return bp;
}


// ---------- one lobe laid over another (the lobes of a braid: one shape repeated — turned, moved, a little smaller,
// every other one mirrored) ----------
namespace {
// the turn best taking the points p onto q (both about their middles): Horn's quaternion — the top eigenvector of his
// 4 × 4 matrix (power iterations, shifted to be positive)
void bestTurn(const std::vector<Vec3>& p, const std::vector<Vec3>& q, double R[3][3]) {
  double S[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  for (size_t i = 0; i < p.size(); i++) { const double a[3] = {p[i].x, p[i].y, p[i].z}, b[3] = {q[i].x, q[i].y, q[i].z}; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) S[r][c] += a[r] * b[c]; }
  const double N[4][4] = {{S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0]},
                          {S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2]},
                          {S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1]},
                          {S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2]}};
  double sh = 0; for (auto& r : N) for (double x : r) sh += std::abs(x);
  double v[4] = {1, 0, 0, 0};
  for (int it = 0; it < 200; it++) { double w[4]; for (int r = 0; r < 4; r++) { w[r] = sh * v[r]; for (int c = 0; c < 4; c++) w[r] += N[r][c] * v[c]; }
    double l = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2] + w[3] * w[3]); if (l < 1e-300) break; for (int r = 0; r < 4; r++) v[r] = w[r] / l; }
  const double a = v[0], b = v[1], c = v[2], d = v[3];
  R[0][0] = a * a + b * b - c * c - d * d; R[0][1] = 2 * (b * c - a * d); R[0][2] = 2 * (b * d + a * c);
  R[1][0] = 2 * (b * c + a * d); R[1][1] = a * a - b * b + c * c - d * d; R[1][2] = 2 * (c * d - a * b);
  R[2][0] = 2 * (b * d - a * c); R[2][1] = 2 * (c * d + a * b); R[2][2] = a * a - b * b - c * c + d * d;
}
Vec3 mulR(const double R[3][3], const Vec3& p) { return Vec3(R[0][0] * p.x + R[0][1] * p.y + R[0][2] * p.z, R[1][0] * p.x + R[1][1] * p.y + R[1][2] * p.z, R[2][0] * p.x + R[2][1] * p.y + R[2][2] * p.z); }
}  // namespace

Vec3 Fit::apply(const Vec3& p) const {
  Vec3 x = p; if (mirror) x = x - n * (2 * (x - rc).dot(n));
  return mulR(R, (x - c0) * s) + c1;
}

std::vector<Fit> fits(const LobeMap& A, const LobeMap& B) {
  std::vector<Fit> out; if (!A.ok || !B.ok || !B.bvh || A.P.size() < 8 || B.P.size() < 8) return out;
  const size_t stride = std::max<size_t>(1, A.P.size() / 400); std::vector<Vec3> pa; for (size_t i = 0; i < A.P.size(); i += stride) pa.push_back(A.P[i]);
  Vec3 ca, a1, a2, cb, b1, b2; plane(A.P, ca, a1, a2); plane(B.P, cb, b1, b2); const Vec3 a3 = a1.cross(a2), b3 = b1.cross(b2);
  auto spread = [](const std::vector<Vec3>& P, const Vec3& c) { double s = 0; for (auto& p : P) s += (p - c).lengthSq(); return std::sqrt(s / std::max<size_t>(1, P.size())); };
  const double s0 = spread(B.P, cb) / std::max(1e-12, spread(A.P, ca));
  // (a run of closest points: the pose refined — turn, place, size — from where it starts)
  auto icp = [&](Fit f, int iters) {
    for (int it = 0; it <= iters; it++) {
      std::vector<Vec3> src, dst; double e = 0;
      for (const Vec3& p : pa) { const Vec3 x = f.apply(p); double d = 0; const auto q = B.bvh->closest(x, nullptr, &d); if (!q) continue; Vec3 m = p; if (f.mirror) m = m - f.n * (2 * (m - f.rc).dot(f.n)); src.push_back(m); dst.push_back(*q); e += d * d; }
      f.err = std::sqrt(e / std::max<size_t>(1, src.size())); if (it == iters || src.size() < 4) break;
      Vec3 cs, cd; for (size_t i = 0; i < src.size(); i++) { cs += src[i]; cd += dst[i]; } cs = cs * (1.0 / double(src.size())); cd = cd * (1.0 / double(dst.size()));
      std::vector<Vec3> ps, qs; double ss = 0, sd = 0; for (size_t i = 0; i < src.size(); i++) { ps.push_back(src[i] - cs); qs.push_back(dst[i] - cd); ss += ps.back().lengthSq(); sd += qs.back().lengthSq(); }
      bestTurn(ps, qs, f.R); f.s = std::clamp(std::sqrt(sd / std::max(1e-30, ss)), s0 * 0.7, s0 * 1.4); f.c0 = cs; f.c1 = cd; }
    return f; };
  // (the starts: the main axes laid on each other every way round — turned, and mirrored)
  std::vector<Fit> starts;
  for (int mir = 0; mir < 2; mir++) for (int sx : {1, -1}) for (int sy : {1, -1}) {
    Fit f; f.mirror = mir == 1; f.n = a3; f.rc = ca; f.c0 = ca; f.c1 = cb; f.s = s0;   // (mirrored: across its own middle plane — the axes stay, the shape turns over)
    const Vec3 ta[3] = {a1, a2, a3}; const Vec3 tb[3] = {b1 * double(sx), b2 * double(sy), b3 * double(sx * sy)};
    // (R = Σ tb ⊗ ta: a's axes onto b's — a turn either way, the mirror done by the reflection before it)
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { f.R[r][c] = 0; for (int k = 0; k < 3; k++) f.R[r][c] += tb[k][r] * ta[k][c]; }
    starts.push_back(icp(f, 6)); }
  std::sort(starts.begin(), starts.end(), [](const Fit& x, const Fit& y) { return x.err < y.err; });
  for (size_t i = 0; i < starts.size() && i < 3; i++) out.push_back(icp(starts[i], 25));
  std::sort(out.begin(), out.end(), [](const Fit& x, const Fit& y) { return x.err < y.err; });
  return out;
}
}  // namespace hm::lobes
