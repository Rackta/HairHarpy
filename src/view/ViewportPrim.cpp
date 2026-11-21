// Primitives (the "+" menu): a mesh made from its settings (scene/Primitives) — made again as they change, till Convert
// to object leaves it a plain polygonal mesh.
#include "scene/Primitives.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>

namespace hm {

namespace {
std::optional<Vec3> v3(const QJsonValue& v) { const QJsonArray a = v.toArray(); if (a.size() != 3) return std::nullopt; return Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()); }
}  // namespace

int Viewport::createPrimitive(const QString& type) {
  if (prim::params(type).empty()) return -1;
  // (its size: a 12th of the scene's; its place: the point the camera turns round — the last one clicked)
  const double u = std::max(1e-6, cam_.sceneScale / 12); const Vec3 c = cam_.pivot;
  const QJsonObject p = prim::defaults(type);
  const int id = topoNewMesh(-1, prim::label(type), prim::build(type, p, c, u));
  if (id < 0) return -1;
  scene_->item(id)->raw.insert("prim", QJsonObject{{"type", type}, {"p", p}, {"c", QJsonArray{c.x, c.y, c.z}}, {"u", u}});
  sel_ = {}; sel_.items = {id}; onSelChange();
  stepLabel_ = tr("Primitive: %1").arg(prim::label(type)); afterEdit(true); emit sceneEdited();
  ui::toast(tr("%1 made — its settings in the panel; Convert to object makes it a plain mesh").arg(prim::label(type)));
  return id;
}

void Viewport::primSet(int id, const QString& key, double v, bool commitNow) {
  Item* it = scene_->item(id); if (!it || !it->raw.value("prim").isObject()) return;
  QJsonObject pr = it->raw.value("prim").toObject(), p = pr.value("p").toObject(); const QString type = pr.value("type").toString();
  if (!key.isEmpty()) { bool integer = false; for (auto& d : prim::params(type)) if (key == d.key) integer = d.integer; if (integer) p.insert(key, int(std::lround(v))); else p.insert(key, v); }
  pr.insert("p", p); it->raw.insert("prim", pr);
  const Vec3 c = v3(pr.value("c")).value_or(Vec3()); const double u = pr.value("u").toDouble(1);
  topoWrite(id, std::make_shared<PolyMesh>(prim::build(type, p, c, u)));
  if (Item* i2 = scene_->item(id)) i2->raw.insert("prim", pr);   // (kept — the write may have left the item's raw as it was)
  if (commitNow) { stepLabel_ = tr("%1: settings").arg(prim::label(type)); afterEdit(true); emit sceneEdited(); } else { scene_->revision++; requestUpdate(); }
}

// ---------- Head Collision ----------
namespace {
void dropXform(Scene& sc, int m) {                              // (a copy is placed as its mesh is: a Transform of its own would move it twice)
  for (int x : meshmods::modsOf(sc, m)) if (sc.item(x)->raw.value("kind").toString() == "xform") { sc.removeFromParent(x); sc.items.erase(x); }
}
}  // namespace

int Viewport::collCreateFrom(int mesh) { const auto v = collCreateFrom(std::vector<int>{mesh}); return v.empty() ? -1 : v.front(); }

std::vector<int> Viewport::collCreateFrom(const std::vector<int>& meshes) {
  std::vector<int> made; QStringList names; bool hidden = false;
  if (room() != "hair") return made;
  for (int mesh : meshes) {
    const Item* m = scene_->item(mesh); if (!m || !m->poly || coll::inGroup(*scene_, mesh)) continue;
    const QString nm = m->name; const bool hide = scene_->isVisible(*m);   // (it lies right on the mesh: both shown, they flicker through each other)
    const int id = coll::addObject(*scene_, nm, coll::meshCopy(*scene_, *m), QJsonObject{{"collSrc", mesh}}); if (id < 0) continue;
    if (hide) { scene_->item(id)->visible = false; hidden = true; }
    made.push_back(id); names << nm;
  }
  if (made.empty()) return made;
  sel_ = {}; sel_.items = made; onSelChange();
  const QString what = names.size() == 1 ? names[0] : tr("%1 meshes").arg(names.size());
  stepLabel_ = tr("Collision object: %1").arg(what); afterEdit(true); emit sceneEdited();
  ui::toast(hidden ? tr("%1 — a copy in Head Collision, hidden (it lies on the mesh; it collides hidden too — 👁 shows it)").arg(what) : tr("%1 — a copy in Head Collision").arg(what));
  return made;
}

int Viewport::collAdd(const QString& shape) {
  if (room() != "hair") return -1;
  int id = -1;
  if (shape.startsWith("mesh:")) return collCreateFrom(shape.mid(5).toInt());
  // (a primitive or a preset: at the point the camera turns round, a sixth of the scene's size)
  const double u = std::max(1e-6, cam_.sceneScale / 6);
  const bool pre = shape.startsWith("preset:");
  id = pre ? coll::addPreset(*scene_, shape.mid(7), cam_.pivot, u) : coll::addPrimitive(*scene_, shape, cam_.pivot, u);
  if (id < 0) return -1;
  sel_ = {}; sel_.items = {id}; onSelChange();
  stepLabel_ = tr("Collision object: %1").arg(pre ? coll::presetLabel(shape.mid(7)) : prim::label(shape)); afterEdit(true); emit sceneEdited();
  return id;
}

void Viewport::collPresetSet(int id, double size, bool commitNow) {
  Item* it = scene_->item(id); if (!it || !it->raw.value("preset").isObject()) return;
  QJsonObject pr = it->raw.value("preset").toObject(); pr.insert("s", size);
  const QJsonArray a = pr.value("c").toArray(); const Vec3 c = a.size() == 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3();
  topoWrite(id, std::make_shared<PolyMesh>(coll::presetBuild(pr.value("name").toString(), c, pr.value("u").toDouble(1), size)));
  if (Item* i2 = scene_->item(id)) i2->raw.insert("preset", pr);
  if (commitNow) { stepLabel_ = tr("%1: size").arg(coll::presetLabel(pr.value("name").toString())); afterEdit(true); emit sceneEdited(); } else { scene_->revision++; requestUpdate(); }
}

void Viewport::collSetShape(int id, const QString& shape) {
  Item* it = scene_->item(id); if (!it || !it->headTag) return;
  const PolyMesh* B = it->base ? it->base.get() : it->poly.get(); if (!B) return;
  if (shape.startsWith("mesh:")) {                              // (a copy of that mesh, where it is)
    const int src = shape.mid(5).toInt(); const Item* s = scene_->item(src); if (!s || !s->poly || src == id) return;
    PolyMesh d = coll::meshCopy(*scene_, *s); const QString nm = s->name;
    dropXform(*scene_, id); it = scene_->item(id);
    it->raw.remove("prim"); it->raw.remove("preset"); it->raw.insert("collSrc", src); it->name = nm;
    if (scene_->isVisible(*s)) it->visible = false;               // (on its mesh: hidden — both shown, they flicker; hidden, it still collides)
    topoWrite(id, std::make_shared<PolyMesh>(std::move(d)));
    stepLabel_ = tr("Collision: %1").arg(nm);
  } else {                                                      // (a primitive or a preset: where the shape was, as big)
    const bool pre = shape.startsWith("preset:"); const QString pn = pre ? shape.mid(7) : QString();
    if (pre ? std::find(coll::presets().begin(), coll::presets().end(), pn) == coll::presets().end() : prim::params(shape).empty()) return;
    QJsonObject old = it->raw.value("prim").toObject(); if (old.isEmpty()) old = it->raw.value("preset").toObject();
    Vec3 c; double u = 1;
    if (const QJsonArray a = old.value("c").toArray(); a.size() == 3) { c = Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()); u = old.value("u").toDouble(1); }
    else { const Box3 b = B->bounds(); const Vec3 sz = b.size(); c = b.center(); u = std::max({sz.x, sz.y, sz.z, 1e-6}); }
    it->raw.remove("collSrc"); it->raw.remove("prim"); it->raw.remove("preset");
    QString key; QJsonValue keep; std::shared_ptr<PolyMesh> mesh;
    if (pre) { key = "preset"; keep = QJsonObject{{"name", pn}, {"c", QJsonArray{c.x, c.y, c.z}}, {"u", u}, {"s", 1.0}}; mesh = std::make_shared<PolyMesh>(coll::presetBuild(pn, c, u, 1.0)); it->name = coll::presetLabel(pn); }
    else { const QJsonObject p = prim::defaults(shape); key = "prim"; keep = QJsonObject{{"type", shape}, {"p", p}, {"c", QJsonArray{c.x, c.y, c.z}}, {"u", u}}; mesh = std::make_shared<PolyMesh>(prim::build(shape, p, c, u)); it->name = prim::label(shape); }
    it->raw.insert(key, keep);
    const QString nm = it->name;
    topoWrite(id, mesh);
    if (Item* i2 = scene_->item(id)) i2->raw.insert(key, keep);
    stepLabel_ = tr("Collision: %1").arg(nm);
  }
  scene_->reindex(); afterEdit(true); emit sceneEdited();
}

void Viewport::primConvert(int id) {
  Item* it = scene_->item(id); if (!it || !it->raw.contains("prim")) return;
  const QString type = it->raw.value("prim").toObject().value("type").toString();
  it->raw.remove("prim");
  stepLabel_ = tr("%1 → a mesh").arg(prim::label(type)); afterEdit(true); emit sceneEdited();
  ui::toast(tr("%1 is a plain polygonal mesh now").arg(it->name));
}

}  // namespace hm
