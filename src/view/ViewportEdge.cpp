// ╱ The Edges tool: picking the polygon edges of imported / converted meshes (the polygon hit by the ray — its nearest
// edge; near the outline any edge within 8 px), edge loops, the shortest path, the box deselect; the selected path →
// curves and Mesh → curves & surfaces (src/scene/MeshCurves).
#include "scene/MeshCurves.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include "scene/MeshMods.h"
#include <QDateTime>
#include <cmath>
#include <set>

namespace hm {

namespace {
bool heavy(const PolyMesh& m) { return meshmods::heavy(m); }
double segDist2(double mx, double my, const ScreenPt& a, const ScreenPt& b) {
  double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy, t = l2 > 0 ? clamp(((mx - a.x) * vx + (my - a.y) * vy) / l2, 0, 1) : 0;
  return std::hypot(a.x + vx * t - mx, a.y + vy * t - my);
}
}  // namespace

std::optional<Viewport::EdgePick> Viewport::pickEdge(const QPointF& m, bool hover) const {
  std::vector<const Item*> list;
  for (auto& [id, it] : scene_->items) if ((it.type == ItemType::IMesh || it.type == ItemType::Mesh) && it.poly && scene_->isVisible(it) && !heavy(*it.poly)) list.push_back(&it);
  if (list.empty()) return std::nullopt;
  Projector P = projector();
  auto nearIn = [&](const Item* it, const meshc::Topo& T, const std::vector<int>& cand, double maxD) -> std::optional<EdgePick> {
    std::optional<EdgePick> best; double bd = maxD; const auto& W = it->poly->pos;
    for (int e : cand) { ScreenPt a = P.toScreen(W[size_t(T.ea[size_t(e)])]), b = P.toScreen(W[size_t(T.eb[size_t(e)])]); if (a.z > 1 || b.z > 1) continue;
      double d = segDist2(m.x(), m.y(), a, b); if (d < bd) { bd = d; best = EdgePick{it->id, e}; } }
    return best;
  };
  // the polygon under the cursor (each item's triangles fanned per polygon: triangle → polygon)
  std::vector<PickMesh> pm; std::vector<std::vector<int>> triFace;
  for (const Item* it : list) {
    PickMesh k; k.itemId = it->id; k.pos = &it->poly->pos; k.box = it->poly->bounds(); std::vector<int> tf;
    for (int f = 0; f < it->poly->faceCount(); f++) { int s = it->poly->faceStart[size_t(f)], e = it->poly->faceStart[size_t(f) + 1];
      for (int q = s + 1; q + 1 < e; q++) { k.tris.push_back(uint32_t(it->poly->faceVerts[size_t(s)])); k.tris.push_back(uint32_t(it->poly->faceVerts[size_t(q)])); k.tris.push_back(uint32_t(it->poly->faceVerts[size_t(q + 1)])); tf.push_back(f); } }
    pm.push_back(std::move(k)); triFace.push_back(std::move(tf));
  }
  if (auto h = raycast(ray(m), pm); h && h->tri >= 0) {
    for (size_t i = 0; i < list.size(); i++) if (list[i]->id == h->itemId) {
      auto T = meshc::topoOf(*list[i]->poly); int f = triFace[i][size_t(h->tri)];
      if (auto r = nearIn(list[i], *T, T->fEdges[size_t(f)], 1e9)) return r;
    }
  }
  if (hover) return std::nullopt;
  std::optional<EdgePick> best;
  for (const Item* it : list) { auto T = meshc::topoOf(*it->poly); std::vector<int> all(size_t(T->ne)); for (int e = 0; e < T->ne; e++) all[size_t(e)] = e; if (auto r = nearIn(it, *T, all, 8)) best = r; }
  return best;
}

void Viewport::setEdgeSel(int mesh, const std::vector<int>& list, const QString& mode) {
  if (edges.mesh != mesh) edges = {mesh, {}, -1};
  if (mode == "set") edges.list.clear();
  for (int e : list) {
    auto it = std::find(edges.list.begin(), edges.list.end(), e);
    if (mode == "remove") { if (it != edges.list.end()) edges.list.erase(it); }
    else if (it == edges.list.end()) edges.list.push_back(e);
  }
  overlayRev_++; requestUpdate();
}
void Viewport::clearEdges() { edges.list.clear(); edges.last = -1; overlayRev_++; requestUpdate(); emit brushChanged(); }

void Viewport::edgeClick(const QPointF& m, bool ctrl, bool shift) {
  auto r = pickEdge(m, false);
  if (!r) { if (!ctrl && !shift && !edges.list.empty()) clearEdges(); return; }
  qint64 now = QDateTime::currentMSecsSinceEpoch();
  bool dbl = edgeClick_.mesh == r->mesh && edgeClick_.e == r->e && now - edgeClick_.t < 400;
  if (dbl) edgeClick_ = {}; else { edgeClick_.mesh = r->mesh; edgeClick_.e = r->e; edgeClick_.t = now; }
  bool same = edges.mesh == r->mesh; auto T = meshc::topoOf(*scene_->item(r->mesh)->poly);
  if (ctrl && shift) {                                       // the shortest path from the last picked edge
    if (same && edges.last >= 0 && edges.last != r->e && !edges.list.empty()) {
      auto p = meshc::edgePath(*T, scene_->item(r->mesh)->poly->pos, edges.last, r->e);
      if (!p.empty()) setEdgeSel(r->mesh, p, "add"); else { ui::toast(tr("No path between these edges (different parts of the mesh)")); setEdgeSel(r->mesh, {r->e}, "add"); }
    } else setEdgeSel(r->mesh, {r->e}, same ? "add" : "set");
    edges.last = r->e;
  } else if (ctrl) { if (same) setEdgeSel(r->mesh, dbl ? meshc::edgeLoop(*T, r->e).edges : std::vector<int>{r->e}, "remove"); }
  else if (dbl) {
    auto L = meshc::edgeLoop(*T, r->e); setEdgeSel(r->mesh, L.edges, "add"); edges.last = r->e;
    ui::toast(tr("Edge loop: %1 edge%2%3").arg(L.edges.size()).arg(L.edges.size() == 1 ? "" : "s").arg(L.closed ? tr(" (closed)") : QString()));
  } else { setEdgeSel(r->mesh, {r->e}, shift && same ? "add" : "set"); edges.last = r->e; }
  if (sel_.items.size() != 1 || sel_.items[0] != r->mesh) { sel_.anchors.clear(); sel_.items = {r->mesh}; onSelChange(); } else emit brushChanged();
}

// Ctrl + drag: every selected edge whose middle is inside the box is deselected
void Viewport::edgeBoxDeselect(double x0, double y0, double x1, double y1) {
  const Item* it = scene_->item(edges.mesh); if (!it || !it->poly || edges.list.empty()) return;
  auto T = meshc::topoOf(*it->poly); Projector P = projector(); std::vector<int> out;
  for (int e : edges.list) { ScreenPt s = P.toScreen((it->poly->pos[size_t(T->ea[size_t(e)])] + it->poly->pos[size_t(T->eb[size_t(e)])]) * 0.5);
    if (s.z < 1 && s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1) out.push_back(e); }
  if (out.empty()) return;
  setEdgeSel(edges.mesh, out, "remove"); emit brushChanged();
  ui::toast(tr("%1 edge%2 deselected").arg(out.size()).arg(out.size() == 1 ? "" : "s"));
}

std::vector<Viewport::EdgeSeg> Viewport::edgeLines() const {
  std::vector<EdgeSeg> out; if (tool_ != "edge") return out;
  if (const Item* it = scene_->item(edges.mesh); it && it->poly && scene_->isVisible(*it)) {
    auto T = meshc::topoOf(*it->poly);
    for (int e : edges.list) if (e < T->ne) out.push_back({it->poly->pos[size_t(T->ea[size_t(e)])], it->poly->pos[size_t(T->eb[size_t(e)])], false});
  }
  if (hoverEdge_) if (const Item* it = scene_->item(hoverEdge_->mesh); it && it->poly) { auto T = meshc::topoOf(*it->poly); if (hoverEdge_->e < T->ne) out.push_back({it->poly->pos[size_t(T->ea[size_t(hoverEdge_->e)])], it->poly->pos[size_t(T->eb[size_t(hoverEdge_->e)])], true}); }
  return out;
}

int Viewport::convertibleMesh() const {
  for (auto r = sel_.items.rbegin(); r != sel_.items.rend(); ++r) if (const Item* x = scene_->item(*r); x && (x->type == ItemType::IMesh || x->type == ItemType::Mesh) && x->poly) return x->id;
  const Item* e = scene_->item(edges.mesh); return e && e->poly ? e->id : -1;
}
void Viewport::edgesToCurves() {
  int mesh = edges.mesh; auto list = edges.list;
  runEdit([mesh, list](Scene& s, Selection& sl, const CurveCache&) { return meshc::edgesToCurves(s, sl, mesh, list); }, true);
}
void Viewport::meshToCurves(int mesh) {
  if (mesh < 0) { ui::toast(tr("Select an imported or a converted mesh")); return; }
  auto opt = m2c;
  runEdit([mesh, opt](Scene& s, Selection& sl, const CurveCache&) { return meshc::meshToCurves(s, sl, mesh, opt); }, true);
}

}  // namespace hm
