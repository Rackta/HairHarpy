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

void Viewport::primConvert(int id) {
  Item* it = scene_->item(id); if (!it || !it->raw.contains("prim")) return;
  const QString type = it->raw.value("prim").toObject().value("type").toString();
  it->raw.remove("prim");
  stepLabel_ = tr("%1 → a mesh").arg(prim::label(type)); afterEdit(true); emit sceneEdited();
  ui::toast(tr("%1 is a plain polygonal mesh now").arg(it->name));
}

}  // namespace hm
