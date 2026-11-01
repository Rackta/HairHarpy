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
  refPlanes.clear(); refBoard = {}; nextRefId = 1;
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
  if (it.type == ItemType::Head || (it.type == ItemType::IMesh && it.headTag && room == "hair")) return iso->head;
  return iso->keep.count(it.id) || (iso->group >= 0 && isDescendant(it.id, iso->group));
}

bool Scene::isVisible(const Item& it) const {
  if (!isoKept(it)) return false;
  // (a mesh of the head: its own eye only — its folder isn't listed in the Hair room; the Head row shows / hides it)
  if (it.type == ItemType::IMesh && it.headTag && room == "hair") return it.visible;
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
    bool service = p->raw.value("guidesGroup").toBool() || p->raw.value("meshesGroup").toBool() || p->raw.value("importGroup").toBool();
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
  curvesAt.clear();
  for (const Item* c : curves()) for (int a : c->anchors) {
    auto& v = curvesAt[a];
    if (v.empty() || v.back() != c->id) v.push_back(c->id);   // (in file order, once per curve)
  }
}

}  // namespace hm
