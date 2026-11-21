#include "scene/Uv.h"
#include "scene/UvSolve.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
  const bool uv = has(m); std::vector<int> reps; const std::vector<char> seam = uv ? seamFlags(m, T) : std::vector<char>{};
  for (int v = 0; v < T.n; v++) {
    const int first = P.count(); reps.clear();
    if (!uv) {                                                   // (no UV: one point to a vertex)
      for (int i = voff[size_t(v)]; i < voff[size_t(v) + 1]; i++) { const int k = vc[size_t(i)]; if (reps.empty()) { reps.push_back(k); P.vert.push_back(v); } P.of[size_t(k)] = first; }
      continue; }
    // (a point: the corners round the vertex joined through edges that are not seams, where their UV is one. A seam cuts the point in two even where the UV lies
    // in one place: the two sides are two shells' borders, not one shell with a line in it)
    const int n0 = voff[size_t(v)], cnt = voff[size_t(v) + 1] - n0; std::vector<int> par(static_cast<size_t>(cnt)); for (int i = 0; i < cnt; i++) par[size_t(i)] = i;
    std::function<int(int)> find = [&](int x) { while (par[size_t(x)] != x) { par[size_t(x)] = par[size_t(par[size_t(x)])]; x = par[size_t(x)]; } return x; };
    auto local = [&](int k) { for (int i = 0; i < cnt; i++) if (vc[size_t(n0 + i)] == k) return i; return -1; };
    for (int i = T.voff[size_t(v)]; i < T.voff[size_t(v) + 1]; i++) { const int e = T.vE[size_t(i)]; const int f0 = T.ef0[size_t(e)], f1 = T.ef1[size_t(e)]; if (f1 < 0 || seam[size_t(e)]) continue;
      const int k0 = cornerAt(T, f0, v), k1 = cornerAt(T, f1, v); if (k0 < 0 || k1 < 0 || !same(m.cornerUV[size_t(k0)], m.cornerUV[size_t(k1)])) continue;
      const int l0 = local(k0), l1 = local(k1); if (l0 >= 0 && l1 >= 0) par[size_t(find(l0))] = find(l1); }
    std::map<int, int> rootPt;
    for (int i = 0; i < cnt; i++) { const int k = vc[size_t(n0 + i)]; const int r = find(i); auto g = rootPt.find(r); int pt;
      if (g == rootPt.end()) { pt = first + int(rootPt.size()); rootPt[r] = pt; P.vert.push_back(v); } else pt = g->second;
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
double tileCoverage(const std::vector<Covered>& ms, int n) {
  n = std::clamp(n, 16, 4096); std::vector<uint8_t> bm(size_t(n) * size_t(n), 0);
  for (const Covered& c : ms) { if (!c.m || !has(*c.m)) continue; const PolyMesh& m = *c.m;
    for (int f = 0; f < m.faceCount(); f++) { if (size_t(f) < c.hidden.size() && c.hidden[size_t(f)]) continue;
      const int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
      for (int k = s + 1; k + 1 < e; k++) {
        const double X[3] = {m.cornerUV[size_t(s)][0] * n, m.cornerUV[size_t(k)][0] * n, m.cornerUV[size_t(k + 1)][0] * n}, Y[3] = {m.cornerUV[size_t(s)][1] * n, m.cornerUV[size_t(k)][1] * n, m.cornerUV[size_t(k + 1)][1] * n};
        const int y0 = std::max(0, int(std::floor(std::min({Y[0], Y[1], Y[2]}) - 0.5))), y1 = std::min(n - 1, int(std::ceil(std::max({Y[0], Y[1], Y[2]}) - 0.5)));
        for (int y = y0; y <= y1; y++) { const double yc = y + 0.5; double a = 1e300, b = -1e300;   // (the row's middle line through the triangle: from a to b)
          for (int i = 0; i < 3; i++) { const int j = (i + 1) % 3; if ((Y[i] <= yc) == (Y[j] <= yc)) continue; const double x = X[i] + (X[j] - X[i]) * (yc - Y[i]) / (Y[j] - Y[i]); a = std::min(a, x); b = std::max(b, x); }
          if (a > b) continue; const int x0 = std::max(0, int(std::ceil(a - 0.5))), x1 = std::min(n - 1, int(std::floor(b - 0.5)));
          if (x0 <= x1) std::fill(bm.begin() + (size_t(y) * size_t(n) + size_t(x0)), bm.begin() + (size_t(y) * size_t(n) + size_t(x1) + 1), uint8_t(1)); } } } }
  size_t on = 0; for (uint8_t v : bm) on += v; return double(on) / double(bm.size());
}


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

// Faces wound the other way than their neighbours (a ribbon whose normals turn over halfway — folded like a book): 1 for the faces of the second kind, in each piece
// joined through the edges `joined` says. Two faces joined run their shared edge in opposite directions when they agree
std::vector<char> windingFlips(const topo::TTopo& T, const std::function<bool(int)>& joined) {
  std::vector<char> col(size_t(T.nf), 0), seen(size_t(T.nf), 0);
  auto dirIn = [&](int f, int e, int& a, int& b) { for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) if (T.ce[size_t(k)] == e) { a = T.fv[size_t(k)]; b = T.fv[size_t(k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)])]; return true; } return false; };
  for (int s = 0; s < T.nf; s++) { if (seen[size_t(s)]) continue; seen[size_t(s)] = 1; std::vector<int> st{s};
    while (!st.empty()) { const int f = st.back(); st.pop_back();
      for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int e = T.ce[size_t(k)]; if (T.ef1[size_t(e)] < 0 || !joined(e)) continue; const int g = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; if (g < 0 || seen[size_t(g)]) continue;
        int a0, b0, a1, b1; if (!dirIn(f, e, a0, b0) || !dirIn(g, e, a1, b1)) continue; const bool same = a0 == a1 && b0 == b1;
        seen[size_t(g)] = 1; col[size_t(g)] = same ? char(1 - col[size_t(f)]) : col[size_t(f)]; st.push_back(g); } } }
  return col;
}
namespace {
// a polygon's triangles in a shell (its corners' local vertices li): cut the right way round in its plane
void addTris(const PolyMesh& m, const topo::TTopo& T, int f, const std::vector<int>& li, std::vector<solve::Tri>& tris) {
  std::vector<Vec3> ring; for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) ring.push_back(m.pos[size_t(T.fv[size_t(k)])]);
  for (auto& t : solve::triangulate(ring)) tris.push_back({li[size_t(t[0])], li[size_t(t[1])], li[size_t(t[2])]});
}
// Triangles of a solved shell lying over each other (not just turned over): pairs that share no vertex, tested apart by a separating axis; sorted along x
bool tangled(const std::vector<solve::P2>& U, const std::vector<solve::Tri>& tris) {
  struct B { double x0, x1, y0, y1; int i; }; std::vector<B> bs; bs.reserve(tris.size());
  for (size_t i = 0; i < tris.size(); i++) { const auto& t = tris[i]; B b{1e300, -1e300, 1e300, -1e300, int(i)}; for (int k = 0; k < 3; k++) { const auto& q = U[size_t(t[size_t(k)])]; b.x0 = std::min(b.x0, q[0]); b.x1 = std::max(b.x1, q[0]); b.y0 = std::min(b.y0, q[1]); b.y1 = std::max(b.y1, q[1]); } bs.push_back(b); }
  std::sort(bs.begin(), bs.end(), [](const B& a, const B& b) { return a.x0 < b.x0; });
  double avg = 0; for (const B& b : bs) avg += std::max(b.x1 - b.x0, b.y1 - b.y0); avg /= double(std::max<size_t>(bs.size(), 1)); const double tol = avg * 1e-3;
  auto apart = [&](const solve::Tri& A, const solve::Tri& C) {
    const solve::P2* pa[3] = {&U[size_t(A[0])], &U[size_t(A[1])], &U[size_t(A[2])]}; const solve::P2* pc[3] = {&U[size_t(C[0])], &U[size_t(C[1])], &U[size_t(C[2])]};
    for (int w = 0; w < 2; w++) { const solve::P2** q = w ? pc : pa;
      for (int i = 0; i < 3; i++) { const auto &p0 = *q[i], &p1 = *q[(i + 1) % 3]; const double nx = p1[1] - p0[1], ny = p0[0] - p1[0], nl = std::hypot(nx, ny); if (nl < 1e-15) continue;
        double a0 = 1e300, a1 = -1e300, c0 = 1e300, c1 = -1e300; for (int k = 0; k < 3; k++) { const double da = ((*pa[k])[0] * nx + (*pa[k])[1] * ny) / nl, dc = ((*pc[k])[0] * nx + (*pc[k])[1] * ny) / nl; a0 = std::min(a0, da); a1 = std::max(a1, da); c0 = std::min(c0, dc); c1 = std::max(c1, dc); }
        if (a1 <= c0 + tol || c1 <= a0 + tol) return true; } }
    return false; };
  for (size_t i = 0; i < bs.size(); i++) for (size_t j = i + 1; j < bs.size() && bs[j].x0 < bs[i].x1; j++) {
    if (bs[j].y0 >= bs[i].y1 || bs[i].y0 >= bs[j].y1) continue; const auto &A = tris[size_t(bs[i].i)], &C = tris[size_t(bs[j].i)];
    bool share = false; for (int a : A) for (int c : C) if (a == c) share = true; if (share) continue;
    if (!apart(A, C)) return true; }
  return false;
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
// The h / v constraints' chains (the aligned edges of one kind joined end to end, each a plain path): every edge laid at its 3D length along its line,
// all of a chain the same way — ties with offsets, for LSCM's start. With the ties alone LSCM may lay a chain up and back down the same line (a hairpin:
// no triangle turned over, so SLIM can never open it — the neck's back seam, 2026-11-10). The way: from the border edges — the shell's inside on the
// left of each triangle's edge (R: the layout without them, its turn `or`), else as the chain runs in R. align: (a, b, 1 across / 2 up); how many tied
// An h / v edge whose two ends both lie on one chain of the other kind (a wrist's row marked a step too far — one edge up the side seam): level and upright
// at once it would be a point — the corner crushed into a fan (2026-11-10). Such edges made of the other kind, or left out; how many changed
int dropCollapsing(std::vector<std::array<int, 3>>& align, const std::vector<std::array<int, 3>>* tris = nullptr, const std::vector<Vec3>* X = nullptr) {
  std::map<int, int> par; std::function<int(int)> find = [&](int x) { auto it = par.find(x); if (it == par.end()) { par[x] = x; return x; } if (it->second == x) return x; const int r = find(it->second); par[x] = r; return r; };
  auto key = [](int kind, int v) { return v * 2 + (kind - 1); };   // (each kind its own chains)
  for (auto& a : align) { const int x = find(key(a[2], a[0])), y = find(key(a[2], a[1])); if (x != y) par[x] = y; }
  // (such an edge runs along that chain: made of its kind — the corner stays square; the user's mark one step too far just turns the corner, 2026-11-10)
  int changed = 0; std::set<std::pair<int, int>> turned;
  for (auto& a : align) { const int o = 3 - a[2]; if (!par.count(key(o, a[0])) || !par.count(key(o, a[1])) || find(key(o, a[0])) != find(key(o, a[1]))) continue;
    a[2] = o; turned.insert({std::min(a[0], a[1]), std::max(a[0], a[1])}); changed++; }
  // (a chain turning inside one triangle — all three of its corners on one line: it would be flat. The edge past the turn: the chain's tail there, else the
  //  shorter — made of the other kind when a chain of that kind meets it (it runs on along that one: the corner square), else left out)
  if (tris && X) for (int pass = 0; pass < 4; pass++) {
    std::map<std::pair<int, int>, int> kindOf; std::map<std::pair<int, int>, int> deg; for (auto& a : align) { kindOf[{std::min(a[0], a[1]), std::max(a[0], a[1])}] = a[2]; deg[{a[2], a[0]}]++; deg[{a[2], a[1]}]++; }
    std::map<std::pair<int, int>, int> fix;   // (edge → 0 left out, else its new kind)
    for (auto& t : *tris) for (int K = 1; K <= 2; K++) {
      std::vector<std::pair<int, int>> es; for (int j = 0; j < 3; j++) { const int a = t[size_t(j)], b = t[size_t((j + 1) % 3)]; auto it = kindOf.find({std::min(a, b), std::max(a, b)}); if (it != kindOf.end() && it->second == K) es.push_back({std::min(a, b), std::max(a, b)}); }
      if (es.size() < 2) continue;
      auto tail = [&](const std::pair<int, int>& e) { return deg[{K, e.first}] == 1 || deg[{K, e.second}] == 1; };
      auto len = [&](const std::pair<int, int>& e) { return ((*X)[size_t(e.first)] - (*X)[size_t(e.second)]).length(); };
      std::pair<int, int> d = es[0]; for (auto& e : es) { const bool te = tail(e), td = tail(d); if ((te && !td) || (te == td && len(e) < len(d))) d = e; }
      const int o = 3 - K; fix[d] = !turned.count(d) && (deg[{o, d.first}] > 0 || deg[{o, d.second}] > 0) ? o : 0; }
    if (fix.empty()) break;
    std::vector<std::array<int, 3>> kept; for (auto a : align) { const auto k = std::make_pair(std::min(a[0], a[1]), std::max(a[0], a[1])); const auto g = fix.find(k);
      if (g == fix.end()) { kept.push_back(a); continue; } changed++; if (g->second) { a[2] = g->second; turned.insert(k); kept.push_back(a); } }
    align = std::move(kept); }
  return changed;
}
// Parts of one shell lying over each other (toes squeezed between upright sides, fingers fanned in): pushed apart. Each border point inside a triangle
// not its own is pulled out to the nearest border of the part it sank into (a little past it), against that border's nearest point — both go half way;
// SLIM keeps every triangle the right way round and the ties (D) hold. A few rounds, the best kept: how many points sank in at the end (2026-11-10)
int sunkPoints(const std::vector<solve::P2>& U, const std::vector<solve::Tri>& tris, const std::vector<char>& border, std::vector<std::pair<int, int>>* hits = nullptr) {
  auto inTri = [&](const solve::P2& p, const solve::Tri& t) { const auto &a = U[size_t(t[0])], &b = U[size_t(t[1])], &c = U[size_t(t[2])];
    const double d1 = (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]), d2 = (c[0] - b[0]) * (p[1] - b[1]) - (c[1] - b[1]) * (p[0] - b[0]), d3 = (a[0] - c[0]) * (p[1] - c[1]) - (a[1] - c[1]) * (p[0] - c[0]);
    const double s = std::abs(d1) + std::abs(d2) + std::abs(d3), e = 1e-9 * s; return (d1 > e && d2 > e && d3 > e) || (d1 < -e && d2 < -e && d3 < -e); };
  std::vector<std::array<double, 4>> box(tris.size()); for (size_t t = 0; t < tris.size(); t++) { auto& b = box[t]; b = {1e300, 1e300, -1e300, -1e300}; for (int v : tris[t]) { b[0] = std::min(b[0], U[size_t(v)][0]); b[1] = std::min(b[1], U[size_t(v)][1]); b[2] = std::max(b[2], U[size_t(v)][0]); b[3] = std::max(b[3], U[size_t(v)][1]); } }
  int n = 0;
  for (size_t p = 0; p < U.size(); p++) { if (!border[p]) continue;
    for (size_t t = 0; t < tris.size(); t++) { const auto& b = box[t]; if (U[p][0] < b[0] || U[p][0] > b[2] || U[p][1] < b[1] || U[p][1] > b[3]) continue;
      const auto& tt = tris[t]; if (tt[0] == int(p) || tt[1] == int(p) || tt[2] == int(p)) continue;
      if (inTri(U[p], tt)) { n++; if (hits) hits->push_back({int(p), int(t)}); break; } } }
  return n;
}
int pushApart(std::vector<solve::P2>& U, const std::vector<Vec3>& X, const std::vector<solve::Tri>& tris, const std::vector<solve::Tri>& trisS, const solve::Dofs& D) {
  std::map<std::pair<int, int>, int> ec; for (auto& t : tris) for (int j = 0; j < 3; j++) { const int a = t[size_t(j)], b = t[size_t((j + 1) % 3)]; ec[{std::min(a, b), std::max(a, b)}]++; }
  std::vector<char> border(U.size(), 0); std::vector<std::pair<int, int>> bes; for (auto& [e, c] : ec) if (c == 1) { border[size_t(e.first)] = border[size_t(e.second)] = 1; bes.push_back(e); }
  std::vector<std::vector<int>> triAt(U.size()); for (size_t t = 0; t < tris.size(); t++) for (int v : tris[t]) triAt[size_t(v)].push_back(int(t));
  double el = 0; for (auto& e : bes) el += std::hypot(U[size_t(e.first)][0] - U[size_t(e.second)][0], U[size_t(e.first)][1] - U[size_t(e.second)][1]); el = bes.empty() ? 0 : el / double(bes.size());
  int best = sunkPoints(U, tris, border); auto bestU = U; std::map<std::array<int, 2>, double> held;   // (pairs pushed apart in a round before: held apart after — else they sink back in the next)
  for (int round = 0; round < 8 && best > 0; round++) {
    std::vector<std::pair<int, int>> hits; sunkPoints(U, tris, border, &hits);
    std::vector<std::array<int, 2>> pull; std::vector<double> off;
    for (auto [p, t0] : hits) {
      // (the border near the triangle it sank into: the triangles round it a few rings out, not through p's own)
      std::set<int> seen{t0}; std::vector<int> fr{t0}; std::set<std::pair<int, int>> near;
      for (int ring = 0; ring < 6 && !fr.empty(); ring++) { std::vector<int> nx; for (int t : fr) for (int v : tris[size_t(t)]) { if (v == p) continue;
          for (int j = 0; j < 3; j++) { const int a = tris[size_t(t)][size_t(j)], b = tris[size_t(t)][size_t((j + 1) % 3)]; if (a != p && b != p && ec[{std::min(a, b), std::max(a, b)}] == 1) near.insert({std::min(a, b), std::max(a, b)}); }
          for (int t2 : triAt[size_t(v)]) if (seen.insert(t2).second) nx.push_back(t2); } fr = std::move(nx); }
      double bd = 1e300; solve::P2 ex{0, 0}; int q = -1;
      for (auto [a, b] : near) { const auto &A = U[size_t(a)], &B = U[size_t(b)]; const double dx = B[0] - A[0], dy = B[1] - A[1], L2 = dx * dx + dy * dy; const double s = L2 > 0 ? std::clamp(((U[size_t(p)][0] - A[0]) * dx + (U[size_t(p)][1] - A[1]) * dy) / L2, 0.0, 1.0) : 0;
        const solve::P2 c{A[0] + dx * s, A[1] + dy * s}; const double d = std::hypot(c[0] - U[size_t(p)][0], c[1] - U[size_t(p)][1]); if (d < bd) { bd = d; ex = c; q = s < 0.5 ? a : b; } }
      if (q < 0 || !(bd > 0)) continue;
      const double k = (bd + 0.1 * el) / bd; const solve::P2 to{U[size_t(p)][0] + (ex[0] - U[size_t(p)][0]) * k, U[size_t(p)][1] + (ex[1] - U[size_t(p)][1]) * k};   // (a little past the border)
      for (int c = 0; c < 2; c++) held[{p * 2 + c, q * 2 + c}] = to[size_t(c)] - U[size_t(q)][size_t(c)]; }
    for (auto& [pq, o] : held) { pull.push_back(pq); off.push_back(o); }
    if (pull.empty()) break;
    auto W = U; const auto r = solve::slim(W, X, trisS, D, 40, &pull, 1e3, &off);
    if (!r.ok || solve::flips(W, X, tris)) break;
    U = std::move(W); const int now = sunkPoints(U, tris, border); if (std::getenv("UVDBG")) std::fprintf(stderr, "  push apart round %d: %d sunk (was %d)\n", round, now, best);
    if (now < best) { best = now; bestU = U; } }
  U = std::move(bestU);
  if (!held.empty()) {                                           // (eased after: the pushes held softly — a point pushed too far (a spike off a toe) comes back in; kept unless more sink in)
    std::vector<std::array<int, 2>> pull; std::vector<double> off; for (auto& [pq, o] : held) { pull.push_back(pq); off.push_back(o); }
    auto W = U; const auto r = solve::slim(W, X, trisS, D, 60, &pull, 30, &off);
    if (r.ok && !solve::flips(W, X, tris)) { const int now = sunkPoints(W, tris, border); if (std::getenv("UVDBG")) std::fprintf(stderr, "  push apart eased: %d sunk (was %d)\n", now, best); if (now <= best) { U = std::move(W); best = now; } } }
  return best;
}
// a shell symmetric in itself, wherever it lies (not across the model's plane — a leaf, a bow's tail, a brooch on one side): its own
// mirror plane — one of its main axes' planes through its middle — and each local point's mirror. Taken only when every point has its
// mirror near the reflected place and every edge goes to an edge (the seams alike on both halves); a point split by a cut — its twins
// told apart by their neighbours. Empty: not symmetric. n — the plane's normal, c — a point on it, up — its long axis along the plane
std::vector<int> selfMirror(const std::vector<Vec3>& X, const std::set<std::pair<int, int>>& E, Vec3& nOut, Vec3& cOut, Vec3& upOut) {
  const int n = int(X.size()); if (n < 6 || E.empty()) return {};
  Vec3 c; for (const Vec3& p : X) c += p; c = c / double(n);
  double C[3][3] = {}; for (const Vec3& p : X) { const double d[3] = {p.x - c.x, p.y - c.y, p.z - c.z}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) C[i][j] += d[i] * d[j]; }
  double V[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};               // (Jacobi: the covariance's axes)
  for (int sweep = 0; sweep < 30; sweep++) for (int p = 0; p < 2; p++) for (int q = p + 1; q < 3; q++) { if (std::abs(C[p][q]) < 1e-18) continue;
    const double th = 0.5 * std::atan2(2 * C[p][q], C[q][q] - C[p][p]), cs = std::cos(th), sn = std::sin(th);
    for (int k = 0; k < 3; k++) { const double a = C[k][p], b = C[k][q]; C[k][p] = cs * a - sn * b; C[k][q] = sn * a + cs * b; }
    for (int k = 0; k < 3; k++) { const double a = C[p][k], b = C[q][k]; C[p][k] = cs * a - sn * b; C[q][k] = sn * a + cs * b; }
    for (int k = 0; k < 3; k++) { const double a = V[k][p], b = V[k][q]; V[k][p] = cs * a - sn * b; V[k][q] = sn * a + cs * b; } }
  std::vector<double> el; for (auto& [a, b] : E) el.push_back(X[size_t(a)].distanceTo(X[size_t(b)])); std::nth_element(el.begin(), el.begin() + long(el.size() / 2), el.end());
  const double me = el[el.size() / 2]; if (!(me > 0)) return {};
  const double tol = me * 0.2, cell = me;
  std::unordered_map<long long, std::vector<int>> grid; auto key = [&](const Vec3& p) { const long long ix = (long long)std::floor(p.x / cell), iy = (long long)std::floor(p.y / cell), iz = (long long)std::floor(p.z / cell); return (ix * 73856093LL) ^ (iy * 19349663LL) ^ (iz * 83492791LL); };
  for (int i = 0; i < n; i++) grid[key(X[size_t(i)])].push_back(i);
  std::vector<std::vector<int>> nb(static_cast<size_t>(n)); for (auto& [a, b] : E) { nb[size_t(a)].push_back(b); nb[size_t(b)].push_back(a); }
  auto hasE = [&](int a, int b) { return E.count({std::min(a, b), std::max(a, b)}) > 0; };
  for (int ax = 0; ax < 3; ax++) {
    const Vec3 N = Vec3(V[0][ax], V[1][ax], V[2][ax]).normalized();
    std::vector<std::vector<int>> cand(static_cast<size_t>(n)); bool ok = true;
    for (int i = 0; i < n && ok; i++) { const Vec3 r = X[size_t(i)] - N * (2 * (X[size_t(i)] - c).dot(N));
      if (n <= 3000) { for (int j = 0; j < n; j++) if (X[size_t(j)].distanceTo(r) < tol) cand[size_t(i)].push_back(j); }   // (a shell of a few thousand points: each one tried — the grid's cells missed a point lying right on the mirror place, 2026-11-11)
      else for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) { const Vec3 q(r.x + dx * cell, r.y + dy * cell, r.z + dz * cell); auto g = grid.find(key(q)); if (g == grid.end()) continue;
        for (int j : g->second) if (X[size_t(j)].distanceTo(r) < tol && std::find(cand[size_t(i)].begin(), cand[size_t(i)].end(), j) == cand[size_t(i)].end()) cand[size_t(i)].push_back(j); }
      if (cand[size_t(i)].empty()) { ok = false; if (std::getenv("UVDBG")) { double dmin = 1e300; for (int j = 0; j < n; j++) dmin = std::min(dmin, X[size_t(j)].distanceTo(r)); std::printf("selfMirror axis %d: point %d has no mirror (nearest %.4f, tol %.4f, edge %.4f)\n", ax, i, dmin, tol, me); } } }
    if (!ok) continue;
    std::vector<int> mir(static_cast<size_t>(n), -1); for (int i = 0; i < n; i++) if (cand[size_t(i)].size() == 1) mir[size_t(i)] = cand[size_t(i)][0];
    for (int round = 0; round < 50; round++) { bool ch = false;   // (twins on a cut: the one whose neighbours mirror this one's)
      for (int i = 0; i < n; i++) { if (mir[size_t(i)] >= 0) continue; int best = -1, bs = -1, ties = 0;
        for (int j : cand[size_t(i)]) { int s = 0; for (int a : nb[size_t(i)]) if (mir[size_t(a)] >= 0 && hasE(mir[size_t(a)], j)) s++; if (s > bs) { bs = s; best = j; ties = 1; } else if (s == bs) ties++; }
        if (bs > 0 && ties == 1) { mir[size_t(i)] = best; ch = true; } }
      if (!ch) break; }
    int fixedPts = 0; for (int i = 0; i < n && ok; i++) { const int j = mir[size_t(i)]; if (j < 0 || mir[size_t(j)] != i) ok = false; else if (j == i) fixedPts++; }
    if (!ok || fixedPts * 2 > n) { if (std::getenv("UVDBG")) { int un = 0, bad = 0; for (int i = 0; i < n; i++) { if (mir[size_t(i)] < 0) un++; else if (mir[size_t(mir[size_t(i)])] != i) bad++; } std::printf("selfMirror axis %d: unresolved %d, not paired back %d, fixed %d of %d\n", ax, un, bad, fixedPts, n); } continue; }   // (a flat shell's own plane maps each point to itself: no mirror)
    for (auto& [a, b] : E) if (!hasE(mir[size_t(a)], mir[size_t(b)])) { ok = false; if (std::getenv("UVDBG")) std::printf("selfMirror axis %d: edge %d-%d goes to no edge\n", ax, a, b); break; }
    if (!ok) continue;
    int o1 = (ax + 1) % 3, o2 = (ax + 2) % 3; if (C[o2][o2] > C[o1][o1]) std::swap(o1, o2);   // (the long axis along the plane: the shell's height)
    Vec3 up = Vec3(V[0][o1], V[1][o1], V[2][o1]).normalized(); if (up.y < 0) up = -up;
    nOut = N; cOut = c; upOut = up; return mir; }
  return {};
}

struct ChainStep { int dq, dp; double off; };   // (dof q = dof p + off)
std::vector<ChainStep> chainTargets(const std::vector<std::array<int, 3>>& align, const std::vector<Vec3>& X, const std::vector<solve::Tri>& tris, const std::vector<solve::P2>& R) {
  std::vector<ChainStep> out; if (align.empty() || R.size() != X.size()) return out;
  std::set<std::pair<int, int>> dir; double sa = 0;
  for (auto& t : tris) { for (int j = 0; j < 3; j++) dir.insert({t[size_t(j)], t[size_t((j + 1) % 3)]});
    const auto &p = R[size_t(t[0])], &q = R[size_t(t[1])], &r = R[size_t(t[2])]; sa += (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]); }
  const double orn = sa < 0 ? -1 : 1; double mid[2] = {0, 0}; for (auto& q : R) { mid[0] += q[0] / double(R.size()); mid[1] += q[1] / double(R.size()); }
  for (int kind = 1; kind <= 2; kind++) {
    const int c = kind == 1 ? 0 : 1, o = 1 - c;                     // (c: the coordinate along the chain; o: the one the ties keep the same)
    std::map<int, std::set<int>> adj; for (auto& a : align) if (a[2] == kind && a[0] != a[1]) { adj[a[0]].insert(a[1]); adj[a[1]].insert(a[0]); }
    std::set<int> seen;
    for (auto& [v0, nb0] : adj) { if (seen.count(v0)) continue;
      std::vector<int> comp{v0}; seen.insert(v0); size_t ne2 = 0;
      for (size_t h = 0; h < comp.size(); h++) for (int w : adj[comp[h]]) { ne2++; if (seen.insert(w).second) comp.push_back(w); }
      bool path = ne2 / 2 + 1 == comp.size(); int end = -1; for (int v : comp) { if (adj[v].size() > 2) path = false; if (adj[v].size() == 1 && end < 0) end = v; }
      if (!path || end < 0 || comp.size() < 2) continue;              // (a ring or a branching one: left to the ties alone)
      std::vector<int> ord{end}; for (int prev = -1, cur = end; ord.size() < comp.size();) { int nx = -1; for (int w : adj[cur]) if (w != prev) { nx = w; break; } if (nx < 0) break; ord.push_back(nx); prev = cur; cur = nx; }
      if (ord.size() != comp.size()) continue;
      // (first: the way the chain already runs in R — R the free unfold turned by the constraints: a finger's side runs down the finger, whichever side the
      // shell's bulk is; only a chain lying across there (less than a third of its length along its line) — by the shell's bulk, below)
      { double net = 0, len = 0; for (size_t i = 0; i + 1 < ord.size(); i++) { const auto& p = R[size_t(ord[i])]; const auto& q = R[size_t(ord[i + 1])]; net += q[size_t(c)] - p[size_t(c)]; len += std::hypot(q[0] - p[0], q[1] - p[1]); }
        if (len > 0 && std::abs(net) > len / 3) { const double sg = net < 0 ? -1 : 1;
          for (size_t i = 0; i + 1 < ord.size(); i++) { const int p = ord[i], q = ord[i + 1]; out.push_back({q * 2 + c, p * 2 + c, sg * std::max((X[size_t(q)] - X[size_t(p)]).length(), 1e-9)}); }
          continue; } }
      double cm = 0; for (int v : ord) cm += R[size_t(v)][size_t(o)] / double(ord.size());
      const bool inPos = mid[o] > cm;                                 // (the shell's bulk on the + side of the chain's line)
      double vote = 0;
      for (size_t i = 0; i + 1 < ord.size(); i++) { const int p = ord[i], q = ord[i + 1]; const bool pq = dir.count({p, q}) > 0, qp = dir.count({q, p}) > 0; if (pq == qp) continue;   // (a border edge only)
        const double w = pq ? 1 : -1; const double inSide = kind == 1 ? (inPos ? 1 : -1) : (inPos ? -1 : 1);   // (left of +U is +V; left of +V is −U)
        vote += w * inSide * orn * (X[size_t(q)] - X[size_t(p)]).length(); }
      if (vote == 0) vote = R[size_t(ord.back())][size_t(c)] - R[size_t(ord.front())][size_t(c)];
      const double sg = vote < 0 ? -1 : 1;
      for (size_t i = 0; i + 1 < ord.size(); i++) { const int p = ord[i], q = ord[i + 1]; out.push_back({q * 2 + c, p * 2 + c, sg * std::max((X[size_t(q)] - X[size_t(p)]).length(), 1e-9)}); }
    } }

  return out;
}
// A comb (a palm with every finger's sides upright): the chains of one kind one after another along the outer border keep their order — the short stretch of
// border between two of them (a fingertip, the web between two fingers) laid across at its 3D length, all the same way. Without it the fingers, fanned out in
// the free unfold, are turned down through each other (2026-11-10). The way: from the chain after the other kind's run (the wrist) — the teeth's way; the
// border walked as the triangles go round (the inside on the left; `orn` −1: the layout mirrored). Soft pairs (dof q, dof p, q − p).
// The chains on that border also get their way from the walk, not from the free unfold (a finger lying across there): the first after the wrist as `steps`
// has it, each next one the same way when it runs on in 3D the same way as the one before (a side marked in pieces), the other way when it runs back (the
// two sides of a finger, a finger and the next across the web) — `run`: those chains' steps, `covered`: their edges (their steps in `steps` dropped)
struct Comb { std::vector<ChainStep> run, gaps; std::set<std::pair<int, int>> covered; double wristK = 1; int wristC = -1; };   // (wristK: the wrist widened by — its steps along coordinate wristC)
Comb combTargets(const std::vector<std::array<int, 3>>& align, const std::vector<Vec3>& X, const std::vector<solve::Tri>& tris, const std::vector<ChainStep>& steps, double orn) {
  Comb out; if (align.empty()) return out;
  std::map<std::pair<int, int>, int> kindOf; for (auto& a : align) kindOf[{std::min(a[0], a[1]), std::max(a[0], a[1])}] = a[2];
  std::map<std::pair<int, int>, double> stepOff; for (auto& s : steps) stepOff[{s.dp, s.dq}] = s.off;   // (dof p → dof q: the chain's way along its line)
  std::map<std::pair<int, int>, int> cnt; for (auto& t : tris) for (int j = 0; j < 3; j++) { const int a = t[size_t(j)], b = t[size_t((j + 1) % 3)]; cnt[{std::min(a, b), std::max(a, b)}]++; }
  std::map<int, int> nxt; for (auto& t : tris) for (int j = 0; j < 3; j++) { const int a = t[size_t(j)], b = t[size_t((j + 1) % 3)]; if (cnt[{std::min(a, b), std::max(a, b)}] == 1) nxt[a] = b; }
  std::vector<int> best; double bestL = 0; std::set<int> seen;   // (the outer border: the longest loop)
  for (auto& [v0, w0] : nxt) { if (seen.count(v0)) continue; std::vector<int> loop; double L = 0; int v = v0;
    while (!seen.count(v) && nxt.count(v)) { seen.insert(v); loop.push_back(v); const int w = nxt[v]; L += (X[size_t(w)] - X[size_t(v)]).length(); v = w; }
    if (v == v0 && L > bestL) { bestL = L; best = loop; } }
  const int n = int(best.size()); if (n < 4) return out;
  auto P = [&](int i) { return best[size_t(((i % n) + n) % n)]; };
  auto kindAt = [&](int i) { const int a = P(i), b = P(i + 1); auto it = kindOf.find({std::min(a, b), std::max(a, b)}); return it == kindOf.end() ? 0 : it->second; };
  auto wayAt = [&](int i, int c) { const int a = P(i), b = P(i + 1); if (auto it = stepOff.find({a * 2 + c, b * 2 + c}); it != stepOff.end()) return it->second < 0 ? -1 : 1; if (auto it = stepOff.find({b * 2 + c, a * 2 + c}); it != stepOff.end()) return it->second < 0 ? 1 : -1; return 0; };
  auto elen = [&](int i) { return (X[size_t(P(i + 1))] - X[size_t(P(i))]).length(); };
  for (int K = 1; K <= 2; K++) {
    const int c = K == 1 ? 0 : 1, ac = 1 - c, other = 3 - K;
    int s = -1; for (int i = 0; i < n; i++) if (kindAt(i) == other && kindAt(i + 1) != other) { s = i + 1; break; } if (s < 0) continue;   // (just past the other kind's run: the wrist)
    struct Run { int i0, i1; double len; Vec3 w; };
    std::vector<Run> runs; for (int i = s; i < s + n;) { if (kindAt(i) != K) { i++; continue; } Run r{i, i, 0, Vec3()}; while (r.i1 < s + n && kindAt(r.i1) == K) { r.len += elen(r.i1); r.i1++; } r.w = X[size_t(P(r.i1))] - X[size_t(P(r.i0))]; runs.push_back(r); i = r.i1; }
    if (runs.empty()) continue;
    std::vector<int> sg(runs.size(), 0); for (int i = runs[0].i0; i < runs[0].i1 && !sg[0]; i++) sg[0] = wayAt(i, c);
    if (!sg[0]) continue;
    std::vector<char> back(runs.size(), 0);
    for (size_t r = 1; r < runs.size(); r++) { const double l = runs[r].w.length() * runs[r - 1].w.length(), d = l > 0 ? runs[r].w.dot(runs[r - 1].w) / l : 0;
      if (std::abs(d) < 0.2) { int w = 0; for (int i = runs[r].i0; i < runs[r].i1 && !w; i++) w = wayAt(i, c); sg[r] = w ? w : sg[r - 1]; }   // (neither on nor back: as the free unfold has it)
      else { back[r] = d < 0; sg[r] = d < 0 ? -sg[r - 1] : sg[r - 1]; } }
    for (size_t r = 0; r < runs.size(); r++) for (int i = runs[r].i0; i < runs[r].i1; i++) { const int p = P(i), q = P(i + 1); out.run.push_back({q * 2 + c, p * 2 + c, sg[r] * std::max(elen(i), 1e-9)}); out.covered.insert({std::min(p, q), std::max(p, q)}); }
    const double across = (K == 2 ? (sg[0] < 0 ? 1 : -1) : (sg[0] < 0 ? -1 : 1)) * orn;   // (CCW: teeth down — each gap goes +U; teeth left — each goes −V)
    const size_t gStart = out.gaps.size();
    for (size_t r = 1; r < runs.size(); r++) {                     // (a fingertip / a web: the next chain runs back, the gap short and of no other kind)
      if (std::getenv("UVDBG")) { double gl = 0; for (int i = runs[r - 1].i1; i < runs[r].i0; i++) gl += elen(i); const double l = runs[r].w.length() * runs[r - 1].w.length();
        std::fprintf(stderr, "   K%d run %zu: len %.2f (w %.2f) sign %d  back %d cos %.2f  gap %.2f (%d edges)\n", K, r, runs[r].len, runs[r].w.length(), sg[r], int(back[r]), l > 0 ? runs[r].w.dot(runs[r - 1].w) / l : 0.0, gl, runs[r].i0 - runs[r - 1].i1); }
      if (!back[r]) continue; double gl = 0; bool hasOther = false; for (int i = runs[r - 1].i1; i < runs[r].i0; i++) { gl += elen(i); if (kindAt(i) == other) hasOther = true; }
      if (hasOther || gl > 0.6 * std::max(runs[r - 1].len, runs[r].len)) continue;   // (a side marked only in part: the longer of the two says what is short)
      out.gaps.push_back({P(runs[r].i0) * 2 + ac, P(runs[r - 1].i1) * 2 + ac, across * gl}); }
    // (a side running on — the palm's edge into the little finger's — one line: no step across; and the teeth all across no wider than the other kind's run
    //  they hang from (the wrist): the gaps narrowed to fit — else the fingers stick out past the palm's upright sides and the palm between is torn)
    const size_t g0 = gStart;
    for (size_t r = 1; r < runs.size(); r++) { if (back[r] || sg[r] != sg[r - 1]) continue; bool hasOther = false; for (int i = runs[r - 1].i1; i < runs[r].i0; i++) if (kindAt(i) == other) hasOther = true;
      if (!hasOther) out.gaps.push_back({P(runs[r].i0) * 2 + ac, P(runs[r - 1].i1) * 2 + ac, 0.0}); }
    { double span = 0, wrist = 0; int otherRuns = 0; for (size_t k = g0; k < out.gaps.size(); k++) span += out.gaps[k].off;
      for (int i = s - 1, guard = 0; guard < n && kindAt(i) == other; i--, guard++) wrist += elen(i);
      for (int i = 0; i < n; i++) if (kindAt(i) == other && kindAt(i + 1) != other) otherRuns++;
      if (otherRuns == 1 && wrist > 0 && std::abs(span) > wrist) { const double r = std::abs(span) / wrist; const double kg = 1 / std::sqrt(r); for (size_t j = g0; j < out.gaps.size(); j++) out.gaps[j].off *= kg; out.wristK = std::sqrt(r); out.wristC = 1 - c; } }   // (the fingers wider than the wrist: half way each — the wrist widened, the fingers narrowed, by the same factor)
  }
  return out;
}
int chainLengths(solve::Dofs& D, const std::vector<std::array<int, 3>>& align, const std::vector<Vec3>& X, const std::vector<solve::Tri>& tris, const std::vector<solve::P2>& R) {
  const auto st = chainTargets(align, X, tris, R); for (auto& s : st) D.tie(s.dq, s.dp, 1, s.off); return int(st.size());
}
// that start's few turned-over triangles (LSCM bent hard round the chains — a finger's tip by the line) eased out by the triangles' own shapes, the chains'
// points held (their ties kept): how many still turned over
int unflipStart(std::vector<solve::P2>& U, const std::vector<Vec3>& X, const std::vector<solve::Tri>& tris, const std::vector<std::array<int, 3>>& align) {
  int fl = solve::flips(U, X, tris); if (!fl) return 0;
  std::vector<char> hv(X.size(), 0); for (auto& a : align) hv[size_t(a[0])] = hv[size_t(a[1])] = 1;
  if (std::getenv("UVDBG")) { double s0 = 0; for (auto& t : tris) { auto &p = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; s0 += (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]); }
    for (auto& t : tris) { auto &p = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; const double a = (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]); if (a * s0 <= 0) std::fprintf(stderr, "   flipped tri %d %d %d chain %d%d%d area %.3g  at (%.3f %.3f) (%.3f %.3f) (%.3f %.3f)\n", t[0], t[1], t[2], hv[size_t(t[0])], hv[size_t(t[1])], hv[size_t(t[2])], a, p[0], p[1], q[0], q[1], r[0], r[1]); } }
  // (only round them: each point there the mean of its neighbours — a little Tutte inside the patch, the patch's rim, the chains and the border held — the patch grown till none is turned)
  std::vector<std::set<int>> nb(X.size()); std::map<std::pair<int, int>, int> ec;
  for (auto& t : tris) for (int j = 0; j < 3; j++) { const int a = t[size_t(j)], b = t[size_t((j + 1) % 3)]; nb[size_t(a)].insert(b); nb[size_t(b)].insert(a); ec[{std::min(a, b), std::max(a, b)}]++; }
  std::vector<char> rim(X.size(), 0); for (auto& [e, c] : ec) if (c == 1) rim[size_t(e.first)] = rim[size_t(e.second)] = 1;
  double s0 = 0; for (auto& t : tris) { auto &p = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; s0 += (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]); }
  std::set<int> bad; for (auto& t : tris) { auto &p = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; if (((q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1])) * s0 <= 0) for (int v : t) bad.insert(v); }
  const auto U0 = U;
  for (int ring = 1; ring <= 5 && fl; ring++) {
    U = U0; std::set<int> reg = bad; for (int r = 0; r < ring; r++) { std::set<int> g = reg; for (int v : reg) for (int w : nb[size_t(v)]) g.insert(w); reg = std::move(g); }
    std::vector<int> mv; for (int v : reg) if (!hv[size_t(v)]) { bool inner = true; for (int w : nb[size_t(v)]) if (!reg.count(w)) inner = false; if (inner || bad.count(v)) mv.push_back(v); }
    for (int it = 0; it < 200; it++) for (int v : mv) { double x = 0, y = 0, n = 0;   // (a point on the border: along it — the mean of its border neighbours)
      for (int w : nb[size_t(v)]) { if (rim[size_t(v)] && !(rim[size_t(w)] && ec[{std::min(v, w), std::max(v, w)}] == 1)) continue; x += U[size_t(w)][0]; y += U[size_t(w)][1]; n++; }
      if (n > 0) U[size_t(v)] = {x / n, y / n}; }
    fl = solve::flips(U, X, tris); }
  if (fl) U = U0;
  return fl;
}
}  // namespace

// A shell that folds over itself after Unfold (a strand curled round: its outline crosses itself, though no polygon is turned over): unbent step by step
// — Straighten at 35%, 65%, 100% (its shape along the strand eased, its middle and its area kept) — until nothing lies over itself. A shell that no
// amount mends is left as it was. Returns how many shells were mended; `left`: how many still fold
static int& depth() { static thread_local int d = 0; return d; }   // (the fold cut unfolds again: once, never deeper)
// The tears: a shell (cut by its seams) whose UV is in several pieces — a part of it dragged off where no seam is (the symmetry's partners pulled one by one
// in an asymmetric mesh, 2026-11-10). The biggest piece (by 3D area) stays; a piece touching the ones kept along a tear gets its points on the tear set on the
// kept side's, the rest of its points relaxed between them (where it lay, shifted by the mean of the moves) — until every piece is back
int healTears(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces) {
  if (!has(m)) return 0;
  std::vector<char> work(size_t(T.nf), faces.empty() ? 1 : 0); for (int f : faces) if (f >= 0 && f < T.nf) work[size_t(f)] = 1;
  const auto fl = seamFlags(m, T); Dsu ds(size_t(T.nf));
  for (int e = 0; e < T.ne; e++) { const int a = T.ef0[size_t(e)], b = T.ef1[size_t(e)]; if (b < 0 || fl[size_t(e)] || !work[size_t(a)] || !work[size_t(b)]) continue; ds.unite(a, b); }
  int healed = 0;
  for (int round = 0; round < 64; round++) {
    const Points P = points(m, T); const Shells S = shells(m, T, P);
    std::map<int, std::map<int, double>> pieces; for (int f = 0; f < T.nf; f++) if (work[size_t(f)]) pieces[ds.find(f)][S.of[size_t(f)]] += faceArea3(m, f);
    int did = 0;
    for (auto& [root, pc] : pieces) { if (pc.size() < 2) continue;
      int big = pc.begin()->first; for (auto& [s, a] : pc) if (a > pc[big]) big = s;
      // (a piece next to the big one along a tear: the first found)
      int mover = -1; std::vector<std::array<int, 2>> tear;   // (corner of the mover, corner of the big one, at one vertex)
      for (int e = 0; e < T.ne; e++) { const int a = T.ef0[size_t(e)], b = T.ef1[size_t(e)]; if (b < 0 || fl[size_t(e)] || !work[size_t(a)] || !work[size_t(b)] || ds.find(a) != root) continue;
        const int sa = S.of[size_t(a)], sb = S.of[size_t(b)]; if (sa == sb || (sa != big && sb != big)) continue; const int fm = sa == big ? b : a, fb = sa == big ? a : b, sm = S.of[size_t(fm)];
        if (mover < 0) mover = sm; if (sm != mover) continue;
        for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) { const int km = cornerAt(T, fm, v), kb = cornerAt(T, fb, v); if (km >= 0 && kb >= 0) tear.push_back({km, kb}); } }
      if (mover < 0 || tear.empty()) continue;
      std::vector<int> fsm; for (int f = 0; f < T.nf; f++) if (work[size_t(f)] && S.of[size_t(f)] == mover) fsm.push_back(f);
      std::map<int, std::array<double, 2>> tgt; std::map<int, int> tn; for (auto& [km, kb] : tear) { auto& t = tgt[P.of[size_t(km)]]; t[0] += m.cornerUV[size_t(kb)][0]; t[1] += m.cornerUV[size_t(kb)][1]; tn[P.of[size_t(km)]]++; }
      double sx = 0, sy = 0; std::map<int, std::array<double, 2>> pos; std::map<int, std::vector<int>> nbr;
      for (int f : fsm) { const int s0 = T.fo[size_t(f)], e0 = T.fo[size_t(f) + 1]; for (int k = s0; k < e0; k++) { const int p = P.of[size_t(k)]; if (!pos.count(p)) pos[p] = {m.cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1]};
          const int nk = k + 1 < e0 ? k + 1 : s0, pk = k > s0 ? k - 1 : e0 - 1; nbr[p].push_back(P.of[size_t(nk)]); nbr[p].push_back(P.of[size_t(pk)]); } }
      for (auto& [p, t] : tgt) { t = {t[0] / tn[p], t[1] / tn[p]}; sx += t[0] - pos[p][0]; sy += t[1] - pos[p][1]; }
      sx /= double(tgt.size()); sy /= double(tgt.size());
      for (auto& [p, q] : pos) { if (tgt.count(p)) q = tgt[p]; else q = {q[0] + sx, q[1] + sy}; }   // (the rest moved along with it first)
      for (int it = 0; it < 200; it++) for (auto& [p, q] : pos) { if (tgt.count(p)) continue; double x = 0, y = 0; for (int o : nbr[p]) { x += pos[o][0]; y += pos[o][1]; } const double nn = double(nbr[p].size()); if (nn > 0) q = {x / nn, y / nn}; }
      for (int f : fsm) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const auto& q = pos[P.of[size_t(k)]]; m.cornerUV[size_t(k)] = {float(q[0]), float(q[1])}; }
      for (auto& [km, kb] : tear) m.cornerUV[size_t(km)] = m.cornerUV[size_t(kb)];   // (exactly one point there: joined)
      did++; }
    healed += did; if (!did) break; }
  // (a slit left — torn along some edges, the piece still one: at each vertex on it, the corners of the shell set on the point most of them share)
  for (int round = 0; round < 8; round++) {
    const Points P = points(m, T); std::set<int> vs;
    for (int e = 0; e < T.ne; e++) { const int a = T.ef0[size_t(e)], b = T.ef1[size_t(e)]; if (b < 0 || fl[size_t(e)] || !work[size_t(a)] || !work[size_t(b)] || continuous(m, T, P, e)) continue; vs.insert(T.ea[size_t(e)]); vs.insert(T.eb[size_t(e)]); }
    if (vs.empty()) break;
    for (int v : vs) { std::map<int, std::map<int, std::vector<int>>> byRoot;   // (root → UV point → corners)
      for (int i = T.vfo[size_t(v)]; i < T.vfo[size_t(v) + 1]; i++) { const int f = T.vF[size_t(i)]; if (!work[size_t(f)]) continue; const int k = cornerAt(T, f, v); if (k >= 0) byRoot[ds.find(f)][P.of[size_t(k)]].push_back(k); }
      for (auto& [r, pts] : byRoot) { if (pts.size() < 2) continue; const std::vector<int>* best = nullptr; for (auto& [p, ks] : pts) if (!best || ks.size() > best->size()) best = &ks;
        const UV u = m.cornerUV[size_t((*best)[0])]; for (auto& [p, ks] : pts) for (int k : ks) m.cornerUV[size_t(k)] = u; healed++; } } }
  return healed;
}

int mendSelfOverlap(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& fs, const PackOpts* poP) {
  const PackOpts po = poP ? *poP : PackOpts{}; int left = 0; (void)left;
  if (fs.empty() || !has(m)) return 0;
  const Points P = points(m, T); const Shells S = shells(m, T, P); std::map<int, std::vector<int>> byShell; for (int f : fs) byShell[S.of[size_t(f)]].push_back(f);
  CheckOpts co; co.w = po.w; co.h = po.h; co.pad = 0; int mended = 0;
  auto overlaps = [&](const std::vector<int>& fl) { const auto rp = check(m, T, fl, co); return rp.overlap.size() + rp.flipped.size(); };   // (lying over others, or turned over: both are a fold)
  auto measure = [&](const std::vector<int>& fl, double& cx, double& cy, double& area) { double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300; area = 0;
    for (int f : fl) { area += std::abs(faceArea(m, f)); for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const auto& q = m.cornerUV[size_t(k)]; x0 = std::min(x0, double(q[0])); y0 = std::min(y0, double(q[1])); x1 = std::max(x1, double(q[0])); y1 = std::max(y1, double(q[1])); } }
    cx = (x0 + x1) / 2; cy = (y0 + y1) / 2; };
  for (auto& [sh, fl] : byShell) {
    if (overlaps(fl) == 0) continue;
    { // (only a strand — long and thin in 3D, however it curls in the UV: its length along the surface at least 3 times the side of a square of its area. A wide
      //  part folded over itself is left as it is, shown pink: straightened or laid as a strip it became a rectangle — the brooch, 2026-11-10)
      std::map<int, std::vector<std::pair<int, double>>> adj; double a3 = 0;
      for (int f : fl) { a3 += faceArea3(m, f); for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)]; const int a = T.fv[size_t(k)], b = T.fv[size_t(nk)]; const double l = m.pos[size_t(a)].distanceTo(m.pos[size_t(b)]); adj[a].push_back({b, l}); adj[b].push_back({a, l}); } }
      auto far = [&](int s, double& dmax) { std::map<int, double> d{{s, 0}}; std::set<std::pair<double, int>> q{{0, s}}; int best = s; dmax = 0;
        while (!q.empty()) { auto [dd, u] = *q.begin(); q.erase(q.begin()); if (dd > dmax) { dmax = dd; best = u; } for (auto [v, w] : adj[u]) { auto it = d.find(v); if (it == d.end() || dd + w < it->second) { if (it != d.end()) q.erase({it->second, v}); d[v] = dd + w; q.insert({dd + w, v}); } } }
        return best; };
      if (adj.empty() || !(a3 > 0)) continue; double L = 0; const int e1 = far(adj.begin()->first, L); far(e1, L);
      if (L < 3 * std::sqrt(a3)) continue; }
    std::vector<std::array<float, 2>> keep; std::vector<int> cs; for (int f : fl) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { cs.push_back(k); keep.push_back(m.cornerUV[size_t(k)]); }
    double cx0, cy0, a0; measure(fl, cx0, cy0, a0); bool ok = false;
    for (double amt : {0.35, 0.65, 1.0}) {
      for (size_t i = 0; i < cs.size(); i++) m.cornerUV[size_t(cs[i])] = keep[i];                   // (from the unfolded one each time)
      if (!straighten(m, T, fl, amt, false)) break;
      double cx1, cy1, a1; measure(fl, cx1, cy1, a1); const double k = a1 > 1e-12 && a0 > 1e-12 ? std::sqrt(a0 / a1) : 1.0;
      for (int c : cs) { auto& q = m.cornerUV[size_t(c)]; q = {float(cx0 + (q[0] - cx1) * k), float(cy0 + (q[1] - cy1) * k)}; }
      if (overlaps(fl) == 0) { ok = true; break; } }
    if (!ok) {                                                  // (straightening does not mend it — its bend too sharp: laid as a strip along its middle line, its width even — no fold in that)
      for (size_t i = 0; i < cs.size(); i++) m.cornerUV[size_t(cs[i])] = keep[i];
      if (rectangle(m, T, fl) > 0) { double cx1, cy1, a1; measure(fl, cx1, cy1, a1); const double k = a1 > 1e-12 && a0 > 1e-12 ? std::sqrt(a0 / a1) : 1.0;
        for (int c : cs) { auto& q = m.cornerUV[size_t(c)]; q = {float(cx0 + (q[0] - cx1) * k), float(cy0 + (q[1] - cy1) * k)}; }
        if (overlaps(fl) == 0) ok = true; } }
    if (!ok && depth() < 1) {                                   // (last resort: the fold cut — the polygons of the layer that lies over the other get a seam round them, and the shell is unfolded again: two shells)
      for (size_t i = 0; i < cs.size(); i++) m.cornerUV[size_t(cs[i])] = keep[i];
      const auto seamsKeep = m.seams;
      const auto rep = check(m, T, fl, co); std::set<int> badSet(rep.overlap.begin(), rep.overlap.end()); badSet.insert(rep.flipped.begin(), rep.flipped.end());
      std::map<int, int> comp; std::vector<std::vector<int>> parts;                          // (the bad polygons joined through edges: a layer each)
      for (int f0 : badSet) { if (comp.count(f0)) continue; std::vector<int> st{f0}, part; comp[f0] = int(parts.size());
        while (!st.empty()) { const int f = st.back(); st.pop_back(); part.push_back(f);
          for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int e = T.ce[size_t(k)]; const int g = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; if (g >= 0 && badSet.count(g) && !comp.count(g)) { comp[g] = int(parts.size()); st.push_back(g); } } }
        parts.push_back(std::move(part)); }
      if (parts.size() >= 1) {
        size_t pick = 0; for (size_t i = 1; i < parts.size(); i++) if (parts[i].size() < parts[pick].size()) pick = i;       // (the smaller piece: cut off)
        std::set<int> in(parts[pick].begin(), parts[pick].end()); std::set<int> shellSet(fl.begin(), fl.end()); auto fs = seamFlags(m, T); int added = 0;
        for (int f : parts[pick]) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int e = T.ce[size_t(k)]; const int g = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)];
          if (g >= 0 && shellSet.count(g) && !in.count(g) && !fs[size_t(e)]) { fs[size_t(e)] = 1; added++; } }
        if (added) { setSeams(m, T, fs); depth() = 1; unfold(m, T, fl, true, true, nullptr, nullptr, &po, nullptr); depth() = 0;
          const Points P2 = points(m, T); const Shells S2 = shells(m, T, P2); std::map<int, std::vector<int>> by2; for (int f : fl) by2[S2.of[size_t(f)]].push_back(f);
          bool all0 = true; for (auto& [s2, l2] : by2) if (overlaps(l2) > 0) all0 = false;
          if (all0) { ok = true; } else m.seams = seamsKeep; } } }
    if (ok) mended++; else { for (size_t i = 0; i < cs.size(); i++) m.cornerUV[size_t(cs[i])] = keep[i]; left++; }
  }
  return mended;
}
// An unfolded shell that came to lie on another shell: moved sideways (along U) by the least that takes it off — not away to a far place. Both ways tried, the
// nearer free place taken; a shell nothing lies under is left where it is. Returns how many were moved
int nudgeOffOthers(PolyMesh& m, const topo::TTopo& T, const std::vector<std::vector<int>>& shells) {
  if (shells.empty() || !has(m)) return 0;
  double gx0 = 1e300, gy0 = 1e300, gx1 = -1e300, gy1 = -1e300; for (const auto& q : m.cornerUV) { gx0 = std::min(gx0, double(q[0])); gy0 = std::min(gy0, double(q[1])); gx1 = std::max(gx1, double(q[0])); gy1 = std::max(gy1, double(q[1])); }
  if (!(gx1 > gx0 && gy1 > gy0)) return 0;
  const int N = 640; const double sx = N / (gx1 - gx0), sy = N / (gy1 - gy0);
  std::vector<char> mine(size_t(T.nf), 0); for (const auto& s : shells) for (int f : s) mine[size_t(f)] = 1;
  std::vector<char> occ(size_t(N) * N, 0);
  auto rast = [&](int f, auto&& visit) { const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
    for (int k = s + 1; k + 1 < e; k++) { const auto &a = m.cornerUV[size_t(s)], &b = m.cornerUV[size_t(k)], &c = m.cornerUV[size_t(k + 1)];
      const double ax = (a[0] - gx0) * sx, ay = (a[1] - gy0) * sy, bx = (b[0] - gx0) * sx, by = (b[1] - gy0) * sy, cx = (c[0] - gx0) * sx, cy = (c[1] - gy0) * sy;
      const int x0 = std::max(0, int(std::floor(std::min({ax, bx, cx})))), x1 = std::min(N - 1, int(std::ceil(std::max({ax, bx, cx})))), y0 = std::max(0, int(std::floor(std::min({ay, by, cy})))), y1 = std::min(N - 1, int(std::ceil(std::max({ay, by, cy}))));
      const double d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy); if (std::abs(d) < 1e-12) continue;
      for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) { const double px = x + 0.5, py = y + 0.5;
        const double l1 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d, l2 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d, l3 = 1 - l1 - l2;
        if (l1 >= 0 && l2 >= 0 && l3 >= 0) visit(x, y); } } };
  for (int f = 0; f < T.nf; f++) if (!mine[size_t(f)]) rast(f, [&](int x, int y) { occ[size_t(y) * N + size_t(x)] = 1; });
  int moved = 0;
  for (const auto& s : shells) {
    std::vector<std::pair<int, int>> cells; for (int f : s) rast(f, [&](int x, int y) { cells.push_back({x, y}); });
    if (cells.empty()) continue;
    auto clash = [&](int dx) { long n = 0; for (const auto& [x, y] : cells) { const int xx = x + dx; if (xx >= 0 && xx < N && occ[size_t(y) * N + size_t(xx)]) n++; } return n; };
    const long base = clash(0); if (base * 100 < long(cells.size())) continue;                // (under 1% of it on others: let it lie)
    int best = 0; bool found = false;
    for (int k = 1; k < 3 * N && !found; k++) for (int sg : {1, -1}) if (clash(sg * k) * 400 < long(cells.size())) { best = sg * k; found = true; break; }
    if (!found) continue;
    const double du = best / sx; for (int f : s) for (int c = T.fo[size_t(f)]; c < T.fo[size_t(f) + 1]; c++) m.cornerUV[size_t(c)][0] += float(du);
    for (const auto& [x, y] : cells) { const int xx = x + best; if (xx >= 0 && xx < N) occ[size_t(y) * N + size_t(xx)] = 1; }   // (where it stands now: in the way of the next)
    moved++;
  }
  return moved;
}

// Shut eyes / lips: the shell is first unfolded with them open (the natural layout), then each shut rim is laid on one straight line — upper and lower chain on it, each point by its 3D length along
// the chain, a half shut one with a lens-shaped slit — and that move dies out through the shell by springs, the shell's outer border and every pin staying where they are
static void closeFeatures(PolyMesh& m, const topo::TTopo& T, const std::vector<Feature>& feats, const std::vector<int>& faces, const Pins* pins) {
  const Points P = points(m, T); const Shells S = shells(m, T, P); std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf) want.insert(S.of[size_t(f)]);
  std::map<int, std::vector<int>> by; for (int f = 0; f < T.nf; f++) if (faces.empty() || want.count(S.of[size_t(f)])) by[S.of[size_t(f)]].push_back(f);
  using V2 = std::array<double, 2>;
  for (auto& [sh, fl] : by) {
    std::unordered_map<int, int> loc; std::vector<int> pts; auto id = [&](int k) { const int p = P.of[size_t(k)]; auto r = loc.emplace(p, int(pts.size())); if (r.second) pts.push_back(p); return r.first->second; };
    std::set<std::pair<int, int>> de; std::map<int, int> vOf; std::set<int> twice;
    for (int f : fl) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)]; const int a = id(k), b = id(nk); de.insert({a, b}); const int v = T.fv[size_t(k)]; auto r = vOf.emplace(v, a); if (!r.second && r.first->second != a) twice.insert(v); }
    const int n = int(pts.size()); auto at = [&](int i) { const UV& q = m.cornerUV[size_t(P.list[size_t(P.off[size_t(pts[size_t(i)])])])]; return V2{q[0], q[1]}; };
    std::vector<V2> U(static_cast<size_t>(n)); for (int i = 0; i < n; i++) U[size_t(i)] = at(i);
    std::vector<char> rim(static_cast<size_t>(n), 0); Dsu d(static_cast<size_t>(n)); std::vector<std::vector<std::pair<int, double>>> adj(static_cast<size_t>(n));
    auto len = [&](int a, int b) { return std::hypot(U[size_t(a)][0] - U[size_t(b)][0], U[size_t(a)][1] - U[size_t(b)][1]); };
    for (auto [a, b] : de) { const bool border = !de.count({b, a}); if (border) { rim[size_t(a)] = rim[size_t(b)] = 1; d.unite(a, b); } if (border || a < b) { const double w = 1.0 / std::max(len(a, b), 1e-7); adj[size_t(a)].push_back({b, w}); adj[size_t(b)].push_back({a, w}); } }
    std::map<int, double> blen; for (auto [a, b] : de) if (!de.count({b, a})) blen[d.find(a)] += len(a, b); int outer = -1; double bl = -1; for (auto [r, l] : blen) if (l > bl) { bl = l; outer = r; }
    std::map<int, V2> target;
    for (const Feature& ft : feats) { if (!ft.closed || ft.a.size() < 2 || ft.b.size() < 2) continue;
      auto pointOf = [&](int v) { if (twice.count(v)) return -1; const auto g = vOf.find(v); return g == vOf.end() || !rim[size_t(g->second)] || d.find(g->second) == outer ? -1 : g->second; };   // (only a hole of this shell: for the shell inside the hole the same rim is its outer border)
      bool ok = true; for (auto* c : {&ft.a, &ft.b}) for (int v : *c) if (pointOf(v) < 0) ok = false; if (!ok) continue;
      V2 p0 = U[size_t(pointOf(ft.a.front()))], p1 = U[size_t(pointOf(ft.a.back()))]; if (ft.straight) p0[1] = p1[1] = (p0[1] + p1[1]) / 2;
      const double L = std::hypot(p1[0] - p0[0], p1[1] - p0[1]);
      // (each rim point comes to the line straight across — its place along the line kept, so the rows of the mesh run on through the slit without a kink; a point of the upper chain and the one facing it on the lower: one place)
      auto along = [&](const std::vector<int>& c) { std::vector<double> t(c.size(), 0); const double l2 = std::max(L * L, 1e-18); for (size_t i = 0; i < c.size(); i++) { const V2& u = U[size_t(pointOf(c[i]))]; t[i] = std::clamp(((u[0] - p0[0]) * (p1[0] - p0[0]) + (u[1] - p0[1]) * (p1[1] - p0[1])) / l2, 0.0, 1.0); } t.front() = 0; t.back() = 1; return t; };
      std::vector<double> ta = along(ft.a), tb = along(ft.b);
      if (ta.size() == tb.size() || ta.size() + 2 == tb.size() || tb.size() + 2 == ta.size()) { auto& ts = ta.size() <= tb.size() ? ta : tb; auto& tl = ta.size() <= tb.size() ? tb : ta; const size_t sh = (tl.size() - ts.size()) / 2; for (size_t i = 1; i + 1 < ts.size(); i++) ts[i] = tl[i + sh] = (ts[i] + tl[i + sh]) / 2; if (sh) { tl[1] = 0; tl[tl.size() - 2] = 1; } }   /* (as many points on each rim, or the corner a short edge — two points more on one rim, one by each end: those go into the corners, the rest each with the one facing it, by number) */   // (as many points on each rim: each with the one facing it, by number)
      else { const double sp = 1.0 / double(std::max(ft.a.size(), ft.b.size()) - 1); std::vector<char> usedB(tb.size(), 0);
        for (size_t i = 1; i + 1 < ta.size(); i++) { int best = -1; double bd = 0.45 * sp; for (size_t j = 1; j + 1 < tb.size(); j++) if (!usedB[j] && std::abs(tb[j] - ta[i]) < bd) { bd = std::abs(tb[j] - ta[i]); best = int(j); } if (best >= 0) { usedB[size_t(best)] = 1; ta[i] = tb[size_t(best)] = (ta[i] + tb[size_t(best)]) / 2; } } }
      for (auto* t : {&ta, &tb}) { const double eps = 1e-4; for (size_t i = 1; i + 1 < t->size(); i++) (*t)[i] = std::max((*t)[i], (*t)[i - 1] + eps); for (size_t i = t->size() - 1; i-- > 1;) (*t)[i] = std::min((*t)[i], (*t)[i + 1] - eps); }   // (in order along the line)
      int lowSide = 1; { auto inward = [&](const std::vector<int>& c) { double s = 0; int n2 = 0; for (size_t i = 1; i + 1 < c.size(); i++) { const int p = pointOf(c[i]); for (auto [q, w] : adj[size_t(p)]) if (!rim[size_t(q)]) { s += U[size_t(q)][1] - U[size_t(p)][1]; n2++; } } return n2 ? s / n2 : 0.0; };   // (which way its own polygons go from the rim, in V: "upper" is by 3D height, and a mouth's corners can turn that round — then the lips came to lie over each other)
        const double ia = inward(ft.a), ib = inward(ft.b); lowSide = ia < ib ? 0 : 1; }
      int side = 0; for (auto* c : {&ft.a, &ft.b}) { const auto& tt = side ? tb : ta;
        for (size_t i = 0; i < c->size(); i++) { const double t = tt[i]; V2 q{p0[0] + (p1[0] - p0[0]) * t, p0[1] + (p1[1] - p0[1]) * t}; if (ft.gap > 0) q[1] += (side == lowSide ? -0.5 : 0.5) * ft.gap * L * 0.5 * std::pow(std::sin(M_PI * t), 0.35);   /* (the slit shared by the two rims: when only one moved its rows came closer together than the other's) */ target[pointOf((*c)[i])] = q; }
        side++; } }
    if (target.empty()) continue;
    std::vector<char> fixed(static_cast<size_t>(n), 0); for (int i = 0; i < n; i++) if (rim[size_t(i)]) fixed[size_t(i)] = 1;
    if (pins) for (int f : fl) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)]; const int a = T.fv[size_t(k)], b = T.fv[size_t(nk)]; if (pointPinned(*pins, a, f)) fixed[size_t(id(k))] = 1; if (edgePinned(*pins, a, b, f, T.ce[size_t(k)])) fixed[size_t(id(k))] = fixed[size_t(id(nk))] = 1; }
    std::vector<V2> D(static_cast<size_t>(n), V2{0, 0}); for (auto& [i, q] : target) { D[size_t(i)] = {q[0] - U[size_t(i)][0], q[1] - U[size_t(i)][1]}; fixed[size_t(i)] = 1; }
    double maxD = 0; for (auto& q : D) maxD = std::max(maxD, std::hypot(q[0], q[1]));
    for (int it = 0; it < 6000; it++) { double mv = 0; for (int i = 0; i < n; i++) { if (fixed[size_t(i)]) continue; double sx = 0, sy = 0, sw = 0; for (auto [q, w] : adj[size_t(i)]) { sx += D[size_t(q)][0] * w; sy += D[size_t(q)][1] * w; sw += w; } if (sw <= 0) continue; const V2 nv{D[size_t(i)][0] + 1.6 * (sx / sw - D[size_t(i)][0]), D[size_t(i)][1] + 1.6 * (sy / sw - D[size_t(i)][1])}; mv = std::max(mv, std::abs(nv[0] - D[size_t(i)][0]) + std::abs(nv[1] - D[size_t(i)][1])); D[size_t(i)] = nv; } if (mv < 1e-7 * std::max(maxD, 1e-9)) break; }
    std::vector<solve::P2> W(static_cast<size_t>(n)); for (int i = 0; i < n; i++) W[size_t(i)] = {U[size_t(i)][0] + D[size_t(i)][0], U[size_t(i)][1] + D[size_t(i)][1]};
    {                                                            // (the polygons round the closed rim evened out — the rims, the border and the pins stay; nothing may turn over on the way)
      std::vector<solve::Tri> tr; double au = 0, a3 = 0; for (int f : fl) { const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1]; for (int k = s + 1; k + 1 < e; k++) tr.push_back({id(s), id(k), id(k + 1)}); au += std::abs(faceArea(m, f)); a3 += faceArea3(m, f); }
      double sg = 0; for (auto& t : tr) { auto &p = W[size_t(t[0])], &q = W[size_t(t[1])], &r = W[size_t(t[2])]; sg += (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]); }
      const double sk = a3 > 0 ? std::sqrt(au / a3) : 1; std::vector<Vec3> X(static_cast<size_t>(n)); for (int i = 0; i < n; i++) X[size_t(i)] = m.pos[size_t(P.vert[size_t(pts[size_t(i)])])] * sk;
      if (sg < 0) for (auto& t : tr) std::swap(t[1], t[2]);
      std::vector<solve::Tri> trS; std::set<std::pair<int, int>> onRim; for (auto& t : tr) { bool thin = false; for (int j = 0; j < 3; j++) { auto &p = W[size_t(t[size_t(j)])], &q = W[size_t(t[size_t((j + 1) % 3)])]; if (std::hypot(p[0] - q[0], p[1] - q[1]) < 1e-9) thin = true; } if (!thin) trS.push_back(t); }
      solve::Dofs Dd(n); for (int i = 0; i < n; i++) if (fixed[size_t(i)]) Dd.holdPoint(i, W[size_t(i)]);
      auto Wn = W; if (!solve::flips(Wn, X, trS)) { const auto r = solve::slim(Wn, X, trS, Dd, 25); if (r.ok && !solve::flips(Wn, X, trS)) W = std::move(Wn); } }
    for (int i = 0; i < n; i++) { const int p = pts[size_t(i)]; for (int j = P.off[size_t(p)]; j < P.off[size_t(p) + 1]; j++) m.cornerUV[size_t(P.list[size_t(j)])] = {float(W[size_t(i)][0]), float(W[size_t(i)][1])}; }
  }
}

UnfoldResult unfold(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, bool keepPlace, bool optimize, const Pins* pins, const SymUnfold* sym, const PackOpts* po, const std::vector<Feature>* feats) {
  if (feats && std::any_of(feats->begin(), feats->end(), [](const Feature& f) { return f.closed; })) {   // (shut ones: laid out open, then closed)
    std::vector<Feature> open = *feats; for (auto& f : open) { f.closed = false; f.gap = 0; }
    const UnfoldResult r0 = unfold(m, T, faces, keepPlace, optimize, pins, sym, po, &open); closeFeatures(m, T, *feats, faces, pins); return r0; }
  // (the texture: texel-square UV in it — the shells unfolded in its space, its longer side 1; then U = X / Dw, V = Y / Dh)
  const double TM = po ? std::max(po->w, po->h) : 1, Dw = po ? po->w / TM : 1, Dh = po ? po->h / TM : 1;
  UnfoldResult R; const bool hadUV = has(m); ensure(m); const std::vector<UV> uvBefore = m.cornerUV;
  const std::vector<char> seam = seamFlags(m, T); const size_t nc = T.fv.size();
  auto cut = [&](int e) { return T.ef1[size_t(e)] < 0 || seam[size_t(e)]; };
  // the shells cut along the seams; the corners joined across what isn't cut (one local vertex)
  Dsu fu(size_t(T.nf)), cu(nc);
  for (int e = 0; e < T.ne; e++) if (!cut(e)) {
    int f0 = T.ef0[size_t(e)], f1 = T.ef1[size_t(e)]; fu.unite(f0, f1);
    for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) { int a = cornerAt(T, f0, v), b = cornerAt(T, f1, v); if (a >= 0 && b >= 0) cu.unite(a, b); }
  }
  const std::vector<char> wf = windingFlips(T, [&](int e) { return !cut(e); });   // (a ribbon turned over halfway: laid out as the one surface it is)
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
        // (cut the same way on both sides: each corner's mirror joined to the same as it — else, mirrored corner by corner, the other side's UV broke where it has
        //  no seam (a strip cut across on one sleeve, a ring on the other: its UV split with no seam there, 2026-11-10) — then each unfolded on its own)
        { bool same = true; std::map<int, int> mapC;
          for (int f : byShell[root]) { for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1] && same; k++) { const int mk = sym->mc[size_t(k)]; if (mk < 0) { same = false; break; } const int a = int(cu.find(k)), b = int(cu.find(mk)); auto it = mapC.find(a); if (it == mapC.end()) mapC[a] = b; else if (it->second != b) same = false; } if (!same) break; }
          if (same) { std::set<int> img; for (auto& [a, b] : mapC) if (!img.insert(b).second) { same = false; break; } }   // (one-to-one: two joined on this side, apart on that — or the other way)
          if (!same) { R.asymPair++; R.asymFaces.insert(R.asymFaces.end(), byShell[root].begin(), byShell[root].end()); want.insert(root); want.insert(to); continue; } }
        want.insert(root); want.insert(to);
        if (meanX(root) >= meanX(to)) follows[to] = root; else follows[root] = to;
      }
    }
  }
  std::set<int> unfolded, noMend;                               // (the leaders done: their followers mirror them; noMend: laid round pins / a held rest or with a face's features — never straightened or cut afterwards)
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
      { std::vector<int> li; for (int k = s; k < e; k++) li.push_back(cl[k]); const size_t t0 = tris.size(); addTris(m, T, f, li, tris); if (wf[size_t(f)]) for (size_t i = t0; i < tris.size(); i++) std::swap(tris[i][1], tris[i][2]); }
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
    const bool hasFt = !ft.pairs.empty(); if (hasFt) noMend.insert(root);
    std::vector<std::array<int, 2>> soft; for (auto& p : ft.pairs) { soft.push_back({p[0] * 2, p[1] * 2}); if (p[2]) soft.push_back({p[0] * 2 + 1, p[1] * 2 + 1}); } for (auto& l : ft.level) soft.push_back({l[1] * 2 + 1, l[0] * 2 + 1});   // (the same ties as dof pairs: SLIM's pull from a start that can't keep them)
    // the alignments in it (the h / v constraints): each edge's two ends one V (across) / one U (up) — ties, nothing held: the shell unfolds round
    // them as it likes and goes where its old UV was; never turned afterwards (only by half a turn, upright)
    std::vector<std::array<int, 3>> align;                        // (local a, local b, 1 across / 2 up)
    if (pins && !pins->empty()) for (int f : fs) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
      if (const int al = edgeAligned(*pins, f, T.ce[size_t(k)])) if (cl[k] != cl[nk]) align.push_back({cl[k], cl[nk], al}); }
    dropCollapsing(align, &tris, &X);
    const bool aligned = !align.empty(); if (aligned) noMend.insert(root);
    auto addAlign = [&](solve::Dofs& D) { for (auto& a : align) { const int c = a[2] == 1 ? 1 : 0; D.tie(a[1] * 2 + c, a[0] * 2 + c); } };
    auto addFt = [&](solve::Dofs& D) { for (auto& p : ft.pairs) { D.tie(p[0] * 2, p[1] * 2); if (p[2]) D.tie(p[0] * 2 + 1, p[1] * 2 + 1); } for (auto& l : ft.level) D.tie(l[1] * 2 + 1, l[0] * 2 + 1); };
    auto addTies = [&](solve::Dofs& D) { addFt(D); addAlign(D); };
    // the shell unfolded free (no h / v ties: never folded, no chain turned back), turned so its h edges lie across and its v ones up (the mean of their doubled
    // angles: an edge either way round the same) — which way each chain should run is read off it; empty: it couldn't be had flat
    std::vector<solve::P2> freeF; bool freeDone = false;
    auto freeTurned = [&]() -> const std::vector<solve::P2>& {
      if (freeDone) return freeF; freeDone = true;
      int f0 = -1; for (size_t i = 0; i < X.size(); i++) if (rim[i]) { f0 = int(i); break; } if (f0 < 0) return freeF;
      auto farR = [&](int from) { int best = from; double bd = -1; for (size_t i = 0; i < X.size(); i++) if (rim[i]) { const double d = (X[i] - X[size_t(from)]).lengthSq(); if (d > bd) { bd = d; best = int(i); } } return best; };
      const int f1 = farR(f0), f2 = farR(f1); if (f1 == f2) return freeF;
      Vec3 N; for (int f : fs) N += newell(m, f); N = N.lengthSq() > 0 ? N.normalized() : Vec3(0, 0, 1);
      Vec3 ax = std::abs(N.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0); const Vec3 bx = N.cross(ax).normalized(); ax = bx.cross(N);
      std::vector<std::array<double, 2>> g0(X.size()); for (size_t i = 0; i < X.size(); i++) g0[i] = {X[i].dot(ax), X[i].dot(bx)};
      solve::Dofs Df(int(X.size())); Df.holdPoint(f2, {0, 0}); Df.holdPoint(f1, {(X[size_t(f1)] - X[size_t(f2)]).length(), 0}); addFt(Df);
      auto F = solve::lscm(X, tris, Df, g0); if (solve::flips(F, X, tris)) return freeF;
      { solve::Dofs Dn(int(X.size())); addFt(Dn); auto F2 = F; if (solve::slim(F2, X, trisS, Dn, 30).ok && !solve::flips(F2, X, tris)) F = std::move(F2); }
      double cs = 0, sn = 0; for (auto& a : align) { const double dx = F[size_t(a[1])][0] - F[size_t(a[0])][0], dy = F[size_t(a[1])][1] - F[size_t(a[0])][1], L = std::hypot(dx, dy); if (!(L > 0)) continue;
        const double t = std::atan2(dy, dx) - (a[2] == 2 ? M_PI / 2 : 0); cs += L * std::cos(2 * t); sn += L * std::sin(2 * t); }
      const double th = -std::atan2(sn, cs) / 2, c = std::cos(th), s = std::sin(th); for (auto& q : F) q = {q[0] * c - q[1] * s, q[0] * s + q[1] * c};
      freeF = std::move(F); return freeF; };
    auto signRef = [&](const std::vector<solve::P2>& fallback) -> const std::vector<solve::P2>& { const auto& F = freeTurned(); return F.empty() ? fallback : F; };
    // pinned edges in it (their UV as it is now): the shell laid round them — at the density they give (their 3D length
    // to their UV length), in place, not turned
    if (pins && !pins->empty() && hadUV) {
      std::map<int, std::array<double, 2>> fix, ptPin; double l3 = 0, l2 = 0;
      for (int f : fs) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
        const int a = T.fv[size_t(k)], b = T.fv[size_t(nk)];
        if (pointPinned(*pins, a, f)) { const UV& u = m.cornerUV[size_t(k)]; fix[cl[k]] = {u[0] * Dw, u[1] * Dh}; ptPin[a] = {u[0] * Dw, u[1] * Dh}; }   // (a pinned point — the Pin brush's)
        if (!edgePinned(*pins, a, b, f, T.ce[size_t(k)])) continue;
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
        R.done++; R.pinned++; unfolded.insert(root); noMend.insert(root); continue;
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
    // (symmetric in itself, off the model's plane — a leaf, a bow's tail: its own mirror plane found, the shell turned so that plane is
    //  x = 0 and its long axis the height; then laid out as a shell across the plane is — both halves alike, upright, no skew, 2026-11-11)
    if (mirL.empty() && !(useSym && mirShell[root] == root)) { Vec3 Nn, Cc, Up;
      auto sm = selfMirror(X, E, Nn, Cc, Up);
      if (!sm.empty()) { const Vec3 W = Nn.cross(Up).normalized(); for (Vec3& p : X) { const Vec3 d = p - Cc; p = Vec3(d.dot(Nn), d.dot(Up), d.dot(W)); }
        mirL = std::move(sm); R.selfSym++; } }
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
      if (aligned) { solve::Dofs Ds = Dt; if (chainLengths(Ds, align, X, tris, signRef(U))) { Ds.hold(p * 2 + 1, 0); auto U2 = solve::lscm(X, tris, Ds, g); if (!unflipStart(U2, X, tris, align)) U = std::move(U2); } }   // (the h / v chains each laid one way at their length: no hairpin for SLIM to keep)
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
      const bool oldOk = oa > 1e-10 && A3 > 0 && (kOther <= 0 || oa / A3 > kOther * 1e-3);   // (its old UV a size of its own — not a point: a shell squeezed to a point stayed one, 2026-11-10)
      const double k = oldOk ? oa / A3 : kOther;   // (its own size kept — else the rest's density)
      double scale = au > 0 && A3 > 0 ? std::sqrt(A3 / au) : 1, cx = 0, cy = 0;
      if (!packAfter && k > 0 && (oldOk || (kOther > 0 && ou0 < 1e299))) { scale *= std::sqrt(k); cx = (ou0 + ou1) / 2; cy = (ov0 + ov1) / 2 - (y0 + y1) / 2 * scale; }   // (its middle line where its old box's middle was)
      else { packAfter = true; cx = -x0 * scale; cy = -y0 * scale; }
      for (size_t i = 0; i < corners.size(); i++) { auto& q = U[size_t(cornerLoc[i])]; m.cornerUV[size_t(corners[i])] = {float(q[0] * scale + cx), float(q[1] * scale + cy)}; }
      R.done++; R.symmetric++; unfolded.insert(root); noMend.insert(root); continue;   // (never straightened or laid as a strip after: a symmetric part (a brooch) came out a rectangle, 2026-11-10)
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
    solve::Dofs D(int(X.size()));
    if (aligned && !(hasFt && ft.frame[0] >= 0)) {               // (aligned: LSCM held by one aligned edge only — its first end in place, its other at its 3D length along it; the ties fix the turn)
      const auto& a = align[0]; const double d = (X[size_t(a[1])] - X[size_t(a[0])]).length(); const int c = a[2] == 1 ? 0 : 1;   // (c: the coordinate along the edge)
      const double sg = g[size_t(a[1])][size_t(c)] - g[size_t(a[0])][size_t(c)] < 0 ? -1 : 1;
      D.holdPoint(a[0], {0, 0}); addTies(D); D.hold(a[1] * 2 + c, sg * std::max(d, 1e-9)); }
    else { D.holdPoint(b0, {0, 0}); D.holdPoint(b1, {(X[size_t(b1)] - X[size_t(b0)]).length(), 0}); addTies(D); }
    auto U = solve::lscm(X, tris, D, g);
    bool chainOk = false, guided = false;   // (guided: laid by the chains' pull from the free unfold)
    if (aligned) { solve::Dofs Ds(int(X.size())); addTies(Ds); if (chainLengths(Ds, align, X, tris, signRef(U))) { Ds.holdPoint(align[0][0], {0, 0}); auto U2 = solve::lscm(X, tris, Ds, g); if (std::getenv("UVDBG")) std::fprintf(stderr, "  chain start: flips %d dist %.4f\n", solve::flips(U2, X, tris), solve::distortion(U2, X, tris)); const int fl2 = unflipStart(U2, X, tris, align); if (std::getenv("UVDBG")) std::fprintf(stderr, "  chain start eased: flips %d\n", fl2); if (!fl2 || std::getenv("UVCHAINONLY")) { U = std::move(U2); chainOk = true; } } }   // (the h / v chains each laid one way at their length: no hairpin for SLIM to keep)
    // the chains laid at their length can't be had flat at once (a hand: its middle fingers squeezed over each other): the shell unfolded free, turned so its
    // h edges lie across and its v ones up, then pulled softly — the edges level / upright, each chain at its length one way — and tied. SLIM never lets a
    // triangle turn over on the way, so no chain comes back on itself (2026-11-10, the hand's sides came out bent up)
    auto tryGuided = [&]() {
      const auto& F = freeTurned(); if (F.empty()) return;
      std::vector<std::array<int, 2>> pull; std::vector<double> off; std::vector<double> pw;
      for (auto& a : align) { const int o = a[2] == 1 ? 1 : 0; pull.push_back({a[1] * 2 + o, a[0] * 2 + o}); off.push_back(0); pw.push_back(1); }
      const auto steps = chainTargets(align, X, tris, F);
      for (auto& x : steps) { pull.push_back({x.dq, x.dp}); off.push_back(x.off); pw.push_back(0.01); }   // (the lengths only a light hint: which way each chain runs — the edges level / upright pulled hard; a shell narrower at one end than the other (a forearm to the wrist) — its ends' lengths can't both be had)
      if (std::getenv("UVDBG")) { std::fprintf(stderr, "  guided: %zu chain steps\n", steps.size());
        for (auto& a : align) { const double dx = F[size_t(a[1])][0] - F[size_t(a[0])][0], dy = F[size_t(a[1])][1] - F[size_t(a[0])][1], L = std::hypot(dx, dy); const double along = a[2] == 1 ? std::abs(dx) : std::abs(dy);
          if (L > 0 && along / L < 0.5) std::fprintf(stderr, "   across its line in F: %c edge %d-%d at (%.2f %.2f) len %.3f\n", a[2] == 1 ? 'h' : 'v', a[0], a[1], F[size_t(a[0])][0], F[size_t(a[0])][1], L); } }
      bool ok = false, pulled = false; auto Wg = F;
      for (double w : {1e3, 1e4, 1e5, 1e6}) {                       // (pulled ever harder, tied after each: the tie snaps what the pull left off its line — taken only when that crushes nothing)
        solve::Dofs D0(int(X.size())); addFt(D0); const auto r0 = solve::slim(Wg, X, trisS, D0, 150, &pull, w, &off, &pw); if (!r0.ok || solve::flips(Wg, X, tris)) break; pulled = true;
        if (std::getenv("UVDBG")) { std::fprintf(stderr, "  guided w %.0e: F dist %.4f, pulled dist %.4f (iters %d)\n", w, solve::distortion(F, X, tris), solve::distortion(Wg, X, tris), r0.iters);
          std::vector<std::pair<double, size_t>> rs; for (size_t i = 0; i < pull.size(); i++) rs.push_back({std::abs(Wg[size_t(pull[i][0] / 2)][size_t(pull[i][0] % 2)] - Wg[size_t(pull[i][1] / 2)][size_t(pull[i][1] % 2)] - off[i]), i}); std::sort(rs.rbegin(), rs.rend());
          for (size_t k = 0; k < std::min<size_t>(4, rs.size()); k++) { const size_t i = rs[k].second; std::fprintf(stderr, "   residual %.4f  pair %d-%d (w %.2f) at (%.2f %.2f)\n", rs[k].first, pull[i][0], pull[i][1], pw[i], Wg[size_t(pull[i][1] / 2)][0], Wg[size_t(pull[i][1] / 2)][1]); } }
        solve::Dofs Dh(int(X.size())); addTies(Dh); auto Wh = Wg; const auto r1 = solve::slim(Wh, X, trisS, Dh, 60);
        if (std::getenv("UVDBG")) { std::fprintf(stderr, "  guided tied: ok %d dist %.4f (iters %d)\n", int(r1.ok), solve::distortion(Wh, X, tris), r1.iters);
          size_t wi = 0; double wd = -1; for (size_t i = 0; i < Wh.size(); i++) { const double d = std::hypot(Wh[i][0] - Wg[i][0], Wh[i][1] - Wg[i][1]); if (d > wd) { wd = d; wi = i; } }
          std::fprintf(stderr, "   moved most: local %zu by %.4f  (%.2f %.2f) -> (%.2f %.2f)\n", wi, wd, Wg[wi][0], Wg[wi][1], Wh[wi][0], Wh[wi][1]);
          { solve::Dofs Dz(int(X.size())); addTies(Dz); auto Wz = Wg; solve::slim(Wz, X, trisS, Dz, 0); size_t zi = 0; double zd = -1; for (size_t i = 0; i < Wz.size(); i++) { const double d = std::hypot(Wz[i][0] - Wg[i][0], Wz[i][1] - Wg[i][1]); if (d > zd) { zd = d; zi = i; } }
            std::fprintf(stderr, "   the snap alone: local %zu by %.4f (%.2f %.2f) -> (%.2f %.2f), dist %.4f\n", zi, zd, Wg[zi][0], Wg[zi][1], Wz[zi][0], Wz[zi][1], solve::distortion(Wz, X, tris));
            std::set<int> onA; for (auto& a : align) { onA.insert(a[0]); onA.insert(a[1]); } int shown = 0;
            for (auto& t : tris) { auto &p = Wz[size_t(t[0])], &q = Wz[size_t(t[1])], &r = Wz[size_t(t[2])]; const double ar = (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]); auto &p0 = Wg[size_t(t[0])], &q0 = Wg[size_t(t[1])], &r0 = Wg[size_t(t[2])]; const double a0 = (q0[0] - p0[0]) * (r0[1] - p0[1]) - (r0[0] - p0[0]) * (q0[1] - p0[1]);
              if (std::abs(ar) < 1e-9 * std::max(1.0, std::abs(a0)) * 1e6 && shown++ < 5) std::fprintf(stderr, "    flat tri %d %d %d (on chains %d%d%d) area %.3g (was %.3g)  at (%.2f %.2f) (%.2f %.2f) (%.2f %.2f)\n", t[0], t[1], t[2], int(onA.count(t[0])), int(onA.count(t[1])), int(onA.count(t[2])), ar, a0, p[0], p[1], q[0], q[1], r[0], r[1]); } } }
        if (r1.ok && !solve::flips(Wh, X, tris) && solve::distortion(Wh, X, tris) < std::max(2 * solve::distortion(Wg, X, tris), 12.0)) { U = std::move(Wh); guided = ok = true; break; } }
      if (std::getenv("UVDBG")) std::fprintf(stderr, "  guided: pull ok %d, tied ok %d, tangled %d\n", int(pulled), int(ok), ok ? int(tangled(U, tris)) : -1); };
    if (aligned && optimize && !chainOk && !std::getenv("UVCHAINONLY")) tryGuided();
    if (std::getenv("UVDBG") && aligned) std::fprintf(stderr, "aligned shell %zu tris %zu ties: lscm flips %d dist %.4f\n", tris.size(), align.size(), solve::flips(U, X, tris), solve::distortion(U, X, tris));
    if (optimize && !guided && !std::getenv("UVCHAINONLY")) { solve::Dofs D2(int(X.size())), D2l(int(X.size())); addTies(D2);
      const bool ok = slimShell(U, X, trisS, D2, 60, true, nullptr, &weld, hasFt ? &D2l : nullptr, hasFt ? &soft : nullptr);   // (the stretch evened out — nothing held but the ties: its place comes after)
      if (std::getenv("UVDBG") && aligned) std::fprintf(stderr, "  slim ok %d flips %d dist %.4f\n", int(ok), solve::flips(U, X, tris), solve::distortion(U, X, tris)); }
    // lying over itself (a fold SLIM can't undo, or a border it let cross): from Tutte's disc (never over itself), SLIM in short runs, the last run that is still clean kept
    if (std::getenv("UVDBG")) { std::map<std::pair<int, int>, int> dd; for (auto& t : tris) for (int j = 0; j < 3; j++) dd[{t[size_t(j)], t[size_t((j + 1) % 3)]}]++; int dup = 0; for (auto& [e, c] : dd) if (c > 1) dup++; if (dup) std::fprintf(stderr, "  SAME-DIRECTION edges: %d (inconsistent winding)\n", dup); }
    if (std::getenv("UVDBG")) std::fprintf(stderr, "shell %zu tris: flips %d tangled %d\n", tris.size(), solve::flips(U, X, tris), int(tangled(U, tris)));
    if (optimize && aligned && !guided && !std::getenv("UVCHAINONLY") && (solve::flips(U, X, tris) || tangled(U, tris))) tryGuided();   // (lying over itself with its chains laid: the pull from the free unfold tried before Tutte's disc — the disc may turn a chain back)
    if (optimize && !hasFt && !guided && !std::getenv("UVCHAINONLY") && (solve::flips(U, X, tris) || tangled(U, tris))) {
      auto W = solve::tutte(X, tris);
      if (std::getenv("UVDBG")) std::fprintf(stderr, "  rescue %zu tris: tutte empty %d flips %d tangled %d\n", tris.size(), int(W.empty()), W.empty() ? -1 : solve::flips(W, X, tris), W.empty() ? -1 : int(tangled(W, tris)));
      if (!W.empty() && !solve::flips(W, X, tris) && !tangled(W, tris)) {
        solve::Dofs D3(int(X.size())); auto best = W; bool keepsAlign = !aligned;
        if (aligned) {                                            // (the alignments kept from the disc on, when they can be: first a strong pull towards them, then tied)
          solve::Dofs D3a(int(X.size())), D0(int(X.size())); addAlign(D3a);
          std::vector<std::array<int, 2>> pull; for (auto& a : align) { const int c = a[2] == 1 ? 1 : 0; pull.push_back({a[0] * 2 + c, a[1] * 2 + c}); }
          auto Ws = W; const auto r0 = solve::slim(Ws, X, tris, D0, 60, &pull);
          for (auto* st : {&Ws, &W}) { if (st == &Ws && !r0.ok) continue; auto Wa = *st; const auto r = solve::slim(Wa, X, tris, D3a, 60);
            if (r.ok && !solve::flips(Wa, X, tris) && !tangled(Wa, tris)) { best = std::move(Wa); D3 = D3a; keepsAlign = true; break; } }
          if (std::getenv("UVDBG")) std::fprintf(stderr, "  rescue keeps the alignments: %d\n", int(keepsAlign)); }
        if (keepsAlign) {                                         // (never the user's h / v constraints dropped for an untangled one: they can't be kept — left as SLIM laid it, lines straight, the overlap shown pink — a seam there)
          for (int run = 0; run < 14; run++) { auto Wn = best; const auto r = solve::slim(Wn, X, tris, D3, 6); if (!r.ok || solve::flips(Wn, X, tris) || tangled(Wn, tris)) break; best = std::move(Wn); if (r.iters < 6 || std::abs(r.e0 - r.e1) < 1e-6 * std::max(1.0, std::abs(r.e0))) break; }
          U = std::move(best); R.untangled++; } } }
    // fingers over each other (a palm with its fingers' sides upright): laid evenly side by side from the layout as it is — their order along the border already right
    // there, so nothing has to pass through anything: each two beside each other (a fingertip, a web) pulled apart by its 3D width; all of them wider
    // than the wrist they hang from — the wrist widened and the fingers narrowed half way each (2026-11-10, the user's call)
    for (int pass = 0; pass < 4 && aligned && optimize && !std::getenv("UVCHAINONLY") && !solve::flips(U, X, tris) && tangled(U, tris); pass++) {   // (a few passes, while any lies over another)
      double so = 0; for (auto& t : tris) { auto &p = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; so += (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]); }
      const auto st = chainTargets(align, X, tris, U); const Comb cb = combTargets(align, X, tris, st, so < 0 ? -1 : 1);
      if (!cb.gaps.empty()) {
        std::vector<std::array<int, 2>> pull; std::vector<double> off;
        int vote = 0; for (auto& x : cb.gaps) if (x.off != 0) { const double d = U[size_t(x.dq / 2)][size_t(x.dq % 2)] - U[size_t(x.dp / 2)][size_t(x.dp % 2)]; vote += d > 0 ? 1 : d < 0 ? -1 : 0; }
        const double way = vote < 0 ? -1 : 1;                       // (a comb's gaps all go one way across: the way most of them already go — the one or two fingers lying over the next turned the same way)
        // (every pair — a fingertip, a web — pulled to its width: the ones over each other apart, the ones too far apart (a little finger off on its own)
        //  together — the fingers evenly side by side, as the user lays them by hand; the wrist free to give way)
        for (auto& x : cb.gaps) { if (x.off == 0) continue; pull.push_back({x.dq, x.dp}); off.push_back(way * std::abs(x.off)); }
        solve::Dofs Dh(int(X.size())); addTies(Dh); auto W = U; const auto r = solve::slim(W, X, trisS, Dh, 120, &pull, 1e4, &off);
        const bool ok = r.ok && !solve::flips(W, X, tris);
        if (std::getenv("UVDBG")) { std::fprintf(stderr, "  spread: %zu gaps, wrist x%.2f, ok %d, tangled %d\n", cb.gaps.size(), cb.wristK, int(ok), ok ? int(tangled(W, tris)) : -1);
          for (size_t i = 0; i < pull.size(); i++) std::fprintf(stderr, "   pull %d-%d target %.3f  before %.3f after %.3f  (p at %.2f %.2f)\n", pull[i][0], pull[i][1], off[i], U[size_t(pull[i][0] / 2)][size_t(pull[i][0] % 2)] - U[size_t(pull[i][1] / 2)][size_t(pull[i][1] % 2)], W[size_t(pull[i][0] / 2)][size_t(pull[i][0] % 2)] - W[size_t(pull[i][1] / 2)][size_t(pull[i][1] % 2)], U[size_t(pull[i][1] / 2)][0], U[size_t(pull[i][1] / 2)][1]); }
        if (!ok || pull.empty()) break; U = std::move(W); } else break; }
    // still lying over itself (toes squeezed between upright sides — no chains of their own to space them): its parts pushed apart, the ties kept
    if (aligned && optimize && !std::getenv("UVCHAINONLY") && !solve::flips(U, X, tris) && tangled(U, tris)) { solve::Dofs Dp(int(X.size())); addTies(Dp); pushApart(U, X, tris, trisS, Dp); }
    // turned the right way out, to its smallest box, sized to its 3D area
    double sa = 0; for (auto& t : tris) { auto &p = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; sa += (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]); }
    if (sa < 0) for (auto& q : U) q[0] = -q[0];
    if (aligned && hadUV) {                                       // (aligned: the way it lay before — half a turn at most, its lines stay level / upright)
      std::vector<solve::P2> O(X.size()); for (size_t i = 0; i < corners.size(); i++) { const UV& q = m.cornerUV[size_t(corners[i])]; O[size_t(cornerLoc[i])] = {q[0] * Dw, q[1] * Dh}; }
      double a0[2] = {0, 0}, b0[2] = {0, 0}; const double n = double(X.size()); for (size_t i = 0; i < X.size(); i++) { a0[0] += U[i][0] / n; a0[1] += U[i][1] / n; b0[0] += O[i][0] / n; b0[1] += O[i][1] / n; }
      double c = 0; for (size_t i = 0; i < X.size(); i++) c += (U[i][0] - a0[0]) * (O[i][0] - b0[0]) + (U[i][1] - a0[1]) * (O[i][1] - b0[1]); if (c < 0) for (auto& q : U) q = {-q[0], -q[1]}; }
    else if (hasFt || aligned) { double mv = 0, my = 0; for (size_t i = 0; i < X.size(); i++) { mv += U[i][1]; my += X[i].y; } mv /= double(X.size()); my /= double(X.size());   // (features: kept level — upright, not turned to a box)
      double c = 0; for (size_t i = 0; i < X.size(); i++) c += (U[i][1] - mv) * (X[i].y - my); if (c < 0) for (auto& q : U) q = {-q[0], -q[1]}; }
    else {
      // upright: turned so the model's up (+Y) goes up V — the way the model stands, not the way a box fits it. A shell with hardly any height (a lid, a
      // plate lying flat) takes the model's front (+Z) for its up: as the lid of a box, its front edge at the top of the shell. Neither — the box that fits it
      double mu = 0, mv = 0; const size_t nu = U.size(); for (size_t i = 0; i < nu; i++) { mu += U[i][0]; mv += U[i][1]; } mu /= double(nu); mv /= double(nu);
      auto turnUp = [&](bool useZ) -> bool {
        double mh = 0; for (size_t i = 0; i < nu; i++) mh += useZ ? X[i].z : X[i].y; mh /= double(nu);
        double gx = 0, gy = 0, uu = 0, hh = 0; for (size_t i = 0; i < nu; i++) { const double du = U[i][0] - mu, dv = U[i][1] - mv, dh = (useZ ? X[i].z : X[i].y) - mh; gx += du * dh; gy += dv * dh; uu += du * du + dv * dv; hh += dh * dh; }
        const double gl = std::hypot(gx, gy);
        if (!(uu > 1e-18 && hh > 1e-18 && gl / std::sqrt(uu * hh) > 0.35)) return false;
        const double t = M_PI / 2 - std::atan2(gy, gx), c = std::cos(t), s = std::sin(t); for (auto& q : U) q = {q[0] * c - q[1] * s, q[0] * s + q[1] * c}; return true; };
      const bool upright = turnUp(false) || turnUp(true);
      if (!upright) {
      auto H = hull(U); double bestA = 1e300, bestT = 0;
      for (size_t i = 0; i < H.size(); i++) { auto& p = H[i]; auto& q = H[(i + 1) % H.size()]; double t = std::atan2(q[1] - p[1], q[0] - p[0]), c = std::cos(t), s = std::sin(t);
        double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300; for (auto& h : H) { double x = h[0] * c + h[1] * s, y = -h[0] * s + h[1] * c; x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
        if ((x1 - x0) * (y1 - y0) < bestA) { bestA = (x1 - x0) * (y1 - y0); bestT = t; } }
      double c = std::cos(bestT), s = std::sin(bestT); for (auto& q : U) q = {q[0] * c + q[1] * s, -q[0] * s + q[1] * c}; } }
    if (Dw != 1 || Dh != 1) for (auto& q : U) q = {q[0] / Dw, q[1] / Dh};   // (into the texture's UV: texel-square)
    double au = 0; for (auto& t : tris) { auto &p = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; au += std::abs((q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1])) / 2; }
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300; for (auto& q : U) { x0 = std::min(x0, q[0]); y0 = std::min(y0, q[1]); x1 = std::max(x1, q[0]); y1 = std::max(y1, q[1]); }
    // where it goes: its old UV's place at the others' density (keepPlace), else from 0, 0 (packed after)
    double ou0 = 1e300, ov0 = 1e300, ou1 = -1e300, ov1 = -1e300, oa = 0;
    if (hadUV) for (int f : fs) { oa += std::abs(faceArea(m, f)); for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const UV& q = m.cornerUV[size_t(k)]; ou0 = std::min(ou0, double(q[0])); ov0 = std::min(ov0, double(q[1])); ou1 = std::max(ou1, double(q[0])); ov1 = std::max(ov1, double(q[1])); } }
    const bool oldOk = oa > 1e-10 && A3 > 0 && (kOther <= 0 || oa / A3 > kOther * 1e-3);   // (its old UV a size of its own — not a point: a shell squeezed to a point stayed one, 2026-11-10)
    double k = oldOk ? oa / A3 : kOther;   // (its own size kept — else the rest's density)
    double scale = au > 0 && A3 > 0 ? std::sqrt(A3 / au) : 1, cx = 0, cy = 0;
    if (!packAfter && k > 0 && (oldOk || (kOther > 0 && ou0 < 1e299))) { scale *= std::sqrt(k); cx = (ou0 + ou1) / 2 - (x0 + x1) / 2 * scale; cy = (ov0 + ov1) / 2 - (y0 + y1) / 2 * scale; }
    else { packAfter = true; cx = -x0 * scale; cy = -y0 * scale; }
    for (size_t i = 0; i < corners.size(); i++) { auto& q = U[size_t(cornerLoc[i])]; m.cornerUV[size_t(corners[i])] = {float(q[0] * scale + cx), float(q[1] * scale + cy)}; }
    R.done++; unfolded.insert(root);
  }
  // a shell with polygons turned over (a fold: it lies over itself) is eased out by Optimize — SLIM lets none stay turned
  if (!unfolded.empty()) {
    for (int r : unfolded) { const auto& fs = byShell[r]; int pos = 0, neg = 0;
      for (int f : fs) { const double a = faceArea(m, f) * (wf[size_t(f)] ? -1 : 1); if (a > 0) pos++; else if (a < 0) neg++; }
      if (std::min(pos, neg) > 0) { hm::uv::optimize(m, T, fs, pins, 80, po, feats); } }
  }
  if (!unfolded.empty()) { std::vector<int> uf; for (int r : unfolded) if (!noMend.count(r)) for (int f : byShell[r]) uf.push_back(f); R.mended = mendSelfOverlap(m, T, uf, po); }   // (a shell that still folds over itself: unbent step by step)
  if (!unfolded.empty() && !packAfter && !R.pinned && R.mended > 0) {       // (only after a fold cut — it made two of one: the new piece moved off the others. A shell unfolded in its place stays there, over others or not — it was pushed off to the side at every Unfold, 2026-11-10; a shell laid round pins / a held rest stays exactly in place — not moved sideways)
    std::vector<char> was(size_t(T.nf), 0); for (int r : unfolded) for (int f : byShell[r]) was[size_t(f)] = 1;
    const Points Pn = points(m, T); const Shells Sn = shells(m, T, Pn); std::set<int> nowIds; for (int f = 0; f < T.nf; f++) if (was[size_t(f)]) nowIds.insert(Sn.of[size_t(f)]);
    std::vector<std::vector<int>> us(nowIds.size()); std::map<int, size_t> at; for (int id : nowIds) { const size_t k = at.size(); at[id] = k; }
    for (int f = 0; f < T.nf; f++) { const auto it = at.find(Sn.of[size_t(f)]); if (it != at.end()) us[it->second].push_back(f); }
    nudgeOffOthers(m, T, us); }   // (kept in its place, but not on another shell: moved sideways by the least)
  // (a half shut eye / lips: its slit is made in closeFeatures — here every feature comes open)
  // the followers: their leader's UV mirrored across U = axis
  for (auto& [fr, ld] : follows) {
    if (!unfolded.count(ld)) continue;
    for (int f : byShell[fr]) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int mk = sym->mc[size_t(k)]; if (mk < 0) continue;
      const UV& q = m.cornerUV[size_t(mk)]; m.cornerUV[size_t(k)] = {float(2 * sym->axis - q[0]), q[1]}; }
    R.done++; R.mirrored++;
  }
  // (a shell laid flat into a line — most of its polygons with no UV area: a ring with no seam across it (a sleeve's hem strip cut off whole) unfolds so — left
  //  as it was before, and told: a seam across it is needed. It came out as two points, its polygons stretched over the whole tile, 2026-11-10)
  if (hadUV) for (auto& [root, fs2] : byShell) { if (!unfolded.count(root) && !follows.count(root)) continue; if (follows.count(root) && !unfolded.count(follows[root])) continue;
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300; for (int f : fs2) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { x0 = std::min(x0, double(m.cornerUV[size_t(k)][0])); x1 = std::max(x1, double(m.cornerUV[size_t(k)][0])); y0 = std::min(y0, double(m.cornerUV[size_t(k)][1])); y1 = std::max(y1, double(m.cornerUV[size_t(k)][1])); }
    // (or blown up: a point not a number, or the shell thrown far past the tile — many times its old size. Left as it was: one such shell hung
    //  the program on drawing, its edges spanning millions of cells, 2026-11-10)
    //  (only the plain cases: before packing a shell lies at the scene's scale, tens of tiles across, and that is fine)
    const bool blown = !std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) || !std::isfinite(y1) || std::max({std::abs(x0), std::abs(x1), std::abs(y0), std::abs(y1)}) > 1e4;
    const double box = std::max(1e-18, (x1 - x0) * (y1 - y0)); int flat = 0; for (int f : fs2) if (std::abs(faceArea(m, f)) < 1e-7 * box) flat++;
    if (!blown && (fs2.size() < 2 || flat * 2 < int(fs2.size()))) continue;
    for (int f : fs2) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) m.cornerUV[size_t(k)] = uvBefore[size_t(k)];
    if (blown) R.blown++; else { R.collapsed++; R.collapsedFaces.insert(R.collapsedFaces.end(), fs2.begin(), fs2.end()); } if (R.done > 0) R.done--; }
  if (R.done && packAfter) { std::vector<int> pf; if (!all) for (int r : want) for (int f : byShell[r]) pf.push_back(f);   // (a pair asked by one shell: both packed)
    PackOpts o = po ? *po : PackOpts{}; o.rotate = o.rotate && !R.pinned; o.freeTurn = o.freeTurn && !R.pinned; if (useSym && sym->pairs) { o.mc = &sym->mc; o.axis = sym->axis; }   // (pinned shells: never turned; Symmetry: packed symmetric)
    if (!packShapes(m, T, pf, o).ok) pack(m, T, pf, 0.01, !R.pinned && !R.symmetric && !R.mirrored); }   // (by shape; the boxes if it can't)
  return R;
}

// ---------- Optimize ----------
// (as RizomUV's Optimize: the UV as it is — its shells where they lie, their size, no seams cut, nothing unfolded anew —
// by SLIM: the stretch evened out, no triangle turned over on the way; pinned points held. Turned-over triangles to
// start with: eased out first; still there — a shell with nothing pinned laid anew from Tutte's disc, fitted to where
// it lay; else left)
double selfSymError(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& fs) {
  if (!has(m) || fs.empty()) return -1;
  const Points P = points(m, T); std::unordered_map<int, int> loc; std::vector<int> pt;
  auto L = [&](int k) { const int p = P.of[size_t(k)]; auto r = loc.emplace(p, int(pt.size())); if (r.second) pt.push_back(p); return r.first->second; };
  std::set<std::pair<int, int>> E; for (int f : fs) { const int s0 = T.fo[size_t(f)], e0 = T.fo[size_t(f) + 1]; for (int k = s0; k < e0; k++) { const int a = L(k), b = L(k + 1 < e0 ? k + 1 : s0); if (a != b) E.insert({std::min(a, b), std::max(a, b)}); } }
  std::vector<Vec3> X(pt.size()); std::vector<std::array<double, 2>> U(pt.size());
  for (size_t i = 0; i < pt.size(); i++) { X[i] = m.pos[size_t(P.vert[size_t(pt[i])])]; const UV& q = m.cornerUV[size_t(P.list[size_t(P.off[size_t(pt[i])])])]; U[i] = {q[0], q[1]}; }
  Vec3 sn, sc, su; const auto mir = selfMirror(X, E, sn, sc, su); if (mir.empty()) return -1;
  double C2 = 0, S2 = 0; for (size_t i = 0; i < mir.size(); i++) { const int j = mir[i]; if (j <= int(i)) continue; const double ex = U[i][0] - U[size_t(j)][0], ey = U[i][1] - U[size_t(j)][1], l = std::hypot(ex, ey); if (l > 0) { const double a = std::atan2(ey, ex); C2 += l * std::cos(2 * a); S2 += l * std::sin(2 * a); } }
  const double th = std::atan2(S2, C2) / 2, c = std::cos(-th), s = std::sin(-th);
  double mx = 0, my = 0; for (size_t i = 0; i < mir.size(); i++) { mx += (U[i][0] + U[size_t(mir[i])][0]) / 2; my += (U[i][1] + U[size_t(mir[i])][1]) / 2; } mx /= double(mir.size()); my /= double(mir.size());
  for (auto& q : U) { const double x = q[0] - mx, y = q[1] - my; q = {x * c - y * s, x * s + y * c}; }
  double e2 = 0, x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300; for (auto& q : U) { x0 = std::min(x0, q[0]); x1 = std::max(x1, q[0]); y0 = std::min(y0, q[1]); y1 = std::max(y1, q[1]); }
  for (size_t i = 0; i < mir.size(); i++) { const auto& a = U[i]; const auto& b = U[size_t(mir[i])]; e2 += (a[0] + b[0]) * (a[0] + b[0]) + (a[1] - b[1]) * (a[1] - b[1]); }
  const double size = std::max(x1 - x0, y1 - y0); return size > 0 ? std::sqrt(e2 / double(mir.size())) / size : -1;
}

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
      const int a = T.fv[size_t(k)], b = T.fv[size_t(nk)]; if (pointPinned(*pins, a, f)) { held[size_t(L(k))] = 1; anyHeld = true; }
      if (edgePinned(*pins, a, b, f, T.ce[size_t(k)])) { held[size_t(L(k))] = held[size_t(L(nk))] = 1; anyHeld = true; } }
    double sa = 0; for (auto& t : tris) { auto &p0 = U[size_t(t[0])], &q = U[size_t(t[1])], &r = U[size_t(t[2])]; sa += (q[0] - p0[0]) * (r[1] - p0[1]) - (r[0] - p0[0]) * (q[1] - p0[1]); }
    const bool mir = sa < 0; if (mir) for (auto& q : U) q[0] = -q[0];  // (laid mirrored: relaxed as it lies)
    double c0[2] = {0, 0}; for (auto& q : U) { c0[0] += q[0]; c0[1] += q[1]; }
    std::vector<solve::P2> U0 = U;
    solve::Dofs D(int(pt.size())); for (size_t i = 0; i < pt.size(); i++) if (held[i]) D.holdPoint(int(i), U[i]);
    bool aligned = false; std::array<int, 3> al0{-1, -1, 0}; std::vector<std::array<int, 3>> alO;          // (the alignments: their edges kept level / upright — tied, not held)
    if (pins && !pins->empty()) for (int f : fs) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T.fo[size_t(f) + 1] ? k + 1 : T.fo[size_t(f)];
      if (const int al = edgeAligned(*pins, f, T.ce[size_t(k)])) { const int a = L(k), b = L(nk); if (a == b) continue; alO.push_back({a, b, al}); } }
    dropCollapsing(alO, &tris, &X);   // (an edge both of whose ends one chain of the other kind holds: left out — it would be a point)
    for (auto& e : alO) { const int c = e[2] == 1 ? 1 : 0; D.tie(e[1] * 2 + c, e[0] * 2 + c); aligned = true; if (al0[0] < 0) al0 = e; }
    // (symmetric in itself — a leaf, a bow's tail off the model's plane: relaxed with its halves tied, as Unfold lays it. Its axis as it lies in
    //  the UV found from the mirror pairs, the shell turned so that axis is upright, both halves made alike, the ties kept by the solve, turned
    //  back. Not when held (pins), nor when aligned and not upright already — the turn would break its lines, 2026-11-11)
    std::vector<int> smir; double rTh = 0, rCx = 0, rCy = 0;
    if (!anyHeld && X.size() >= 6) {
      std::set<std::pair<int, int>> E; for (int f : fs) { const int s0 = T.fo[size_t(f)], e0 = T.fo[size_t(f) + 1]; for (int k = s0; k < e0; k++) { const int a = L(k), b = L(k + 1 < e0 ? k + 1 : s0); if (a != b) E.insert({std::min(a, b), std::max(a, b)}); } }
      Vec3 sn, sc, su; smir = selfMirror(X, E, sn, sc, su);
      if (!smir.empty()) { double C2 = 0, S2 = 0; int np = 0; for (size_t i = 0; i < smir.size(); i++) { const int j = smir[i]; if (j <= int(i)) continue;
          const double ex = U[i][0] - U[size_t(j)][0], ey = U[i][1] - U[size_t(j)][1], l = std::hypot(ex, ey); if (!(l > 0)) continue; const double a = std::atan2(ey, ex); C2 += l * std::cos(2 * a); S2 += l * std::sin(2 * a); np++; }
        if (!np) smir.clear();
        else { rTh = std::atan2(S2, C2) / 2;                               // (the pairs' direction: across the axis — turned onto U)
          if (aligned && std::abs(rTh) > 0.02 && std::abs(std::abs(rTh) - M_PI) > 0.02) smir.clear(); } }
      if (!smir.empty()) { double mx = 0, my = 0; for (size_t i = 0; i < smir.size(); i++) { const int j = smir[i]; mx += (U[i][0] + U[size_t(j)][0]) / 2; my += (U[i][1] + U[size_t(j)][1]) / 2; } rCx = mx / double(smir.size()); rCy = my / double(smir.size());
        const double c = std::cos(-rTh), s = std::sin(-rTh);
        for (auto* V : {&U, &U0}) for (auto& q : *V) { const double x = q[0] - rCx, y = q[1] - rCy; q = {x * c - y * s, x * s + y * c}; }
        for (size_t i = 0; i < smir.size(); i++) { const int j = smir[i]; if (j < int(i)) continue; if (j == int(i)) { U[i][0] = 0; continue; }
          const double u = (U[i][0] - U[size_t(j)][0]) / 2, v = (U[i][1] + U[size_t(j)][1]) / 2; U[i] = {u, v}; U[size_t(j)] = {-u, v}; }
        for (size_t i = 0; i < smir.size(); i++) { const int j = smir[i]; if (j == int(i)) D.tie(int(i) * 2, int(i) * 2, -1, 0); else if (int(i) < j) { D.tie(j * 2, int(i) * 2, -1, 0); D.tie(j * 2 + 1, int(i) * 2 + 1, 1, 0); } } }
    }
    std::vector<solve::Tri> trisS = tris; std::vector<std::array<int, 2>> weld;   // (the face's features: their ties, as Unfold has them)
    if (feats && !feats->empty()) { std::vector<int> vloc(m.pos.size(), -1); for (size_t i = 0; i < pt.size(); i++) { int& w = vloc[size_t(P.vert[size_t(pt[i])])]; w = w == -1 ? int(i) : -2; } for (int& w : vloc) if (w < 0) w = -1;
      const auto inner = innerRims(int(X.size()), tris, X); std::vector<Feature> openF;   // (a shut rim stays exactly as Unfold laid it: held, not tied)
      for (const Feature& fe : *feats) { if (!fe.closed) { openF.push_back(fe); continue; } for (auto* c : {&fe.a, &fe.b}) for (int v : *c) { const int i0 = vloc[size_t(v)]; if (i0 >= 0 && size_t(i0) < inner.size() && inner[size_t(i0)]) { held[size_t(i0)] = 1; anyHeld = true; D.holdPoint(i0, U[size_t(i0)]); } } }
      const FeatureTies ft = featureTies(openF, vloc, m.pos, &inner); std::set<std::pair<int, int>> pr;
      for (auto& p : ft.pairs) { D.tie(p[0] * 2, p[1] * 2); if (p[2]) D.tie(p[0] * 2 + 1, p[1] * 2 + 1); weld.push_back({p[0], p[1]}); pr.insert({std::min(p[0], p[1]), std::max(p[0], p[1])}); }
      for (auto& l : ft.level) D.tie(l[1] * 2 + 1, l[0] * 2 + 1);
      if (!pr.empty()) { trisS.clear(); for (auto& t : tris) { bool both = false; for (int j = 0; j < 3; j++) { const int a = t[size_t(j)], b = t[size_t((j + 1) % 3)]; if (pr.count({std::min(a, b), std::max(a, b)})) both = true; } if (!both) trisS.push_back(t); } } }
    if (solve::flips(U, X, trisS)) shapeRelax(U, X, trisS, 30, &held);   // (turned over: eased out first)
    if (!slimShell(U, X, trisS, D, iters, false)) {
      bool laid = false;
      if (aligned && !anyHeld) {                                      // (aligned, its lines not straight yet: laid anew round them — LSCM held by one aligned edge, the ties fixing the turn — then relaxed)
        const int a = al0[0], b = al0[1], c = al0[2] == 1 ? 0 : 1; const double d = (X[size_t(b)] - X[size_t(a)]).length(), sg = U0[size_t(b)][size_t(c)] - U0[size_t(a)][size_t(c)] < 0 ? -1 : 1;
        solve::Dofs Dl = D; Dl.holdPoint(a, U0[size_t(a)]); Dl.hold(b * 2 + c, U0[size_t(a)][size_t(c)] + sg * std::max(d, 1e-9));
        auto W = solve::lscm(X, tris, Dl, U0); if (slimShell(W, X, trisS, D, iters, true, nullptr, &weld)) { U = std::move(W); laid = true; } }
      if (!laid) {
        if (anyHeld || !slimShell(U, X, trisS, D, iters, true, nullptr, &weld)) { U = U0; continue; }   // (still turned over, pinned: left)
        if (!aligned) fitTo(U, U0); }                                 // (aligned: not turned — its lines stay level / upright)
    }
    if (!smir.empty()) { const double c = std::cos(rTh), s = std::sin(rTh); for (auto& q : U) { const double x = q[0], y = q[1]; q = {x * c - y * s + rCx, x * s + y * c + rCy}; } }   // (turned back as it lay)
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
  const double hi0 = hi;
  for (int i = 0; i < 50; i++) { double mid = (lo + hi) / 2; if (tryScale(mid, false)) lo = mid; else hi = mid; }
  if (lo <= 0 && pad > 0) { pad = 0; hi = hi0; for (int i = 0; i < 50; i++) { double mid = (lo + hi) / 2; if (tryScale(mid, false)) lo = mid; else hi = mid; } }   // (no scale fits with the padding — too many shells: laid without it, not left one on another)
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
  std::vector<double> a(size_t(T.nf)), sum(size_t(S.count), 0); const Points pts = points(m, T);
  const std::vector<char> wf = windingFlips(T, [&](int e) { return continuous(m, T, pts, e); });   // (a face wound against its neighbours is expected turned: not a flip)
  for (int f = 0; f < T.nf; f++) { a[size_t(f)] = faceArea(m, f) * (wf[size_t(f)] ? -1 : 1); sum[size_t(S.of[size_t(f)])] += a[size_t(f)]; }
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
int rectangle(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, int mode) {
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
    { double ua = 0; for (auto& L0 : loops) { double a = 0; for (size_t k = 0; k < L0.size(); k++) { const auto& p = orig[size_t(L0[k])]; const auto& q = orig[size_t(L0[(k + 1) % L0.size()])]; a += p[0] * q[1] - q[0] * p[1]; } ua += std::abs(a) / 2; }
      if (ua < 1e-10) continue; }   // (no UV yet — not unfolded, or squeezed to a point: its sizes come from its UV, the rectangle would be a point too (the sleeve, 2026-11-10) — left; Unfold it first)
    auto dijkstra = [&](int src) { std::vector<double> d(static_cast<size_t>(n), 1e300); using Q = std::pair<double, int>; std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q; d[size_t(src)] = 0; q.push({0, src});
      while (!q.empty()) { auto [dd, u] = q.top(); q.pop(); if (dd > d[size_t(u)]) continue; for (auto [v, w] : adj[size_t(u)]) if (dd + w < d[size_t(v)]) { d[size_t(v)] = dd + w; q.push({d[size_t(v)], v}); } }
      return d; };
    auto far = [&](const std::vector<double>& d) { int b = 0; for (int i = 0; i < n; i++) if (d[size_t(i)] < 1e299 && d[size_t(i)] > d[size_t(b)]) b = i; return b; };
    const int a0 = far(dijkstra(0)); const auto dA = dijkstra(a0); const double L = dA[size_t(far(dA))]; if (L <= 0) continue;
    // (a shell that is a disc with one border: a true rectangle — the border laid on the rectangle's perimeter: the two ends of the strand (their caps — the run of border round each tip that runs across the strand)
    //  the short sides, the two long sides each by its length; the inside between them by springs (Tutte: nothing turned over). Else — the way below)
    bool nw = false; std::vector<V2> outN; double rotC = 1, rotS = 0;
    { std::set<std::pair<int, int>> de; for (auto& L0 : loops) for (size_t k = 0; k < L0.size(); k++) de.insert({L0[k], L0[(k + 1) % L0.size()]});
      std::map<int, int> nxt; bool multi = false; int nb = 0; for (auto& e : de) if (!de.count({e.second, e.first})) { if (nxt.count(e.first)) multi = true; nxt[e.first] = e.second; nb++; }
      if (!multi && nb >= 4) {
        std::vector<int> cyc; { int st = nxt.begin()->first, c = st; do { cyc.push_back(c); c = nxt.count(c) ? nxt[c] : st; } while (c != st && cyc.size() <= size_t(nb) + 1); if (c != st || int(cyc.size()) != nb) cyc.clear(); }
        if (!cyc.empty()) {
          auto P2 = [&](int i) -> const V2& { return orig[size_t(cyc[size_t(((i % nb) + nb) % nb)])]; };
          auto nearB = [&](int p) { int b = 0; double bd = 1e300; for (int i = 0; i < nb; i++) { const double d = std::hypot(P2(i)[0] - orig[size_t(p)][0], P2(i)[1] - orig[size_t(p)][1]); if (d < bd) { bd = d; b = i; } } return b; };
          int ia = nearB(a0), ib = nearB(far(dA)); if (P2(ib)[1] < P2(ia)[1]) std::swap(ia, ib);            // (A the lower end)
          if (ia != ib) {
            double ux = P2(ib)[0] - P2(ia)[0], uy = P2(ib)[1] - P2(ia)[1]; if (std::abs(uy) >= std::abs(ux)) { rotC = uy >= 0 ? 1 : -1; rotS = 0; } else { rotC = 0; rotS = ux > 0 ? -1 : 1; } const double ul = std::hypot(ux, uy); ux /= std::max(ul, 1e-18); uy /= std::max(ul, 1e-18);
            auto across = [&](int i) { const double dx = P2(i + 1)[0] - P2(i)[0], dy = P2(i + 1)[1] - P2(i)[1], l = std::hypot(dx, dy); return l > 1e-15 && std::abs(dx * ux + dy * uy) / l < 0.5; };   // (the border segment i→i+1 runs across the strand)
            auto capOf = [&](int tip) { int lo = tip, hi = tip; int guard = 0; while (guard++ < nb / 2 && across(lo - 1)) lo--; guard = 0; while (guard++ < nb / 2 && across(hi)) hi++; int pointed = 0; if (lo == hi) { lo--; hi++; pointed = 1; }   /* (a pointed end: the tip with its two neighbours — the end laid flat across the strand) */ return std::array<int, 3>{lo, hi, pointed}; };
            auto ca = capOf(ia), cb = capOf(ib);
            // (a wide shell — a sleeve, a forearm opened by its seam: four clear corners — the far ends are two of them across the diagonal, so the strand's
            //  caps above come out wrong (a fan in a corner, 2026-11-10). Then the corners make the rectangle: the shorter pair of opposite sides its ends)
            { std::vector<std::pair<double, int>> turnAt; const int st = std::max(1, nb / 40);
              for (int i = 0; i < nb; i++) { const V2 &a = P2(i - st), &b = P2(i), &c = P2(i + st); const double x1 = b[0] - a[0], y1 = b[1] - a[1], x2 = c[0] - b[0], y2 = c[1] - b[1];
                const double l = std::hypot(x1, y1) * std::hypot(x2, y2); if (l <= 0) continue; turnAt.push_back({std::atan2(x1 * y2 - y1 * x2, x1 * x2 + y1 * y2), i}); }
              double tsum = 0; for (auto& t : turnAt) tsum += t.first; const double dirS = tsum >= 0 ? 1 : -1;   // (the border's way round: convex corners turn that way)
              std::sort(turnAt.begin(), turnAt.end(), [&](const auto& p, const auto& q) { return p.first * dirS > q.first * dirS; });
              std::vector<int> cor; for (auto& [t, i] : turnAt) { if (t * dirS < M_PI / 4 || cor.size() == 4) break; bool near = false; for (int c : cor) if (std::min((i - c + nb) % nb, (c - i + nb) % nb) < nb / 10) near = true; if (!near) cor.push_back(i); }
              if (cor.size() == 4) { std::sort(cor.begin(), cor.end());
                auto runL = [&](int from, int to) { double s = 0; for (int k = from; k != to; k = (k + 1) % nb) s += std::hypot(P2(k + 1)[0] - P2(k)[0], P2(k + 1)[1] - P2(k)[1]); return s; };
                const double r01 = runL(cor[0], cor[1]) + runL(cor[2], cor[3]), r12 = runL(cor[1], cor[2]) + runL(cor[3], cor[0]);
                const bool strandLike = std::max(runL(cor[0], cor[1]), runL(cor[2], cor[3])) < 0.25 * std::min(runL(cor[1], cor[2]), runL(cor[3], cor[0])) || std::max(runL(cor[1], cor[2]), runL(cor[3], cor[0])) < 0.25 * std::min(runL(cor[0], cor[1]), runL(cor[2], cor[3]));
                if (!strandLike) {                                    // (a thin strand keeps its own ends: they are its caps already)
                  std::array<int, 3> A{cor[0], cor[1], 0}, B{cor[2], cor[3], 0}; if (r12 < r01) { A = {cor[1], cor[2], 0}; B = {cor[3], cor[0] + nb, 0}; }
                  const int am = (A[0] + A[1]) / 2, bm = (B[0] + B[1]) / 2;
                  if (P2(bm)[1] < P2(am)[1]) std::swap(A, B);
                  ca = A; cb = B; ia = (A[0] + A[1]) / 2 % nb; ib = (B[0] + B[1]) / 2 % nb;
                  { const double vx = P2(ib)[0] - P2(ia)[0], vy = P2(ib)[1] - P2(ia)[1]; if (std::abs(vy) >= std::abs(vx)) { rotC = vy >= 0 ? 1 : -1; rotS = 0; } else { rotC = 0; rotS = vx > 0 ? -1 : 1; } }   // (its turn from the new ends)
                  if (std::getenv("RECTDBG")) std::fprintf(stderr, "    [rect] corners %d %d %d %d -> ends %d..%d and %d..%d\n", cor[0], cor[1], cor[2], cor[3], ca[0], ca[1], cb[0], cb[1]); } } }
            // (the caps in the border's order — A's, the first side, B's, the second side; the two caps must not meet)
            const int a_lo = ca[0], a_hi = ca[1], b_lo = cb[0], b_hi = cb[1];
            auto fwd = [&](int from, int to) { return ((to - from) % nb + nb) % nb; };           // (steps forward from → to)
            const int s1 = fwd(a_hi, b_lo), cbn = fwd(b_lo, b_hi), s2 = fwd(b_hi, a_lo), can = fwd(a_lo, a_hi);
            if (s1 > 0 && s2 > 0 && s1 + s2 + cbn + can == nb) {
              auto runLen = [&](int from, int steps) { std::vector<double> c(size_t(steps) + 1, 0); for (int k = 1; k <= steps; k++) c[size_t(k)] = c[size_t(k - 1)] + std::hypot(P2(from + k)[0] - P2(from + k - 1)[0], P2(from + k)[1] - P2(from + k - 1)[1]); return c; };
              const auto cA = runLen(a_lo, can), cB = runLen(b_lo, cbn), c1 = runLen(a_hi, s1), c2 = runLen(b_hi, s2);
              double tot = 0, ar = 0; for (auto& L0 : loops) { double a = 0; for (size_t k = 0; k < L0.size(); k++) { const auto& p = orig[size_t(L0[k])]; const auto& q = orig[size_t(L0[(k + 1) % L0.size()])]; a += p[0] * q[1] - q[0] * p[1]; } tot += a; ar += std::abs(a) / 2; }
              double Lr = (c1.back() + c2.back()) / 2, Wr; const bool capsReal = can > 0 && cbn > 0 && !ca[2] && !cb[2]; if (capsReal) Wr = (cA.back() + cB.back()) / 2; else Wr = ar / std::max(Lr, 1e-18);
              if (Lr > 1e-12 && Wr > 1e-12 && ar > 1e-18) {
                if (std::getenv("RECTDBG")) std::fprintf(stderr, "    [rect] nb %d caps %d/%d sides %d/%d lens %.4f %.4f capLens %.4f %.4f area %.5f L %.4f W %.4f\n", nb, can, cbn, s1, s2, c1.back(), c2.back(), cA.back(), cB.back(), ar, Lr, Wr);
                const double k = std::sqrt(ar / (Lr * Wr)); Lr *= k; Wr *= k;                                   // (the area kept)
                const double sg = tot >= 0 ? 1.0 : -1.0; outN.assign(size_t(n), V2{0, 0}); std::vector<char> fixedP(static_cast<size_t>(n), 0);
                // (the rows: the height along the strand of every point — a harmonic function, 0 on A's cap, 1 on B's, free along the sides — so a point on one long side and the one across from it get one height: the rows run level, not slantwise)
                std::vector<double> t1(size_t(s1) + 1), t2(size_t(s2) + 1);
                { std::vector<double> hh(static_cast<size_t>(n)); std::vector<char> fx(static_cast<size_t>(n), 0); const double dmax = std::max(dA[size_t(far(dA))], 1e-18);
                  for (int i = 0; i < n; i++) hh[size_t(i)] = dA[size_t(i)] > 1e299 ? 0.5 : std::clamp(dA[size_t(i)] / dmax, 0.0, 1.0);
                  for (int k2 = 0; k2 <= can; k2++) { const int p = cyc[size_t((((a_lo + k2) % nb) + nb) % nb)]; hh[size_t(p)] = 0; fx[size_t(p)] = 1; }
                  for (int k2 = 0; k2 <= cbn; k2++) { const int p = cyc[size_t((((b_lo + k2) % nb) + nb) % nb)]; hh[size_t(p)] = 1; fx[size_t(p)] = 1; }
                  for (int it = 0; it < 1500; it++) { double mv = 0; for (int i = 0; i < n; i++) { if (fx[size_t(i)]) continue; double sx = 0, sw = 0; for (auto [q, w] : adj[size_t(i)]) { const double k2 = 1.0 / std::max(w, 1e-12); sx += hh[size_t(q)] * k2; sw += k2; } if (sw > 0) { mv = std::max(mv, std::abs(sx / sw - hh[size_t(i)])); hh[size_t(i)] = sx / sw; } } if (mv < 1e-9) break; }
                  for (int k2 = 0; k2 <= s1; k2++) t1[size_t(k2)] = hh[size_t(cyc[size_t((((a_hi + k2) % nb) + nb) % nb)])]; for (int k2 = 0; k2 <= s2; k2++) t2[size_t(k2)] = hh[size_t(cyc[size_t((((b_hi + k2) % nb) + nb) % nb)])];
                  t1[0] = 0; t1.back() = 1; t2[0] = 1; t2.back() = 0;
                  for (size_t k2 = 1; k2 < t1.size(); k2++) t1[k2] = std::max(t1[k2], t1[k2 - 1] + 1e-6); for (size_t k2 = t2.size() - 1; k2-- > 0;) t2[k2] = std::max(t2[k2], t2[k2 + 1] + 1e-6);   // (side 2 runs from the top down: its heights fall)
                  for (auto& x : t1) x = std::clamp(x, 0.0, 1.0); for (auto& x : t2) x = std::clamp(x, 0.0, 1.0); }
                // (Grid: every border point one step of its side — by count, not by length — and every spring alike: a quad strip becomes an even lattice, a
                //  plane subdivided. Relax: the border by its lengths, the inside then relaxed (SLIM, the border held): the least stretch inside the rectangle)
                const bool grid = mode == RectGrid;
                if (grid) { for (int k2 = 0; k2 <= s1; k2++) t1[size_t(k2)] = double(k2) / s1; for (int k2 = 0; k2 <= s2; k2++) t2[size_t(k2)] = 1 - double(k2) / s2; }
                auto put = [&](int idx, double x, double y) { const int p = cyc[size_t(((idx % nb) + nb) % nb)]; outN[size_t(p)] = {x * sg, y}; fixedP[size_t(p)] = 1; };
                auto capX = [&](const std::vector<double>& cc, int k2, int cnt) { return cnt <= 0 ? 0.0 : grid ? double(k2) / cnt : cc[size_t(k2)] / cc.back(); };
                for (int k2 = 0; k2 <= can; k2++) put(a_lo + k2, can > 0 ? -Wr / 2 + Wr * capX(cA, k2, can) : 0.0, 0);              // (A's cap: along the bottom, left to right — a tip alone: its middle)
                for (int k2 = 1; k2 < s1; k2++) put(a_hi + k2, Wr / 2, Lr * t1[size_t(k2)]);                                        // (the first side up the right)
                for (int k2 = 0; k2 <= cbn; k2++) put(b_lo + k2, cbn > 0 ? Wr / 2 - Wr * capX(cB, k2, cbn) : 0.0, Lr);                 // (B's cap along the top, right to left)
                for (int k2 = 1; k2 < s2; k2++) put(b_hi + k2, -Wr / 2, Lr * t2[size_t(k2)]);                                // (the second side down the left)
                for (int i = 0; i < n; i++) if (!fixedP[size_t(i)]) outN[size_t(i)] = {0, Lr / 2};
                for (int it = 0; it < 3000; it++) { double mv = 0; for (int i = 0; i < n; i++) { if (fixedP[size_t(i)]) continue; double sx = 0, sy = 0, sw = 0; for (auto [q, w] : adj[size_t(i)]) { const double k2 = grid ? 1.0 : 1.0 / std::max((m.pos[size_t(P.vert[size_t(pts[size_t(i)])])] - m.pos[size_t(P.vert[size_t(pts[size_t(q)])])]).length(), 1e-12); (void)w; sx += outN[size_t(q)][0] * k2; sy += outN[size_t(q)][1] * k2; sw += k2; }
                    if (sw > 0) { const V2 nv{sx / sw, sy / sw}; mv = std::max(mv, std::abs(nv[0] - outN[size_t(i)][0]) + std::abs(nv[1] - outN[size_t(i)][1])); outN[size_t(i)] = nv; } }
                  if (mv < 1e-9 * Lr) break; }
                if (!grid) {                                            // (Relax: the inside's stretch evened out by SLIM from that start — at the UV's scale, the border points held; turned over — left as the springs laid it)
                  std::vector<Vec3> X3(static_cast<size_t>(n)); for (int i = 0; i < n; i++) X3[size_t(i)] = m.pos[size_t(P.vert[size_t(pts[size_t(i)])])];
                  std::vector<solve::Tri> tr3; for (auto& L0 : loops) { if (L0.size() < 3) continue; std::vector<Vec3> ring; for (int i : L0) ring.push_back(X3[size_t(i)]); for (auto& t : solve::triangulate(ring)) { const solve::Tri tt{L0[size_t(t[0])], L0[size_t(t[1])], L0[size_t(t[2])]};
                      if (!(fixedP[size_t(tt[0])] && fixedP[size_t(tt[1])] && fixedP[size_t(tt[2])])) tr3.push_back(tt); } }   // (a triangle all on the border — a strand's pointed tip laid flat on the end — can't move: left out, its zero area would stop SLIM)
                  double a3 = 0; for (auto& t : tr3) a3 += (X3[size_t(t[1])] - X3[size_t(t[0])]).cross(X3[size_t(t[2])] - X3[size_t(t[0])]).length() / 2;
                  if (a3 > 0) { const double k3 = std::sqrt(Lr * Wr / a3); for (auto& x : X3) x = x * k3;   // (3D at the rectangle's size: its area)
                    solve::Dofs D(n); for (int i = 0; i < n; i++) if (fixedP[size_t(i)]) D.holdPoint(i, outN[size_t(i)]);
                    auto W2 = outN; if (!solve::flips(W2, X3, tr3) && solve::slim(W2, X3, tr3, D, 80).ok && !solve::flips(W2, X3, tr3)) outN = std::move(W2); } }
                nw = true; } } } } } }
    if (std::getenv("RECTDBG")) std::fprintf(stderr, "    [rect] shell %d: %d points, %zu faces -> %s\n", s, n, loops.size(), nw ? "disc rectangle" : "fallback strip");
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
    double W = 0; for (int k = 0; k <= N; k++) W += hwL[size_t(k)] + hwR[size_t(k)]; W /= (2.0 * (N + 1)); if (W <= 0 && !nw) continue;
    std::vector<V2> out(static_cast<size_t>(n)); if (nw) { out = outN; for (auto& q : out) q = {q[0] * rotC - q[1] * rotS, q[0] * rotS + q[1] * rotC}; }   /* (the strip turned to lie as the shell lay: along the nearer axis, the end that was first still first) */
    if (!nw) for (int i = 0; i < n; i++) { const double x = std::clamp(px[size_t(i)], 0.0, double(N)); const int k = std::min(int(x), N - 1); const double t = x - k;
      const double hl = std::max(1e-9, hwL[size_t(k)] + (hwL[size_t(k + 1)] - hwL[size_t(k)]) * t), hr = std::max(1e-9, hwR[size_t(k)] + (hwR[size_t(k + 1)] - hwR[size_t(k)]) * t);
      const double a = across[size_t(i)]; const double side = a >= 0 ? (a <= hl ? a / hl * W : W + (a - hl)) : (-a <= hr ? a / hr * W : -W + (a + hr));   // (an arm past the body: as far out as it went)
      out[size_t(i)] = {-side, along[size_t(i)]}; }
    // (turned-over polygons: their points eased towards their neighbours till none is left)
    { auto area = [&](const std::vector<int>& L0) { double a = 0; for (size_t k = 0; k < L0.size(); k++) { const auto& p = out[size_t(L0[k])]; const auto& q = out[size_t(L0[(k + 1) % L0.size()])]; a += p[0] * q[1] - q[0] * p[1]; } return a; };
      double tot = 0; for (auto& L0 : loops) tot += area(L0); const double sg = tot >= 0 ? 1 : -1;
      for (int it = 0; it < 40; it++) { std::set<int> bad; for (auto& L0 : loops) if (area(L0) * sg <= 0) bad.insert(L0.begin(), L0.end()); if (bad.empty()) break;
        for (int p : bad) { double ax = 0, ay = 0; int c = 0; for (auto [q, w] : adj[size_t(p)]) { ax += out[size_t(q)][0]; ay += out[size_t(q)][1]; c++; } if (c) out[size_t(p)] = {ax / c, ay / c}; } } }
    // in its old middle; the end that was higher on top (turned half round — never mirrored)
    const bool up = nw ? true : C[size_t(N)][1] >= C[0][1];
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
