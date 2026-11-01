#include "scene/UvSolve.h"
#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>

namespace hm::uv::solve {

namespace {
using SpMat = Eigen::SparseMatrix<double>;
using Trip = Eigen::Triplet<double>;
const double INF = std::numeric_limits<double>::infinity();

// a triangle at rest: its gradients (∂/∂x, ∂/∂y of each corner's hat function, in the triangle's own plane) and its area
// (0: too thin to count)
struct Rest { double gx[3] = {0, 0, 0}, gy[3] = {0, 0, 0}, A = 0; };
std::vector<Rest> rests(const std::vector<Vec3>& X, const std::vector<Tri>& tris) {
  std::vector<Rest> R(tris.size());
  for (size_t t = 0; t < tris.size(); t++) {
    const Vec3 &p0 = X[size_t(tris[t][0])], &p1 = X[size_t(tris[t][1])], &p2 = X[size_t(tris[t][2])];
    const Vec3 e1 = p1 - p0, e2 = p2 - p0; const double l = e1.length(); const Vec3 nn = e1.cross(e2); const double A2 = nn.length();
    if (l < 1e-300 || !(A2 > 1e-12 * (e1.dot(e1) + e2.dot(e2)))) continue;
    const Vec3 x = e1 * (1 / l), y = (nn * (1 / A2)).cross(x);
    const double q[3][2] = {{0, 0}, {l, 0}, {e2.dot(x), e2.dot(y)}}; const double A = A2 / 2;
    for (int j = 0; j < 3; j++) { const double ex = q[(j + 2) % 3][0] - q[(j + 1) % 3][0], ey = q[(j + 2) % 3][1] - q[(j + 1) % 3][1];
      R[t].gx[j] = -ey / (2 * A); R[t].gy[j] = ex / (2 * A); }
    R[t].A = A;
  }
  return R;
}
// the UV's Jacobian on a triangle: [a b; c d] = [∂u/∂x ∂u/∂y; ∂v/∂x ∂v/∂y]
struct Jac { double a = 0, b = 0, c = 0, d = 0; double det() const { return a * d - b * c; } double fro2() const { return a * a + b * b + c * c + d * d; } };
Jac jac(const Rest& r, const Tri& t, const std::vector<P2>& U) {
  Jac J; for (int j = 0; j < 3; j++) { const P2& q = U[size_t(t[size_t(j)])]; J.a += q[0] * r.gx[j]; J.b += q[0] * r.gy[j]; J.c += q[1] * r.gx[j]; J.d += q[1] * r.gy[j]; }
  return J;
}
// the shell's way round in UV: +1 — as in 3D, −1 — mirrored (by the area of the triangles each way)
double orientation(const std::vector<Rest>& R, const std::vector<Tri>& tris, const std::vector<P2>& U) {
  double s = 0; for (size_t t = 0; t < tris.size(); t++) if (R[t].A > 0) s += R[t].A * jac(R[t], tris[t], U).det();
  return s < 0 ? -1 : 1;
}
void mirrorRest(std::vector<Rest>& R) { for (auto& r : R) for (double& g : r.gy) g = -g; }
// symmetric Dirichlet per area (∞ — a triangle turned over or flat)
double energyOf(const std::vector<Rest>& R, const std::vector<Tri>& tris, const std::vector<P2>& U) {
  double e = 0, a = 0;
  for (size_t t = 0; t < tris.size(); t++) { if (!(R[t].A > 0)) continue; const Jac J = jac(R[t], tris[t], U); const double dt = J.det(); if (!(dt > 0)) return INF;
    e += R[t].A * J.fro2() * (1 + 1 / (dt * dt)); a += R[t].A; }
  return a > 0 ? e / a : 4;
}
}  // namespace

// ---------- the unknowns ----------
Dofs::Dofs(int verts) : par_(size_t(verts) * 2), s_(size_t(verts) * 2, 1.0), o_(size_t(verts) * 2, 0.0), held_(size_t(verts) * 2, 0), val_(size_t(verts) * 2, 0.0) {
  std::iota(par_.begin(), par_.end(), 0);
}
Dofs::Root Dofs::find(int d) const {
  if (par_[size_t(d)] == d) return {d, 1, 0};
  thread_local std::vector<int> path; path.clear();
  int r = d; while (par_[size_t(r)] != r) { path.push_back(r); r = par_[size_t(r)]; }
  // from the root down, each straight onto it: x = s·x_p + o, x_p = s_p·x_r + o_p
  for (size_t k = path.size(); k-- > 0;) { const int x = path[k], p = par_[size_t(x)]; if (p == r) continue;
    o_[size_t(x)] = s_[size_t(x)] * o_[size_t(p)] + o_[size_t(x)]; s_[size_t(x)] *= s_[size_t(p)]; par_[size_t(x)] = r; }
  return {r, s_[size_t(d)], o_[size_t(d)]};
}
void Dofs::hold(int d, double value) {
  if (d < 0 || d >= size()) return; const Root r = find(d);
  held_[size_t(r.r)] = 1; val_[size_t(r.r)] = (value - r.o) / r.s;
}
void Dofs::tie(int d, int to, double s, double o) {
  if (d < 0 || to < 0 || d >= size() || to >= size() || s == 0) return;
  const Root a = find(d), b = find(to);                       // a.s·x_ra + a.o = s·(b.s·x_rb + b.o) + o
  if (a.r == b.r) {
    const double k = a.s - s * b.s; if (std::abs(k) < 1e-12) return;   // (the same already, or at odds: left)
    held_[size_t(a.r)] = 1; val_[size_t(a.r)] = (s * b.o + o - a.o) / k; return;
  }
  const double ks = s * b.s / a.s, ko = (s * b.o + o - a.o) / a.s;   // x_ra = ks·x_rb + ko
  if (held_[size_t(a.r)] && !held_[size_t(b.r)]) { held_[size_t(b.r)] = 1; val_[size_t(b.r)] = (val_[size_t(a.r)] - ko) / ks; }
  par_[size_t(a.r)] = b.r; s_[size_t(a.r)] = ks; o_[size_t(a.r)] = ko;
}
bool Dofs::anyHeld() const { for (int d = 0; d < size(); d++) if (held_[size_t(find(d).r)]) return true; return false; }
int Dofs::heldPoints() const { int n = 0; for (int i = 0; i * 2 + 1 < size(); i++) if (held_[size_t(find(2 * i).r)] && held_[size_t(find(2 * i + 1).r)]) n++; return n; }
Dofs::Map Dofs::resolve() const {
  Map M; const int n = size(); M.var.assign(size_t(n), -1); M.s.assign(size_t(n), 0); M.o.assign(size_t(n), 0); M.val.assign(size_t(n), 0);
  std::vector<int> idx(size_t(n), -1);
  for (int d = 0; d < n; d++) {
    const Root r = find(d);
    if (held_[size_t(r.r)]) { M.o[size_t(d)] = r.s * val_[size_t(r.r)] + r.o; M.val[size_t(d)] = M.o[size_t(d)]; continue; }
    if (idx[size_t(r.r)] < 0) idx[size_t(r.r)] = M.unknowns++;
    M.var[size_t(d)] = idx[size_t(r.r)]; M.s[size_t(d)] = r.s; M.o[size_t(d)] = r.o;
  }
  return M;
}

// ---------- polygons into triangles ----------
std::vector<Tri> triangulate(const std::vector<Vec3>& ring) {
  const int n = int(ring.size()); std::vector<Tri> out; if (n < 3) return out;
  if (n == 3) return {{0, 1, 2}};
  Vec3 N; for (int k = 0; k < n; k++) { const Vec3& a = ring[size_t(k)]; const Vec3& b = ring[size_t((k + 1) % n)];
    N.x += (a.y - b.y) * (a.z + b.z); N.y += (a.z - b.z) * (a.x + b.x); N.z += (a.x - b.x) * (a.y + b.y); }
  auto fan = [&] { std::vector<Tri> f; for (int k = 1; k + 1 < n; k++) f.push_back({0, k, k + 1}); return f; };
  if (!(N.lengthSq() > 0)) return fan();
  N = N.normalized(); Vec3 ax = std::abs(N.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0); const Vec3 bx = N.cross(ax).normalized(); ax = bx.cross(N);
  std::vector<std::array<double, 2>> p(static_cast<size_t>(n)); for (int k = 0; k < n; k++) p[size_t(k)] = {ring[size_t(k)].dot(ax), ring[size_t(k)].dot(bx)};
  auto area = [&](int a, int b, int c) { return (p[size_t(b)][0] - p[size_t(a)][0]) * (p[size_t(c)][1] - p[size_t(a)][1]) - (p[size_t(c)][0] - p[size_t(a)][0]) * (p[size_t(b)][1] - p[size_t(a)][1]); };
  if (n == 4) {                                               // (the diagonal keeping both halves the right way; both do — the shorter)
    const bool d02 = area(0, 1, 2) > 0 && area(0, 2, 3) > 0, d13 = area(1, 2, 3) > 0 && area(1, 3, 0) > 0;
    const bool use02 = d02 && (!d13 || (ring[0] - ring[2]).lengthSq() <= (ring[1] - ring[3]).lengthSq());
    if (use02 || !d13) return {{0, 1, 2}, {0, 2, 3}};
    return {{1, 2, 3}, {1, 3, 0}};
  }
  std::vector<int> idx(static_cast<size_t>(n)); std::iota(idx.begin(), idx.end(), 0);
  auto inside = [&](int q, int a, int b, int c) { return area(a, b, q) >= 0 && area(b, c, q) >= 0 && area(c, a, q) >= 0; };
  while (idx.size() > 3) {
    const int m = int(idx.size()); bool cut = false;
    for (int i = 0; i < m && !cut; i++) {
      const int a = idx[size_t((i + m - 1) % m)], b = idx[size_t(i)], c = idx[size_t((i + 1) % m)];
      if (!(area(a, b, c) > 0)) continue;
      bool ear = true; for (int q : idx) if (q != a && q != b && q != c && inside(q, a, b, c)) { ear = false; break; }
      if (!ear) continue;
      out.push_back({a, b, c}); idx.erase(idx.begin() + i); cut = true;
    }
    if (!cut) { for (size_t k = 1; k + 1 < idx.size(); k++) out.push_back({idx[0], idx[k], idx[k + 1]}); return out; }   // (no ear: the rest as a fan)
  }
  out.push_back({idx[0], idx[1], idx[2]});
  return out;
}

// ---------- LSCM ----------
std::vector<P2> lscm(const std::vector<Vec3>& X, const std::vector<Tri>& tris, const Dofs& D, const std::vector<P2>& start) {
  const Dofs::Map M = D.resolve(); std::vector<P2> U = start; U.resize(X.size());
  auto put = [&](const Eigen::VectorXd* z) { for (size_t i = 0; i < X.size(); i++) for (int c = 0; c < 2; c++) { const size_t d = i * 2 + size_t(c);
    U[i][size_t(c)] = (M.var[d] >= 0 ? (z ? M.s[d] * (*z)[M.var[d]] : U[i][size_t(c)] - M.o[d]) : 0) + M.o[d]; } };
  if (M.unknowns == 0) { put(nullptr); return U; }
  const std::vector<Rest> R = rests(X, tris);
  std::vector<Trip> T; std::vector<double> b; int row = 0;
  // two rows a triangle (∂u/∂x − ∂v/∂y, ∂u/∂y + ∂v/∂x) × √area: the dofs held — to the right-hand side
  for (size_t t = 0; t < tris.size(); t++) {
    if (!(R[t].A > 0)) continue; const double w = std::sqrt(R[t].A);
    for (int k = 0; k < 2; k++) {
      double cst = 0;
      for (int j = 0; j < 3; j++) {
        const size_t du = size_t(tris[t][size_t(j)]) * 2, dv = du + 1;
        const double cu = k == 0 ? w * R[t].gx[j] : w * R[t].gy[j], cv = k == 0 ? -w * R[t].gy[j] : w * R[t].gx[j];
        for (auto [d, c] : {std::pair<size_t, double>{du, cu}, {dv, cv}}) { if (M.var[d] >= 0) T.emplace_back(row, M.var[d], c * M.s[d]); cst += c * M.o[d]; }
      }
      b.push_back(-cst); row++;
    }
  }
  SpMat A(row, M.unknowns); A.setFromTriplets(T.begin(), T.end());
  const Eigen::VectorXd bv = Eigen::Map<Eigen::VectorXd>(b.data(), Eigen::Index(b.size()));
  const SpMat N = SpMat(A.transpose()) * A; const Eigen::VectorXd rhs = A.transpose() * bv;
  Eigen::SimplicialLDLT<SpMat> ldlt(N); if (ldlt.info() != Eigen::Success) return start;
  const Eigen::VectorXd z = ldlt.solve(rhs); if (ldlt.info() != Eigen::Success || !z.allFinite()) return start;
  put(&z); return U;
}

// ---------- Tutte ----------
namespace {
std::vector<P2> tutteImpl(const std::vector<Vec3>& X, const std::vector<Tri>& tris, const std::vector<int>* mirror) {
  const int n = int(X.size()); if (n < 3 || tris.empty()) return {};
  // the borders: the edges of one triangle only, as the triangles go round (the inside on their left)
  std::map<std::pair<int, int>, int> dir; for (auto& t : tris) for (int j = 0; j < 3; j++) dir[{t[size_t(j)], t[size_t((j + 1) % 3)]}]++;
  std::vector<int> next(size_t(n), -1); int nb = 0;
  for (auto& [e, c] : dir) if (!dir.count({e.second, e.first})) { if (next[size_t(e.first)] >= 0) return {}; next[size_t(e.first)] = e.second; nb++; }
  if (!nb) return {};
  std::vector<std::vector<int>> loops; std::vector<double> len; std::vector<char> seen(size_t(n), 0);
  for (int s = 0; s < n; s++) {
    if (next[size_t(s)] < 0 || seen[size_t(s)]) continue;
    std::vector<int> lp; double l = 0;
    for (int v = s; ; ) { lp.push_back(v); seen[size_t(v)] = 1; const int w = next[size_t(v)]; if (w < 0) return {}; l += (X[size_t(w)] - X[size_t(v)]).length(); v = w; if (v == s) break; if (seen[size_t(v)]) return {}; }
    loops.push_back(std::move(lp)); len.push_back(l);
  }
  // the longest border on a circle (by length); each other one (a hole) closed for the solve: a point of its own joined
  // to all of it, gone after
  const size_t outer = size_t(std::max_element(len.begin(), len.end()) - len.begin());
  const std::vector<int>& loop = loops[outer]; const double L = len[outer]; if (!(L > 0)) return {};
  const int nv = n + int(loops.size()) - 1;
  std::vector<P2> U(size_t(nv), P2{0, 0}); std::vector<char> rim(size_t(nv), 0);
  { const double r = L / (2 * M_PI); double acc = 0, t0 = 0;   // (t0: where the mirror line crosses it — at the bottom)
    if (mirror) for (size_t k = 0; k < loop.size(); k++) { const int v = loop[k], w = loop[(k + 1) % loop.size()]; const double el = (X[size_t(w)] - X[size_t(v)]).length();
      if ((*mirror)[size_t(v)] == v) { t0 = acc; break; } if ((*mirror)[size_t(v)] == w) { t0 = acc + el / 2; break; } acc += el; }
    acc = 0;
    for (size_t k = 0; k < loop.size(); k++) { const double a = 2 * M_PI * (acc - t0) / L - M_PI / 2; U[size_t(loop[k])] = {r * std::cos(a), r * std::sin(a)}; rim[size_t(loop[k])] = 1;
      acc += (X[size_t(loop[(k + 1) % loop.size()])] - X[size_t(loop[k])]).length(); } }
  std::set<std::pair<int, int>> E; for (auto& t : tris) for (int j = 0; j < 3; j++) { int a = t[size_t(j)], b = t[size_t((j + 1) % 3)]; E.insert({std::min(a, b), std::max(a, b)}); }
  { int h = n; for (size_t k = 0; k < loops.size(); k++) if (k != outer) { for (int v : loops[k]) E.insert({v, h}); h++; } }
  std::vector<int> var(size_t(nv), -1); int m = 0; for (int i = 0; i < nv; i++) if (!rim[size_t(i)]) var[size_t(i)] = m++;
  if (m) {
    std::vector<Trip> T; Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(m, 2);
    for (auto& e : E) for (auto [a, b] : {e, std::pair<int, int>{e.second, e.first}}) {
      if (var[size_t(a)] < 0) continue; T.emplace_back(var[size_t(a)], var[size_t(a)], 1.0);
      if (var[size_t(b)] >= 0) T.emplace_back(var[size_t(a)], var[size_t(b)], -1.0); else { rhs(var[size_t(a)], 0) += U[size_t(b)][0]; rhs(var[size_t(a)], 1) += U[size_t(b)][1]; }
    }
    SpMat Lp(m, m); Lp.setFromTriplets(T.begin(), T.end());
    Eigen::SimplicialLDLT<SpMat> ldlt(Lp); if (ldlt.info() != Eigen::Success) return {};
    const Eigen::MatrixXd z = ldlt.solve(rhs); if (ldlt.info() != Eigen::Success || !z.allFinite()) return {};
    for (int i = 0; i < nv; i++) if (var[size_t(i)] >= 0) U[size_t(i)] = {z(var[size_t(i)], 0), z(var[size_t(i)], 1)};
  }
  if (mirror) { const auto V = U; for (int i = 0; i < n; i++) { const int j = (*mirror)[size_t(i)]; if (j >= 0) U[size_t(i)] = {(V[size_t(i)][0] - V[size_t(j)][0]) / 2, (V[size_t(i)][1] + V[size_t(j)][1]) / 2}; } }   // (a mesh a hair off symmetric: made exact)
  U.resize(size_t(n));
  return U;
}

}  // namespace
std::vector<P2> tutte(const std::vector<Vec3>& X, const std::vector<Tri>& tris, const std::vector<int>* mirror, const std::vector<std::array<int, 2>>* weld) {
  if (!weld || weld->empty()) return tutteImpl(X, tris, mirror);
  // welded: each pair one vertex (a hole zipped shut), the triangles over them; then every vertex where its one lies
  const int n = int(X.size()); std::vector<int> rep(static_cast<size_t>(n)); std::iota(rep.begin(), rep.end(), 0);
  auto find = [&](int x) { while (rep[size_t(x)] != x) x = rep[size_t(x)] = rep[size_t(rep[size_t(x)])]; return x; };
  for (auto& w : *weld) if (w[0] >= 0 && w[1] >= 0 && w[0] < n && w[1] < n) { const int a = find(w[0]), b = find(w[1]); if (a != b) rep[size_t(a)] = b; }
  std::vector<int> id(size_t(n), -1); std::vector<Vec3> X2; std::vector<Tri> T2;
  for (auto& t : tris) { Tri q; for (int j = 0; j < 3; j++) q[size_t(j)] = find(t[size_t(j)]); if (q[0] == q[1] || q[1] == q[2] || q[0] == q[2]) continue;
    for (int& v : q) { if (id[size_t(v)] < 0) { id[size_t(v)] = int(X2.size()); X2.push_back(X[size_t(v)]); } v = id[size_t(v)]; } T2.push_back(q); }
  std::vector<int> mir2; if (mirror) { mir2.assign(X2.size(), -1); for (int i = 0; i < n; i++) { const int a = id[size_t(find(i))], j = (*mirror)[size_t(i)]; if (a >= 0 && j >= 0 && id[size_t(find(j))] >= 0) mir2[size_t(a)] = id[size_t(find(j))]; } }
  const auto U2 = tutteImpl(X2, T2, mirror ? &mir2 : nullptr); if (U2.empty()) return {};
  std::vector<P2> U(size_t(n), P2{0, 0}); for (int i = 0; i < n; i++) { const int a = id[size_t(find(i))]; if (a >= 0) U[size_t(i)] = U2[size_t(a)]; }
  return U;
}

// ---------- what is wrong ----------
int flips(const std::vector<P2>& U, const std::vector<Vec3>& X, const std::vector<Tri>& tris) {
  const std::vector<Rest> R = rests(X, tris); const double o = orientation(R, tris, U); int n = 0;
  for (size_t t = 0; t < tris.size(); t++) if (R[t].A > 0 && !(o * jac(R[t], tris[t], U).det() > 0)) n++;
  return n;
}
double distortion(const std::vector<P2>& U, const std::vector<Vec3>& X, const std::vector<Tri>& tris) {
  const std::vector<Rest> R = rests(X, tris);
  double a3 = 0, au = 0; for (size_t t = 0; t < tris.size(); t++) if (R[t].A > 0) { a3 += R[t].A; au += R[t].A * std::abs(jac(R[t], tris[t], U).det()); }
  if (!(a3 > 0) || !(au > 0)) return INF;
  const double k2 = a3 / au; double e = 0;                    // (the UV taken at the 3D size: J·k)
  for (size_t t = 0; t < tris.size(); t++) { if (!(R[t].A > 0)) continue; const Jac J = jac(R[t], tris[t], U); const double dt = std::abs(J.det()) * k2;
    if (!(dt > 1e-300)) return INF; e += R[t].A * J.fro2() * k2 * (1 + 1 / (dt * dt)); }
  return e / a3;
}

// ---------- SLIM ----------
SlimResult slim(std::vector<P2>& U, const std::vector<Vec3>& X, const std::vector<Tri>& tris, const Dofs& D, int iters, const std::vector<std::array<int, 2>>* soft, double wSoft) {
  SlimResult res; if (U.size() != X.size() || tris.empty()) return res;
  std::vector<Rest> R = rests(X, tris);
  if (orientation(R, tris, U) < 0) mirrorRest(R);             // (laid mirrored: worked on as it lies)
  const Dofs::Map M = D.resolve(); if (M.unknowns == 0) return res;
  // the unknowns from U (a tie not kept by U: the first of its dofs wins), then U made to keep them all
  Eigen::VectorXd z = Eigen::VectorXd::Zero(M.unknowns); std::vector<char> got(size_t(M.unknowns), 0);
  for (size_t d = 0; d < size_t(D.size()); d++) { const int v = M.var[d]; if (v < 0 || got[size_t(v)]) continue; z[v] = (U[d / 2][d % 2] - M.o[d]) / M.s[d]; got[size_t(v)] = 1; }
  auto toU = [&](const Eigen::VectorXd& zz, std::vector<P2>& out) { out.resize(X.size()); for (size_t d = 0; d < size_t(D.size()); d++) out[d / 2][d % 2] = (M.var[d] >= 0 ? M.s[d] * zz[M.var[d]] : 0) + M.o[d]; };
  toU(z, U);
  double sumA = 0; for (auto& r : R) if (r.A > 0) sumA += r.A; if (!(sumA > 0)) sumA = 1;
  auto penalty = [&](const std::vector<P2>& V) { double p = 0; if (soft) for (auto& s : *soft) { const double d = V[size_t(s[0]) / 2][size_t(s[0]) % 2] - V[size_t(s[1]) / 2][size_t(s[1]) % 2]; p += d * d; } return wSoft * p / sumA; };
  auto energy = [&](const std::vector<P2>& V) { return energyOf(R, tris, V) + penalty(V); };   // (the stretch and the soft pull: what the steps lower)
  double E = energy(U); res.e0 = E; res.e1 = E;
  if (!std::isfinite(E)) return res;                           // (a triangle turned over: SLIM can't start)
  res.ok = true;
  const double lambda = 1e-4;                                  // (a pull to where each unknown is: the place, and the steps kept sane)
  Eigen::SimplicialLDLT<SpMat> ldlt; bool analysed = false;
  std::vector<Trip> T; T.reserve(tris.size() * 24 + size_t(M.unknowns)); std::vector<double> b; b.reserve(tris.size() * 4);
  std::vector<P2> Un; std::vector<double> dU(size_t(D.size()));
  for (int it = 0; it < iters; it++) {
    // local: per triangle its nearest rotation and the weights that make the next solve a step of the symmetric Dirichlet
    T.clear(); b.clear(); int row = 0;
    for (size_t t = 0; t < tris.size(); t++) {
      if (!(R[t].A > 0)) continue;
      const Jac J = jac(R[t], tris[t], U); Eigen::Matrix2d Jm; Jm << J.a, J.b, J.c, J.d;
      Eigen::JacobiSVD<Eigen::Matrix2d> svd(Jm, Eigen::ComputeFullU | Eigen::ComputeFullV);
      const Eigen::Vector2d sg = svd.singularValues(); Eigen::Matrix2d Uu = svd.matrixU(), Vv = svd.matrixV();
      if (Uu.determinant() * Vv.determinant() < 0) { Uu.col(1) *= -1; }   // (det J > 0: a rotation — kept so if the SVD took a mirror)
      Eigen::Vector2d w; for (int k = 0; k < 2; k++) { const double s = std::max(sg[k], 1e-8); w[k] = std::abs(s - 1) < 1e-8 ? 1 : std::sqrt((s - std::pow(s, -3)) / (s - 1)); }
      const Eigen::Matrix2d Rt = Uu * Vv.transpose(), W = Uu * w.asDiagonal() * Uu.transpose(), WR = W * Rt;
      const double sa = std::sqrt(R[t].A);
      // global rows: √A · (W·J − W·R)[r][c], J's column c = ∇_c u, ∇_c v
      for (int r = 0; r < 2; r++) for (int c = 0; c < 2; c++) {
        double cst = 0;
        for (int j = 0; j < 3; j++) {
          const double g = c == 0 ? R[t].gx[j] : R[t].gy[j]; const size_t du = size_t(tris[t][size_t(j)]) * 2, dv = du + 1;
          for (auto [d, cf] : {std::pair<size_t, double>{du, sa * W(r, 0) * g}, {dv, sa * W(r, 1) * g}}) { if (M.var[d] >= 0) T.emplace_back(row, M.var[d], cf * M.s[d]); cst += cf * M.o[d]; }
        }
        b.push_back(sa * WR(r, c) - cst); row++;
      }
    }
    if (soft) { const double sw = std::sqrt(wSoft);              // (the soft pairs: √w · (x_a − x_b) → 0)
      for (auto& s : *soft) { double cst = 0; for (auto [d, cf] : {std::pair<size_t, double>{size_t(s[0]), sw}, {size_t(s[1]), -sw}}) { if (M.var[d] >= 0) T.emplace_back(row, M.var[d], cf * M.s[d]); cst += cf * M.o[d]; }
        b.push_back(-cst); row++; } }
    SpMat A(row, M.unknowns); A.setFromTriplets(T.begin(), T.end());
    const Eigen::VectorXd bv = Eigen::Map<Eigen::VectorXd>(b.data(), Eigen::Index(b.size()));
    SpMat N = SpMat(A.transpose()) * A; Eigen::VectorXd rhs = A.transpose() * bv;
    for (int k = 0; k < M.unknowns; k++) { N.coeffRef(k, k) += lambda; rhs[k] += lambda * z[k]; }
    if (!analysed) { ldlt.analyzePattern(N); analysed = true; }
    ldlt.factorize(N); if (ldlt.info() != Eigen::Success) break;
    const Eigen::VectorXd p = ldlt.solve(rhs); if (ldlt.info() != Eigen::Success || !p.allFinite()) break;
    const Eigen::VectorXd dz = p - z;
    for (size_t d = 0; d < dU.size(); d++) dU[d] = M.var[d] >= 0 ? M.s[d] * dz[M.var[d]] : 0;
    // the longest step no triangle turns over in (its det along the step: a quadratic), then halved till the energy drops
    double amax = 1;
    for (size_t t = 0; t < tris.size(); t++) {
      if (!(R[t].A > 0)) continue; const Jac J = jac(R[t], tris[t], U); Jac G;
      for (int j = 0; j < 3; j++) { const size_t i = size_t(tris[t][size_t(j)]); G.a += dU[i * 2] * R[t].gx[j]; G.b += dU[i * 2] * R[t].gy[j]; G.c += dU[i * 2 + 1] * R[t].gx[j]; G.d += dU[i * 2 + 1] * R[t].gy[j]; }
      const double qa = G.det(), qb = J.a * G.d + J.d * G.a - J.b * G.c - J.c * G.b, qc = J.det();
      double root = INF;
      if (std::abs(qa) < 1e-30 * std::max(1.0, std::abs(qb))) { if (qb < 0) root = -qc / qb; }
      else { const double disc = qb * qb - 4 * qa * qc; if (disc >= 0) { const double sq = std::sqrt(disc); for (double x : {(-qb - sq) / (2 * qa), (-qb + sq) / (2 * qa)}) if (x > 0) root = std::min(root, x); } }
      amax = std::min(amax, 0.9 * root);
    }
    double a = amax, En = INF; bool moved = false;
    for (int k = 0; k < 30 && a > 1e-12; k++, a *= 0.5) {
      Un = U; for (size_t d = 0; d < dU.size(); d++) Un[d / 2][d % 2] += a * dU[d];
      En = energy(Un); if (En < E) { moved = true; break; }
    }
    res.iters = it + 1;
    if (!moved) break;
    U.swap(Un); z += a * dz; const double dE = E - En; E = En;
    if (dE < 1e-4 * E) break;                                  // (settled)
  }
  res.e1 = E;
  return res;
}

}  // namespace hm::uv::solve
