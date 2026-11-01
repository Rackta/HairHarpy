// The outliner's pie (Shift+RMB over the outliner): an empty group at the selection's centre (its own point — the gizmo
// stands there while the group alone is picked, and moves it); the picked meshes merged into one — in world space, each
// one's polygroups kept (named after it when it had none), the UVs and materials when every one of them has them; the
// new mesh's pivot where the user chose
#include "scene/Edit.h"
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <set>

namespace hm {

namespace {
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
const PolyMesh* baseOf(const Item& m) { return baseSp(m).get(); }
Item* xformOfMesh(Scene& sc, int m) { for (int x : meshmods::modsOf(sc, m)) if (sc.item(x)->raw.value("kind").toString() == "xform") return sc.item(x); return nullptr; }
}  // namespace

std::optional<Vec3> Viewport::selectionCentre() const {
  if (room() == "topo" && topoMode != "object") {             // (a component mode: the selected elements' middle)
    const int m = topoMesh(); if (m < 0) return std::nullopt; const auto vs = tSelVerts(m); if (vs.empty()) return std::nullopt;
    const auto& W = scene_->item(m)->poly->pos; Vec3 c; for (int v : vs) c += W[size_t(v)]; return c * (1.0 / double(vs.size()));
  }
  if (auto tg = gizmoTargets()) return tg->pivot;              // (objects: where the gizmo stands)
  return std::nullopt;
}

std::vector<int> Viewport::mergeableMeshes() const {
  std::vector<int> o;
  for (int id : sel_.items) if (const Item* it = scene_->item(id); it && it->type == ItemType::IMesh && baseOf(*it) && baseOf(*it)->faceCount() > 0 && !it->raw.value("tghost").toBool()) o.push_back(id);
  return o;
}

QString Viewport::mergeMeshes(const std::vector<int>& ids, const Vec3& pivot, const std::optional<Quat>& axes) {
  std::vector<const Item*> L; for (int id : ids) if (const Item* it = scene_->item(id); it && baseOf(*it)) L.push_back(it);
  if (L.size() < 2) { ui::toast(tr("Pick two meshes or more to merge")); return "!"; }
  bool uv = true, mat = true; for (const Item* it : L) { const PolyMesh& B = *baseOf(*it); uv = uv && !B.cornerUV.empty(); mat = mat && int(B.faceMat.size()) == B.faceCount(); }
  PolyMesh d;
  for (const Item* it : L) {
    const PolyMesh& B = *baseOf(*it); const Affine M = meshmods::meshMatrix(*scene_, *it); const int v0 = int(d.pos.size()), g0 = int(d.groupNames.size()), m0 = int(d.matNames.size());
    for (const Vec3& p : B.pos) d.pos.push_back(M.apply(p));
    if (B.groupNames.empty()) d.groupNames.push_back(it->name); else d.groupNames.insert(d.groupNames.end(), B.groupNames.begin(), B.groupNames.end());
    if (mat) { d.matNames.insert(d.matNames.end(), B.matNames.begin(), B.matNames.end()); for (size_t k = 0; k < B.matNames.size(); k++) { d.matColor.push_back(k < B.matColor.size() ? B.matColor[k] : QString()); d.matAlpha.push_back(k < B.matAlpha.size() ? B.matAlpha[k] : 0); d.matMaps.push_back(k < B.matMaps.size() ? B.matMaps[k] : std::array<PolyMesh::Map, 2>{}); } }
    for (int f = 0; f < B.faceCount(); f++) {
      std::vector<int> vs; for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) { vs.push_back(v0 + B.faceVerts[size_t(k)]); if (uv) d.cornerUV.push_back(B.cornerUV[size_t(k)]); }
      d.addFace(vs.data(), int(vs.size()));
      d.faceGroup.push_back(g0 + (B.groupNames.empty() || size_t(f) >= B.faceGroup.size() ? 0 : B.faceGroup[size_t(f)]));
      if (mat) d.faceMat.push_back(B.faceMat[size_t(f)] < 0 ? -1 : m0 + B.faceMat[size_t(f)]); }
  }
  const Item* first = L[0]; const QString name = first->name; const int parent = first->parent; const QJsonObject keep = first->raw;
  // (its place in the list: where the first one stood — counted among the ones that stay — not at the end)
  int place = 0; { const std::vector<int>& P0 = scene_->item(parent) ? scene_->item(parent)->children : scene_->roots;
    for (int c : P0) { if (c == first->id) break; if (std::find(ids.begin(), ids.end(), c) == ids.end()) place++; } }
  sel_ = {}; sel_.items = ids; edit::deleteItems(*scene_, hair_.curves, sel_);   // (the meshes merged gone, their modifiers with them)
  const int id = topoNewMesh(scene_->item(parent) ? parent : -1, name, d); if (id < 0) return "!";
  if (const Item* nw = scene_->item(id); nw && nw->parent == parent) { std::vector<int>& P = scene_->item(parent) ? scene_->item(parent)->children : scene_->roots;
    P.erase(std::remove(P.begin(), P.end(), id), P.end()); P.insert(P.begin() + std::min<size_t>(size_t(place), P.size()), id); scene_->reindex(); }
  Item* ni = scene_->item(id);
  for (const char* k : {"look", "smoothN", "twire", "mat", "matPreset", "matOff", "unlit", "pgView"}) if (keep.contains(k)) ni->raw.insert(k, keep.value(k));   // (it looks as the first one did)
  ni->raw.insert("pivot", QJsonArray{pivot.x, pivot.y, pivot.z});
  if (axes && std::abs(axes->normalized().w) < 1 - 1e-12) {   // (the gizmo's axes of the mesh its pivot came from: kept in a Transform of nothing)
    if (!xformOfMesh(*scene_, id)) meshmods::setMeshMatrix(*scene_, id, Affine{});
    if (Item* x = xformOfMesh(*scene_, id)) { const Quat q = axes->normalized(); QJsonObject p = x->raw.value("p").toObject(); p.insert("gq", QJsonArray{q.x, q.y, q.z, q.w}); x->raw.insert("p", p); }
  }
  sel_ = {}; sel_.items = {id}; onSelChange(); scene_->revision++; emit sceneEdited(); commit(true); emit topoState();
  const QString msg = tr("%1 meshes merged into %2: %3 polygons").arg(L.size()).arg(name).arg(d.faceCount()); ui::toast(msg); return msg;
}

}  // namespace hm
