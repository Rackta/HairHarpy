// The snap mask's proxy of a high-poly mesh: a decimated copy (the file's space) made once in the background — its
// vertices merged by a grid whose cells are three times finer at the mesh's rims, its polygroups' borders and where
// another mesh comes near (there the mask must follow the shape closely). The mask brush, its lasso, the polygroups'
// highlight and the green of the mask work on it; each polygon of the mesh knows its proxy triangle (f2p), so a mask
// painted on the proxy goes onto the mesh at once and back.
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace hm {

namespace {
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
long long trisOf(const PolyMesh& b) { return (long long)b.faceVerts.size() - 2LL * b.faceCount(); }
const long long PX_FROM = 200000;     // (from here a mesh gets a proxy)
const long long PX_TARGET = 320000;   // (about this many triangles where the grid is coarse)
const long long PX_MAX = 640000;      // (more — the grid made coarser)
}  // namespace

void Viewport::maskProxyBuild(MaskProxyJob& J) {
  const PolyMesh& B = *J.src; const int nf = B.faceCount(); const size_t nv = B.pos.size();
  if (!nf || !nv) return;
  double area = 0; const Box3 box = B.bounds();
  for (int f = 0; f < nf; f++) { const int s = B.faceStart[size_t(f)]; for (int k = s + 1; k + 1 < B.faceStart[size_t(f) + 1]; k++)
    area += (B.pos[size_t(B.faceVerts[size_t(k)])] - B.pos[size_t(B.faceVerts[size_t(s)])]).cross(B.pos[size_t(B.faceVerts[size_t(k + 1)])] - B.pos[size_t(B.faceVerts[size_t(s)])]).length() / 2; }
  const double cell0 = std::max(std::sqrt(area / double(PX_TARGET / 2)), box.size().length() * 1e-5 + 1e-12);
  const bool fg = int(B.faceGroup.size()) == nf;
  auto cellOf = [&](const Vec3& p, double c) { const Vec3 q = (p - box.min) * (1 / c);
    return (uint64_t(std::min(1048575.0, std::max(0.0, q.x))) << 40) | (uint64_t(std::min(1048575.0, std::max(0.0, q.y))) << 20) | uint64_t(std::min(1048575.0, std::max(0.0, q.z))); };
  // (the vertices that want the fine grid round them: on a rim, on a polygroup border, where another mesh touches)
  std::vector<char> want(nv, 0);
  { std::vector<int> g0(nv, -1);
    for (int f = 0; f < nf; f++) { const int g = fg ? B.faceGroup[size_t(f)] : 0; for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) { int& x = g0[size_t(B.faceVerts[size_t(k)])];
      if (x < 0) x = g; else if (x != g) want[size_t(B.faceVerts[size_t(k)])] = 1; } } }
  if (J.cancel) return;
  { std::vector<uint64_t> es; es.reserve(B.faceVerts.size());
    for (int f = 0; f < nf; f++) { const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1];
      for (int k = s; k < e; k++) { const uint32_t a = uint32_t(B.faceVerts[size_t(k)]), b = uint32_t(B.faceVerts[size_t(k + 1 < e ? k + 1 : s)]); if (a != b) es.push_back((uint64_t(std::min(a, b)) << 32) | std::max(a, b)); } }
    std::sort(es.begin(), es.end());
    for (size_t i = 0; i < es.size();) { size_t j = i; while (j < es.size() && es[j] == es[i]) j++;
      if (j - i == 1) { want[size_t(es[i] >> 32)] = 1; want[size_t(es[i] & 0xffffffffu)] = 1; }   // (a rim)
      i = j; } }
  if (J.cancel) return;
  if (!J.others.empty()) {                                       // (another mesh touching or crossing it: within a third of a cell)
    Box3 bb = box; bb.expand(box.min - Vec3(cell0, cell0, cell0)); bb.expand(box.max + Vec3(cell0, cell0, cell0));
    std::vector<Vec3> OP; std::vector<uint32_t> OT;
    for (auto& [O, A] : J.others) {
      const size_t b0 = OP.size(); OP.reserve(b0 + O->pos.size()); for (const Vec3& p : O->pos) OP.push_back(A.apply(p));
      auto in = [&](int v) { const Vec3& p = OP[b0 + size_t(v)]; return p.x >= bb.min.x && p.x <= bb.max.x && p.y >= bb.min.y && p.y <= bb.max.y && p.z >= bb.min.z && p.z <= bb.max.z; };
      for (int f = 0; f < O->faceCount(); f++) { const int s = O->faceStart[size_t(f)]; for (int k = s + 1; k + 1 < O->faceStart[size_t(f) + 1]; k++) {
        const int a = O->faceVerts[size_t(s)], b = O->faceVerts[size_t(k)], c = O->faceVerts[size_t(k + 1)];
        if (in(a) || in(b) || in(c)) { OT.push_back(uint32_t(b0 + size_t(a))); OT.push_back(uint32_t(b0 + size_t(b))); OT.push_back(uint32_t(b0 + size_t(c))); } } }
      if (J.cancel) return;
    }
    if (!OT.empty()) {
      topo::Bvh ob; ob.build(OP, OT); const double fine0 = cell0 / 3;
      std::unordered_map<uint64_t, int> rep; rep.reserve(size_t(PX_TARGET) * 4);
      for (size_t v = 0; v < nv; v++) rep.emplace(cellOf(B.pos[v], fine0), int(v));   // (looked at once per fine cell)
      std::vector<int> near; size_t i = 0;
      for (auto& [k, v] : rep) { double d = 1e300; if (ob.closest(B.pos[size_t(v)], nullptr, &d) && d < cell0 / 3) near.push_back(v); if (++i % 4096 == 0 && J.cancel) return; }
      for (int v : near) want[size_t(v)] = 2;
    }
  }
  if (J.cancel) return;
  // (a small separate part — a pin, a bead, a stitch, under a few cells across: kept whole, vertex for vertex — the grid
  // would fold it into a point and it could not be painted; as many of them as the budget holds, the smallest first)
  std::vector<char> keepV(nv, 0);
  { std::vector<uint32_t> up(nv); for (size_t v = 0; v < nv; v++) up[v] = uint32_t(v);
    auto root = [&](uint32_t x) { while (up[x] != x) { up[x] = up[up[x]]; x = up[x]; } return x; };
    for (int f = 0; f < nf; f++) { const int s = B.faceStart[size_t(f)]; for (int k = s + 1; k < B.faceStart[size_t(f) + 1]; k++) { const uint32_t a = root(uint32_t(B.faceVerts[size_t(s)])), b = root(uint32_t(B.faceVerts[size_t(k)])); if (a != b) up[std::max(a, b)] = std::min(a, b); } }
    std::unordered_map<uint32_t, std::pair<Box3, size_t>> part; for (size_t v = 0; v < nv; v++) { auto& P = part[root(uint32_t(v))]; P.first.expand(B.pos[v]); P.second++; }
    std::vector<std::pair<double, uint32_t>> small; for (auto& [r, P] : part) { const double d = (P.first.max - P.first.min).length(); if (d < cell0 * 3) small.push_back({d, r}); }
    std::sort(small.begin(), small.end()); size_t budget = size_t(PX_TARGET); std::unordered_set<uint32_t> take;
    for (auto& [d, r] : small) { const size_t n = part[r].second; if (n > budget) break; budget -= n; take.insert(r); }
    if (!take.empty()) for (size_t v = 0; v < nv; v++) if (take.count(root(uint32_t(v)))) keepV[v] = 1; }
  if (J.cancel) return;
  std::vector<Vec3> vn(nv);
  for (int f = 0; f < nf; f++) { const int s = B.faceStart[size_t(f)]; for (int k = s + 1; k + 1 < B.faceStart[size_t(f) + 1]; k++) {
    const int a = B.faceVerts[size_t(s)], b = B.faceVerts[size_t(k)], c = B.faceVerts[size_t(k + 1)]; const Vec3 x = (B.pos[size_t(b)] - B.pos[size_t(a)]).cross(B.pos[size_t(c)] - B.pos[size_t(a)]);
    vn[size_t(a)] += x; vn[size_t(b)] += x; vn[size_t(c)] += x; } }
  std::vector<uint64_t> side(nv);
  for (size_t v = 0; v < nv; v++) { const Vec3& d = vn[v]; side[v] = std::abs(d.x) >= std::abs(d.y) && std::abs(d.x) >= std::abs(d.z) ? (d.x >= 0 ? 0 : 1) : std::abs(d.y) >= std::abs(d.z) ? (d.y >= 0 ? 2 : 3) : (d.z >= 0 ? 4 : 5); }
  // the clusters and the triangles over them — the grid coarsened until the proxy keeps to its budget
  double cell = cell0;
  for (int attempt = 0; attempt < 4; attempt++) {
    if (J.cancel) return;
    const double fine = cell / 3;
    std::unordered_set<uint64_t> fineCells; for (size_t v = 0; v < nv; v++) if (want[v]) fineCells.insert(cellOf(B.pos[v], cell));
    // (a cluster: the vertices of one cell joined by edges inside it — two layers of cloth in a cell stay apart)
    std::vector<uint64_t> key(nv);
    for (size_t v = 0; v < nv; v++) { const bool fn = fineCells.count(cellOf(B.pos[v], cell)) > 0; key[v] = (uint64_t(fn) << 63) | (side[v] << 60) | cellOf(B.pos[v], fn ? fine : cell); }
    std::vector<uint32_t> up(nv); for (size_t v = 0; v < nv; v++) up[v] = uint32_t(v);
    auto root = [&](uint32_t x) { while (up[x] != x) { up[x] = up[up[x]]; x = up[x]; } return x; };
    for (int f = 0; f < nf; f++) { const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1];
      for (int k = s; k < e; k++) { const uint32_t a = uint32_t(B.faceVerts[size_t(k)]), b = uint32_t(B.faceVerts[size_t(k + 1 < e ? k + 1 : s)]);
        if (key[a] == key[b] && !keepV[a] && !keepV[b]) { const uint32_t ra = root(a), rb = root(b); if (ra != rb) up[std::max(ra, rb)] = std::min(ra, rb); } } }
    std::vector<uint32_t> map(nv), cid(nv, UINT32_MAX); std::vector<Vec3> sum; std::vector<int> cnt;
    for (size_t v = 0; v < nv; v++) {
      const uint32_t r = root(uint32_t(v)); if (cid[r] == UINT32_MAX) { cid[r] = uint32_t(cnt.size()); cnt.push_back(0); sum.emplace_back(); }
      const uint32_t c = cid[r]; map[v] = c; cnt[c]++; sum[c] += B.pos[v];
    }
    if (cnt.size() >= (1u << 21)) { cell *= 1.6; continue; }
    std::unordered_map<uint64_t, int> tri; tri.reserve(size_t(PX_TARGET) * 3); std::vector<uint32_t> idx; std::vector<int> f2p(size_t(nf), -1), pSrc;
    for (int f = 0; f < nf; f++) { const int s = B.faceStart[size_t(f)];
      for (int k = s + 1; k + 1 < B.faceStart[size_t(f) + 1]; k++) {
        uint32_t q[3] = {map[size_t(B.faceVerts[size_t(s)])], map[size_t(B.faceVerts[size_t(k)])], map[size_t(B.faceVerts[size_t(k + 1)])]};
        if (q[0] == q[1] || q[1] == q[2] || q[0] == q[2]) continue;
        uint32_t o[3] = {q[0], q[1], q[2]}; std::sort(o, o + 3); const uint64_t key = (uint64_t(o[0]) << 42) | (uint64_t(o[1]) << 21) | o[2];
        auto r = tri.emplace(key, int(idx.size() / 3));
        if (r.second) { idx.insert(idx.end(), q, q + 3); pSrc.push_back(f); }
        if (f2p[size_t(f)] < 0) f2p[size_t(f)] = r.first->second; } }
    const size_t np = idx.size() / 3;
    if (np > size_t(PX_MAX) && attempt < 3) { cell *= std::max(1.15, std::sqrt(double(np) / double(PX_TARGET + PX_TARGET / 2))); continue; }
    if (!np) return;
    // (each cluster at the member nearest its average: on the surface, not under it)
    std::vector<Vec3> cp(cnt.size()); std::vector<double> best(cnt.size(), 1e300);
    for (size_t c = 0; c < cnt.size(); c++) sum[c] = sum[c] * (1.0 / cnt[c]);
    for (size_t v = 0; v < nv; v++) { const uint32_t c = map[v]; const double d = B.pos[v].distanceTo(sum[c]); if (d < best[c]) { best[c] = d; cp[c] = B.pos[v]; } }
    // (a triangle the merging turned over — facing against the polygon it came from — turned back: drawn, not culled)
    for (size_t t = 0; t < np; t++) { const int f = pSrc[t], s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; Vec3 o;
      for (int k = s; k < e; k++) { const Vec3& a = B.pos[size_t(B.faceVerts[size_t(k)])]; const Vec3& b = B.pos[size_t(B.faceVerts[size_t(k + 1 < e ? k + 1 : s)])]; o += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)); }
      const Vec3 n = (cp[idx[t * 3 + 1]] - cp[idx[t * 3]]).cross(cp[idx[t * 3 + 2]] - cp[idx[t * 3]]);
      if (n.dot(o) < 0) std::swap(idx[t * 3 + 1], idx[t * 3 + 2]); }
    auto L = std::make_shared<PolyMesh>(); L->pos = std::move(cp); L->groupNames = B.groupNames; L->faceGroup.reserve(np);
    for (size_t t = 0; t < np; t++) { const int qi[3] = {int(idx[t * 3]), int(idx[t * 3 + 1]), int(idx[t * 3 + 2])}; L->addFace(qi, 3); L->faceGroup.push_back(fg ? B.faceGroup[size_t(pSrc[t])] : 0); }
    // (a polygon merged away: a triangle at one of its clusters — of its own polygroup if there is one)
    std::vector<int> off(cnt.size() + 1, 0), inc(np * 3);
    for (size_t t = 0; t < np; t++) for (int j = 0; j < 3; j++) off[idx[t * 3 + size_t(j)] + 1]++;
    for (size_t c = 0; c < cnt.size(); c++) off[c + 1] += off[c];
    { std::vector<int> fill(off.begin(), off.end() - 1); for (size_t t = 0; t < np; t++) for (int j = 0; j < 3; j++) inc[size_t(fill[idx[t * 3 + size_t(j)]]++)] = int(t); }
    for (int f = 0; f < nf; f++) if (f2p[size_t(f)] < 0) {
      const int g = fg ? B.faceGroup[size_t(f)] : 0; int any = -1;
      for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1] && f2p[size_t(f)] < 0; k++) { const uint32_t c = map[size_t(B.faceVerts[size_t(k)])];
        for (int i = off[c]; i < off[c + 1]; i++) { const int t = inc[size_t(i)]; if (any < 0) any = t; if (L->faceGroup[size_t(t)] == g) { f2p[size_t(f)] = t; break; } } }
      if (f2p[size_t(f)] < 0) f2p[size_t(f)] = any; }
    auto bvh = std::make_shared<topo::Bvh>(); bvh->build(L->pos, idx);
    auto X = std::make_shared<MaskProxy>(); X->mesh = L; X->bvh = bvh; X->f2p = std::move(f2p); X->pSrc = std::move(pSrc); J.out = X;
    return;
  }
}

// (from heavyTick) the proxies of the Topology room's high-poly meshes started, finished, dropped → still waiting
bool Viewport::maskProxyTick() {
  const qint64 now = QDateTime::currentMSecsSinceEpoch(); bool waiting = false, got = false; std::set<int> alive;
  static const bool noPx = qEnvironmentVariableIsSet("HM_NOPROXY");   // (checks: no proxies)
  if (room() == "topo" && !noPx) for (auto& [id, it] : scene_->items) {
    if (it.type != ItemType::IMesh) continue; auto B = baseSp(it); if (!B || trisOf(*B) < PX_FROM) continue;
    alive.insert(id); MaskProxySlot& s = mproxy_[id];
    if (s.src.lock() != B) { s.src = B; s.px.reset(); s.failed = false; s.seen = now; if (s.job) s.job->cancel = true; s.job.reset(); }
    if (s.job && s.job->done) { if (s.job->src == B && !s.job->cancel && s.job->out) { s.px = s.job->out; got = true; } s.job.reset(); if (!s.px) s.failed = true; }   // (none made: not tried again for this geometry)
    if (!s.px && !s.job && !s.failed) {
      if (now - s.seen >= 400 && drag_ == Drag::None) {
        auto J = std::make_shared<MaskProxyJob>(); J->src = B; const Affine Mi = meshmods::meshMatrix(*scene_, it).inverse();
        for (auto& [oid, o] : scene_->items) {                 // (the other shown meshes: where they come near, the proxy is finer)
          if (oid == id || !o.poly || !scene_->isVisible(o) || !(o.type == ItemType::IMesh || o.type == ItemType::Mesh || o.type == ItemType::Head)) continue;
          if (auto lod = lodOf(oid)) J->others.push_back({std::const_pointer_cast<PolyMesh>(lod), Affine::mul(Mi, meshmods::meshMatrix(*scene_, o))});
          else J->others.push_back({o.poly, Mi});
        }
        s.job = J; std::thread([J] { maskProxyBuild(*J); J->done = true; }).detach();
      } else waiting = true;
    }
    if (s.job) waiting = true;
  }
  for (auto it = mproxy_.begin(); it != mproxy_.end();) { if (alive.count(it->first)) { ++it; continue; } if (it->second.job) it->second.job->cancel = true; it = mproxy_.erase(it); }
  if (got) { topoRev_++; requestUpdate(); }
  return waiting;
}

std::shared_ptr<const Viewport::MaskProxy> Viewport::maskProxy(int id) const {
  auto f = mproxy_.find(id); if (f == mproxy_.end() || !f->second.px) return nullptr;
  const Item* it = scene_->item(id); if (!it || f->second.src.lock() != baseSp(*it)) return nullptr;
  return f->second.px;
}

// a mask of the mesh's polygons → of the proxy's triangles (a triangle in when most of its polygons are; one no polygon
// went onto — as the polygon it was made from)
std::vector<char> Viewport::pxFromMask(const MaskProxy& X, const std::vector<char>& a) {
  const size_t np = size_t(X.mesh->faceCount()); std::vector<int> in(np, 0), all(np, 0);
  for (size_t f = 0; f < X.f2p.size(); f++) { const int p = X.f2p[f]; if (p < 0) continue; all[size_t(p)]++; if (f < a.size() && a[f]) in[size_t(p)]++; }
  std::vector<char> out(np, 0);
  for (size_t p = 0; p < np; p++) out[p] = all[p] ? in[p] * 2 >= all[p] : (p < X.pSrc.size() && size_t(X.pSrc[p]) < a.size() && a[size_t(X.pSrc[p])]);
  return out;
}
// a stroke on the proxy (before → after) onto the mesh's mask a: only the polygons of the triangles it changed, not a
// hidden one (locked)
std::vector<char> Viewport::pxApply(const MaskProxy& X, const std::vector<char>& before, const std::vector<char>& after, std::vector<char> a, const std::vector<char>& locked) {
  if (a.size() != X.f2p.size()) a.assign(X.f2p.size(), 0);
  for (size_t f = 0; f < a.size(); f++) { const size_t p = size_t(X.f2p[f]); if (X.f2p[f] < 0 || p >= after.size() || p >= before.size() || after[p] == before[p]) continue;
    if (f < locked.size() && locked[f]) continue; a[f] = after[p]; }
  return a;
}
// the mesh's hidden polygons (its hidden polygroups, the faces hidden with H) — and its proxy's triangles of them
std::vector<char> Viewport::faceLocked(int m) const {
  const Item* it = scene_->item(m); if (!it) return {}; auto Bs = baseSp(*it); const PolyMesh* B = Bs.get(); if (!B) return {};
  std::vector<char> h = meshmods::hiddenFaces(*it, *B);
  const QJsonArray hg = it->raw.value("hidGroups").toArray();
  if (!hg.isEmpty() && int(B->faceGroup.size()) == B->faceCount()) { std::set<int> gs; for (const QJsonValue& v : hg) gs.insert(v.toInt());
    if (h.empty()) h.assign(size_t(B->faceCount()), 0); for (size_t f = 0; f < h.size(); f++) if (gs.count(B->faceGroup[f])) h[f] = 1; }
  return h;
}
const Viewport::PxVis& Viewport::pxVis(int m, const MaskProxy& X) const {
  ProfScope prof_("pxVis");
  const Item* it = scene_->item(m); auto& c = pxVis_[m];
  const QByteArray key = QByteArray::number(quintptr(&X)) + "|" + QJsonDocument(it ? it->raw.value("hidGroups").toArray() : QJsonArray()).toJson(QJsonDocument::Compact)
                         + "|" + QByteArray::number(it ? qHash(it->raw.value("hidF").toString()) : 0);
  if (c.key == key) return c;
  c = {}; c.key = key; const size_t np = size_t(X.mesh->faceCount());
  const auto lk = faceLocked(m);
  if (!lk.empty()) { c.hid.assign(np, 0); bool any = false;
    for (size_t p = 0; p < np && p < X.pSrc.size(); p++) if (size_t(X.pSrc[p]) < lk.size() && lk[size_t(X.pSrc[p])]) { c.hid[p] = 1; any = true; }
    if (!any) c.hid.clear(); }
  if (c.hid.empty()) c.bvh = X.bvh;                           // (nothing hidden: the whole proxy hides what is behind it)
  else { std::vector<uint32_t> idx; const auto& I = X.bvh->idx; for (size_t p = 0; p < np; p++) if (!c.hid[p]) for (int j = 0; j < 3; j++) idx.push_back(I[p * 3 + size_t(j)]);
    auto b = std::make_shared<topo::Bvh>(); b->build(X.mesh->pos, idx); c.bvh = b; }
  return c;
}
const std::vector<char>& Viewport::pxMaskOf(int m, const MaskProxy& X) const {
  ProfScope prof_("pxMaskOf");
  const Item* it = scene_->item(m); const double rev = it ? it->raw.value("smRev").toDouble() : 0;
  auto& c = pxMask_[m];
  if (c.first != rev || c.second.size() != size_t(X.mesh->faceCount())) { c.first = rev; auto a = smArr(m); c.second = a.empty() ? std::vector<char>(size_t(X.mesh->faceCount()), 0) : pxFromMask(X, a); }
  return c.second;
}

}  // namespace hm

namespace hm {
int Viewport::maskProxyTris(int id) const { auto p = maskProxy(id); return p ? p->mesh->faceCount() : 0; }
}  // namespace hm

namespace hm {
Viewport::Occluders Viewport::meshOccluders(int m) const {
  ProfScope prof_("meshOccluders");
  Occluders o;
  for (auto& [id, x] : scene_->items) { if (id == m || x.type != ItemType::IMesh || !x.poly || !scene_->isVisible(x)) continue;
    if (heavyRay(id, Ray{cam_.pos, cam_.dir()}, false)) o.push_back({id, nullptr});
    else if ((long long)x.poly->faceVerts.size() - 2LL * x.poly->faceCount() < 400000) { if (auto b = bvhOf(id, false)) o.push_back({id, b}); } }
  return o;
}
bool Viewport::occludedBy(const Occluders& o, const Vec3& eye, const Vec3& p, double tol) const {
  const Vec3 d = (p - eye).normalized(); const double L = eye.distanceTo(p) - tol; if (L <= 0) return false;
  for (auto& [id, b] : o) { if (b) { if (b->ray(eye, d, L)) return true; continue; }
    if (auto hr = heavyRay(id, Ray{eye, d}, false); hr && *hr && (*hr)->t < L) return true; }
  return false;
}
}  // namespace hm

namespace hm {
int Viewport::smCount(int m) const {
  const Item* it = scene_->item(m); if (!it) return 0; const double rev = it->raw.value("smRev").toDouble();
  if (!it->raw.contains("smask")) return 0;
  auto& c = smCount_[m]; if (c.first == rev && rev != 0) return c.second;
  const auto a = smArr(m); c = {rev, int(std::count(a.begin(), a.end(), 1))}; return c.second;
}
std::vector<int> Viewport::groupCountsOf(int m) const {
  const Item* it = scene_->item(m); if (!it) return {}; auto B = baseSp(*it); if (!B) return {};
  auto& c = groupCnt_[m]; if (c.first == B.get() && c.second.size() == B->groupNames.size()) return c.second;
  std::vector<int> cnt(B->groupNames.size(), 0); for (int g : B->faceGroup) if (g >= 0 && size_t(g) < cnt.size()) cnt[size_t(g)]++;
  c = {B.get(), cnt}; return cnt;
}
}  // namespace hm

namespace hm {
void Viewport::heavyWork(const QString& what, const std::function<void()>& fn) {
  const int m = room() == "topo" ? topoMesh() : -1; const Item* it = m >= 0 ? scene_->item(m) : nullptr; auto B = it ? baseSp(*it) : nullptr;
  if (!B || B->faceCount() < 500000) { fn(); return; }
  ui::ThinkScope t(tr("%1 — %2 polygons of %3, a moment").arg(what).arg(B->faceCount()).arg(it->name), 0); t.now();
  fn();
}
}  // namespace hm

namespace hm {
int Viewport::heavyFace(int id, int tri) const {   // (tri: as heavyRay gives it — the pick cache's numbering)
  auto f = heavy_.find(id); if (f == heavy_.end() || tri < 0) return -1; const HeavySlot& S = f->second;
  const auto& L = S.hidTri.empty() ? S.t2f : S.pc2f; return size_t(tri) < L.size() ? L[size_t(tri)] : -1;
}
// a proxy triangle seen from the camera: the real mesh's surface first hit on the way to it is its own polygon or right
// by it — not a layer over it (a plate on the cloth) that the proxy has holes in; no hierarchy yet: the proxy's own
bool Viewport::pxSeen(int m, const MaskProxy& X, const PxVis& V, int t, const Vec3& eyeW, const Affine& M, bool ortho, const Vec3& dirW) const {
  const auto& P = X.mesh->pos; const auto& I = X.bvh->idx;
  const Vec3 &a = P[I[size_t(t) * 3]], &b = P[I[size_t(t) * 3 + 1]], &c = P[I[size_t(t) * 3 + 2]]; const Vec3 mid = (a + b + c) * (1.0 / 3);
  const Vec3 wm = M.apply(mid); const Vec3 o = ortho ? wm - dirW * (eyeW.distanceTo(wm) + 1) : eyeW; const Vec3 d = (wm - o).normalized(); const double L = o.distanceTo(wm);
  const double near = std::max({M.apply(a).distanceTo(wm), M.apply(b).distanceTo(wm), M.apply(c).distanceTo(wm)});   // (its size: a hit this close is it)
  if (auto hr = heavyRay(m, Ray{o, d}, false)) {
    if (!*hr) return true;                                     // (nothing on the way)
    const RayHit& h = **hr; if (h.t >= L - near * 0.5) return true;   // (the first surface is at it)
    const int f = heavyFace(m, h.tri); if (f >= 0 && size_t(f) < X.f2p.size() && X.f2p[size_t(f)] == t) return true;   // (its own polygon)
    return h.point.distanceTo(wm) < near;                      // (a neighbour of it, seen at a slant)
  }
  const Affine Mi = M.inverse(); const Vec3 ol = Mi.apply(o), ml = mid;   // (no hierarchy: the proxy without its hidden parts)
  return !V.bvh->ray(ol, (ml - ol).normalized(), ol.distanceTo(ml) - std::max(1e-9, X.mesh->bounds().size().length()) * 1e-4);
}
}  // namespace hm

namespace hm {
int Viewport::maskOccluded(int m, int g) const {
  const Item* it = scene_->item(m); if (!it) return -1; auto B = baseSp(*it); const auto a = smArr(m); if (a.empty()) return 0;
  const Affine M = meshmods::meshMatrix(*scene_, *it); int n = 0;
  for (int f = 0; f < B->faceCount(); f++) { if (!a[size_t(f)] || B->faceGroup[size_t(f)] != g) continue;
    Vec3 c; const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) c += B->pos[size_t(B->faceVerts[size_t(k)])]; c = M.apply(c * (1.0 / (e - s)));
    const ScreenPt sp = projector().toScreen(c); if (sp.z >= 1 || sp.x < 0 || sp.y < 0 || sp.x > width() || sp.y > height()) continue;
    const Vec3 d = (c - cam_.pos).normalized(); auto hr = heavyRay(m, Ray{cam_.pos, d}, false); if (!hr || !*hr) continue;
    if (heavyFace(m, (**hr).tri) != f && (**hr).point.distanceTo(c) > 0.3) n++; }
  return n;
}
}  // namespace hm

namespace hm {
// (checks) the high-poly mesh's small separate parts (a bead, a stone: under 1% of its size across): of them, those whose
// polygons go onto proxy triangles of their own part — paintable
QString Viewport::testProxyParts(int id) const {
  auto X = maskProxy(id); const Item* it = scene_->item(id); if (!X || !it) return "no proxy yet";
  auto Bs = baseSp(*it); const PolyMesh& B = *Bs; const int nf = B.faceCount(); const size_t nv = B.pos.size();
  std::vector<uint32_t> up(nv); for (size_t v = 0; v < nv; v++) up[v] = uint32_t(v);
  auto root = [&](uint32_t x) { while (up[x] != x) { up[x] = up[up[x]]; x = up[x]; } return x; };
  for (int f = 0; f < nf; f++) { const int s = B.faceStart[size_t(f)]; for (int k = s + 1; k < B.faceStart[size_t(f) + 1]; k++) { const uint32_t a = root(uint32_t(B.faceVerts[size_t(s)])), b = root(uint32_t(B.faceVerts[size_t(k)])); if (a != b) up[std::max(a, b)] = std::min(a, b); } }
  std::unordered_map<uint32_t, Box3> box; for (size_t v = 0; v < nv; v++) box[root(uint32_t(v))].expand(B.pos[v]);
  const double lim = B.bounds().size().length() * 0.1; std::unordered_map<uint32_t, std::pair<int, int>> small;   // (faces, kept)
  int hist[5] = {0, 0, 0, 0, 0}; for (auto& [r, b] : box) { const double q = (b.max - b.min).length() / B.bounds().size().length(); hist[q < 0.003 ? 0 : q < 0.01 ? 1 : q < 0.03 ? 2 : q < 0.1 ? 3 : 4]++; }
  for (int f = 0; f < nf; f++) { const uint32_t r = root(uint32_t(B.faceVerts[size_t(B.faceStart[size_t(f)])])); const Box3& b = box[r]; if ((b.max - b.min).length() >= lim) continue;
    auto& c = small[r]; c.first++; const int p = size_t(f) < X->f2p.size() ? X->f2p[size_t(f)] : -1;
    if (p >= 0 && size_t(p) < X->pSrc.size() && root(uint32_t(B.faceVerts[size_t(B.faceStart[size_t(X->pSrc[size_t(p)])])])) == r) c.second++; }
  int parts = 0, whole = 0; for (auto& [r, c] : small) { parts++; if (c.second * 10 >= c.first * 9) whole++; }
  return QString("proxy %1 triangles · parts by size <0.3%% %4, <1%% %5, <3%% %6, <10%% %7, more %8 · small (<10%%) %2, paintable %3").arg(X->mesh->faceCount()).arg(parts).arg(whole).arg(hist[0]).arg(hist[1]).arg(hist[2]).arg(hist[3]).arg(hist[4]);
}
}  // namespace hm
