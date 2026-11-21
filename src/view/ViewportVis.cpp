// One way of hiding and isolating in every room (19.11, the author: "приведи работу с изоляцией к единому стандарту"), the Paint
// room's (14.11): on the selection — and with nothing selected, on what is under the pointer:
//   H — hide it · Shift+H — show only it · Ctrl+H — hide the whole object · Ctrl+Shift+H — show only the whole object ·
//   Ctrl+X — show everything again. A second press does not bring things back (Ctrl+X or Ctrl+Z does).
// The Paint room keeps its own pvKey (the same rules over its polygons); here the Topology room ("object" — the mesh; the part — the
// polygons picked, the polygroup, the part under the pointer). The Hair room keeps its own isolation (19.11: more is isolated there
// than hidden — H / Ctrl+H / Shift+H with the meshes tagged HEAD / Ctrl+Shift+H, MainWindow); here only its Ctrl+X.
#include "scene/Edit.h"
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <functional>
#include <set>

namespace hm {

namespace {
// an isolation keeping these items and what they hang on (a surface's curves, a tube's source, a modifier's tube or mesh…)
Scene::Isolation keepOf(const Scene& sc, const std::set<int>& ids) {
  Scene::Isolation iso;
  std::function<void(int)> add = [&](int id) {
    const Item* it = sc.item(id); if (!it || iso.keep.count(id)) return; iso.keep.insert(id);
    if (it->type == ItemType::Group) for (int c : it->children) add(c);
    if (it->type == ItemType::Surface) { add(it->a); add(it->b); }
    if (it->type == ItemType::Guide) for (const QJsonValue& r : it->raw.value("rings").toArray()) add(r.toInt());
    if (it->type == ItemType::Tube) add(it->raw.value("src").toInt(-1));
    if (it->type == ItemType::XMod) add(it->raw.contains("tube") && !it->raw.value("tube").isNull() ? it->raw.value("tube").toInt() : it->raw.value("mesh").toInt(-1));
    if (it->type == ItemType::Crown) { add(it->raw.value("part").toInt(-1)); add(it->raw.value("low").toInt(-1)); }
    if (it->type == ItemType::IMesh) for (int x : meshmods::modsOf(sc, id)) add(x);   // (a mesh's modifiers stay with it)
  };
  for (int id : ids) add(id);
  return iso;
}
}  // namespace

QString Viewport::visKey(const QString& op) {
  const QString r = room();
  auto shown = [&] { staticRev_++; topoRev_++; allWalls_ = true; pickCache_.clear(); sceneChanged(false); emit sceneEdited(); emit topoState(); requestUpdate(); };
  auto say = [&](const QString& s) { ui::toast(s); return s; };
  const QString back = tr(" — Ctrl+X shows everything");
  const bool only = op == "only" || op == "onlyObj", whole = op == "hideObj" || op == "onlyObj";
  auto hideItems = [&](const std::set<int>& ids, std::set<int>& keep) { for (int id : ids) if (Item* it = scene_->item(id); it && it->visible) { it->visible = false; keep.insert(id); } };   // (only what was shown: Ctrl+X must not bring back what the eyes hid before)
  auto isolate = [&](const std::set<int>& ids) { scene_->iso = keepOf(*scene_, ids); };

  if (r == "topo") {
    if (op == "showAll") { tKeyUnder("showAll"); return QString(); }
    const int m = topoMesh();
    // (1) polygons / edges / vertices picked
    if (tHasCompSel()) {
      std::set<int> meshes; for (int x : tselMeshes()) meshes.insert(x);
      if (op == "hide") return tHideSel();
      if (op == "only") { if (tcIso_) tIsolateSel(); tIsolateSel(); shown(); return QString(); }   // (a second isolation: the first one let go, then this one — not a toggle)
      if (op == "hideObj") { hideItems(meshes, tHidByKey_); shown(); return say(tr("%1 mesh%2 hidden").arg(meshes.size()).arg(meshes.size() == 1 ? "" : "es") + back); }
      isolate(meshes); shown(); return say(tr("Only the selected mesh%1 shown").arg(meshes.size() == 1 ? "" : "es") + back);
    }
    // (2) polygroups picked (the list, Ctrl+Shift)
    if (m >= 0) if (const auto gs = smPickedGroups(m); !gs.empty() && !whole) {
      if (op == "hide") { tgSetVisible(m, std::vector<int>(gs.begin(), gs.end()), false); shown(); return say(tr("%1 polygroup%2 hidden").arg(gs.size()).arg(gs.size() == 1 ? "" : "s") + back); }
      if (tgIso_) tgIsolateToggle(); tgIsolateToggle(); shown(); return QString(); }
    // (3) objects selected
    std::set<int> objs; for (int id : sel_.items) if (const Item* it = scene_->item(id); it && it->type == ItemType::IMesh) objs.insert(id);
    if (!objs.empty()) {
      if (!only) { hideItems(objs, tHidByKey_); shown(); return say(tr("%1 mesh%2 hidden").arg(objs.size()).arg(objs.size() == 1 ? "" : "es") + back); }
      isolate(objs); shown(); return say(tr("Only the selected mesh%1 shown").arg(objs.size() == 1 ? "" : "es") + back); }
    // (4) nothing selected: what is under the pointer
    if (tKeyUnder(op)) return QString();
    return say(tr("Select something, or point at the model: H and its kin act on what is under the pointer"));
  }

  if (r == "hair") {
    if (op == "showAll") {
      int n = 0; for (int id : hairHid_) if (Item* it = scene_->item(id); it && !it->visible) { it->visible = true; n++; } hairHid_.clear();
      if (scene_->iso && !scene_->iso->solo) { scene_->iso.reset(); n++; }
      shown(); return say(n ? tr("Everything shown") : tr("Nothing was hidden"));
    }
    return QString();                                           // (H and its kin keep their own way in the Hair room — MainWindow)
  }
  return QString();
}

}  // namespace hm
