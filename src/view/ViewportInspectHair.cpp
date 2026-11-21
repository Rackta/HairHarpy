// The Maranspector in the Hair room: the anchors practically in one place but not welded (the near misses — closer than
// 0.15 of the weld distance, yet not exactly in one place; on curves that share none), the degenerate curves (fewer than two anchors, no length, or anchors one on
// another — closer than an eighth of the loop step), the twisted walls (turning round their length by over the angle set
// within one loop step), and the summary of what Convert would make of the selected cocoons.
#include "scene/Convert.h"
#include "scene/Walls.h"
#include "view/Viewport.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <unordered_map>
#include <unordered_set>

namespace hm {

void Viewport::loadHairInspect() {
  const QJsonObject o = QJsonDocument::fromJson(QSettings().value("inspectHair").toByteArray()).object();
  hinsp.near = o.value("near").toBool(); hinsp.twisted = o.value("twisted").toBool(); hinsp.degen = o.value("degen").toBool(); hinsp.clip = o.value("clip").toBool();
  hinsp.twistDeg = std::clamp(o.value("twistDeg").toDouble(30), 1.0, 90.0);
}
void Viewport::hinspChanged() {
  QSettings().setValue("inspectHair", QJsonDocument(QJsonObject{{"near", hinsp.near}, {"twisted", hinsp.twisted}, {"degen", hinsp.degen}, {"clip", hinsp.clip}, {"twistDeg", hinsp.twistDeg}}).toJson(QJsonDocument::Compact));
  topoRev_++; requestUpdate();
}

// the weld distance (Auto weld points): set, else a quarter of the loop step
double Viewport::weldDistance() const {
  const double d = scene_->fileRaw.value("weld").toObject().value("dist").toDouble(0);
  return d > 0 ? d : 0.25 * projectStep(*scene_);
}

void Viewport::hairInspectDraw(TopoDraw& D) const {
  if (room() != "hair") return;
  const QColor nearC(0xff, 0x8c, 0x1a), degC(0xff, 0x3a, 0x3a), twC(0x9c, 0xff, 0x2e);
  // ---- the anchors almost touching, not welded ----
  if (hinsp.near) {
    std::unordered_map<int, std::vector<int>> curvesOf;              // anchor → its curves
    for (const Item* c : scene_->curves()) if (c->ring < 0 && scene_->isVisible(*c)) for (int a : c->anchors) curvesOf[a].push_back(c->id);
    const double thr = 0.15 * weldDistance(); const double cell = std::max(1e-9, thr);   // (practically one place)
    std::unordered_map<long long, std::vector<int>> grid;
    auto key = [&](long long x, long long y, long long z) { return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
    auto cellOf = [&](const Vec3& p) { return std::array<long long, 3>{(long long)std::floor(p.x / cell), (long long)std::floor(p.y / cell), (long long)std::floor(p.z / cell)}; };
    for (auto& [a, cs] : curvesOf) if (const Anchor* an = scene_->anchor(a)) { auto c = cellOf(an->pos); grid[key(c[0], c[1], c[2])].push_back(a); }
    TopoDraw::Lines L; L.c = nearC; L.w = 3; TopoDraw::Pts pt; pt.size = 11; pt.xray = true;
    for (auto& [a, cs] : curvesOf) {
      const Anchor* A = scene_->anchor(a); if (!A) continue; auto c = cellOf(A->pos);
      for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) for (int dz = -1; dz <= 1; dz++) {
        auto g = grid.find(key(c[0] + dx, c[1] + dy, c[2] + dz)); if (g == grid.end()) continue;
        for (int b : g->second) {
          if (b <= a) continue; const Anchor* Bn = scene_->anchor(b); if (!Bn) continue;
          const double dd = A->pos.distanceTo(Bn->pos); if (dd >= thr || dd < 1e-4 * thr) continue;   // (exactly one place: snapped there on purpose — welded anyway; only the near misses)

          bool shared = false; for (int x : cs) for (int y : curvesOf[b]) if (x == y) shared = true;   // (on one curve — its own neighbours, not a joint)
          if (shared) continue;
          // (only within one lock — its group: two locks lying close is no fault, they are never welded — 21.11, the author: "the
          //  Inspector reacts to anchors of different locks and marks them")
          bool oneLock = false; for (int x : cs) for (int y : curvesOf[b]) { const Item* X = scene_->item(x); const Item* Y = scene_->item(y); if (X && Y && X->parent == Y->parent) oneLock = true; }
          if (!oneLock) continue;
          L.p.push_back(A->pos); L.p.push_back(Bn->pos); for (const Vec3* q : {&A->pos, &Bn->pos}) { pt.p.push_back(*q); pt.c.push_back(nearC); }
        }
      }
    }
    if (!L.p.empty()) D.lines.push_back(std::move(L)); if (!pt.p.empty()) D.pts.push_back(std::move(pt));
  }
  // ---- the degenerate curves ----
  if (hinsp.degen) {
    const double tiny = 0.12 * projectStep(*scene_);           // (anchors closer than an eighth of the loop step: on one another)
    // (a curve with no length / under two anchors: all of it lit; anchors one on another: the curve lit thin, the place
    // itself — a big yellow mark over everything)
    TopoDraw::Lines L; L.c = degC; L.w = 4; TopoDraw::Lines Lt; Lt.c = degC; Lt.w = 1.5f; TopoDraw::Pts pt; pt.size = 10; pt.xray = true;
    TopoDraw::Pts st; st.size = 20; st.xray = true; const QColor stC(0xff, 0xe1, 0x4a);
    for (const Item* c : scene_->curves()) {
      if (c->ring >= 0 || !scene_->isVisible(*c)) continue;
      std::vector<Vec3> P; for (int a : c->anchors) if (const Anchor* an = scene_->anchor(a)) P.push_back(an->pos);
      double len = 0; std::vector<Vec3> stacks; for (size_t i = 0; i + 1 < P.size(); i++) { double d = P[i].distanceTo(P[i + 1]); len += d; if (d < tiny) stacks.push_back((P[i] + P[i + 1]) * 0.5); }
      const bool whole = P.size() < 2 || len < tiny;
      if (!whole && stacks.empty()) continue;
      auto cs = hair_.curves.find(c->id); const auto& S = cs != hair_.curves.end() && cs->second.p.size() >= 2 ? cs->second.p : P;
      TopoDraw::Lines& LL = whole ? L : Lt;
      for (size_t i = 0; i + 1 < S.size(); i++) { LL.p.push_back(S[i]); LL.p.push_back(S[i + 1]); }
      if (whole) for (const Vec3& q : P) { pt.p.push_back(q); pt.c.push_back(degC); }
      for (const Vec3& q : stacks) { st.p.push_back(q); st.c.push_back(stC); }
    }
    if (!L.p.empty()) D.lines.push_back(std::move(L)); if (!Lt.p.empty()) D.lines.push_back(std::move(Lt));
    if (!pt.p.empty()) D.pts.push_back(std::move(pt)); if (!st.p.empty()) D.pts.push_back(std::move(st));
  }
  // ---- the twisted walls: over one loop step along the lock the wall turns round its length by more than the angle set
  // (the converted polygons there come out bent); its pointed ends — too narrow to tell — left out ----
  if (hinsp.twisted) {
    TopoDraw::Tris T; T.c = twC; T.op = 0.6f; const double lim = std::cos(hinsp.twistDeg * M_PI / 180), step = projectStep(*scene_);
    for (const auto& [sid, W] : hair_.walls) {
      if (const Item* s = scene_->item(sid); !s || !scene_->isVisible(*s)) continue;
      for (const auto& g : W.grids) {
        if (g.R < 2 || g.M < 2) continue;
        auto at = [&](int r, int c) -> const Vec3& { return W.pos[size_t(g.start + r * g.M + c)]; };
        // (each row runs across the lock: its span, its middle; the length along the middles)
        std::vector<Vec3> across(size_t(g.R)), mid(size_t(g.R)); std::vector<double> arc(size_t(g.R), 0); double widest = 0;
        for (int r = 0; r < g.R; r++) { across[size_t(r)] = at(r, g.M - 1) - at(r, 0); mid[size_t(r)] = (at(r, g.M - 1) + at(r, 0)) * 0.5; widest = std::max(widest, across[size_t(r)].length());
          if (r) arc[size_t(r)] = arc[size_t(r - 1)] + mid[size_t(r)].distanceTo(mid[size_t(r - 1)]); }
        std::vector<char> bad(size_t(g.R), 0);
        for (int r = 0, r2 = 0; r < g.R; r++) {
          while (r2 < g.R && arc[size_t(r2)] - arc[size_t(r)] < step) r2++;
          if (r2 >= g.R) break;
          if (across[size_t(r)].length() < 0.25 * widest || across[size_t(r2)].length() < 0.25 * widest) continue;   // (a pointed end)
          const Vec3 t = (mid[size_t(r2)] - mid[size_t(r)]).normalized();
          Vec3 a = across[size_t(r)] - t * across[size_t(r)].dot(t), b = across[size_t(r2)] - t * across[size_t(r2)].dot(t);   // (seen along the lock)
          if (a.lengthSq() <= 0 || b.lengthSq() <= 0) continue;
          if (a.normalized().dot(b.normalized()) < lim) for (int q = r; q < r2; q++) bad[size_t(q)] = 1;
        }
        for (int r = 0; r + 1 < g.R; r++) if (bad[size_t(r)]) for (int c = 0; c + 1 < g.M; c++) {
          T.p.push_back(at(r, c)); T.p.push_back(at(r, c + 1)); T.p.push_back(at(r + 1, c + 1));
          T.p.push_back(at(r, c)); T.p.push_back(at(r + 1, c + 1)); T.p.push_back(at(r + 1, c)); }
      }
    }
    if (!T.p.empty()) D.tris.push_back(std::move(T));
  }
  // ---- a lock clipping through itself (21.11, the author: "add to the Inspector a highlight of the place where a lock clips
  // through itself"): its walls passing through one another, or a wall through itself — the line where they cross, magenta,
  // over everything; the polygons on it tinted ----
  if (hinsp.clip) {
    const auto& C = selfClips();
    const QColor clC(0xff, 0x3d, 0xd0);
    if (!C.tris.empty()) { TopoDraw::Tris T; T.c = clC; T.op = 0.35f; T.p = C.tris; D.tris.push_back(std::move(T)); }
    if (!C.segs.empty()) { TopoDraw::Lines L; L.c = clC; L.w = 4; L.xray = 0.8f; L.p = C.segs; D.lines.push_back(std::move(L)); }
  }
}

namespace {
// the segment p–q through triangle abc: where (Möller–Trumbore)
bool segTri(const Vec3& p, const Vec3& q, const Vec3& a, const Vec3& b, const Vec3& c, Vec3& out) {
  const Vec3 e1 = b - a, e2 = c - a, d = q - p, h = d.cross(e2); const double det = e1.dot(h);
  if (std::abs(det) <= 1e-12 * e1.length() * e2.length() * d.length()) return false;   // (parallel)
  const double f = 1 / det; const Vec3 s = p - a; const double u = f * s.dot(h); if (u < 0 || u > 1) return false;
  const Vec3 qv = s.cross(e1); const double v = f * d.dot(qv); if (v < 0 || u + v > 1) return false;
  const double t = f * e2.dot(qv); if (t < 0 || t > 1) return false;
  out = p + d * t; return true;
}
double segDist(const Vec3& x, const Vec3& a, const Vec3& b) { const Vec3 ab = b - a; const double L = ab.lengthSq(); const double t = L > 0 ? std::clamp((x - a).dot(ab) / L, 0.0, 1.0) : 0; return x.distanceTo(a + ab * t); }
}  // namespace

// (each lock's walls — the walls in one group — as triangles, their own sides only (a second surface lies on its wall); every two
// that cross and share no corner: the line they cross along. Crossings along the edge of both — two walls meeting on a curve they
// share, each sampled its own way — are no clip. Kept until the scene changes; while a drag goes on, the last one shown)
const Viewport::SelfClips& Viewport::selfClips() const {
  const bool dragging = drag_ != Drag::None;
  if (clipRev_ == scene_->revision || (dragging && clipRev_ >= 0)) return clips_;
  clipRev_ = scene_->revision; clips_ = {};
  struct Tri { Vec3 a, b, c; int lock; Vec3 lo, hi; int wall; };
  std::vector<Tri> T; double ext = 0;
  struct Rim { std::vector<std::pair<Vec3, Vec3>> segs; double width = 0; }; std::unordered_map<int, Rim> rims;   // (each wall's two sides; its mean width)
  for (const auto& [sid, W] : hair_.walls) {
    const Item* s = scene_->item(sid); if (!s || !scene_->isVisible(*s)) continue;
    const int lock = s->parent;
    for (const auto& g : W.grids) {
      if (g.twin || g.R < 2 || g.M < 2) continue;
      auto at = [&](int r, int c) -> const Vec3& { return W.pos[size_t(g.start + r * g.M + c)]; };
      { Rim& rm = rims[sid]; double wsum = 0; for (int r = 0; r < g.R; r++) { wsum += at(r, 0).distanceTo(at(r, g.M - 1)); if (r) { rm.segs.push_back({at(r - 1, 0), at(r, 0)}); rm.segs.push_back({at(r - 1, g.M - 1), at(r, g.M - 1)}); } }
        rm.width = std::max(rm.width, wsum / g.R); }
      for (int r = 0; r + 1 < g.R; r++) for (int c = 0; c + 1 < g.M; c++)
        for (auto [p, q, w] : {std::tuple{at(r, c), at(r, c + 1), at(r + 1, c + 1)}, std::tuple{at(r, c), at(r + 1, c + 1), at(r + 1, c)}}) {
          if ((q - p).cross(w - p).lengthSq() <= 0) continue;   // (a pointed end's collapsed corner)
          Tri t{p, q, w, lock, p, p, sid}; for (const Vec3* v : {&q, &w}) { t.lo = {std::min(t.lo.x, v->x), std::min(t.lo.y, v->y), std::min(t.lo.z, v->z)}; t.hi = {std::max(t.hi.x, v->x), std::max(t.hi.y, v->y), std::max(t.hi.z, v->z)}; }
          ext += std::max({t.hi.x - t.lo.x, t.hi.y - t.lo.y, t.hi.z - t.lo.z}); T.push_back(t);
        }
    }
  }
  if (T.size() < 2) return clips_;
  // (twin walls: on both sides the same pieces of curve — a lock whose tip splits in two: two walls on its trunk, one on the other, each
  //  going on to its own tip. Their crossing where they part is the split, no clip — 21.11, the author: "the Inspector shows the double
  //  wall needed for the diverging tip as a clipping problem")
  auto pieces = [&](int cid) { std::set<int> o; const Item* c = scene_->item(cid); if (!c) return o; if (c->hasPath()) for (auto& p : c->path) o.insert(p[0]); else o.insert(cid); return o; };
  auto meets = [](const std::set<int>& x, const std::set<int>& y) { for (int v : x) if (y.count(v)) return true; return false; };
  auto twins = [&](int w1, int w2) {
    const Item* A = scene_->item(w1); const Item* B = scene_->item(w2); if (!A || !B || w1 == w2) return false;
    const auto a1 = pieces(A->a), b1 = pieces(A->b), a2 = pieces(B->a), b2 = pieces(B->b);
    return (meets(a1, a2) && meets(b1, b2)) || (meets(a1, b2) && meets(b1, a2)); };
  std::map<std::pair<int, int>, bool> twinOf;
  const double cell = std::max(1e-9, ext / double(T.size()) * 1.5), step = projectStep(*scene_);
  const double same = 1e-4 * step, tol = 0.03 * step;
  std::unordered_map<long long, std::vector<int>> grid;
  auto key = [](long long x, long long y, long long z) { return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); };
  for (int i = 0; i < int(T.size()); i++) {
    const Tri& t = T[size_t(i)];
    const long long x0 = (long long)std::floor(t.lo.x / cell), x1 = (long long)std::floor(t.hi.x / cell), y0 = (long long)std::floor(t.lo.y / cell), y1 = (long long)std::floor(t.hi.y / cell), z0 = (long long)std::floor(t.lo.z / cell), z1 = (long long)std::floor(t.hi.z / cell);
    if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 512) continue;   // (a sliver across half the scene: left out)
    for (long long x = x0; x <= x1; x++) for (long long y = y0; y <= y1; y++) for (long long z = z0; z <= z1; z++) grid[key(x, y, z)].push_back(i);
  }
  std::unordered_set<long long> tested, lit;
  auto onEdge = [&](const Vec3& x, const Tri& t) { return std::min({segDist(x, t.a, t.b), segDist(x, t.b, t.c), segDist(x, t.c, t.a)}) < tol; };
  for (const auto& [k, v] : grid) for (size_t m = 0; m < v.size(); m++) for (size_t n = m + 1; n < v.size(); n++) {
    const int i = std::min(v[m], v[n]), j = std::max(v[m], v[n]); const Tri &A = T[size_t(i)], &B = T[size_t(j)];
    if (A.lock != B.lock) continue;
    if (A.lo.x > B.hi.x || B.lo.x > A.hi.x || A.lo.y > B.hi.y || B.lo.y > A.hi.y || A.lo.z > B.hi.z || B.lo.z > A.hi.z) continue;
    if (!tested.insert((long long)i * 4000037LL + j).second) continue;
    bool touch = false; for (const Vec3* p : {&A.a, &A.b, &A.c}) for (const Vec3* q : {&B.a, &B.b, &B.c}) if (p->distanceTo(*q) <= same) touch = true;
    if (touch) continue;                                           // (neighbours: a corner in common)
    { const auto k = std::pair{std::min(A.wall, B.wall), std::max(A.wall, B.wall)}; auto f = twinOf.find(k); if (f == twinOf.end()) f = twinOf.emplace(k, twins(k.first, k.second)).first;
      if (f->second) continue; }
    // (lying on one another — under 12° between them: the double wall of a tip that splits, two walls on the same curves until they
    //  part — 21.11, the author: "the Inspector shows the double wall needed for the diverging tip as a clipping problem")
    { const Vec3 nA = (A.b - A.a).cross(A.c - A.a), nB = (B.b - B.a).cross(B.c - B.a); const double l = nA.length() * nB.length();
      if (l > 0 && std::abs(nA.dot(nB)) / l > std::cos(12 * M_PI / 180)) continue; }
    std::vector<Vec3> P; Vec3 h;
    for (auto [p, q] : {std::pair{&A.a, &A.b}, {&A.b, &A.c}, {&A.c, &A.a}}) if (segTri(*p, *q, B.a, B.b, B.c, h)) P.push_back(h);
    for (auto [p, q] : {std::pair{&B.a, &B.b}, {&B.b, &B.c}, {&B.c, &B.a}}) if (segTri(*p, *q, A.a, A.b, A.c, h)) P.push_back(h);
    if (P.size() < 2) continue;
    Vec3 e0 = P[0], e1 = P[1]; double best = -1;
    for (size_t a = 0; a < P.size(); a++) for (size_t b = a + 1; b < P.size(); b++) if (const double d = P[a].distanceTo(P[b]); d > best) { best = d; e0 = P[a]; e1 = P[b]; }
    if (best < 1e-3 * step) continue;
    if (onEdge(e0, A) && onEdge(e0, B) && onEdge(e1, A) && onEdge(e1, B)) continue;   // (along the rim of both: two walls on one curve)
    { const Vec3 mid = (e0 + e1) * 0.5; auto rimD = [&](int w) { double d = 1e300; for (auto& [p, q] : rims[w].segs) d = std::min(d, segDist(mid, p, q)); return d; };
      const double dA = rimD(A.wall), dB = rimD(B.wall), wA = std::max(1e-12, rims[A.wall].width), wB = std::max(1e-12, rims[B.wall].width);
      // (right at the side of both — under a twentieth of their width: the seam of two walls on one curve, each sampled its own way;
      //  on the Miku tails these lay at 0.02–0.03 of the width, the real clips from 0.1 to over the whole width)
      if (dA < 0.05 * wA && dB < 0.05 * wB) continue;
      { const Vec3 nA = (A.b - A.a).cross(A.c - A.a), nB = (B.b - B.a).cross(B.c - B.a); clips_.dbg.push_back({dA / wA, dB / wB, std::acos(std::min(1.0, std::abs(nA.dot(nB)) / (nA.length() * nB.length()))) * 180 / M_PI, double(A.wall), double(B.wall), mid.x, mid.y, mid.z}); } }
    clips_.segs.push_back(e0); clips_.segs.push_back(e1); clips_.n++; clips_.walls.insert({std::min(A.wall, B.wall), std::max(A.wall, B.wall)});
    for (int x : {i, j}) if (lit.insert(x).second) { const Tri& t = T[size_t(x)]; clips_.tris.push_back(t.a); clips_.tris.push_back(t.b); clips_.tris.push_back(t.c); }
  }
  return clips_;
}

// what Convert would make of the selected cocoons (each lock's own mesh, before the welding of locks): kept until the
// scene or the selection changes, worked out at most twice a second
Viewport::ConvSum Viewport::convertSummary() {
  QByteArray k = QByteArray::number(scene_->revision) + "|"; for (int id : sel_.items) k += QByteArray::number(id) + ",";
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (k == convKey_ || now - convAt_ < 500) return convSum_;
  convKey_ = k; convAt_ = now; convSum_ = {};
  CocoonSystem* cs = hair_.cocoonSystem(); if (!cs) return convSum_;
  const WeldSettings weld = weldSettings(*scene_); const double scale = sceneScale(*scene_);
  for (int id : sel_.items) {
    const Item* it = scene_->item(id); if (!it || it->type != ItemType::Cocoon) continue;
    LockInput in; if (!cs->lockInput(id, in)) continue;
    LockMesh r = ringMeshOf(in, hair_.walls, weld, scale); if (r.faces.empty()) continue;
    convSum_.locks++; convSum_.verts += int(r.pos.size()); convSum_.polys += int(r.faces.size());
    for (auto& f : r.faces) convSum_.tris += std::max(0, int(f.size()) - 2);
  }
  return convSum_;
}

}  // namespace hm
