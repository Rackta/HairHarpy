#include "scene/Subdiv.h"
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>
#include <QByteArray>
#include <QJsonArray>

namespace hm::subdiv {
namespace {

uint64_t ekey(int a, int b) { if (a > b) std::swap(a, b); return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); }

// one Catmull–Clark step over positions + polygons (CSR). New vertices: the old ones (same index, moved), then one per edge
// (V + e), then one per polygon (V + E + f). Every polygon of k corners gives k quads [corner, edge point after it, polygon
// point, edge point before it] — in the same order whatever the positions are (the UVs are run through it as a mesh of
// their own and line up corner for corner)
struct Core {
  std::vector<Vec3> pos;                 // out
  std::vector<int> fs{0}, fv;            // out polygons
  std::vector<int> srcFace;              // out polygon → in polygon
  std::vector<int> ea, eb;               // edges (in vertex ids)
  std::unordered_map<uint64_t, int> edgeId;
  int V = 0, E = 0;
};

Core cc(const std::vector<Vec3>& pos, const std::vector<int>& fs, const std::vector<int>& fv, const std::unordered_map<uint64_t, float>* crease, bool smoothOpen) {
  Core c; const int V = int(pos.size()), nF = int(fs.size()) - 1; c.V = V;
  auto valid = [&](int f) { return fs[size_t(f) + 1] - fs[size_t(f)] >= 3; };
  std::vector<int> eoc(fv.size(), -1);                       // the edge after each corner
  std::vector<int> nf, f1, f2;
  for (int f = 0; f < nF; f++) { if (!valid(f)) continue; const int s = fs[size_t(f)], e = fs[size_t(f) + 1];
    for (int q = s; q < e; q++) { const int a = fv[size_t(q)], b = fv[size_t(q + 1 < e ? q + 1 : s)]; const uint64_t k = ekey(a, b);
      auto it = c.edgeId.find(k); int id;
      if (it == c.edgeId.end()) { id = int(c.ea.size()); c.edgeId.emplace(k, id); c.ea.push_back(a); c.eb.push_back(b); nf.push_back(0); f1.push_back(-1); f2.push_back(-1); } else id = it->second;
      eoc[size_t(q)] = id; if (nf[size_t(id)] == 0) f1[size_t(id)] = f; else if (nf[size_t(id)] == 1) f2[size_t(id)] = f; nf[size_t(id)]++; } }
  const int E = int(c.ea.size()); c.E = E;
  std::vector<Vec3> fp(static_cast<size_t>(nF));
  for (int f = 0; f < nF; f++) { if (!valid(f)) continue; Vec3 s; const int a = fs[size_t(f)], b = fs[size_t(f) + 1]; for (int q = a; q < b; q++) s += pos[size_t(fv[size_t(q)])]; fp[size_t(f)] = s * (1.0 / (b - a)); }
  std::vector<char> sharpB(size_t(E), 0), sharpC(size_t(E), 0);
  for (int e = 0; e < E; e++) { sharpB[size_t(e)] = nf[size_t(e)] != 2;
    if (crease && nf[size_t(e)] == 2) if (auto it = crease->find(ekey(c.ea[size_t(e)], c.eb[size_t(e)])); it != crease->end() && it->second >= 0.5f) sharpC[size_t(e)] = 1; }
  c.pos.resize(size_t(V + E + nF));
  // edge points
  for (int e = 0; e < E; e++) { const Vec3& a = pos[size_t(c.ea[size_t(e)])]; const Vec3& b = pos[size_t(c.eb[size_t(e)])];
    c.pos[size_t(V + e)] = (sharpB[size_t(e)] || sharpC[size_t(e)] || f2[size_t(e)] < 0) ? (a + b) * 0.5 : (a + b + fp[size_t(f1[size_t(e)])] + fp[size_t(f2[size_t(e)])]) * 0.25; }
  for (int f = 0; f < nF; f++) if (valid(f)) c.pos[size_t(V + E + f)] = fp[size_t(f)];
  // vertex points
  std::vector<std::vector<int>> vE(static_cast<size_t>(V)); for (int e = 0; e < E; e++) { vE[size_t(c.ea[size_t(e)])].push_back(e); vE[size_t(c.eb[size_t(e)])].push_back(e); }
  std::vector<Vec3> fsum(static_cast<size_t>(V)); std::vector<int> fcnt(size_t(V), 0);
  for (int f = 0; f < nF; f++) { if (!valid(f)) continue; for (int q = fs[size_t(f)]; q < fs[size_t(f) + 1]; q++) { fsum[size_t(fv[size_t(q)])] += fp[size_t(f)]; fcnt[size_t(fv[size_t(q)])]++; } }
  for (int v = 0; v < V; v++) {
    const auto& es = vE[size_t(v)]; const int n = int(es.size()); const Vec3& P = pos[size_t(v)];
    if (n == 0 || fcnt[size_t(v)] == 0) { c.pos[size_t(v)] = P; continue; }
    int sb = 0, sc = 0; int o[2] = {-1, -1}; Vec3 R;
    for (int e : es) { const int other = c.ea[size_t(e)] == v ? c.eb[size_t(e)] : c.ea[size_t(e)]; R += (P + pos[size_t(other)]) * 0.5;
      if (sharpB[size_t(e)]) { if (sb + sc < 2) o[sb + sc] = other; sb++; } else if (sharpC[size_t(e)]) { if (sb + sc < 2) o[sb + sc] = other; sc++; } }
    const int s = sb + sc;
    if (sb > 0 && !smoothOpen) c.pos[size_t(v)] = P;                                        // (an open border kept as it is)
    else if (s == 0) { if (n < 3) { c.pos[size_t(v)] = P; continue; }
      const Vec3 Q = fsum[size_t(v)] * (1.0 / fcnt[size_t(v)]); R = R * (1.0 / n); c.pos[size_t(v)] = (Q + R * 2.0 + P * double(n - 3)) * (1.0 / n); }
    else if (s == 2) c.pos[size_t(v)] = P * 0.75 + (pos[size_t(o[0])] + pos[size_t(o[1])]) * 0.125;   // (along a border / crease: the curve's own rule)
    else c.pos[size_t(v)] = P;                                                               // (a corner: the end of a crease, a fork)
  }
  for (int f = 0; f < nF; f++) { if (!valid(f)) continue; const int s = fs[size_t(f)], e = fs[size_t(f) + 1], k = e - s;
    for (int i = 0; i < k; i++) { const int q = s + i, qp = s + (i + k - 1) % k;
      c.fv.push_back(fv[size_t(q)]); c.fv.push_back(V + eoc[size_t(q)]); c.fv.push_back(V + E + f); c.fv.push_back(V + eoc[size_t(qp)]);
      c.fs.push_back(int(c.fv.size())); c.srcFace.push_back(f); } }
  return c;
}

// the UVs of one layout (per corner of `in`) as a surface of their own: the corners welded by (vertex, UV) into points,
// the polygons the mesh's; the island rims are its open borders
std::vector<std::array<float, 2>> uvStep(const PolyMesh& in, const std::vector<std::array<float, 2>>& uv, const Core& g, bool smooth) {
  std::unordered_map<uint64_t, int> id; std::vector<Vec3> up; std::vector<int> upv, uf(in.faceVerts.size());
  auto q16 = [](float x) { return int64_t(std::llround(double(x) * 20000.0)); };
  for (size_t q = 0; q < in.faceVerts.size(); q++) {
    uint64_t h = uint64_t(uint32_t(in.faceVerts[q])) * 0x9E3779B97F4A7C15ull ^ (uint64_t(q16(uv[q][0])) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(q16(uv[q][1])) * 0x165667B19E3779F9ull);
    // (a hash may meet another corner's: compared whole, a clash goes on to the next slot)
    for (;; h++) { auto it = id.find(h); if (it == id.end()) { id.emplace(h, int(up.size())); uf[q] = int(up.size()); up.push_back({double(uv[q][0]), double(uv[q][1]), 0}); upv.push_back(in.faceVerts[q]); break; }
      const Vec3& p = up[size_t(it->second)]; if (upv[size_t(it->second)] == in.faceVerts[q] && std::abs(p.x - uv[q][0]) < 1e-4 && std::abs(p.y - uv[q][1]) < 1e-4) { uf[q] = it->second; break; } } }
  Core u = cc(up, in.faceStart, uf, nullptr, smooth);
  std::vector<std::array<float, 2>> out; out.reserve(u.fv.size());
  for (int v : u.fv) out.push_back({float(u.pos[size_t(v)].x), float(u.pos[size_t(v)].y)});
  (void)g; return out;
}

std::shared_ptr<PolyMesh> step(const PolyMesh& in, const Opts& o, Core* coreOut = nullptr) {
  std::unordered_map<uint64_t, float> cr; for (const auto& c : in.creases) if (c.w > 0) cr[ekey(c.a, c.b)] = std::max(c.w, cr.count(ekey(c.a, c.b)) ? cr[ekey(c.a, c.b)] : 0.f);
  Core g = cc(in.pos, in.faceStart, in.faceVerts, &cr, o.smoothOpen); if (coreOut) *coreOut = g;
  auto out = std::make_shared<PolyMesh>(); PolyMesh& P = *out;
  P.pos = std::move(g.pos); P.faceStart = g.fs; P.faceVerts = g.fv;
  const int nF = P.faceCount();
  auto per = [&](const auto& src, auto& dst) { if (src.size() == size_t(in.faceCount())) { dst.reserve(size_t(nF)); for (int f = 0; f < nF; f++) dst.push_back(src[size_t(g.srcFace[size_t(f)])]); } };
  per(in.faceGroup, P.faceGroup); per(in.faceMat, P.faceMat); per(in.faceSets, P.faceSets); per(in.faceLive, P.faceLive);
  P.groupNames = in.groupNames; P.matNames = in.matNames; P.matMaps = in.matMaps; P.matColor = in.matColor; P.matAlpha = in.matAlpha; P.mtllib = in.mtllib; P.flipY = in.flipY; P.uvSetNames = in.uvSetNames;
  if (in.cornerUV.size() == in.faceVerts.size() && !in.faceVerts.empty()) P.cornerUV = uvStep(in, in.cornerUV, g, o.uvSmooth);
  for (const auto& s : in.setUV) { if (s.size() == in.faceVerts.size()) P.setUV.push_back(uvStep(in, s, g, o.uvSmooth)); else P.setUV.emplace_back(); }
  if (in.vcol.size() == in.pos.size()) {                        // (the polypaint: the old points keep theirs, an edge point — the middle, a polygon's — the mean)
    P.vcol = in.vcol; P.vcol.resize(P.pos.size());
    for (int e = 0; e < g.E; e++) for (int k = 0; k < 3; k++) P.vcol[size_t(g.V + e)][size_t(k)] = (in.vcol[size_t(g.ea[size_t(e)])][size_t(k)] + in.vcol[size_t(g.eb[size_t(e)])][size_t(k)]) * 0.5f;
    for (int f = 0; f < in.faceCount(); f++) { const int s = in.faceStart[size_t(f)], e = in.faceStart[size_t(f) + 1]; if (e - s < 3) continue; std::array<float, 3> a{0, 0, 0};
      for (int q = s; q < e; q++) for (int k = 0; k < 3; k++) a[size_t(k)] += in.vcol[size_t(in.faceVerts[size_t(q)])][size_t(k)];
      for (int k = 0; k < 3; k++) P.vcol[size_t(g.V + g.E + f)][size_t(k)] = a[size_t(k)] / float(e - s); } }
  // (creases, hard edges, UV seams: each edge cut in two — the halves carry it on to the next level and into Apply)
  auto half = [&](int a, int b, int& ma, int& mb, int& mid) { auto it = g.edgeId.find(ekey(a, b)); if (it == g.edgeId.end()) return false; mid = g.V + it->second; ma = a; mb = b; return true; };
  for (const auto& c : in.creases) { int a, b, m; if (half(c.a, c.b, a, b, m)) { P.creases.push_back({a, m, c.w}); P.creases.push_back({m, b, c.w}); } }
  for (const auto& h : in.hardEdges) { int a, b, m; if (half(h[0], h[1], a, b, m)) { P.hardEdges.push_back({a, m}); P.hardEdges.push_back({m, b}); } }
  for (const auto& h : in.seams) { int a, b, m; if (half(h[0], h[1], a, b, m)) { P.seams.push_back({a, m}); P.seams.push_back({m, b}); } }
  return out;
}

// ---------- the details: the shape a level has beyond the subdivision of the one below ----------
struct Frm { Vec3 n, t, b; };
// a frame at every point: the normal (the polygons round it, by area), the tangent (towards the first neighbour in the
// point's first polygon, laid in the surface), the bitangent
std::vector<Frm> framesOf(const PolyMesh& m) {
  const size_t V = m.pos.size(); std::vector<Vec3> N(V); std::vector<int> nb(V, -1);
  for (int f = 0; f < m.faceCount(); f++) { const int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1]; if (e - s < 3) continue; Vec3 a;
    for (int q = s; q < e; q++) a += m.pos[size_t(m.faceVerts[size_t(q)])].cross(m.pos[size_t(m.faceVerts[size_t(q + 1 < e ? q + 1 : s)])]);
    for (int q = s; q < e; q++) { const size_t v = size_t(m.faceVerts[size_t(q)]); N[v] += a; if (nb[v] < 0) nb[v] = m.faceVerts[size_t(q + 1 < e ? q + 1 : s)]; } }
  std::vector<Frm> out(V);
  for (size_t v = 0; v < V; v++) { Frm& F = out[v]; F.n = N[v].lengthSq() > 1e-30 ? N[v].normalized() : Vec3(0, 0, 1);
    Vec3 d = nb[v] >= 0 ? m.pos[size_t(nb[v])] - m.pos[v] : Vec3(); Vec3 t = d - F.n * d.dot(F.n);
    if (t.lengthSq() < 1e-24) { const Vec3 u = std::abs(F.n.x) < 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0); t = u - F.n * u.dot(F.n); }
    F.t = t.normalized(); F.b = F.n.cross(F.t); }
  return out;
}
void applyDetail(PolyMesh& S, const std::vector<std::array<float, 3>>& d) {
  const auto fr = framesOf(S); for (size_t i = 0; i < S.pos.size() && i < d.size(); i++) S.pos[i] += fr[i].n * double(d[i][0]) + fr[i].t * double(d[i][1]) + fr[i].b * double(d[i][2]);
}

// ---------- reconstruct ----------
using Row = std::vector<std::pair<int, double>>;
// how the subdivision puts each point of a mesh: as the weights of the mesh's own points (the rule of cc() for the old points)
std::vector<Row> vertexRows(const PolyMesh& m, bool smoothOpen) {
  const int V = int(m.pos.size()); std::unordered_map<uint64_t, int> id; std::vector<int> ea, eb, nf;
  std::vector<std::vector<int>> vE(static_cast<size_t>(V)), vF(static_cast<size_t>(V));
  for (int f = 0; f < m.faceCount(); f++) { const int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1]; if (e - s < 3) continue;
    for (int q = s; q < e; q++) { const int a = m.faceVerts[size_t(q)], b = m.faceVerts[size_t(q + 1 < e ? q + 1 : s)]; vF[size_t(a)].push_back(f);
      auto it = id.find(ekey(a, b)); if (it == id.end()) { id.emplace(ekey(a, b), int(ea.size())); ea.push_back(a); eb.push_back(b); nf.push_back(1); } else nf[size_t(it->second)]++; } }
  for (int e = 0; e < int(ea.size()); e++) { vE[size_t(ea[size_t(e)])].push_back(e); vE[size_t(eb[size_t(e)])].push_back(e); }
  std::vector<Row> rows(static_cast<size_t>(V));
  for (int v = 0; v < V; v++) { Row& r = rows[size_t(v)]; const auto& es = vE[size_t(v)]; const int n = int(es.size()), nfv = int(vF[size_t(v)].size());
    auto add = [&](int u, double w) { for (auto& p : r) if (p.first == u) { p.second += w; return; } r.push_back({u, w}); };
    int sb = 0; int o[2] = {-1, -1};                       // (an edge of one polygon or of three and more is a border: as the subdivision does)
    for (int e : es) { const int other = ea[size_t(e)] == v ? eb[size_t(e)] : ea[size_t(e)]; if (nf[size_t(e)] != 2) { if (sb < 2) o[sb] = other; sb++; } }
    if (n == 0) { r = {{v, 1.0}}; continue; }
    if (sb > 0) { if (!smoothOpen || sb != 2) r = {{v, 1.0}}; else r = {{v, 0.75}, {o[0], 0.125}, {o[1], 0.125}}; continue; }
    if (n < 3 || nfv != n) { r = {{v, 1.0}}; continue; }
    add(v, double(n - 3) / n);
    for (int f : vF[size_t(v)]) { const int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1]; for (int q = s; q < e; q++) add(m.faceVerts[size_t(q)], 1.0 / (double(n) * n * (e - s))); }
    for (int e : es) { const int other = ea[size_t(e)] == v ? eb[size_t(e)] : ea[size_t(e)]; add(v, 1.0 / (double(n) * n)); add(other, 1.0 / (double(n) * n)); }
  }
  return rows;
}
// the mesh whose subdivision, at its old points, is `target` (least squares, conjugate gradients on the normal equations)
std::vector<Vec3> solveCage(const PolyMesh& cage, const std::vector<Vec3>& target, bool smoothOpen) {
  const auto rows = vertexRows(cage, smoothOpen); const size_t n = rows.size(); std::vector<Vec3> x = target, r(n), s(n), p(n), q(n);
  auto mul = [&](const std::vector<Vec3>& in, std::vector<Vec3>& out) { for (size_t i = 0; i < n; i++) { Vec3 a; for (auto& c : rows[i]) a += in[size_t(c.first)] * c.second; out[i] = a; } };
  auto mulT = [&](const std::vector<Vec3>& in, std::vector<Vec3>& out) { std::fill(out.begin(), out.end(), Vec3()); for (size_t i = 0; i < n; i++) for (auto& c : rows[i]) out[size_t(c.first)] += in[i] * c.second; };
  auto dot = [&](const std::vector<Vec3>& a, const std::vector<Vec3>& b) { double d = 0; for (size_t i = 0; i < n; i++) d += a[i].dot(b[i]); return d; };
  mul(x, q); for (size_t i = 0; i < n; i++) r[i] = target[i] - q[i]; mulT(r, s); p = s; double g = dot(s, s); const double g0 = g;
  for (int it = 0; it < 400 && g > 1e-26 * g0 + 1e-300; it++) {
    mul(p, q); const double qq = dot(q, q); if (qq < 1e-300) break; const double a = g / qq;
    for (size_t i = 0; i < n; i++) { x[i] += p[i] * a; r[i] -= q[i] * a; }
    mulT(r, s); const double g2 = dot(s, s); const double b = g2 / g; g = g2; for (size_t i = 0; i < n; i++) p[i] = s[i] + p[i] * b;
  }
  return x;
}
void weldUV(const PolyMesh& in, const std::vector<std::array<float, 2>>& uv, std::vector<Vec3>& up, std::vector<int>& uf) {
  std::unordered_map<uint64_t, int> id; std::vector<int> upv; uf.assign(in.faceVerts.size(), 0); up.clear();
  auto q16 = [](float x) { return int64_t(std::llround(double(x) * 20000.0)); };
  for (size_t q = 0; q < in.faceVerts.size(); q++) {
    uint64_t h = uint64_t(uint32_t(in.faceVerts[q])) * 0x9E3779B97F4A7C15ull ^ (uint64_t(q16(uv[q][0])) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(q16(uv[q][1])) * 0x165667B19E3779F9ull);
    for (;; h++) { auto it = id.find(h); if (it == id.end()) { id.emplace(h, int(up.size())); uf[q] = int(up.size()); up.push_back({double(uv[q][0]), double(uv[q][1]), 0}); upv.push_back(in.faceVerts[q]); break; }
      const Vec3& p = up[size_t(it->second)]; if (upv[size_t(it->second)] == in.faceVerts[q] && std::abs(p.x - uv[q][0]) < 1e-4 && std::abs(p.y - uv[q][1]) < 1e-4) { uf[q] = it->second; break; } } }
}

enum : char { LV = 1, LE = 2, LF = 3 };   // a point of a subdivision: an old point, an edge point, a polygon's point
uint64_t dkey(int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); }
bool nextLabels(int x, int y, int& p, int& q) {   // (x, y — two labels in a row round a quad V E F E: the next two)
  if (x == LV && y == LE) { p = LF; q = LE; } else if (x == LE && y == LV) { p = LE; q = LF; } else if (x == LE && y == LF) { p = LE; q = LV; } else if (x == LF && y == LE) { p = LV; q = LE; } else return false;
  return true;
}

struct Built {
  bool ok = false; QString why; std::shared_ptr<PolyMesh> cage, C; std::vector<int> hToC; std::vector<std::array<float, 3>> d; double residual = 0;
};
// the cage a labelling of the quad mesh H (every quad V E F E round) makes, solved, with the detail — or why it cannot be
Built buildFromLabels(const PolyMesh& H, const std::vector<char>& L, const Opts& o, bool full, bool allowNgon = false) {
  Built R; const int nV = int(H.pos.size()), nQ = H.faceCount();
  auto Q = [&](int q, int i) { return H.faceVerts[size_t(H.faceStart[size_t(q)] + ((i % 4 + 4) % 4))]; };
  std::vector<int> cv(static_cast<size_t>(nV), -1), vs; for (int v = 0; v < nV; v++) if (L[size_t(v)] == LV) { cv[size_t(v)] = int(vs.size()); vs.push_back(v); }
  // each edge point: the two old points it lies between
  std::vector<std::array<int, 2>> eV(static_cast<size_t>(nV), std::array<int, 2>{-1, -1});
  for (int q = 0; q < nQ; q++) for (int i = 0; i < 4; i++) if (L[size_t(Q(q, i))] == LE) { auto& ev = eV[size_t(Q(q, i))];
    for (int nb : {Q(q, i - 1), Q(q, i + 1)}) if (L[size_t(nb)] == LV) { const int c = cv[size_t(nb)]; if (ev[0] == c || ev[1] == c) continue; if (ev[0] < 0) ev[0] = c; else if (ev[1] < 0) ev[1] = c; else { R.why = "nolabel"; return R; } } }
  for (int v = 0; v < nV; v++) if (L[size_t(v)] == LE && (eV[size_t(v)][0] < 0 || eV[size_t(v)][1] < 0)) { R.why = "nolabel"; return R; }
  // the cage's polygons: round each polygon point, the old points of the quads that meet there (the way the quads go round it)
  std::vector<int> faceOfF(static_cast<size_t>(nV), -1); std::vector<std::vector<int>> at(static_cast<size_t>(nV));   // (at: the quads (q * 4 + corner) each polygon point is in)
  for (int q = 0; q < nQ; q++) for (int i = 0; i < 4; i++) if (L[size_t(Q(q, i))] == LF) at[size_t(Q(q, i))].push_back(q * 4 + i);
  auto cage = std::make_shared<PolyMesh>(); std::vector<std::vector<int>> cfq, cfc;   // (per cage polygon: its quads, the corner of each that is the old point)
  for (int f = 0; f < nV; f++) { if (L[size_t(f)] != LF) continue; if (at[size_t(f)].empty()) { R.why = "nolabel"; return R; }
    // (round the point: from a quad out through its edge towards the previous corner, into the other quad on that edge, and so on — whichever way the quads face)
    std::vector<int> fv, qs, cs; const int start = at[size_t(f)][0]; int cur = start, entry = Q(cur >> 2, (cur & 3) + 1), count = 0;   // (the first quad is entered by its edge after the point: left by the one before it — the way its own winding goes round)
    do { const int q = cur >> 2, i = cur & 3; const int vc = Q(q, i + 2); if (L[size_t(vc)] != LV) { R.why = "nolabel"; return R; }
      fv.push_back(cv[size_t(vc)]); qs.push_back(q); cs.push_back(H.faceStart[size_t(q)] + ((i + 2) & 3));
      const int e1 = Q(q, i + 1), e2 = Q(q, i - 1), out = entry == e2 ? e1 : e2;      // (the way out is the other edge of this quad at the point)
      int next = -1; for (int o2 : at[size_t(f)]) { const int q2 = o2 >> 2, i2 = o2 & 3; if (q2 == q) continue; const int n1 = Q(q2, i2 + 1), n2 = Q(q2, i2 - 1);
        if (n1 == out || n2 == out) { if (next >= 0) { R.why = "nolabel"; return R; } next = o2; } }
      if (next < 0) { R.why = "nolabel"; return R; }
      entry = out; cur = next;
      if (++count > (allowNgon ? 64 : 4)) { R.why = allowNgon ? "nolabel" : "ngon"; return R; } } while (cur != start);
    if (count < 3) { R.why = "nolabel"; return R; }
    faceOfF[size_t(f)] = cage->faceCount(); cage->addFace(fv.data(), int(fv.size())); cfq.push_back(qs); cfc.push_back(cs); }
  if (cage->faceCount() == 0 || vs.empty()) { R.why = "nolabel"; return R; }
  // (every old point lies in a polygon of the cage)
  { std::vector<char> used(vs.size(), 0); for (int v : cage->faceVerts) used[size_t(v)] = 1; for (char u : used) if (!u) { R.why = "nolabel"; return R; } }
  std::vector<Vec3> target; for (int v : vs) target.push_back(H.pos[size_t(v)]);
  cage->pos = target; cage->pos = solveCage(*cage, target, o.smoothOpen);
  if (full) {                                                      // (what the polygons and points carry)
    const int nc = cage->faceCount();
    auto per = [&](const auto& src, auto& dst) { if (src.size() == size_t(nQ)) for (int f = 0; f < nc; f++) dst.push_back(src[size_t(cfq[size_t(f)][0])]); };
    per(H.faceGroup, cage->faceGroup); per(H.faceMat, cage->faceMat); per(H.faceSets, cage->faceSets); per(H.faceLive, cage->faceLive);
    cage->groupNames = H.groupNames; cage->matNames = H.matNames; cage->matMaps = H.matMaps; cage->matColor = H.matColor; cage->matAlpha = H.matAlpha; cage->mtllib = H.mtllib; cage->flipY = H.flipY; cage->uvSetNames = H.uvSetNames;
    if (H.vcol.size() == size_t(nV)) for (int v : vs) cage->vcol.push_back(H.vcol[size_t(v)]);
    // the UVs: a surface of their own over the same polygons — the corners welded into points, its borders the islands' rims
    auto uvOf = [&](const std::vector<std::array<float, 2>>& uv) {
      std::vector<Vec3> up; std::vector<int> uf; weldUV(H, uv, up, uf); std::unordered_map<int, int> cu; std::vector<int> uids; PolyMesh U; std::vector<int> corner;
      for (int f = 0; f < nc; f++) { std::vector<int> fv; for (int hc : cfc[size_t(f)]) { const int uid = uf[size_t(hc)]; auto it = cu.find(uid); if (it == cu.end()) { it = cu.emplace(uid, int(uids.size())).first; uids.push_back(uid); } fv.push_back(it->second); corner.push_back(it->second); }
        U.addFace(fv.data(), int(fv.size())); }
      std::vector<Vec3> tg; for (int u : uids) tg.push_back(up[size_t(u)]); U.pos = tg; const auto P = solveCage(U, tg, o.uvSmooth);
      std::vector<std::array<float, 2>> out; for (int c : corner) out.push_back({float(P[size_t(c)].x), float(P[size_t(c)].y)}); return out; };
    if (H.cornerUV.size() == H.faceVerts.size() && !H.faceVerts.empty()) cage->cornerUV = uvOf(H.cornerUV);
    for (const auto& s : H.setUV) { if (s.size() == H.faceVerts.size()) cage->setUV.push_back(uvOf(s)); else cage->setUV.emplace_back(); }
    // seams, hard edges, creases: an edge of the cage is two edges of H (old point — edge point), either marks it
    auto cageEdge = [&](int a, int b, std::array<int, 2>& out) { if (L[size_t(a)] == LE) std::swap(a, b); if (L[size_t(a)] != LV || L[size_t(b)] != LE) return false;
      const auto& ev = eV[size_t(b)]; const int ca = cv[size_t(a)]; const int other = ev[0] == ca ? ev[1] : ev[0]; if (ev[0] != ca && ev[1] != ca) return false; out = {ca, other}; return true; };
    std::set<std::pair<int, int>> done; for (const auto& h : H.seams) { std::array<int, 2> e; if (cageEdge(h[0], h[1], e) && done.insert({std::min(e[0], e[1]), std::max(e[0], e[1])}).second) cage->seams.push_back(e); }
    done.clear(); for (const auto& h : H.hardEdges) { std::array<int, 2> e; if (cageEdge(h[0], h[1], e) && done.insert({std::min(e[0], e[1]), std::max(e[0], e[1])}).second) cage->hardEdges.push_back(e); }
    std::map<std::pair<int, int>, float> cr; for (const auto& c : H.creases) { std::array<int, 2> e; if (c.w > 0 && cageEdge(c.a, c.b, e)) { auto& w = cr[{std::min(e[0], e[1]), std::max(e[0], e[1])}]; w = std::max(w, c.w); } }
    for (auto& [k, w] : cr) cage->creases.push_back({k.first, k.second, w});
  }
  // its subdivision, point for point against H: where each point of H lies in it, and the offset (in the frame there)
  Core core; auto C = step(*cage, o, &core); const int nCV = int(cage->pos.size());
  if (int(C->pos.size()) != nV) { R.why = "nolabel"; return R; }
  std::vector<int> hToC(static_cast<size_t>(nV), -1);
  for (int v = 0; v < nV; v++) { if (L[size_t(v)] == LV) hToC[size_t(v)] = cv[size_t(v)];
    else if (L[size_t(v)] == LF) hToC[size_t(v)] = nCV + core.E + faceOfF[size_t(v)];
    else { auto it = core.edgeId.find(ekey(eV[size_t(v)][0], eV[size_t(v)][1])); if (it == core.edgeId.end()) { R.why = "nolabel"; return R; } hToC[size_t(v)] = nCV + it->second; } }
  { std::vector<char> hit(static_cast<size_t>(nV), 0); for (int i : hToC) { if (i < 0 || i >= nV || hit[size_t(i)]) { R.why = "nolabel"; return R; } hit[size_t(i)] = 1; } }
  const auto fr = framesOf(*C); R.d.assign(static_cast<size_t>(nV), {0, 0, 0}); double err = 0; const Box3 bb = H.bounds(); const double dg = std::max(1e-12, bb.size().length());
  for (int v = 0; v < nV; v++) { const int i = hToC[size_t(v)]; const Vec3 dl = H.pos[size_t(v)] - C->pos[size_t(i)]; err += dl.lengthSq();
    R.d[size_t(i)] = {float(dl.dot(fr[size_t(i)].n)), float(dl.dot(fr[size_t(i)].t)), float(dl.dot(fr[size_t(i)].b))}; }
  R.residual = err / (double(nV) * dg * dg); R.cage = cage; R.C = C; R.hToC = std::move(hToC); R.ok = true; return R;
}

// the way round a quad mesh's components: its quads, edge by edge
struct QuadNet {
  std::unordered_map<uint64_t, std::vector<int>> adj;   // an edge (either way round) → the quads on it, each as quad * 4 + the corner the edge starts at
  void build(const PolyMesh& H) { adj.clear(); for (int q = 0; q < H.faceCount(); q++) for (int i = 0; i < 4; i++) {
      const int a = H.faceVerts[size_t(H.faceStart[size_t(q)] + i)], b = H.faceVerts[size_t(H.faceStart[size_t(q)] + (i + 1) % 4)]; adj[ekey(a, b)].push_back(q * 4 + i); } }
};
// the labelling that starts the first quad as option o (a turn of V E F E) — carried over every quad, whichever way the quads
// face and however many meet on an edge (false: it cannot be)
bool labelFrom(const PolyMesh& H, const QuadNet& net, int seed, int o, std::vector<char>& L, std::vector<char>& seen) {
  static const char cyc[4] = {LV, LE, LF, LE}; const int nQ = H.faceCount();
  auto Qv = [&](int q, int i) { return H.faceVerts[size_t(H.faceStart[size_t(q)] + ((i % 4 + 4) % 4))]; };
  auto set = [&](int v, int l) { if (L[size_t(v)] == 0) { L[size_t(v)] = char(l); return true; } return L[size_t(v)] == l; };
  for (int i = 0; i < 4; i++) if (!set(Qv(seed, i), cyc[(i + o) % 4])) return false;
  std::vector<int> stack{seed}; seen[size_t(seed)] = 1;
  while (!stack.empty()) { const int x = stack.back(); stack.pop_back();
    for (int i = 0; i < 4; i++) { auto it = net.adj.find(ekey(Qv(x, i), Qv(x, i + 1))); if (it == net.adj.end()) continue;
      for (int yj : it->second) { const int y = yj >> 2, j = yj & 3; if (y == x) continue;      // (y has the same edge, at its corner j and j + 1: its other two follow the pattern)
        int p, q; if (!nextLabels(L[size_t(Qv(y, j))], L[size_t(Qv(y, j + 1))], p, q)) return false;
        if (!set(Qv(y, j + 2), p) || !set(Qv(y, j + 3), q)) return false;
        if (!seen[size_t(y)]) { seen[size_t(y)] = 1; stack.push_back(y); } } } }
  for (int q = 0; q < nQ; q++) if (seen[size_t(q)]) { int k = -1; for (int r = 0; r < 4; r++) { bool m = true; for (int i = 0; i < 4; i++) if (L[size_t(Qv(q, i))] != cyc[(i + r) % 4]) m = false; if (m) k = r; } if (k < 0) return false; }
  return true;
}

// the details a mesh had, carried over to the new numbering after it went one level down (they were given as offsets at points
// numbered by its old order; the same shapes are now numbered as the subdivision of the new cage numbers them, and the frames
// there differ — so each offset is made a vector in the world first and then taken in the new frame)
std::vector<Detail> restack(const PolyMesh& O0, const std::vector<Detail>& old, const std::vector<int>& pi0, const PolyMesh& N1, const Opts& o, uint64_t newSig) {
  std::vector<Detail> out; auto O = std::make_shared<PolyMesh>(O0); auto N = std::make_shared<PolyMesh>(N1); std::vector<int> pi = pi0;
  for (const Detail& D : old) {
    Core cO, cN; auto CO = step(*O, o, &cO); auto CN = step(*N, o, &cN);
    if (D.d.size() != CO->pos.size() || CN->pos.size() != CO->pos.size() || pi.size() != O->pos.size()) break;
    std::vector<int> q(CO->pos.size(), -1); const int VO = cO.V, EO = cO.E, VN = cN.V, EN = cN.E; bool good = true;
    for (int j = 0; j < VO; j++) q[size_t(j)] = pi[size_t(j)];
    for (int e = 0; e < EO && good; e++) { auto it = cN.edgeId.find(ekey(pi[size_t(cO.ea[size_t(e)])], pi[size_t(cO.eb[size_t(e)])])); if (it == cN.edgeId.end()) good = false; else q[size_t(VO + e)] = VN + it->second; }
    std::map<std::array<int, 4>, int> byVerts;
    for (int g = 0; g < N->faceCount(); g++) { const int s = N->faceStart[size_t(g)], e = N->faceStart[size_t(g) + 1]; if (e - s != 4) continue; std::array<int, 4> k{N->faceVerts[size_t(s)], N->faceVerts[size_t(s) + 1], N->faceVerts[size_t(s) + 2], N->faceVerts[size_t(s) + 3]}; std::sort(k.begin(), k.end()); byVerts[k] = g; }
    for (int f = 0; f < O->faceCount() && good; f++) { const int s = O->faceStart[size_t(f)], e = O->faceStart[size_t(f) + 1]; if (e - s != 4) { good = false; break; }
      std::array<int, 4> k; for (int i = 0; i < 4; i++) k[size_t(i)] = pi[size_t(O->faceVerts[size_t(s + i)])]; std::sort(k.begin(), k.end());
      auto it = byVerts.find(k); if (it == byVerts.end()) good = false; else q[size_t(VO + EO + f)] = VN + EN + it->second; }
    if (!good) break;
    const auto frO = framesOf(*CO), frN = framesOf(*CN); Detail nd; nd.sig = newSig; nd.d.assign(CN->pos.size(), {0, 0, 0});
    for (size_t j = 0; j < q.size(); j++) { const Vec3 w = frO[j].n * double(D.d[j][0]) + frO[j].t * double(D.d[j][1]) + frO[j].b * double(D.d[j][2]); const size_t i = size_t(q[j]);
      nd.d[i] = {float(w.dot(frN[i].n)), float(w.dot(frN[i].t)), float(w.dot(frN[i].b))}; }
    applyDetail(*CO, D.d); applyDetail(*CN, nd.d); out.push_back(std::move(nd)); O = CO; N = CN; pi = q;
  }
  return out;
}

}  // namespace

uint64_t topoSig(const PolyMesh& m) {
  uint64_t h = 1469598103934665603ull; auto mix = [&](uint64_t x) { h ^= x; h *= 1099511628211ull; };
  mix(m.pos.size()); mix(m.faceStart.size()); for (int v : m.faceStart) mix(uint64_t(v)); for (int v : m.faceVerts) mix(uint64_t(v)); return h;
}
QJsonArray packDetails(const std::vector<Detail>& v) {
  QJsonArray a; for (const Detail& d : v) { const QByteArray raw(reinterpret_cast<const char*>(d.d.data()), int(d.d.size() * sizeof(std::array<float, 3>)));
    a.append(QJsonObject{{"sig", QString::number(d.sig)}, {"n", int(d.d.size())}, {"d", QString::fromLatin1(qCompress(raw, 3).toBase64())}}); }
  return a;
}
std::vector<Detail> unpackDetails(const QJsonValue& v) {
  std::vector<Detail> out; for (const QJsonValue& x : v.toArray()) { const QJsonObject o = x.toObject(); const int n = o.value("n").toInt(); if (n <= 0) break;
    const QByteArray raw = qUncompress(QByteArray::fromBase64(o.value("d").toString().toLatin1())); if (raw.size() != int(n * sizeof(std::array<float, 3>))) break;
    Detail d; d.sig = o.value("sig").toString().toULongLong(); d.d.resize(size_t(n)); std::memcpy(d.d.data(), raw.constData(), size_t(raw.size())); out.push_back(std::move(d)); }
  return out;
}

Reconstruction reconstruct(const PolyMesh& H, const std::vector<Detail>& old, const Opts& o) {
  Reconstruction R; const int nQ = H.faceCount(), nV = int(H.pos.size());
  if (nQ == 0 || nV == 0) { R.why = "empty"; return R; }
  for (int q = 0; q < nQ; q++) if (H.faceStart[size_t(q) + 1] - H.faceStart[size_t(q)] != 4) { R.why = "notquads"; return R; }   // (a subdivision is quads only)
  QuadNet net; net.build(H);
  // the pieces of the mesh (a subdivision is labelled piece by piece: each may be a turn of the pattern of its own)
  std::vector<int> comp(static_cast<size_t>(nQ), -1); int nc = 0;
  for (int s = 0; s < nQ; s++) { if (comp[size_t(s)] >= 0) continue; std::vector<int> st{s}; comp[size_t(s)] = nc;
    while (!st.empty()) { const int x = st.back(); st.pop_back(); for (int i = 0; i < 4; i++) { const int a = H.faceVerts[size_t(H.faceStart[size_t(x)] + i)], b = H.faceVerts[size_t(H.faceStart[size_t(x)] + (i + 1) % 4)];
        auto it = net.adj.find(ekey(a, b)); if (it != net.adj.end()) for (int yj : it->second) if (comp[size_t(yj >> 2)] < 0) { comp[size_t(yj >> 2)] = nc; st.push_back(yj >> 2); } } }
    nc++; }
  std::vector<char> L(static_cast<size_t>(nV), 0); QString why = "nolabel";
  for (int c = 0; c < nc; c++) {
    // (the piece as a mesh of its own)
    PolyMesh sub; std::vector<int> vmap, back(static_cast<size_t>(nV), -1);
    for (int q = 0; q < nQ; q++) if (comp[size_t(q)] == c) { int v4[4]; for (int i = 0; i < 4; i++) { const int v = H.faceVerts[size_t(H.faceStart[size_t(q)] + i)]; if (back[size_t(v)] < 0) { back[size_t(v)] = int(vmap.size()); vmap.push_back(v); sub.pos.push_back(H.pos[size_t(v)]); } v4[i] = back[size_t(v)]; } sub.addFace(v4, 4); }
    QuadNet sn; sn.build(sub); double best = 1e300, ngonBest = 1e300; std::vector<char> bestL;
    for (int opt = 0; opt < 4; opt++) {
      std::vector<char> sl(sub.pos.size(), 0), seen(static_cast<size_t>(sub.faceCount()), 0);
      if (!labelFrom(sub, sn, 0, opt, sl, seen)) continue;
      bool all = true; for (char s : seen) if (!s) all = false; if (!all) continue;
      Built b = buildFromLabels(sub, sl, o, false);
      if (!b.ok) { if (b.why == "ngon") { why = "ngon"; const Built nb = buildFromLabels(sub, sl, o, false, true); if (nb.ok) ngonBest = std::min(ngonBest, nb.residual); } continue; }   // (the way that needs an n-gon: how well it would fit)
      if (b.residual < best) { best = b.residual; bestL = sl; } }
    // (the mesh is the subdivision of one with n-gons — those are not restored: another reading that fits worse (the dual one) is not taken instead)
    if (bestL.empty() || ngonBest <= best) { R.why = bestL.empty() ? why : QString("ngon"); return R; }
    for (size_t i = 0; i < vmap.size(); i++) L[size_t(vmap[i])] = bestL[i];
  }
  Built B = buildFromLabels(H, L, o, true);
  if (!B.ok) { R.why = B.why.isEmpty() ? "nolabel" : B.why; return R; }
  R.cage = B.cage; const uint64_t sig = topoSig(*B.cage);
  Detail d1; d1.sig = sig; d1.d = std::move(B.d); R.details.push_back(std::move(d1));
  // (what the mesh had as details before: now one level up)
  if (!old.empty() && old[0].sig == topoSig(H)) {
    auto N1 = std::make_shared<PolyMesh>(*B.C); applyDetail(*N1, R.details[0].d);
    auto more = restack(H, old, B.hToC, *N1, o, sig); for (auto& m : more) R.details.push_back(std::move(m)); }
  return R;
}

// ---------- the Subdivide tool: a region of the mesh, or all of it, split for good ----------
namespace {
std::shared_ptr<PolyMesh> regionStep(const PolyMesh& in, const std::vector<char>& picked, bool smooth, std::vector<char>& outPicked) {
  const int V = int(in.pos.size()), nF = in.faceCount();
  auto sz = [&](int f) { return in.faceStart[size_t(f) + 1] - in.faceStart[size_t(f)]; };
  std::vector<char> sel(static_cast<size_t>(nF), 0); for (int f = 0; f < nF; f++) sel[size_t(f)] = size_t(f) < picked.size() && picked[size_t(f)] && sz(f) >= 3;
  // edges, and which are split (an edge of a picked polygon)
  std::unordered_map<uint64_t, int> eid; std::vector<int> ea, eb, nf, f1, f2; std::vector<int> eoc(in.faceVerts.size(), -1);
  for (int f = 0; f < nF; f++) { if (sz(f) < 3) continue; const int s = in.faceStart[size_t(f)], e = in.faceStart[size_t(f) + 1];
    for (int q = s; q < e; q++) { const int a = in.faceVerts[size_t(q)], b = in.faceVerts[size_t(q + 1 < e ? q + 1 : s)]; auto it = eid.find(ekey(a, b)); int id;
      if (it == eid.end()) { id = int(ea.size()); eid.emplace(ekey(a, b), id); ea.push_back(a); eb.push_back(b); nf.push_back(0); f1.push_back(-1); f2.push_back(-1); } else id = it->second;
      eoc[size_t(q)] = id; if (nf[size_t(id)] == 0) f1[size_t(id)] = f; else if (nf[size_t(id)] == 1) f2[size_t(id)] = f; nf[size_t(id)]++; } }
  const int E = int(ea.size()); std::vector<char> split(static_cast<size_t>(E), 0), sharp(static_cast<size_t>(E), 0);
  std::unordered_map<uint64_t, float> cr; for (const auto& c : in.creases) if (c.w > 0) cr[ekey(c.a, c.b)] = std::max(c.w, cr.count(ekey(c.a, c.b)) ? cr[ekey(c.a, c.b)] : 0.f);
  for (int f = 0; f < nF; f++) if (sel[size_t(f)]) for (int q = in.faceStart[size_t(f)]; q < in.faceStart[size_t(f) + 1]; q++) split[size_t(eoc[size_t(q)])] = 1;
  for (int e = 0; e < E; e++) { sharp[size_t(e)] = nf[size_t(e)] != 2; if (!sharp[size_t(e)]) if (auto it = cr.find(ekey(ea[size_t(e)], eb[size_t(e)])); it != cr.end() && it->second >= 0.5f) sharp[size_t(e)] = 1; }
  // the new points: an edge point on each split edge, a point in each picked polygon
  std::vector<int> epIdx(static_cast<size_t>(E), -1), fpIdx(static_cast<size_t>(nF), -1); int nP = V;
  for (int e = 0; e < E; e++) if (split[size_t(e)]) epIdx[size_t(e)] = nP++;
  for (int f = 0; f < nF; f++) if (sel[size_t(f)]) fpIdx[size_t(f)] = nP++;
  std::vector<Vec3> pos(static_cast<size_t>(nP)); for (int v = 0; v < V; v++) pos[size_t(v)] = in.pos[size_t(v)];
  std::vector<Vec3> cen(static_cast<size_t>(nF));
  for (int f = 0; f < nF; f++) if (sel[size_t(f)]) { Vec3 c; const int s = in.faceStart[size_t(f)], e = in.faceStart[size_t(f) + 1]; for (int q = s; q < e; q++) c += in.pos[size_t(in.faceVerts[size_t(q)])]; cen[size_t(f)] = c * (1.0 / (e - s)); pos[size_t(fpIdx[size_t(f)])] = cen[size_t(f)]; }
  for (int e = 0; e < E; e++) if (split[size_t(e)]) { const Vec3 &a = in.pos[size_t(ea[size_t(e)])], &b = in.pos[size_t(eb[size_t(e)])];
    const bool inner = smooth && !sharp[size_t(e)] && f1[size_t(e)] >= 0 && f2[size_t(e)] >= 0 && sel[size_t(f1[size_t(e)])] && sel[size_t(f2[size_t(e)])];   // (inside the region: Catmull–Clark's edge point; on its border: the middle, as the polygon beyond gets it)
    pos[size_t(epIdx[size_t(e)])] = inner ? (a + b + cen[size_t(f1[size_t(e)])] + cen[size_t(f2[size_t(e)])]) * 0.25 : (a + b) * 0.5; }
  if (smooth) {                                                  // (the old points wholly inside the region: where the surface puts them; on its border they stay)
    std::vector<int> cnt(static_cast<size_t>(V), 0), scnt(static_cast<size_t>(V), 0); std::vector<std::vector<int>> vE(static_cast<size_t>(V)); std::vector<Vec3> fsum(static_cast<size_t>(V));
    for (int f = 0; f < nF; f++) { if (sz(f) < 3) continue; for (int q = in.faceStart[size_t(f)]; q < in.faceStart[size_t(f) + 1]; q++) { const size_t v = size_t(in.faceVerts[size_t(q)]); cnt[v]++; if (sel[size_t(f)]) { scnt[v]++; fsum[v] += cen[size_t(f)]; } } }
    for (int e = 0; e < E; e++) { vE[size_t(ea[size_t(e)])].push_back(e); vE[size_t(eb[size_t(e)])].push_back(e); }
    for (int v = 0; v < V; v++) { if (cnt[size_t(v)] == 0 || cnt[size_t(v)] != scnt[size_t(v)]) continue; const auto& es = vE[size_t(v)]; const int n = int(es.size()); const Vec3& P = in.pos[size_t(v)];
      int s = 0; int o[2] = {-1, -1}; Vec3 R; for (int e : es) { const int other = ea[size_t(e)] == v ? eb[size_t(e)] : ea[size_t(e)]; R += (P + in.pos[size_t(other)]) * 0.5; if (sharp[size_t(e)]) { if (s < 2) o[s] = other; s++; } }
      if (s == 0) { if (n < 3 || cnt[size_t(v)] != n) continue; const Vec3 Q = fsum[size_t(v)] * (1.0 / cnt[size_t(v)]); pos[size_t(v)] = (Q + R * (2.0 / n) + P * double(n - 3)) * (1.0 / n); }
      else if (s == 2) pos[size_t(v)] = P * 0.75 + (in.pos[size_t(o[0])] + in.pos[size_t(o[1])]) * 0.125; } }
  // the polygons: a picked one into a quad at each corner, the others with the new points of their split edges in them
  auto out = std::make_shared<PolyMesh>(); PolyMesh& P = *out; P.pos = std::move(pos); std::vector<int> src; outPicked.clear();
  struct Cn { int a, b; };                                      // (a corner of the new polygon: of the source corner a (b < 0), or the middle of corners a and b, or (a = −1) of the whole polygon b)
  std::vector<Cn> cn; const bool hasUV = in.cornerUV.size() == in.faceVerts.size() && !in.faceVerts.empty(), hasN = in.cornerNormal.size() == in.faceVerts.size() && !in.cornerNormal.empty();
  for (int f = 0; f < nF; f++) { const int s = in.faceStart[size_t(f)], e = in.faceStart[size_t(f) + 1], k = e - s;
    auto nx = [&](int q) { return q + 1 < e ? q + 1 : s; };
    if (sel[size_t(f)]) { for (int q = s; q < e; q++) { const int qp = q - 1 >= s ? q - 1 : e - 1;
        const int v4[4] = {in.faceVerts[size_t(q)], epIdx[size_t(eoc[size_t(q)])], fpIdx[size_t(f)], epIdx[size_t(eoc[size_t(qp)])]}; P.addFace(v4, 4); src.push_back(f); outPicked.push_back(1);
        cn.push_back({q, -1}); cn.push_back({q, nx(q)}); cn.push_back({-1, f}); cn.push_back({qp, q}); } }
    else { std::vector<int> fv; for (int q = s; q < e; q++) { fv.push_back(in.faceVerts[size_t(q)]); cn.push_back({q, -1});
        if (k >= 3 && split[size_t(eoc[size_t(q)])]) { fv.push_back(epIdx[size_t(eoc[size_t(q)])]); cn.push_back({q, nx(q)}); } }
      P.addFace(fv.data(), int(fv.size())); src.push_back(f); outPicked.push_back(0); } }
  const int nO = P.faceCount();
  auto per = [&](const auto& s, auto& d) { if (s.size() == size_t(nF)) { d.reserve(size_t(nO)); for (int i = 0; i < nO; i++) d.push_back(s[size_t(src[size_t(i)])]); } };
  per(in.faceGroup, P.faceGroup); per(in.faceMat, P.faceMat); per(in.faceSets, P.faceSets); per(in.faceLive, P.faceLive);
  P.groupNames = in.groupNames; P.matNames = in.matNames; P.matMaps = in.matMaps; P.matColor = in.matColor; P.matAlpha = in.matAlpha; P.mtllib = in.mtllib; P.flipY = in.flipY; P.uvSetNames = in.uvSetNames;
  auto uvOut = [&](const std::vector<std::array<float, 2>>& u) { std::vector<std::array<float, 2>> o; o.reserve(cn.size());
    for (const Cn& c : cn) { if (c.a >= 0 && c.b < 0) o.push_back(u[size_t(c.a)]); else if (c.a >= 0) o.push_back({(u[size_t(c.a)][0] + u[size_t(c.b)][0]) * 0.5f, (u[size_t(c.a)][1] + u[size_t(c.b)][1]) * 0.5f});
      else { float x = 0, y = 0; const int s = in.faceStart[size_t(c.b)], e = in.faceStart[size_t(c.b) + 1]; for (int q = s; q < e; q++) { x += u[size_t(q)][0]; y += u[size_t(q)][1]; } o.push_back({x / (e - s), y / (e - s)}); } }
    return o; };
  if (hasUV) P.cornerUV = uvOut(in.cornerUV);
  for (const auto& s : in.setUV) { if (s.size() == in.faceVerts.size()) P.setUV.push_back(uvOut(s)); else P.setUV.emplace_back(); }
  if (hasN) { P.cornerNormal.reserve(cn.size()); auto nrm = [](Vec3 v, const std::array<float, 3>& fb) -> std::array<float, 3> { if (v.lengthSq() < 1e-12) return fb; v = v.normalized(); return {float(v.x), float(v.y), float(v.z)}; };
    auto N3 = [&](int q) { const auto& n = in.cornerNormal[size_t(q)]; return Vec3(n[0], n[1], n[2]); };
    for (const Cn& c : cn) { if (c.a >= 0 && c.b < 0) P.cornerNormal.push_back(in.cornerNormal[size_t(c.a)]); else if (c.a >= 0) P.cornerNormal.push_back(nrm(N3(c.a) + N3(c.b), in.cornerNormal[size_t(c.a)]));
      else { Vec3 a; for (int q = in.faceStart[size_t(c.b)]; q < in.faceStart[size_t(c.b) + 1]; q++) a += N3(q); P.cornerNormal.push_back(nrm(a, in.cornerNormal[size_t(in.faceStart[size_t(c.b)])])); } } }
  if (in.vcol.size() == in.pos.size()) { P.vcol = in.vcol; P.vcol.resize(P.pos.size());
    for (int e = 0; e < E; e++) if (split[size_t(e)]) for (int k = 0; k < 3; k++) P.vcol[size_t(epIdx[size_t(e)])][size_t(k)] = (in.vcol[size_t(ea[size_t(e)])][size_t(k)] + in.vcol[size_t(eb[size_t(e)])][size_t(k)]) * 0.5f;
    for (int f = 0; f < nF; f++) if (sel[size_t(f)]) { std::array<float, 3> a{0, 0, 0}; const int s = in.faceStart[size_t(f)], e = in.faceStart[size_t(f) + 1]; for (int q = s; q < e; q++) for (int k = 0; k < 3; k++) a[size_t(k)] += in.vcol[size_t(in.faceVerts[size_t(q)])][size_t(k)];
      for (int k = 0; k < 3; k++) P.vcol[size_t(fpIdx[size_t(f)])][size_t(k)] = a[size_t(k)] / float(e - s); } }
  // (creases, hard edges, seams: a split edge is two edges now)
  auto halve = [&](int a, int b, auto&& add1, auto&& add2) { auto it = eid.find(ekey(a, b)); if (it != eid.end() && split[size_t(it->second)]) { const int m = epIdx[size_t(it->second)]; add2(a, m); add2(m, b); } else add1(a, b); };
  for (const auto& c : in.creases) halve(c.a, c.b, [&](int a, int b) { P.creases.push_back({a, b, c.w}); }, [&](int a, int b) { P.creases.push_back({a, b, c.w}); });
  for (const auto& h : in.hardEdges) halve(h[0], h[1], [&](int a, int b) { P.hardEdges.push_back({a, b}); }, [&](int a, int b) { P.hardEdges.push_back({a, b}); });
  for (const auto& h : in.seams) halve(h[0], h[1], [&](int a, int b) { P.seams.push_back({a, b}); }, [&](int a, int b) { P.seams.push_back({a, b}); });
  return out;
}
}  // namespace

std::shared_ptr<PolyMesh> subdivideRegion(const PolyMesh& in, const std::vector<char>& picked, bool smooth, int levels, std::vector<int>* made, int* levelDone, long long maxFaces) {
  std::shared_ptr<PolyMesh> cur; std::vector<char> pk = picked; int done = 0;
  for (int l = 0; l < levels; l++) {
    const PolyMesh& src = cur ? *cur : in; long long next = 0;
    for (int f = 0; f < src.faceCount(); f++) { const int k = src.faceStart[size_t(f) + 1] - src.faceStart[size_t(f)]; next += (size_t(f) < pk.size() && pk[size_t(f)] && k >= 3) ? k : 1; }
    if (next > maxFaces) break;
    std::vector<char> np; cur = regionStep(src, pk, smooth, np); pk = std::move(np); done++;
  }
  if (levelDone) *levelDone = done;
  if (made && cur) { made->clear(); for (size_t i = 0; i < pk.size(); i++) if (pk[i]) made->push_back(int(i)); }
  return cur;
}

Opts optsOf(const QJsonObject& r) {
  Opts o; o.level = std::clamp(r.value("level").toInt(1), 0, 6); o.smoothOpen = r.value("smoothOpen").toBool(true); o.uvSmooth = r.value("uv").toString("smooth") != "keep"; return o;
}

long long facesAfter(const PolyMesh& in, int level) {
  long long n = 0; for (int f = 0; f < in.faceCount(); f++) { const int k = in.faceStart[size_t(f) + 1] - in.faceStart[size_t(f)]; if (k >= 3) n += k; }   // (the first level: a quad per corner)
  if (level <= 0) return in.faceCount();
  for (int l = 1; l < level; l++) n *= 4;
  return n;
}

std::shared_ptr<PolyMesh> run(const PolyMesh& in, const Opts& o, int* levelDone, long long maxFaces, const std::vector<Detail>* details) {
  int done = 0; std::shared_ptr<PolyMesh> cur; const uint64_t sig = details && !details->empty() ? topoSig(in) : 0;
  for (int l = 0; l < o.level; l++) {
    const PolyMesh& src = cur ? *cur : in; long long next = 0;
    for (int f = 0; f < src.faceCount(); f++) { const int k = src.faceStart[size_t(f) + 1] - src.faceStart[size_t(f)]; if (k >= 3) next += k; }
    if (next > maxFaces) break;
    cur = step(src, o); done++;
    if (details && size_t(l) < details->size() && (*details)[size_t(l)].sig == sig && (*details)[size_t(l)].d.size() == cur->pos.size()) applyDetail(*cur, (*details)[size_t(l)].d);   // (the level's own shape)
  }
  if (levelDone) *levelDone = done;
  return cur;
}

}  // namespace hm::subdiv
