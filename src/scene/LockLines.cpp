#include "scene/LockLines.h"
#include <QObject>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace hm::locklines {
namespace {

// ---------- the mesh's neighbourhoods ----------
struct Inc { std::vector<int> start, face, corner; };          // vertex → (polygon, corner)
Inc incidence(const PolyMesh& m) {
  const size_t n = m.pos.size(); const int nf = m.faceCount(); Inc c; c.start.assign(n + 1, 0);
  for (int f = 0; f < nf; f++) for (int k = m.faceStart[size_t(f)]; k < m.faceStart[size_t(f) + 1]; k++) c.start[size_t(m.faceVerts[size_t(k)]) + 1]++;
  for (size_t i = 0; i < n; i++) c.start[i + 1] += c.start[i];
  c.face.resize(size_t(c.start[n])); c.corner.resize(size_t(c.start[n])); std::vector<int> fill(c.start.begin(), c.start.end() - 1);
  for (int f = 0; f < nf; f++) { const int b = m.faceStart[size_t(f)], nv = m.faceStart[size_t(f) + 1] - b;
    for (int k = 0; k < nv; k++) { const int v = m.faceVerts[size_t(b + k)]; const int p = fill[size_t(v)]++; c.face[size_t(p)] = f; c.corner[size_t(p)] = k; } }
  return c;
}

// the vertices round v in a cycle — those sharing a polygon with it, the ones joined to it by an edge flagged
struct Ring { std::vector<int> v; std::vector<char> edge; };
Ring ringOf(const PolyMesh& m, const Inc& inc, int v) {
  struct FC { int prev, next; std::vector<int> seq; bool used = false; };
  std::vector<FC> fcs;
  for (int p = inc.start[size_t(v)]; p < inc.start[size_t(v) + 1]; p++) {
    const int f = inc.face[size_t(p)], kk = inc.corner[size_t(p)], b = m.faceStart[size_t(f)], nv = m.faceStart[size_t(f) + 1] - b;
    if (nv < 3) continue;
    auto at = [&](int j) { return m.faceVerts[size_t(b + ((kk + j) % nv + nv) % nv)]; };
    FC c; c.next = at(1); c.prev = at(-1); for (int j = -1; j >= -(nv - 1); j--) c.seq.push_back(at(j)); fcs.push_back(std::move(c));
  }
  Ring r; std::unordered_set<int> ed; for (auto& c : fcs) { ed.insert(c.prev); ed.insert(c.next); }
  for (size_t s = 0; s < fcs.size(); s++) {
    if (fcs[s].used) continue;
    size_t cur = s; fcs[cur].used = true; const size_t first = r.v.size(); r.v.insert(r.v.end(), fcs[cur].seq.begin(), fcs[cur].seq.end());
    while (true) {
      size_t nx = fcs.size(); for (size_t q = 0; q < fcs.size(); q++) if (!fcs[q].used && fcs[q].prev == fcs[cur].next) { nx = q; break; }
      if (nx == fcs.size()) break;
      fcs[nx].used = true; r.v.insert(r.v.end(), fcs[nx].seq.begin() + 1, fcs[nx].seq.end()); cur = nx;
    }
    if (fcs[cur].next == fcs[s].prev && r.v.size() - first > 1 && r.v.back() == r.v[first]) r.v.pop_back();   // (closed round the vertex)
  }
  r.edge.resize(r.v.size()); for (size_t i = 0; i < r.v.size(); i++) r.edge[i] = ed.count(r.v[i]) ? 1 : 0;
  return r;
}

struct UF { std::vector<int> p; explicit UF(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); } int f(int x) { while (p[size_t(x)] != x) { p[size_t(x)] = p[size_t(p[size_t(x)])]; x = p[size_t(x)]; } return x; } void u(int a, int b) { p[size_t(f(a))] = f(b); } };

// ---------- the marked vertices ----------
double lum(const std::array<float, 3>& c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

// ---------- the skeleton's graph ----------
struct Branch { int a = -1, b = -1; std::vector<Vec3> pts; bool dead = false; double length() const { double s = 0; for (size_t i = 1; i < pts.size(); i++) s += pts[i].distanceTo(pts[i - 1]); return s; } };
struct Node { Vec3 p; };
struct Graph { std::vector<Node> nodes; std::vector<Branch> br; };

// branches between the ends and junctions of a graph of points (nb: each point's neighbours)
Graph branchesOf(const std::vector<Vec3>& P, const std::vector<std::vector<int>>& nb) {
  const int n = int(P.size()); Graph G; std::vector<char> isNode(size_t(n), 0);
  for (int i = 0; i < n; i++) { const size_t d = nb[size_t(i)].size(); if (d == 1 || d >= 3) isNode[size_t(i)] = 1; }
  UF uf{static_cast<size_t>(n)}; for (int i = 0; i < n; i++) if (isNode[size_t(i)]) for (int j : nb[size_t(i)]) if (isNode[size_t(j)]) uf.u(i, j);
  std::unordered_map<int, int> nid; std::vector<int> nodeOf(size_t(n), -1); std::vector<Vec3> sum; std::vector<int> cnt;
  auto addNode = [&](int v) { const int r = uf.f(v); auto it = nid.find(r); int id; if (it == nid.end()) { id = int(sum.size()); nid[r] = id; sum.push_back({}); cnt.push_back(0); } else id = it->second; sum[size_t(id)] = sum[size_t(id)] + P[size_t(v)]; cnt[size_t(id)]++; nodeOf[size_t(v)] = id; };
  for (int i = 0; i < n; i++) if (isNode[size_t(i)]) addNode(i);
  std::vector<char> seen(size_t(n), 0);
  // (a loop with no end or junction: a node of its own at any of its points)
  for (int i = 0; i < n; i++) if (!isNode[size_t(i)] && !seen[size_t(i)] && nb[size_t(i)].size() == 2) {
    bool reachNode = false; std::vector<int> st{i}; std::vector<int> comp; std::unordered_set<int> vis{i};
    while (!st.empty()) { const int x = st.back(); st.pop_back(); comp.push_back(x); for (int y : nb[size_t(x)]) { if (isNode[size_t(y)]) reachNode = true; else if (!vis.count(y)) { vis.insert(y); st.push_back(y); } } }
    for (int x : comp) seen[size_t(x)] = 2;                                  // (2: looked at)
    if (!reachNode) { isNode[size_t(i)] = 1; addNode(i); }
  }
  std::fill(seen.begin(), seen.end(), 0);
  for (size_t k = 0; k < sum.size(); k++) G.nodes.push_back({sum[k] / double(cnt[k])});
  for (int u = 0; u < n; u++) {
    if (!isNode[size_t(u)]) continue;
    for (int w : nb[size_t(u)]) {
      if (isNode[size_t(w)] || seen[size_t(w)]) continue;
      std::vector<int> path{u, w}; seen[size_t(w)] = 1; int prev = u, cur = w;
      while (!isNode[size_t(cur)]) {
        int nx = -1; for (int y : nb[size_t(cur)]) if (y != prev) { nx = y; break; }
        if (nx < 0) break; prev = cur; cur = nx; path.push_back(cur); if (!isNode[size_t(cur)]) seen[size_t(cur)] = 1; }
      if (!isNode[size_t(cur)]) continue;
      Branch b; b.a = nodeOf[size_t(u)]; b.b = nodeOf[size_t(cur)];
      b.pts.push_back(G.nodes[size_t(b.a)].p); for (size_t i = 1; i + 1 < path.size(); i++) b.pts.push_back(P[size_t(path[i])]); b.pts.push_back(G.nodes[size_t(b.b)].p);
      if (b.a == b.b && b.pts.size() < 4) continue;
      G.br.push_back(std::move(b));
    }
  }
  return G;
}

std::vector<int> degrees(const Graph& G) { std::vector<int> d(G.nodes.size(), 0); for (auto& b : G.br) if (!b.dead) { d[size_t(b.a)]++; d[size_t(b.b)]++; } return d; }

// nodes with two branches: the branches joined into one
void joinDegree2(Graph& G) {
  bool again = true;
  while (again) {
    again = false; const auto d = degrees(G);
    for (size_t n = 0; n < G.nodes.size() && !again; n++) {
      if (d[n] != 2) continue; int i1 = -1, i2 = -1;
      for (size_t i = 0; i < G.br.size(); i++) if (!G.br[i].dead && (G.br[i].a == int(n) || G.br[i].b == int(n))) { if (i1 < 0) i1 = int(i); else if (i2 < 0) i2 = int(i); }
      if (i1 < 0 || i2 < 0) continue;                                         // (a loop on one node: stays)
      Branch& A = G.br[size_t(i1)]; Branch& B = G.br[size_t(i2)];
      if (A.b != int(n)) { std::reverse(A.pts.begin(), A.pts.end()); std::swap(A.a, A.b); }
      if (B.a != int(n)) { std::reverse(B.pts.begin(), B.pts.end()); std::swap(B.a, B.b); }
      A.pts.insert(A.pts.end(), B.pts.begin() + 1, B.pts.end()); A.b = B.b; B.dead = true; again = true;
    }
  }
}

// short branches that end free next to a junction: the paint's blots and rough ends
void pruneSpurs(Graph& G, double spurLen) {
  bool changed = true;
  while (changed) {
    changed = false; const auto d = degrees(G);
    for (auto& b : G.br) { if (b.dead) continue;
      const bool free = (d[size_t(b.a)] == 1 && d[size_t(b.b)] >= 3) || (d[size_t(b.b)] == 1 && d[size_t(b.a)] >= 3);
      if (free && b.length() < spurLen) { b.dead = true; changed = true; break; } }
    joinDegree2(G);
  }
}

// the closest point of a triangle (Ericson, Real-Time Collision Detection 5.1.5)
Vec3 closestOnTri(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
  const Vec3 ab = b - a, ac = c - a, ap = p - a; const double d1 = ab.dot(ap), d2 = ac.dot(ap); if (d1 <= 0 && d2 <= 0) return a;
  const Vec3 bp = p - b; const double d3 = ab.dot(bp), d4 = ac.dot(bp); if (d3 >= 0 && d4 <= d3) return b;
  const double vc = d1 * d4 - d3 * d2; if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
  const Vec3 cp = p - c; const double d5 = ab.dot(cp), d6 = ac.dot(cp); if (d6 >= 0 && d5 <= d6) return c;
  const double vb = d5 * d2 - d1 * d6; if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4; if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double den = 1 / (va + vb + vc); return a + ab * (vb * den) + ac * (vc * den);
}

// a line's hooks and notches cut off: where it goes on, turns back and comes again close to where it was (the thinning of a blot, or of
// two lines' paint merging: the skeleton runs up into the blot and back) — the way across instead. Its ends stay.
void unhook(std::vector<Vec3>& pts, double maxGap, double maxArc, double slack) {
  const size_t n = pts.size(); if (n < 4) return;
  std::vector<double> s(n, 0); for (size_t i = 1; i < n; i++) s[i] = s[i - 1] + pts[i].distanceTo(pts[i - 1]);
  std::vector<Vec3> out{pts[0]}; size_t i = 0;
  while (i + 1 < n) {
    size_t best = i + 1;
    for (size_t j = i + 2; j < n; j++) { const double arc = s[j] - s[i]; if (arc > maxArc) break; const double st = pts[i].distanceTo(pts[j]);
      if (st < maxGap && arc > 2 * st + slack) best = j; }
    out.push_back(pts[best]); i = best;
  }
  pts = std::move(out);
}
// a free end that hooks back (it goes out past where the line turns and comes back): cut to the hook's turn — the point of the end
// stretch farthest from the line further in. atA: the end is the first point.
void trimHook(std::vector<Vec3>& pts, bool atA, double window, double reach) {
  if (pts.size() < 4) return; if (!atA) std::reverse(pts.begin(), pts.end());
  double s = 0; size_t K = pts.size() - 1; for (size_t i = 1; i < pts.size(); i++) { s += pts[i].distanceTo(pts[i - 1]); if (s >= reach) { K = i; break; } }
  size_t best = 0; double bd = pts[0].distanceTo(pts[K]); s = 0;
  for (size_t i = 1; i < K; i++) { s += pts[i].distanceTo(pts[i - 1]); if (s > window) break; const double d = pts[i].distanceTo(pts[K]); if (d > bd * 1.02) { bd = d; best = i; } }
  if (best > 0) pts.erase(pts.begin(), pts.begin() + long(best));
  if (!atA) std::reverse(pts.begin(), pts.end());
}

// a branch cut in two at its point k: the node there (an end of it, or a new one)
int splitAt(Graph& G, int bi, size_t k) {
  { const Branch& b = G.br[size_t(bi)]; if (k == 0) return b.a; if (k + 1 >= b.pts.size()) return b.b; }
  const int nn = int(G.nodes.size()); G.nodes.push_back({G.br[size_t(bi)].pts[k]});
  Branch t; t.a = nn; t.b = G.br[size_t(bi)].b; t.pts.assign(G.br[size_t(bi)].pts.begin() + long(k), G.br[size_t(bi)].pts.end());
  Branch& b = G.br[size_t(bi)]; b.pts.resize(k + 1); b.b = nn; G.br.push_back(std::move(t));
  return nn;
}

// the mesh's vertices by cells: the nearest one to a point
struct VGrid {
  double cs = 1; Vec3 o; std::vector<std::pair<long long, int>> v; std::unordered_map<long long, std::pair<int, int>> range;
  long long key(long long x, long long y, long long z) const { return (x * 100003LL + y) * 100003LL + z; }
  void cell(const Vec3& p, long long& x, long long& y, long long& z) const { x = (long long)std::floor((p.x - o.x) / cs); y = (long long)std::floor((p.y - o.y) / cs); z = (long long)std::floor((p.z - o.z) / cs); }
  VGrid(const std::vector<Vec3>& P, const Box3& bb, double cell_) : cs(std::max(cell_, 1e-9)), o(bb.min) {
    v.reserve(P.size()); for (size_t i = 0; i < P.size(); i++) { long long x, y, z; cell(P[i], x, y, z); v.push_back({key(x, y, z), int(i)}); }
    std::sort(v.begin(), v.end()); for (size_t i = 0; i < v.size();) { size_t j = i; while (j < v.size() && v[j].first == v[i].first) j++; range[v[i].first] = {int(i), int(j)}; i = j; } }
  template <class F> void within(const std::vector<Vec3>& P, const Vec3& p, double r, F&& fn) const {      // (each vertex nearer than r)
    long long cx, cy, cz; cell(p, cx, cy, cz); const long long k = (long long)std::ceil(r / cs); const double r2 = r * r;
    for (long long x = cx - k; x <= cx + k; x++) for (long long y = cy - k; y <= cy + k; y++) for (long long z = cz - k; z <= cz + k; z++) {
      auto it = range.find(key(x, y, z)); if (it == range.end()) continue;
      for (int j = it->second.first; j < it->second.second; j++) { const int w = v[size_t(j)].second; if ((P[size_t(w)] - p).lengthSq() < r2) fn(w); } } }
  int nearest(const std::vector<Vec3>& P, const Vec3& p) const {
    long long cx, cy, cz; cell(p, cx, cy, cz); int best = -1; double bd = 1e300;
    for (int r = 1; r <= 4 && best < 0; r++)                                                   // (the ring of cells widened till one is found)
      for (long long x = cx - r; x <= cx + r; x++) for (long long y = cy - r; y <= cy + r; y++) for (long long z = cz - r; z <= cz + r; z++) {
        auto it = range.find(key(x, y, z)); if (it == range.end()) continue;
        for (int k = it->second.first; k < it->second.second; k++) { const double d = (P[size_t(v[size_t(k)].second)] - p).lengthSq(); if (d < bd) { bd = d; best = v[size_t(k)].second; } } }
    return best; }
};

}  // namespace

Result trace(const PolyMesh& m, const Opt& o) {
  Result R; const int nv = int(m.pos.size());
  if (nv < 4 || m.faceCount() < 1) { R.error = QObject::tr("The mesh is empty"); return R; }
  Box3 bb = m.bounds(); const double diag = bb.size().length();
  // 1) the marked vertices
  std::vector<float> lumV; double lumMed = 0;                                                  // (polypaint: each vertex's brightness, the background's)
  std::vector<char> ink(size_t(nv), 0); std::vector<std::array<int, 2>> marked;                // (the marked edges: edge modes)
  Inc inc;
  auto forNb = [&](int v, auto&& fn) {                                                       // (the vertices sharing a polygon with v)
    for (int p = inc.start[size_t(v)]; p < inc.start[size_t(v) + 1]; p++) { const int f = inc.face[size_t(p)];
      for (int k = m.faceStart[size_t(f)]; k < m.faceStart[size_t(f) + 1]; k++) { const int w = m.faceVerts[size_t(k)]; if (w != v) fn(w); } } };
  if (o.mode == Mode::Polypaint) {
    if (m.vcol.size() != m.pos.size()) { R.error = QObject::tr("This mesh has no polypaint (vertex colours)"); return R; }
    std::vector<float> L(static_cast<size_t>(nv)); for (int i = 0; i < nv; i++) L[size_t(i)] = float(lum(m.vcol[size_t(i)]));
    std::vector<float> S = L; std::nth_element(S.begin(), S.begin() + long(S.size() / 2), S.end()); const double med = S[S.size() / 2]; lumMed = med;
    // (two thresholds: the clear paint, and the faint paint that touches it — a line that fades a little stays whole)
    inc = incidence(m);
    std::vector<int> q; for (int i = 0; i < nv; i++) if (std::abs(L[size_t(i)] - med) > o.contrast) { ink[size_t(i)] = 1; q.push_back(i); }
    const double weak = o.contrast * 0.5;
    for (size_t h = 0; h < q.size(); h++) forNb(q[h], [&](int w) { if (!ink[size_t(w)] && std::abs(L[size_t(w)] - med) > weak) { ink[size_t(w)] = 1; q.push_back(w); } });
    // (pinholes in the paint: an unmarked vertex all round marked — marked too; else the thinning leaves a loop round it)
    for (int pass = 0; pass < 2; pass++) { std::vector<int> fill;
      for (int i = 0; i < nv; i++) { if (ink[size_t(i)]) continue; bool all = true, any = false; forNb(i, [&](int w) { any = true; if (!ink[size_t(w)]) all = false; }); if (any && all) fill.push_back(i); }
      for (int i : fill) ink[size_t(i)] = 1; if (fill.empty()) break; }
    lumV = std::move(L);
  } else if (o.mode == Mode::HardEdges) { for (auto& e : m.hardEdges) if (e[0] >= 0 && e[1] >= 0 && e[0] < nv && e[1] < nv && e[0] != e[1]) { marked.push_back(e); ink[size_t(e[0])] = ink[size_t(e[1])] = 1; } }
  else { for (auto& c : m.creases) if (c.w > 0 && c.a >= 0 && c.b >= 0 && c.a < nv && c.b < nv && c.a != c.b) { marked.push_back({c.a, c.b}); ink[size_t(c.a)] = ink[size_t(c.b)] = 1; } }
  for (int i = 0; i < nv; i++) R.inkVertices += ink[size_t(i)];
  if (!R.inkVertices) { R.error = o.mode == Mode::Polypaint ? QObject::tr("No contrast lines found — lower the Contrast") : o.mode == Mode::HardEdges ? QObject::tr("This mesh has no hard edges") : QObject::tr("This mesh has no creases"); return R; }

  // 2) the lock's ends along its surface: Euclid's farthest point from the middle, then Dijkstra to the farthest from it, and back
  if (inc.start.empty()) inc = incidence(m);
  auto dijkstra = [&](int src, std::vector<float>& d, std::vector<int>* par = nullptr) {
    d.assign(size_t(nv), 1e30f); if (par) par->assign(size_t(nv), -1); using QP = std::pair<float, int>; std::priority_queue<QP, std::vector<QP>, std::greater<QP>> q; d[size_t(src)] = 0; q.push({0, src});
    while (!q.empty()) { auto [dd, v] = q.top(); q.pop(); if (dd > d[size_t(v)]) continue;
      for (int p = inc.start[size_t(v)]; p < inc.start[size_t(v) + 1]; p++) { const int f = inc.face[size_t(p)], kk = inc.corner[size_t(p)], b = m.faceStart[size_t(f)], n = m.faceStart[size_t(f) + 1] - b;
        for (int s : {1, -1}) { const int w = m.faceVerts[size_t(b + ((kk + s) % n + n) % n)]; const float nd = dd + float(m.pos[size_t(v)].distanceTo(m.pos[size_t(w)])); if (nd < d[size_t(w)]) { d[size_t(w)] = nd; if (par) (*par)[size_t(w)] = v; q.push({nd, w}); } } } } };
  // (the ends looked for on the mesh's biggest connected piece: a stray part elsewhere is not taken for the lock's end)
  int big = 0; UF pc{size_t(nv)};
  { for (int f = 0; f < m.faceCount(); f++) { const int b = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1]; for (int k = b + 1; k < e; k++) pc.u(m.faceVerts[size_t(b)], m.faceVerts[size_t(k)]); }
    std::vector<int> cnt(size_t(nv), 0); for (int i = 0; i < nv; i++) cnt[size_t(pc.f(i))]++; big = int(std::max_element(cnt.begin(), cnt.end()) - cnt.begin()); }
  const Vec3 mid = bb.center(); int g0 = 0; { double best = -1; for (int i = 0; i < nv; i++) { if (pc.f(i) != big) continue; const double dd = m.pos[size_t(i)].distanceTo(mid); if (dd > best) { best = dd; g0 = i; } } }
  std::vector<float> d1, d2; dijkstra(g0, d1); int E1 = g0; { float best = -1; for (int i = 0; i < nv; i++) if (d1[size_t(i)] < 1e29f && d1[size_t(i)] > best) { best = d1[size_t(i)]; E1 = i; } }
  dijkstra(E1, d2); int E2 = E1; { float best = -1; for (int i = 0; i < nv; i++) if (d2[size_t(i)] < 1e29f && d2[size_t(i)] > best) { best = d2[size_t(i)]; E2 = i; } }
  // (the marks on the other pieces: no distance from the root there — left out, not all merged into the tip)
  for (int i = 0; i < nv; i++) if (d2[size_t(i)] >= 1e29f) ink[size_t(i)] = 0;
  marked.erase(std::remove_if(marked.begin(), marked.end(), [&](const std::array<int, 2>& e) { return d2[size_t(e[0])] >= 1e29f || d2[size_t(e[1])] >= 1e29f; }), marked.end());
  R.length = d2[size_t(E2)]; const double Lk = R.length;

  // 2b) the lock's end faces: paint there is not a line (the lines' paint run over the edge onto the cut, a blot on it) — the lines run
  // along the lock. An end face: near an end, its surface facing along the lock's axis there
  {
    std::vector<Vec3> vn(size_t(nv), Vec3(0, 0, 0));
    for (int f = 0; f < m.faceCount(); f++) { const int b = m.faceStart[size_t(f)], n = m.faceStart[size_t(f) + 1] - b; Vec3 fnm(0, 0, 0);
      for (int k = 0; k < n; k++) { const Vec3& a = m.pos[size_t(m.faceVerts[size_t(b + k)])]; const Vec3& c = m.pos[size_t(m.faceVerts[size_t(b + (k + 1) % n)])];
        fnm.x += (a.y - c.y) * (a.z + c.z); fnm.y += (a.z - c.z) * (a.x + c.x); fnm.z += (a.x - c.x) * (a.y + c.y); }
      for (int k = 0; k < n; k++) { Vec3& x = vn[size_t(m.faceVerts[size_t(b + k)])]; x = x + fnm; } }
    std::vector<char> cap(size_t(nv), 0);
    auto capOf = [&](const std::vector<float>& d) {
      Vec3 c0(0, 0, 0), c1(0, 0, 0); double n0 = 0, n1 = 0;
      for (int i = 0; i < nv; i++) { const double x = d[size_t(i)]; if (x < 0.03 * Lk) { c0 = c0 + m.pos[size_t(i)]; n0++; } else if (x > 0.10 * Lk && x < 0.14 * Lk) { c1 = c1 + m.pos[size_t(i)]; n1++; } }
      if (!n0 || !n1) return; Vec3 ax = c0 / n0 - c1 / n1; const double l = ax.length(); if (l < 1e-12) return; ax = ax / l;
      for (int i = 0; i < nv; i++) if (d[size_t(i)] < 0.1 * Lk) { const Vec3& nn = vn[size_t(i)]; const double nl = nn.length(); if (nl > 1e-20 && std::abs(nn.dot(ax)) / nl > 0.7) cap[size_t(i)] = 1; } };
    capOf(d2); { std::vector<float> dE2; dijkstra(E2, dE2); capOf(dE2); }                // (both ends: which is the root is told later)
    for (int i = 0; i < nv; i++) if (cap[size_t(i)]) ink[size_t(i)] = 0;
    marked.erase(std::remove_if(marked.begin(), marked.end(), [&](const std::array<int, 2>& e) { return cap[size_t(e[0])] && cap[size_t(e[1])]; }), marked.end());
    R.inkVertices = 0; for (int i = 0; i < nv; i++) R.inkVertices += ink[size_t(i)];
  }

  // 3) the skeleton: the points and their neighbours
  std::vector<Vec3> P; std::vector<std::vector<int>> nb;
  if (o.mode == Mode::Polypaint) {
    std::vector<int> ids; for (int i = 0; i < nv; i++) if (ink[size_t(i)]) ids.push_back(i);
    std::unordered_map<int, int> pos; pos.reserve(ids.size() * 2); for (size_t i = 0; i < ids.size(); i++) pos[ids[i]] = int(i);
    std::vector<Ring> rings(ids.size()); for (size_t i = 0; i < ids.size(); i++) rings[i] = ringOf(m, inc, ids[i]);
    std::vector<char> alive(size_t(nv), 0); for (int v : ids) alive[size_t(v)] = 1;
    // (the border's distance: edge steps from the nearest unmarked vertex)
    std::vector<int> dist(ids.size(), 0); std::vector<int> cur;
    for (size_t i = 0; i < ids.size(); i++) { const Ring& r = rings[i]; bool b = false; for (size_t k = 0; k < r.v.size(); k++) if (r.edge[k] && !ink[size_t(r.v[k])]) b = true; if (b) { dist[i] = 1; cur.push_back(int(i)); } }
    int maxd = 1; for (size_t h = 0; h < cur.size(); h++) { const int i = cur[h]; const Ring& r = rings[size_t(i)];
      for (size_t k = 0; k < r.v.size(); k++) if (r.edge[k] && ink[size_t(r.v[k])]) { const int j = pos[r.v[k]]; if (!dist[size_t(j)]) { dist[size_t(j)] = dist[size_t(i)] + 1; maxd = std::max(maxd, dist[size_t(j)]); cur.push_back(j); } } }
    for (size_t i = 0; i < ids.size(); i++) if (!dist[i]) dist[i] = 1;                       // (a marked area with no unmarked vertex: all at the border)
    std::vector<std::vector<int>> layer(size_t(maxd) + 1); for (size_t i = 0; i < ids.size(); i++) layer[size_t(dist[i])].push_back(int(i));
    // (simple point: the marked vertices round it make one piece (8-connected) and the unmarked ones too (4-connected))
    auto simple = [&](size_t i) {
      const Ring& r = rings[i]; const size_t k = r.v.size(); if (k < 3) return false;
      std::vector<char> a(k); size_t na = 0; for (size_t j = 0; j < k; j++) { a[j] = alive[size_t(r.v[j])]; na += a[j]; }
      if (na == 0 || na == k) return false;
      UF f(k), g(k); std::vector<size_t> es; for (size_t j = 0; j < k; j++) if (r.edge[j]) es.push_back(j);
      for (size_t j = 0; j < k; j++) { const size_t q = (j + 1) % k; if (a[j] && a[q]) f.u(int(j), int(q)); if (!a[j] && !a[q]) g.u(int(j), int(q)); }
      for (size_t t = 0; t < es.size(); t++) { const size_t x = es[t], y = es[(t + 1) % es.size()]; if (a[x] && a[y]) f.u(int(x), int(y)); }
      int fc = 0, gc = 0; std::vector<char> sf(k, 0), sg(k, 0);
      for (size_t j = 0; j < k; j++) { if (a[j]) { const int rt = f.f(int(j)); if (!sf[size_t(rt)]) { sf[size_t(rt)] = 1; fc++; } } else if (r.edge[j]) { const int rt = g.f(int(j)); if (!sg[size_t(rt)]) { sg[size_t(rt)] = 1; gc++; } } }
      return fc == 1 && gc == 1; };
    auto endpoint = [&](size_t i) { const Ring& r = rings[i]; int c = 0; for (int x : r.v) c += alive[size_t(x)]; return c <= 1; };
    for (int d = 1; d <= maxd; d++) {
      bool ch = true; while (ch) { ch = false; for (int i : layer[size_t(d)]) { const int v = ids[size_t(i)]; if (!alive[size_t(v)] || endpoint(size_t(i))) continue; if (simple(size_t(i))) { alive[size_t(v)] = 0; ch = true; } } } }
    { bool ch = true; while (ch) { ch = false; for (size_t i = 0; i < ids.size(); i++) { const int v = ids[i]; if (!alive[size_t(v)] || endpoint(i)) continue; if (simple(i)) { alive[size_t(v)] = 0; ch = true; } } } }
    std::vector<int> idx(size_t(nv), -1); for (size_t i = 0; i < ids.size(); i++) if (alive[size_t(ids[i])]) { idx[size_t(ids[i])] = int(P.size()); P.push_back(m.pos[size_t(ids[i])]); }
    nb.assign(P.size(), {});
    for (size_t i = 0; i < ids.size(); i++) { if (idx[size_t(ids[i])] < 0) continue; auto& L = nb[size_t(idx[size_t(ids[i])])]; for (int x : rings[i].v) if (idx[size_t(x)] >= 0 && std::find(L.begin(), L.end(), idx[size_t(x)]) == L.end()) L.push_back(idx[size_t(x)]); }
  } else {
    std::vector<int> idx(size_t(nv), -1); for (auto& e : marked) for (int x : e) if (idx[size_t(x)] < 0) { idx[size_t(x)] = int(P.size()); P.push_back(m.pos[size_t(x)]); }
    nb.assign(P.size(), {}); for (auto& e : marked) { auto& A = nb[size_t(idx[size_t(e[0])])]; auto& B = nb[size_t(idx[size_t(e[1])])]; const int a = idx[size_t(e[0])], b = idx[size_t(e[1])]; if (std::find(A.begin(), A.end(), b) == A.end()) { A.push_back(b); B.push_back(a); } }
  }
  if (P.empty()) { R.error = QObject::tr("Nothing left of the lines"); return R; }

  // 4) the branches: spurs cut, near ends joined
  Graph G = branchesOf(P, nb);
  pruneSpurs(G, o.spur * Lk); joinDegree2(G);
  { const double st = o.stitch * Lk; UF uf{G.nodes.size()};
    const auto d = degrees(G);
    for (size_t i = 0; i < G.nodes.size(); i++) for (size_t j = i + 1; j < G.nodes.size(); j++) if (d[i] && d[j] && G.nodes[i].p.distanceTo(G.nodes[j].p) < st) uf.u(int(i), int(j));
    std::vector<Vec3> sum(G.nodes.size()); std::vector<int> cnt(G.nodes.size(), 0); for (size_t i = 0; i < G.nodes.size(); i++) if (d[i]) { const int r = uf.f(int(i)); sum[size_t(r)] = sum[size_t(r)] + G.nodes[i].p; cnt[size_t(r)]++; }
    std::vector<Vec3> np(G.nodes.size()); for (size_t i = 0; i < G.nodes.size(); i++) if (cnt[i]) np[i] = sum[i] / double(cnt[i]);
    for (auto& b : G.br) { if (b.dead) continue; b.a = uf.f(b.a); b.b = uf.f(b.b); b.pts.front() = np[size_t(b.a)]; b.pts.back() = np[size_t(b.b)];
      if (b.a == b.b && b.length() < 4 * st) b.dead = true; }
    for (size_t i = 0; i < G.nodes.size(); i++) if (cnt[i]) G.nodes[i].p = np[i];
  }
  joinDegree2(G); pruneSpurs(G, o.spur * Lk); joinDegree2(G);

  // 5) the root: the end the free line ends lie around (the lines start apart there and come together at the tip); a tie — the higher end
  bool root1;
  { const auto d = degrees(G); const double rad = 0.12 * Lk; int c1 = 0, c2 = 0;
    for (size_t i = 0; i < G.nodes.size(); i++) if (d[i] == 1) { if (G.nodes[i].p.distanceTo(m.pos[size_t(E1)]) < rad) c1++; if (G.nodes[i].p.distanceTo(m.pos[size_t(E2)]) < rad) c2++; }
    root1 = c1 > c2 || (c1 == c2 && m.pos[size_t(E1)].y >= m.pos[size_t(E2)].y); if (o.swapEnds) root1 = !root1;
    R.root = m.pos[size_t(root1 ? E1 : E2)]; R.tip = m.pos[size_t(root1 ? E2 : E1)]; R.hasEnds = true; }
  const double endRad = 0.07 * Lk;

  // (the distance from the root along the lock, at any point: its nearest vertex's)
  const VGrid vg(m.pos, bb, std::max(diag / 256, 1e-9));
  auto drV = [&](int v) { const float x = d2[size_t(v)]; return root1 ? double(x) : double(Lk) - double(x); };
  auto drAt = [&](const Vec3& p) { const int v = vg.nearest(m.pos, p); return v >= 0 ? drV(v) : 0.0; };
  auto branchOf = [&](int n) { for (size_t i = 0; i < G.br.size(); i++) if (!G.br[i].dead && (G.br[i].a == n || G.br[i].b == n)) return int(i); return -1; };
  auto endTan = [&](const Branch& b, int node, double reach) { const bool atA = b.a == node; const int n = int(b.pts.size()); const Vec3 from = atA ? b.pts.front() : b.pts.back(); Vec3 q = from;
    for (int k = 1; k < n; k++) { q = atA ? b.pts[size_t(k)] : b.pts[size_t(n - 1 - k)]; if (from.distanceTo(q) >= reach) break; }
    Vec3 t = from - q; const double l = t.length(); return l > 1e-12 ? t / l : Vec3(0, 0, 0); };   // (pointing out of the line, at its end)

  // 5b) two lines joined over the root (their paint meets on the lock's end): the line runs up to the root and back — cut there, each
  // piece starts at the root on its own
  for (size_t bi = 0; bi < G.br.size(); bi++) {
    if (G.br[bi].dead || G.br[bi].pts.size() < 5) continue;
    std::vector<double> r; r.reserve(G.br[bi].pts.size()); for (const Vec3& p : G.br[bi].pts) r.push_back(drAt(p));
    const size_t k = size_t(std::min_element(r.begin() + 1, r.end() - 1) - r.begin());
    if (r[k] > 0.08 * Lk || r.front() < r[k] + 0.03 * Lk || r.back() < r[k] + 0.03 * Lk) continue;
    const int na = int(G.nodes.size()); G.nodes.push_back({G.br[bi].pts[k]}); const int nb = int(G.nodes.size()); G.nodes.push_back({G.br[bi].pts[k]});
    Branch t; t.a = nb; t.b = G.br[bi].b; t.pts.assign(G.br[bi].pts.begin() + long(k), G.br[bi].pts.end());
    G.br[bi].pts.resize(k + 1); G.br[bi].b = na;
    // (each piece drawn back off the turn — they part there; carried on to the lock's end later, each its own way)
    auto back = [&](std::vector<Vec3>& p, bool fromEnd) { double s = 0; while (p.size() > 3) { const size_t a = fromEnd ? p.size() - 1 : 0, b = fromEnd ? p.size() - 2 : 1;
        s += p[a].distanceTo(p[b]); if (s > 0.02 * Lk) break; p.erase(p.begin() + long(a)); } };
    back(G.br[bi].pts, true); back(t.pts, false);
    G.nodes[size_t(na)].p = G.br[bi].pts.back(); G.nodes[size_t(nb)].p = t.pts.front();
    G.br.push_back(std::move(t));
    bi--;                                                                                     // (the first piece again: another turn in it)
  }
  // 6) two lines crossing: a node of 4 branches that make two straight pairs — each line goes on through it. A node of 3 is two lines
  // coming together (or one parting): it stays a junction, the lines meeting there share it.
  for (size_t nd = 0; nd < G.nodes.size(); nd++) {
    if (G.nodes[nd].p.distanceTo(R.root) < endRad || G.nodes[nd].p.distanceTo(R.tip) < endRad) continue;
    std::vector<int> bs; for (size_t i = 0; i < G.br.size(); i++) if (!G.br[i].dead && (G.br[i].a == int(nd) || G.br[i].b == int(nd)) && G.br[i].a != G.br[i].b) bs.push_back(int(i));
    if (bs.size() != 4) continue;
    Vec3 t[4]; for (int k = 0; k < 4; k++) t[k] = -endTan(G.br[size_t(bs[size_t(k)])], int(nd), 0.03 * Lk);   // (from the node along each)
    const int pr[3][4] = {{0, 1, 2, 3}, {0, 2, 1, 3}, {0, 3, 1, 2}}; int bp = -1; double bs2 = 1e9;
    for (int c = 0; c < 3; c++) { const double d1 = t[pr[c][0]].dot(t[pr[c][1]]), d2_ = t[pr[c][2]].dot(t[pr[c][3]]);
      if (d1 < -0.6 && d2_ < -0.6 && d1 + d2_ < bs2) { bs2 = d1 + d2_; bp = c; } }                     // (each pair turns less than ~53°)
    if (bp < 0) continue;
    for (int h = 0; h < 2; h++) {
      Branch& A = G.br[size_t(bs[size_t(pr[bp][2 * h])])]; Branch& B = G.br[size_t(bs[size_t(pr[bp][2 * h + 1])])];
      if (A.b != int(nd)) { std::reverse(A.pts.begin(), A.pts.end()); std::swap(A.a, A.b); }
      if (B.a != int(nd)) { std::reverse(B.pts.begin(), B.pts.end()); std::swap(B.a, B.b); }
      A.pts.insert(A.pts.end(), B.pts.begin() + 1, B.pts.end()); A.b = B.b; B.dead = true; }
  }
  // (a branch that comes back to its own node: a hairpin, not a line)
  for (auto& b : G.br) if (!b.dead && b.a == b.b) b.dead = true;
  // 6b) the paint's blots: a short rung across the lock between two lines (where their paint touches), and the loop round a hole in a
  // line's paint (two short branches between the same two nodes: the shorter stays)
  for (int guard = 0; guard < 64; guard++) {
    bool ch = false; const auto d = degrees(G);
    // (a branch on the side of a line going straight on through the node)
    auto sideAt = [&](int bi, int nd) { std::vector<int> os;
      for (size_t i = 0; i < G.br.size(); i++) if (int(i) != bi && !G.br[i].dead && G.br[i].a != G.br[i].b && (G.br[i].a == nd || G.br[i].b == nd)) os.push_back(int(i));
      if (os.size() != 2) return false;
      return endTan(G.br[size_t(os[0])], nd, 0.03 * Lk).dot(endTan(G.br[size_t(os[1])], nd, 0.03 * Lk)) < -0.6; };
    for (size_t i = 0; i < G.br.size(); i++) { Branch& b = G.br[i]; if (b.dead || b.a == b.b) continue; const double len = b.length();
      const bool both = d[size_t(b.a)] >= 3 && d[size_t(b.b)] >= 3;
      // (a rung across the lock; a bridge between two lines that each go on — where their paint touches)
      if (both && ((len < 0.08 * Lk && std::abs(drAt(b.pts.front()) - drAt(b.pts.back())) < 0.4 * len) || (len < 0.15 * Lk && sideAt(int(i), b.a) && sideAt(int(i), b.b)))) { b.dead = true; ch = true; continue; }
      // (a short stub off a line in the lock's middle: a blot — the lines start at the root and end at the tip)
      const int fr = d[size_t(b.a)] == 1 && d[size_t(b.b)] >= 3 ? b.a : d[size_t(b.b)] == 1 && d[size_t(b.a)] >= 3 ? b.b : -1;
      if (fr >= 0 && len < 0.06 * Lk) { const double r = drAt(G.nodes[size_t(fr)].p); if (r > 0.08 * Lk && r < 0.92 * Lk) { b.dead = true; ch = true; } } }
    std::map<std::pair<int, int>, int> pairOf;
    for (size_t i = 0; i < G.br.size(); i++) { Branch& b = G.br[i]; if (b.dead) continue; const auto key = std::make_pair(std::min(b.a, b.b), std::max(b.a, b.b));
      auto it = pairOf.find(key); if (it == pairOf.end()) { pairOf[key] = int(i); continue; }
      Branch& o2 = G.br[size_t(it->second)]; const double l1 = b.length(), l2 = o2.length(); if (std::max(l1, l2) > 0.1 * Lk) continue;
      if (l1 < l2) { o2.dead = true; it->second = int(i); } else b.dead = true; ch = true; }
    joinDegree2(G); pruneSpurs(G, o.spur * Lk); joinDegree2(G);
    if (!ch) break;
  }
  // 6c) the hooks: a line that runs up into a blot and back, a notch, a free end bent back — cut off (they made zigzags of anchors, and a
  // junction up where the paint first touched instead of where the lines meet)
  auto unhookAll = [&]() {
    const auto d = degrees(G);
    for (auto& b : G.br) { if (b.dead) continue;
      unhook(b.pts, 0.02 * Lk, 0.15 * Lk, 0.002 * Lk);
      if (d[size_t(b.a)] == 1 && b.a != b.b) { trimHook(b.pts, true, 0.06 * Lk, 0.1 * Lk); G.nodes[size_t(b.a)].p = b.pts.front(); }
      if (d[size_t(b.b)] == 1 && b.a != b.b) { trimHook(b.pts, false, 0.06 * Lk, 0.1 * Lk); G.nodes[size_t(b.b)].p = b.pts.back(); } } };
  unhookAll();
  // 7) gaps: two free ends that face each other close by, the one on from the other along the lock — one line (a bridge between them)
  {
    bool again = true;
    while (again) { again = false; const auto d = degrees(G); double best = 0.06 * Lk; int bn1 = -1, bn2 = -1;
      std::vector<int> ends; for (size_t i = 0; i < G.nodes.size(); i++) if (d[i] == 1) ends.push_back(int(i));
      for (size_t x = 0; x < ends.size(); x++) for (size_t y = x + 1; y < ends.size(); y++) {
        const int n1 = ends[x], n2 = ends[y]; const double dd = G.nodes[size_t(n1)].p.distanceTo(G.nodes[size_t(n2)].p); if (dd >= best) continue;
        const int b1 = branchOf(n1), b2 = branchOf(n2); if (b1 < 0 || b2 < 0 || b1 == b2) continue;
        const Vec3 t1 = endTan(G.br[size_t(b1)], n1, 0.04 * Lk), t2 = endTan(G.br[size_t(b2)], n2, 0.04 * Lk), v = (G.nodes[size_t(n2)].p - G.nodes[size_t(n1)].p) / std::max(dd, 1e-12);
        if (t1.dot(v) > 0.5 && t2.dot(v) < -0.5 && std::abs(drAt(G.nodes[size_t(n2)].p) - drAt(G.nodes[size_t(n1)].p)) > 0.5 * dd) { best = dd; bn1 = n1; bn2 = n2; } }
      if (bn1 >= 0) { Branch br; br.a = bn1; br.b = bn2; br.pts = {G.nodes[size_t(bn1)].p, G.nodes[size_t(bn2)].p}; G.br.push_back(br); joinDegree2(G); again = true; } }
  }
  // 7b) a line that runs into another one and stops just short of it (their paint merges, the thinning parted them): joined to it there
  {
    const double reach = std::max(o.stitch * Lk * 1.5, 0.012 * Lk);
    for (size_t i = 0; i < G.nodes.size(); i++) {
      if (degrees(G)[i] != 1) continue; const Vec3 p = G.nodes[i].p; const double r = drAt(p); if (r < 0.08 * Lk || r > 0.97 * Lk) continue;   // (not at the lock's ends)
      const int own = branchOf(int(i)); if (own < 0) continue; const Vec3 t = endTan(G.br[size_t(own)], int(i), 0.03 * Lk);
      double bd = reach * reach; int hb = -1; size_t hk = 0;
      for (size_t bi = 0; bi < G.br.size(); bi++) { const Branch& b = G.br[bi]; if (b.dead || int(bi) == own) continue;
        for (size_t k = 0; k < b.pts.size(); k++) { const Vec3 dv = b.pts[k] - p; const double dd = dv.lengthSq(); if (dd >= bd) continue;
          const double l = std::sqrt(dd); if (l > 1e-12 && t.dot(dv / l) < 0.2) continue;                     // (ahead of the end, not behind it)
          bd = dd; hb = int(bi); hk = k; } }
      if (hb < 0) continue;
      const int nn = splitAt(G, hb, hk); Branch br; br.a = int(i); br.b = nn; br.pts = {p, G.nodes[size_t(nn)].p}; G.br.push_back(std::move(br));
    }
    joinDegree2(G);
  }
  // 8) the tip: a line that stops short of it, in its last stretch, is carried on along the lock's surface — to the first line it meets on
  // the way (the two come together there, as the painted ones do), or to the tip. The nearest to the tip go first: the line that reaches
  // it, then the others onto it.
  std::vector<float> dT; std::vector<int> par; const int tipV = root1 ? E2 : E1; dijkstra(tipV, dT, &par);   // (the way to the tip over the surface)
  {
    int tipNode = -1; { const auto d = degrees(G); int bd = 0; for (size_t i = 0; i < G.nodes.size(); i++) if (d[i] >= 1 && d[i] > bd && G.nodes[i].p.distanceTo(R.tip) < endRad) { bd = d[i]; tipNode = int(i); } }
    struct FE { int node, v0; float dt; }; std::vector<FE> fe;
    { const auto d = degrees(G);
      for (size_t i = 0; i < G.nodes.size(); i++) { if (d[i] != 1 || int(i) == tipNode) continue; const Vec3 p = G.nodes[i].p; const int v0 = vg.nearest(m.pos, p);
        if (v0 < 0 || drV(v0) < 0.5 * Lk || dT[size_t(v0)] > 0.4 * Lk || par[size_t(v0)] < 0) continue; fe.push_back({int(i), v0, dT[size_t(v0)]}); } }
    std::sort(fe.begin(), fe.end(), [](const FE& a, const FE& b) { return a.dt < b.dt; });
    const double meet = std::max(o.stitch * Lk * 1.5, 0.012 * Lk);
    for (const FE& e : fe) {
      const int own = branchOf(e.node); if (own < 0) continue;
      std::vector<int> path; for (int v = e.v0, guard = 0; v >= 0 && guard < nv; v = par[size_t(v)], guard++) path.push_back(v);
      int hitB = -1; size_t hitK = 0, hitAt = path.size();
      const double clear = 2 * meet;                                                        // (the start, next to its own line, is passed by)
      for (size_t s = 0; s < path.size() && hitB < 0; s += 2) { const Vec3 q = m.pos[size_t(path[s])]; if (q.distanceTo(G.nodes[size_t(e.node)].p) < clear) continue;
        double bd = meet * meet;
        for (size_t bi = 0; bi < G.br.size(); bi++) { const Branch& b = G.br[bi]; if (b.dead || int(bi) == own) continue;
          for (size_t k = 0; k < b.pts.size(); k++) { const double dd = (b.pts[k] - q).lengthSq(); if (dd < bd) { bd = dd; hitB = int(bi); hitK = k; hitAt = s; } } } }
      int target; Branch ex; ex.a = e.node; ex.pts.push_back(G.nodes[size_t(e.node)].p);
      if (hitB >= 0) { for (size_t s = 0; s < hitAt; s++) ex.pts.push_back(m.pos[size_t(path[s])]); target = splitAt(G, hitB, hitK); }
      else { for (int v : path) ex.pts.push_back(m.pos[size_t(v)]); if (tipNode < 0) { tipNode = int(G.nodes.size()); G.nodes.push_back({R.tip}); } target = tipNode; }
      ex.b = target; ex.pts.push_back(G.nodes[size_t(target)].p); G.br.push_back(std::move(ex));
    }
    joinDegree2(G);
  }
  // 8b) the root: a line that fades out short of it, in its first stretch, is carried on the way it goes, over the surface, till the lock
  // ends (the surface folds over its end)
  {
    const auto d = degrees(G); const double step = 0.004 * Lk;
    for (size_t i = 0; i < G.nodes.size(); i++) {
      if (d[i] != 1) continue; const Vec3 p0 = G.nodes[i].p; const double r0 = drAt(p0); if (r0 > 0.25 * Lk || r0 < 2 * step) continue;
      const int bi = branchOf(int(i)); if (bi < 0) continue;
      Vec3 dir = endTan(G.br[size_t(bi)], int(i), 0.06 * Lk); if (dir.length() < 0.5) continue;   // (the way it goes over a stretch, not its last bit)
      std::vector<Vec3> ext; Vec3 q = p0;
      for (int s = 0; s < int(0.15 * Lk / step); s++) {
        // (the next point: the most painted vertex just ahead — the line followed as it fades, not its tangent; the paint gone: the end)
        int v = -1;
        if (!lumV.empty()) { double best = -1;
          vg.within(m.pos, q + dir * step, 0.4 * step, [&](int w) { const Vec3 dv = m.pos[size_t(w)] - q; const double l = dv.length(); if (l < 0.3 * step || dv.dot(dir) < 0.8 * l) return;
            const double dev = std::abs(double(lumV[size_t(w)]) - lumMed); if (dev > best) { best = dev; v = w; } });
          if (v >= 0 && best < 0.1 * o.contrast) break; }
        if (v < 0) v = vg.nearest(m.pos, q + dir * step);
        if (v < 0) break; const Vec3 np = m.pos[size_t(v)];
        Vec3 mv = np - q; const double ml = mv.length(); if (ml < 1e-12) break; mv = mv / ml;
        // (the surface folds — over the lock's end: the end. The distance from the root can't tell it: near the root it is measured
        // from one point of the end's rim. Another line right there (nearer than a quarter of the paint's width — the lines near the root
        // come close, 0.7% of the lock apart in the example, but don't touch): it went over onto that line's paint — no further)
        if (mv.dot(dir) < 0.8) break;
        { const double n2 = (0.003 * Lk) * (0.003 * Lk); bool hit = false;
          for (size_t bj = 0; bj < G.br.size() && !hit; bj++) { if (G.br[bj].dead || int(bj) == bi) continue; for (const Vec3& x : G.br[bj].pts) if ((x - np).lengthSq() < n2) { hit = true; break; } }
          if (hit) break; }
        ext.push_back(np); dir = (dir * 0.7 + mv * 0.3); dir = dir / std::max(dir.length(), 1e-12); q = np; }
      if (ext.size() < 2) continue;
      const int nn = int(G.nodes.size()); G.nodes.push_back({ext.back()});
      Branch ex; ex.a = int(i); ex.b = nn; ex.pts.push_back(p0); ex.pts.insert(ex.pts.end(), ext.begin(), ext.end()); G.br.push_back(std::move(ex));
    }
    joinDegree2(G);
  }
  // 8c) the tip: the nodes in the lock's last stretch are the tip — one node, where all the lines that reach it meet (the bits of line
  // between them go)
  {
    const auto d = degrees(G); int tn = -1;
    for (size_t i = 0; i < G.nodes.size(); i++) if (d[i] && (G.nodes[i].p.distanceTo(R.tip) < 0.03 * Lk || drAt(G.nodes[i].p) > 0.97 * Lk)) {
      if (tn < 0) { tn = int(i); G.nodes[i].p = R.tip; } else for (auto& b : G.br) { if (b.dead) continue; if (b.a == int(i)) b.a = tn; if (b.b == int(i)) b.b = tn; } }
    // (each line into it: from where it enters the tip's stretch on over the surface — not a jump through the air to the tip, the curve
    // bulged off the lock there)
    auto inZone = [&](const Vec3& p) { return p.distanceTo(R.tip) < 0.03 * Lk || drAt(p) > 0.97 * Lk; };
    if (tn >= 0) for (auto& b : G.br) { if (b.dead || b.a == b.b || (b.a != tn && b.b != tn)) continue;
      const bool atA = b.a == tn; if (atA) std::reverse(b.pts.begin(), b.pts.end());
      size_t k = 0; while (k + 1 < b.pts.size() && !inZone(b.pts[k])) k++;
      if (k == 0) k = 1; b.pts.resize(std::min(k + 1, b.pts.size()));
      const int v0 = vg.nearest(m.pos, b.pts.back());
      for (int v = v0, guard = 0; v >= 0 && guard < nv; v = par[size_t(v)], guard++) b.pts.push_back(m.pos[size_t(v)]);
      b.pts.back() = R.tip;
      if (atA) std::reverse(b.pts.begin(), b.pts.end()); }
  }
  for (auto& b : G.br) if (!b.dead && b.a == b.b) b.dead = true;
  // (two lines between the same two nodes that run side by side: one thick stroke of paint parted by the thinning — the shorter stays)
  {
    auto at = [](const Branch& b, double f) { const double L = b.length() * f; double s = 0;
      for (size_t i = 1; i < b.pts.size(); i++) { const double l = b.pts[i].distanceTo(b.pts[i - 1]); if (s + l >= L) return b.pts[i - 1] + (b.pts[i] - b.pts[i - 1]) * ((L - s) / std::max(l, 1e-12)); s += l; }
      return b.pts.back(); };
    for (size_t i = 0; i < G.br.size(); i++) for (size_t j = i + 1; j < G.br.size(); j++) {
      Branch& A = G.br[i]; Branch& B = G.br[j]; if (A.dead || B.dead) continue;
      const bool same = A.a == B.a && A.b == B.b, flip = A.a == B.b && A.b == B.a; if (!same && !flip) continue;
      double far = 0; for (int s = 1; s < 10; s++) far = std::max(far, at(A, s / 10.0).distanceTo(at(B, flip ? 1 - s / 10.0 : s / 10.0)));
      if (far < 0.04 * Lk) (A.length() <= B.length() ? B : A).dead = true; }
  }
  joinDegree2(G); unhookAll();                                                              // (the joins above can make new hooks)
  // (a short line on its own — both ends free — is a stray mark; a short piece of the lines' tree stays)
  { const auto d = degrees(G); for (auto& b : G.br) if (!b.dead && d[size_t(b.a)] == 1 && d[size_t(b.b)] == 1 && b.length() < o.minLine * Lk) b.dead = true; }
  joinDegree2(G);
  // 8d) where two lines come together: their paint first touches some way before they are one line, and the thinning parts them there —
  // the junction goes on along the one line by about the paint's width (as moved by hand in the example: 0.9–1.6% of the lock's
  // length), the two lines straight to it
  {
    const double shift = 0.0125 * Lk;
    auto pointAlong = [](const Branch& b, int nd, double reach) { const bool atA = b.a == nd; const int n = int(b.pts.size()); double s = 0;
      for (int k = 1; k < n; k++) { const Vec3& p = atA ? b.pts[size_t(k)] : b.pts[size_t(n - 1 - k)], &q = atA ? b.pts[size_t(k - 1)] : b.pts[size_t(n - k)]; s += p.distanceTo(q); if (s >= reach) return p; }
      return atA ? b.pts.back() : b.pts.front(); };
    const size_t nN = G.nodes.size();
    for (size_t nd = 0; nd < nN; nd++) {
      std::vector<int> bs; for (size_t i = 0; i < G.br.size(); i++) if (!G.br[i].dead && G.br[i].a != G.br[i].b && (G.br[i].a == int(nd) || G.br[i].b == int(nd))) bs.push_back(int(i));
      if (bs.size() != 3) continue;
      const double r0 = drAt(G.nodes[nd].p); if (r0 < 0.1 * Lk || r0 > 0.95 * Lk) continue;
      int tw = -1, nt = 0; for (int bi : bs) if (drAt(pointAlong(G.br[size_t(bi)], int(nd), 0.05 * Lk)) > r0) { tw = bi; nt++; }
      if (nt != 1 || G.br[size_t(tw)].length() < 3 * shift) continue;                       // (two in from the root, one on to the tip)
      // (only where the two bend in to meet — over 20° between them in their last stretch: the skeleton turning to the point the paint
      // first touched. Lines that come in alongside already meet where they are one; the example kept those)
      { std::vector<Vec3> tin; for (int bi : bs) if (bi != tw) tin.push_back(endTan(G.br[size_t(bi)], int(nd), 0.0165 * Lk));
        if (tin.size() != 2 || tin[0].dot(tin[1]) > std::cos(20 * 3.14159265358979 / 180)) continue; }
      { Branch& T = G.br[size_t(tw)]; if (T.a != int(nd)) { std::reverse(T.pts.begin(), T.pts.end()); std::swap(T.a, T.b); }
        double s = 0; size_t k = 1; for (; k < T.pts.size(); k++) { s += T.pts[k].distanceTo(T.pts[k - 1]); if (s >= shift) break; }
        if (k + 1 >= T.pts.size()) continue;
        T.pts.erase(T.pts.begin(), T.pts.begin() + long(k)); G.nodes[nd].p = T.pts.front(); }
      const Vec3 q = G.nodes[nd].p;
      for (int bi : bs) { if (bi == tw) continue; Branch& b = G.br[size_t(bi)]; const bool atA = b.a == int(nd); if (atA) std::reverse(b.pts.begin(), b.pts.end());
        double t = 0; while (b.pts.size() > 3) { t += b.pts.back().distanceTo(b.pts[b.pts.size() - 2]); if (t > shift) break; b.pts.pop_back(); }
        b.pts.push_back(q); if (atA) std::reverse(b.pts.begin(), b.pts.end()); }
    }
  }

  // 8e) two lines that come into a junction side by side, closer than the paint's half width over a long stretch (one stroke of paint
  // the thinning kept as two): they become one line where they come that close — the junction moves back along them to there (else the
  // two curves overlap, and the walls between them twist)
  {
    const double zipD = 0.003 * Lk, minRun = 0.012 * Lk;
    const size_t nN = G.nodes.size();
    for (size_t nd = 0; nd < nN; nd++) {
      std::vector<int> bs; for (size_t i = 0; i < G.br.size(); i++) if (!G.br[i].dead && G.br[i].a != G.br[i].b && (G.br[i].a == int(nd) || G.br[i].b == int(nd))) bs.push_back(int(i));
      if (bs.size() != 3) continue;
      const double r0 = drAt(G.nodes[nd].p);
      std::vector<int> in; int out = -1;
      for (int bi : bs) { const Branch& b = G.br[size_t(bi)]; const Vec3 far = b.a == int(nd) ? b.pts[std::min(b.pts.size() - 1, size_t(10))] : b.pts[b.pts.size() - 1 - std::min(b.pts.size() - 1, size_t(10))];
        if (drAt(far) < r0) in.push_back(bi); else out = bi; }
      if (in.size() != 2 || out < 0) continue;
      for (int bi : in) { Branch& b = G.br[size_t(bi)]; if (b.b != int(nd)) { std::reverse(b.pts.begin(), b.pts.end()); std::swap(b.a, b.b); } }   // (both end at the node)
      { Branch& O = G.br[size_t(out)]; if (O.a != int(nd)) { std::reverse(O.pts.begin(), O.pts.end()); std::swap(O.a, O.b); } }
      Branch& A = G.br[size_t(in[0])]; Branch& B = G.br[size_t(in[1])];
      // (back along A from the node: how far B stays this close)
      auto nearB = [&](const Vec3& p, size_t& j) { double bd = 1e300; for (size_t q = 0; q < B.pts.size(); q++) { const double d = (B.pts[q] - p).lengthSq(); if (d < bd) { bd = d; j = q; } } return std::sqrt(bd); };
      double s = 0; size_t kA = A.pts.size() - 1, kB = B.pts.size() - 1;
      for (size_t k = A.pts.size() - 1; k-- > 1;) { s += A.pts[k].distanceTo(A.pts[k + 1]); size_t j = 0; if (nearB(A.pts[k], j) > zipD) break; kA = k; kB = j; }
      double run = 0; for (size_t k = kA; k + 1 < A.pts.size(); k++) run += A.pts[k].distanceTo(A.pts[k + 1]);
      if (run < minRun || kA < 2 || kB < 2) continue;
      // (the run's middle line: the trunk on from the new junction to the old one, then on as before)
      std::vector<Vec3> mid; for (size_t k = kA; k < A.pts.size(); k++) { size_t j = 0; nearB(A.pts[k], j); mid.push_back((A.pts[k] + B.pts[j]) * 0.5); }
      mid.back() = G.nodes[nd].p;
      Branch& O = G.br[size_t(out)]; O.pts.insert(O.pts.begin(), mid.begin(), mid.end() - 1);
      A.pts.resize(kA + 1); B.pts.resize(kB + 1); const Vec3 q = mid.front(); A.pts.back() = q; B.pts.back() = q; G.nodes[nd].p = q;
    }
  }

  // 9) the lines: root → tip, smoothed
  auto dr = [&](int v) { const float x = d2[size_t(v)]; return root1 ? double(x) : double(Lk) - double(x); };   // (distance from the root along the lock)
  for (auto& b : G.br) { if (b.dead) continue; Line L; L.pts = b.pts; L.a = b.a; L.b = b.b; R.lines.push_back(std::move(L)); }
  { struct Cell { std::vector<int> v; }; const double cs = std::max(1e-9, diag / 64); std::unordered_map<long long, Cell> grid;
    auto key = [&](const Vec3& p) { return ((long long)std::floor((p.x - bb.min.x) / cs) * 100003LL + (long long)std::floor((p.y - bb.min.y) / cs)) * 100003LL + (long long)std::floor((p.z - bb.min.z) / cs); };
    std::unordered_set<long long> need; for (auto& L : R.lines) { need.insert(key(L.pts.front())); need.insert(key(L.pts.back())); }
    for (int i = 0; i < nv; i++) { const long long k = key(m.pos[size_t(i)]); if (need.count(k)) grid[k].v.push_back(i); }
    auto nearV = [&](const Vec3& p) { int best = -1; double bd = 1e300; for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
        const Vec3 q = p + Vec3(dx * cs, dy * cs, dz * cs); auto it = grid.find(key(q)); if (it == grid.end()) continue; for (int v : it->second.v) { const double dd = (m.pos[size_t(v)] - p).lengthSq(); if (dd < bd) { bd = dd; best = v; } } }
      return best; };
    for (auto& L : R.lines) { const int va = nearV(L.pts.front()), vb = nearV(L.pts.back()); if (va >= 0 && vb >= 0 && dr(va) > dr(vb)) { std::reverse(L.pts.begin(), L.pts.end()); std::swap(L.a, L.b); } } }
  // (nodes: renumbered, the line ends as they are)
  { std::unordered_map<int, int> remap; for (auto& L : R.lines) for (int* x : {&L.a, &L.b}) { auto it = remap.find(*x); if (it == remap.end()) { const int id = int(R.nodes.size()); remap[*x] = id; R.nodes.push_back(G.nodes[size_t(*x)].p); *x = id; } else *x = it->second; } }
  // smoothing on the surface: each line evened out (a point every 0.4% of the lock's length) and relaxed without shrinking, put back
  // onto the lock after each pass (the closest point of the polygons round the nearest vertex). The ends (the nodes) stay. The stairs of
  // a way over the mesh's edges (at the tip, where its polygons are long) and the kinks next to a junction go
  auto onSurface = [&](const Vec3& p) {
    const int v = vg.nearest(m.pos, p); if (v < 0) return p; Vec3 best = m.pos[size_t(v)]; double bd = (best - p).lengthSq();
    for (int k = inc.start[size_t(v)]; k < inc.start[size_t(v) + 1]; k++) { const int f = inc.face[size_t(k)], b = m.faceStart[size_t(f)], n = m.faceStart[size_t(f) + 1] - b;
      for (int t = 1; t + 1 < n; t++) { const Vec3 q = closestOnTri(p, m.pos[size_t(m.faceVerts[size_t(b)])], m.pos[size_t(m.faceVerts[size_t(b + t)])], m.pos[size_t(m.faceVerts[size_t(b + t + 1)])]);
        const double dd = (q - p).lengthSq(); if (dd < bd) { bd = dd; best = q; } } }
    return best; };
  const double h = 0.004 * Lk;
  if (h > 0) for (auto& L : R.lines) {                              // (a lock of no length — every point in one place: nothing to even out, and a step of 0 would never end)
    std::vector<Vec3> E{L.pts.front()}; double acc = 0;                                       // (evened out)
    for (size_t i = 1; i < L.pts.size(); i++) { const Vec3 a = L.pts[i - 1], b = L.pts[i]; const double l = a.distanceTo(b); double t0 = 0;
      while (acc + (l - t0) >= h) { t0 += h - acc; acc = 0; E.push_back(a + (b - a) * (t0 / std::max(l, 1e-12))); }
      acc += l - t0; }
    if (E.back().distanceTo(L.pts.back()) < 0.5 * h && E.size() > 1) E.back() = L.pts.back(); else E.push_back(L.pts.back());
    L.pts = std::move(E);
    for (int pass = 0; pass < std::max(o.smooth, 1) * 2; pass++) {
      for (double f : {0.5, -0.53}) { auto T = L.pts; for (size_t i = 1; i + 1 < L.pts.size(); i++) T[i] = L.pts[i] + ((L.pts[i - 1] + L.pts[i + 1]) * 0.5 - L.pts[i]) * f; L.pts = T; }
      for (size_t i = 1; i + 1 < L.pts.size(); i++) L.pts[i] = onSurface(L.pts[i]); }
    L.pts.front() = R.nodes[size_t(L.a)]; L.pts.back() = R.nodes[size_t(L.b)]; }
  for (const Vec3& p : R.nodes) R.nodeDr.push_back(drAt(p));
  R.info = QObject::tr("%1 lines, %2 junctions / ends").arg(R.lines.size()).arg(R.nodes.size());
  return R;
}

}  // namespace hm::locklines
