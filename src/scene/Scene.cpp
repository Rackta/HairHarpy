#include "scene/Scene.h"
#include <algorithm>

namespace hm {

namespace {
struct TypeInfo { const char* key; ItemType t; const char* name; const char* icon; };
// names and icons as TYPE_NAMES / ICONS in the HTML version
const TypeInfo TYPES[] = {
  {"group", ItemType::Group, "Group", "▣"},     {"curve", ItemType::Curve, "Curve", "∿"},
  {"surface", ItemType::Surface, "Surface", "▦"}, {"guide", ItemType::Guide, "Guide", "┼"},
  {"mesh", ItemType::Mesh, "Mesh", "◆"},        {"imesh", ItemType::IMesh, "Imported mesh", "⬢"},
  {"tube", ItemType::Tube, "Modifier", "⬡"},    {"xmod", ItemType::XMod, "Extra", "✦"},
  {"crown", ItemType::Crown, "Crown", "◠"},     {"cap", ItemType::Cap, "Cap", "◒"},
  {"skin", ItemType::Skin, "Fill holes", "◭"},  {"tip", ItemType::Tip, "Density", "⋎"},
  {"shell", ItemType::Shell, "Proxy shell", "◎"}, {"even", ItemType::Even, "Even loops", "≡"},
  {"pinf", ItemType::PInf, "Parent influence", "⇅"}, {"cocoon", ItemType::Cocoon, "Cocoon", "◍"},
  {"head", ItemType::Head, "Head", "☺"},         {"braid", ItemType::Braid, "Braid", "⌇"},
  {"spiral", ItemType::Spiral, "Spiral", "➰"},
};
}  // namespace

ItemType itemTypeFromString(const QString& s) {
  for (auto& t : TYPES) if (s == QLatin1String(t.key)) return t.t;
  return ItemType::Unknown;
}
QString itemTypeName(ItemType t) {
  for (auto& x : TYPES) if (x.t == t) return QString::fromUtf8(x.name);
  return QStringLiteral("Item");
}
QString itemTypeKey(ItemType t) {
  for (auto& x : TYPES) if (x.t == t) return QString::fromLatin1(x.key);
  return QStringLiteral("item");
}
QString itemTypeIcon(ItemType t) {
  for (auto& x : TYPES) if (x.t == t) return QString::fromUtf8(x.icon);
  return QStringLiteral("?");
}

void Scene::clear() {
  items.clear(); anchors.clear(); roots.clear(); fileRaw = {}; camera = {}; curvesAt.clear(); path.clear(); revision++;
  refPlanes.clear(); refBoard = {}; nextRefId = 1; refSketches.clear(); nextCanvasId = 1; refMarkers.clear(); nextMarkerId = 1; refMarkersShown = true; refMarkRev++;
}

const std::vector<int>& Scene::childrenOf(int parentId) const {
  if (parentId < 0) return roots;
  static const std::vector<int> none;
  const Item* p = item(parentId);
  return p ? p->children : none;
}

// isolation: everything but the kept items (and optionally the head) hidden; a group's isolation keeps all inside it
bool Scene::isoKept(const Item& it) const {
  if (!iso || it.type == ItemType::Group) return true;
  if (it.type == ItemType::Head) return iso->head;
  // (the Hair room: a mesh or a collision object tagged Head in the outliner stays with an isolation + head — Shift+H; 19.11, there is
  //  no head item any more, the tag stands for it. A collision object counts as tagged until the tag is switched off)
  if (iso->head && it.type == ItemType::IMesh && room == "hair" && it.raw.value(QLatin1String("headMark")).toBool(it.headTag)) return true;
  return iso->keep.count(it.id) || (iso->group >= 0 && isDescendant(it.id, iso->group));
}

// the Paint room: an object marked NoPainted (itself or its folder) is not there at all; one hidden there (pHid — the room's own
// eye, the other rooms don't see it) is not shown
bool Scene::isVisible(const Item& it) const {
  if (!isVisibleBase(it)) return false;
  if (room == QLatin1String("paint")) for (const Item* p = &it; p; p = p->parent >= 0 ? item(p->parent) : nullptr) if (p->raw.value(QLatin1String("pHid")).toBool()) return false;
  return true;
}
bool Scene::noPaint(const Item& it) const {
  for (const Item* p = &it; p; p = p->parent >= 0 ? item(p->parent) : nullptr) if (p->raw.value(QLatin1String("noPaint")).toBool()) return true;
  return false;
}
bool Scene::isVisibleBase(const Item& it) const {
  if (!isoKept(it)) return false;
  if (room == QLatin1String("paint") && noPaint(it)) return false;
  for (const Item* p = &it; p; p = p->parent >= 0 ? item(p->parent) : nullptr) if (!p->visible) return false;
  return true;
}

bool Scene::isDescendant(int id, int ancestor) const {
  for (const Item* p = item(id); p; p = p->parent >= 0 ? item(p->parent) : nullptr) if (p->id == ancestor) return true;
  return false;
}

QString Scene::tintOf(const Item& it) const {
  QString c;
  for (const Item* p = it.parent >= 0 ? item(it.parent) : nullptr; p; p = p->parent >= 0 ? item(p->parent) : nullptr) {
    bool service = p->raw.value("guidesGroup").toBool() || p->raw.value("meshesGroup").toBool() || p->raw.value("importGroup").toBool() || p->raw.value("collGroup").toBool();
    if (!p->color.isEmpty() && !service) c = p->color;
  }
  return c;
}

std::vector<const Item*> Scene::curves() const {
  std::vector<const Item*> out;
  for (auto& [id, it] : items) if (it.type == ItemType::Curve) out.push_back(&it);
  std::sort(out.begin(), out.end(), [](const Item* a, const Item* b) { return a->order < b->order; });
  return out;
}

Box3 Scene::bounds() const {
  Box3 b;
  for (auto& [id, it] : items) if ((it.type == ItemType::IMesh || it.type == ItemType::Head) && it.poly && it.headTag) b.unite(it.poly->bounds());
  if (!b.empty()) return b;
  for (auto& [id, it] : items) if ((it.type == ItemType::IMesh || it.type == ItemType::Head || it.type == ItemType::Mesh) && it.poly) b.unite(it.poly->bounds());
  for (auto& [id, it] : items) if (it.type == ItemType::Curve) for (int a : it.anchors) if (auto* an = anchor(a)) b.expand(an->pos);
  return b;
}

Item& Scene::addItem(ItemType type, int parent, const QString& name) {
  Item it; it.id = nextId++; it.order = nextOrder++; it.type = type; it.name = name; it.parent = parent;
  for (auto& t : TYPES) if (t.t == type) it.typeStr = QString::fromLatin1(t.key);
  int id = it.id;
  auto& placed = items.emplace(id, std::move(it)).first->second;
  if (parent >= 0) { if (Item* p = item(parent)) p->children.push_back(id); } else roots.push_back(id);
  if (iso) iso->keep.insert(id);                              // (made while isolated: stays visible)
  return placed;
}

Anchor& Scene::addAnchor(const Vec3& p) {
  Anchor a; a.id = nextId++; a.pos = p; a.smooth = 1;
  return anchors.emplace(a.id, a).first->second;
}

void Scene::removeFromParent(int id) {
  const Item* it = item(id); if (!it) return;
  std::vector<int>& arr = it->parent >= 0 && item(it->parent) ? item(it->parent)->children : roots;
  arr.erase(std::remove(arr.begin(), arr.end(), id), arr.end());
}

void Scene::reindex() {
  // (the Head Collision group's objects: headTag — what the collision, the crown and the scale go by; none in the other rooms)
  int cg = -1; for (int r : roots) if (const Item* g = item(r); g && g->type == ItemType::Group && g->raw.value(QLatin1String("collGroup")).toBool()) { cg = r; break; }
  for (auto& [id, it] : items) it.headTag = cg >= 0 && it.poly && (it.type == ItemType::IMesh || it.type == ItemType::Mesh) && isDescendant(id, cg);
  curvesAt.clear();
  for (const Item* c : curves()) for (int a : c->anchors) {
    auto& v = curvesAt[a];
    if (v.empty() || v.back() != c->id) v.push_back(c->id);   // (in file order, once per curve)
  }
}

void plainMesh(Item& it) {
  if (it.type != ItemType::Mesh || it.raw.value("pending").toBool()) return;
  it.type = ItemType::IMesh; it.typeStr = QStringLiteral("imesh");
  if (it.poly) {
    if (it.poly->groupNames.empty() || it.poly->faceGroup.size() != size_t(it.poly->faceCount())) { auto p = std::make_shared<PolyMesh>(*it.poly); p->groupNames = {it.name}; p->faceGroup.assign(size_t(p->faceCount()), 0); it.poly = p; }
    it.base = it.poly;
  }
  it.raw.remove("data"); it.raw.remove("pending");
  it.raw.insert("src", QJsonObject{{"ext", "pmesh"}, {"live", true}});
}

}  // namespace hm
