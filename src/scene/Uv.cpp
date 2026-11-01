#include "scene/Uv.h"
#include "scene/UvSolve.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_map>

namespace hm::uv {

namespace {
struct Dsu { std::vector<int> p; explicit Dsu(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
  int find(int x) { while (p[size_t(x)] != x) x = p[size_t(x)] = p[size_t(p[size_t(x)])]; return x; }
  void unite(int a, int b) { a = find(a); b = find(b); if (a != b) p[size_t(a)] = b; } };
const float EPS = 1e-6f;
bool same(const UV& a, const UV& b) { return std::abs(a[0] - b[0]) < EPS && std::abs(a[1] - b[1]) < EPS; }
Vec3 newell(const PolyMesh& m, int f) {
  Vec3 n; int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
  for (int k = s; k < e; k++) { const Vec3& a = m.pos[size_t(m.faceVerts[size_t(k)])]; const Vec3& b = m.pos[size_t(m.faceVerts[size_t(k + 1 < e ? k + 1 : s)])];
    n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
  return n;
}
// the nearest point of triangle abc to p: its barycentric weights (Ericson, Real-Time Collision Detection 5.1.5)
std::array<double, 3> closestBary(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
  Vec3 ab = b - a, ac = c - a, ap = p - a;
  double d1 = ab.dot(ap), d2 = ac.dot(ap); if (d1 <= 0 && d2 <= 0) return {1, 0, 0};
  Vec3 bp = p - b; double d3 = ab.dot(bp), d4 = ac.dot(bp); if (d3 >= 0 && d4 <= d3) return {0, 1, 0};
  double vc = d1 * d4 - d3 * d2; if (vc <= 0 && d1 >= 0 && d3 <= 0) { double v = d1 / (d1 - d3); return {1 - v, v, 0}; }
  Vec3 cp = p - c; double d5 = ab.dot(cp), d6 = ac.dot(cp); if (d6 >= 0 && d5 <= d6) return {0, 0, 1};
  double vb = d5 * d2 - d1 * d6; if (vb <= 0 && d2 >= 0 && d6 <= 0) { double w = d2 / (d2 - d6); return {1 - w, 0, w}; }
  double va = d3 * d6 - d5 * d4; if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { double w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); return {0, 1 - w, w}; }
  double den = 1 / (va + vb + vc), v = vb * den, w = vc * den; return {1 - v - w, v, w};
}
// the convex hull of 2D points (Andrew's monotone chain)
std::vector<std::array<double, 2>> hull(std::vector<std::array<double, 2>> p) {
  std::sort(p.begin(), p.end()); p.erase(std::unique(p.begin(), p.end()), p.end()); if (p.size() < 3) return p;
  auto cross = [](const std::array<double, 2>& o, const std::array<double, 2>& a, const std::array<double, 2>& b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); };
  std::vector<std::array<double, 2>> h(p.size() * 2); size_t k = 0;
  for (size_t i = 0; i < p.size(); i++) { while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) k--; h[k++] = p[i]; }
  for (size_t i = p.size() - 1, t = k + 1; i > 0; i--) { while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0) k--; h[k++] = p[i - 1]; }
  h.resize(k - 1); return h;
}
}  // namespace

bool has(const PolyMesh& m) { return !m.cornerUV.empty() && m.cornerUV.size() == m.faceVerts.size(); }
void ensure(PolyMesh& m) { if (!has(m)) m.cornerUV.assign(m.faceVerts.size(), UV{0, 0}); }
int cornerAt(const topo::TTopo& T, int f, int v) { for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) if (T.fv[size_t(k)] == v) return k; return -1; }

Points points(const PolyMesh& m, const topo::TTopo& T) {
  Points P; const size_t nc = T.fv.size(); P.of.assign(nc, -1);
  std::vector<int> voff(size_t(T.n) + 1, 0); for (size_t k = 0; k < nc; k++) voff[size_t(T.fv[k]) + 1]++;
  for (int v = 0; v < T.n; v++) voff[size_t(v) + 1] += voff[size_t(v)];
  std::vector<int> vc(nc), at(voff.begin(), voff.end() - 1); for (size_t k = 0; k < nc; k++) vc[size_t(at[size_t(T.fv[k])]++)] = int(k);
  const bool uv = has(m); std::vector<int> reps;
  for (int v = 0; v < T.n; v++) {
    const int first = P.count(); reps.clear();
    for (int i = voff[size_t(v)]; i < voff[size_t(v) + 1]; i++) { const int k = vc[size_t(i)]; int pt = -1;
      for (size_t q = 0; q < reps.size() && pt < 0; q++) if (!uv || same(m.cornerUV[size_t(k)], m.cornerUV[size_t(reps[q])])) pt = first + int(q);
      if (pt < 0) { pt = first + int(reps.size()); reps.push_back(k); P.vert.push_back(v); }
      P.of[size_t(k)] = pt; }
  }
  P.off.assign(size_t(P.count()) + 1, 0); for (size_t k = 0; k < nc; k++) P.off[size_t(P.of[k]) + 1]++;
  for (int p = 0; p < P.count(); p++) P.off[size_t(p) + 1] += P.off[size_t(p)];
  P.list.resize(nc); std::vector<int> pa(P.off.begin(), P.off.end() - 1); for (size_t k = 0; k < nc; k++) P.list[size_t(pa[size_t(P.of[k])]++)] = int(k);
  return P;
}
bool continuous(const PolyMesh& m, const topo::TTopo& T, const Points& P, int e) {
  int f0 = T.ef0[size_t(e)], f1 = T.ef1[size_t(e)]; if (f1 < 0) return false;
  if (!has(m)) return true;
  int a = T.ea[size_t(e)], b = T.eb[size_t(e)];
  int c0a = cornerAt(T, f0, a), c0b = cornerAt(T, f0, b), c1a = cornerAt(T, f1, a), c1b = cornerAt(T, f1, b);
  if (c0a < 0 || c0b < 0 || c1a < 0 || c1b < 0) return false;
  return P.of[size_t(c0a)] == P.of[size_t(c1a)] && P.of[size_t(c0b)] == P.of[size_t(c1b)];
}

Shells shells(const PolyMesh& m, const topo::TTopo& T, const Points& P) {
  Dsu u(size_t(T.nf)); for (int e = 0; e < T.ne; e++) if (T.ef1[size_t(e)] >= 0 && continuous(m, T, P, e)) u.unite(T.ef0[size_t(e)], T.ef1[size_t(e)]);
  Shells S; S.of.assign(size_t(T.nf), -1); std::unordered_map<int, int> id;
  for (int f = 0; f < T.nf; f++) { auto r = id.emplace(u.find(f), S.count); if (r.second) S.count++; S.of[size_t(f)] = r.first->second; }
  return S;
}
std::vector<int> borderEdges(const PolyMesh& m, const topo::TTopo& T, const Points& P) {
  std::vector<int> o; if (!has(m)) return o;
  for (int e = 0; e < T.ne; e++) if (T.ef1[size_t(e)] >= 0 && !continuous(m, T, P, e)) o.push_back(e);
  return o;
}

std::vector<char> seamFlags(const PolyMesh& m, const topo::TTopo& T) {
  std::vector<char> s(size_t(T.ne), 0); for (auto& p : m.seams) { int e = T.edgeOf(p[0], p[1]); if (e >= 0) s[size_t(e)] = 1; }
  return s;
}
void setSeams(PolyMesh& m, const topo::TTopo& T, const std::vector<char>& flags) {
  m.seams.clear(); for (int e = 0; e < T.ne && size_t(e) < flags.size(); e++) if (flags[size_t(e)]) m.seams.push_back({T.ea[size_t(e)], T.eb[size_t(e)]});
}
std::vector<int> seamsByAngle(const PolyMesh& m, const topo::TTopo& T, double deg) {
  std::vector<Vec3> n(size_t(T.nf)); for (int f = 0; f < T.nf; f++) { Vec3 v = newell(m, f); n[size_t(f)] = v.lengthSq() > 0 ? v.normalized() : v; }
  const double c = std::cos(deg * M_PI / 180); std::vector<int> o;
  for (int e = 0; e < T.ne; e++) { int a = T.ef0[size_t(e)], b = T.ef1[size_t(e)]; if (b >= 0 && n[size_t(a)].dot(n[size_t(b)]) < c) o.push_back(e); }
  return o;
}

// Auto seams for a closed shell (no border — Unfold can't open it): the cut graph of the classic tree / cotree split —
// the polygons joined by a maximum spanning tree over the edges dearest to cut (flat, long-visible, facing the front +Z),
// the edges left over pruned of their dangling ends: what stays opens the handles (a torus: two loops); and a path
// between the shell's two farthest points over the cheapest edges (sharp, at the back, below) opens the rest into a disk
std::vector<int> autoSeams(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces) {
  { Dsu pc(size_t(T.nf)); std::vector<char> inF(size_t(T.nf), 0); for (int f : faces) if (f >= 0 && f < T.nf) inF[size_t(f)] = 1;   // (shell by shell)
    const std::vector<char> sf = seamFlags(m, T);
    for (int e = 0; e < T.ne; e++) { const int a = T.ef0[size_t(e)], b = T.ef1[size_t(e)]; if (b >= 0 && inF[size_t(a)] && inF[size_t(b)] && !sf[size_t(e)]) pc.unite(a, b); }
    std::map<int, std::vector<int>> by; for (int f = 0; f < T.nf; f++) if (inF[size_t(f)]) by[pc.find(f)].push_back(f);
    if (by.size() > 1) { std::vector<int> out; for (auto& [r, fs] : by) if (fs.size() > 1) { auto o = autoSeams(m, T, fs); out.insert(out.end(), o.begin(), o.end()); } return out; } }
  const std::vector<char> seam = seamFlags(m, T); std::vector<char> in(size_t(T.nf), 0); for (int f : faces) if (f >= 0 && f < T.nf) in[size_t(f)] = 1;
  std::vector<Vec3> n(size_t(T.nf)), c(size_t(T.nf)); Vec3 lo(1e30, 1e30, 1e30), hi(-1e30, -1e30, -1e30);
  for (int f = 0; f < T.nf; f++) if (in[size_t(f)]) { Vec3 v = newell(m, f); n[size_t(f)] = v.lengthSq() > 0 ? v.normalized() : v;
    for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const Vec3& p = m.pos[size_t(T.fv[size_t(k)])]; lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)); hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)); } }
  const double hy = std::max(1e-12, hi.y - lo.y);
  // (what an edge costs to cut: its length × how flat the polygons meet there × how much it faces the front, its height)
  std::vector<double> cost(size_t(T.ne), -1); std::vector<char> inner(size_t(T.ne), 0);
  for (int e = 0; e < T.ne; e++) { const int a = T.ef0[size_t(e)], b = T.ef1[size_t(e)]; if (b < 0 || !in[size_t(a)] || !in[size_t(b)] || seam[size_t(e)]) continue; inner[size_t(e)] = 1;
    const Vec3& pa = m.pos[size_t(T.ea[size_t(e)])]; const Vec3& pb = m.pos[size_t(T.eb[size_t(e)])]; const Vec3 nm = n[size_t(a)] + n[size_t(b)];
    const double flat = std::clamp((1 + n[size_t(a)].dot(n[size_t(b)])) / 2, 0.0, 1.0), front = nm.lengthSq() > 0 ? std::max(0.0, nm.normalized().z) : 0, up = ((pa.y + pb.y) / 2 - lo.y) / hy;
    cost[size_t(e)] = pa.distanceTo(pb) * (0.15 + flat * flat) * (1 + 1.5 * front) * (0.8 + 0.4 * up); }
  // the dual tree: the dearest edges kept uncut
  std::vector<int> es; for (int e = 0; e < T.ne; e++) if (inner[size_t(e)]) es.push_back(e);
  std::sort(es.begin(), es.end(), [&](int x, int y) { return cost[size_t(x)] > cost[size_t(y)]; });
  Dsu d(size_t(T.nf)); std::vector<char> cut(size_t(T.ne), 0);
  for (int e : es) { const int a = d.find(T.ef0[size_t(e)]), b = d.find(T.ef1[size_t(e)]); if (a != b) d.unite(a, b); else cut[size_t(e)] = 1; }
  // pruned: an edge with an end no other cut edge meets goes (again and again)
  std::map<int, int> deg; for (int e : es) if (cut[size_t(e)]) { deg[T.ea[size_t(e)]]++; deg[T.eb[size_t(e)]]++; }
  std::map<int, std::vector<int>> at; for (int e : es) if (cut[size_t(e)]) { at[T.ea[size_t(e)]].push_back(e); at[T.eb[size_t(e)]].push_back(e); }
  std::vector<int> leaf; for (auto& [v, k] : deg) if (k == 1) leaf.push_back(v);
  while (!leaf.empty()) { const int v = leaf.back(); leaf.pop_back(); if (deg[v] != 1) continue;
    for (int e : at[v]) if (cut[size_t(e)]) { cut[size_t(e)] = 0; deg[v]--; const int w = T.ea[size_t(e)] == v ? T.eb[size_t(e)] : T.ea[size_t(e)]; if (--deg[w] == 1) leaf.push_back(w); break; } }
  // the path: from the point farthest from any one, to the point farthest from it — over the cheapest edges
  std::map<int, std::vector<std::pair<int, int>>> adj; for (int e : es) { adj[T.ea[size_t(e)]].push_back({T.eb[size_t(e)], e}); adj[T.eb[size_t(e)]].push_back({T.ea[size_t(e)], e}); }
  if (!adj.empty()) {
    auto dijkstra = [&](int s, bool byCost, std::map<int, int>* prev) { std::map<int, double> dist; using Q = std::pair<double, int>; std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q; dist[s] = 0; q.push({0, s});
      while (!q.empty()) { auto [dv, v] = q.top(); q.pop(); if (dv > dist[v]) continue;
        for (auto [w, e] : adj[v]) { const double l = m.pos[size_t(T.ea[size_t(e)])].distanceTo(m.pos[size_t(T.eb[size_t(e)])]), nd = dv + (byCost ? cost[size_t(e)] + 1e-3 * l : l);
          auto it = dist.find(w); if (it == dist.end() || nd < it->second) { dist[w] = nd; if (prev) (*prev)[w] = e; q.push({nd, w}); } } }
      int far = s; double fd = -1; for (auto& [v, dv] : dist) if (dv > fd) { fd = dv; far = v; } return far; };
    int onLoop = -1; for (int e : es) if (cut[size_t(e)]) { onLoop = T.ea[size_t(e)]; break; }   // (handles there: the path starts on their loops — one cut, one disk)
    const int a = onLoop >= 0 ? onLoop : dijkstra(dijkstra(adj.begin()->first, false, nullptr), false, nullptr), b = dijkstra(a, false, nullptr);
    std::map<int, int> prev; dijkstra(a, true, &prev);
    for (int v = b; v != a && prev.count(v);) { const int e = prev[v]; cut[size_t(e)] = 1; v = T.ea[size_t(e)] == v ? T.eb[size_t(e)] : T.ea[size_t(e)]; }
  }
  // a round shell (a ball, a head) torn too much by one cut: branches — from the point farthest from the cut, the
  // cheapest way back to it — while the angles' stretch drops (unfolded on a copy, the best kept)
  if (!adj.empty() && faces.size() > 8) {
    auto stretch = [&](const std::vector<char>& c) { PolyMesh w = m; std::vector<char> fl = seam; for (int e : es) if (c[size_t(e)]) fl[size_t(e)] = 1; setSeams(w, T, fl);
      const auto r = unfold(w, T, faces, true, true); if (!r.done || r.closed) return 1e9; const auto a = angleStretch(w, T); double s = 0; for (int f : faces) s += std::min(20.0f, a[size_t(f)]); return s / double(faces.size()); };
    double best = stretch(cut); std::vector<char> bestCut = cut;
    for (int it = 0; it < 4 && best > 1.25; it++) {
      std::map<int, double> dl; std::map<int, int> prev; using Q = std::pair<double, int>; std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
      for (int e : es) if (cut[size_t(e)]) for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) if (!dl.count(v)) { dl[v] = 0; q.push({0, v}); }
      while (!q.empty()) { auto [dv, v] = q.top(); q.pop(); if (dv > dl[v]) continue;
        for (auto [w, e] : adj[v]) { const double nd = dv + m.pos[size_t(T.ea[size_t(e)])].distanceTo(m.pos[size_t(T.eb[size_t(e)])]); auto f = dl.find(w); if (f == dl.end() || nd < f->second) { dl[w] = nd; q.push({nd, w}); } } }
      int far = -1; double fd = 0; for (auto& [v, dv] : dl) if (dv > fd) { fd = dv; far = v; } if (far < 0) break;
      std::map<int, double> dc; dc[far] = 0; q.push({0, far}); int hit = -1;   // (the cheapest way from it to the cut)
      while (!q.empty()) { auto [dv, v] = q.top(); q.pop(); if (dv > dc[v]) continue; if (v != far && dl.count(v) && dl[v] == 0) { hit = v; break; }
        for (auto [w, e] : adj[v]) { const double nd = dv + cost[size_t(e)] + 1e-6; auto f = dc.find(w); if (f == dc.end() || nd < f->second) { dc[w] = nd; prev[w] = e; q.push({nd, w}); } } }
      if (hit < 0) break; while (!q.empty()) q.pop();
      for (int v = hit; v != far && prev.count(v);) { const int e = prev[v]; cut[size_t(e)] = 1; v = T.ea[size_t(e)] == v ? T.eb[size_t(e)] : T.ea[size_t(e)]; }
      const double s = stretch(cut); if (s < best - 0.02) { best = s; bestCut = cut; } else break; }
    cut = bestCut;
  }
  std::vector<int> out; for (int e : es) if (cut[size_t(e)]) out.push_back(e); return out;
}
double faceArea(const PolyMesh& m, int f) {
  double a = 0; int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
  for (int k = s; k < e; k++) { const UV& p = m.cornerUV[size_t(k)]; const UV& q = m.cornerUV[size_t(k + 1 < e ? k + 1 : s)]; a += double(p[0]) * q[1] - double(q[0]) * p[1]; }
  return a / 2;
}
double faceArea3(const PolyMesh& m, int f) { return newell(m, f).length() / 2; }

// ---------- Unfold ----------
// (Unwrap, optimized: each triangle's own shape — laid flat — fitted into the UV turned as it lies there, every point
// the mean of where its triangles want it; repeated — the stretch evened out)
static void shapeRelax(std::vector<std::array<double, 2>>& U, const std::vector<Vec3>& X, const std::vector<std::array<int, 3>>& tris, int iters, const std::vector<char>* held = nullptr) {
  struct Rest { double p[3][2]; }; std::vector<Rest> rest(tris.size());
  for (size_t t = 0; t < tris.size(); t++) { const Vec3 a = X[size_t(tris[t][0])], b = X[size_t(tris[t][1])], c = X[size_t(tris[t][2])];
    const Vec3 e1 = b - a; const double L = e1.length(); const Vec3 x = L > 0 ? e1 * (1 / L) : Vec3(1, 0, 0); Vec3 n = e1.cross(c - a); Vec3 y = n.cross(e1); y = y.lengthSq() > 0 ? y.normalized() : Vec3(0, 1, 0);
    rest[t] = {{{0, 0}, {L, 0}, {(c - a).dot(x), (c - a).dot(y)}}}; }
  std::vector<std::array<double, 2>> sum(U.size()); std::vector<int> cnt(U.size());
  for (int it = 0; it < iters; it++) {
    std::fill(sum.begin(), sum.end(), std::array<double, 2>{0, 0}); std::fill(cnt.begin(), cnt.end(), 0);
    for (size_t t = 0; t < tris.size(); t++) { const auto& R = rest[t].p; double cu[2] = {0, 0}, cr[2] = {0, 0};
      for (int i = 0; i < 3; i++) { cu[0] += U[size_t(tris[t][size_t(i)])][0] / 3; cu[1] += U[size_t(tris[t][size_t(i)])][1] / 3; cr[0] += R[i][0] / 3; cr[1] += R[i][1] / 3; }
      double sc = 0, ss = 0; for (int i = 0; i < 3; i++) { const double rx = R[i][0] - cr[0], ry = R[i][1] - cr[1], ux = U[size_t(tris[t][size_t(i)])][0] - cu[0], uy = U[size_t(tris[t][size_t(i)])][1] - cu[1]; sc += rx * ux + ry * uy; ss += rx * uy - ry * ux; }
      const double th = std::atan2(ss, sc), c = std::cos(th), s = std::sin(th);
      for (int i = 0; i < 3; i++) { const double rx = R[i][0] - cr[0], ry = R[i][1] - cr[1]; auto& S = sum[size_t(tris[t][size_t(i)])]; S[0] += cu[0] + c * rx - s * ry; S[1] += cu[1] + s * rx + c * ry; cnt[size_t(tris[t][size_t(i)])]++; } }
    for (size_t v = 0; v < U.size(); v++) if (cnt[v] && !(held && (*held)[v])) U[v] = {sum[v][0] / cnt[v], sum[v][1] / cnt[v]};   // (the pinned ones stay)
  }
}

namespace {
// a polygon's triangles in a shell (its corners' local vertices li): cut the right way round in its plane
void addTris(const PolyMesh& m, const topo::TTopo& T, int f, const std::vector<int>& li, std::vector<solve::Tri>& tris) {
  std::vector<Vec3> ring; for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) ring.push_back(m.pos[size_t(T.fv[size_t(k)])]);
  for (auto& t : solve::triangulate(ring)) tris.push_back({li[size_t(t[0])], li[size_t(t[1])], li[size_t(t[2])]});
}
// SLIM on a shell's UV (at its 3D size); a start with a triangle turned over: Tutte's disc instead when `restart` (one
// border, nothing held; mirror: a symmetric shell, the disc symmetric too; weld: a face's pairs zipped) — else left as it is → done
bool slimShell(std::vector<solve::P2>& U, const std::vector<Vec3>& X, const std::vector<solve::Tri>& tris, const solve::Dofs& D, int iters, bool restart, const std::vector<int>* mirror = nullptr, const std::vector<std::array<int, 2>>* weld = nullptr, const solve::Dofs* loose = nullptr, const std::vector<std::array<int, 2>>* soft = nullptr) {
  if (solve::flips(U, X, tris)) {
    if (!restart) return false; auto Tt = solve::tutte(X, tris, mirror, weld);
    if (Tt.empty() || solve::flips(Tt, X, tris)) return false;
    // (from the disc; a face's ties it can't keep yet: first as a strong pull (loose — without them), then held)
    auto W = Tt; auto r = solve::slim(W, X, tris, loose && soft ? *loose : D, iters, loose && soft ? soft : nullptr);
    if (r.ok && loose && soft) { auto W2 = W; const auto r2 = solve::slim(W2, X, tris, D, iters); if (r2.ok) W = std::move(W2); }
    if (r.ok) U = std::move(W); return r.ok;
  }
  return solve::slim(U, X, tris, D, iters).ok;
}
// U turned and moved to lie as `to` does (the best fit, no scaling)
void fitTo(std::vector<solve::P2>& U, const std::vector<solve::P2>& to) {
  const double n = double(U.size()); if (!(n > 0)) return; double a[2] = {0, 0}, b[2] = {0, 0};
  for (size_t i = 0; i < U.size(); i++) { a[0] += U[i][0] / n; a[1] += U[i][1] / n; b[0] += to[i][0] / n; b[1] += to[i][1] / n; }
  double sc = 0, ss = 0; for (size_t i = 0; i < U.size(); i++) { const double x = U[i][0] - a[0], y = U[i][1] - a[1], u = to[i][0] - b[0], v = to[i][1] - b[1]; sc += x * u + y * v; ss += x * v - y * u; }
  const double th = std::atan2(ss, sc), c = std::cos(th), s = std::sin(th);
  for (auto& q : U) { const double x = q[0] - a[0], y = q[1] - a[1]; q = {b[0] + c * x - s * y, b[1] + s * x + c * y}; }
}
}  // namespace

UnfoldResult unfold(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, bool keepPlace, bool optimize, const Pins* pins, const SymUnfold* sym, const PackOpts* po, const std::vector<Feature>* feats) {
  // (the texture: texel-square UV in it — the shells unfolded in its space, its longer side 1; then U = X / Dw, V = Y / Dh)
  const double TM = po ? std::max(po->w, po->h) : 1, Dw = po ? po->w / TM : 1, Dh = po ? po->h / TM : 1;
  UnfoldResult R; const bool hadUV = has(m); ensure(m);
  const std::vector<char> seam = seamFlags(m, T); const size_t nc = T.fv.size();
  auto cut = [&](int e) { return T.ef1[size_t(e)] < 0 || seam[size_t(e)]; };
  // the shells cut along the seams; the corners joined across what isn't cut (one local vertex)
  Dsu fu(size_t(T.nf)), cu(nc);
  for (int e = 0; e < T.ne; e++) if (!cut(e)) {
    int f0 = T.ef0[size_t(e)], f1 = T.ef1[size_t(e)]; fu.unite(f0, f1);
    for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) { int a = cornerAt(T, f0, v), b = cornerAt(T, f1, v); if (a >= 0 && b >= 0) cu.unite(a, b); }
  }
  std::map<int, std::vector<int>> byShell; for (int f = 0; f < T.nf; f++) byShell[fu.find(f)].push_back(f);
  std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf) want.insert(fu.find(f));
  const bool all = faces.empty();
  // the symmetry: each shell's mirror — itself (lying across the plane), another shell, or none (−1); a pair's either
  // shell asked — both done, the one on +X leading, the other (follows) its mirror image after
  const bool useSym = sym && sym->mc.size() == nc;
  std::map<int, int> mirShell; std::map<int, int> follows;
  if (useSym) {
    for (auto& [root, fs] : byShell) { int to = -2;
      for (int f : fs) { const int k = sym->mc[size_t(T.fo[size_t(f)])]; const int g = k >= 0 ? fu.find(T.cf[size_t(k)]) : -1; if (to == -2) to = g; else if (to != g) to = -1; if (to < 0) break; }
      mirShell[root] = to < 0 ? -1 : to; }
    if (sym->pairs) {
      auto meanX = [&](int root) { double x = 0; int n = 0; for (int f : byShell[root]) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { x += m.pos[size_t(T.fv[size_t(k)])].x; n++; } return n ? x / n : 0.0; };
      for (auto& [root, to] : mirShell) {
        if (to < 0 || to == root || mirShell[to] != root || follows.count(root) || follows.count(to)) continue;
        if (!all && !want.count(root) && !want.count(to)) continue;
        want.insert(root); want.insert(to);
        if (meanX(root) >= meanX(to)) follows[to] = root; else follows[root] = to;
      }
    }
  }
  std::set<int> unfolded;                                       // (the leaders done: their followers mirror them)
  std::vector<char> doing(size_t(T.nf), 0);
  for (auto& [root, fs] : byShell) if (all || want.count(root)) for (int f : fs) doing[size_t(f)] = 1;
  // the density of the UV left as it is (its area to its 3D area) — the unfolded ones are made the same
  double kOther = 0; { double au = 0, a3 = 0; if (hadUV) for (int f = 0; f < T.nf; f++) if (!doing[size_t(f)]) { au += std::abs(faceArea(m, f)); a3 += faceArea3(m, f); } if (au > 0 && a3 > 0) kOther = au / a3; }
  bool packAfter = all || !keepPlace;
  for (auto& [root, fs] : byShell) {
    if (!(all || want.count(root)) || follows.count(root)) continue;
    // its local vertices, triangles, border
    std::unordered_map<int, int> loc; std::vector<Vec3> X; std::vector<int> cornerLoc; std::vector<int> corners;
    for (int f : fs) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { auto r = loc.emplace(cu.find(k), int(X.size())); if (r.second) X.push_back(m.pos[size_t(T.fv[size_t(k)])]); corners.push_back(k); cornerLoc.push_back(r.first->second); }
    std::unordered_map<int, int> cl; for (size_t i = 0; i < corners.size(); i++) cl[corners[i]] = cornerLoc[i];
    std::vector<std::array<int, 3>> tris; std::set<std::pair<int, int>> E; std::vector<char> rim(X.size(), 0); bool anyRim = false;
    double A3 = 0;
    for (int f : fs) { int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1]; A3 += faceArea3(m, f);
      { std::vector<int> li; for (int k = s; k < e; k++) li.push_back(cl[k]); addTris(m, T, f, li, tris); }
      for (int k = s; k < e; k++) { int nk = k + 1 < e ? k + 1 : s; int a = cl[k], b = cl[nk]; E.insert({std::min(a, b), std::max(a, b)});
        if (cut(T.ce[size_t(k)])) { rim[size_t(a)] = rim[size_t(b)] = 1; anyRim = true; } } }
    if (!anyRim) { R.closed++; R.closedFaces.insert(R.closedFaces.end(), fs.begin(), fs.end()); continue; }   // (closed: a seam needed)
    if (int(X.size()) - int(E.size()) + int(fs.size()) != 1) R.holes++;
    // the face's features in it (lips, lids): their ties; the triangles over a pair (both its points in one) left out of
    // the stretch — shut, they have no area; Tutte's start (a turned-over triangle) zips the pairs
    FeatureTies ft; std::vector<std::array<int, 2>> weld; std::vector<solve::Tri> trisS = tris;
    if (feats && !feats->empty()) {
      std::vector<int> vloc(m.pos.size(), -1);
      for (size_t i = 0; i < corners.size(); i++) { int& w = vloc[size_t(T.fv[size_t(corners[i])])]; const int l = cornerLoc[i]; w = w == -1 ? l : (w == l ? l : -2); }
      for (int& w : vloc) if (w < 0) w = -1;                         // (a vertex twice in the shell — along a seam: not used)
      const auto inner = innerRims(int(X.size()), tris, X); ft = featureTies(*feats, vloc, m.pos, &inner);   // (only where it's a hole of this shell)
      if (!ft.pairs.empty()) {
        std::set<std::pair<int, int>> pr; for (auto& p : ft.pairs) { weld.push_back({p[0], p[1]}); pr.insert({std::min(p[0], p[1]), std::max(p[0], p[1])}); }
        trisS.clear(); for (auto& t : tris) { bool both = false; for (int j = 0; j < 3; j++) { const int a = t[size_t(j)], b = t[size_t((j + 1) % 3)]; if (pr.count({std::min(a, b), std::max(a, b)})) both = true; } if (!both) trisS.push_back(t); }
        R.featured++; }
    }
    const bool hasFt = !ft.pairs.empty();
    std::vector<std::array<int, 2>> soft; for (auto& p : ft.pairs) { soft.push_back({p[0] * 2, p[1] * 2}); if (p[2]) soft.push_back({p[0] * 2 + 1, p[1] * 2 + 1}); } for (auto& l : ft.level) soft.push_back({l[1] * 2 + 1, l[0] * 2 + 1});   // (the same ties as dof pairs: SLIM's pull from a start that can't keep them)
    auto addTies = [&](solve::Dofs& D) { for (auto& p : ft.pairs) { D.tie(p[0] * 2, p[1] * 2); if (p[2]) D.tie(p[0] * 2 + 1, p[1] * 2 + 1); } for (auto& l : ft.level) D.tie(l[1] * 2 + 1, l[0] * 2 + 1); };
    // pinned edges in it (their UV as it is now): the shell laid round them — at the density they give (their 3D length
    // to their UV length), in place, not turned
    if (pins && !pins->empty() && hadUV) {
      std::map<int, std::array<double, 2>> fix, ptPin; double l3 = 0, l2 = 0;
      for (int f : fs) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
        const int a = T.fv[size_t(k)], b = T.fv[size_t(nk)];
        if (pins->count({a, a})) { const UV& u = m.cornerUV[size_t(k)]; fix[cl[k]] = {u[0] * Dw, u[1] * Dh}; ptPin[a] = {u[0] * Dw, u[1] * Dh}; }   // (a pinned point — the Pin brush's)
        if (!pins->count({std::min(a, b), std::max(a, b)})) continue;
        const UV& ua = m.cornerUV[size_t(k)]; const UV& ub = m.cornerUV[size_t(nk)];
        fix[cl[k]] = {ua[0] * Dw, ua[1] * Dh}; fix[cl[nk]] = {ub[0] * Dw, ub[1] * Dh};
        l3 += m.pos[size_t(a)].distanceTo(m.pos[size_t(b)]); l2 += std::hypot((double(ua[0]) - ub[0]) * Dw, (double(ua[1]) - ub[1]) * Dh); }
      if (l2 <= 1e-12 && ptPin.size() >= 2) { const auto& [v0, q0] = *ptPin.begin();   // (points alone: the scale from their distances to the first)
        for (auto& [v, q] : ptPin) if (v != v0) { l3 += m.pos[size_t(v)].distanceTo(m.pos[size_t(v0)]); l2 += std::hypot(q[0] - q0[0], q[1] - q0[1]); } }
      if (fix.size() >= 2 && l2 > 1e-12 && l3 > 0) {
        const double sk = l3 / l2;                                    // (UV → the shell's 3D scale)
        for (auto& [i, q] : fix) q = {q[0] * sk, q[1] * sk};
        std::vector<std::array<double, 2>> g(X.size()); { Vec3 N; for (int f : fs) N += newell(m, f); N = N.lengthSq() > 0 ? N.normalized() : Vec3(0, 0, 1);
          Vec3 ax = std::abs(N.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0); Vec3 bx = N.cross(ax).normalized(); ax = bx.cross(N); for (size_t i = 0; i < X.size(); i++) g[i] = {X[i].dot(ax), X[i].dot(bx)}; }
        solve::Dofs D(int(X.size())); for (auto& [i, q] : fix) D.holdPoint(i, q); addTies(D);
        auto U = solve::lscm(X, tris, D, g);
        if (optimize && !slimShell(U, X, trisS, D, 60, false)) {     // (turned over round the pins: from its old UV — it keeps them)
          std::vector<solve::P2> O(X.size()); for (size_t i = 0; i < corners.size(); i++) { const UV& q = m.cornerUV[size_t(corners[i])]; O[size_t(cornerLoc[i])] = {q[0] * Dw * sk, q[1] * Dh * sk}; }
          if (slimShell(O, X, trisS, D, 60, false)) U = std::move(O);
        }
        for (size_t i = 0; i < corners.size(); i++) { auto& q = U[size_t(cornerLoc[i])]; m.cornerUV[size_t(corners[i])] = {float(q[0] / sk / Dw), float(q[1] / sk / Dh)}; }
        R.done++; R.pinned++; unfolded.insert(root); continue;
      }
    }
    // lying across the plane: each local vertex's mirror (the seams symmetric — else unfolded as it is)
    std::vector<int> mirL;
    if (useSym && mirShell[root] == root) {
      mirL.assign(X.size(), -1); bool ok = true;
      for (size_t i = 0; i < corners.size() && ok; i++) { const int mk = sym->mc[size_t(corners[i])]; auto g = mk >= 0 ? cl.find(mk) : cl.end();
        if (g == cl.end()) { ok = false; break; } int& w = mirL[size_t(cornerLoc[i])]; if (w >= 0 && w != g->second) ok = false; w = g->second; }
      for (size_t i = 0; i < mirL.size() && ok; i++) if (mirL[i] < 0 || mirL[size_t(mirL[i])] != int(i)) ok = false;
      double far = 0; for (size_t i = 0; i < X.size() && ok; i++) far = std::max(far, std::abs(X[i].x));
      if (!ok || !(far > 0)) { mirL.clear(); R.asym++; R.asymFaces.insert(R.asymFaces.end(), fs.begin(), fs.end()); }
    }
    if (!mirL.empty()) {
      // symmetric: u of each pair opposite, v the same, the plane's points on U = 0; LSCM held only for its size (the
      // farthest point from the plane at its distance) and its height; then SLIM keeps the ties
      solve::Dofs Dt(int(X.size()));
      for (size_t i = 0; i < X.size(); i++) { const int j = mirL[i]; if (j == int(i)) Dt.tie(int(i) * 2, int(i) * 2, -1, 0); else if (int(i) < j) { Dt.tie(j * 2, int(i) * 2, -1, 0); Dt.tie(j * 2 + 1, int(i) * 2 + 1, 1, 0); } }
      const solve::Dofs DtLoose = Dt;                             // (without the face's: for a start that can't keep them)
      addTies(Dt);                                                // (the face's features too)
      int p = 0; for (size_t i = 0; i < X.size(); i++) if (std::abs(X[i].x) > std::abs(X[size_t(p)].x)) p = int(i);
      solve::Dofs Dl = Dt; Dl.hold(p * 2, std::abs(X[size_t(p)].x)); Dl.hold(p * 2 + 1, 0);
      std::vector<solve::P2> g(X.size()); for (size_t i = 0; i < X.size(); i++) g[i] = {X[i].x, X[i].y};
      auto U = solve::lscm(X, tris, Dl, g);
      if (optimize) slimShell(U, X, trisS, Dt, 60, true, &mirL, &weld, hasFt ? &DtLoose : nullptr, hasFt ? &soft : nullptr);
      double sa = 0; for (auto& t : tris) { auto &p0 = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; sa += (q[0] - p0[0]) * (r[1] - p0[1]) - (r[0] - p0[0]) * (q[1] - p0[1]); }
      if (sa < 0) for (auto& q : U) q[0] = -q[0];
      { double mv = 0, my = 0; for (size_t i = 0; i < X.size(); i++) { mv += U[i][1]; my += X[i].y; } mv /= double(X.size()); my /= double(X.size());   // (upright: its V goes as the 3D height does)
        double c = 0; for (size_t i = 0; i < X.size(); i++) c += (U[i][1] - mv) * (X[i].y - my); if (c < 0) for (auto& q : U) q = {-q[0], -q[1]}; }
      if (Dw != 1 || Dh != 1) for (auto& q : U) q = {q[0] / Dw, q[1] / Dh};   // (into the texture's UV: texel-square)
      double au = 0; for (auto& t : tris) { auto &p0 = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; au += std::abs((q[0] - p0[0]) * (r[1] - p0[1]) - (r[0] - p0[0]) * (q[1] - p0[1])) / 2; }
      double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300; for (auto& q : U) { x0 = std::min(x0, q[0]); y0 = std::min(y0, q[1]); x1 = std::max(x1, q[0]); y1 = std::max(y1, q[1]); }
      double ou0 = 1e300, ov0 = 1e300, ou1 = -1e300, ov1 = -1e300, oa = 0;
      if (hadUV) for (int f : fs) { oa += std::abs(faceArea(m, f)); for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const UV& q = m.cornerUV[size_t(k)]; ou0 = std::min(ou0, double(q[0])); ov0 = std::min(ov0, double(q[1])); ou1 = std::max(ou1, double(q[0])); ov1 = std::max(ov1, double(q[1])); } }
      const double k = oa > 0 && A3 > 0 ? oa / A3 : kOther;   // (its own size kept — else the rest's density)
      double scale = au > 0 && A3 > 0 ? std::sqrt(A3 / au) : 1, cx = 0, cy = 0;
      if (!packAfter && k > 0 && oa > 0) { scale *= std::sqrt(k); cx = (ou0 + ou1) / 2; cy = (ov0 + ov1) / 2 - (y0 + y1) / 2 * scale; }   // (its middle line where its old box's middle was)
      else { packAfter = true; cx = -x0 * scale; cy = -y0 * scale; }
      for (size_t i = 0; i < corners.size(); i++) { auto& q = U[size_t(cornerLoc[i])]; m.cornerUV[size_t(corners[i])] = {float(q[0] * scale + cx), float(q[1] * scale + cy)}; }
      R.done++; R.symmetric++; unfolded.insert(root); continue;
    }
    // the pins: the two border points farthest apart
    int b0 = -1; for (size_t i = 0; i < X.size(); i++) if (rim[i]) { b0 = int(i); break; }
    auto far = [&](int from) { int best = from; double bd = -1; for (size_t i = 0; i < X.size(); i++) if (rim[i]) { double d = (X[i] - X[size_t(from)]).lengthSq(); if (d > bd) { bd = d; best = int(i); } } return best; };
    int b1 = far(b0); b0 = far(b1); if (b0 == b1) { for (size_t i = 0; i < X.size(); i++) if (int(i) != b1) { b0 = int(i); break; } }
    // the start: the points laid on the shell's plane (its pins where the map puts them)
    Vec3 N; for (int f : fs) N += newell(m, f); N = N.lengthSq() > 0 ? N.normalized() : Vec3(0, 0, 1);
    Vec3 ax = std::abs(N.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0); Vec3 bx = N.cross(ax).normalized(); ax = bx.cross(N);
    std::vector<std::array<double, 2>> g(X.size()); for (size_t i = 0; i < X.size(); i++) g[i] = {X[i].dot(ax), X[i].dot(bx)};
    { double dx = g[size_t(b1)][0] - g[size_t(b0)][0], dy = g[size_t(b1)][1] - g[size_t(b0)][1], L = std::hypot(dx, dy), d = (X[size_t(b1)] - X[size_t(b0)]).length();
      double c = L > 0 ? dx / L : 1, s = L > 0 ? dy / L : 0, k = L > 0 ? d / L : 1; std::array<double, 2> o = g[size_t(b0)];
      for (auto& q : g) { double x = q[0] - o[0], y = q[1] - o[1]; q = {(x * c + y * s) * k, (-x * s + y * c) * k}; } }
    if (hasFt && ft.frame[0] >= 0) { b0 = ft.frame[0]; b1 = ft.frame[1]; }   // (a face's feature: its corners level — the mouth, the eye across)
    solve::Dofs D(int(X.size())); D.holdPoint(b0, {0, 0}); D.holdPoint(b1, {(X[size_t(b1)] - X[size_t(b0)]).length(), 0}); addTies(D);
    auto U = solve::lscm(X, tris, D, g);
    if (optimize) { solve::Dofs D2(int(X.size())), D2l(int(X.size())); addTies(D2); slimShell(U, X, trisS, D2, 60, true, nullptr, &weld, hasFt ? &D2l : nullptr, hasFt ? &soft : nullptr); }   // (the stretch evened out — nothing held but the ties: its place comes after)
    // turned the right way out, to its smallest box, sized to its 3D area
    double sa = 0; for (auto& t : tris) { auto &p = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; sa += (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]); }
    if (sa < 0) for (auto& q : U) q[0] = -q[0];
    if (hasFt) { double mv = 0, my = 0; for (size_t i = 0; i < X.size(); i++) { mv += U[i][1]; my += X[i].y; } mv /= double(X.size()); my /= double(X.size());   // (features: kept level — upright, not turned to a box)
      double c = 0; for (size_t i = 0; i < X.size(); i++) c += (U[i][1] - mv) * (X[i].y - my); if (c < 0) for (auto& q : U) q = {-q[0], -q[1]}; }
    else { auto H = hull(U); double bestA = 1e300, bestT = 0;
      for (size_t i = 0; i < H.size(); i++) { auto& p = H[i]; auto& q = H[(i + 1) % H.size()]; double t = std::atan2(q[1] - p[1], q[0] - p[0]), c = std::cos(t), s = std::sin(t);
        double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300; for (auto& h : H) { double x = h[0] * c + h[1] * s, y = -h[0] * s + h[1] * c; x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
        if ((x1 - x0) * (y1 - y0) < bestA) { bestA = (x1 - x0) * (y1 - y0); bestT = t; } }
      double c = std::cos(bestT), s = std::sin(bestT); for (auto& q : U) q = {q[0] * c + q[1] * s, -q[0] * s + q[1] * c}; }
    if (Dw != 1 || Dh != 1) for (auto& q : U) q = {q[0] / Dw, q[1] / Dh};   // (into the texture's UV: texel-square)
    double au = 0; for (auto& t : tris) { auto &p = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; au += std::abs((q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1])) / 2; }
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300; for (auto& q : U) { x0 = std::min(x0, q[0]); y0 = std::min(y0, q[1]); x1 = std::max(x1, q[0]); y1 = std::max(y1, q[1]); }
    // where it goes: its old UV's place at the others' density (keepPlace), else from 0, 0 (packed after)
    double ou0 = 1e300, ov0 = 1e300, ou1 = -1e300, ov1 = -1e300, oa = 0;
    if (hadUV) for (int f : fs) { oa += std::abs(faceArea(m, f)); for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const UV& q = m.cornerUV[size_t(k)]; ou0 = std::min(ou0, double(q[0])); ov0 = std::min(ov0, double(q[1])); ou1 = std::max(ou1, double(q[0])); ov1 = std::max(ov1, double(q[1])); } }
    double k = oa > 0 && A3 > 0 ? oa / A3 : kOther;   // (its own size kept — else the rest's density)
    double scale = au > 0 && A3 > 0 ? std::sqrt(A3 / au) : 1, cx = 0, cy = 0;
    if (!packAfter && k > 0 && oa > 0) { scale *= std::sqrt(k); cx = (ou0 + ou1) / 2 - (x0 + x1) / 2 * scale; cy = (ov0 + ov1) / 2 - (y0 + y1) / 2 * scale; }
    else { packAfter = true; cx = -x0 * scale; cy = -y0 * scale; }
    for (size_t i = 0; i < corners.size(); i++) { auto& q = U[size_t(cornerLoc[i])]; m.cornerUV[size_t(corners[i])] = {float(q[0] * scale + cx), float(q[1] * scale + cy)}; }
    R.done++; unfolded.insert(root);
  }
  // the followers: their leader's UV mirrored across U = axis
  for (auto& [fr, ld] : follows) {
    if (!unfolded.count(ld)) continue;
    for (int f : byShell[fr]) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int mk = sym->mc[size_t(k)]; if (mk < 0) continue;
      const UV& q = m.cornerUV[size_t(mk)]; m.cornerUV[size_t(k)] = {float(2 * sym->axis - q[0]), q[1]}; }
    R.done++; R.mirrored++;
  }
  if (R.done && packAfter) { std::vector<int> pf; if (!all) for (int r : want) for (int f : byShell[r]) pf.push_back(f);   // (a pair asked by one shell: both packed)
    PackOpts o = po ? *po : PackOpts{}; o.rotate = o.rotate && !R.pinned; if (useSym && sym->pairs) { o.mc = &sym->mc; o.axis = sym->axis; }   // (pinned shells: never turned; Symmetry: packed symmetric)
    if (!packShapes(m, T, pf, o).ok) pack(m, T, pf, 0.01, !R.pinned && !R.symmetric && !R.mirrored); }   // (by shape; the boxes if it can't)
  return R;
}

// ---------- Optimize ----------
// (as RizomUV's Optimize: the UV as it is — its shells where they lie, their size, no seams cut, nothing unfolded anew —
// by SLIM: the stretch evened out, no triangle turned over on the way; pinned points held. Turned-over triangles to
// start with: eased out first; still there — a shell with nothing pinned laid anew from Tutte's disc, fitted to where
// it lay; else left)
int optimize(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const Pins* pins, int iters, const PackOpts* po, const std::vector<Feature>* feats) {
  const double TM = po ? std::max(po->w, po->h) : 1, Dw = po ? po->w / TM : 1, Dh = po ? po->h / TM : 1;   // (worked on in the texture's space: texel-square)
  if (!has(m)) return 0;
  const Points P = points(m, T); const Shells S = shells(m, T, P);
  std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf) want.insert(S.of[size_t(f)]);
  std::vector<std::vector<int>> byShell(size_t(S.count)); for (int f = 0; f < T.nf; f++) byShell[size_t(S.of[size_t(f)])].push_back(f);
  int done = 0;
  for (int sh = 0; sh < S.count; sh++) {
    if (!faces.empty() && !want.count(sh)) continue;
    const auto& fs = byShell[size_t(sh)]; if (fs.empty()) continue;
    std::unordered_map<int, int> loc; std::vector<int> pt;           // (the shell's UV points, local)
    auto L = [&](int k) { const int p = P.of[size_t(k)]; auto r = loc.emplace(p, int(pt.size())); if (r.second) pt.push_back(p); return r.first->second; };
    std::vector<solve::Tri> tris; double au = 0, a3 = 0;
    for (int f : fs) { const int s0 = T.fo[size_t(f)], e0 = T.fo[size_t(f) + 1]; au += std::abs(faceArea(m, f)); a3 += faceArea3(m, f);
      std::vector<int> li; for (int k = s0; k < e0; k++) li.push_back(L(k)); addTris(m, T, f, li, tris); }
    if (tris.empty() || !(au > 1e-14) || !(a3 > 0)) continue;
    au *= Dw * Dh; const double sk = std::sqrt(au / a3);              // (3D → the shell's UV size, in the texture's space: kept)
    std::vector<Vec3> X(pt.size()); std::vector<std::array<double, 2>> U(pt.size());
    for (size_t i = 0; i < pt.size(); i++) { X[i] = m.pos[size_t(P.vert[size_t(pt[i])])] * sk; const UV& q = m.cornerUV[size_t(P.list[size_t(P.off[size_t(pt[i])])])]; U[i] = {q[0] * Dw, q[1] * Dh}; }
    std::vector<char> held(pt.size(), 0); bool anyHeld = false;
    if (pins && !pins->empty()) for (int f : fs) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
      const int a = T.fv[size_t(k)], b = T.fv[size_t(nk)]; if (pins->count({a, a})) { held[size_t(L(k))] = 1; anyHeld = true; }
      if (pins->count({std::min(a, b), std::max(a, b)})) { held[size_t(L(k))] = held[size_t(L(nk))] = 1; anyHeld = true; } }
    double sa = 0; for (auto& t : tris) { auto &p0 = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; sa += (q[0] - p0[0]) * (r[1] - p0[1]) - (r[0] - p0[0]) * (q[1] - p0[1]); }
    const bool mir = sa < 0; if (mir) for (auto& q : U) q[0] = -q[0];  // (laid mirrored: relaxed as it lies)
    double c0[2] = {0, 0}; for (auto& q : U) { c0[0] += q[0]; c0[1] += q[1]; }
    const std::vector<solve::P2> U0 = U;
    solve::Dofs D(int(pt.size())); for (size_t i = 0; i < pt.size(); i++) if (held[i]) D.holdPoint(int(i), U[i]);
    std::vector<solve::Tri> trisS = tris; std::vector<std::array<int, 2>> weld;   // (the face's features: their ties, as Unfold has them)
    if (feats && !feats->empty()) { std::vector<int> vloc(m.pos.size(), -1); for (size_t i = 0; i < pt.size(); i++) { int& w = vloc[size_t(P.vert[size_t(pt[i])])]; w = w == -1 ? int(i) : -2; } for (int& w : vloc) if (w < 0) w = -1;
      const auto inner = innerRims(int(X.size()), tris, X); const FeatureTies ft = featureTies(*feats, vloc, m.pos, &inner); std::set<std::pair<int, int>> pr;
      for (auto& p : ft.pairs) { D.tie(p[0] * 2, p[1] * 2); if (p[2]) D.tie(p[0] * 2 + 1, p[1] * 2 + 1); weld.push_back({p[0], p[1]}); pr.insert({std::min(p[0], p[1]), std::max(p[0], p[1])}); }
      for (auto& l : ft.level) D.tie(l[1] * 2 + 1, l[0] * 2 + 1);
      if (!pr.empty()) { trisS.clear(); for (auto& t : tris) { bool both = false; for (int j = 0; j < 3; j++) { const int a = t[size_t(j)], b = t[size_t((j + 1) % 3)]; if (pr.count({std::min(a, b), std::max(a, b)})) both = true; } if (!both) trisS.push_back(t); } } }
    if (solve::flips(U, X, trisS)) shapeRelax(U, X, trisS, 30, &held);   // (turned over: eased out first)
    if (!slimShell(U, X, trisS, D, iters, false)) {
      if (anyHeld || !slimShell(U, X, trisS, D, iters, true, nullptr, &weld)) { U = U0; continue; }   // (still turned over, pinned: left)
      fitTo(U, U0);
    }
    if (!anyHeld) { double c1[2] = {0, 0}; for (auto& q : U) { c1[0] += q[0]; c1[1] += q[1]; } const double n = double(U.size());   // (where it lay)
      for (auto& q : U) { q[0] += (c0[0] - c1[0]) / n; q[1] += (c0[1] - c1[1]) / n; } }
    if (mir) for (auto& q : U) q[0] = -q[0];
    for (int f : fs) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const auto& q = U[size_t(L(k))]; m.cornerUV[size_t(k)] = {float(q[0] / Dw), float(q[1] / Dh)}; }
    done++;
  }
  return done;
}

// ---------- Pack ----------
double pack(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, double pad, bool rotate) {
  if (!has(m)) return 0;
  const Points P = points(m, T); const Shells S = shells(m, T, P);
  std::vector<char> want(size_t(S.count), faces.empty() ? 1 : 0); for (int f : faces) if (f >= 0 && f < T.nf) want[size_t(S.of[size_t(f)])] = 1;
  struct Box { int s; std::vector<int> corners; double u0, v0, w, h; bool turn = false; double x = 0, y = 0; };
  std::vector<Box> B(size_t(S.count)); for (int s = 0; s < S.count; s++) B[size_t(s)].s = s;
  for (int f = 0; f < T.nf; f++) if (want[size_t(S.of[size_t(f)])]) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) B[size_t(S.of[size_t(f)])].corners.push_back(k);
  std::vector<Box*> L;
  for (auto& b : B) { if (b.corners.empty()) continue; double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
    for (int k : b.corners) { const UV& q = m.cornerUV[size_t(k)]; u0 = std::min(u0, double(q[0])); v0 = std::min(v0, double(q[1])); u1 = std::max(u1, double(q[0])); v1 = std::max(v1, double(q[1])); }
    b.u0 = u0; b.v0 = v0; b.w = std::max(u1 - u0, 1e-9); b.h = std::max(v1 - v0, 1e-9); if (rotate && b.h > b.w) { b.turn = true; std::swap(b.w, b.h); } L.push_back(&b); }
  if (L.empty()) return 0;
  std::sort(L.begin(), L.end(), [](const Box* a, const Box* b) { return a->h > b->h; });
  // the skyline (bottom-left): each box where its top comes lowest along the outline of those placed — each with its
  // gap on the right and on top (the left and bottom margins: the outline starts there); the largest scale that fits
  struct Seg { double x, y, w; };
  auto tryScale = [&](double s, bool place) {
    std::vector<Seg> sky{{pad, pad, 1 - pad}};
    for (Box* b : L) {
      const double bw = b->w * s + pad, bh = b->h * s + pad; if (bw > 1 - pad + 1e-12) return false;
      size_t bi = sky.size(); double by = 1e300, bx = 1e300;
      for (size_t i = 0; i < sky.size(); i++) {
        double x = sky[i].x; if (x + bw > 1 + 1e-12) break;
        double rem = bw, y = 0; size_t j = i; bool ok = true;
        while (rem > 1e-12) { if (j >= sky.size()) { ok = false; break; } y = std::max(y, sky[j].y); rem -= sky[j].w; j++; }
        if (ok && (y + bh < by + bh - 1e-12 || (std::abs(y - by) < 1e-12 && x < bx))) { by = y; bx = x; bi = i; }
      }
      if (bi == sky.size() || by + bh > 1 + 1e-12) return false;
      if (place) { b->x = bx; b->y = by; }
      const double nx0 = bx, nx1 = bx + bw; std::vector<Seg> out;
      for (auto& g : sky) { double gx0 = g.x, gx1 = g.x + g.w;
        if (gx1 <= nx0 + 1e-12 || gx0 >= nx1 - 1e-12) { out.push_back(g); continue; }
        if (gx0 < nx0) out.push_back({gx0, g.y, nx0 - gx0});
        if (gx1 > nx1) out.push_back({nx1, g.y, gx1 - nx1}); }
      out.push_back({nx0, by + bh, bw});
      std::sort(out.begin(), out.end(), [](const Seg& a, const Seg& b) { return a.x < b.x; });
      sky.clear(); for (auto& g : out) { if (!sky.empty() && std::abs(sky.back().y - g.y) < 1e-12) sky.back().w += g.w; else sky.push_back(g); }
    }
    return true;
  };  double lo = 0, hi = 1; for (Box* b : L) hi = std::max(hi, 1 / std::max(b->w, b->h)); hi *= 2;
  for (int i = 0; i < 50; i++) { double mid = (lo + hi) / 2; if (tryScale(mid, false)) lo = mid; else hi = mid; }
  if (lo <= 0) return 0;
  tryScale(lo, true);
  for (Box* b : L) for (int k : b->corners) { UV& q = m.cornerUV[size_t(k)]; double du = q[0] - b->u0, dv = q[1] - b->v0;
    if (b->turn) { double t = du; du = b->w - dv; dv = t; }                  // (a quarter turn: its box lies flat)
    q = {float(b->x + du * lo), float(b->y + dv * lo)}; }
  return coverage(m);
}

// ---------- the symmetry ----------
std::vector<int> mirrorCorners(const PolyMesh& m, const topo::TTopo& T) {
  std::vector<int> mc(T.fv.size(), -1); const auto pv = topo::symPairs(m, m.pos);
  std::map<std::vector<int>, int> byVerts;
  for (int f = 0; f < T.nf; f++) { std::vector<int> vs(T.fv.begin() + T.fo[size_t(f)], T.fv.begin() + T.fo[size_t(f) + 1]); std::sort(vs.begin(), vs.end()); byVerts.emplace(vs, f); }
  for (int f = 0; f < T.nf; f++) {
    std::vector<int> mv; bool ok = true;
    for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1] && ok; k++) { const int v = T.fv[size_t(k)], w = size_t(v) < pv.size() ? pv[size_t(v)] : -1; ok = w >= 0; mv.push_back(w); }
    if (!ok) continue; std::vector<int> sv = mv; std::sort(sv.begin(), sv.end()); auto g = byVerts.find(sv); if (g == byVerts.end()) continue;
    for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) mc[size_t(k)] = cornerAt(T, g->second, mv[size_t(k - T.fo[size_t(f)])]);
  }
  return mc;
}
std::vector<int> mirrorEdges(const PolyMesh& m, const topo::TTopo& T) {
  std::vector<int> me(size_t(T.ne), -1); const auto pv = topo::symPairs(m, m.pos);
  for (int e = 0; e < T.ne; e++) { const int a = pv[size_t(T.ea[size_t(e)])], b = pv[size_t(T.eb[size_t(e)])]; if (a >= 0 && b >= 0) me[size_t(e)] = T.edgeOf(a, b); }
  return me;
}
int symmetrize(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& mc, const std::vector<int>& from, double axis, int* skipped) {
  if (!has(m) || mc.size() != m.cornerUV.size()) return 0;
  const Points P = points(m, T); const Shells S = shells(m, T, P); const auto U0 = m.cornerUV;
  // each shell lying across the plane: its own middle — the mean of its pairs' middles as they lie
  std::vector<double> mid(size_t(S.count), 0); std::vector<int> cnt(size_t(S.count), 0);
  for (size_t k = 0; k < mc.size(); k++) { const int p = mc[k]; if (p < 0) continue; const int s = S.of[size_t(T.cf[k])];
    if (S.of[size_t(T.cf[size_t(p)])] == s) { mid[size_t(s)] += (double(U0[k][0]) + U0[size_t(p)][0]) / 2; cnt[size_t(s)]++; } }
  // each shell's mirror shell — itself or one other whose mirror it is; else (the seams differ either side, a part without a
  // mirror) −1: left as it is
  std::vector<int> to(size_t(S.count), -2);
  for (size_t k = 0; k < mc.size(); k++) { const int s = S.of[size_t(T.cf[k])]; const int g = mc[k] >= 0 ? S.of[size_t(T.cf[size_t(mc[k])])] : -1; int& x = to[size_t(s)]; if (x == -2) x = g; else if (x != g) x = -1; }
  for (int s = 0; s < S.count; s++) { int& x = to[size_t(s)]; if (x >= 0 && x != s && to[size_t(x)] != s) x = -1; }
  if (skipped) { *skipped = 0; std::set<int> sk; for (int k : from) if (k >= 0 && size_t(k) < mc.size() && to[size_t(S.of[size_t(T.cf[size_t(k)])])] < 0) sk.insert(S.of[size_t(T.cf[size_t(k)])]); *skipped = int(sk.size()); }
  std::vector<char> src(mc.size(), 0); for (int k : from) if (k >= 0 && size_t(k) < src.size()) src[size_t(k)] = 1;
  std::vector<double> sx(size_t(S.count), 0); std::vector<int> sn(size_t(S.count), 0);   // (each shell's mean X: which of a pair is on +X)
  for (size_t k = 0; k < mc.size(); k++) { const int s = S.of[size_t(T.cf[k])]; sx[size_t(s)] += m.pos[size_t(T.fv[k])].x; sn[size_t(s)]++; }
  int n = 0;
  for (int k : from) {
    if (k < 0 || size_t(k) >= mc.size()) continue; const int p = mc[size_t(k)]; if (p < 0) continue;
    const int s = S.of[size_t(T.cf[size_t(k)])], sp = S.of[size_t(T.cf[size_t(p)])]; const bool self = sp == s;
    if (to[size_t(s)] < 0) continue;
    const double a = self && cnt[size_t(s)] ? mid[size_t(s)] / cnt[size_t(s)] : axis;
    const bool onPlane = T.fv[size_t(k)] == T.fv[size_t(p)];
    if (onPlane && self) { m.cornerUV[size_t(k)][0] = float(a); n++; continue; }   // (on the plane, in a shell across it: onto its middle)
    if (src[size_t(p)]) {                                                // (both given — a hair either side of the plane: the +X one's; on it, between a pair: the +X shell's)
      if (!onPlane && !(m.pos[size_t(T.fv[size_t(k)])].x > m.pos[size_t(T.fv[size_t(p)])].x)) continue;
      if (onPlane && !(sx[size_t(s)] / std::max(1, sn[size_t(s)]) > sx[size_t(sp)] / std::max(1, sn[size_t(sp)]))) continue;
    }
    m.cornerUV[size_t(p)] = {float(2 * a - U0[size_t(k)][0]), U0[size_t(k)][1]}; n++;
  }
  return n;
}

// ---------- the points, turns, what is wrong ----------
std::vector<int> pointCorners(const Points& P, const std::vector<int>& corners) {
  std::set<int> pts; for (int k : corners) if (k >= 0 && size_t(k) < P.of.size()) pts.insert(P.of[size_t(k)]);
  std::vector<int> o; for (int p : pts) for (int i = P.off[size_t(p)]; i < P.off[size_t(p) + 1]; i++) o.push_back(P.list[size_t(i)]);
  return o;
}
std::vector<int> faceCorners(const topo::TTopo& T, const std::vector<int>& faces) {
  std::vector<int> o; for (int f : faces) if (f >= 0 && f < T.nf) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) o.push_back(k);
  return o;
}
void transform(PolyMesh& m, const std::vector<int>& corners, const std::function<UV(const UV&)>& fn) {
  if (!has(m)) return; for (int k : corners) if (k >= 0 && size_t(k) < m.cornerUV.size()) m.cornerUV[size_t(k)] = fn(m.cornerUV[size_t(k)]);
}
std::array<double, 4> box(const PolyMesh& m, const std::vector<int>& corners) {
  std::array<double, 4> b{1e300, 1e300, -1e300, -1e300}; if (!has(m)) return {0, 0, 0, 0};
  for (int k : corners) { const UV& q = m.cornerUV[size_t(k)]; b[0] = std::min(b[0], double(q[0])); b[1] = std::min(b[1], double(q[1])); b[2] = std::max(b[2], double(q[0])); b[3] = std::max(b[3], double(q[1])); }
  return b;
}
std::vector<int> flipped(const PolyMesh& m, const topo::TTopo& T, const Shells& S) {
  std::vector<int> o; if (!has(m)) return o;
  std::vector<double> a(size_t(T.nf)), sum(size_t(S.count), 0);
  for (int f = 0; f < T.nf; f++) { a[size_t(f)] = faceArea(m, f); sum[size_t(S.of[size_t(f)])] += a[size_t(f)]; }
  for (int f = 0; f < T.nf; f++) { double s = sum[size_t(S.of[size_t(f)])]; if (std::abs(a[size_t(f)]) > 1e-12 && a[size_t(f)] * s < 0) o.push_back(f); }
  return o;
}
double coverage(const PolyMesh& m) { if (!has(m)) return 0; double a = 0; for (int f = 0; f < m.faceCount(); f++) a += std::abs(faceArea(m, f)); return a; }

// ---------- Preserve UVs ----------
std::vector<UV> preserve(const PolyMesh& orig, const topo::TTopo& T, const std::vector<Vec3>& P, const std::vector<int>& moved) {
  std::vector<UV> out = orig.cornerUV; if (!has(orig) || moved.empty()) return out;
  const Points pts = points(orig, T);
  // (the faces across an unbroken edge from a face: the second ring in the same shell)
  auto neighbours = [&](int f, std::set<int>& into) {
    for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { int e = T.ce[size_t(k)]; if (!continuous(orig, T, pts, e)) continue;
      into.insert(T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]); }
  };
  std::set<int> seen;
  for (int v : moved) {
    if (v < 0 || v >= T.n || !seen.insert(v).second) continue;
    const Vec3& p = P[size_t(v)];
    std::set<int> donePts;
    for (int i = T.vfo[size_t(v)]; i < T.vfo[size_t(v) + 1]; i++) {
      int f0 = T.vF[size_t(i)]; int c0 = cornerAt(T, f0, v); if (c0 < 0) continue; int pt = pts.of[size_t(c0)]; if (!donePts.insert(pt).second) continue;
      std::set<int> ring; for (int j = pts.off[size_t(pt)]; j < pts.off[size_t(pt) + 1]; j++) ring.insert(T.cf[size_t(pts.list[size_t(j)])]);
      std::set<int> cand = ring; for (int f : ring) neighbours(f, cand);
      double bd = 1e300; UV best = orig.cornerUV[size_t(c0)];
      for (int f : cand) { int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
        for (int k = s + 1; k + 1 < e; k++) {
          const int ka = s, kb = k, kc = k + 1;
          const Vec3 &a = orig.pos[size_t(T.fv[size_t(ka)])], &b = orig.pos[size_t(T.fv[size_t(kb)])], &c = orig.pos[size_t(T.fv[size_t(kc)])];
          auto w = closestBary(p, a, b, c); Vec3 q = a * w[0] + b * w[1] + c * w[2]; double d = (q - p).lengthSq();
          if (d < bd) { bd = d; const UV &ua = orig.cornerUV[size_t(ka)], &ub = orig.cornerUV[size_t(kb)], &uc = orig.cornerUV[size_t(kc)];
            best = {float(ua[0] * w[0] + ub[0] * w[1] + uc[0] * w[2]), float(ua[1] * w[0] + ub[1] * w[1] + uc[1] * w[2])}; } } }
      for (int j = pts.off[size_t(pt)]; j < pts.off[size_t(pt) + 1]; j++) out[size_t(pts.list[size_t(j)])] = best;
    }
  }
  return out;
}


// ---------- Straighten: a hair strand's shell unbent ----------
// The strand's middle line (from one far end to the other through the UV: the mean of its points in slices along the
// distance from the end, smoothed) gets a target with `amount` of its bends gone (the same piece lengths). Then, from the
// shell as it is (no kinks to start with), as-rigid-as-possible steps: every triangle keeps its shape (as the UV had it — no
// polygon over another to start from) while the body's slices are drawn, each as a whole, towards their places on the target line — only moved,
// never turned by force, so the sections and a fork's arms turn as the shape asks. An outline crossing itself:
// again, the bend kept about the fork whose arm lay down on the body, else less unbent; turned-over polygons eased out.
int straighten(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, double amount, bool upright) {
  if (!has(m)) return 0;
  const Points P = points(m, T); const Shells S = shells(m, T, P);
  std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf) want.insert(S.of[size_t(f)]);
  amount = std::clamp(amount, 0.0, 1.0); int done = 0;
  auto at = [&](int p) { const auto& u = m.cornerUV[size_t(P.list[size_t(P.off[size_t(p)])])]; return std::array<double, 2>{u[0], u[1]}; };
  using V2 = std::array<double, 2>;
  for (int s = 0; s < S.count; s++) {
    if (!faces.empty() && !want.count(s)) continue;
    // the shell's points, their links (the polygons' sides, in UV), its polygons as point loops
    std::vector<int> pts; std::unordered_map<int, int> idx; std::vector<std::vector<std::pair<int, double>>> adj; std::vector<std::vector<int>> loops;
    auto id = [&](int p) { auto x = idx.find(p); if (x != idx.end()) return x->second; const int i = int(pts.size()); idx[p] = i; pts.push_back(p); adj.emplace_back(); return i; };
    for (int f = 0; f < T.nf; f++) if (S.of[size_t(f)] == s) { std::vector<int> L0;
      for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)]; const int a = id(P.of[size_t(k)]), b = id(P.of[size_t(nk)]); L0.push_back(a);
        const auto A = at(pts[size_t(a)]), B = at(pts[size_t(b)]); const double l = std::hypot(A[0] - B[0], A[1] - B[1]); adj[size_t(a)].push_back({b, l}); adj[size_t(b)].push_back({a, l}); }
      loops.push_back(L0); }
    const int n = int(pts.size()); if (n < 6) continue;
    std::vector<V2> orig(static_cast<size_t>(n)); for (int i = 0; i < n; i++) orig[size_t(i)] = at(pts[size_t(i)]);
    auto dijkstra = [&](int src) { std::vector<double> d(static_cast<size_t>(n), 1e300); using Q = std::pair<double, int>; std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q; d[size_t(src)] = 0; q.push({0, src});
      while (!q.empty()) { auto [dd, u] = q.top(); q.pop(); if (dd > d[size_t(u)]) continue; for (auto [v, w] : adj[size_t(u)]) if (dd + w < d[size_t(v)]) { d[size_t(v)] = dd + w; q.push({d[size_t(v)], v}); } }
      return d; };
    auto far = [&](const std::vector<double>& d) { int b = 0; for (int i = 0; i < n; i++) if (d[size_t(i)] < 1e299 && d[size_t(i)] > d[size_t(b)]) b = i; return b; };
    const int a0 = far(dijkstra(0)); const auto dA = dijkstra(a0); const double L = dA[size_t(far(dA))]; if (L <= 0) continue;
    // (the way between the two ends — the shortest; a point far from it, a fork's arm, left out of the middle line)
    std::vector<char> trunk(static_cast<size_t>(n), 1);
    { std::vector<double> d(static_cast<size_t>(n), 1e300); std::vector<int> pr(static_cast<size_t>(n), -1); using Q = std::pair<double, int>; std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
      d[size_t(a0)] = 0; q.push({0, a0});
      while (!q.empty()) { auto [dd, u] = q.top(); q.pop(); if (dd > d[size_t(u)]) continue; for (auto [v, w] : adj[size_t(u)]) if (dd + w < d[size_t(v)]) { d[size_t(v)] = dd + w; pr[size_t(v)] = u; q.push({d[size_t(v)], v}); } }
      std::vector<double> dp(static_cast<size_t>(n), 1e300); std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q2;
      for (int v = far(dA); v >= 0; v = pr[size_t(v)]) { dp[size_t(v)] = 0; q2.push({0, v}); }
      while (!q2.empty()) { auto [dd, u] = q2.top(); q2.pop(); if (dd > dp[size_t(u)]) continue; for (auto [v, w] : adj[size_t(u)]) if (dd + w < dp[size_t(v)]) { dp[size_t(v)] = dd + w; q2.push({dp[size_t(v)], v}); } }
      std::vector<double> s2; for (double x : dp) if (x < 1e299) s2.push_back(x);
      if (!s2.empty()) { std::nth_element(s2.begin(), s2.begin() + long(s2.size() / 2), s2.end()); const double lim = std::max(1e-9, s2[s2.size() / 2] * 3);
        for (int i = 0; i < n; i++) trunk[size_t(i)] = dp[size_t(i)] <= lim; } }
    // the middle line: the points' mean in slices along the distance from the end, smoothed
    double el = 0; int ne = 0; for (auto& v : adj) for (auto& [b, w] : v) { el += w; ne++; } el = ne ? el / ne : L / 20;
    const int N = std::clamp(int(L / std::max(el, 1e-9)), 6, 200);
    auto sliceOf = [&](int i) { return dA[size_t(i)] > 1e299 ? 0 : std::clamp(int(std::lround(dA[size_t(i)] / L * N)), 0, N); };
    std::vector<V2> C(static_cast<size_t>(N + 1), V2{0, 0}); std::vector<double> cw(static_cast<size_t>(N + 1), 0);
    for (int i = 0; i < n; i++) { if (!trunk[size_t(i)]) continue; const int k = sliceOf(i); C[size_t(k)][0] += orig[size_t(i)][0]; C[size_t(k)][1] += orig[size_t(i)][1]; cw[size_t(k)] += 1; }
    { int last = -1; for (int k = 0; k <= N; k++) if (cw[size_t(k)] > 0) { C[size_t(k)][0] /= cw[size_t(k)]; C[size_t(k)][1] /= cw[size_t(k)];
        if (last >= 0) for (int j = last + 1; j < k; j++) { const double t = double(j - last) / (k - last); for (int c = 0; c < 2; c++) C[size_t(j)][c] = C[size_t(last)][c] + (C[size_t(k)][c] - C[size_t(last)][c]) * t; }
        else for (int j = 0; j < k; j++) C[size_t(j)] = C[size_t(k)];
        last = k; }
      for (int j = last + 1; j <= N && last >= 0; j++) C[size_t(j)] = C[size_t(last)]; }
    for (int it = 0; it < 6; it++) { auto D = C; for (int k = 1; k < N; k++) for (int c = 0; c < 2; c++) D[size_t(k)][c] = (C[size_t(k - 1)][c] + 2 * C[size_t(k)][c] + C[size_t(k + 1)][c]) / 4; C = D; }
    // the target line: the same pieces, each bend between them `amount` smaller
    std::vector<double> len(static_cast<size_t>(N)), ang(static_cast<size_t>(N));
    for (int k = 0; k < N; k++) { const double dx = C[size_t(k + 1)][0] - C[size_t(k)][0], dy = C[size_t(k + 1)][1] - C[size_t(k)][1]; len[size_t(k)] = std::hypot(dx, dy); ang[size_t(k)] = std::atan2(dy, dx); }
    for (int k = 1; k < N; k++) if (len[size_t(k)] < 1e-12) ang[size_t(k)] = ang[size_t(k - 1)];
    std::vector<double> ang2(static_cast<size_t>(N)); ang2[0] = ang[0];
    for (int k = 1; k < N; k++) { double d = ang[size_t(k)] - ang[size_t(k - 1)]; while (d > M_PI) d -= 2 * M_PI; while (d < -M_PI) d += 2 * M_PI; ang2[size_t(k)] = ang2[size_t(k - 1)] + d * (1 - amount); }
    std::vector<V2> C2(static_cast<size_t>(N + 1)); C2[0] = C[0];
    for (int k = 0; k < N; k++) C2[size_t(k + 1)] = {C2[size_t(k)][0] + std::cos(ang2[size_t(k)]) * len[size_t(k)], C2[size_t(k)][1] + std::sin(ang2[size_t(k)]) * len[size_t(k)]};
    { V2 a{0, 0}, b{0, 0}; for (int k = 0; k <= N; k++) for (int c = 0; c < 2; c++) { a[c] += C[size_t(k)][c]; b[c] += C2[size_t(k)][c]; }   // (the target over the line's middle)
      for (auto& q : C2) for (int c = 0; c < 2; c++) q[c] += (a[c] - b[c]) / (N + 1); }
    // the body: the points near the line (within twice the median distance from it); a fork's arm, further out — free
    std::vector<double> off(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) { const int k = sliceOf(i); off[size_t(i)] = std::hypot(orig[size_t(i)][0] - C[size_t(k)][0], orig[size_t(i)][1] - C[size_t(k)][1]); }
    std::vector<double> so = off; std::nth_element(so.begin(), so.begin() + long(so.size() / 2), so.end()); const double hw = std::max(1e-9, so[so.size() / 2] * 2);
    std::vector<char> body(static_cast<size_t>(n)); std::vector<int> slice(static_cast<size_t>(n)); for (int i = 0; i < n; i++) { body[size_t(i)] = off[size_t(i)] <= hw; slice[size_t(i)] = sliceOf(i); }
    // (each point's place along the line: the nearest on it, near where its distance from the end says)
    std::vector<double> px(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) { const auto& u = orig[size_t(i)]; const double x0 = dA[size_t(i)] > 1e299 ? 0 : dA[size_t(i)] / L * N; double x = x0, bd = 1e300;
      for (int k = std::max(0, int(x0) - 4); k <= std::min(N - 1, int(x0) + 4); k++) { const double ax = C[size_t(k)][0], ay = C[size_t(k)][1], vx = C[size_t(k + 1)][0] - ax, vy = C[size_t(k + 1)][1] - ay, l2 = vx * vx + vy * vy;
        const double t = l2 > 0 ? std::clamp(((u[0] - ax) * vx + (u[1] - ay) * vy) / l2, 0.0, 1.0) : 0; const double d = std::hypot(ax + vx * t - u[0], ay + vy * t - u[1]); if (d < bd) { bd = d; x = k + t; } }
      px[size_t(i)] = x; slice[size_t(i)] = std::clamp(int(std::lround(x)), 0, N); }
    // (the pull fades near a fork's arm: the body there left to take the shape the arm asks)
    std::vector<double> nearArm(static_cast<size_t>(n), 1e300);
    { using Q = std::pair<double, int>; std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q; for (int i = 0; i < n; i++) if (!body[size_t(i)]) { nearArm[size_t(i)] = 0; q.push({0, i}); }
      while (!q.empty()) { auto [d0, u0] = q.top(); q.pop(); if (d0 > nearArm[size_t(u0)]) continue; for (auto [v0, w] : adj[size_t(u0)]) if (d0 + w < nearArm[size_t(v0)]) { nearArm[size_t(v0)] = d0 + w; q.push({nearArm[size_t(v0)], v0}); } } }
    // forks: the arms (big enough, going out well past the body), where each grows along the line, its points
    struct Arm { std::vector<int> mem; std::set<int> att; double xf = 0; };
    std::vector<Arm> arms;
    { std::vector<int> comp(static_cast<size_t>(n), -1); int nc = 0;
      for (int i = 0; i < n; i++) { if (body[size_t(i)] || comp[size_t(i)] >= 0) continue; std::vector<int> st{i}; comp[size_t(i)] = nc;
        while (!st.empty()) { const int u0 = st.back(); st.pop_back(); for (auto [v0, w] : adj[size_t(u0)]) if (!body[size_t(v0)] && comp[size_t(v0)] < 0) { comp[size_t(v0)] = nc; st.push_back(v0); } }
        nc++; }
      for (int c = 0; c < nc; c++) { Arm a; double mo = 0; std::vector<double> xs;
        for (int i = 0; i < n; i++) if (comp[size_t(i)] == c) { a.mem.push_back(i); mo = std::max(mo, off[size_t(i)]); for (auto [v0, w] : adj[size_t(i)]) if (body[size_t(v0)]) { a.att.insert(v0); xs.push_back(px[size_t(v0)]); } }
        if (a.mem.size() < 3 || xs.empty()) continue;
        std::nth_element(xs.begin(), xs.begin() + long(xs.size() / 2), xs.end()); a.xf = xs[xs.size() / 2]; (void)mo; arms.push_back(std::move(a)); } }
    // the target line, the bends kept about the forks asked (those whose arm lay down on the body)
    auto buildTarget = [&](const std::set<int>& kept, double amt) {
      std::vector<double> keep(static_cast<size_t>(N), 0.0);
      for (int ai : kept) { const double xf = arms[size_t(ai)].xf; for (int k = 0; k < N; k++) { const double d = std::abs(k + 0.5 - xf); const double g = d <= 1.5 ? 1 : d >= 4 ? 0 : (4 - d) / 2.5; keep[size_t(k)] = std::max(keep[size_t(k)], g); } }
      std::vector<double> a2(static_cast<size_t>(N)); a2[0] = ang[0];
      for (int k = 1; k < N; k++) { double d = ang[size_t(k)] - ang[size_t(k - 1)]; while (d > M_PI) d -= 2 * M_PI; while (d < -M_PI) d += 2 * M_PI;
        const double kp = std::max(keep[size_t(k)], keep[size_t(k - 1)]); a2[size_t(k)] = a2[size_t(k - 1)] + d * (1 - amt * (1 - kp)); }
      C2[0] = C[0]; for (int k = 0; k < N; k++) C2[size_t(k + 1)] = {C2[size_t(k)][0] + std::cos(a2[size_t(k)]) * len[size_t(k)], C2[size_t(k)][1] + std::sin(a2[size_t(k)]) * len[size_t(k)]};
      V2 a{0, 0}, b{0, 0}; for (int k = 0; k <= N; k++) for (int c = 0; c < 2; c++) { a[c] += C[size_t(k)][c]; b[c] += C2[size_t(k)][c]; }
      for (auto& q : C2) for (int c = 0; c < 2; c++) q[c] += (a[c] - b[c]) / (N + 1); };
    // the triangles: their rest shapes
    struct Tri { int v[3]; double x[3][2]; };
    std::vector<Tri> tris; double a2 = 0, a3 = 0;
    for (int f = 0; f < T.nf; f++) if (S.of[size_t(f)] == s) { const int s0 = T.fo[size_t(f)], e0 = T.fo[size_t(f) + 1];
      for (int k = s0 + 1; k + 1 < e0; k++) { Tri t; const int c[3] = {s0, k, k + 1}; Vec3 q[3];
        for (int j = 0; j < 3; j++) { t.v[j] = idx[P.of[size_t(c[j])]]; q[j] = m.pos[size_t(T.fv[size_t(c[j])])]; }
        const Vec3 e1 = q[1] - q[0], e2 = q[2] - q[0]; const double l1 = e1.length(); if (l1 <= 0) continue;
        const Vec3 ex = e1 * (1.0 / l1); const double x2 = e2.dot(ex), y2 = (e2 - ex * x2).length(); if (y2 <= 1e-12 * l1) continue;
        const auto& oA = orig[size_t(t.v[0])]; const auto& oB = orig[size_t(t.v[1])]; const auto& oC = orig[size_t(t.v[2])];
        const double ao = (oB[0] - oA[0]) * (oC[1] - oA[1]) - (oC[0] - oA[0]) * (oB[1] - oA[1]);
        // (its rest shape: as the UV had it — no polygon over another to start from; 0 — the UV as it was)
        for (int j = 0; j < 3; j++) { t.x[j][0] = orig[size_t(t.v[j])][0] - oA[0]; t.x[j][1] = orig[size_t(t.v[j])][1] - oA[1]; }
        (void)x2; (void)y2; tris.push_back(t); } }
    if (tris.empty()) continue; (void)a2; (void)a3;
    std::vector<std::vector<std::pair<int, int>>> inTri(static_cast<size_t>(n)); for (size_t ti = 0; ti < tris.size(); ti++) for (int j = 0; j < 3; j++) inTri[size_t(tris[ti].v[j])].push_back({int(ti), j});
    std::vector<double> rc(tris.size(), 1), rs(tris.size(), 0);
    std::vector<V2> out = orig;
    // as rigid as possible, the body's slices drawn towards the target (the pull growing over the first steps)
    const double lam = 0.3; const int steps = 400;
    std::vector<V2> cen(static_cast<size_t>(N + 1)); std::vector<double> cnt(static_cast<size_t>(N + 1));
    // (the shell's outline: the sides of one polygon only — for its crossings with itself)
    std::vector<std::array<int, 2>> rim; { std::map<std::pair<int, int>, int> ec; for (auto& L0 : loops) for (size_t k = 0; k < L0.size(); k++) { const int a = L0[k], b = L0[(k + 1) % L0.size()]; ec[{std::min(a, b), std::max(a, b)}]++; }
      for (auto& [e, c] : ec) if (c == 1) rim.push_back({e.first, e.second}); }
    auto crossings = [&] { int c = 0; auto cr = [](const V2& a, const V2& b, const V2& q) { return (b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0]); };
      for (size_t i = 0; i < rim.size(); i++) for (size_t j = i + 1; j < rim.size(); j++) { const auto& E = rim[i]; const auto& F = rim[j]; if (E[0] == F[0] || E[0] == F[1] || E[1] == F[0] || E[1] == F[1]) continue;
          const V2& p0 = out[size_t(E[0])]; const V2& p1 = out[size_t(E[1])]; const V2& q0 = out[size_t(F[0])]; const V2& q1 = out[size_t(F[1])];
          if ((cr(p0, p1, q0) > 0) != (cr(p0, p1, q1) > 0) && (cr(q0, q1, p0) > 0) != (cr(q0, q1, p1) > 0)) c++; }
      return c; };
    // (the slices' own middles as they were: a slice is moved by how much the line moved there — none at 0)
    std::vector<V2> cen0(static_cast<size_t>(N + 1), V2{0, 0}); { std::vector<double> c0n(static_cast<size_t>(N + 1), 0);
      for (int i = 0; i < n; i++) if (body[size_t(i)]) { const int k = slice[size_t(i)]; cen0[size_t(k)][0] += orig[size_t(i)][0]; cen0[size_t(k)][1] += orig[size_t(i)][1]; c0n[size_t(k)] += 1; }
      for (int k = 0; k <= N; k++) if (c0n[size_t(k)] > 0) { cen0[size_t(k)][0] /= c0n[size_t(k)]; cen0[size_t(k)][1] /= c0n[size_t(k)]; } }
    std::set<int> kept; double amt = amount;
    for (int attempt = 0; attempt < 7; attempt++) {
    buildTarget(kept, amt); out = orig;
    for (int it = 0; it < steps; it++) {
      for (size_t ti = 0; ti < tris.size(); ti++) { const Tri& t = tris[ti]; double s00 = 0, s01 = 0, s10 = 0, s11 = 0;
        for (int j = 0; j < 3; j++) { const int a = j, b = (j + 1) % 3; const double ux = out[size_t(t.v[b])][0] - out[size_t(t.v[a])][0], uy = out[size_t(t.v[b])][1] - out[size_t(t.v[a])][1];
          const double xx = t.x[b][0] - t.x[a][0], xy = t.x[b][1] - t.x[a][1]; s00 += ux * xx; s01 += ux * xy; s10 += uy * xx; s11 += uy * xy; }
        const double an = std::atan2(s10 - s01, s00 + s11); rc[ti] = std::cos(an); rs[ti] = std::sin(an); }
      std::fill(cen.begin(), cen.end(), V2{0, 0}); std::fill(cnt.begin(), cnt.end(), 0.0);
      for (int i = 0; i < n; i++) if (body[size_t(i)]) { const int k = slice[size_t(i)]; cen[size_t(k)][0] += out[size_t(i)][0]; cen[size_t(k)][1] += out[size_t(i)][1]; cnt[size_t(k)] += 1; }
      for (int k = 0; k <= N; k++) if (cnt[size_t(k)] > 0) { cen[size_t(k)][0] /= cnt[size_t(k)]; cen[size_t(k)][1] /= cnt[size_t(k)]; }
      // (the slices' pull: their way to the target, smoothed along the line, read at each point's own place on it)
      std::vector<V2> dsp(static_cast<size_t>(N + 1), V2{0, 0}); for (int k = 0; k <= N; k++) if (cnt[size_t(k)] > 0) dsp[size_t(k)] = {cen0[size_t(k)][0] + C2[size_t(k)][0] - C[size_t(k)][0] - cen[size_t(k)][0], cen0[size_t(k)][1] + C2[size_t(k)][1] - C[size_t(k)][1] - cen[size_t(k)][1]};
      for (int k = 0; k <= N; k++) if (cnt[size_t(k)] <= 0) { int a = k, b = k; while (a >= 0 && cnt[size_t(a)] <= 0) a--; while (b <= N && cnt[size_t(b)] <= 0) b++; if (a >= 0 && b <= N) for (int c = 0; c < 2; c++) dsp[size_t(k)][c] = (dsp[size_t(a)][c] + dsp[size_t(b)][c]) / 2; else if (a >= 0) dsp[size_t(k)] = dsp[size_t(a)]; else if (b <= N) dsp[size_t(k)] = dsp[size_t(b)]; }
      for (int pass = 0; pass < 4; pass++) { auto D = dsp; for (int k = 1; k < N; k++) for (int c = 0; c < 2; c++) D[size_t(k)][c] = (dsp[size_t(k - 1)][c] + 2 * dsp[size_t(k)][c] + dsp[size_t(k + 1)][c]) / 4; dsp = D; }
      const double w0 = lam * std::min(1.0, (it + 1) / 60.0);
      for (int i = 0; i < n; i++) { if (inTri[size_t(i)].empty()) continue; double sx = 0, sy = 0, w = 0;
        if (body[size_t(i)]) { const double x = std::clamp(px[size_t(i)], 0.0, double(N)); const int k = std::min(int(x), N - 1); const double t = x - k;
          const double ddx = dsp[size_t(k)][0] + (dsp[size_t(k + 1)][0] - dsp[size_t(k)][0]) * t, ddy = dsp[size_t(k)][1] + (dsp[size_t(k + 1)][1] - dsp[size_t(k)][1]) * t;
          const double wi = w0 * std::clamp(nearArm[size_t(i)] / (2 * hw), 0.0, 1.0); sx = wi * (out[size_t(i)][0] + ddx); sy = wi * (out[size_t(i)][1] + ddy); w = wi; }
        for (auto [ti, j] : inTri[size_t(i)]) { const Tri& t = tris[size_t(ti)];
          for (int o = 1; o < 3; o++) { const int jj = (j + o) % 3; const double dx = t.x[j][0] - t.x[jj][0], dy = t.x[j][1] - t.x[jj][1];
            sx += out[size_t(t.v[jj])][0] + rc[size_t(ti)] * dx - rs[size_t(ti)] * dy; sy += out[size_t(t.v[jj])][1] + rs[size_t(ti)] * dx + rc[size_t(ti)] * dy; w += 1; } }
        if (w > 0) out[size_t(i)] = {sx / w, sy / w}; }
    }
      // (the outline crossing itself: once more — the bend kept about a fork whose arm lay down on the body, else less unbent)
      if (crossings() == 0 || attempt == 6) break;
      auto inPolyAt = [&](const std::vector<int>& L0, double x, double y) { bool in = false; for (size_t a = 0, b = L0.size() - 1; a < L0.size(); b = a++) { const auto& A = out[size_t(L0[a])]; const auto& B = out[size_t(L0[b])];
          if ((A[1] > y) != (B[1] > y) && x < (B[0] - A[0]) * (y - A[1]) / (B[1] - A[1]) + A[0]) in = !in; } return in; };
      std::set<int> bad;
      for (size_t ai = 0; ai < arms.size(); ai++) { const Arm& A = arms[ai]; std::set<int> mem(A.mem.begin(), A.mem.end()); bool hit = false;
        for (auto& L0 : loops) { if (hit) break; bool allArm = true, allBody = true; for (int p : L0) { if (!mem.count(p)) allArm = false; if (!body[size_t(p)]) allBody = false; }
          if (allBody) { for (int i : A.mem) if (inPolyAt(L0, out[size_t(i)][0], out[size_t(i)][1])) { hit = true; break; } }
          else if (allArm) { for (int b = 0; b < n && !hit; b++) if (body[size_t(b)] && !A.att.count(b) && inPolyAt(L0, out[size_t(b)][0], out[size_t(b)][1])) hit = true; } }
        if (hit) bad.insert(int(ai)); }
      bool more = false; for (int b : bad) more |= kept.insert(b).second;
      if (!more) amt *= 0.65;
    }
    // (turned-over polygons: their points eased towards their neighbours till none is left)
    { auto area = [&](const std::vector<int>& L0) { double a = 0; for (size_t k = 0; k < L0.size(); k++) { const auto& p = out[size_t(L0[k])]; const auto& q = out[size_t(L0[(k + 1) % L0.size()])]; a += p[0] * q[1] - q[0] * p[1]; } return a; };
      double tot = 0; for (auto& L0 : loops) tot += area(L0); const double sg = tot >= 0 ? 1 : -1;
      for (int it = 0; it < 40; it++) { std::set<int> bad; for (auto& L0 : loops) if (area(L0) * sg <= 0) bad.insert(L0.begin(), L0.end()); if (bad.empty()) break;
        for (int p : bad) { double ax = 0, ay = 0; int c = 0; for (auto [q, w] : adj[size_t(p)]) { ax += out[size_t(q)][0]; ay += out[size_t(q)][1]; c++; } if (c) out[size_t(p)] = {ax / c, ay / c}; } } }
    // upright: the line's ends one above the other (the end that was higher on top); back in its old middle
    double c0[2] = {0, 0}, c1[2] = {0, 0}; for (int i = 0; i < n; i++) for (int c = 0; c < 2; c++) { c0[c] += orig[size_t(i)][c]; c1[c] += out[size_t(i)][c]; }
    for (int c = 0; c < 2; c++) { c0[c] /= n; c1[c] /= n; }
    double turn = 0;
    if (upright) { V2 e0{0, 0}, e1{0, 0}; int n0 = 0, n1 = 0;   // (the ends as they are now: the body's first and last slices)
      for (int i = 0; i < n; i++) if (body[size_t(i)]) { if (slice[size_t(i)] <= 1) { e0[0] += out[size_t(i)][0]; e0[1] += out[size_t(i)][1]; n0++; } if (slice[size_t(i)] >= N - 1) { e1[0] += out[size_t(i)][0]; e1[1] += out[size_t(i)][1]; n1++; } }
      if (n0 && n1) { const double ex = e1[0] / n1 - e0[0] / n0, ey = e1[1] / n1 - e0[1] / n0; const bool up = C[size_t(N)][1] >= C[0][1]; turn = (up ? M_PI / 2 : -M_PI / 2) - std::atan2(ey, ex); } }
    const double cs = std::cos(turn), sn = std::sin(turn);
    for (int i = 0; i < n; i++) { const double x = out[size_t(i)][0] - c1[0], y = out[size_t(i)][1] - c1[1]; const float U = float(c0[0] + x * cs - y * sn), Vv = float(c0[1] + x * sn + y * cs);
      for (int j = P.off[size_t(pts[size_t(i)])]; j < P.off[size_t(pts[size_t(i)]) + 1]; j++) m.cornerUV[size_t(P.list[size_t(j)])] = {U, Vv}; }
    done++;
  }
  return done;
}


// ---------- Rectangle: a hair strand's shell laid out as a strip ----------
// Its middle line as in Straighten; each point placed by how far along the line it is (V) and how far across, the
// body's width there made the strand's mean width (U): the outline a strip of one width, the length kept; a fork's arm
// sticks out as far as it went. Turned-over polygons eased out; in its old middle, upright (or upside down as it lay).
int rectangle(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces) {
  if (!has(m)) return 0;
  const Points P = points(m, T); const Shells S = shells(m, T, P);
  std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf) want.insert(S.of[size_t(f)]);
  int done = 0; using V2 = std::array<double, 2>;
  auto at = [&](int p) { const auto& u = m.cornerUV[size_t(P.list[size_t(P.off[size_t(p)])])]; return V2{u[0], u[1]}; };
  for (int s = 0; s < S.count; s++) {
    if (!faces.empty() && !want.count(s)) continue;
    std::vector<int> pts; std::unordered_map<int, int> idx; std::vector<std::vector<std::pair<int, double>>> adj; std::vector<std::vector<int>> loops;
    auto id = [&](int p) { auto x = idx.find(p); if (x != idx.end()) return x->second; const int i = int(pts.size()); idx[p] = i; pts.push_back(p); adj.emplace_back(); return i; };
    for (int f = 0; f < T.nf; f++) if (S.of[size_t(f)] == s) { std::vector<int> L0;
      for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)]; const int a = id(P.of[size_t(k)]), b = id(P.of[size_t(nk)]); L0.push_back(a);
        const auto A = at(pts[size_t(a)]), B = at(pts[size_t(b)]); const double l = std::hypot(A[0] - B[0], A[1] - B[1]); adj[size_t(a)].push_back({b, l}); adj[size_t(b)].push_back({a, l}); }
      loops.push_back(L0); }
    const int n = int(pts.size()); if (n < 6) continue;
    std::vector<V2> orig(static_cast<size_t>(n)); for (int i = 0; i < n; i++) orig[size_t(i)] = at(pts[size_t(i)]);
    auto dijkstra = [&](int src) { std::vector<double> d(static_cast<size_t>(n), 1e300); using Q = std::pair<double, int>; std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q; d[size_t(src)] = 0; q.push({0, src});
      while (!q.empty()) { auto [dd, u] = q.top(); q.pop(); if (dd > d[size_t(u)]) continue; for (auto [v, w] : adj[size_t(u)]) if (dd + w < d[size_t(v)]) { d[size_t(v)] = dd + w; q.push({d[size_t(v)], v}); } }
      return d; };
    auto far = [&](const std::vector<double>& d) { int b = 0; for (int i = 0; i < n; i++) if (d[size_t(i)] < 1e299 && d[size_t(i)] > d[size_t(b)]) b = i; return b; };
    const int a0 = far(dijkstra(0)); const auto dA = dijkstra(a0); const double L = dA[size_t(far(dA))]; if (L <= 0) continue;
    // (the way between the two ends — the shortest; a point far from it, a fork's arm, left out of the middle line)
    std::vector<char> trunk(static_cast<size_t>(n), 1);
    { std::vector<double> d(static_cast<size_t>(n), 1e300); std::vector<int> pr(static_cast<size_t>(n), -1); using Q = std::pair<double, int>; std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
      d[size_t(a0)] = 0; q.push({0, a0});
      while (!q.empty()) { auto [dd, u] = q.top(); q.pop(); if (dd > d[size_t(u)]) continue; for (auto [v, w] : adj[size_t(u)]) if (dd + w < d[size_t(v)]) { d[size_t(v)] = dd + w; pr[size_t(v)] = u; q.push({d[size_t(v)], v}); } }
      std::vector<double> dp(static_cast<size_t>(n), 1e300); std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q2;
      for (int v = far(dA); v >= 0; v = pr[size_t(v)]) { dp[size_t(v)] = 0; q2.push({0, v}); }
      while (!q2.empty()) { auto [dd, u] = q2.top(); q2.pop(); if (dd > dp[size_t(u)]) continue; for (auto [v, w] : adj[size_t(u)]) if (dd + w < dp[size_t(v)]) { dp[size_t(v)] = dd + w; q2.push({dp[size_t(v)], v}); } }
      std::vector<double> s2; for (double x : dp) if (x < 1e299) s2.push_back(x);
      if (!s2.empty()) { std::nth_element(s2.begin(), s2.begin() + long(s2.size() / 2), s2.end()); const double lim = std::max(1e-9, s2[s2.size() / 2] * 3);
        for (int i = 0; i < n; i++) trunk[size_t(i)] = dp[size_t(i)] <= lim; } }
    double el = 0; int ne = 0; for (auto& v : adj) for (auto& [b, w] : v) { el += w; ne++; } el = ne ? el / ne : L / 20;
    const int N = std::clamp(int(L / std::max(el, 1e-9)), 6, 200);
    std::vector<V2> C(static_cast<size_t>(N + 1), V2{0, 0}); std::vector<double> cw(static_cast<size_t>(N + 1), 0);
    for (int i = 0; i < n; i++) { if (!trunk[size_t(i)]) continue; const int k = dA[size_t(i)] > 1e299 ? 0 : std::clamp(int(std::lround(dA[size_t(i)] / L * N)), 0, N); C[size_t(k)][0] += orig[size_t(i)][0]; C[size_t(k)][1] += orig[size_t(i)][1]; cw[size_t(k)] += 1; }
    { int last = -1; for (int k = 0; k <= N; k++) if (cw[size_t(k)] > 0) { C[size_t(k)][0] /= cw[size_t(k)]; C[size_t(k)][1] /= cw[size_t(k)];
        if (last >= 0) for (int j = last + 1; j < k; j++) { const double t = double(j - last) / (k - last); for (int c = 0; c < 2; c++) C[size_t(j)][c] = C[size_t(last)][c] + (C[size_t(k)][c] - C[size_t(last)][c]) * t; }
        else for (int j = 0; j < k; j++) C[size_t(j)] = C[size_t(k)];
        last = k; }
      for (int j = last + 1; j <= N && last >= 0; j++) C[size_t(j)] = C[size_t(last)]; }
    for (int it = 0; it < 6; it++) { auto D = C; for (int k = 1; k < N; k++) for (int c = 0; c < 2; c++) D[size_t(k)][c] = (C[size_t(k - 1)][c] + 2 * C[size_t(k)][c] + C[size_t(k + 1)][c]) / 4; C = D; }
    std::vector<double> cum(static_cast<size_t>(N + 1), 0); for (int k = 0; k < N; k++) cum[size_t(k + 1)] = cum[size_t(k)] + std::hypot(C[size_t(k + 1)][0] - C[size_t(k)][0], C[size_t(k + 1)][1] - C[size_t(k)][1]);
    // each point: along (the length of the line up to its nearest place on it) and across (signed, to the line's left)
    std::vector<double> along(static_cast<size_t>(n)), across(static_cast<size_t>(n)), px(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) { const auto& u = orig[size_t(i)]; const double x0 = dA[size_t(i)] > 1e299 ? 0 : dA[size_t(i)] / L * N; double x = x0, bd = 1e300; double tx = 0, ty = 1, qx = 0, qy = 0;
      for (int k = std::max(0, int(x0) - 4); k <= std::min(N - 1, int(x0) + 4); k++) { const double ax = C[size_t(k)][0], ay = C[size_t(k)][1], vx = C[size_t(k + 1)][0] - ax, vy = C[size_t(k + 1)][1] - ay, l2 = vx * vx + vy * vy;
        const double t = l2 > 0 ? std::clamp(((u[0] - ax) * vx + (u[1] - ay) * vy) / l2, 0.0, 1.0) : 0; const double d = std::hypot(ax + vx * t - u[0], ay + vy * t - u[1]);
        if (d < bd) { bd = d; x = k + t; const double l = std::sqrt(l2); if (l > 0) { tx = vx / l; ty = vy / l; } qx = ax + vx * t; qy = ay + vy * t; } }
      px[size_t(i)] = x; const int k = std::min(int(x), N - 1); along[size_t(i)] = cum[size_t(k)] + (cum[size_t(k + 1)] - cum[size_t(k)]) * (x - k);
      across[size_t(i)] = -(u[0] - qx) * ty + (u[1] - qy) * tx; }
    // the body's half width along the line (the arms left out: past twice the median offset), smoothed; its mean — the strip's
    std::vector<double> ab(static_cast<size_t>(n)); for (int i = 0; i < n; i++) ab[size_t(i)] = std::abs(across[size_t(i)]);
    auto so = ab; std::nth_element(so.begin(), so.begin() + long(so.size() / 2), so.end()); const double cut = std::max(1e-9, so[so.size() / 2] * 2.5);
    std::vector<double> hwL(static_cast<size_t>(N + 1), 0), hwR(static_cast<size_t>(N + 1), 0);
    for (int i = 0; i < n; i++) { if (ab[size_t(i)] > cut || !trunk[size_t(i)]) continue; const int k = std::clamp(int(std::lround(px[size_t(i)])), 0, N); if (across[size_t(i)] >= 0) hwL[size_t(k)] = std::max(hwL[size_t(k)], across[size_t(i)]); else hwR[size_t(k)] = std::max(hwR[size_t(k)], -across[size_t(i)]); }
    for (auto* H : {&hwL, &hwR}) { auto& h = *H; double last = 0; for (int k = 0; k <= N; k++) { if (h[size_t(k)] <= 0) h[size_t(k)] = last; else last = h[size_t(k)]; }
      for (int k = N; k >= 0; k--) if (h[size_t(k)] <= 0) h[size_t(k)] = k < N ? h[size_t(k + 1)] : 0;
      for (int it = 0; it < 3; it++) { auto D = h; for (int k = 1; k < N; k++) D[size_t(k)] = (h[size_t(k - 1)] + 2 * h[size_t(k)] + h[size_t(k + 1)]) / 4; h = D; } }
    double W = 0; for (int k = 0; k <= N; k++) W += hwL[size_t(k)] + hwR[size_t(k)]; W /= (2.0 * (N + 1)); if (W <= 0) continue;
    std::vector<V2> out(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) { const double x = std::clamp(px[size_t(i)], 0.0, double(N)); const int k = std::min(int(x), N - 1); const double t = x - k;
      const double hl = std::max(1e-9, hwL[size_t(k)] + (hwL[size_t(k + 1)] - hwL[size_t(k)]) * t), hr = std::max(1e-9, hwR[size_t(k)] + (hwR[size_t(k + 1)] - hwR[size_t(k)]) * t);
      const double a = across[size_t(i)]; const double side = a >= 0 ? (a <= hl ? a / hl * W : W + (a - hl)) : (-a <= hr ? a / hr * W : -W + (a + hr));   // (an arm past the body: as far out as it went)
      out[size_t(i)] = {-side, along[size_t(i)]}; }
    // (turned-over polygons: their points eased towards their neighbours till none is left)
    { auto area = [&](const std::vector<int>& L0) { double a = 0; for (size_t k = 0; k < L0.size(); k++) { const auto& p = out[size_t(L0[k])]; const auto& q = out[size_t(L0[(k + 1) % L0.size()])]; a += p[0] * q[1] - q[0] * p[1]; } return a; };
      double tot = 0; for (auto& L0 : loops) tot += area(L0); const double sg = tot >= 0 ? 1 : -1;
      for (int it = 0; it < 40; it++) { std::set<int> bad; for (auto& L0 : loops) if (area(L0) * sg <= 0) bad.insert(L0.begin(), L0.end()); if (bad.empty()) break;
        for (int p : bad) { double ax = 0, ay = 0; int c = 0; for (auto [q, w] : adj[size_t(p)]) { ax += out[size_t(q)][0]; ay += out[size_t(q)][1]; c++; } if (c) out[size_t(p)] = {ax / c, ay / c}; } } }
    // in its old middle; the end that was higher on top (turned half round — never mirrored)
    const bool up = C[size_t(N)][1] >= C[0][1];
    double c0[2] = {0, 0}, c1[2] = {0, 0}; for (int i = 0; i < n; i++) for (int c = 0; c < 2; c++) { c0[c] += orig[size_t(i)][c]; c1[c] += out[size_t(i)][c]; }
    for (int c = 0; c < 2; c++) { c0[c] /= n; c1[c] /= n; }
    for (int i = 0; i < n; i++) { double x = out[size_t(i)][0] - c1[0], y = out[size_t(i)][1] - c1[1]; if (!up) { x = -x; y = -y; }
      const float U = float(c0[0] + x), Vv = float(c0[1] + y);
      for (int j = P.off[size_t(pts[size_t(i)])]; j < P.off[size_t(pts[size_t(i)]) + 1]; j++) m.cornerUV[size_t(P.list[size_t(j)])] = {U, Vv}; }
    done++;
  }
  return done;
}

}  // namespace hm::uv
