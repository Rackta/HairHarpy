#include "scene/TopoEdit.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace hm::tedit {

MeshEd MeshEd::from(const PolyMesh& m) {
  MeshEd E; E.pos = m.pos; const int nf = m.faceCount();
  if (m.vcol.size() == m.pos.size()) E.vcol = m.vcol;
  E.hasUV = !m.cornerUV.empty() && m.cornerUV.size() == m.faceVerts.size(); E.hasMat = int(m.faceMat.size()) == nf;
  E.hasN = !m.cornerNormal.empty() && m.cornerNormal.size() == m.faceVerts.size();
  const bool fg = int(m.faceGroup.size()) == nf;
  // (the UV Sets: carried when the mesh has them whole)
  E.nSets = E.hasUV && int(m.faceSets.size()) == nf && int(m.faceLive.size()) == nf && m.setUV.size() == m.uvSetNames.size() ? std::min(int(m.uvSetNames.size()), kUvSets) : 0;
  for (int s = 0; s < E.nSets; s++) if (m.setUV[size_t(s)].size() != m.faceVerts.size()) E.nSets = 0;
  E.f.resize(static_cast<size_t>(nf));
  for (int i = 0; i < nf; i++) { F& F0 = E.f[size_t(i)];
    if (E.nSets) { F0.sets = m.faceSets[size_t(i)]; F0.live = m.faceLive[size_t(i)]; }
    for (int k = m.faceStart[size_t(i)]; k < m.faceStart[size_t(i) + 1]; k++) { F0.v.push_back(m.faceVerts[size_t(k)]);
      if (E.hasUV) { UVx x{}; x[0] = m.cornerUV[size_t(k)][0]; x[1] = m.cornerUV[size_t(k)][1]; for (int s = 0; s < E.nSets; s++) { x[size_t(2 + 2 * s)] = m.setUV[size_t(s)][size_t(k)][0]; x[size_t(3 + 2 * s)] = m.setUV[size_t(s)][size_t(k)][1]; } F0.uv.push_back(x); } if (E.hasN) F0.n.push_back(m.cornerNormal[size_t(k)]); }
    F0.g = fg ? m.faceGroup[size_t(i)] : 0; F0.mat = E.hasMat ? m.faceMat[size_t(i)] : -1; }
  for (auto& h : m.hardEdges) E.hard.insert(ek(h[0], h[1]));
  for (auto& s : m.seams) E.seam.insert(ek(s[0], s[1]));
  for (auto& c : m.creases) E.crease[ek(c.a, c.b)] = c.w;
  return E;
}

PolyMesh MeshEd::to(const PolyMesh& src, std::vector<int>* vmapOut) const {
  PolyMesh o; o.groupNames = src.groupNames; o.matNames = src.matNames; o.matMaps = src.matMaps; o.matColor = src.matColor; o.matAlpha = src.matAlpha; o.mtllib = src.mtllib; o.flipY = src.flipY;
  if (o.groupNames.empty()) o.groupNames = {"Polygroup"};
  if (nSets) { for (int s = 0; s < nSets; s++) o.uvSetNames.push_back(size_t(s) < src.uvSetNames.size() ? src.uvSetNames[size_t(s)] : QString("UV Set %1").arg(s + 1)); o.setUV.resize(size_t(nSets)); }
  std::vector<int> vmap(pos.size(), -1); int nv = 0;
  for (const F& F0 : f) for (int v : F0.v) if (vmap[size_t(v)] < 0) vmap[size_t(v)] = nv++;
  o.pos.resize(static_cast<size_t>(nv)); for (size_t v = 0; v < pos.size(); v++) if (vmap[v] >= 0) o.pos[size_t(vmap[v])] = pos[v];
  if (!vcol.empty()) {                                            // (polypaint carried; a vertex made since — its neighbours'"'"' mean)
    std::vector<std::array<float, 3>> C = vcol; C.resize(pos.size(), {-1, -1, -1});
    for (int pass = 0; pass < 8; pass++) { bool open = false;
      for (const F& F0 : f) { const size_t n = F0.v.size(); for (size_t k = 0; k < n; k++) { auto& c = C[size_t(F0.v[k])]; if (c[0] >= 0) continue;
        float s[3] = {0, 0, 0}; int cnt = 0; for (int w : {F0.v[(k + n - 1) % n], F0.v[(k + 1) % n]}) if (C[size_t(w)][0] >= 0) { for (int i = 0; i < 3; i++) s[i] += C[size_t(w)][size_t(i)]; cnt++; }
        if (cnt) c = {s[0] / cnt, s[1] / cnt, s[2] / cnt}; else open = true; } }
      if (!open) break; }
    o.vcol.resize(static_cast<size_t>(nv)); for (size_t v = 0; v < pos.size(); v++) if (vmap[v] >= 0) { auto c = C[v]; if (c[0] < 0) c = {1, 1, 1}; o.vcol[size_t(vmap[v])] = c; } }
  std::set<EK> edges;
  for (const F& F0 : f) {
    std::vector<int> vs; for (int v : F0.v) vs.push_back(vmap[size_t(v)]);
    o.addFace(vs.data(), int(vs.size()));
    if (hasUV) for (size_t k = 0; k < F0.v.size(); k++) { const UVx x = k < F0.uv.size() ? F0.uv[k] : UVx{}; o.cornerUV.push_back({x[0], x[1]}); for (int s = 0; s < nSets; s++) o.setUV[size_t(s)].push_back({x[size_t(2 + 2 * s)], x[size_t(3 + 2 * s)]}); }
    if (nSets) { o.faceSets.push_back(F0.sets); o.faceLive.push_back(int8_t(F0.live)); }
    if (hasN) for (size_t k = 0; k < F0.v.size(); k++) o.cornerNormal.push_back(k < F0.n.size() ? F0.n[k] : std::array<float, 3>{0, 0, 0});
    o.faceGroup.push_back(std::clamp(F0.g, 0, int(o.groupNames.size()) - 1)); if (hasMat) o.faceMat.push_back(F0.mat);
    for (size_t k = 0; k < F0.v.size(); k++) edges.insert(ek(F0.v[k], F0.v[(k + 1) % F0.v.size()]));
  }
  auto keep = [&](const EK& e) { return edges.count(e) && vmap[size_t(e.first)] >= 0 && vmap[size_t(e.second)] >= 0; };
  for (auto& e : hard) if (keep(e)) o.hardEdges.push_back({vmap[size_t(e.first)], vmap[size_t(e.second)]});
  for (auto& e : seam) if (keep(e)) o.seams.push_back({vmap[size_t(e.first)], vmap[size_t(e.second)]});
  for (auto& [e, w] : crease) if (keep(e)) o.creases.push_back({vmap[size_t(e.first)], vmap[size_t(e.second)], w});
  if (vmapOut) *vmapOut = std::move(vmap);
  return o;
}

int MeshEd::corner(int fi, int v) const { const auto& V = f[size_t(fi)].v; for (size_t k = 0; k < V.size(); k++) if (V[k] == v) return int(k); return -1; }

std::vector<int> MeshEd::facesOfEdge(int a, int b) const {
  std::vector<int> out;
  for (size_t i = 0; i < f.size(); i++) { const auto& V = f[i].v; const size_t n = V.size();
    for (size_t k = 0; k < n; k++) { const int x = V[k], y = V[(k + 1) % n]; if ((x == a && y == b) || (x == b && y == a)) { out.push_back(int(i)); break; } } }
  return out;
}
std::vector<int> MeshEd::facesOfVert(int v) const { std::vector<int> out; for (size_t i = 0; i < f.size(); i++) if (corner(int(i), v) >= 0) out.push_back(int(i)); return out; }

void MeshEd::insertOnEdge(int a, int b, const std::vector<int>& vs, const std::vector<double>& ts) {
  if (vs.empty()) return;
  for (int fi : facesOfEdge(a, b)) {
    F& F0 = f[size_t(fi)]; const size_t n = F0.v.size();
    for (size_t k = 0; k < n; k++) {
      const size_t k2 = (k + 1) % n; const int x = F0.v[k], y = F0.v[k2];
      if (!((x == a && y == b) || (x == b && y == a))) continue;
      const bool fwd = x == a;                                     // (the polygon runs a → b: the new ones in order, else reversed)
      std::vector<int> ins; std::vector<UVx> iuv; std::vector<std::array<float, 3>> in3;
      for (size_t i = 0; i < vs.size(); i++) { const size_t j = fwd ? i : vs.size() - 1 - i; ins.push_back(vs[j]); const double t = fwd ? ts[j] : 1 - ts[j];
        if (hasUV) { const auto& u0 = F0.uv[k]; const auto& u1 = F0.uv[k2]; UVx x{}; for (size_t q = 0; q < x.size(); q++) x[q] = float(u0[q] + (u1[q] - u0[q]) * t); iuv.push_back(x); }   // (every set's UV lerped too)
        if (hasN) { const auto& a = F0.n[k]; const auto& b = F0.n[k2]; double x = a[0] + (b[0] - a[0]) * t, y = a[1] + (b[1] - a[1]) * t, z = a[2] + (b[2] - a[2]) * t, l = std::sqrt(x * x + y * y + z * z); if (l > 0) { x /= l; y /= l; z /= l; }
          in3.push_back({float(x), float(y), float(z)}); } }
      F0.v.insert(F0.v.begin() + long(k + 1), ins.begin(), ins.end());
      if (hasUV) F0.uv.insert(F0.uv.begin() + long(k + 1), iuv.begin(), iuv.end());
      if (hasN) F0.n.insert(F0.n.begin() + long(k + 1), in3.begin(), in3.end());
      break;
    }
  }
  // (the edge's marks onto its pieces)
  const EK e0 = ek(a, b); std::vector<int> chain{a}; chain.insert(chain.end(), vs.begin(), vs.end()); chain.push_back(b);
  if (hard.count(e0)) { hard.erase(e0); for (size_t i = 0; i + 1 < chain.size(); i++) hard.insert(ek(chain[i], chain[i + 1])); }
  if (seam.count(e0)) { seam.erase(e0); for (size_t i = 0; i + 1 < chain.size(); i++) seam.insert(ek(chain[i], chain[i + 1])); }
  if (auto c = crease.find(e0); c != crease.end()) { float w = c->second; crease.erase(c); for (size_t i = 0; i + 1 < chain.size(); i++) crease[ek(chain[i], chain[i + 1])] = w; }
}

int MeshEd::splitEdge(int a, int b, double t) {
  const int v = addVert(pos[size_t(a)] + (pos[size_t(b)] - pos[size_t(a)]) * t);
  insertOnEdge(a, b, {v}, {t});
  return v;
}

int MeshEd::splitFace(int fi, int va, int vb) {
  if (fi < 0 || size_t(fi) >= f.size() || va == vb) return -1;
  F F0 = f[size_t(fi)]; const int n = int(F0.v.size()); const int ia = corner(fi, va), ib = corner(fi, vb);
  if (ia < 0 || ib < 0 || (ia + 1) % n == ib || (ib + 1) % n == ia) return -1;
  F A, B; A.g = B.g = F0.g; A.mat = B.mat = F0.mat; A.sets = B.sets = F0.sets; A.live = B.live = F0.live;
  for (int k = ia;; k = (k + 1) % n) { A.v.push_back(F0.v[size_t(k)]); if (hasUV) A.uv.push_back(F0.uv[size_t(k)]); if (hasN) A.n.push_back(F0.n[size_t(k)]); if (k == ib) break; }
  for (int k = ib;; k = (k + 1) % n) { B.v.push_back(F0.v[size_t(k)]); if (hasUV) B.uv.push_back(F0.uv[size_t(k)]); if (hasN) B.n.push_back(F0.n[size_t(k)]); if (k == ia) break; }
  f[size_t(fi)] = A; f.push_back(B);
  return int(f.size()) - 1;
}

void MeshEd::weld(const std::vector<int>& to) {
  auto map = [&](int v) { return size_t(v) < to.size() && to[size_t(v)] >= 0 ? to[size_t(v)] : v; };
  std::vector<F> out; out.reserve(f.size());
  for (F& F0 : f) {
    F N; N.g = F0.g; N.mat = F0.mat; N.sets = F0.sets; N.live = F0.live;
    for (size_t k = 0; k < F0.v.size(); k++) { const int v = map(F0.v[k]); if (!N.v.empty() && N.v.back() == v) continue; N.v.push_back(v); if (hasUV) N.uv.push_back(F0.uv[k]); if (hasN) N.n.push_back(k < F0.n.size() ? F0.n[k] : std::array<float, 3>{0, 0, 0}); }
    while (N.v.size() > 1 && N.v.front() == N.v.back()) { N.v.pop_back(); if (hasUV) N.uv.pop_back(); if (hasN) N.n.pop_back(); }
    // (a polygon folded onto itself — a vertex twice: dropped)
    std::set<int> seen(N.v.begin(), N.v.end()); if (N.v.size() < 3 || seen.size() != N.v.size()) continue;
    out.push_back(std::move(N));
  }
  f = std::move(out);
  auto remapSet = [&](std::set<EK>& s) { std::set<EK> o; for (auto& e : s) { EK x = ek(map(e.first), map(e.second)); if (x.first != x.second) o.insert(x); } s = o; };
  remapSet(hard); remapSet(seam);
  std::map<EK, float> c2; for (auto& [e, w] : crease) { EK x = ek(map(e.first), map(e.second)); if (x.first != x.second) c2[x] = w; } crease = c2;
}

void MeshEd::removeFaces(const std::vector<char>& del) {
  std::vector<F> out; for (size_t i = 0; i < f.size(); i++) if (i >= del.size() || !del[i]) out.push_back(std::move(f[i])); f = std::move(out);
}

int MeshEd::addFace(const std::vector<int>& vs, int g, int mat) {
  F N; N.v = vs;
  int g0 = -1, m0 = -1;
  for (int v : vs) for (size_t i = 0; i < f.size() && (g0 < 0); i++) if (corner(int(i), v) >= 0) { g0 = f[i].g; m0 = f[i].mat; N.sets = f[i].sets; N.live = f[i].live; }   // (its UV Sets: a neighbour's)
  N.g = g >= 0 ? g : std::max(0, g0); N.mat = mat >= 0 ? mat : m0;
  if (hasUV) for (int v : vs) { UVx u{}; for (size_t i = 0; i < f.size(); i++) if (int c = corner(int(i), v); c >= 0) { u = f[i].uv[size_t(c)]; break; } N.uv.push_back(u); }
  if (hasN) N.n.assign(vs.size(), {0, 0, 0});                     // (made from the shape when the mesh is written)
  f.push_back(std::move(N)); const int fi = int(f.size()) - 1;
  orient(fi);
  return fi;
}

void MeshEd::orient(int fi) {
  F& N = f[size_t(fi)]; const size_t n = N.v.size();
  for (size_t k = 0; k < n; k++) {
    const int a = N.v[k], b = N.v[(k + 1) % n];
    for (size_t i = 0; i < f.size(); i++) { if (int(i) == fi) continue; const auto& V = f[i].v; const size_t m = V.size();
      for (size_t j = 0; j < m; j++) { if (V[j] == b && V[(j + 1) % m] == a) return;   // (runs it the other way: agrees)
        if (V[j] == a && V[(j + 1) % m] == b) { std::reverse(N.v.begin(), N.v.end()); if (hasUV) std::reverse(N.uv.begin(), N.uv.end()); std::reverse(N.n.begin(), N.n.end()); return; } } }
  }
}

Vec3 MeshEd::normal(int fi) const {
  const auto& V = f[size_t(fi)].v; Vec3 n; const size_t m = V.size();
  for (size_t k = 0; k < m; k++) { const Vec3& a = pos[size_t(V[k])]; const Vec3& b = pos[size_t(V[(k + 1) % m])];
    n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
  return n;
}
Vec3 MeshEd::center(int fi) const { const auto& V = f[size_t(fi)].v; Vec3 c; for (int v : V) c += pos[size_t(v)]; return V.empty() ? c : c * (1.0 / double(V.size())); }
Vec3 MeshEd::vertNormal(int v) const { Vec3 n; for (size_t i = 0; i < f.size(); i++) if (corner(int(i), v) >= 0) { Vec3 x = normal(int(i)); if (x.lengthSq() > 0) n += x.normalized(); } return n.lengthSq() > 0 ? n.normalized() : Vec3(0, 1, 0); }

std::vector<std::pair<int, int>> regionBorder(const MeshEd& E, const std::set<int>& faces) {
  std::map<EK, int> cnt; std::map<EK, std::pair<int, int>> dir;
  for (int fi : faces) { const auto& V = E.f[size_t(fi)].v; const size_t n = V.size();
    for (size_t k = 0; k < n; k++) { const int a = V[k], b = V[(k + 1) % n]; cnt[ek(a, b)]++; dir[ek(a, b)] = {a, b}; } }
  std::vector<std::pair<int, int>> out; for (auto& [e, c] : cnt) if (c == 1) out.push_back(dir[e]);
  return out;
}

std::vector<Chain> edgeChains(const std::vector<EK>& edges) {
  std::map<int, std::vector<int>> adj; for (auto& e : edges) { adj[e.first].push_back(e.second); adj[e.second].push_back(e.first); }
  std::set<EK> used; std::vector<Chain> out;
  auto walk = [&](int start) { Chain c; c.v.push_back(start); int cur = start;
    for (;;) { int nx = -1; for (int w : adj[cur]) if (!used.count(ek(cur, w))) { nx = w; break; } if (nx < 0) break;
      used.insert(ek(cur, nx)); if (nx == start) { c.closed = true; break; } c.v.push_back(nx); cur = nx; }
    return c; };
  for (auto& [v, a] : adj) if (a.size() == 1) { bool fresh = false; for (int w : a) if (!used.count(ek(v, w))) fresh = true; if (fresh) out.push_back(walk(v)); }   // (open chains from their ends)
  for (auto& e : edges) if (!used.count(e)) out.push_back(walk(e.first));                                                                                       // (then the loops)
  return out;
}

}  // namespace hm::tedit

namespace hm::tedit {
int dropCovered(MeshEd& E) {
  std::vector<std::vector<int>> vf(E.pos.size()); for (size_t f = 0; f < E.f.size(); f++) for (int v : E.f[f].v) vf[size_t(v)].push_back(int(f));
  std::vector<char> del(E.f.size(), 0); int n = 0;
  for (size_t f = 0; f < E.f.size(); f++) {
    const auto& F = E.f[f]; if (F.v.size() < 4) continue; const std::set<int> S(F.v.begin(), F.v.end());
    std::set<int> cand; for (int v : F.v) for (int g : vf[size_t(v)]) if (g != int(f) && !del[size_t(g)] && E.f[size_t(g)].v.size() < F.v.size()) cand.insert(g);
    // (the smaller polygons on its corners only: they cover it when every side of it is a side of one of them)
    std::set<EK> sides; int k = 0;
    for (int g : cand) { const auto& G = E.f[size_t(g)].v; bool in = true; for (int v : G) if (!S.count(v)) { in = false; break; } if (!in) continue;
      k++; for (size_t j = 0; j < G.size(); j++) sides.insert(ek(G[j], G[(j + 1) % G.size()])); }
    bool covered = k >= 2; for (size_t j = 0; j < F.v.size() && covered; j++) covered = sides.count(ek(F.v[j], F.v[(j + 1) % F.v.size()])) > 0;
    if (covered) { del[f] = 1; n++; }   // (its own split lies there: it goes, they stay)
  }
  if (n) E.removeFaces(del);
  return n;
}

int makeManifold(PolyMesh& pm) {
  if (pm.faceCount() == 0) return 0;
  MeshEd E = MeshEd::from(pm); int changes = 0;
  // (a corner repeated next to itself dropped — an edge from a vertex to itself; a polygon of fewer than three corners out)
  { std::vector<char> del(E.f.size(), 0); int n = 0;
    for (size_t f = 0; f < E.f.size(); f++) { auto& F = E.f[f];
      for (size_t k = 0; k < F.v.size() && F.v.size() > 1;) { const size_t j = (k + 1) % F.v.size(); if (F.v[k] != F.v[j]) { k++; continue; }
        F.v.erase(F.v.begin() + long(j)); if (j < F.uv.size()) F.uv.erase(F.uv.begin() + long(j)); if (j < F.n.size()) F.n.erase(F.n.begin() + long(j)); changes++; }
      if (std::set<int>(F.v.begin(), F.v.end()).size() < 3) { del[f] = 1; n++; } }
    if (n) { E.removeFaces(del); changes += n; } }
  // (doubled polygons: the same corners twice — one kept)
  { std::set<std::vector<int>> seen; std::vector<char> del(E.f.size(), 0); int n = 0;
    for (size_t f = 0; f < E.f.size(); f++) if (!seen.insert(cycleKey(E.f[f].v)).second) { del[f] = 1; n++; }
    if (n) { E.removeFaces(del); changes += n; } }
  changes += dropCovered(E);   // (a polygon over its own split: out)
  // (the edges and their polygons; for an edge of more than two — its polygons paired)
  std::map<std::pair<int, int>, std::vector<int>> ef;
  for (size_t f = 0; f < E.f.size(); f++) { const auto& V = E.f[f].v; for (size_t k = 0; k < V.size(); k++) { const int a = V[k], b = V[(k + 1) % V.size()]; ef[{std::min(a, b), std::max(a, b)}].push_back(int(f)); } }
  std::map<std::pair<int, int>, std::map<int, int>> pairOf;      // (a crowded edge: each polygon → the one it goes on into, -1 alone)
  for (auto& [e, fs] : ef) { if (fs.size() <= 2) continue; changes++;
    const Vec3 A = E.pos[size_t(e.first)], Bv = E.pos[size_t(e.second)], ax = (Bv - A).normalized(), mid = (A + Bv) * 0.5;
    auto dirOf = [&](int f) { const auto& V = E.f[size_t(f)].v; for (size_t k = 0; k < V.size(); k++) if (V[k] == e.first && V[(k + 1) % V.size()] == e.second) return 1; return -1; };
    auto away = [&](int f) { Vec3 c = E.center(f) - mid; c = c - ax * c.dot(ax); return c.normalized(); };
    std::vector<std::tuple<double, int, int>> cand;
    for (size_t i = 0; i < fs.size(); i++) for (size_t j = i + 1; j < fs.size(); j++) if (dirOf(fs[i]) != dirOf(fs[j])) cand.push_back({away(fs[i]).dot(away(fs[j])), fs[i], fs[j]});
    std::sort(cand.begin(), cand.end());                          // (the most nearly straight on first: their away sides opposite)
    auto& P = pairOf[e]; for (int f : fs) P[f] = -1;
    for (auto& [d, f, g] : cand) if (P[f] < 0 && P[g] < 0) { P[f] = g; P[g] = f; } }
  // (each vertex: its polygons joined across its sides — a side of two polygons, or a crowded side's pair; a copy of the
  // vertex for each fan beyond the first)
  std::vector<std::vector<int>> vf(E.pos.size()); for (size_t f = 0; f < E.f.size(); f++) for (int v : E.f[f].v) vf[size_t(v)].push_back(int(f));
  std::vector<std::vector<int>> F0; for (auto& F : E.f) F0.push_back(F.v);
  const size_t nv0 = E.pos.size();
  for (size_t v = 0; v < nv0; v++) { auto fs = vf[v]; std::sort(fs.begin(), fs.end()); fs.erase(std::unique(fs.begin(), fs.end()), fs.end()); if (fs.size() < 2) continue;
    std::map<int, int> up; for (int f : fs) up[f] = f; std::function<int(int)> root = [&](int x) { while (up[x] != x) x = up[x] = up[up[x]]; return x; };
    for (int f : fs) { const auto& V = F0[size_t(f)]; const size_t n = V.size(); for (size_t k = 0; k < n; k++) if (V[k] == int(v)) for (int o : {V[(k + 1) % n], V[(k + n - 1) % n]}) {
          const std::pair<int, int> e{std::min(int(v), o), std::max(int(v), o)}; auto it = ef.find(e); if (it == ef.end()) continue;
          if (it->second.size() == 2) { up[root(it->second[0])] = root(it->second[1]); continue; }
          auto p = pairOf.find(e); if (p == pairOf.end()) continue; const int g = p->second[f]; if (g >= 0) up[root(f)] = root(g); } }
    std::map<int, std::vector<int>> fans; for (int f : fs) fans[root(f)].push_back(f); if (fans.size() < 2) continue;
    bool first = true; for (auto& [r, fl] : fans) { if (first) { first = false; continue; } const int nv = E.addVert(E.pos[v]); changes++;
      for (int f : fl) for (int& x : E.f[size_t(f)].v) if (x == int(v)) x = nv; } }
  // (a side still crowded — its third polygon joined to the pair round a vertex through other sides, so no fan of its
  // own: the pair kept, each other polygon given its own copy of the side's two vertices — with the polygons round
  // each vertex it is joined to, as long as they don't come round to the pair)
  bool anyDetached = false;
  for (int pass = 0; pass < 4; pass++) {
    std::map<std::pair<int, int>, std::vector<int>> ef2;
    for (size_t f = 0; f < E.f.size(); f++) { const auto& V = E.f[f].v; for (size_t k = 0; k < V.size(); k++) { const int a = V[k], b = V[(k + 1) % V.size()]; ef2[{std::min(a, b), std::max(a, b)}].push_back(int(f)); } }
    int detached = 0;
    for (auto& [e, fs0] : ef2) {
      std::vector<int> fs = fs0; std::sort(fs.begin(), fs.end()); fs.erase(std::unique(fs.begin(), fs.end()), fs.end()); if (fs.size() <= 2) continue;
      // (still crowded now? — an earlier detach may have freed it)
      int now = 0; for (int f : fs) { const auto& V = E.f[size_t(f)].v; for (size_t k = 0; k < V.size(); k++) { const int a = V[k], b = V[(k + 1) % V.size()]; if (std::min(a, b) == e.first && std::max(a, b) == e.second) { now++; break; } } }
      if (now <= 2) continue;
      const Vec3 A = E.pos[size_t(e.first)], Bv = E.pos[size_t(e.second)], ax = (Bv - A).normalized(), mid = (A + Bv) * 0.5;
      auto away = [&](int f) { Vec3 c = E.center(f) - mid; c = c - ax * c.dot(ax); return c.normalized(); };
      int k0 = fs[0], k1 = fs[1]; double best = 2;
      for (size_t i = 0; i < fs.size(); i++) for (size_t j = i + 1; j < fs.size(); j++) { const double d = away(fs[i]).dot(away(fs[j])); if (d < best) { best = d; k0 = fs[i]; k1 = fs[j]; } }
      for (int f : fs) { if (f == k0 || f == k1) continue;
        for (int v : {e.first, e.second}) {
          // (its fan round v: the polygons on v reached across sides of exactly two polygons, not across this side)
          std::set<int> fan{f}; std::vector<int> q{f}; bool reachesPair = false;
          while (!q.empty()) { const int g = q.back(); q.pop_back(); const auto& V = E.f[size_t(g)].v; const size_t n = V.size();
            for (size_t k = 0; k < n; k++) if (V[k] == v) for (int o : {V[(k + 1) % n], V[(k + n - 1) % n]}) {
              if (std::min(v, o) == e.first && std::max(v, o) == e.second) continue;
              auto it = ef2.find({std::min(v, o), std::max(v, o)}); if (it == ef2.end() || it->second.size() != 2) continue;
              const int h = it->second[0] == g ? it->second[1] : it->second[0]; if (h == k0 || h == k1) reachesPair = true; if (fan.insert(h).second) q.push_back(h); } }
          if (reachesPair) fan = {f};
          const int nv = E.addVert(E.pos[size_t(v)]); changes++; detached++;
          for (int g : fan) for (int& x : E.f[size_t(g)].v) if (x == v) x = nv; } } }
    if (!detached) break;
    anyDetached = true;
  }
  if (changes) pm = E.to(pm);
  if (anyDetached) changes += makeManifold(pm);   // (the vertices those copies left touching only: split too)
  return changes;
}
}  // namespace hm::tedit
