#include "view/SceneGeometry.h"
#include "scene/CurveMath.h"
#include <QJsonObject>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace hm {

namespace {
float s2l(double c) { return float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)); }
void putLine(LineBatch& b, const Vec3& p, const Vec3& q, const float c[4]) {
  b.verts.push_back({{float(p.x), float(p.y), float(p.z)}, {c[0], c[1], c[2], c[3]}});
  b.verts.push_back({{float(q.x), float(q.y), float(q.z)}, {c[0], c[1], c[2], c[3]}});
}
}  // namespace

QColor linearColor(const QColor& c) { return QColor::fromRgbF(s2l(c.redF()), s2l(c.greenF()), s2l(c.blueF()), c.alphaF()); }

void toLinear(const QColor& c, float out[4], float alpha) {
  out[0] = s2l(c.redF()); out[1] = s2l(c.greenF()); out[2] = s2l(c.blueF()); out[3] = alpha;
}

void itemBaseColor(const Scene& sc, const Item& it, float out[4]) {
  QJsonObject view = sc.fileRaw.value("settings").toObject().value("view").toObject();
  QString model = view.value("modelColor").toString("#808080"), tint = view.value("oneColor").toBool() ? QString() : sc.tintOf(it);
  if (tint.isEmpty()) { toLinear(QColor(model), out); return; }
  float g[4], t[4]; toLinear(QColor(0x80, 0x80, 0x80), g); toLinear(QColor(tint), t);
  for (int i = 0; i < 3; i++) out[i] = g[i] + (t[i] - g[i]) * 0.5f;
  out[3] = 1;
}

namespace {
// each polygon edge once (shared by two polygons — the first side met): fn(corner, the next corner of its polygon);
// the edges gathered by their lower vertex (a table, no hashing — fast for millions)
template <class Fn> void eachEdgeOnce(const PolyMesh& m, Fn fn) {
  const size_t nc = m.faceVerts.size(), nv = m.pos.size(); const int nf = m.faceCount();
  std::vector<uint32_t> off(nv + 1, 0);
  auto nextOf = [&](int f, int k) { return k + 1 < m.faceStart[size_t(f) + 1] ? k + 1 : m.faceStart[size_t(f)]; };
  for (int f = 0; f < nf; f++) for (int k = m.faceStart[size_t(f)]; k < m.faceStart[size_t(f) + 1]; k++) { int a = m.faceVerts[size_t(k)], b = m.faceVerts[size_t(nextOf(f, k))]; if (a != b) off[size_t(std::min(a, b)) + 1]++; }
  for (size_t v = 0; v < nv; v++) off[v + 1] += off[v];
  std::vector<uint32_t> hi(off[nv]), from(off[nv]), at(off.begin(), off.end() - 1);
  for (int f = 0; f < nf; f++) for (int k = m.faceStart[size_t(f)]; k < m.faceStart[size_t(f) + 1]; k++) { int a = m.faceVerts[size_t(k)], b = m.faceVerts[size_t(nextOf(f, k))]; if (a == b) continue;
    uint32_t s = at[size_t(std::min(a, b))]++; hi[s] = uint32_t(std::max(a, b)); from[s] = uint32_t(k); }
  std::vector<int> faceOf; faceOf.resize(nc); for (int f = 0; f < nf; f++) for (int k = m.faceStart[size_t(f)]; k < m.faceStart[size_t(f) + 1]; k++) faceOf[size_t(k)] = f;
  for (size_t v = 0; v < nv; v++) for (uint32_t i = off[v]; i < off[v + 1]; i++) {
    bool dup = false; for (uint32_t j = off[v]; j < i && !dup; j++) dup = hi[j] == hi[i];
    if (!dup) fn(int(from[i]), nextOf(faceOf[from[i]], int(from[i])));
  }
}
}  // namespace

std::vector<Vec3> hardCornerNormals(const PolyMesh& m) {
  const int nf = m.faceCount(); const size_t nc = m.faceVerts.size(), nv = m.pos.size();
  std::vector<Vec3> fn(size_t(std::max(nf, 0))); std::vector<int> cf(nc);
  for (int f = 0; f < nf; f++) { int s = m.faceStart[f], e = m.faceStart[f + 1]; Vec3 n;
    for (int k = s + 1; k < e - 1; k++) { const Vec3 &a = m.pos[m.faceVerts[s]], &b = m.pos[m.faceVerts[k]], &c = m.pos[m.faceVerts[k + 1]]; n += (b - a).cross(c - a); }
    fn[size_t(f)] = n; for (int k = s; k < e; k++) cf[size_t(k)] = f; }
  auto key = [](int a, int b) { return (uint64_t(uint32_t(std::min(a, b))) << 32) | uint32_t(std::max(a, b)); };
  std::unordered_set<uint64_t> hard; for (auto& h : m.hardEdges) hard.insert(key(h[0], h[1]));
  // (each vertex's corners; the corners round it joined across the edges that are not hard)
  std::vector<int> off(nv + 1, 0), at(nc); for (size_t k = 0; k < nc; k++) off[size_t(m.faceVerts[k]) + 1]++;
  for (size_t v = 0; v < nv; v++) off[v + 1] += off[v];
  { std::vector<int> fill(off.begin(), off.end() - 1); for (size_t k = 0; k < nc; k++) at[size_t(fill[size_t(m.faceVerts[k])]++)] = int(k); }
  std::vector<int> up(nc); for (size_t k = 0; k < nc; k++) up[k] = int(k);
  std::function<int(int)> find = [&](int x) { while (up[size_t(x)] != x) x = up[size_t(x)] = up[size_t(up[size_t(x)])]; return x; };
  auto nbrs = [&](int k, int& prev, int& next) { const int f = cf[size_t(k)], s = m.faceStart[f], e = m.faceStart[f + 1];
    prev = m.faceVerts[size_t(k > s ? k - 1 : e - 1)]; next = m.faceVerts[size_t(k + 1 < e ? k + 1 : s)]; };
  for (size_t v = 0; v < nv; v++) {
    std::unordered_map<int, int> byEdge;                          // (the other end of an edge → the first corner on it)
    for (int i = off[v]; i < off[v + 1]; i++) { const int k = at[size_t(i)]; int p, q; nbrs(k, p, q);
      for (int w : {p, q}) { if (hard.count(key(int(v), w))) continue; auto it = byEdge.find(w); if (it == byEdge.end()) byEdge.emplace(w, k); else { int a = find(k), b = find(it->second); if (a != b) up[size_t(a)] = b; } } }
  }
  std::vector<Vec3> sum(nc); for (size_t k = 0; k < nc; k++) sum[size_t(find(int(k)))] += fn[size_t(cf[k])];
  std::vector<Vec3> out(nc); for (size_t k = 0; k < nc; k++) out[k] = sum[size_t(find(int(k)))].normalized();
  return out;
}

MeshDraw buildMeshDraw(const PolyMesh& m, bool withWire, bool flat, const std::vector<std::array<float, 3>>* tintIn) {
  MeshDraw d;
  const std::vector<std::array<float, 3>>& TC = tintIn && tintIn->size() == m.pos.size() ? *tintIn : m.col;
  const bool tint = TC.size() == m.pos.size();
  const int nf = m.faceCount(); const size_t nc = m.faceVerts.size();
  d.tris.reserve((nc - size_t(std::max(nf, 0)) * 2) * 3);
  // (smooth, but the file's own normals or hard edges: each corner its own normal)
  const bool own = !flat && m.cornerNormal.size() == nc && nc > 0;
  const bool hardSplit = !flat && (own || !m.hardEdges.empty());
  std::vector<Vec3> hcn = hardSplit ? hardCornerNormals(m) : std::vector<Vec3>{};
  if (own) for (size_t k = 0; k < nc; k++) { const auto& o = m.cornerNormal[k]; const Vec3 v(o[0], o[1], o[2]); if (v.lengthSq() > 1e-12) hcn[k] = v.normalized(); }
  if (flat || hardSplit) {                                       // (flat per polygon / split at the hard edges: every corner its own vertex — its index the corner's)
    d.verts.resize(nc);
    for (int f = 0; f < nf; f++) {
      int s = m.faceStart[f], e = m.faceStart[f + 1]; Vec3 n;
      for (int k = s + 1; k < e - 1; k++) { const Vec3 &a = m.pos[m.faceVerts[s]], &b = m.pos[m.faceVerts[k]], &c = m.pos[m.faceVerts[k + 1]]; n += (b - a).cross(c - a); }
      n = n.normalized();
      if (hardSplit) for (int k = s; k < e; k++) { int v = m.faceVerts[k]; const Vec3& cn = hcn[size_t(k)]; MeshVertex& mv = d.verts[size_t(k)]; mv = {{float(m.pos[v].x), float(m.pos[v].y), float(m.pos[v].z)}, {float(cn.x), float(cn.y), float(cn.z)}};
        if (tint) { mv.c[0] = TC[v][0]; mv.c[1] = TC[v][1]; mv.c[2] = TC[v][2]; }
        if (own) { mv.g[0] = float(n.x); mv.g[1] = float(n.y); mv.g[2] = float(n.z); } }   // (a normal of the file's: the polygon's own kept beside it — the PDO goes by the polygon's side)
      if (hardSplit) { for (int k = 1; k < e - s - 1; k++) { d.tris.push_back(uint32_t(s)); d.tris.push_back(uint32_t(s + k)); d.tris.push_back(uint32_t(s + k + 1)); } continue; }
      for (int k = s; k < e; k++) { int v = m.faceVerts[k]; MeshVertex& mv = d.verts[size_t(k)]; mv = {{float(m.pos[v].x), float(m.pos[v].y), float(m.pos[v].z)}, {float(n.x), float(n.y), float(n.z)}};
        if (tint) { mv.c[0] = TC[v][0]; mv.c[1] = TC[v][1]; mv.c[2] = TC[v][2]; } }
      for (int k = 1; k < e - s - 1; k++) { d.tris.push_back(uint32_t(s)); d.tris.push_back(uint32_t(s + k)); d.tris.push_back(uint32_t(s + k + 1)); }
    }
    if (withWire) { d.wire.reserve(nc); eachEdgeOnce(m, [&](int k, int nk) { d.wire.push_back(uint32_t(k)); d.wire.push_back(uint32_t(nk)); }); }
    return d;
  }
  size_t nv = m.pos.size();
  d.verts.resize(nv);
  std::vector<Vec3> nrm(nv);
  for (int f = 0; f < nf; f++) {
    int s = m.faceStart[f], e = m.faceStart[f + 1];
    for (int k = s + 1; k < e - 1; k++) {                        // a fan per polygon
      int a = m.faceVerts[s], b = m.faceVerts[k], c = m.faceVerts[k + 1];
      d.tris.push_back(uint32_t(a)); d.tris.push_back(uint32_t(b)); d.tris.push_back(uint32_t(c));
      Vec3 n = (m.pos[b] - m.pos[a]).cross(m.pos[c] - m.pos[a]);  // area-weighted, as computeVertexNormals
      nrm[a] += n; nrm[b] += n; nrm[c] += n;
    }
  }
  for (size_t i = 0; i < nv; i++) {
    Vec3 n = nrm[i].normalized();
    d.verts[i] = {{float(m.pos[i].x), float(m.pos[i].y), float(m.pos[i].z)}, {float(n.x), float(n.y), float(n.z)}};
    if (tint) { d.verts[i].c[0] = TC[i][0]; d.verts[i].c[1] = TC[i][1]; d.verts[i].c[2] = TC[i][2]; }
  }
  if (withWire) { d.wire.reserve(nc); eachEdgeOnce(m, [&](int k, int nk) { d.wire.push_back(uint32_t(m.faceVerts[size_t(k)])); d.wire.push_back(uint32_t(m.faceVerts[size_t(nk)])); }); }
  return d;
}
TexDraw buildTexDraw(const PolyMesh& m, const std::vector<TexSet>& sets, bool flat) {
  TexDraw d; d.bySet.resize(std::max<size_t>(1, sets.size()));
  if (m.cornerUV.size() != m.faceVerts.size()) return d;
  std::unordered_map<int, size_t> setOf; for (size_t i = 0; i < sets.size(); i++) setOf.emplace(sets[i].mat, i);
  const int nf = m.faceCount();
  std::vector<Vec3> vn; if (!flat) vn.assign(m.pos.size(), Vec3{});
  std::vector<Vec3> fn(size_t(std::max(nf, 0)));
  for (int f = 0; f < nf; f++) {
    int s = m.faceStart[f], e = m.faceStart[f + 1]; Vec3 n;
    for (int k = s + 1; k < e - 1; k++) { const Vec3 &a = m.pos[m.faceVerts[s]], &b = m.pos[m.faceVerts[k]], &c = m.pos[m.faceVerts[k + 1]]; Vec3 x = (b - a).cross(c - a); n += x;
      if (!flat) { vn[size_t(m.faceVerts[s])] += x; vn[size_t(m.faceVerts[k])] += x; vn[size_t(m.faceVerts[k + 1])] += x; } }
    fn[size_t(f)] = n.normalized();
  }
  if (!flat) for (auto& n : vn) n = n.normalized();
  const bool own = !flat && m.cornerNormal.size() == m.faceVerts.size() && !m.faceVerts.empty();
  std::vector<Vec3> hcn = !flat && (own || !m.hardEdges.empty()) ? hardCornerNormals(m) : std::vector<Vec3>{};   // (the hard edges kept sharp; the file's own normals)
  if (own) for (size_t k = 0; k < hcn.size(); k++) { const auto& o = m.cornerNormal[k]; const Vec3 v(o[0], o[1], o[2]); if (v.lengthSq() > 1e-12) hcn[k] = v.normalized(); }
  d.verts.reserve(m.faceVerts.size());
  for (int f = 0; f < nf; f++) {
    int s = m.faceStart[f], e = m.faceStart[f + 1];
    int mat = size_t(f) < m.faceMat.size() ? m.faceMat[size_t(f)] : -1; auto si = setOf.find(mat); size_t set = si != setOf.end() ? si->second : 0;
    uint32_t o = uint32_t(d.verts.size());
    for (int k = s; k < e; k++) {
      int v = m.faceVerts[k]; const Vec3& p = m.pos[size_t(v)]; const Vec3& n = flat ? fn[size_t(f)] : !hcn.empty() ? hcn[size_t(k)] : vn[size_t(v)];
      const auto& uv = m.cornerUV[size_t(k)];
      d.verts.push_back({{float(p.x), float(p.y), float(p.z)}, {float(n.x), float(n.y), float(n.z)}, {uv[0], m.flipY ? 1 - uv[1] : uv[1]}});
    }
    auto& T = d.bySet[set];
    for (int k = 1; k < e - s - 1; k++) { T.push_back(o); T.push_back(o + uint32_t(k)); T.push_back(o + uint32_t(k + 1)); }
  }
  return d;
}

// THREE.GridHelper(scale * 8, 32, 0x4a4a4a, 0x363636) under the model
LineBatch buildFloor(double scale, const Vec3& center, double floorY, double opacity) {
  LineBatch b; b.width = 1; if (opacity <= 0.01) return b;
  float cMain[4], cGrid[4]; toLinear(QColor(0x4a, 0x4a, 0x4a), cMain, float(opacity)); toLinear(QColor(0x36, 0x36, 0x36), cGrid, float(opacity));
  const int div = 32; double size = scale * 8, half = size / 2, step = size / div;
  for (int i = 0; i <= div; i++) {
    double k = -half + i * step; const float* c = i == div / 2 ? cMain : cGrid;
    putLine(b, center + Vec3(-half, 0, k) + Vec3(0, floorY - center.y, 0), center + Vec3(half, 0, k) + Vec3(0, floorY - center.y, 0), c);
    putLine(b, center + Vec3(k, 0, -half) + Vec3(0, floorY - center.y, 0), center + Vec3(k, 0, half) + Vec3(0, floorY - center.y, 0), c);
  }
  return b;
}

// the world axes through the origin (X red, Y green, Z blue), as long as the floor is wide; opacity — the slider next to
// Floor (0.6 by default, 0 — off)
LineBatch buildAxes(double scale, double opacity) {
  LineBatch b; b.width = 1; if (opacity <= 0.01) return b;
  float x[4], y[4], z[4], o = float(opacity);
  toLinear(QColor::fromRgbF(0.88f, 0.3f, 0.3f), x, o); toLinear(QColor::fromRgbF(0.38f, 0.8f, 0.35f), y, o);
  toLinear(QColor::fromRgbF(0.33f, 0.52f, 1.f), z, o);
  double L = scale * 4;
  putLine(b, {-L, 0, 0}, {L, 0, 0}, x); putLine(b, {0, -L, 0}, {0, L, 0}, y); putLine(b, {0, 0, -L}, {0, 0, L}, z);
  return b;
}

}  // namespace hm

namespace hm {
LineBatch buildViewGrid(const QString& view, double scale, const Vec3& center, double floorY, double step) {
  LineBatch b; b.width = 1; if (step <= 0) return b;
  float c[4], c10[4]; toLinear(QColor(0x5a, 0x6a, 0x7a), c, 0.55f); toLinear(QColor(0x70, 0x88, 0xa0), c10, 0.8f);
  const double half = scale * 4; int n = int(std::ceil(half / step)); if (n > 200) { n = 200; }   // (no more than 400 lines a way)
  const bool wallZ = view == "front" || view == "back", wallX = view == "left" || view == "right";
  // (the lines on whole steps, round the scene's centre)
  auto at = [&](double u, double v) { return wallZ ? Vec3(u, v, 0) : wallX ? Vec3(0, v, u) : Vec3(u, floorY, v); };
  const double cu = std::round((wallX ? center.z : center.x) / step) * step, cv = std::round((wallZ || wallX ? center.y : center.z) / step) * step;
  for (int i = -n; i <= n; i++) { const float* k = i % 10 == 0 ? c10 : c; const double u = cu + i * step, v = cv + i * step;
    putLine(b, at(u, cv - n * step), at(u, cv + n * step), k); putLine(b, at(cu - n * step, v), at(cu + n * step, v), k); }
  return b;
}
}  // namespace hm
