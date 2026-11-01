// Face features: the holes of a shell (a mouth, an eye) found along its borders and split into an upper and a lower
// chain at their corners; the chains' points paired by where they lie along them (by length), for Unfold's ties.
#include "scene/Uv.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace hm::uv {

namespace {
struct Dsu { std::vector<int> p; explicit Dsu(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
  int find(int x) { while (p[size_t(x)] != x) x = p[size_t(x)] = p[size_t(p[size_t(x)])]; return x; }
  void unite(int a, int b) { a = find(a); b = find(b); if (a != b) p[size_t(a)] = b; } };
double len(const PolyMesh& m, const std::vector<int>& c) { double l = 0; for (size_t i = 1; i < c.size(); i++) l += m.pos[size_t(c[i])].distanceTo(m.pos[size_t(c[i - 1])]); return l; }
double meanY(const PolyMesh& m, const std::vector<int>& c) { double y = 0; int n = 0; for (size_t i = 1; i + 1 < c.size(); i++) { y += m.pos[size_t(c[i])].y; n++; } return n ? y / n : 0; }
// a closed loop of vertices split at its two points farthest apart: (upper, lower), both from the corner more to −X
std::optional<Feature> split(const PolyMesh& m, const std::vector<int>& loop) {
  const size_t n = loop.size(); if (n < 4) return std::nullopt;
  size_t i0 = 0, i1 = 1; double best = -1;
  for (size_t i = 0; i < n; i++) for (size_t j = i + 1; j < n; j++) { const double d = m.pos[size_t(loop[i])].distanceTo(m.pos[size_t(loop[j])]); if (d > best) { best = d; i0 = i; i1 = j; } }
  if (m.pos[size_t(loop[i1])].x < m.pos[size_t(loop[i0])].x) std::swap(i0, i1);
  std::vector<int> p, q;
  for (size_t k = i0; ; k = (k + 1) % n) { p.push_back(loop[k]); if (k == i1) break; }
  for (size_t k = i0; ; k = (k + n - 1) % n) { q.push_back(loop[k]); if (k == i1) break; }
  if (p.size() < 2 || q.size() < 2) return std::nullopt;
  Feature f; if (meanY(m, p) >= meanY(m, q)) { f.a = p; f.b = q; } else { f.a = q; f.b = p; }
  return f;
}
// the closed loops (each vertex two edges) of a set of edges as vertex pairs
std::vector<std::vector<int>> loops(const std::vector<std::array<int, 2>>& E) {
  std::map<int, std::vector<int>> adj; for (auto& e : E) { adj[e[0]].push_back(e[1]); adj[e[1]].push_back(e[0]); }
  std::set<int> seen; std::vector<std::vector<int>> out;
  for (auto& [v0, nb] : adj) {
    if (seen.count(v0) || nb.size() != 2) continue;
    std::vector<int> L{v0}; seen.insert(v0); int prev = v0, cur = nb[0]; bool ok = true;
    while (cur != v0) { const auto& a = adj[cur]; if (a.size() != 2 || seen.count(cur)) { ok = false; break; } L.push_back(cur); seen.insert(cur); const int nx = a[0] == prev ? a[1] : a[0]; prev = cur; cur = nx; }
    if (ok && L.size() >= 4) out.push_back(L);
  }
  return out;
}
}  // namespace

std::vector<Feature> detectFeatures(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces) {
  const std::vector<char> seam = seamFlags(m, T);
  auto cut = [&](int e) { return T.ef1[size_t(e)] < 0 || seam[size_t(e)]; };
  Dsu fu(size_t(T.nf)); for (int e = 0; e < T.ne; e++) if (!cut(e)) fu.unite(T.ef0[size_t(e)], T.ef1[size_t(e)]);
  std::set<int> want; for (int f : faces) if (f >= 0 && f < T.nf) want.insert(fu.find(f));
  std::map<int, std::vector<std::array<int, 2>>> rim;            // (each shell's border edges)
  for (int e = 0; e < T.ne; e++) if (cut(e)) for (int f : {T.ef0[size_t(e)], T.ef1[size_t(e)]}) { if (f < 0) continue; const int r = fu.find(f); if (faces.empty() || want.count(r)) rim[r].push_back({T.ea[size_t(e)], T.eb[size_t(e)]}); }
  std::vector<Feature> out;
  for (auto& [r, E] : rim) {
    auto Ls = loops(E); if (Ls.size() < 2) continue;
    size_t outer = 0; for (size_t i = 1; i < Ls.size(); i++) if (len(m, Ls[i]) + m.pos[size_t(Ls[i][0])].distanceTo(m.pos[size_t(Ls[i].back())]) > len(m, Ls[outer]) + m.pos[size_t(Ls[outer][0])].distanceTo(m.pos[size_t(Ls[outer].back())])) outer = i;
    for (size_t i = 0; i < Ls.size(); i++) if (i != outer) if (auto f = split(m, Ls[i])) out.push_back(*f);
  }
  return out;
}

std::optional<Feature> featureFromEdges(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& edges) {
  std::vector<std::array<int, 2>> E; for (int e : edges) if (e >= 0 && e < T.ne) E.push_back({T.ea[size_t(e)], T.eb[size_t(e)]});
  // (the upper chain and the lower between the same corners make one loop too)
  auto Ls = loops(E); if (Ls.size() == 1) return split(m, Ls[0]);
  return std::nullopt;
}

std::optional<Feature> mirrorFeature(const Feature& f, const std::vector<int>& vp) {
  Feature g = f; for (auto* c : {&g.a, &g.b}) for (int& v : *c) { if (v < 0 || size_t(v) >= vp.size() || vp[size_t(v)] < 0) return std::nullopt; v = vp[size_t(v)]; }
  if (g.a == f.a && g.b == f.b) return std::nullopt;            // (across the plane: its own mirror)
  std::reverse(g.a.begin(), g.a.end()); std::reverse(g.b.begin(), g.b.end());   // (from the corner more to −X again)
  return g;
}

FeatureTies featureTies(const std::vector<Feature>& fs, const std::vector<int>& vloc, const std::vector<Vec3>& pos, const std::vector<char>* inner) {
  FeatureTies R;
  auto L = [&](int v) { const int l = v >= 0 && size_t(v) < vloc.size() ? vloc[size_t(v)] : -1; return l >= 0 && inner && !(size_t(l) < inner->size() && (*inner)[size_t(l)]) ? -1 : l; };
  for (const Feature& f : fs) {
    if (f.a.size() < 2 || f.b.size() < 2) continue;
    const int c0 = L(f.a.front()), c1 = L(f.a.back()); if (c0 < 0 || c1 < 0 || c0 == c1) continue;
    std::vector<int> A, B; std::vector<double> ta, tb; bool ok = true;
    auto params = [&](const std::vector<int>& c, std::vector<int>& loc, std::vector<double>& t) {
      double tot = 0; for (size_t i = 1; i < c.size(); i++) tot += pos[size_t(c[i])].distanceTo(pos[size_t(c[i - 1])]);
      double acc = 0; for (size_t i = 1; i + 1 < c.size(); i++) { acc += pos[size_t(c[i])].distanceTo(pos[size_t(c[i - 1])]); const int l = L(c[i]); if (l < 0) ok = false; loc.push_back(l); t.push_back(tot > 0 ? acc / tot : 0); } };
    params(f.a, A, ta); params(f.b, B, tb); if (!ok) continue;
    // (the same count: one to one; else the shorter's each to the nearest along the longer, in order)
    const bool aShort = A.size() <= B.size(); const auto &S = aShort ? A : B, &Lg = aShort ? B : A; const auto &ts = aShort ? ta : tb, &tl = aShort ? tb : ta;
    size_t from = 0;
    for (size_t i = 0; i < S.size(); i++) {
      size_t j = from; if (S.size() != Lg.size()) { double bd = 1e300; for (size_t k = from; k + (S.size() - i) <= Lg.size(); k++) { const double d = std::abs(tl[k] - ts[i]); if (d < bd) { bd = d; j = k; } } } else j = i;
      if (j >= Lg.size()) break; from = j + 1;
      const int up = aShort ? S[i] : Lg[j], lo = aShort ? Lg[j] : S[i];
      if (up != lo) R.pairs.push_back({lo, up, f.closed ? 1 : 0});
    }
    if (f.straight) R.level.push_back({c0, c1});
    if (R.frame[0] < 0) R.frame = {c0, c1};
  }
  return R;
}

}  // namespace hm::uv

namespace hm::uv {
std::vector<char> innerRims(int n, const std::vector<std::array<int, 3>>& tris, const std::vector<Vec3>& X) {
  std::vector<char> in(size_t(n), 0);
  std::map<std::pair<int, int>, int> dir; for (auto& t : tris) for (int j = 0; j < 3; j++) dir[{t[size_t(j)], t[size_t((j + 1) % 3)]}]++;
  std::vector<std::array<int, 2>> E; for (auto& [e, c] : dir) if (!dir.count({e.second, e.first})) E.push_back({e.first, e.second});
  auto Ls = loops(E); if (Ls.size() < 2) return in;
  auto L = [&](const std::vector<int>& c) { double l = 0; for (size_t i = 0; i < c.size(); i++) l += X[size_t(c[i])].distanceTo(X[size_t(c[(i + 1) % c.size()])]); return l; };
  size_t outer = 0; for (size_t i = 1; i < Ls.size(); i++) if (L(Ls[i]) > L(Ls[outer])) outer = i;
  for (size_t i = 0; i < Ls.size(); i++) if (i != outer) for (int v : Ls[i]) if (v >= 0 && v < n) in[size_t(v)] = 1;
  return in;
}
}  // namespace hm::uv
