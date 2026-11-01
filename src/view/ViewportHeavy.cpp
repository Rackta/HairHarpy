// Heavy meshes (over lodTris() triangles — a high-poly model, a scan): worked out once in the background when the mesh
// comes (a load, an import) or after it changed and stays put a moment — a lighter copy shown while the camera moves
// (the LOD: clustered to a couple of million triangles first when it is huge, then quadric-error collapses to
// LOD_TARGET) and a bounding volume hierarchy of its triangles, so a click, the wheel or the hover finds the surface at
// once instead of trying every triangle.
#include "scene/MeshMods.h"
#include "view/Viewport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QTimer>
#include <cmath>
#include <thread>
#include <unordered_map>

namespace hm {

namespace {
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
long long trisOf(const PolyMesh& b) { return (long long)b.faceVerts.size() - 2LL * b.faceCount(); }
const long long LOD_TARGET = 700000;

// the vertices merged by a grid whose cells hold about `want` vertices of the surface (Lindstrom's quadric clustering:
// each cell's vertex where its triangles' planes meet best, pulled a little toward their average, kept in the cell)
// → triangles over the cells' vertices (src: an original vertex of each)
void cluster(const PolyMesh& B, std::vector<float>& pos, std::vector<uint32_t>& idx, std::vector<int>& src, long long want) {
  double area = 0; Box3 box = B.bounds();
  for (int f = 0; f < B.faceCount(); f++) { int s = B.faceStart[size_t(f)]; for (int k = s + 1; k + 1 < B.faceStart[size_t(f) + 1]; k++)
    area += (B.pos[size_t(B.faceVerts[size_t(k)])] - B.pos[size_t(B.faceVerts[size_t(s)])]).cross(B.pos[size_t(B.faceVerts[size_t(k + 1)])] - B.pos[size_t(B.faceVerts[size_t(s)])]).length() / 2; }
  const double cell = std::max(std::sqrt(area / double(std::max<long long>(want, 1))), box.size().length() * 1e-6 + 1e-12);
  std::unordered_map<uint64_t, uint32_t> at; at.reserve(size_t(want) * 2);
  std::vector<uint32_t> map(B.pos.size()); std::vector<double> sum; std::vector<int> n;
  // (each vertex's facing — one of six directions: the two sides of a thin wall never merge, their normals don't cancel)
  std::vector<Vec3> vn(B.pos.size());
  for (int f = 0; f < B.faceCount(); f++) { int s = B.faceStart[size_t(f)]; for (int k = s + 1; k + 1 < B.faceStart[size_t(f) + 1]; k++) {
    int a = B.faceVerts[size_t(s)], b = B.faceVerts[size_t(k)], c = B.faceVerts[size_t(k + 1)]; Vec3 x = (B.pos[size_t(b)] - B.pos[size_t(a)]).cross(B.pos[size_t(c)] - B.pos[size_t(a)]);
    vn[size_t(a)] += x; vn[size_t(b)] += x; vn[size_t(c)] += x; } }
  for (size_t v = 0; v < B.pos.size(); v++) {
    const Vec3 q = (B.pos[v] - box.min) * (1 / cell); const Vec3& d = vn[v];
    const uint64_t side = std::abs(d.x) >= std::abs(d.y) && std::abs(d.x) >= std::abs(d.z) ? (d.x >= 0 ? 0 : 1) : std::abs(d.y) >= std::abs(d.z) ? (d.y >= 0 ? 2 : 3) : (d.z >= 0 ? 4 : 5);
    const uint64_t key = (side << 60) | (uint64_t(std::min(1048575.0, std::max(0.0, q.x))) << 40) | (uint64_t(std::min(1048575.0, std::max(0.0, q.y))) << 20) | uint64_t(std::min(1048575.0, std::max(0.0, q.z)));
    auto r = at.emplace(key, uint32_t(n.size()));
    if (r.second) { n.push_back(0); sum.insert(sum.end(), {0, 0, 0}); src.push_back(int(v)); }
    const uint32_t c = r.first->second; map[v] = c; n[c]++; sum[c * 3] += B.pos[v].x; sum[c * 3 + 1] += B.pos[v].y; sum[c * 3 + 2] += B.pos[v].z;
  }
  // the planes of the triangles at each cell (area-weighted): A (xx xy xz yy yz zz), b, c
  std::vector<double> Q(n.size() * 9, 0.0);
  for (int f = 0; f < B.faceCount(); f++) { int s = B.faceStart[size_t(f)]; for (int k = s + 1; k + 1 < B.faceStart[size_t(f) + 1]; k++) {
    const int vs[3] = {B.faceVerts[size_t(s)], B.faceVerts[size_t(k)], B.faceVerts[size_t(k + 1)]};
    Vec3 nn = (B.pos[size_t(vs[1])] - B.pos[size_t(vs[0])]).cross(B.pos[size_t(vs[2])] - B.pos[size_t(vs[0])]); double a = nn.length(); if (a < 1e-30) continue;
    Vec3 u = nn * (1 / a); double d = -u.dot(B.pos[size_t(vs[0])]), w = a / 2;
    const double q[9] = {u.x * u.x, u.x * u.y, u.x * u.z, u.y * u.y, u.y * u.z, u.z * u.z, u.x * d, u.y * d, u.z * d};
    uint32_t cs[3] = {map[size_t(vs[0])], map[size_t(vs[1])], map[size_t(vs[2])]};
    for (int j = 0; j < 3; j++) { if ((j == 1 && cs[1] == cs[0]) || (j == 2 && (cs[2] == cs[0] || cs[2] == cs[1]))) continue; double* o = &Q[size_t(cs[j]) * 9]; for (int i = 0; i < 9; i++) o[i] += w * q[i]; } } }
  pos.resize(n.size() * 3);
  for (size_t c = 0; c < n.size(); c++) {
    const Vec3 avg(sum[c * 3] / n[c], sum[c * 3 + 1] / n[c], sum[c * 3 + 2] / n[c]); const double* q = &Q[c * 9];
    const double lam = std::max(1e-12, (q[0] + q[3] + q[5]) * 1e-3);
    double a00 = q[0] + lam, a01 = q[1], a02 = q[2], a11 = q[3] + lam, a12 = q[4], a22 = q[5] + lam;
    double b0 = -q[6] + lam * avg.x, b1 = -q[7] + lam * avg.y, b2 = -q[8] + lam * avg.z;
    double det = a00 * (a11 * a22 - a12 * a12) - a01 * (a01 * a22 - a12 * a02) + a02 * (a01 * a12 - a11 * a02);
    Vec3 p = avg;
    if (std::abs(det) > 1e-30) {
      p.x = (b0 * (a11 * a22 - a12 * a12) - a01 * (b1 * a22 - a12 * b2) + a02 * (b1 * a12 - a11 * b2)) / det;
      p.y = (a00 * (b1 * a22 - a12 * b2) - b0 * (a01 * a22 - a12 * a02) + a02 * (a01 * b2 - b1 * a02)) / det;
      p.z = (a00 * (a11 * b2 - b1 * a12) - a01 * (a01 * b2 - b1 * a02) + b0 * (a01 * a12 - a11 * a02)) / det;
      if ((p - avg).length() > cell) p = avg;                    // (out of its cell: the average)
    }
    pos[c * 3] = float(p.x); pos[c * 3 + 1] = float(p.y); pos[c * 3 + 2] = float(p.z);
  }
  for (int f = 0; f < B.faceCount(); f++) { int s = B.faceStart[size_t(f)]; for (int k = s + 1; k + 1 < B.faceStart[size_t(f) + 1]; k++) {
    uint32_t a = map[size_t(B.faceVerts[size_t(s)])], b = map[size_t(B.faceVerts[size_t(k)])], c = map[size_t(B.faceVerts[size_t(k + 1)])];
    if (a == b || b == c || a == c) continue;
    // (a triangle the merging turned over — facing against the one it came from — left out: no dark slivers)
    const Vec3 o = (B.pos[size_t(B.faceVerts[size_t(k)])] - B.pos[size_t(B.faceVerts[size_t(s)])]).cross(B.pos[size_t(B.faceVerts[size_t(k + 1)])] - B.pos[size_t(B.faceVerts[size_t(s)])]);
    auto P = [&](uint32_t i) { return Vec3(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]); };
    const Vec3 nn = (P(b) - P(a)).cross(P(c) - P(a));
    if (nn.dot(o) <= 0.2 * nn.length() * o.length()) continue;
    idx.push_back(a); idx.push_back(b); idx.push_back(c); } }
}
}  // namespace

namespace { const long long BVH_TRIS = 200000; }   // (from here a click / the wheel go through a hierarchy)

namespace {
// each edge once: by its lower vertex, the higher ones it meets (a table, no hashing)
long long edgeCount(const PolyMesh& m) {
  const size_t nv = m.pos.size(); const int nf = m.faceCount(); std::vector<uint32_t> off(nv + 1, 0);
  for (int f = 0; f < nf; f++) { int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) { int a = m.faceVerts[size_t(k)], b = m.faceVerts[size_t(k + 1 < e ? k + 1 : s)]; if (a != b) off[size_t(std::min(a, b)) + 1]++; } }
  for (size_t v = 0; v < nv; v++) off[v + 1] += off[v];
  std::vector<uint32_t> hi(off[nv]), at(off.begin(), off.end() - 1);
  for (int f = 0; f < nf; f++) { int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) { int a = m.faceVerts[size_t(k)], b = m.faceVerts[size_t(k + 1 < e ? k + 1 : s)]; if (a != b) hi[at[size_t(std::min(a, b))]++] = uint32_t(std::max(a, b)); } }
  long long n = 0;
  for (size_t v = 0; v < nv; v++) for (uint32_t i = off[v]; i < off[v + 1]; i++) { bool dup = false; for (uint32_t j = off[v]; j < i && !dup; j++) dup = hi[j] == hi[i]; n += !dup; }
  return n;
}
}  // namespace

Viewport::MeshStats Viewport::modelStats() {
  MeshStats S; std::vector<const Item*> list;
  auto counts = [&](const Item& it) { return it.poly && scene_->isVisible(it) && (it.type == ItemType::IMesh || it.type == ItemType::Mesh || it.type == ItemType::Head); };
  for (int id : sel_.items) if (const Item* it = scene_->item(id); it && counts(*it)) list.push_back(it);
  S.all = list.empty(); if (S.all) for (auto& [id, it] : scene_->items) if (counts(it)) list.push_back(&it);
  for (auto it = statEdges_.begin(); it != statEdges_.end();) { if (it->second.poly.expired()) it = statEdges_.erase(it); else ++it; }
  bool edgesKnown = true;
  for (const Item* it : list) {
    const PolyMesh& P = *it->poly; S.meshes++; S.verts += (long long)P.pos.size(); S.faces += P.faceCount();
    S.triangulated += (long long)P.faceVerts.size() - 2LL * P.faceCount();
    auto& slot = statEdges_[it->poly.get()];
    if (slot.poly.lock() != it->poly) {                          // (counted once per geometry — the edges in the background when big)
      slot.poly = it->poly; slot.edges = std::make_shared<std::atomic<long long>>(-1); slot.quads = slot.tris = slot.ngons = 0;
      for (int f = 0; f < P.faceCount(); f++) { int n = P.faceStart[size_t(f) + 1] - P.faceStart[size_t(f)]; if (n == 3) slot.tris++; else if (n == 4) slot.quads++; else if (n > 4) slot.ngons++; }
      if (P.faceVerts.size() < 400000) *slot.edges = edgeCount(P);
      else { auto e = slot.edges; std::shared_ptr<PolyMesh> keep = it->poly; std::thread([e, keep] { *e = edgeCount(*keep); }).detach(); }
    }
    S.quads += slot.quads; S.tris += slot.tris; S.ngons += slot.ngons;
    long long e = *slot.edges; if (e < 0) edgesKnown = false; else S.edges += e;
  }
  if (!edgesKnown) S.edges = -1;
  return S;
}

long long Viewport::lodTris() { static const long long v = qEnvironmentVariableIsSet("HM_LOD_TRIS") ? qEnvironmentVariable("HM_LOD_TRIS").toLongLong() : 1000000; return v; }

void Viewport::heavyBuild(HeavyJob& J) {
  const PolyMesh& B = *J.src;
  // the hierarchy (the file's space): its triangles as the picking makes them (a fan per polygon, in order)
  std::vector<uint32_t> tris; tris.reserve(size_t(trisOf(B)) * 3); J.t2f.reserve(size_t(trisOf(B)));
  for (int f = 0; f < B.faceCount(); f++) { int s = B.faceStart[size_t(f)]; for (int k = s + 1; k + 1 < B.faceStart[size_t(f) + 1]; k++) {
    tris.push_back(uint32_t(B.faceVerts[size_t(s)])); tris.push_back(uint32_t(B.faceVerts[size_t(k)])); tris.push_back(uint32_t(B.faceVerts[size_t(k + 1)])); J.t2f.push_back(f); } }
  if (J.cancel) return;
  auto bvh = std::make_shared<topo::Bvh>(); bvh->build(B.pos, tris); J.bvh = bvh; J.bvhDone = true;   // (taken at once — the LOD may take longer)
  if (J.cancel || !J.wantLod) return;
  // the LOD: huge — clustered first, then the quadric collapses
  std::vector<float> pos; std::vector<uint32_t> idx; std::vector<int> src;
  cluster(B, pos, idx, src, LOD_TARGET / 2);
  if (J.cancel) return;
  topo::QemResult r; r.pos = std::move(pos); r.idx = std::move(idx); r.src.resize(r.pos.size() / 3); for (size_t v = 0; v < r.src.size(); v++) r.src[v] = int(v);
  // as a mesh: each vertex's group / material / UV from one of the polygons at its original vertex
  std::vector<int> firstCorner(B.pos.size(), -1); for (size_t k = 0; k < B.faceVerts.size(); k++) if (firstCorner[size_t(B.faceVerts[k])] < 0) firstCorner[size_t(B.faceVerts[k])] = int(k);
  std::vector<int> faceOf(B.faceVerts.size()); for (int f = 0; f < B.faceCount(); f++) for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) faceOf[size_t(k)] = f;
  auto L = std::make_shared<PolyMesh>(); L->groupNames = B.groupNames; L->matNames = B.matNames; L->matMaps = B.matMaps; L->matColor = B.matColor; L->matAlpha = B.matAlpha; L->flipY = B.flipY;
  const size_t nv = r.pos.size() / 3; L->pos.resize(nv); std::vector<int> corner(nv, -1);
  const bool pp = B.vcol.size() == B.pos.size() && !B.vcol.empty(); if (pp) L->vcol.assign(nv, {1, 1, 1});   // (polypaint: each vertex its original's colour)
  for (size_t v = 0; v < nv; v++) { L->pos[v] = {r.pos[v * 3], r.pos[v * 3 + 1], r.pos[v * 3 + 2]}; int o = src[size_t(r.src[v])]; corner[v] = o >= 0 && size_t(o) < firstCorner.size() ? firstCorner[size_t(o)] : -1; }
  if (pp) for (size_t v = 0; v < nv; v++) { const int o = src[size_t(r.src[v])]; if (o >= 0 && size_t(o) < B.vcol.size()) L->vcol[v] = B.vcol[size_t(o)]; }
  const bool uv = B.cornerUV.size() == B.faceVerts.size() && !B.cornerUV.empty(), fg = int(B.faceGroup.size()) == B.faceCount(), fm = int(B.faceMat.size()) == B.faceCount();
  for (size_t t = 0; t + 2 < r.idx.size(); t += 3) {
    int q[3] = {int(r.idx[t]), int(r.idx[t + 1]), int(r.idx[t + 2])}; L->addFace(q, 3);
    int k0 = corner[size_t(q[0])], f0 = k0 >= 0 ? faceOf[size_t(k0)] : -1;
    if (fg) L->faceGroup.push_back(f0 >= 0 ? B.faceGroup[size_t(f0)] : 0);
    if (fm) L->faceMat.push_back(f0 >= 0 ? B.faceMat[size_t(f0)] : 0);
    if (uv) for (int j = 0; j < 3; j++) { int k = corner[size_t(q[j])]; L->cornerUV.push_back(k >= 0 ? B.cornerUV[size_t(k)] : std::array<float, 2>{0, 0}); }
  }
  J.lod = L;
}

// (beforeFrame, and a timer while something waits): the heavy meshes' LOD / hierarchy started, finished, dropped
void Viewport::heavyTick() {
  const qint64 now = QDateTime::currentMSecsSinceEpoch(); bool waiting = false, got = false;
  std::set<int> alive;
  for (auto& [id, it] : scene_->items) {
    if (it.type != ItemType::IMesh) continue; auto B = baseSp(it); if (!B || trisOf(*B) < BVH_TRIS) continue;
    alive.insert(id); HeavySlot& s = heavy_[id]; s.wantLod = trisOf(*B) >= lodTris();
    if (s.src.lock() != B) { s.src = B; s.lod.reset(); s.bvh.reset(); s.t2f.clear(); s.seen = now; if (s.job) s.job->cancel = true; s.job.reset(); }
    if (s.job && s.job->bvhDone && !s.bvh && s.job->src == B && !s.job->cancel) { s.bvh = s.job->bvh; s.t2f = s.job->t2f; }   // (the hierarchy first)
    if (s.job && s.job->done) {
      if (s.job->src == B && !s.job->cancel) { s.lod = s.job->lod; if (!s.bvh) { s.bvh = s.job->bvh; s.t2f = std::move(s.job->t2f); } got = s.lod != nullptr; }
      s.job.reset();
    }
    if ((!s.bvh || (s.wantLod && !s.lod)) && !s.job) {
      if (now - s.seen >= 400 && drag_ == Drag::None) {
        auto J = std::make_shared<HeavyJob>(); J->src = B; J->wantLod = s.wantLod; s.job = J;
        std::thread([J] { heavyBuild(*J); J->done = true; }).detach();
      }
      waiting = true;
    }
    if (s.job) waiting = true;
  }
  for (auto it = heavy_.begin(); it != heavy_.end();) { if (alive.count(it->first)) { ++it; continue; } if (it->second.job) it->second.job->cancel = true; it = heavy_.erase(it); }
  if (got) { staticRev_++; requestUpdate(); }
  if (maskProxyTick()) waiting = true;                         // (the snap mask's proxies of the high-poly meshes too)
  if (waiting) { if (!heavyPoll_) { heavyPoll_ = new QTimer(this); heavyPoll_->setInterval(150); connect(heavyPoll_, &QTimer::timeout, this, [this] { heavyTick(); }); } if (!heavyPoll_->isActive()) heavyPoll_->start(); }
  else if (heavyPoll_) heavyPoll_->stop();
}
void Viewport::heavyStale(int m) { auto f = heavy_.find(m); if (f == heavy_.end()) return; f->second.lod.reset(); f->second.bvh.reset(); f->second.seen = QDateTime::currentMSecsSinceEpoch(); if (f->second.job) f->second.job->cancel = true; f->second.job.reset(); f->second.src.reset(); }

std::shared_ptr<const PolyMesh> Viewport::lodOf(int id) const {
  auto f = heavy_.find(id); if (f == heavy_.end() || !f->second.lod) return nullptr;
  const Item* it = scene_->item(id); if (!it || f->second.src.lock() != baseSp(*it)) return nullptr;
  return f->second.lod;
}
bool Viewport::navigating() const {
  return drag_ == Drag::Orbit || drag_ == Drag::Pan || drag_ == Drag::Dolly || spinNav_ || (navAnim_ && navAnim_->isActive())
      || QDateTime::currentMSecsSinceEpoch() - wheelAt_ < 280;
}

// a ray against a heavy mesh's hierarchy (the file's space — the ray taken there and back): nullopt — none ready (the
// caller tries every triangle), else the hit or a miss
std::optional<std::optional<RayHit>> Viewport::heavyRay(int id, const Ray& r, bool exactTri) const {
  (void)exactTri;                                              // (the hit's triangle always as the pick cache numbers them)
  auto f = heavy_.find(id); if (f == heavy_.end() || !f->second.bvh) return std::nullopt;
  const Item* it = scene_->item(id); if (!it || f->second.src.lock() != baseSp(*it)) return std::nullopt;
  const HeavySlot& S = f->second;
  // (its hidden polygons — polygroups, H — their triangles passed through; the rest numbered as the pick cache does)
  if (const QByteArray hs = meshmods::hiddenSig(*it); S.hidSig != hs) {
    S.hidSig = hs; S.hidTri.clear(); S.pcOf.clear(); S.pc2f.clear();
    const PolyMesh* B = it->base ? it->base.get() : it->poly.get(); const std::vector<char> hf = meshmods::hiddenFaces(*it, *B);
    if (!hf.empty()) { S.hidTri.resize(S.t2f.size()); S.pcOf.resize(S.t2f.size()); int k = 0;
      for (size_t t = 0; t < S.t2f.size(); t++) { const int fi = S.t2f[t]; const bool h = fi >= 0 && size_t(fi) < hf.size() && hf[size_t(fi)]; S.hidTri[t] = h; S.pcOf[t] = h ? -1 : k++; if (!h) S.pc2f.push_back(fi); } }
  }
  Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse();
  Vec3 o = Mi.apply(r.o); const Vec3 d = Mi.lin(r.d); const double eps = std::max(1e-9, S.bvh->nodes.empty() ? 1e-6 : S.bvh->nodes[0].b.size().length() * 1e-7);
  std::optional<std::pair<double, int>> h;
  for (int pass = 0; pass < 64; pass++) {
    h = S.bvh->ray(o, d, 1e300); if (!h) return std::optional<RayHit>{};
    if (S.hidTri.empty() || !S.hidTri[size_t(h->second)]) break;
    o = o + d * (h->first + eps); h.reset();                   // (a hidden one: on behind it)
  }
  if (!h) return std::optional<RayHit>{};
  Vec3 pb = o + d * h->first, pw = M.apply(pb);
  const auto& P = S.bvh->P; const auto& I = S.bvh->idx; size_t t = size_t(h->second);
  Vec3 n = M.lin((P[I[t * 3 + 1]] - P[I[t * 3]]).cross(P[I[t * 3 + 2]] - P[I[t * 3]])).normalized();
  return std::optional<RayHit>{RayHit{id, (pw - r.o).dot(r.d), pw, n, S.pcOf.empty() ? h->second : S.pcOf[t]}};
}

}  // namespace hm

namespace hm {
// what is picked now, for the stats card: the elements of the mesh being edited in a component mode (one vertex or a
// group of them shown apart), else the objects
QString Viewport::pickedInfo() const {
  auto many = [](size_t n, const QString& one, const QString& more) { return QString("%1 %2").arg(n).arg(n == 1 ? one : more); };
  if (room() == "topo" && topoMode != "object") {
    if (tsel_.id < 0 || tsel_.id != topoMesh()) return tr("nothing");
    const size_t n = topoMode == "vert" ? tsel_.v.size() : topoMode == "edge" ? tsel_.e.size() : tsel_.f.size(); if (!n) return tr("nothing");
    return topoMode == "vert" ? many(n, tr("vertex"), tr("vertices")) : topoMode == "edge" ? many(n, tr("edge"), tr("edges")) : many(n, tr("polygon"), tr("polygons")); }
  return sel_.items.empty() ? tr("nothing") : many(sel_.items.size(), tr("object"), tr("objects"));
}
}  // namespace hm
