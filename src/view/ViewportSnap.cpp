// The element snap: a drag of picked vertices takes hold of one of them — the one taken hold of goes onto a vertex, a
// point of an edge, of a polygon of the shown meshes, or a node of the view's grid; the rest go with it by the same
// offset (a head torn off put back by its neck's vertex onto the neck's: it stands in place, not folded into a point).
// The views' grids: each view its own, shown or not, of its own step — the floor for the perspective and the top views,
// a wall for the side ones.
#include "scene/MeshMods.h"
#include "view/SceneGeometry.h"
#include "view/Viewport.h"
#include <QSettings>
#include <cmath>
#include <map>

namespace hm {

namespace {
std::map<QString, Viewport::ViewGrid>& gridsCache() { static std::map<QString, Viewport::ViewGrid> g; return g; }
QString gridKey(const QString& view) { return view == "ortho" ? QString("persp") : view; }   // (the free orthographic camera: the floor's grid)
}  // namespace

Viewport::ViewGrid Viewport::viewGrid(const QString& view) const {
  const QString k = gridKey(view); auto& C = gridsCache(); auto f = C.find(k); if (f != C.end()) return f->second;
  QSettings st; ViewGrid g; g.on = st.value("grid/" + k + "/on", false).toBool(); g.step = std::max(1e-4, st.value("grid/" + k + "/step", 1.0).toDouble());
  C[k] = g; return g;
}
void Viewport::setViewGrid(const QString& view, const ViewGrid& g) {
  const QString k = gridKey(view); gridsCache()[k] = g; QSettings st; st.setValue("grid/" + k + "/on", g.on); st.setValue("grid/" + k + "/step", g.step);
  staticRev_++; requestUpdate();
}
double Viewport::floorY() const {
  const Box3 b = scene_->bounds(); const double hh = b.empty() ? 1 : std::max(1e-9, b.size().y);
  return (floor.zero ? 0 : (b.empty() ? 0 : b.min.y)) + floor.dy * hh;
}

std::optional<Vec3> Viewport::elemSnap(const QPointF& mp, int m, const std::set<int>& moving, const Vec3& from) {
  (void)from;
  const QString mode = tset.esnapMode; const Ray R = ray(mp);
  if (mode == "grid") {                                         // (the view's grid: the node nearest where the pointer is on its plane)
    const ViewGrid g = viewGrid(cam_.view); if (!g.on) return std::nullopt;
    const bool wallZ = cam_.view == "front" || cam_.view == "back", wallX = cam_.view == "left" || cam_.view == "right";
    const Vec3 n = wallZ ? Vec3(0, 0, 1) : wallX ? Vec3(1, 0, 0) : Vec3(0, 1, 0); const Vec3 o = wallZ || wallX ? Vec3() : Vec3(0, floorY(), 0);
    auto h = planeHit(R, o, n); if (!h) return std::nullopt; Vec3 p = *h; const double s = g.step;
    if (!wallX) p.x = std::round(p.x / s) * s; if (!wallZ) p.z = std::round(p.z / s) * s; if (wallZ || wallX) p.y = std::round(p.y / s) * s;
    return p;
  }
  Projector P = projector(); const double px = tset.esnapPx;
  std::vector<int> ids = topoMeshes();
  for (auto& [id, it] : scene_->items) if (it.type == ItemType::IMesh && it.poly && it.raw.value("tref").toBool() && scene_->isVisible(it)) ids.push_back(id);
  std::optional<Vec3> best; double bd = px;
  auto movingFace = [&](const PolyMesh& B, int f) { for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) if (moving.count(B.faceVerts[size_t(k)])) return true; return false; };
  if (mode == "vert" || mode == "edge") {
    for (int id : ids) { const Item* it = scene_->item(id); if (!it || !it->poly || it->poly->faceCount() > 300000) continue;   // (a high-poly one: too many to look through)
      const PolyMesh& S = *it->poly; std::vector<ScreenPt> sp(S.pos.size()); for (size_t v = 0; v < S.pos.size(); v++) sp[v] = P.toScreen(S.pos[v]);
      if (mode == "vert") { for (size_t v = 0; v < S.pos.size(); v++) { if (id == m && moving.count(int(v))) continue; const ScreenPt& s = sp[v]; if (s.z >= 1) continue;
          const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (d < bd) { bd = d; best = S.pos[v]; } } continue; }
      for (int f = 0; f < S.faceCount(); f++) { const int s0 = S.faceStart[size_t(f)], e0 = S.faceStart[size_t(f) + 1];
        for (int k = s0; k < e0; k++) { const int a = S.faceVerts[size_t(k)], b = S.faceVerts[size_t(k + 1 < e0 ? k + 1 : s0)];
          if (id == m && (moving.count(a) || moving.count(b))) continue; const ScreenPt &sa = sp[size_t(a)], &sb = sp[size_t(b)]; if (sa.z >= 1 || sb.z >= 1) continue;
          const double dx = sb.x - sa.x, dy = sb.y - sa.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? std::clamp(((mp.x() - sa.x) * dx + (mp.y() - sa.y) * dy) / l2, 0.0, 1.0) : 0.0;
          const double d = std::hypot(sa.x + dx * t - mp.x(), sa.y + dy * t - mp.y()); if (d < bd) { bd = d; best = S.pos[size_t(a)] + (S.pos[size_t(b)] - S.pos[size_t(a)]) * t; } } } }
    return best;
  }
  // a polygon: the nearest surface under the pointer — of the mesh being moved, only its polygons that stay
  double bt = 1e300;
  for (int id : ids) { const Item* it = scene_->item(id); if (!it || !it->poly) continue;
    if (id != m) {
      if (auto hr = heavyRay(id, R, false)) { if (*hr && (*hr)->t < bt) { bt = (*hr)->t; best = (*hr)->point; } continue; }   // (a high-poly one: its own hierarchy)
      if (auto b = bvhOf(id, false)) if (auto h = b->ray(R.o, R.d, 1e12); h && h->first < bt) { bt = h->first; best = R.o + R.d * h->first; }
      continue; }
    const PolyMesh& S = *it->poly;
    for (int f = 0; f < S.faceCount(); f++) { if (movingFace(S, f)) continue; const int s0 = S.faceStart[size_t(f)];
      for (int k = s0 + 1; k + 1 < S.faceStart[size_t(f) + 1]; k++) { const Vec3 &a = S.pos[size_t(S.faceVerts[size_t(s0)])], &b = S.pos[size_t(S.faceVerts[size_t(k)])], &c = S.pos[size_t(S.faceVerts[size_t(k + 1)])];
        const Vec3 e1 = b - a, e2 = c - a, pv = R.d.cross(e2); const double det = e1.dot(pv); if (std::abs(det) < 1e-14) continue; const double inv = 1 / det; const Vec3 tv = R.o - a;
        const double u = tv.dot(pv) * inv; if (u < 0 || u > 1) continue; const Vec3 qv = tv.cross(e1); const double v = R.d.dot(qv) * inv; if (v < 0 || u + v > 1) continue;
        const double t = e2.dot(qv) * inv; if (t > 0 && t < bt) { bt = t; best = R.o + R.d * t; } } } }
  return best;
}

}  // namespace hm

namespace hm {
void Viewport::esnapOverlay(std::vector<Overlay>& out) const {
  if (room() != "topo" || !tset.esnap) return;
  Projector P = projector(); const QColor hot(0xff, 0xa0, 0x30), dim(0xc0, 0xc8, 0xd0);
  auto ring = [&](double x, double y, double r, const QColor& c) { Overlay o; o.color = c; o.closed = true; for (int i = 0; i < 20; i++) { const double a = 2 * M_PI * i / 20; o.pts.push_back({x + r * std::cos(a), y + r * std::sin(a)}); } out.push_back(o); };
  // (while a drag goes: what it could take near the pointer — the vertices, or the edges, of the shown meshes)
  const bool penDrag = drag_ == Drag::Topo && td_.type == 20 && tt_.drag == 1;   // (PolyPen dragging its vertices)
  if (drag_ == Drag::Topo && (td_.type == 3 || penDrag) && (tset.esnapMode == "vert" || tset.esnapMode == "edge")) {
    const std::vector<int>& V = penDrag ? tt_.verts : td_.verts; const int mm = penDrag ? tt_.m : td_.m; const std::set<int> mv(V.begin(), V.end()); const double R = 160; int shown = 0;
    std::vector<int> ids = topoMeshes(); for (auto& [id, it] : scene_->items) if (it.type == ItemType::IMesh && it.poly && it.raw.value("tref").toBool() && scene_->isVisible(it)) ids.push_back(id);
    for (int id : ids) { const Item* it = scene_->item(id); if (!it || !it->poly || it->poly->faceCount() > 300000) continue; const PolyMesh& S = *it->poly;
      if (tset.esnapMode == "vert") { for (size_t v = 0; v < S.pos.size() && shown < 600; v++) { if (id == mm && mv.count(int(v))) continue; const ScreenPt s = P.toScreen(S.pos[v]);
          if (s.z >= 1 || std::hypot(s.x - mouse_.x(), s.y - mouse_.y()) > R) continue; Overlay o; o.color = dim; o.closed = true; o.pts = {{s.x - 2, s.y - 2}, {s.x + 2, s.y - 2}, {s.x + 2, s.y + 2}, {s.x - 2, s.y + 2}}; out.push_back(o); shown++; } }
      else for (int f = 0; f < S.faceCount() && shown < 600; f++) { const int s0 = S.faceStart[size_t(f)], e0 = S.faceStart[size_t(f) + 1];
          for (int k = s0; k < e0; k++) { const int a = S.faceVerts[size_t(k)], b = S.faceVerts[size_t(k + 1 < e0 ? k + 1 : s0)]; if (id == mm && (mv.count(a) || mv.count(b))) continue;
            const ScreenPt sa = P.toScreen(S.pos[size_t(a)]), sb = P.toScreen(S.pos[size_t(b)]); if (sa.z >= 1 || sb.z >= 1) continue;
            if (std::hypot((sa.x + sb.x) / 2 - mouse_.x(), (sa.y + sb.y) / 2 - mouse_.y()) > R) continue; Overlay o; o.color = dim; o.closed = false; o.pts = {{sa.x, sa.y}, {sb.x, sb.y}}; out.push_back(o); shown++; } } } }
  if (esnapHit_) { const ScreenPt s = P.toScreen(*esnapHit_); if (s.z < 1) { ring(s.x, s.y, 7, hot); ring(s.x, s.y, 2.5, hot); } }   // (where the one taken hold of goes)
}
}  // namespace hm
