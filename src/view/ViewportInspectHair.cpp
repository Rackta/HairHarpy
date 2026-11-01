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

namespace hm {

void Viewport::loadHairInspect() {
  const QJsonObject o = QJsonDocument::fromJson(QSettings().value("inspectHair").toByteArray()).object();
  hinsp.near = o.value("near").toBool(); hinsp.twisted = o.value("twisted").toBool(); hinsp.degen = o.value("degen").toBool();
  hinsp.twistDeg = std::clamp(o.value("twistDeg").toDouble(30), 1.0, 90.0);
}
void Viewport::hinspChanged() {
  QSettings().setValue("inspectHair", QJsonDocument(QJsonObject{{"near", hinsp.near}, {"twisted", hinsp.twisted}, {"degen", hinsp.degen}, {"twistDeg", hinsp.twistDeg}}).toJson(QJsonDocument::Compact));
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
