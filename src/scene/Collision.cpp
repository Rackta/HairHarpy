#include "scene/Collision.h"
#include "io/MeshImport.h"
#include "scene/MeshMods.h"
#include "scene/Primitives.h"
#include <QFile>
#include <QJsonArray>
#include <map>
#include <algorithm>
#include <memory>
#include <set>

namespace hm::coll {

bool isGroup(const Item& it) { return it.type == ItemType::Group && it.raw.value(QLatin1String("collGroup")).toBool(); }

int group(const Scene& sc) {
  for (int r : sc.roots) if (const Item* it = sc.item(r); it && isGroup(*it)) return r;
  for (auto& [id, it] : sc.items) if (isGroup(it)) return id;
  return -1;
}

bool active(const Scene& sc) { const Item* g = sc.item(group(sc)); return g && !g->raw.value(QLatin1String("collOff")).toBool(); }
bool locked(const Scene& sc) { const Item* g = sc.item(group(sc)); return g && g->raw.value(QLatin1String("selLock")).toBool(true); }
bool inGroup(const Scene& sc, int id) { const int g = group(sc); return g >= 0 && sc.isDescendant(id, g); }
int count(const Scene& sc) { int n = 0; for (auto& [id, it] : sc.items) if (it.headTag) n++; return n; }

PolyMesh plain(const PolyMesh& m) {
  PolyMesh o; o.pos = m.pos; o.faceStart = m.faceStart; o.faceVerts = m.faceVerts;
  o.faceGroup.assign(size_t(std::max(0, m.faceCount())), 0); o.groupNames = {QStringLiteral("Collision")};
  return o;
}

PolyMesh meshCopy(const Scene& sc, const Item& m) {
  const PolyMesh* B = m.base ? m.base.get() : m.poly.get(); if (!B) return {};
  PolyMesh o = plain(*B); const Affine M = meshmods::meshMatrix(sc, m);
  for (Vec3& p : o.pos) p = M.apply(p);
  return o;
}

int ensureGroup(Scene& sc) {
  int g = group(sc);
  if (g < 0) {
    Item& n = sc.addItem(ItemType::Group, -1, QStringLiteral("Head Collision")); n.expanded = true;
    n.raw = QJsonObject{{"collGroup", true}, {"collOff", false}, {"selLock", true}, {"guides", "hide"}, {"side", "scene"}, {"logic", 2}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false}};
    g = n.id;
  }
  Item& G = *sc.item(g);
  if (G.parent >= 0) { sc.removeFromParent(g); G.parent = -1; sc.roots.push_back(g); }
  sc.roots.erase(std::remove(sc.roots.begin(), sc.roots.end(), g), sc.roots.end()); sc.roots.insert(sc.roots.begin(), g);   // (first of all)
  return g;
}

int addObject(Scene& sc, const QString& name, const PolyMesh& m, const QJsonObject& extra) {
  if (m.pos.empty() || m.faceCount() <= 0) return -1;
  const int g = ensureGroup(sc);
  Item& it = sc.addItem(ItemType::IMesh, g, name);
  it.raw = QJsonObject{{"src", QJsonObject{{"ext", "pmesh"}, {"live", true}}}, {"collObj", true}};
  for (auto k = extra.begin(); k != extra.end(); ++k) it.raw.insert(k.key(), k.value());
  auto B = std::make_shared<PolyMesh>(m); it.base = B; it.poly = B;
  const int id = it.id; sc.item(g)->expanded = true;
  sc.reindex(); sc.revision++;
  return id;
}

int addPrimitive(Scene& sc, const QString& type, const Vec3& c, double u) {
  if (prim::params(type).empty()) return -1;
  const QJsonObject p = prim::defaults(type);
  return addObject(sc, prim::label(type), prim::build(type, p, c, u), QJsonObject{{"prim", QJsonObject{{"type", type}, {"p", p}, {"c", QJsonArray{c.x, c.y, c.z}}, {"u", u}}}});
}

const std::vector<QString>& presets() { static const std::vector<QString> P{QStringLiteral("head")}; return P; }
QString presetLabel(const QString& name) { return name == QLatin1String("head") ? QStringLiteral("Head") : name; }

PolyMesh presetBuild(const QString& name, const Vec3& c, double u, double size) {
  // (read once: plain, its box's middle at 0, its biggest side 1)
  static std::map<QString, PolyMesh> unit;
  auto f = unit.find(name);
  if (f == unit.end()) {
    PolyMesh m; QFile in(QStringLiteral(":/coll/%1.fbx").arg(name));
    if (in.open(QIODevice::ReadOnly)) if (auto p = importMesh(QStringLiteral("fbx"), in.readAll())) {
      m = plain(*p); const Box3 b = m.bounds(); const Vec3 sz = b.size(), mid = b.center(); const double big = std::max({sz.x, sz.y, sz.z});
      if (big > 0) for (Vec3& q : m.pos) q = (q - mid) * (1.0 / big); }
    f = unit.emplace(name, std::move(m)).first;
  }
  PolyMesh o = f->second; const double k = u * size; for (Vec3& q : o.pos) q = c + q * k;
  return o;
}

int addPreset(Scene& sc, const QString& name, const Vec3& c, double u) {
  return addObject(sc, presetLabel(name), presetBuild(name, c, u, 1.0), QJsonObject{{"preset", QJsonObject{{"name", name}, {"c", QJsonArray{c.x, c.y, c.z}}, {"u", u}, {"s", 1.0}}}});
}

int prepare(Scene& sc) {
  const bool had = group(sc) >= 0; const int g = ensureGroup(sc); int made = 0;
  // the older marks: ☺ Head, ⛨ Collision on — each mesh's shape (its capsule, the mesh standing for it, else itself) copied in
  std::vector<Item*> old;
  for (auto& [id, it] : sc.items) if ((it.type == ItemType::IMesh || it.type == ItemType::Mesh) && it.poly && !sc.isDescendant(id, g) && !it.raw.contains(QLatin1String("collCap"))
                                       && (it.raw.value(QLatin1String("headTag")).toBool() || it.raw.value(QLatin1String("collide")).toBool())) old.push_back(&it);
  std::sort(old.begin(), old.end(), [](const Item* a, const Item* b) { return a->order < b->order; });
  std::vector<std::pair<int, PolyMesh>> shapes; std::vector<QJsonObject> extras; std::vector<QString> names;
  for (Item* m : old) {
    const QJsonObject co = m->raw.value(QLatin1String("coll")).toObject(); const QString type = co.value(QLatin1String("type")).toString();
    PolyMesh shape;
    if (type == QLatin1String("capsule")) { for (auto& [cid, c] : sc.items) if (c.raw.value(QLatin1String("collCap")).toInt(-1) == m->id && c.poly) { shape = meshCopy(sc, c); break; } }
    else if (type == QLatin1String("custom")) { if (const Item* o = sc.item(co.value(QLatin1String("mesh")).toInt(-1)); o && o->poly) shape = meshCopy(sc, *o); }
    if (shape.pos.empty()) shape = meshCopy(sc, *m);
    QJsonObject ex{{"collSrc", m->id}}; if (co.contains(QLatin1String("d"))) ex.insert("coll", QJsonObject{{"d", co.value(QLatin1String("d"))}});
    shapes.push_back({m->id, std::move(shape)}); extras.push_back(ex); names.push_back(m->name);
    for (const char* k : {"headTag", "collide", "coll"}) m->raw.remove(QLatin1String(k));
    m->headTag = false;
  }
  // (each copy hidden: it lies right on its mesh — both shown, they flicker through each other; hidden, it still collides)
  for (size_t i = 0; i < shapes.size(); i++) if (const int id = addObject(sc, names[i], shapes[i].second, extras[i]); id >= 0) { sc.item(id)->visible = false; made++; }
  // (the capsules the older collision made: gone, with their modifiers)
  std::set<int> gone; for (auto& [id, it] : sc.items) if (it.raw.contains(QLatin1String("collCap"))) gone.insert(id);
  for (auto& [id, it] : sc.items) if (it.type == ItemType::XMod && it.raw.value(QLatin1String("mesh")).isDouble() && gone.count(it.raw.value(QLatin1String("mesh")).toInt())) gone.insert(id);
  for (int id : gone) { sc.removeFromParent(id); sc.items.erase(id); }
  // an empty scene (a new project): a PolySphere to start with
  if (!had && made == 0) {
    bool empty = true;
    for (auto& [id, it] : sc.items) if (!(it.type == ItemType::Group && (isGroup(it) || it.raw.value(QLatin1String("meshesGroup")).toBool() || it.raw.value(QLatin1String("importGroup")).toBool()))) { empty = false; break; }
    if (empty && addPrimitive(sc, QStringLiteral("psphere"), Vec3(0, 0.5, 0), 1.0) >= 0) made++;
  }
  sc.reindex(); sc.revision++;
  return made + (had ? 0 : 1);
}

}  // namespace hm::coll
