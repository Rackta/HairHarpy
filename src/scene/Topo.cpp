#include "scene/Topo.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <queue>
#include <thread>

namespace hm::topo {

namespace {
inline uint64_t pairKey(int lo, int hi) { return (uint64_t(uint32_t(lo)) << 32) | uint32_t(hi); }
inline double& at(Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

// a cache per base mesh: the entry lives while the mesh does (a new base — made anew)
template <class T> struct Cache {
  std::map<const PolyMesh*, std::pair<std::weak_ptr<PolyMesh>, std::shared_ptr<T>>> m;
  std::mutex mu;
  std::shared_ptr<T> get(const std::shared_ptr<PolyMesh>& B) {
    std::lock_guard<std::mutex> l(mu);
    for (auto it = m.begin(); it != m.end();) { if (it->second.first.expired()) it = m.erase(it); else ++it; }
    auto f = m.find(B.get()); if (f != m.end() && f->second.first.lock() == B) return f->second.second;
    return nullptr;
  }
  void put(const std::shared_ptr<PolyMesh>& B, std::shared_ptr<T> v) { std::lock_guard<std::mutex> l(mu); m[B.get()] = {B, std::move(v)}; }
};
Cache<TTopo> TT; Cache<IsoClasses> ISO; Cache<Partners> PART;

Vec3 faceCenter(const PolyMesh& B, int f) {
  Vec3 c; int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1];
  for (int k = s; k < e; k++) c += B.pos[size_t(B.faceVerts[size_t(k)])];
  return c * (1.0 / (e - s));
}
}  // namespace

int triCount(const PolyMesh& m) { int t = 0; for (int f = 0; f < m.faceCount(); f++) t += m.faceStart[size_t(f) + 1] - m.faceStart[size_t(f)] - 2; return t; }

void shareTopo(const std::shared_ptr<PolyMesh>& to, const std::shared_ptr<PolyMesh>& from) {
  if (!to || !from) return;
  if (auto c = TT.get(from)) TT.put(to, std::const_pointer_cast<TTopo>(c));
}

int TTopo::edgeOf(int a, int b) const { auto it = H.find(pairKey(std::min(a, b), std::max(a, b))); return it == H.end() ? -1 : it->second; }

std::shared_ptr<const TTopo> ttopo(const std::shared_ptr<PolyMesh>& B) {
  if (!B) return nullptr;
  if (auto c = TT.get(B)) return c;
  auto T = std::make_shared<TTopo>();
  const auto& fo = B->faceStart; const auto& fv = B->faceVerts; const int nf = B->faceCount(), nc = int(fv.size()), n = int(B->pos.size());
  T->n = n; T->nf = nf; T->fo = fo; T->fv = fv; T->ce.resize(static_cast<size_t>(nc)); T->cf.resize(static_cast<size_t>(nc)); T->H.reserve(static_cast<size_t>(nc));
  for (int f = 0; f < nf; f++) {
    int s = fo[size_t(f)], e = fo[size_t(f) + 1];
    for (int k = s; k < e; k++) {
      T->cf[size_t(k)] = f; int a = fv[size_t(k)], b = fv[size_t(k + 1 < e ? k + 1 : s)], lo = std::min(a, b), hi = std::max(a, b);
      auto r = T->H.emplace(pairKey(lo, hi), T->ne);
      if (r.second) { T->ea.push_back(lo); T->eb.push_back(hi); T->ef0.push_back(f); T->ef1.push_back(-1); T->ce[size_t(k)] = T->ne++; }
      else { int x = r.first->second; T->ce[size_t(k)] = x; if (T->ef1[size_t(x)] < 0 && T->ef0[size_t(x)] != f) T->ef1[size_t(x)] = f; }
    }
  }
  T->voff.assign(size_t(n) + 1, 0); T->vfo.assign(size_t(n) + 1, 0);
  for (int e = 0; e < T->ne; e++) { T->voff[size_t(T->ea[size_t(e)]) + 1]++; T->voff[size_t(T->eb[size_t(e)]) + 1]++; }
  for (int k = 0; k < nc; k++) T->vfo[size_t(fv[size_t(k)]) + 1]++;
  for (int v = 0; v < n; v++) { T->voff[size_t(v) + 1] += T->voff[size_t(v)]; T->vfo[size_t(v) + 1] += T->vfo[size_t(v)]; }
  T->vE.resize(size_t(T->ne) * 2); T->vF.resize(static_cast<size_t>(nc));
  std::vector<int> a1(T->voff.begin(), T->voff.end() - 1), a2(T->vfo.begin(), T->vfo.end() - 1);
  for (int e = 0; e < T->ne; e++) { T->vE[size_t(a1[size_t(T->ea[size_t(e)])]++)] = e; T->vE[size_t(a1[size_t(T->eb[size_t(e)])]++)] = e; }
  for (int k = 0; k < nc; k++) T->vF[size_t(a2[size_t(fv[size_t(k)])]++)] = T->cf[size_t(k)];
  TT.put(B, T);
  return T;
}

// the edge loop through e (as the Edges tool: across vertices with 4 edges, along a rim through vertices with 3)
static int loopNext(const TTopo& T, int e, int v) {
  int s = T.voff[size_t(v)], t = T.voff[size_t(v) + 1], cnt = t - s;
  auto nfOf = [&](int x) { return (T.ef0[size_t(x)] >= 0) + (T.ef1[size_t(x)] >= 0); };
  int fe[2] = {T.ef0[size_t(e)], T.ef1[size_t(e)]}; int nfe = (fe[0] >= 0) + (fe[1] >= 0);
  bool inside = cnt == 4, rimOnes = 0; int ones = 0;
  for (int j = s; j < t; j++) { if (nfOf(T.vE[size_t(j)]) != 2) inside = false; if (nfOf(T.vE[size_t(j)]) == 1) ones++; }
  (void)rimOnes;
  bool rim = cnt == 3 && nfe == 1 && ones == 2;
  if (!inside && !rim) return -1;
  int found = -1, c = 0;
  for (int j = s; j < t; j++) { int x = T.vE[size_t(j)]; if (x == e) continue;
    bool share = false; for (int f : {T.ef0[size_t(x)], T.ef1[size_t(x)]}) if (f >= 0 && (f == fe[0] || f == fe[1])) share = true;
    if (!share) { found = x; c++; } }
  return c == 1 ? found : -1;
}
std::vector<int> edgeLoop(const TTopo& T, int e0) {
  std::vector<int> out{e0}; std::set<int> seen{e0};
  for (int v0 : {T.ea[size_t(e0)], T.eb[size_t(e0)]}) { int e = e0, v = v0;
    for (;;) { int nx = loopNext(T, e, v); if (nx < 0 || seen.count(nx)) break; seen.insert(nx); out.push_back(nx); v = T.other(nx, v); e = nx; } }
  return out;
}
std::vector<int> faceLoop(const TTopo& T, int f0, int e0) {
  std::vector<int> out{f0}; std::set<int> seen{f0};
  auto across = [&](int f, int e) { int s = T.fo[size_t(f)], n = T.fo[size_t(f) + 1] - s; if (n != 4) return -1; for (int k = 0; k < 4; k++) if (T.ce[size_t(s + k)] == e) return T.ce[size_t(s + (k + 2) % 4)]; return -1; };
  for (int start : {e0, across(f0, e0)}) {
    int f = f0, e = start;
    for (;;) {
      if (e < 0) break; int g = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; if (g < 0 || seen.count(g)) break;
      seen.insert(g); out.push_back(g); int ne = across(g, e); f = g; e = ne;
    }
  }
  return out;
}
std::optional<std::vector<int>> flood(const TTopo& T, int f0, const std::set<int>& blocked, int cap) {
  std::vector<char> seen(size_t(T.nf), 0); std::vector<int> st{f0}, out; seen[size_t(f0)] = 1;
  while (!st.empty()) {
    int f = st.back(); st.pop_back(); out.push_back(f); if (int(out.size()) > cap) return std::nullopt;
    for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) { int e = T.ce[size_t(k)], g = T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)];
      if (g >= 0 && !seen[size_t(g)] && !blocked.count(g)) { seen[size_t(g)] = 1; st.push_back(g); } }
  }
  return out;
}
std::optional<std::vector<int>> path(const TTopo& T, const std::vector<Vec3>& P, char key, int a, int b) {
  const int N = key == 'f' ? T.nf : T.n;
  std::vector<double> dist(size_t(N), 1e300); std::vector<int> prev(size_t(N), -1);
  using QE = std::pair<double, int>; std::priority_queue<QE, std::vector<QE>, std::greater<QE>> heap;
  auto pt = [&](int x) { if (key != 'f') return P[size_t(x)]; Vec3 c; int s = T.fo[size_t(x)], e = T.fo[size_t(x) + 1]; for (int k = s; k < e; k++) c += P[size_t(T.fv[size_t(k)])]; return c * (1.0 / (e - s)); };
  std::vector<int> starts = key == 'e' ? std::vector<int>{T.ea[size_t(a)], T.eb[size_t(a)]} : std::vector<int>{a};
  std::set<int> goals = key == 'e' ? std::set<int>{T.ea[size_t(b)], T.eb[size_t(b)]} : std::set<int>{b};
  for (int s : starts) { dist[size_t(s)] = 0; heap.push({0, s}); }
  int end = -1;
  while (!heap.empty()) {
    auto [d, x] = heap.top(); heap.pop(); if (d > dist[size_t(x)]) continue; if (goals.count(x)) { end = x; break; }
    Vec3 p = pt(x);
    auto relax = [&](int y) { double nd = d + p.distanceTo(pt(y)); if (nd < dist[size_t(y)]) { dist[size_t(y)] = nd; prev[size_t(y)] = x; heap.push({nd, y}); } };
    if (key == 'f') { for (int k = T.fo[size_t(x)]; k < T.fo[size_t(x) + 1]; k++) { int e = T.ce[size_t(k)], g = T.ef0[size_t(e)] == x ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; if (g >= 0) relax(g); } }
    else for (int j = T.voff[size_t(x)]; j < T.voff[size_t(x) + 1]; j++) relax(T.other(T.vE[size_t(j)], x));
  }
  if (end < 0) return std::nullopt;
  std::vector<int> chain; for (int x = end; x >= 0; x = prev[size_t(x)]) chain.push_back(x);
  if (key != 'e') return chain;
  std::vector<int> es{b}; for (size_t i = 0; i + 1 < chain.size(); i++) es.push_back(T.edgeOf(chain[i], chain[i + 1]));
  return es;
}
std::vector<int> selVerts(const TTopo& T, char key, const std::set<int>& sel) {
  std::set<int> out;
  if (key == 'f') { for (int f : sel) if (f < T.nf) for (int k = T.fo[size_t(f)]; k < T.fo[size_t(f) + 1]; k++) out.insert(T.fv[size_t(k)]); }
  else if (key == 'e') { for (int e : sel) if (e < T.ne) { out.insert(T.ea[size_t(e)]); out.insert(T.eb[size_t(e)]); } }
  else for (int v : sel) if (v < T.n) out.insert(v);
  return {out.begin(), out.end()};
}
std::vector<int> groupFaces(const PolyMesh& B, int gi) { std::vector<int> o; for (size_t f = 0; f < B.faceGroup.size(); f++) if (B.faceGroup[f] == gi) o.push_back(int(f)); return o; }

bool hasUvSets(const PolyMesh& d) {
  if (d.uvSetNames.empty() || int(d.faceSets.size()) != d.faceCount() || int(d.faceLive.size()) != d.faceCount() || d.setUV.size() != d.uvSetNames.size()) return false;
  for (auto& L : d.setUV) if (L.size() != d.faceVerts.size()) return false;
  return true;
}
void addFaceSets(PolyMesh& o, const PolyMesh& d, int f, const std::vector<int>& cs) {
  if (o.uvSetNames.empty()) { o.uvSetNames = d.uvSetNames; o.setUV.assign(d.uvSetNames.size(), {}); }
  o.faceSets.push_back(d.faceSets[size_t(f)]); o.faceLive.push_back(d.faceLive[size_t(f)]);
  for (size_t s = 0; s < o.setUV.size(); s++) for (int k : cs) o.setUV[s].push_back(d.setUV[s][size_t(k)]);
}

// ---- the edit format ----
PolyMesh subset(const PolyMesh& d, const std::vector<char>& keep, std::vector<int>* vmapOut) {
  const int nf = d.faceCount(), n = int(d.pos.size()); std::vector<int> vmap(size_t(n), -1); int nv = 0;
  PolyMesh o; o.groupNames = d.groupNames; o.matNames = d.matNames; o.matMaps = d.matMaps; o.matColor = d.matColor; o.matAlpha = d.matAlpha; o.mtllib = d.mtllib; o.flipY = d.flipY;
  const bool uv = d.cornerUV.size() == d.faceVerts.size() && !d.cornerUV.empty(), fm = int(d.faceMat.size()) == nf, fg = int(d.faceGroup.size()) == nf;
  const bool sets = hasUvSets(d);
  const bool nrm = d.cornerNormal.size() == d.faceVerts.size() && !d.cornerNormal.empty();
  for (int f = 0; f < nf; f++) if (keep[size_t(f)]) for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) if (vmap[size_t(d.faceVerts[size_t(k)])] < 0) vmap[size_t(d.faceVerts[size_t(k)])] = nv++;
  o.pos.resize(static_cast<size_t>(nv)); for (int i = 0; i < n; i++) if (vmap[size_t(i)] >= 0) o.pos[size_t(vmap[size_t(i)])] = d.pos[size_t(i)];
  if (d.vcol.size() == d.pos.size() && !d.vcol.empty()) { o.vcol.resize(size_t(nv)); for (int i = 0; i < n; i++) if (vmap[size_t(i)] >= 0) o.vcol[size_t(vmap[size_t(i)])] = d.vcol[size_t(i)]; }   // (the polypaint kept)
  for (int f = 0; f < nf; f++) if (keep[size_t(f)]) {
    for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) { o.faceVerts.push_back(vmap[size_t(d.faceVerts[size_t(k)])]); if (uv) o.cornerUV.push_back(d.cornerUV[size_t(k)]); if (nrm) o.cornerNormal.push_back(d.cornerNormal[size_t(k)]); }
    o.faceStart.push_back(int(o.faceVerts.size())); o.faceGroup.push_back(fg ? d.faceGroup[size_t(f)] : 0); if (fm) o.faceMat.push_back(d.faceMat[size_t(f)]);
    if (sets) { std::vector<int> cs; for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) cs.push_back(k); addFaceSets(o, d, f, cs); }
  }
  // (the hard edges and creases whose two ends stay)
  for (auto& e : d.hardEdges) if (size_t(e[0]) < vmap.size() && size_t(e[1]) < vmap.size() && vmap[size_t(e[0])] >= 0 && vmap[size_t(e[1])] >= 0) o.hardEdges.push_back({vmap[size_t(e[0])], vmap[size_t(e[1])]});
  for (auto& c : d.creases) if (size_t(c.a) < vmap.size() && size_t(c.b) < vmap.size() && vmap[size_t(c.a)] >= 0 && vmap[size_t(c.b)] >= 0) o.creases.push_back({vmap[size_t(c.a)], vmap[size_t(c.b)], c.w});
  for (auto& e : d.seams) if (size_t(e[0]) < vmap.size() && size_t(e[1]) < vmap.size() && vmap[size_t(e[0])] >= 0 && vmap[size_t(e[1])] >= 0) o.seams.push_back({vmap[size_t(e[0])], vmap[size_t(e[1])]});
  if (vmapOut) *vmapOut = std::move(vmap);
  return o;
}
void dropGroup(PolyMesh& d, int gi) {
  for (int& g : d.faceGroup) if (g > gi) g--;
  if (gi >= 0 && size_t(gi) < d.groupNames.size()) d.groupNames.erase(d.groupNames.begin() + gi);
}
namespace {
QByteArray header(uint32_t magic, const QJsonObject& h) {
  QByteArray js = QJsonDocument(h).toJson(QJsonDocument::Compact); int hl = (int(js.size()) + 3) & ~3; js.append(QByteArray(hl - js.size(), ' '));
  QByteArray out(8, 0); uint32_t m = magic, l = uint32_t(hl); std::memcpy(out.data(), &m, 4); std::memcpy(out.data() + 4, &l, 4); out += js; return out;
}
template <class T> void put(QByteArray& b, const T& v) { b.append(reinterpret_cast<const char*>(&v), sizeof v); }
}  // namespace
QByteArray packPMesh(const PolyMesh& d) {
  const int nf = d.faceCount(); const bool uv = !d.cornerUV.empty() && d.cornerUV.size() == d.faceVerts.size();
  QJsonArray gs; for (auto& g : d.groupNames) gs.append(g); QJsonArray ms; for (auto& m : d.matNames) ms.append(m);
  // (the UVs: one per corner, each corner its own)
  const bool nrm = !d.cornerNormal.empty() && d.cornerNormal.size() == d.faceVerts.size();
  QJsonObject hd{{"n", int(d.pos.size())}, {"f", nf}, {"c", int(d.faceVerts.size())}, {"groups", gs}, {"mats", ms}, {"nt", uv ? int(d.cornerUV.size()) : 0},
                                                {"nn", nrm ? int(d.cornerNormal.size()) : 0}, {"nh", int(d.hardEdges.size())}, {"nc", int(d.creases.size())}, {"ns", int(d.seams.size())}};
  const bool sets = uv && hasUvSets(d); if (sets) { QJsonArray us; for (auto& s : d.uvSetNames) us.append(s); hd.insert("uvsets", us); }   // (the UV Sets: after the seams)
  const bool vc = !d.vcol.empty() && d.vcol.size() == d.pos.size(); if (vc) hd.insert("nvc", int(d.vcol.size()));
  QByteArray b = header(0x48534d50, hd);
  b.reserve(b.size() + int(d.pos.size() * 12 + d.faceVerts.size() * 8 + size_t(nf) * 12 + 4 + (uv ? d.cornerUV.size() * 8 : 0)));
  for (auto& p : d.pos) { put(b, float(p.x)); put(b, float(p.y)); put(b, float(p.z)); }
  for (int x : d.faceStart) put(b, int32_t(x));
  for (int x : d.faceVerts) put(b, int32_t(x));
  for (int f = 0; f < nf; f++) put(b, int32_t(size_t(f) < d.faceGroup.size() ? d.faceGroup[size_t(f)] : 0));
  for (int f = 0; f < nf; f++) put(b, int32_t(size_t(f) < d.faceMat.size() ? d.faceMat[size_t(f)] : -1));
  for (size_t k = 0; k < d.faceVerts.size(); k++) put(b, int32_t(uv ? int(k) : -1));
  if (uv) for (auto& t : d.cornerUV) { put(b, t[0]); put(b, t[1]); }
  // (this version's own: the normals per corner, the hard edges, the creases — after what the HTML version reads)
  if (nrm) for (auto& n : d.cornerNormal) { put(b, n[0]); put(b, n[1]); put(b, n[2]); }
  for (auto& e : d.hardEdges) { put(b, int32_t(e[0])); put(b, int32_t(e[1])); }
  for (auto& c : d.creases) { put(b, int32_t(c.a)); put(b, int32_t(c.b)); put(b, c.w); }
  for (auto& e : d.seams) { put(b, int32_t(e[0])); put(b, int32_t(e[1])); }
  if (sets) { for (uint32_t m : d.faceSets) put(b, m); for (int8_t l : d.faceLive) put(b, int32_t(l)); for (auto& L : d.setUV) for (auto& t : L) { put(b, t[0]); put(b, t[1]); } }
  if (vc) for (auto& c : d.vcol) { put(b, c[0]); put(b, c[1]); put(b, c[2]); }   // (polypaint: after the UV Sets)
  return b;
}
QByteArray packHMesh(const Dec& d) {
  QJsonArray gs; for (auto& g : d.groups) gs.append(g);
  QByteArray b = header(0x48534d48, QJsonObject{{"n", int(d.pos.size())}, {"t", int(d.idx.size() / 3)}, {"groups", gs}});
  for (auto& p : d.pos) { put(b, float(p.x)); put(b, float(p.y)); put(b, float(p.z)); }
  for (uint32_t x : d.idx) put(b, x);
  for (uint16_t g : d.tg) put(b, g);
  while (b.size() % 4) b.append('\0');
  return b;
}
PolyMesh decSubset(const Dec& dd, int g, const QString& name) {
  const size_t nt = dd.idx.size() / 3; std::vector<int> vmap(dd.pos.size(), -1); int nv = 0;
  PolyMesh o;
  for (size_t t = 0; t < nt; t++) if (g < 0 || dd.tg[t] == g) for (int j = 0; j < 3; j++) { uint32_t v = dd.idx[t * 3 + size_t(j)]; if (vmap[v] < 0) vmap[v] = nv++; }
  o.pos.resize(static_cast<size_t>(nv)); for (size_t i = 0; i < dd.pos.size(); i++) if (vmap[i] >= 0) o.pos[size_t(vmap[i])] = dd.pos[i];
  for (size_t t = 0; t < nt; t++) if (g < 0 || dd.tg[t] == g) { int f[3]; for (int j = 0; j < 3; j++) f[j] = vmap[dd.idx[t * 3 + size_t(j)]]; o.addFace(f, 3); o.faceGroup.push_back(g < 0 ? dd.tg[t] : 0); }
  o.groupNames = g < 0 ? dd.groups : std::vector<QString>{name};
  return o;
}
PolyMesh decMesh(const Dec& d) { PolyMesh m = decSubset(d, -1, {}); if (m.groupNames.empty()) m.groupNames = {"Polygroup 1"}; return m; }

// ---- pieces ----
Pieces meshPieces(const PolyMesh& o) {
  const int nv = int(o.pos.size()), nf = o.faceCount(); std::vector<int> up(static_cast<size_t>(nv)); for (int i = 0; i < nv; i++) up[size_t(i)] = i;
  auto find = [&](int i) { while (up[size_t(i)] != i) { up[size_t(i)] = up[size_t(up[size_t(i)])]; i = up[size_t(i)]; } return i; };
  for (int f = 0; f < nf; f++) { int s = o.faceStart[size_t(f)]; for (int k = s + 1; k < o.faceStart[size_t(f) + 1]; k++) { int q = find(o.faceVerts[size_t(k)]), r = find(o.faceVerts[size_t(s)]); if (q != r) up[size_t(q)] = r; } }
  Pieces P; std::vector<int> pid(size_t(nv), -1); P.pv.assign(size_t(nv), -1);
  for (int f = 0; f < nf; f++) { int r = find(o.faceVerts[size_t(o.faceStart[size_t(f)])]); if (pid[size_t(r)] < 0) pid[size_t(r)] = P.np++; }
  P.pf.resize(static_cast<size_t>(nf)); std::vector<int> cnt(size_t(P.np) + 1, 0);
  for (int f = 0; f < nf; f++) { P.pf[size_t(f)] = pid[size_t(find(o.faceVerts[size_t(o.faceStart[size_t(f)])]))]; cnt[size_t(P.pf[size_t(f)]) + 1]++; }
  P.poff.assign(size_t(P.np) + 1, 0); for (int p = 0; p < P.np; p++) P.poff[size_t(p) + 1] = P.poff[size_t(p)] + cnt[size_t(p) + 1];
  P.plist.resize(static_cast<size_t>(nf)); std::vector<int> fill(P.poff.begin(), P.poff.end() - 1); for (int f = 0; f < nf; f++) P.plist[size_t(fill[size_t(P.pf[size_t(f)])]++)] = f;
  P.vloc.assign(size_t(nv), -1); std::vector<int> vcnt(size_t(P.np), 0); P.vlist.resize(static_cast<size_t>(nv)); int w = 0; P.voff.assign(size_t(P.np) + 1, 0);
  for (int p = 0; p < P.np; p++) {
    P.voff[size_t(p)] = w;
    for (int i = P.poff[size_t(p)]; i < P.poff[size_t(p) + 1]; i++) { int f = P.plist[size_t(i)]; for (int k = o.faceStart[size_t(f)]; k < o.faceStart[size_t(f) + 1]; k++) { int v = o.faceVerts[size_t(k)]; if (P.vloc[size_t(v)] < 0) { P.vloc[size_t(v)] = vcnt[size_t(p)]++; P.pv[size_t(v)] = p; P.vlist[size_t(w++)] = v; } } }
  }
  P.voff[size_t(P.np)] = w; P.vlist.resize(static_cast<size_t>(w));
  return P;
}

namespace {
struct PieceHE { int p = 0, nV = 0, nF = 0, H = 0; std::vector<int> hv, hf, hn, hp, ht, off, out, gr; std::vector<uint8_t> lk; bool nm = false; const int* verts = nullptr; };
PieceHE pieceHE(const PolyMesh& o, const Pieces& P, int p, const std::vector<uint8_t>* lockG) {
  PieceHE E; E.p = p; const int f0 = P.poff[size_t(p)], f1 = P.poff[size_t(p) + 1]; E.nF = f1 - f0; E.nV = P.voff[size_t(p) + 1] - P.voff[size_t(p)];
  for (int i = f0; i < f1; i++) { int f = P.plist[size_t(i)]; E.H += o.faceStart[size_t(f) + 1] - o.faceStart[size_t(f)]; }
  E.hv.resize(size_t(E.H)); E.hf.resize(size_t(E.H)); E.hn.resize(size_t(E.H)); E.hp.resize(size_t(E.H)); E.ht.assign(size_t(E.H), -1); E.gr.resize(size_t(E.nF));
  int h = 0;
  for (int i = 0; i < E.nF; i++) {
    int f = P.plist[size_t(f0 + i)], s = o.faceStart[size_t(f)], n = o.faceStart[size_t(f) + 1] - s; E.gr[size_t(i)] = size_t(f) < o.faceGroup.size() ? o.faceGroup[size_t(f)] : 0;
    for (int k = 0; k < n; k++) { E.hv[size_t(h + k)] = P.vloc[size_t(o.faceVerts[size_t(s + k)])]; E.hf[size_t(h + k)] = i; E.hn[size_t(h + k)] = h + (k + 1) % n; E.hp[size_t(h + k)] = h + (k + n - 1) % n; }
    h += n;
  }
  E.off.assign(size_t(E.nV) + 1, 0); for (int i = 0; i < E.H; i++) E.off[size_t(E.hv[size_t(i)]) + 1]++;
  for (int v = 0; v < E.nV; v++) E.off[size_t(v) + 1] += E.off[size_t(v)];
  E.out.resize(size_t(E.H)); std::vector<int> fl(E.off.begin(), E.off.end() - 1); for (int i = 0; i < E.H; i++) E.out[size_t(fl[size_t(E.hv[size_t(i)])]++)] = i;
  for (int i = 0; i < E.H; i++) {
    int a = E.hv[size_t(i)], b = E.hv[size_t(E.hn[size_t(i)])], t = -1, c = 0;
    for (int j = E.off[size_t(b)]; j < E.off[size_t(b) + 1]; j++) { int g = E.out[size_t(j)]; if (E.hv[size_t(E.hn[size_t(g)])] == a) { t = g; c++; } }
    if (c > 1) E.nm = true; E.ht[size_t(i)] = t;
  }
  E.lk.assign(size_t(E.nV), 0); const int V0 = P.voff[size_t(p)];
  for (int v = 0; v < E.nV; v++) E.lk[size_t(v)] = lockG ? (*lockG)[size_t(P.vlist[size_t(V0 + v)])] : 0;
  E.verts = P.vlist.data() + V0;
  return E;
}
std::vector<int32_t> heColors(const PieceHE& E) {
  std::vector<int32_t> c(size_t(E.nV));
  for (int v = 0; v < E.nV; v++) { int rim = 0; for (int j = E.off[size_t(v)]; j < E.off[size_t(v) + 1]; j++) if (E.ht[size_t(E.out[size_t(j)])] < 0 || E.ht[size_t(E.hp[size_t(E.out[size_t(j)])])] < 0) rim = 1;
    c[size_t(v)] = int32_t(uint32_t(E.off[size_t(v) + 1] - E.off[size_t(v)] + 1) * 0x9e3779b1u) ^ (rim * 0x51ed27); }
  for (int r = 0; r < 2; r++) {
    std::vector<int32_t> d(size_t(E.nV));
    for (int v = 0; v < E.nV; v++) { uint32_t s = 0; for (int j = E.off[size_t(v)]; j < E.off[size_t(v) + 1]; j++) { int g = E.out[size_t(j)]; s = s + uint32_t(c[size_t(E.hv[size_t(E.hn[size_t(g)])])]) + uint32_t(c[size_t(E.hv[size_t(E.hp[size_t(g)])])]); }
      uint32_t x = (uint32_t(c[size_t(v)]) ^ (s >> 7)) * 0x85ebca6bu ^ s; x ^= x >> 13; d[size_t(v)] = int32_t(x * 0xc2b2ae35u); }
    c = d;
  }
  return c;
}
struct IsoMap { std::vector<int> toB, mapF; bool rev = false; std::array<double, 9> M{}; std::array<double, 3> t{}; double err = 1e300; };
std::optional<IsoMap> heIso(const PieceHE& A, const PieceHE& B, int h0, int g0, bool rev) {
  std::vector<int> mapH(size_t(A.H), -1); IsoMap r; r.toB.assign(size_t(A.nV), -1); r.mapF.assign(size_t(A.nF), -1); std::vector<uint8_t> usedB(size_t(B.nV), 0); r.rev = rev;
  std::vector<int> stack{h0, g0};
  while (!stack.empty()) {
    int g = stack.back(); stack.pop_back(); int h = stack.back(); stack.pop_back();
    if (mapH[size_t(h)] >= 0) { if (mapH[size_t(h)] != g) return std::nullopt; continue; }
    mapH[size_t(h)] = g;
    int va = A.hv[size_t(h)], vb = rev ? B.hv[size_t(B.hn[size_t(g)])] : B.hv[size_t(g)];
    if (r.toB[size_t(va)] < 0) { if (usedB[size_t(vb)]) return std::nullopt; r.toB[size_t(va)] = vb; usedB[size_t(vb)] = 1; } else if (r.toB[size_t(va)] != vb) return std::nullopt;
    int fa = A.hf[size_t(h)], fb = B.hf[size_t(g)];
    if (r.mapF[size_t(fa)] < 0) r.mapF[size_t(fa)] = fb; else if (r.mapF[size_t(fa)] != fb) return std::nullopt;
    stack.push_back(A.hn[size_t(h)]); stack.push_back(rev ? B.hp[size_t(g)] : B.hn[size_t(g)]);
    int ta = A.ht[size_t(h)], tb = B.ht[size_t(g)];
    if ((ta < 0) != (tb < 0)) return std::nullopt;
    if (ta >= 0) { stack.push_back(ta); stack.push_back(tb); }
  }
  for (int v = 0; v < A.nV; v++) if (r.toB[size_t(v)] < 0) return std::nullopt;
  return r;
}
bool inv3(const double a[9], double o[9]) {
  double d = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
  if (std::abs(d) < 1e-300) return false;
  o[0] = (a[4] * a[8] - a[5] * a[7]) / d; o[1] = (a[2] * a[7] - a[1] * a[8]) / d; o[2] = (a[1] * a[5] - a[2] * a[4]) / d;
  o[3] = (a[5] * a[6] - a[3] * a[8]) / d; o[4] = (a[0] * a[8] - a[2] * a[6]) / d; o[5] = (a[2] * a[3] - a[0] * a[5]) / d;
  o[6] = (a[3] * a[7] - a[4] * a[6]) / d; o[7] = (a[1] * a[6] - a[0] * a[7]) / d; o[8] = (a[0] * a[4] - a[1] * a[3]) / d;
  return true;
}
double det3(const std::array<double, 9>& a) { return a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]); }
// the affine map carrying A's points onto B's (least squares on up to 20 000 pairs), its rms error relative to the size
void isoAffine(const PolyMesh& o, const PieceHE& A, const PieceHE& B, IsoMap& r) {
  const int n = A.nV, step = std::max(1, n / 20000); double ca[3] = {0, 0, 0}, cb[3] = {0, 0, 0}; int k = 0;
  auto pa = [&](int v) { return o.pos[size_t(A.verts[v])]; }; auto pb = [&](int v) { return o.pos[size_t(B.verts[r.toB[size_t(v)]])]; };
  for (int v = 0; v < n; v += step) { Vec3 a = pa(v), b = pb(v); for (int i = 0; i < 3; i++) { ca[i] += a[i]; cb[i] += b[i]; } k++; }
  for (int i = 0; i < 3; i++) { ca[i] /= k; cb[i] /= k; }
  double Sxx[9] = {}, Syx[9] = {}, ext = 0;
  for (int v = 0; v < n; v += step) {
    Vec3 a = pa(v), b = pb(v); double x[3] = {a.x - ca[0], a.y - ca[1], a.z - ca[2]}, y[3] = {b.x - cb[0], b.y - cb[1], b.z - cb[2]};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { Sxx[i * 3 + j] += x[i] * x[j]; Syx[i * 3 + j] += y[i] * x[j]; }
    ext = std::max({ext, std::abs(x[0]), std::abs(x[1]), std::abs(x[2])});
  }
  double tr = Sxx[0] + Sxx[4] + Sxx[8]; if (!tr) tr = 1; for (int i = 0; i < 3; i++) Sxx[i * 4] += tr * 1e-9;
  double I[9]; if (!inv3(Sxx, I)) { r.err = 1e300; return; }
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { double s = 0; for (int q = 0; q < 3; q++) s += Syx[i * 3 + q] * I[q * 3 + j]; r.M[size_t(i * 3 + j)] = s; }
  for (int i = 0; i < 3; i++) r.t[size_t(i)] = cb[i] - (r.M[size_t(i * 3)] * ca[0] + r.M[size_t(i * 3 + 1)] * ca[1] + r.M[size_t(i * 3 + 2)] * ca[2]);
  double e2 = 0;
  for (int v = 0; v < n; v += step) { Vec3 a = pa(v), b = pb(v);
    for (int i = 0; i < 3; i++) { double p = r.M[size_t(i * 3)] * a.x + r.M[size_t(i * 3 + 1)] * a.y + r.M[size_t(i * 3 + 2)] * a.z + r.t[size_t(i)] - b[i]; e2 += p * p; } }
  r.err = std::sqrt(e2 / k) / (ext ? ext : 1);
}
std::optional<IsoMap> pieceIso(const PolyMesh& o, const PieceHE& A, const PieceHE& B, const std::vector<int32_t>& cA) {
  if (A.nV != B.nV || A.H != B.H || A.nF != B.nF || A.nm || B.nm) return std::nullopt;
  std::optional<IsoMap> best; int tries = 0;
  auto take = [&](std::optional<IsoMap> r) { if (!r) return false; isoAffine(o, A, B, *r); if (!best || r->err < best->err) best = r; return r->err < 1e-3; };
  if (take(heIso(A, B, 0, 0, false)) || take(heIso(A, B, 0, B.hp[0], true))) return best;
  auto cB = heColors(B); std::map<int32_t, int> cnt, cntB;
  for (int v = 0; v < A.nV; v++) cnt[cA[size_t(v)]]++;
  for (int v = 0; v < B.nV; v++) cntB[cB[size_t(v)]]++;
  for (auto& [k, n] : cnt) { auto f = cntB.find(k); if (f == cntB.end() || f->second != n) return best; }
  int32_t rare = 0; int rn = INT32_MAX; for (auto& [k, n] : cnt) if (n < rn) { rn = n; rare = k; }
  int va = int(std::find(cA.begin(), cA.end(), rare) - cA.begin()), h0 = A.out[size_t(A.off[size_t(va)])];
  auto len = [&](const PieceHE& E, int h) { return o.pos[size_t(E.verts[E.hv[size_t(h)]])].distanceTo(o.pos[size_t(E.verts[E.hv[size_t(E.hn[size_t(h)])]])]); };
  auto ring = [&](const PieceHE& E, int v) { double s = 0; for (int j = E.off[size_t(v)]; j < E.off[size_t(v) + 1]; j++) s += len(E, E.out[size_t(j)]); return s; };
  double l0 = len(A, h0), l1 = len(A, A.hn[size_t(h0)]), r0 = ring(A, va);
  struct C { double s; int g; bool rev; }; std::vector<C> cands;
  for (int vb = 0; vb < B.nV; vb++) {
    if (cB[size_t(vb)] != rare) continue;
    double rb = std::abs(ring(B, vb) - r0);
    for (int j = B.off[size_t(vb)]; j < B.off[size_t(vb) + 1]; j++) { int g = B.out[size_t(j)], q = B.hp[size_t(g)];
      cands.push_back({rb + std::abs(len(B, g) - l0) + std::abs(len(B, B.hn[size_t(g)]) - l1), g, false});
      cands.push_back({rb + std::abs(len(B, q) - l0) + std::abs(len(B, B.hp[size_t(q)]) - l1), q, true}); }
  }
  std::stable_sort(cands.begin(), cands.end(), [](const C& a, const C& b) { return a.s < b.s; });
  for (auto& c : cands) { if (++tries > 96) break; if (take(heIso(A, B, h0, c.g, c.rev))) return best; }
  return best;
}
}  // namespace

std::shared_ptr<const IsoClasses> isoClasses(const std::shared_ptr<PolyMesh>& B) {
  if (auto c = ISO.get(B)) return c;
  auto I = std::make_shared<IsoClasses>(); const PolyMesh& o = *B;
  I->P = meshPieces(o); const Pieces& P = I->P;
  I->cls.assign(size_t(P.np), -1); I->M.assign(size_t(P.np), {1, 0, 0, 0, 1, 0, 0, 0, 1}); I->T.assign(size_t(P.np), {0, 0, 0}); I->mapF.assign(size_t(P.np), {});
  std::map<std::pair<int, int>, std::vector<int>> buckets;
  for (int p = 0; p < P.np; p++) buckets[{P.poff[size_t(p) + 1] - P.poff[size_t(p)], P.voff[size_t(p) + 1] - P.voff[size_t(p)]}].push_back(p);
  for (auto& [k, list] : buckets) {
    struct Rep { PieceHE E; std::vector<int32_t> col; };
    std::vector<Rep> reps;
    for (int p : list) {
      bool hit = false;
      if (list.size() > 1) {
        PieceHE E = pieceHE(o, P, p, nullptr);
        for (auto& R : reps) { if (R.col.empty()) R.col = heColors(R.E); auto r = pieceIso(o, R.E, E, R.col);
          if (r && r->err < 0.05) { I->cls[size_t(p)] = R.E.p; I->M[size_t(p)] = r->M; I->T[size_t(p)] = r->t; I->mapF[size_t(p)] = r->mapF; hit = true; break; } }
        if (!hit) { I->cls[size_t(p)] = p; reps.push_back({std::move(E), {}}); }
      } else I->cls[size_t(p)] = p;
    }
  }
  ISO.put(B, I);
  return I;
}
std::vector<int> similarFaces(const std::shared_ptr<PolyMesh>& B, int gi) {
  auto Ip = isoClasses(B); const IsoClasses& I = *Ip; const Pieces& P = I.P; std::vector<int> out;
  std::map<int, std::vector<int>> inv; auto invOf = [&](int q) -> const std::vector<int>& { auto f = inv.find(q); if (f != inv.end()) return f->second;
    const auto& m = I.mapF[size_t(q)]; std::vector<int> a(m.size()); for (size_t i = 0; i < m.size(); i++) a[size_t(m[i])] = int(i); return inv[q] = a; };
  std::map<int, std::vector<int>> byPiece;
  for (size_t f = 0; f < B->faceGroup.size(); f++) if (B->faceGroup[f] == gi) byPiece[P.pf[f]].push_back(int(f));
  for (auto& [p, fs] : byPiece) {
    int c = I.cls[size_t(p)]; std::vector<int> mates; for (int q = 0; q < P.np; q++) if (q != p && I.cls[size_t(q)] == c) mates.push_back(q);
    if (mates.empty()) continue;
    std::unordered_map<int, int> pos; for (int i = P.poff[size_t(p)]; i < P.poff[size_t(p) + 1]; i++) pos[P.plist[size_t(i)]] = i - P.poff[size_t(p)];
    for (int f : fs) {
      int li = pos[f], ri = c == p ? li : invOf(p)[size_t(li)];
      for (int q : mates) { int k = q == c ? ri : (size_t(ri) < I.mapF[size_t(q)].size() ? I.mapF[size_t(q)][size_t(ri)] : -1); if (k >= 0) out.push_back(P.plist[size_t(P.poff[size_t(q)] + k)]); }
    }
  }
  return out;
}
static std::vector<int> pieceFaces(const Pieces& P, const std::vector<int>& ps) { std::vector<int> o; for (int p : ps) for (int i = P.poff[size_t(p)]; i < P.poff[size_t(p) + 1]; i++) o.push_back(P.plist[size_t(i)]); return o; }
std::vector<int> identicalFaces(const std::shared_ptr<PolyMesh>& B, int gi, int* parts, int* own) {
  auto I = isoClasses(B); std::set<int> mine; for (int f : groupFaces(*B, gi)) mine.insert(I->P.pf[size_t(f)]);
  std::set<int> cl; for (int p : mine) cl.insert(I->cls[size_t(p)]);
  std::vector<int> ps; for (int p = 0; p < I->P.np; p++) if (cl.count(I->cls[size_t(p)])) ps.push_back(p);
  if (parts) *parts = int(ps.size()); if (own) *own = int(mine.size());
  return pieceFaces(I->P, ps);
}
std::vector<int> symmetricFaces(const std::shared_ptr<PolyMesh>& B, int gi, QString* msg) {
  auto I = isoClasses(B); auto gf = groupFaces(*B, gi); std::set<int> mine; for (int f : gf) mine.insert(I->P.pf[size_t(f)]);
  std::set<int> out;
  for (int p : mine) {
    double Mi[9]; double Mp0[9]; for (int i = 0; i < 9; i++) Mp0[i] = I->M[size_t(p)][size_t(i)];
    if (!inv3(Mp0, Mi)) continue;
    for (int q = 0; q < I->P.np; q++) {
      if (q == p || I->cls[size_t(q)] != I->cls[size_t(p)]) continue;
      std::array<double, 9> R{}; const auto& Mq = I->M[size_t(q)];
      for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { double s = 0; for (int k = 0; k < 3; k++) s += Mq[size_t(i * 3 + k)] * Mi[k * 3 + j]; R[size_t(i * 3 + j)] = s; }
      if (det3(R) < 0) out.insert(q);
    }
  }
  if (!out.empty()) { if (msg) *msg = QStringLiteral("Symmetric: %1 mirrored part%2").arg(out.size()).arg(out.size() == 1 ? "" : "s"); return pieceFaces(I->P, {out.begin(), out.end()}); }
  // (no mirrored copy: by position, across the mesh's middle in X)
  auto T = ttopo(B); const auto& P = B->pos; Box3 box = B->bounds(); double cx = (box.min.x + box.max.x) / 2, L = 0; int ln = 0;
  for (int e = 0; e < std::min(T->ne, 4000); e++) { L += P[size_t(T->ea[size_t(e)])].distanceTo(P[size_t(T->eb[size_t(e)])]); ln++; }
  double tol = L / std::max(ln, 1) * 0.35, q = tol > 0 ? tol : 1e-6;
  auto key = [&](double x, double y, double z) { return (int64_t(std::llround(x / q)) * 73856093) ^ (int64_t(std::llround(y / q)) * 19349663) ^ (int64_t(std::llround(z / q)) * 83492791); };
  std::unordered_map<int64_t, std::vector<int>> grid; std::vector<Vec3> cen(size_t(T->nf));
  for (int f = 0; f < T->nf; f++) { cen[size_t(f)] = faceCenter(*B, f); grid[key(cen[size_t(f)].x, cen[size_t(f)].y, cen[size_t(f)].z)].push_back(f); }
  std::set<int> gs(gf.begin(), gf.end()); std::set<int> res;
  for (int f : gf) {
    Vec3 p = cen[size_t(f)]; double mx = 2 * cx - p.x; int64_t i0 = std::llround(mx / q), j0 = std::llround(p.y / q), k0 = std::llround(p.z / q); int best = -1; double bd = tol;
    for (int64_t i = i0 - 1; i <= i0 + 1; i++) for (int64_t j = j0 - 1; j <= j0 + 1; j++) for (int64_t k = k0 - 1; k <= k0 + 1; k++) {
      auto g = grid.find((i * 73856093) ^ (j * 19349663) ^ (k * 83492791)); if (g == grid.end()) continue;
      for (int h : g->second) { double d = std::hypot(cen[size_t(h)].x - mx, cen[size_t(h)].y - p.y, cen[size_t(h)].z - p.z); if (d < bd) { bd = d; best = h; } } }
    if (best >= 0 && !gs.count(best)) res.insert(best);
  }
  if (res.empty()) { if (msg) *msg = QStringLiteral("!No symmetric counterpart found (no mirrored part, nothing mirrored across X)"); return {}; }
  if (msg) *msg = QStringLiteral("Symmetric across X: %1 polygon%2").arg(res.size()).arg(res.size() == 1 ? "" : "s");
  return {res.begin(), res.end()};
}
std::vector<int> groupBorder(const TTopo& T, const PolyMesh& B, int gi) {
  std::vector<int> es;
  for (int e = 0; e < T.ne; e++) { bool a = B.faceGroup[size_t(T.ef0[size_t(e)])] == gi, b = T.ef1[size_t(e)] >= 0 && B.faceGroup[size_t(T.ef1[size_t(e)])] == gi; if (a != b) es.push_back(e); }
  return es;
}

// ---- snap mask ----
static QString b36(int v) { return QString::number(v, 36); }
QString smEnc(const std::vector<char>& a) {
  QStringList out; int v = 0, run = 0;
  for (char x0 : a) { int x = x0 ? 1 : 0; if (x == v) run++; else { out << b36(run); v = x; run = 1; } }
  out << b36(run); return out.join('.');
}
std::vector<char> smDec(const QString& s, int n) {
  std::vector<char> a(size_t(std::max(n, 0)), 0); int i = 0, v = 0;
  for (const QString& t : s.split('.')) { bool ok; int r = t.toInt(&ok, 36); if (!ok) r = 0; if (v) for (int k = std::min(n, i); k < std::min(n, i + r); k++) a[size_t(k)] = 1; i += r; v ^= 1; }
  return a;
}
std::shared_ptr<const Partners> smPartners(const std::shared_ptr<PolyMesh>& B) {
  if (auto c = PART.get(B)) return c;
  auto T = ttopo(B); const auto& P = B->pos; const int nf = B->faceCount();
  auto R = std::make_shared<Partners>(); R->cx.resize(static_cast<size_t>(nf)); std::vector<double> cy(static_cast<size_t>(nf)), cz(static_cast<size_t>(nf));
  for (int f = 0; f < nf; f++) { Vec3 c = faceCenter(*B, f); R->cx[size_t(f)] = c.x; cy[size_t(f)] = c.y; cz[size_t(f)] = c.z; }
  double L = 0; int ln = 0; for (int e = 0; e < std::min(T->ne, 20000); e++) { L += P[size_t(T->ea[size_t(e)])].distanceTo(P[size_t(T->eb[size_t(e)])]); ln++; }
  double q = L / std::max(ln, 1) * 0.5; if (!(q > 0)) q = 1e-6; R->plane = q * 0.25;
  auto key = [](int64_t i, int64_t j, int64_t k) { return (i * 73856093) ^ (j * 19349663) ^ (k * 83492791); };
  std::unordered_map<int64_t, std::vector<int>> grid; R->part.assign(size_t(nf), -1);
  for (int f = 0; f < nf; f++) grid[key(std::llround(R->cx[size_t(f)] / q), std::llround(cy[size_t(f)] / q), std::llround(cz[size_t(f)] / q))].push_back(f);
  for (int f = 0; f < nf; f++) {
    if (std::abs(R->cx[size_t(f)]) <= R->plane) { R->part[size_t(f)] = f; continue; }
    double mx = -R->cx[size_t(f)]; int64_t i0 = std::llround(mx / q), j0 = std::llround(cy[size_t(f)] / q), k0 = std::llround(cz[size_t(f)] / q); int best = -1; double bd = q;
    for (int64_t i = i0 - 1; i <= i0 + 1; i++) for (int64_t j = j0 - 1; j <= j0 + 1; j++) for (int64_t k = k0 - 1; k <= k0 + 1; k++) {
      auto g = grid.find(key(i, j, k)); if (g == grid.end()) continue;
      for (int h : g->second) { if (R->cx[size_t(h)] * R->cx[size_t(f)] >= 0) continue; double d = std::hypot(R->cx[size_t(h)] - mx, cy[size_t(h)] - cy[size_t(f)], cz[size_t(h)] - cz[size_t(f)]); if (d < bd) { bd = d; best = h; } } }
    R->part[size_t(f)] = best;
  }
  PART.put(B, R);
  return R;
}
std::vector<int> symPairs(const PolyMesh& B, const std::vector<Vec3>& base) {
  // (the nearest mirrored vertex within the tolerance — the neighbouring cells looked in too: two twins a hair apart
  // used to fall into different cells by rounding, and that vertex then had no twin)
  const int n = int(base.size()); double size = B.bounds().size().length(); double q = (size > 0 ? size : 1) * 5e-4;
  std::unordered_map<int64_t, std::vector<int>> grid; std::vector<int> pair(size_t(n), -1);
  auto key = [](int64_t i, int64_t j, int64_t k) { return (i * 73856093) ^ (j * 19349663) ^ (k * 83492791); };
  auto cell = [&](double x) { return int64_t(std::floor(x / q)); };
  for (int v = 0; v < n; v++) grid[key(cell(base[size_t(v)].x), cell(base[size_t(v)].y), cell(base[size_t(v)].z))].push_back(v);
  for (int v = 0; v < n; v++) {
    const Vec3 m(-base[size_t(v)].x, base[size_t(v)].y, base[size_t(v)].z); const int64_t i0 = cell(m.x), j0 = cell(m.y), k0 = cell(m.z);
    double bd = q; int best = -1;
    for (int64_t i = i0 - 1; i <= i0 + 1; i++) for (int64_t j = j0 - 1; j <= j0 + 1; j++) for (int64_t k = k0 - 1; k <= k0 + 1; k++) {
      auto g = grid.find(key(i, j, k)); if (g == grid.end()) continue;
      for (int h : g->second) { const double d = base[size_t(h)].distanceTo(m); if (d < bd) { bd = d; best = h; } } }
    pair[size_t(v)] = best;
  }
  return pair;
}

// ---- decimation: quadric-error edge collapses (the "fast quadric mesh simplification" scheme, S. Forstmann, MIT) ----
QemResult qemSimplify(const std::vector<float>& posIn, const std::vector<uint32_t>& idxIn, int target, double aggr, const std::vector<uint8_t>* lock) {
  int T = int(idxIn.size() / 3); const int V = int(posIn.size() / 3);
  std::vector<int> tv(idxIn.begin(), idxIn.end()); std::vector<float> terr(size_t(T) * 4), tn(size_t(T) * 3); std::vector<uint8_t> tdel(size_t(T), 0), tdirty(size_t(T), 0);
  std::vector<double> vp(posIn.begin(), posIn.end()), vq(size_t(V) * 10, 0); std::vector<int> vts(static_cast<size_t>(V)), vtc(static_cast<size_t>(V)); std::vector<uint8_t> vb(size_t(V), 0);
  std::vector<int> refT(size_t(T) * 3); std::vector<int8_t> refV(size_t(T) * 3); size_t refN = 0;
  auto pushRef = [&](int t, int s) { if (refN >= refT.size()) { refT.resize(refT.size() * 2 + 16); refV.resize(refV.size() * 2 + 16); } refT[refN] = t; refV[refN++] = int8_t(s); };
  double Q[10], R[3];
  auto det = [&](int a11, int a12, int a13, int a21, int a22, int a23, int a31, int a32, int a33) { return Q[a11] * Q[a22] * Q[a33] + Q[a13] * Q[a21] * Q[a32] + Q[a12] * Q[a23] * Q[a31] - Q[a13] * Q[a22] * Q[a31] - Q[a11] * Q[a23] * Q[a32] - Q[a12] * Q[a21] * Q[a33]; };
  auto vErr = [&](double x, double y, double z) { return Q[0] * x * x + 2 * Q[1] * x * y + 2 * Q[2] * x * z + 2 * Q[3] * x + Q[4] * y * y + 2 * Q[5] * y * z + 2 * Q[6] * y + Q[7] * z * z + 2 * Q[8] * z + Q[9]; };
  auto cErr = [&](int a, int b) {
    for (int k = 0; k < 10; k++) Q[k] = vq[size_t(a) * 10 + size_t(k)] + vq[size_t(b) * 10 + size_t(k)];
    bool border = vb[size_t(a)] & vb[size_t(b)]; double d = det(0, 1, 2, 1, 4, 5, 2, 5, 7);
    if (d != 0 && !border) { R[0] = -1 / d * det(1, 2, 3, 4, 5, 6, 5, 7, 8); R[1] = 1 / d * det(0, 2, 3, 1, 5, 6, 2, 7, 8); R[2] = -1 / d * det(0, 1, 3, 1, 4, 6, 2, 5, 8); return vErr(R[0], R[1], R[2]); }
    double ax = vp[size_t(a) * 3], ay = vp[size_t(a) * 3 + 1], az = vp[size_t(a) * 3 + 2], bx = vp[size_t(b) * 3], by = vp[size_t(b) * 3 + 1], bz = vp[size_t(b) * 3 + 2], mx = (ax + bx) / 2, my = (ay + by) / 2, mz = (az + bz) / 2;
    double e1 = vErr(ax, ay, az), e2 = vErr(bx, by, bz), e3 = vErr(mx, my, mz), e = std::min({e1, e2, e3});
    if (e == e1) { R[0] = ax; R[1] = ay; R[2] = az; } else if (e == e2) { R[0] = bx; R[1] = by; R[2] = bz; } else { R[0] = mx; R[1] = my; R[2] = mz; }
    return e;
  };
  auto triErr = [&](int t) { for (int j = 0; j < 3; j++) terr[size_t(t) * 4 + size_t(j)] = float(cErr(tv[size_t(t) * 3 + size_t(j)], tv[size_t(t) * 3 + size_t((j + 1) % 3)]));
    terr[size_t(t) * 4 + 3] = std::min({terr[size_t(t) * 4], terr[size_t(t) * 4 + 1], terr[size_t(t) * 4 + 2]}); };
  auto updateMesh = [&](int it) {
    if (it > 0) { int d = 0; for (int t = 0; t < T; t++) if (!tdel[size_t(t)]) { if (d != t) { for (int k = 0; k < 3; k++) { tv[size_t(d) * 3 + size_t(k)] = tv[size_t(t) * 3 + size_t(k)]; tn[size_t(d) * 3 + size_t(k)] = tn[size_t(t) * 3 + size_t(k)]; } for (int k = 0; k < 4; k++) terr[size_t(d) * 4 + size_t(k)] = terr[size_t(t) * 4 + size_t(k)]; tdel[size_t(d)] = 0; tdirty[size_t(d)] = tdirty[size_t(t)]; } d++; } T = d; }
    std::fill(vtc.begin(), vtc.end(), 0); for (int t = 0; t < T; t++) for (int j = 0; j < 3; j++) vtc[size_t(tv[size_t(t) * 3 + size_t(j)])]++;
    int s = 0; for (int v = 0; v < V; v++) { vts[size_t(v)] = s; s += vtc[size_t(v)]; vtc[size_t(v)] = 0; }
    refN = size_t(s); if (refT.size() < refN) { refT.resize(refN * 2); refV.resize(refN * 2); }
    for (int t = 0; t < T; t++) for (int j = 0; j < 3; j++) { int v = tv[size_t(t) * 3 + size_t(j)], r = vts[size_t(v)] + vtc[size_t(v)]++; refT[size_t(r)] = t; refV[size_t(r)] = int8_t(j); }
    if (it != 0) return;
    int ids[64], cn[64];
    for (int v = 0; v < V; v++) {
      int n = 0;
      for (int r = vts[size_t(v)]; r < vts[size_t(v)] + vtc[size_t(v)]; r++) { int t = refT[size_t(r)]; for (int k = 0; k < 3; k++) { int id = tv[size_t(t) * 3 + size_t(k)], j = 0; while (j < n && ids[j] != id) j++; if (j == n) { if (n < 64) { ids[n] = id; cn[n++] = 1; } } else cn[j]++; } }
      for (int j = 0; j < n; j++) if (cn[j] == 1) vb[size_t(ids[j])] = 1;
    }
    for (int t = 0; t < T; t++) {
      size_t a = size_t(tv[size_t(t) * 3]) * 3, b = size_t(tv[size_t(t) * 3 + 1]) * 3, c = size_t(tv[size_t(t) * 3 + 2]) * 3;
      double ux = vp[b] - vp[a], uy = vp[b + 1] - vp[a + 1], uz = vp[b + 2] - vp[a + 2], wx = vp[c] - vp[a], wy = vp[c + 1] - vp[a + 1], wz = vp[c + 2] - vp[a + 2];
      double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx, l = std::hypot(nx, ny, nz); if (!l) l = 1; nx /= l; ny /= l; nz /= l;
      tn[size_t(t) * 3] = float(nx); tn[size_t(t) * 3 + 1] = float(ny); tn[size_t(t) * 3 + 2] = float(nz);
      double d = -(nx * vp[a] + ny * vp[a + 1] + nz * vp[a + 2]), m[10] = {nx * nx, nx * ny, nx * nz, nx * d, ny * ny, ny * nz, ny * d, nz * nz, nz * d, d * d};
      for (int j = 0; j < 3; j++) { size_t o = size_t(tv[size_t(t) * 3 + size_t(j)]) * 10; for (int k = 0; k < 10; k++) vq[o + size_t(k)] += m[k]; }
    }
    for (int t = 0; t < T; t++) triErr(t);
  };
  std::vector<uint8_t> del0(64), del1(64);
  auto flipped = [&](double px, double py, double pz, int i0, int i1, std::vector<uint8_t>& del) {
    for (int k = 0; k < vtc[size_t(i0)]; k++) {
      int t = refT[size_t(vts[size_t(i0)] + k)]; if (tdel[size_t(t)]) continue;
      int s = refV[size_t(vts[size_t(i0)] + k)], id1 = tv[size_t(t) * 3 + size_t((s + 1) % 3)], id2 = tv[size_t(t) * 3 + size_t((s + 2) % 3)];
      if (id1 == i1 || id2 == i1) { del[size_t(k)] = 1; continue; }
      double d1x = vp[size_t(id1) * 3] - px, d1y = vp[size_t(id1) * 3 + 1] - py, d1z = vp[size_t(id1) * 3 + 2] - pz, l1 = std::hypot(d1x, d1y, d1z); if (!l1) l1 = 1; d1x /= l1; d1y /= l1; d1z /= l1;
      double d2x = vp[size_t(id2) * 3] - px, d2y = vp[size_t(id2) * 3 + 1] - py, d2z = vp[size_t(id2) * 3 + 2] - pz, l2 = std::hypot(d2x, d2y, d2z); if (!l2) l2 = 1; d2x /= l2; d2y /= l2; d2z /= l2;
      if (std::abs(d1x * d2x + d1y * d2y + d1z * d2z) > 0.999) return true;
      double nx = d1y * d2z - d1z * d2y, ny = d1z * d2x - d1x * d2z, nz = d1x * d2y - d1y * d2x, l = std::hypot(nx, ny, nz); if (!l) l = 1;
      del[size_t(k)] = 0; if ((nx * tn[size_t(t) * 3] + ny * tn[size_t(t) * 3 + 1] + nz * tn[size_t(t) * 3 + 2]) / l < 0.2) return true;
    }
    return false;
  };
  int deleted = 0;
  auto updateTris = [&](int i0, int v, std::vector<uint8_t>& del) {
    for (int k = 0; k < vtc[size_t(v)]; k++) {
      int t = refT[size_t(vts[size_t(v)] + k)], s = refV[size_t(vts[size_t(v)] + k)]; if (tdel[size_t(t)]) continue;
      if (del[size_t(k)]) { tdel[size_t(t)] = 1; deleted++; continue; }
      tv[size_t(t) * 3 + size_t(s)] = i0; tdirty[size_t(t)] = 1; triErr(t); pushRef(t, s);
    }
  };
  for (int it = 0; it < 100; it++) {
    if (T - deleted <= target) break;
    if (it % 5 == 0) { updateMesh(it); deleted = 0; }
    std::fill(tdirty.begin(), tdirty.begin() + T, 0);
    const double thr = 1e-9 * std::pow(it + 3, aggr);
    for (int t = 0; t < T && T - deleted > target; t++) {
      if (terr[size_t(t) * 4 + 3] > thr || tdel[size_t(t)] || tdirty[size_t(t)]) continue;
      for (int j = 0; j < 3; j++) {
        if (terr[size_t(t) * 4 + size_t(j)] >= thr) continue;
        int i0 = tv[size_t(t) * 3 + size_t(j)], i1 = tv[size_t(t) * 3 + size_t((j + 1) % 3)]; if (vb[size_t(i0)] != vb[size_t(i1)] || (lock && ((*lock)[size_t(i0)] || (*lock)[size_t(i1)]))) continue;
        cErr(i0, i1); double px = R[0], py = R[1], pz = R[2];
        if (del0.size() < size_t(vtc[size_t(i0)])) del0.resize(size_t(vtc[size_t(i0)]) * 2); if (del1.size() < size_t(vtc[size_t(i1)])) del1.resize(size_t(vtc[size_t(i1)]) * 2);
        if (flipped(px, py, pz, i0, i1, del0) || flipped(px, py, pz, i1, i0, del1)) continue;
        vp[size_t(i0) * 3] = px; vp[size_t(i0) * 3 + 1] = py; vp[size_t(i0) * 3 + 2] = pz;
        for (int k = 0; k < 10; k++) vq[size_t(i0) * 10 + size_t(k)] += vq[size_t(i1) * 10 + size_t(k)];
        size_t ts = refN; updateTris(i0, i0, del0); updateTris(i0, i1, del1);
        int tc = int(refN - ts);
        if (tc <= vtc[size_t(i0)]) { std::copy(refT.begin() + long(ts), refT.begin() + long(ts) + tc, refT.begin() + vts[size_t(i0)]); std::copy(refV.begin() + long(ts), refV.begin() + long(ts) + tc, refV.begin() + vts[size_t(i0)]); }
        else vts[size_t(i0)] = int(ts);
        vtc[size_t(i0)] = tc;
        break;
      }
    }
  }
  std::vector<int> map(size_t(V), -1); int nv = 0, nt = 0;
  for (int t = 0; t < T; t++) if (!tdel[size_t(t)]) { nt++; for (int j = 0; j < 3; j++) { int v = tv[size_t(t) * 3 + size_t(j)]; if (map[size_t(v)] < 0) map[size_t(v)] = nv++; } }
  QemResult r; r.pos.resize(size_t(nv) * 3); r.idx.reserve(size_t(nt) * 3); r.src.resize(static_cast<size_t>(nv));
  for (int v = 0; v < V; v++) if (map[size_t(v)] >= 0) { for (int k = 0; k < 3; k++) r.pos[size_t(map[size_t(v)]) * 3 + size_t(k)] = float(vp[size_t(v) * 3 + size_t(k)]); r.src[size_t(map[size_t(v)])] = v; }
  for (int t = 0; t < T; t++) if (!tdel[size_t(t)]) for (int j = 0; j < 3; j++) r.idx.push_back(uint32_t(map[size_t(tv[size_t(t) * 3 + size_t(j)])]));
  return r;
}

namespace {
// every piece's original: rep[p] — the piece p is a copy of (−1: an original), iso[p] — how
void findTwinPieces(const PolyMesh& o, const Pieces& P, const std::vector<uint8_t>& lockG, std::vector<int>& rep, std::vector<std::optional<IsoMap>>& iso) {
  rep.assign(size_t(P.np), -1); iso.assign(size_t(P.np), std::nullopt);
  std::map<std::array<int, 3>, std::vector<int>> buckets;
  for (int p = 0; p < P.np; p++) { int nc = 0; for (int i = P.poff[size_t(p)]; i < P.poff[size_t(p) + 1]; i++) { int f = P.plist[size_t(i)]; nc += o.faceStart[size_t(f) + 1] - o.faceStart[size_t(f)]; }
    buckets[{P.poff[size_t(p) + 1] - P.poff[size_t(p)], P.voff[size_t(p) + 1] - P.voff[size_t(p)], nc}].push_back(p); }
  for (auto& [k, list] : buckets) {
    if (list.size() < 2) continue;
    struct Orig { PieceHE E; std::vector<int32_t> col; }; std::vector<Orig> origs;
    for (int p : list) {
      PieceHE E = pieceHE(o, P, p, &lockG); bool hit = false;
      for (auto& R : origs) { if (R.col.empty()) R.col = heColors(R.E); auto r = pieceIso(o, R.E, E, R.col); if (r) { rep[size_t(p)] = R.E.p; iso[size_t(p)] = r; hit = true; break; } }
      if (!hit) origs.push_back({std::move(E), {}});
    }
  }
}
}  // namespace

Dec decimateByGroups(const PolyMesh& o, double ratio, const std::function<void(double)>& progress) {
  const int nv = int(o.pos.size()), nf = o.faceCount();
  std::vector<int> fg = o.faceGroup; if (int(fg.size()) != nf) fg.assign(size_t(nf), 0);
  std::vector<int> vg(size_t(nv), -1); std::vector<uint8_t> lockG(size_t(nv), 0);
  for (int f = 0; f < nf; f++) { int g = fg[size_t(f)]; for (int k = o.faceStart[size_t(f)]; k < o.faceStart[size_t(f) + 1]; k++) { int v = o.faceVerts[size_t(k)]; if (vg[size_t(v)] < 0) vg[size_t(v)] = g; else if (vg[size_t(v)] != g) lockG[size_t(v)] = 1; } }
  Pieces P = meshPieces(o); std::vector<int> rep; std::vector<std::optional<IsoMap>> iso; findTwinPieces(o, P, lockG, rep, iso);
  std::vector<std::vector<int>> copies(size_t(P.np)); int twins = 0;
  for (int p = 0; p < P.np; p++) if (rep[size_t(p)] >= 0) { copies[size_t(rep[size_t(p)])].push_back(p); twins++; }
  struct Job { int p; std::vector<int> gs, fs; int id; };
  std::vector<Job> jobs; std::vector<uint8_t> lockX = lockG; std::vector<int> vkey(size_t(nv), -1);
  for (int p = 0; p < P.np; p++) {
    if (rep[size_t(p)] >= 0) continue;
    const auto& C = copies[size_t(p)]; const int f0 = P.poff[size_t(p)]; std::map<std::vector<int>, size_t> byK; std::vector<Job> local;
    for (int i = f0; i < P.poff[size_t(p) + 1]; i++) {
      int f = P.plist[size_t(i)]; std::vector<int> gs{fg[size_t(f)]};
      for (int q : C) gs.push_back(fg[size_t(P.plist[size_t(P.poff[size_t(q)] + iso[size_t(q)]->mapF[size_t(i - f0)])])]);
      auto bk = byK.find(gs); size_t ji;
      if (bk == byK.end()) { ji = local.size(); local.push_back({p, gs, {}, int(jobs.size() + local.size())}); byK[gs] = ji; } else ji = bk->second;
      Job& J = local[ji]; J.fs.push_back(f);
      if (!C.empty()) for (int k = o.faceStart[size_t(f)]; k < o.faceStart[size_t(f) + 1]; k++) {
        int a = o.faceVerts[size_t(k)]; if (vkey[size_t(a)] < 0) { vkey[size_t(a)] = J.id; continue; }
        if (vkey[size_t(a)] != J.id) { lockX[size_t(a)] = 1; for (int q : C) lockX[size_t(P.vlist[size_t(P.voff[size_t(q)] + iso[size_t(q)]->toB[size_t(P.vloc[size_t(a)])])])] = 1; }
      }
    }
    for (auto& j : local) jobs.push_back(std::move(j));
  }
  Dec out; out.groups = o.groupNames; out.twins = twins; out.pieces = P.np;
  std::vector<int> gIdOf(size_t(nv), -1);
  std::mutex mu; std::atomic<size_t> next{0}; int done = 0;
  std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.fs.size() > b.fs.size(); });
  // one result into the output — for the original piece (C null) or for a copy, g — its group
  auto put1 = [&](const QemResult& r, const std::vector<int>& used, int g, const IsoMap* C, int q) {
    std::vector<uint32_t> map(r.pos.size() / 3);
    for (size_t i = 0; i < map.size(); i++) {
      int a = used[size_t(r.src[i])], v0 = C ? P.vlist[size_t(P.voff[size_t(q)] + C->toB[size_t(P.vloc[size_t(a)])])] : a;
      double x = r.pos[i * 3], y = r.pos[i * 3 + 1], z = r.pos[i * 3 + 2];
      if (C) { const Vec3& A = o.pos[size_t(a)]; double dx = x - A.x, dy = y - A.y, dz = z - A.z; const Vec3& B0 = o.pos[size_t(v0)]; const auto& M = C->M;
        x = B0.x + M[0] * dx + M[1] * dy + M[2] * dz; y = B0.y + M[3] * dx + M[4] * dy + M[5] * dz; z = B0.z + M[6] * dx + M[7] * dy + M[8] * dz; }
      if (lockX[size_t(v0)]) { if (gIdOf[size_t(v0)] < 0) { gIdOf[size_t(v0)] = int(out.pos.size()); out.pos.push_back({double(float(x)), double(float(y)), double(float(z))}); } map[i] = uint32_t(gIdOf[size_t(v0)]); }
      else { map[i] = uint32_t(out.pos.size()); out.pos.push_back({double(float(x)), double(float(y)), double(float(z))}); }
    }
    bool flip = C && C->rev;
    for (size_t t = 0; t + 2 < r.idx.size(); t += 3) {
      if (flip) { out.idx.push_back(map[r.idx[t]]); out.idx.push_back(map[r.idx[t + 2]]); out.idx.push_back(map[r.idx[t + 1]]); }
      else { out.idx.push_back(map[r.idx[t]]); out.idx.push_back(map[r.idx[t + 1]]); out.idx.push_back(map[r.idx[t + 2]]); }
      out.tg.push_back(uint16_t(g));
    }
  };
  auto lane = [&] {
    std::vector<int> local(size_t(nv), -1);
    for (;;) {
      size_t ji = next++; if (ji >= jobs.size()) return;
      const Job& J = jobs[ji]; std::vector<int> used; int nt = 0;
      for (int f : J.fs) { nt += o.faceStart[size_t(f) + 1] - o.faceStart[size_t(f)] - 2; for (int k = o.faceStart[size_t(f)]; k < o.faceStart[size_t(f) + 1]; k++) { int v = o.faceVerts[size_t(k)]; if (local[size_t(v)] < 0) { local[size_t(v)] = int(used.size()); used.push_back(v); } } }
      std::vector<float> pos(used.size() * 3); std::vector<uint8_t> lock(used.size()); std::vector<uint32_t> idx; idx.reserve(size_t(nt) * 3);
      for (size_t i = 0; i < used.size(); i++) { const Vec3& p = o.pos[size_t(used[i])]; pos[i * 3] = float(p.x); pos[i * 3 + 1] = float(p.y); pos[i * 3 + 2] = float(p.z); lock[i] = lockX[size_t(used[i])]; }
      for (int f : J.fs) { int s = o.faceStart[size_t(f)], e = o.faceStart[size_t(f) + 1]; for (int k = s + 1; k < e - 1; k++) { idx.push_back(uint32_t(local[size_t(o.faceVerts[size_t(s)])])); idx.push_back(uint32_t(local[size_t(o.faceVerts[size_t(k)])])); idx.push_back(uint32_t(local[size_t(o.faceVerts[size_t(k + 1)])])); } }
      for (int v : used) local[size_t(v)] = -1;
      int target = std::max(4, int(std::lround(nt * ratio)));
      QemResult r = qemSimplify(pos, idx, target, 7, &lock);
      std::lock_guard<std::mutex> l(mu);
      put1(r, used, J.gs[0], nullptr, -1);
      for (size_t qi = 0; qi < copies[size_t(J.p)].size(); qi++) { int q = copies[size_t(J.p)][qi]; put1(r, used, J.gs[qi + 1], &*iso[size_t(q)], q); }
      done += int(J.fs.size() * (1 + copies[size_t(J.p)].size())); if (progress) progress(double(done) / std::max(nf, 1));
    }
  };
  int NW = std::max(1, std::min(4, int(std::thread::hardware_concurrency()) - 1));
  std::vector<std::thread> ws; for (int i = 1; i < NW; i++) ws.emplace_back(lane);
  lane(); for (auto& w : ws) w.join();
  return out;
}

// ---- BVH ----
namespace {
Vec3 closestOnTri(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {   // (Ericson, Real-Time Collision Detection)
  Vec3 ab = b - a, ac = c - a, ap = p - a; double d1 = ab.dot(ap), d2 = ac.dot(ap); if (d1 <= 0 && d2 <= 0) return a;
  Vec3 bp = p - b; double d3 = ab.dot(bp), d4 = ac.dot(bp); if (d3 >= 0 && d4 <= d3) return b;
  double vc = d1 * d4 - d3 * d2; if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
  Vec3 cp = p - c; double d5 = ab.dot(cp), d6 = ac.dot(cp); if (d6 >= 0 && d5 <= d6) return c;
  double vb = d5 * d2 - d1 * d6; if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
  double va = d3 * d6 - d5 * d4; if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  double den = 1 / (va + vb + vc); return a + ab * (vb * den) + ac * (vc * den);
}
double boxDist(const Box3& b, const Vec3& p) {
  double dx = std::max({b.min.x - p.x, 0.0, p.x - b.max.x}), dy = std::max({b.min.y - p.y, 0.0, p.y - b.max.y}), dz = std::max({b.min.z - p.z, 0.0, p.z - b.max.z});
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}
bool rayBoxT(const Vec3& o, const Vec3& d, const Box3& b, double tmax) {
  double t0 = 0, t1 = tmax;
  for (int k = 0; k < 3; k++) { double oo = o[k], dd = d[k], lo = b.min[k], hi = b.max[k];
    if (std::abs(dd) < 1e-15) { if (oo < lo || oo > hi) return false; continue; }
    double a = (lo - oo) / dd, c = (hi - oo) / dd; if (a > c) std::swap(a, c); t0 = std::max(t0, a); t1 = std::min(t1, c); if (t0 > t1) return false; }
  return true;
}
}  // namespace
void Bvh::build(const std::vector<Vec3>& pos, const std::vector<uint32_t>& tris) {
  P = pos; idx = tris; nodes.clear(); const int n = int(idx.size() / 3); order.resize(static_cast<size_t>(n));
  std::vector<Vec3> cen(static_cast<size_t>(n)); std::vector<Box3> tb(static_cast<size_t>(n));
  for (int t = 0; t < n; t++) { order[size_t(t)] = t; for (int j = 0; j < 3; j++) tb[size_t(t)].expand(P[idx[size_t(t) * 3 + size_t(j)]]); cen[size_t(t)] = tb[size_t(t)].center(); }
  if (!n) return;
  nodes.reserve(size_t(n / 2) + 1);
  std::function<int(int, int)> rec = [&](int first, int count) -> int {
    int id = int(nodes.size()); nodes.push_back({}); Box3 b, cb;
    for (int i = first; i < first + count; i++) { b.unite(tb[size_t(order[size_t(i)])]); cb.expand(cen[size_t(order[size_t(i)])]); }
    nodes[size_t(id)].b = b;
    if (count <= 8) { nodes[size_t(id)].first = first; nodes[size_t(id)].count = count; return id; }
    Vec3 s = cb.size(); int ax = s.x >= s.y && s.x >= s.z ? 0 : s.y >= s.z ? 1 : 2;
    int mid = first + count / 2;
    std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count, [&](int a, int c) { return cen[size_t(a)][ax] < cen[size_t(c)][ax]; });
    int l = rec(first, mid - first), r = rec(mid, first + count - mid);
    nodes[size_t(id)].left = l; nodes[size_t(id)].right = r; return id;
  };
  rec(0, n);
}
std::optional<Vec3> Bvh::closest(const Vec3& q, const std::function<bool(int)>& ok, double* dOut, int* triOut) const {
  if (nodes.empty()) return std::nullopt;
  double bd = 1e300; std::optional<Vec3> best;
  std::vector<int> st{0};
  while (!st.empty()) {
    int ni = st.back(); st.pop_back(); const Node& nd = nodes[size_t(ni)];
    if (boxDist(nd.b, q) >= bd) continue;
    if (nd.left < 0) {
      for (int i = nd.first; i < nd.first + nd.count; i++) { int t = order[size_t(i)]; if (ok && !ok(t)) continue;
        Vec3 c = closestOnTri(q, P[idx[size_t(t) * 3]], P[idx[size_t(t) * 3 + 1]], P[idx[size_t(t) * 3 + 2]]); double d = c.distanceTo(q); if (d < bd) { bd = d; best = c; if (triOut) *triOut = t; } }
      continue;
    }
    double dl = boxDist(nodes[size_t(nd.left)].b, q), dr = boxDist(nodes[size_t(nd.right)].b, q);
    if (dl < dr) { st.push_back(nd.right); st.push_back(nd.left); } else { st.push_back(nd.left); st.push_back(nd.right); }
  }
  if (dOut) *dOut = bd;
  return best;
}
std::optional<std::pair<double, int>> Bvh::ray(const Vec3& o, const Vec3& d, double tmax) const {
  if (nodes.empty()) return std::nullopt;
  std::optional<std::pair<double, int>> best; double bt = tmax; std::vector<int> st{0};
  while (!st.empty()) {
    int ni = st.back(); st.pop_back(); const Node& nd = nodes[size_t(ni)];
    if (!rayBoxT(o, d, nd.b, bt)) continue;
    if (nd.left >= 0) { st.push_back(nd.left); st.push_back(nd.right); continue; }
    for (int i = nd.first; i < nd.first + nd.count; i++) {
      int t = order[size_t(i)]; const Vec3 &a = P[idx[size_t(t) * 3]], &b = P[idx[size_t(t) * 3 + 1]], &c = P[idx[size_t(t) * 3 + 2]];
      Vec3 e1 = b - a, e2 = c - a, pv = d.cross(e2); double det = e1.dot(pv); if (std::abs(det) < 1e-18) continue;
      double inv = 1 / det; Vec3 tv = o - a; double u = tv.dot(pv) * inv; if (u < 0 || u > 1) continue;
      Vec3 qv = tv.cross(e1); double v = d.dot(qv) * inv; if (v < 0 || u + v > 1) continue;
      double tt = e2.dot(qv) * inv; if (tt <= 0 || tt >= bt) continue;
      bt = tt; best = std::make_pair(tt, t);
    }
  }
  return best;
}
void Bvh::sphere(const Vec3& c, double r, const std::function<void(int)>& fn) const {
  if (nodes.empty()) return;
  std::vector<int> st{0};
  while (!st.empty()) {
    int ni = st.back(); st.pop_back(); const Node& nd = nodes[size_t(ni)];
    if (boxDist(nd.b, c) >= r) continue;
    if (nd.left >= 0) { st.push_back(nd.left); st.push_back(nd.right); continue; }
    for (int i = nd.first; i < nd.first + nd.count; i++) {
      int t = order[size_t(i)]; Vec3 q = closestOnTri(c, P[idx[size_t(t) * 3]], P[idx[size_t(t) * 3 + 1]], P[idx[size_t(t) * 3 + 2]]);
      if (q.distanceTo(c) <= r) fn(t);
    }
  }
}

// ---- the polygroups' tree ----
qint64 tgNid() { static qint64 n = 1; return (QDateTime::currentMSecsSinceEpoch() % 1000000) * 1000 + (n++ % 1000); }
namespace {
void nodeFrom(const QJsonObject& o, TgNode& n) {
  n.id = qint64(o.value("id").toDouble()); if (!n.id) n.id = tgNid();
  n.isFolder = o.contains("f") && o.value("f").isString(); if (n.isFolder) n.f = o.value("f").toString();
  n.g = o.value("g").toInt(-1); n.open = o.value("open").toBool(true);
  for (const QJsonValue& k : o.value("kids").toArray()) { TgNode c; nodeFrom(k.toObject(), c); n.kids.push_back(std::move(c)); }
}
std::vector<TgNode> clean(std::vector<TgNode>& list, int G, std::set<int>& seen) {
  std::vector<TgNode> out;
  for (auto& n : list) {
    n.kids = clean(n.kids, G, seen);
    if (n.isFolder) { out.push_back(std::move(n)); continue; }
    if (n.g >= 0 && n.g < G && !seen.count(n.g)) { seen.insert(n.g); out.push_back(std::move(n)); }
    else for (auto& k : n.kids) out.push_back(std::move(k));
  }
  return out;
}
}  // namespace
std::vector<TgNode> tgTree(const QJsonValue& raw, int G, const QJsonValue& gOrder) {
  std::vector<TgNode> t; for (const QJsonValue& v : raw.toArray()) { TgNode n; nodeFrom(v.toObject(), n); t.push_back(std::move(n)); }
  std::set<int> seen; t = clean(t, G, seen);
  std::vector<int> order; for (const QJsonValue& v : gOrder.toArray()) { int g = v.toInt(-1); if (g >= 0 && g < G && std::find(order.begin(), order.end(), g) == order.end()) order.push_back(g); }
  for (int g = 0; g < G; g++) if (std::find(order.begin(), order.end(), g) == order.end()) order.push_back(g);
  for (int g : order) if (!seen.count(g)) { seen.insert(g); TgNode n; n.id = tgNid(); n.g = g; t.push_back(std::move(n)); }
  return t;
}
QJsonArray tgJson(const std::vector<TgNode>& t) {
  QJsonArray a;
  for (auto& n : t) { QJsonObject o{{"id", double(n.id)}, {"kids", tgJson(n.kids)}, {"open", n.open}}; if (n.isFolder) o.insert("f", n.f); else o.insert("g", n.g); a.append(o); }
  return a;
}
void tgWalk(const std::vector<TgNode>& list, const std::function<void(const TgNode&, int)>& fn, int depth) { for (auto& n : list) { fn(n, depth); tgWalk(n.kids, fn, depth + 1); } }
std::vector<int> tgUnder(const TgNode& n) { std::vector<int> o; if (!n.isFolder) o.push_back(n.g); tgWalk(n.kids, [&](const TgNode& x, int) { if (!x.isFolder) o.push_back(x.g); }); return o; }
TgNode* tgFind(std::vector<TgNode>& list, qint64 id, std::vector<TgNode>** arr) {
  for (auto& n : list) { if (n.id == id) { if (arr) *arr = &list; return &n; } if (TgNode* r = tgFind(n.kids, id, arr)) return r; }
  return nullptr;
}
QJsonArray selSetsFollow(const QJsonArray& sets, const std::shared_ptr<PolyMesh>& A, const std::shared_ptr<PolyMesh>& B, bool dropEmpty) {
  if (sets.isEmpty() || !A || !B) return sets;
  auto T0 = ttopo(A), T1 = ttopo(B); if (!T0 || !T1) return sets;
  using Q = std::array<long long, 3>;
  const double qq = std::max(1e-9, A->bounds().size().length() * 1e-5);
  auto qk = [qq](const Vec3& p) { return Q{std::llround(p.x / qq), std::llround(p.y / qq), std::llround(p.z / qq)}; };
  auto fkey = [&](const TTopo& T, const std::vector<Vec3>& P, int f) { std::vector<Q> k; for (int j = T.fo[size_t(f)]; j < T.fo[size_t(f) + 1]; j++) k.push_back(qk(P[size_t(T.fv[size_t(j)])])); std::sort(k.begin(), k.end()); return k; };
  auto ekey = [&](const TTopo& T, const std::vector<Vec3>& P, int e) { Q a = qk(P[size_t(T.ea[size_t(e)])]), b = qk(P[size_t(T.eb[size_t(e)])]); if (b < a) std::swap(a, b); return std::make_pair(a, b); };
  // (the new geometry's elements by place — made once, for all the sets)
  std::map<std::vector<Q>, std::vector<int>> F1; std::map<std::pair<Q, Q>, std::vector<int>> E1; std::map<Q, std::vector<int>> V1; bool fM = false, eM = false, vM = false;
  QJsonArray out;
  for (const QJsonValue& sv : sets) { QJsonObject s = sv.toObject(); const QString k = s.value("k").toString(); const char kc = k.isEmpty() ? 'f' : k[0].toLatin1();
    const int N0 = kc == 'f' ? T0->nf : kc == 'e' ? T0->ne : T0->n, N1 = kc == 'f' ? T1->nf : kc == 'e' ? T1->ne : T1->n;
    if (s.value("n").toInt() != N0) { out.append(s); continue; }
    const auto in = smDec(s.value("d").toString(), N0); std::vector<char> a1(size_t(N1), 0); int cnt = 0;
    if (kc == 'f') { if (!fM) { fM = true; for (int f = 0; f < T1->nf; f++) F1[fkey(*T1, B->pos, f)].push_back(f); }
      for (int i = 0; i < N0; i++) if (in[size_t(i)]) if (auto q = F1.find(fkey(*T0, A->pos, i)); q != F1.end()) for (int f : q->second) { cnt += !a1[size_t(f)]; a1[size_t(f)] = 1; } }
    else if (kc == 'e') { if (!eM) { eM = true; for (int e = 0; e < T1->ne; e++) E1[ekey(*T1, B->pos, e)].push_back(e); }
      for (int i = 0; i < N0; i++) if (in[size_t(i)]) if (auto q = E1.find(ekey(*T0, A->pos, i)); q != E1.end()) for (int e : q->second) { cnt += !a1[size_t(e)]; a1[size_t(e)] = 1; } }
    else { if (!vM) { vM = true; for (int v = 0; v < T1->n; v++) V1[qk(B->pos[size_t(v)])].push_back(v); }
      for (int i = 0; i < N0; i++) if (in[size_t(i)]) if (auto q = V1.find(qk(A->pos[size_t(i)])); q != V1.end()) for (int v : q->second) { cnt += !a1[size_t(v)]; a1[size_t(v)] = 1; } }
    if (!cnt && dropEmpty) continue;                               // (nothing of it left: no use keeping it)
    s.insert("n", N1); s.insert("d", smEnc(a1)); out.append(s); }
  return out;
}

void groupsFollow(QJsonObject& raw, const std::function<int(int)>& map) {
  for (const char* k : {"hidGroups", "gOrder"}) if (raw.value(k).isArray()) { QJsonArray o; for (const QJsonValue& v : raw.value(k).toArray()) { int g = map(v.toInt()); if (g >= 0) o.append(g); } raw.insert(k, o); }
  if (raw.value("gTree").isArray()) {
    std::function<void(QJsonArray&)> fol = [&](QJsonArray& a) { for (int i = 0; i < a.size(); i++) { QJsonObject o = a[i].toObject(); if (o.contains("g")) o.insert("g", map(o.value("g").toInt())); QJsonArray k = o.value("kids").toArray(); fol(k); o.insert("kids", k); a[i] = o; } };
    QJsonArray t = raw.value("gTree").toArray(); fol(t); raw.insert("gTree", t);
  }
}

}  // namespace hm::topo
