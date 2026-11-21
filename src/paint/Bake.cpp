#include "paint/Bake.h"
#include "scene/Scene.h"
#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <thread>

namespace hm::paint {

namespace {
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 norm(V3 a) { const float l = len(a); return l > 1e-12f ? a * (1 / l) : V3{0, 0, 1}; }
inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// ---- BVH over the high-poly's triangles ----
struct Bvh {
  struct Node { V3 lo, hi; int a = 0, n = 0; };   // n > 0: a leaf over order[a…a+n); else children a, a+1
  const TriMesh* m = nullptr; std::vector<Node> nodes; std::vector<int> order;
  void build(const TriMesh& mesh) {
    m = &mesh; order.resize(mesh.tri.size()); for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
    std::vector<V3> cen(mesh.tri.size());
    for (size_t i = 0; i < cen.size(); i++) { const auto& t = mesh.tri[i]; cen[i] = (mesh.pos[t[0]] + mesh.pos[t[1]] + mesh.pos[t[2]]) * (1.f / 3); }
    nodes.reserve(mesh.tri.size() / 2 + 2); nodes.emplace_back(); split(0, 0, int(order.size()), cen);
  }
  void bounds(int a, int n, V3& lo, V3& hi) const {
    lo = {1e30f, 1e30f, 1e30f}; hi = {-1e30f, -1e30f, -1e30f};
    for (int i = a; i < a + n; i++) for (int k : m->tri[order[i]]) { const V3& p = m->pos[k]; lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}; hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)}; }
  }
  void split(int ni, int a, int n, const std::vector<V3>& cen) {
    V3 lo, hi; bounds(a, n, lo, hi); nodes[ni].lo = lo; nodes[ni].hi = hi;
    if (n <= 4) { nodes[ni].a = a; nodes[ni].n = n; return; }
    const V3 e = hi - lo; const int ax = (e.x > e.y && e.x > e.z) ? 0 : (e.y > e.z ? 1 : 2);
    auto key = [&](int t) { const V3& c = cen[t]; return ax == 0 ? c.x : ax == 1 ? c.y : c.z; };
    const int mid = n / 2;
    std::nth_element(order.begin() + a, order.begin() + a + mid, order.begin() + a + n, [&](int p, int q) { return key(p) < key(q); });
    const int l = int(nodes.size()); nodes.emplace_back(); nodes.emplace_back(); nodes[ni].a = l; nodes[ni].n = 0;
    split(l, a, mid, cen); split(l + 1, a + mid, n - mid, cen);
  }
  struct Hit { float t = 1e30f; int tri = -1; float u = 0, v = 0; };
  static bool tri_(const V3& o, const V3& d, const V3& a, const V3& b, const V3& c, float tmax, float& t, float& u, float& v) {
    const V3 e1 = b - a, e2 = c - a, p = cross(d, e2); const float det = dot(e1, p); if (std::fabs(det) < 1e-14f) return false;
    const float inv = 1 / det; const V3 s = o - a; u = dot(s, p) * inv; if (u < 0 || u > 1) return false;
    const V3 q = cross(s, e1); v = dot(d, q) * inv; if (v < 0 || u + v > 1) return false;
    t = dot(e2, q) * inv; return t > 1e-7f && t < tmax;
  }
  static bool box(const Node& n, const V3& o, const V3& inv, float tmax) {
    float t0 = 0, t1 = tmax;
    const float lo[3] = {n.lo.x, n.lo.y, n.lo.z}, hi[3] = {n.hi.x, n.hi.y, n.hi.z}, oo[3] = {o.x, o.y, o.z}, ii[3] = {inv.x, inv.y, inv.z};
    for (int k = 0; k < 3; k++) { float a = (lo[k] - oo[k]) * ii[k], b = (hi[k] - oo[k]) * ii[k]; if (a > b) std::swap(a, b); t0 = std::max(t0, a); t1 = std::min(t1, b); if (t0 > t1) return false; }
    return true;
  }
  Hit closest(const V3& o, const V3& d, float tmax, bool any = false) const {
    Hit h; h.t = tmax; if (nodes.empty()) return h;
    const V3 inv{std::fabs(d.x) > 1e-20f ? 1 / d.x : 1e20f, std::fabs(d.y) > 1e-20f ? 1 / d.y : 1e20f, std::fabs(d.z) > 1e-20f ? 1 / d.z : 1e20f};
    int stack[96]; int sp = 0; stack[sp++] = 0;
    while (sp) {
      const Node& n = nodes[stack[--sp]]; if (!box(n, o, inv, h.t)) continue;
      if (n.n) { for (int i = n.a; i < n.a + n.n; i++) { const int ti = order[i]; const auto& t = m->tri[ti]; float tt, u, v;
          if (tri_(o, d, m->pos[t[0]], m->pos[t[1]], m->pos[t[2]], h.t, tt, u, v)) { h.t = tt; h.tri = ti; h.u = u; h.v = v; if (any) return h; } } }
      else if (sp < 94) { stack[sp++] = n.a; stack[sp++] = n.a + 1; }
    }
    return h;
  }
};

V3 hitNormal(const TriMesh& m, const Bvh::Hit& h) { const auto& n = m.nrm[h.tri]; return norm(n[0] * (1 - h.u - h.v) + n[1] * h.u + n[2] * h.v); }

float hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return (x & 0xffffff) / 16777216.f; }
}  // namespace

TriMesh triMeshFrom(const hm::PolyMesh& m) {
  TriMesh out; out.pos.reserve(m.pos.size());
  for (const auto& p : m.pos) out.pos.push_back({float(p.x), float(p.y), float(p.z)});
  out.flipV = m.flipY;
  if (m.vcol.size() == m.pos.size()) { out.col.reserve(m.vcol.size()); for (const auto& c : m.vcol) out.col.push_back({c[0], c[1], c[2]}); }
  const int F = m.faceCount(); const bool uvs = m.cornerUV.size() == m.faceVerts.size() && !m.cornerUV.empty();
  const bool cn = m.cornerNormal.size() == m.faceVerts.size() && !m.cornerNormal.empty();
  std::set<std::pair<int, int>> hard; for (auto& e : m.hardEdges) hard.insert({std::min(e[0], e[1]), std::max(e[0], e[1])});
  std::vector<V3> vn(out.pos.size());
  for (int f = 0; f < F; f++) {   // (area-weighted: a polygon's normal summed from the cross products of its fan)
    const int a = m.faceStart[f], n = m.faceStart[f + 1] - a; V3 fn;
    for (int i = 1; i + 1 < n; i++) fn = fn + cross(out.pos[m.faceVerts[a + i]] - out.pos[m.faceVerts[a]], out.pos[m.faceVerts[a + i + 1]] - out.pos[m.faceVerts[a]]);
    for (int i = 0; i < n; i++) vn[m.faceVerts[a + i]] = vn[m.faceVerts[a + i]] + fn;
  }
  for (auto& v : vn) v = norm(v);
  for (int f = 0; f < F; f++) {
    const int a = m.faceStart[f], n = m.faceStart[f + 1] - a; if (n < 3) continue;
    V3 fn; for (int i = 1; i + 1 < n; i++) fn = fn + cross(out.pos[m.faceVerts[a + i]] - out.pos[m.faceVerts[a]], out.pos[m.faceVerts[a + i + 1]] - out.pos[m.faceVerts[a]]); fn = norm(fn);
    auto cornerN = [&](int i) -> V3 {
      if (cn) { const auto& c = m.cornerNormal[a + i]; return norm({c[0], c[1], c[2]}); }
      const int v = m.faceVerts[a + i], pv = m.faceVerts[a + (i + n - 1) % n], nx = m.faceVerts[a + (i + 1) % n];
      if (hard.count({std::min(v, pv), std::max(v, pv)}) || hard.count({std::min(v, nx), std::max(v, nx)})) return fn;
      return vn[v];
    };
    for (int i = 1; i + 1 < n; i++) {
      const int c[3] = {0, i, i + 1}; std::array<int, 3> t; std::array<V3, 3> nr; std::array<V2, 3> uv{};
      for (int k = 0; k < 3; k++) { t[k] = m.faceVerts[a + c[k]]; nr[k] = cornerN(c[k]); if (uvs) { const auto& u = m.cornerUV[a + c[k]]; uv[k] = {u[0], u[1]}; } }
      out.tri.push_back(t); out.nrm.push_back(nr); if (uvs) out.uv.push_back(uv);
    }
  }
  return out;
}

BakeResult bake(const TriMesh& low, const TriMesh& high, const BakeSettings& s, const std::function<bool(float)>& progress) {
  BakeResult R; QElapsedTimer clock; clock.start();
  if (low.empty() || low.uv.size() != low.tri.size()) { R.log = "the low-poly has no UVs"; return R; }
  if (high.empty()) { R.log = "the high-poly is empty"; return R; }
  const int N = std::clamp(s.size, 16, 8192);
  V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (const V3& p : high.pos) { lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}; hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)}; }
  const float diag = std::max(len(hi - lo), 1e-6f), cage = s.cage * diag, back = s.back * diag, aoDist = s.aoDist * diag;
  Bvh bvh; bvh.build(high);
  const QImage msk = s.mask.isNull() ? QImage() : s.mask.convertToFormat(QImage::Format_Grayscale8); const bool hasMask = !msk.isNull();

  const size_t NP = size_t(N) * N;
  std::vector<float> nx(NP, 0), ny(NP, 0), nz(NP, 0), ht(NP, 0.5f), ao(NP, 1), px(NP), py(NP), pz(NP), cnt(NP, 0);
  const bool wantCol = s.colour && high.col.size() == high.pos.size();
  std::vector<float> cr(wantCol ? NP : 0, 0), cg(wantCol ? NP : 0, 0), cb(wantCol ? NP : 0, 0);
  std::atomic<int> next{0}, done{0}; std::atomic<bool> stop{false};
  std::array<std::mutex, 64> rowLock;                              // (the texel sums: one lock per row stripe)
  const int T = int(low.tri.size()), SS = std::clamp(s.samples, 1, 2);
  auto worker = [&] {
    for (;;) {
      const int ti = next.fetch_add(1); if (ti >= T || stop) break;
      const auto& tr = low.tri[ti]; const auto& uv = low.uv[ti]; const auto& nr = low.nrm[ti];
      const V3 p0 = low.pos[tr[0]], p1 = low.pos[tr[1]], p2 = low.pos[tr[2]];
      // the UV → position frame: T along u, B along v
      const float du1 = uv[1].x - uv[0].x, dv1 = uv[1].y - uv[0].y, du2 = uv[2].x - uv[0].x, dv2 = uv[2].y - uv[0].y;
      const float det = du1 * dv2 - du2 * dv1; if (std::fabs(det) < 1e-12f) { done++; continue; }
      const V3 e1 = p1 - p0, e2 = p2 - p0; const float r = 1 / det;
      const V3 Tt = (e1 * dv2 - e2 * dv1) * r, Bt = (e2 * du1 - e1 * du2) * r;
      const float minx = std::min({uv[0].x, uv[1].x, uv[2].x}) * N, maxx = std::max({uv[0].x, uv[1].x, uv[2].x}) * N;
      const float miny = std::min({uv[0].y, uv[1].y, uv[2].y}) * N, maxy = std::max({uv[0].y, uv[1].y, uv[2].y}) * N;
      const int x0 = std::max(0, int(std::floor(minx))), x1 = std::min(N - 1, int(std::ceil(maxx))), y0 = std::max(0, int(std::floor(miny))), y1 = std::min(N - 1, int(std::ceil(maxy)));
      for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        if (hasMask && !msk.constScanLine(std::min(msk.height() - 1, (low.flipV ? N - 1 - y : y) * msk.height() / N))[std::min(msk.width() - 1, x * msk.width() / N)]) continue;   // (a partial bake: not asked for here)
        for (int sy = 0; sy < SS; sy++) for (int sx = 0; sx < SS; sx++) {
          const float u = (x + (sx + 0.5f) / SS) / N, v = (y + (sy + 0.5f) / SS) / N;   // (the image's row 0 is v = 0: the exporter's flip is the caller's)
          const float w1 = ((u - uv[0].x) * dv2 - (v - uv[0].y) * du2) / det, w2 = ((v - uv[0].y) * du1 - (u - uv[0].x) * dv1) / det, w0 = 1 - w1 - w2;
          if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
          const V3 P = p0 * w0 + p1 * w1 + p2 * w2, Nn = norm(nr[0] * w0 + nr[1] * w1 + nr[2] * w2);
          const V3 o = P + Nn * cage; const Bvh::Hit h = bvh.closest(o, Nn * -1.f, cage + back);
          if (h.tri < 0) continue;
          const size_t id = size_t(y) * N + x; V3 hn = hitNormal(high, h);
          V3 Tn = norm(Tt - Nn * dot(Nn, Tt)); V3 Bn = cross(Nn, Tn); if (dot(Bn, Bt) < 0) Bn = Bn * -1.f;
          const auto& ht3 = high.tri[h.tri]; const float hw = 1 - h.u - h.v;
          const V3 HP = high.pos[ht3[0]] * hw + high.pos[ht3[1]] * h.u + high.pos[ht3[2]] * h.v;
          float aoAdd = 0;
          if (s.ao && s.aoRays > 0) {
            // (cosine-weighted rays about the high surface's normal; a fixed pseudo-random pattern per texel)
            V3 hnn = dot(hn, Nn) < 0 ? hn * -1.f : hn; const V3 up = std::fabs(hnn.z) < 0.9f ? V3{0, 0, 1} : V3{1, 0, 0}; const V3 t1 = norm(cross(up, hnn)), t2 = cross(hnn, t1);
            int occ = 0; const V3 ho = HP + hnn * (diag * 1e-5f);
            for (int k = 0; k < s.aoRays; k++) {
              const float a = hash(uint32_t(id * 31 + k * 7919)), b = hash(uint32_t(id * 17 + k * 104729 + 3)); const float rr = std::sqrt(a), ph = 6.2831853f * b;
              const V3 d = norm(t1 * (rr * std::cos(ph)) + t2 * (rr * std::sin(ph)) + hnn * std::sqrt(std::max(0.f, 1 - a)));
              if (bvh.closest(ho, d, aoDist, true).tri >= 0) occ++;
            }
            aoAdd = (1.f - float(occ) / s.aoRays) - 1.f;
          }
          // (the sums of a texel: two threads work on the two triangles of a quad at once and both reach the texels on its diagonal —
          //  added under the lock of the texel's row, the rays above cast outside it)
          std::lock_guard<std::mutex> lk(rowLock[size_t(y) % rowLock.size()]);
          nx[id] += dot(hn, Tn); ny[id] += dot(hn, Bn) * (s.flipGreen ? -1.f : 1.f); nz[id] += dot(hn, Nn);
          ht[id] += clamp01(0.5f + (cage - h.t) / (cage + back) * 0.5f) - 0.5f;   // (0.5 at the low surface)
          if (wantCol) { const V3 C3 = high.col[size_t(ht3[0])] * hw + high.col[size_t(ht3[1])] * h.u + high.col[size_t(ht3[2])] * h.v; cr[id] += C3.x; cg[id] += C3.y; cb[id] += C3.z; }
          px[id] += (HP.x - lo.x) / (hi.x - lo.x + 1e-9f); py[id] += (HP.y - lo.y) / (hi.y - lo.y + 1e-9f); pz[id] += (HP.z - lo.z) / (hi.z - lo.z + 1e-9f);
          ao[id] += aoAdd;
          cnt[id] += 1;
        }
      }
      const int d = ++done; if (progress && (d & 63) == 0 && !progress(float(d) / T)) stop = true;
    }
  };
  const int nt = std::max(1u, std::thread::hardware_concurrency());
  { std::vector<std::thread> th; for (int i = 0; i < nt; i++) th.emplace_back(worker); for (auto& t : th) t.join(); }
  if (stop) { R.log = "stopped"; return R; }

  // averages, the coverage; the maps' empty texels left for the padding
  std::vector<char> cov(NP, 0); int hit = 0;
  for (size_t i = 0; i < NP; i++) if (cnt[i] > 0) { const float k = 1 / cnt[i];
    float vx = nx[i] * k, vy = ny[i] * k, vz = nz[i] * k; const V3 n = norm({vx, vy, vz}); nx[i] = n.x; ny[i] = n.y; nz[i] = n.z;
    ht[i] = 0.5f + (ht[i] - 0.5f) * k; ao[i] = 1 + (ao[i] - 1) * k;
    px[i] *= k; py[i] *= k; pz[i] *= k; if (wantCol) { cr[i] *= k; cg[i] *= k; cb[i] *= k; } cov[i] = 1; hit++; }
  R.texels = int(NP); R.hit = hit;
  // (the height accumulates 'value − 0.5' per sample from 0.5: divided by the count it is the mean)
  // padding: each pass copies the neighbours' values into the empty texels next to the filled ones
  for (int pass = 0; pass < s.padding; pass++) {
    std::vector<char> c2 = cov; std::vector<float> a1 = nx, a2 = ny, a3 = nz, a4 = ht, a5 = ao, a6 = px, a7 = py, a8 = pz, b1 = cr, b2 = cg, b3 = cb;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) { const size_t i = size_t(y) * N + x; if (cov[i]) continue;
      for (int dy = -1; dy <= 1 && !c2[i]; dy++) for (int dx = -1; dx <= 1; dx++) { const int X = x + dx, Y = y + dy; if (X < 0 || Y < 0 || X >= N || Y >= N) continue; const size_t j = size_t(Y) * N + X;
        if (cov[j]) { a1[i] = nx[j]; a2[i] = ny[j]; a3[i] = nz[j]; a4[i] = ht[j]; a5[i] = ao[j]; a6[i] = px[j]; a7[i] = py[j]; a8[i] = pz[j]; if (wantCol) { b1[i] = cr[j]; b2[i] = cg[j]; b3[i] = cb[j]; } c2[i] = 1; break; } } }
    nx = a1; ny = a2; nz = a3; ht = a4; ao = a5; px = a6; py = a7; pz = a8; if (wantCol) { cr = b1; cg = b2; cb = b3; } cov = c2;
  }
  if (s.normal || s.curvature) {
    R.normal = QImage(N, N, QImage::Format_RGB888);
    for (int y = 0; y < N; y++) { uchar* o = R.normal.scanLine(y); for (int x = 0; x < N; x++) { const size_t i = size_t(y) * N + x;
      if (cov[i]) { o[x * 3] = uchar(clamp01(nx[i] * 0.5f + 0.5f) * 255 + 0.5f); o[x * 3 + 1] = uchar(clamp01(ny[i] * 0.5f + 0.5f) * 255 + 0.5f); o[x * 3 + 2] = uchar(clamp01(nz[i] * 0.5f + 0.5f) * 255 + 0.5f); }
      else { o[x * 3] = 128; o[x * 3 + 1] = 128; o[x * 3 + 2] = 255; } } }
  }
  if (s.ao) { R.ao = QImage(N, N, QImage::Format_Grayscale8); for (int y = 0; y < N; y++) { uchar* o = R.ao.scanLine(y); for (int x = 0; x < N; x++) { const size_t i = size_t(y) * N + x; o[x] = cov[i] ? uchar(clamp01(ao[i]) * 255 + 0.5f) : 255; } } }
  if (s.height) { R.height = QImage(N, N, QImage::Format_Grayscale16); for (int y = 0; y < N; y++) { quint16* o = reinterpret_cast<quint16*>(R.height.scanLine(y)); for (int x = 0; x < N; x++) { const size_t i = size_t(y) * N + x; o[x] = quint16(clamp01(cov[i] ? ht[i] : 0.5f) * 65535 + 0.5f); } } }
  if (s.position) { R.position = QImage(N, N, QImage::Format_RGB888); for (int y = 0; y < N; y++) { uchar* o = R.position.scanLine(y); for (int x = 0; x < N; x++) { const size_t i = size_t(y) * N + x; o[x * 3] = uchar(clamp01(cov[i] ? px[i] : 0) * 255 + 0.5f); o[x * 3 + 1] = uchar(clamp01(cov[i] ? py[i] : 0) * 255 + 0.5f); o[x * 3 + 2] = uchar(clamp01(cov[i] ? pz[i] : 0) * 255 + 0.5f); } } }
  if (wantCol) { R.colour = QImage(N, N, QImage::Format_RGB888); for (int y = 0; y < N; y++) { uchar* o = R.colour.scanLine(y); for (int x = 0; x < N; x++) { const size_t i = size_t(y) * N + x;
      o[x * 3] = uchar(clamp01(cov[i] ? cr[i] : 0) * 255 + 0.5f); o[x * 3 + 1] = uchar(clamp01(cov[i] ? cg[i] : 0) * 255 + 0.5f); o[x * 3 + 2] = uchar(clamp01(cov[i] ? cb[i] : 0) * 255 + 0.5f); } } }
  if (s.curvature) {
    R.curvature = QImage(N, N, QImage::Format_Grayscale8);
    for (int y = 0; y < N; y++) { uchar* o = R.curvature.scanLine(y); for (int x = 0; x < N; x++) {
      auto at = [&](int X, int Y) { X = std::clamp(X, 0, N - 1); Y = std::clamp(Y, 0, N - 1); return size_t(Y) * N + X; };
      const float c = (nx[at(x + 1, y)] - nx[at(x - 1, y)]) + (ny[at(x, y + 1)] - ny[at(x, y - 1)]);   // (the normal's divergence in the texture's plane)
      o[x] = uchar(clamp01(0.5f - c * 4) * 255 + 0.5f); } }
  }
  // (the rows were worked from v = 0 up; a mesh whose textures read from the top — the usual — gets its maps turned over: they lie
  // as the painted layers do, as the model shows them and as a file is written)
  if (low.flipV) for (QImage* im : {&R.normal, &R.ao, &R.height, &R.position, &R.curvature, &R.colour}) if (!im->isNull()) *im = im->mirrored(false, true);
  R.log = QString("%1 of %2 texels hit, %3 s").arg(hit).arg(NP).arg(clock.elapsed() / 1000.0, 0, 'f', 1);
  return R;
}

QImage heightToNormal(const QImage& h16, float strength, bool flipGreen) {
  if (h16.isNull()) return {};
  const QImage src = h16.format() == QImage::Format_Grayscale16 ? h16 : h16.convertToFormat(QImage::Format_Grayscale16);
  const int W = src.width(), H = src.height(); QImage out(W, H, QImage::Format_RGB888);
  auto at = [&](int x, int y) { x = (x % W + W) % W; y = (y % H + H) % H; return reinterpret_cast<const quint16*>(src.constScanLine(y))[x] / 65535.f; };
  for (int y = 0; y < H; y++) { uchar* o = out.scanLine(y); for (int x = 0; x < W; x++) {
    const float dx = (at(x + 1, y) - at(x - 1, y)) * 0.5f * strength * 8, dy = (at(x, y + 1) - at(x, y - 1)) * 0.5f * strength * 8;
    const V3 n = norm({-dx, (flipGreen ? -1.f : 1.f) * dy, 1.f});   /* (the rows run down while v runs up: a rise toward the top of the picture tilts the normal up — green over 128, OpenGL's) */ o[x * 3] = uchar((n.x * 0.5f + 0.5f) * 255 + 0.5f); o[x * 3 + 1] = uchar((n.y * 0.5f + 0.5f) * 255 + 0.5f); o[x * 3 + 2] = uchar((n.z * 0.5f + 0.5f) * 255 + 0.5f); } }
  return out;
}

QImage finalNormal(const QImage& bakedNormal, const QImage& bakeMask, const QImage& height16, float strength, bool flipGreen, const QSize& size) {
  if (size.isEmpty() || (bakedNormal.isNull() && height16.isNull())) return {};
  const int W = size.width(), H = size.height();
  const QImage bk = bakedNormal.isNull() ? QImage() : bakedNormal.convertToFormat(QImage::Format_RGB888);
  const QImage mk = bakeMask.isNull() ? QImage() : bakeMask.convertToFormat(QImage::Format_Grayscale8);
  QImage bump = height16.isNull() ? QImage() : heightToNormal(height16, strength, flipGreen); if (!bump.isNull() && bump.size() != size) bump = bump.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
  QImage out(W, H, QImage::Format_RGB888);
  for (int y = 0; y < H; y++) { uchar* o = out.scanLine(y);
    const uchar* b = bk.isNull() ? nullptr : bk.constScanLine(std::min(bk.height() - 1, y * bk.height() / H));
    const uchar* m = mk.isNull() ? nullptr : mk.constScanLine(std::min(mk.height() - 1, y * mk.height() / H));
    const uchar* p = bump.isNull() ? nullptr : bump.constScanLine(y);
    for (int x = 0; x < W; x++) {
      V3 n{0, 0, 1};
      if (b) { const int bx = std::min(bk.width() - 1, x * bk.width() / W); const float k = m ? m[std::min(mk.width() - 1, x * mk.width() / W)] / 255.f : 1.f;
        n = norm({(b[bx * 3] / 127.5f - 1) * k, (b[bx * 3 + 1] / 127.5f - 1) * k, 1 + (b[bx * 3 + 2] / 127.5f - 1 - 1) * k}); }   // (flat → the bake, by the mask)
      if (p) { const V3 q{p[x * 3] / 127.5f - 1, p[x * 3 + 1] / 127.5f - 1, p[x * 3 + 2] / 127.5f - 1}; n = norm({n.x + q.x, n.y + q.y, n.z * q.z}); }   // (the bump's slope added: the two tilts sum)
      o[x * 3] = uchar(clamp01(n.x * 0.5f + 0.5f) * 255 + 0.5f); o[x * 3 + 1] = uchar(clamp01(n.y * 0.5f + 0.5f) * 255 + 0.5f); o[x * 3 + 2] = uchar(clamp01(n.z * 0.5f + 0.5f) * 255 + 0.5f); } }
  return out;
}


namespace {
template <class F> void par(int n, int chunk, F f) {
  const int nt = std::clamp(int(std::thread::hardware_concurrency()), 1, 16); std::vector<std::thread> ts; std::atomic<int> next{0};
  auto worker = [&] { for (;;) { const int a = next.fetch_add(chunk); if (a >= n) break; f(a, std::min(n, a + chunk)); } };
  for (int k = 1; k < nt; k++) ts.emplace_back(worker); worker(); for (auto& t : ts) t.join();
}
}  // namespace

std::shared_ptr<ShadowFrame> shadowFrame(const float* P, const float* N, int w, int h) {
  auto F = std::make_shared<ShadowFrame>(); if (!P || !N || w <= 0 || h <= 0) return F;
  F->w = w; F->h = h; const size_t n = size_t(w) * h;
  auto surf = [&](size_t i) { return N[i * 3] != 0 || N[i * 3 + 1] != 0 || N[i * 3 + 2] != 0; };
  auto pos = [&](size_t i) { return V3{P[i * 3], P[i * 3 + 1], P[i * 3 + 2]}; };
  // (a texel's size on the model: how far to start the rays off the surface, and what counts as a neighbour)
  std::vector<float> st; for (size_t i = 0; i + 1 < n && st.size() < 100000; i += 7) if (surf(i) && (i + 1) % size_t(w) && surf(i + 1)) st.push_back(len(pos(i) - pos(i + 1)));
  if (!st.empty()) { std::nth_element(st.begin(), st.begin() + st.size() / 2, st.end()); F->tex = st[st.size() / 2]; }
  const int step = F->step = w * h >= 4096 * 4096 ? 4 : w * h >= 2048 * 2048 ? 2 : 1;
  const int gw = F->gw = (w + step - 1) / step, gh = F->gh = (h + step - 1) / step;
  F->gp.assign(size_t(gw) * gh, {}); F->gn.assign(size_t(gw) * gh, {}); F->gok.assign(size_t(gw) * gh, 0); F->near4.assign(n, 0xff);
  par(gh, 16, [&](int y0, int y1) { for (int gy = y0; gy < y1; gy++) for (int gx = 0; gx < gw; gx++) {
    size_t best = size_t(-1); int bd = 1 << 30; const int cx = gx * step + step / 2, cy = gy * step + step / 2;   // (the cell's surface texel nearest its middle)
    for (int yy = gy * step; yy < std::min(h, (gy + 1) * step); yy++) for (int xx = gx * step; xx < std::min(w, (gx + 1) * step); xx++) {
      const size_t j = size_t(yy) * w + xx; if (!surf(j)) continue; const int dd = (xx - cx) * (xx - cx) + (yy - cy) * (yy - cy); if (dd < bd) { bd = dd; best = j; } }
    if (best == size_t(-1)) continue; const size_t c = size_t(gy) * gw + gx;
    F->gok[c] = 1; F->gp[c] = pos(best); F->gn[c] = norm(V3{N[best * 3], N[best * 3 + 1], N[best * 3 + 2]}); } });
  const float far = std::max(F->tex, 1e-9f) * step * 3;
  std::vector<std::vector<int>> loneT(size_t(std::max(1, h)));
  par(h, 32, [&](int y0, int y1) { for (int y = y0; y < y1; y++) for (int x = 0; x < w; x++) { const size_t i = size_t(y) * w + x; if (!surf(i)) continue; const V3 p = pos(i);
    const int x0 = int(std::floor((x + 0.5f) / step - 0.5f)), y0g = int(std::floor((y + 0.5f) / step - 0.5f)); unsigned char b = 0;
    for (int k = 0; k < 4; k++) { const int gx = x0 + (k & 1), gy = y0g + (k >> 1); if (gx < 0 || gy < 0 || gx >= gw || gy >= gh) continue; const size_t c = size_t(gy) * gw + gx;
      if (!F->gok[c]) continue; if (step > 1 && len(F->gp[c] - p) > far) continue; b |= (unsigned char)(1 << k); }
    F->near4[i] = b; if (!b) loneT[size_t(y)].push_back(int(i)); } });
  for (const auto& v : loneT) for (int i : v) { F->lone.push_back(i); F->lp.push_back(pos(size_t(i))); F->ln.push_back(norm(V3{N[size_t(i) * 3], N[size_t(i) * 3 + 1], N[size_t(i) * 3 + 2]})); }
  return F;
}

QImage castShadow(const ShadowFrame& F, const TriMesh& casters, const float L0[3], float softDeg, int rays, const std::function<bool(float)>& progress) {
  const int w = F.w, h = F.h; QImage out(std::max(1, w), std::max(1, h), QImage::Format_Grayscale8); out.fill(0);
  if (w <= 0 || h <= 0 || casters.empty()) return out;
  Bvh bvh; bvh.build(casters);
  V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (const V3& p : casters.pos) { lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}; hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)}; }
  const float diag = std::max(1e-6f, len(hi - lo)), reach = diag * 4, bias = std::max(F.tex * 1.5f, diag * 1e-4f);
  const V3 L = norm(V3{L0[0], L0[1], L0[2]});
  const V3 a0 = std::fabs(L.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0}; const V3 T = norm(cross(L, a0)), B = cross(L, T);   // (across the light: the cone of a soft one)
  const float spread = std::tan(std::clamp(softDeg, 0.f, 45.f) * float(M_PI) / 180.f); const int nr = std::clamp(rays, 1, 256);
  auto shade = [&](const V3& p, const V3& Nn, uint32_t seed) {   // (the share of the light's rays some caster blocks)
    if (dot(Nn, L) <= 0) return 0.f;                             // (turned away: its own shade, not a cast shadow)
    const V3 o = p + Nn * bias; int hit = 0;
    for (int r = 0; r < nr; r++) {
      V3 d = L;
      if (nr > 1 && spread > 0) {          // (stratified over a disc across the light, the pattern turned per sample - no bands)
        const float rr = std::sqrt((r + 0.5f) / nr), ang = r * 2.39996323f + hash(seed * 9781u + 17u) * 6.2831853f;
        d = norm(L + (T * std::cos(ang) + B * std::sin(ang)) * (spread * rr)); }
      if (bvh.closest(o, d, reach, true).tri >= 0) hit++; }
    return float(hit) / nr; };
  std::vector<float> g(F.gok.size(), 0.f); std::atomic<int> done{0}; std::atomic<bool> stop{false};
  par(F.gh, 4, [&](int y0, int y1) { for (int gy = y0; gy < y1 && !stop; gy++) {
    for (int gx = 0; gx < F.gw; gx++) { const size_t c = size_t(gy) * F.gw + gx; if (F.gok[c]) g[c] = shade(F.gp[c], F.gn[c], uint32_t(c)); }
    const int k = ++done; if (progress && (k & 15) == 0 && !progress(float(k) / F.gh)) stop = true; } });
  if (stop) return out;
  // (one smoothing pass over the grid — the soft edge's grain from a few rays — among neighbours that lie side by side on the model)
  if (spread > 0 && nr > 1) {
    const float far = std::max(F.tex, 1e-9f) * F.step * 2.5f; std::vector<float> sm(g);
    par(F.gh, 16, [&](int y0, int y1) { for (int gy = y0; gy < y1; gy++) for (int gx = 0; gx < F.gw; gx++) { const size_t c = size_t(gy) * F.gw + gx; if (!F.gok[c]) continue;
      float s = g[c] * 4, ws = 4;
      for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) { if (!dx && !dy) continue; const int X = gx + dx, Y = gy + dy; if (X < 0 || Y < 0 || X >= F.gw || Y >= F.gh) continue;
        const size_t d = size_t(Y) * F.gw + X; if (!F.gok[d] || len(F.gp[d] - F.gp[c]) > far * (dx && dy ? 1.42f : 1.f)) continue; const float wt = dx && dy ? 1.f : 2.f; s += g[d] * wt; ws += wt; }
      sm[c] = s / ws; } });
    g.swap(sm); }
  std::vector<float> lone(F.lone.size(), 0.f);
  par(int(F.lone.size()), 256, [&](int a, int b) { for (int k = a; k < b; k++) lone[size_t(k)] = shade(F.lp[size_t(k)], F.ln[size_t(k)], uint32_t(F.lone[size_t(k)])); });
  const int step = F.step;
  par(h, 32, [&](int ya, int yb) { for (int y = ya; y < yb; y++) { uchar* o = out.scanLine(y);
    for (int x = 0; x < w; x++) { const size_t i = size_t(y) * w + x; const unsigned char b = F.near4[i]; if (b == 0xff || !b) continue;
      const float fx = (x + 0.5f) / step - 0.5f, fy = (y + 0.5f) / step - 0.5f; const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy)); float sum = 0, ws = 0;
      for (int k = 0; k < 4; k++) if (b >> k & 1) { const int dx = k & 1, dy = k >> 1; const float wt = (dx ? fx - x0 : 1 - (fx - x0)) * (dy ? fy - y0 : 1 - (fy - y0)) + 1e-4f;
        sum += g[size_t(y0 + dy) * F.gw + size_t(x0 + dx)] * wt; ws += wt; }
      o[x] = uchar(clamp01(sum / ws) * 255 + 0.5f); } } });
  for (size_t k = 0; k < F.lone.size(); k++) { const int i = F.lone[k]; out.scanLine(i / w)[i % w] = uchar(clamp01(lone[k]) * 255 + 0.5f); }
  return out;
}

}  // namespace hm::paint
