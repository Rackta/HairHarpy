#include "scene/Symmetry.h"
#include "scene/Convert.h"
#include "scene/Edit.h"
#include <QJsonArray>
#include <QObject>
#include <functional>
#include <map>
#include <set>

namespace hm::sym {

namespace {

const Item* parentOf(const Scene& sc, const Item& it) { return it.parent >= 0 ? sc.item(it.parent) : nullptr; }
std::vector<int>& childrenRef(Scene& sc, int parent) { Item* p = parent >= 0 ? sc.item(parent) : nullptr; return p ? p->children : sc.roots; }

// (a group / cocoon walked into, as the outliner shows them)
std::vector<int> outlinerOrder(const Scene& sc) {
  std::vector<int> out;
  std::function<void(const std::vector<int>&)> walk = [&](const std::vector<int>& ids) {
    for (int id : ids) { const Item* it = sc.item(id); if (!it) continue; out.push_back(id); if (it->type == ItemType::Group || it->type == ItemType::Cocoon) walk(it->children); }
  };
  walk(sc.roots);
  return out;
}

// moves n right after afterId among its parent's children
void place(Scene& sc, int n, int afterId) {
  if (afterId < 0) return;
  std::vector<int>& arr = childrenRef(sc, sc.item(n)->parent);
  arr.erase(std::remove(arr.begin(), arr.end(), n), arr.end());
  auto at = std::find(arr.begin(), arr.end(), afterId);
  arr.insert(at == arr.end() ? arr.end() : at + 1, n);
}

// a copy of the item's record (everything it carries) under a new id; no children
int cloneItem(Scene& sc, int srcId, int parent, const QString& name) {
  ItemType t = sc.item(srcId)->type;
  edit::nextNumber(sc, t);
  Item& n = sc.addItem(t, parent, name);
  int id = n.id, order = n.order;
  n = *sc.item(srcId);
  n.id = id; n.order = order; n.parent = parent; n.name = name; n.children.clear();
  return id;
}

int mapId(const std::map<int, int>& m, int id) { auto it = m.find(id); return it == m.end() ? id : it->second; }
void remapKey(QJsonObject& o, const char* k, const std::map<int, int>& m) {
  if (o.value(k).isDouble()) o.insert(k, mapId(m, o.value(k).toInt()));
}

std::vector<int> xmodsOf(const Scene& sc, const char* key, int id) {
  std::vector<int> out;
  for (int x : outlinerOrder(sc)) { const Item* it = sc.item(x); if (it->type == ItemType::XMod && it->raw.value(key).isDouble() && it->raw.value(key).toInt() == id) out.push_back(x); }
  return out;
}

QJsonArray flipProf(const QJsonArray& P) {      // (x → −x, the order reversed: the same winding)
  QJsonArray o;
  for (int i = P.size() - 1; i >= 0; i--) { QJsonArray q = P[i].toArray(); if (q.size() >= 2) q[0] = -q[0].toDouble(); o.append(q); }
  return o;
}

}  // namespace

int symAxis(const Scene& sc, const Item& it) {
  for (const Item* p = it.type == ItemType::Group ? &it : parentOf(sc, it); p; p = parentOf(sc, *p)) {
    QJsonObject s = p->raw.value("sym").toObject();
    if (s.value("on").toBool()) { QString a = s.value("axis").toString("x"); return a == "y" ? 1 : a == "z" ? 2 : 0; }
  }
  return -1;
}

Vec3 groupPoint(const Item& g) { const QJsonArray a = g.raw.value("gpivot").toArray(); return a.size() == 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3(); }

Vec3 symCenter(const Scene& sc, const Item& it, const Vec3& head) {
  const Item* g = it.type == ItemType::Group ? &it : parentOf(sc, it);
  for (; g; g = parentOf(sc, *g)) if (g->raw.value("sym").toObject().value("on").toBool()) break;
  if (!g) return head;
  for (const Item* p = parentOf(sc, *g); p; p = parentOf(sc, *p)) if (p->type == ItemType::Group && p->raw.value("gpivot").isArray()) return groupPoint(*p);
  return head;
}

// the mirrored copy baked into real, editable curves and surfaces (anchors lying on the plane stay shared). It goes into
// its own group "<name> mirror" right after the original (sub-groups recreated inside it), with the same colour and
// conversion settings
QString applySymmetry(Scene& sc, int gid, const Vec3& headC, Selection& sel) {
  Item* g0 = sc.item(gid); if (!g0) return "!";
  const Vec3 center = symCenter(sc, *g0, headC);
  QJsonObject sym = g0->raw.value("sym").toObject(); if (!sym.value("on").toBool()) return "!";
  int ax = symAxis(sc, *g0); double eps = sceneScale(sc) * 1e-4;
  auto cloneGroup = [&](int srcId, int parent, int after) {
    edit::nextNumber(sc, ItemType::Group);
    const Item src = *sc.item(srcId);
    Item& n = sc.addItem(ItemType::Group, parent, src.name + " mirror");
    n.color = src.color; n.expanded = src.expanded; n.visible = src.visible;
    n.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", src.raw.value("guides").toString("auto")}, {"conv", src.raw.value("conv")}, {"guidesGroup", false},
                        {"meshesGroup", false}, {"importGroup", false}, {"labels", src.raw.value("labels")}, {"guidesOff", false}, {"logic", 2},
                        {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}};
    int id = n.id; place(sc, id, after);
    return id;
  };
  int mg = cloneGroup(gid, g0->parent, gid);
  std::map<int, int> gmap{{gid, mg}};
  std::vector<int> list;
  std::function<void(std::vector<int>)> walk = [&](std::vector<int> ids) {
    for (int id : ids) {
      const Item* it = sc.item(id); if (!it) continue;
      if (it->type == ItemType::Group) { gmap[id] = cloneGroup(id, gmap.at(it->parent), -1); walk(sc.item(id)->children); } else list.push_back(id);
    }
  };
  walk(sc.item(gid)->children);
  auto par = [&](int id) { auto it = gmap.find(sc.item(id)->parent); return it == gmap.end() ? mg : it->second; };
  std::map<int, int> amap, imap;
  auto cpA = [&](int a) {
    auto f = amap.find(a); if (f != amap.end()) return f->second;
    const Anchor A = *sc.anchor(a);
    if (std::abs(A.pos[ax] - center[ax]) < eps) return amap[a] = a;
    Anchor& n = sc.addAnchor(mirrorPt(A.pos, ax, center)); n.smooth = A.smooth;
    if (A.raw.contains("sc") && !A.raw.value("sc").isNull()) n.raw.insert("sc", A.raw.value("sc"));
    return amap[a] = n.id;
  };
  auto flipV = [&](Vec3 v) { if (ax == 0) v.x = -v.x; else if (ax == 1) v.y = -v.y; else v.z = -v.z; return v; };
  for (int id : list) {
    const Item cv = *sc.item(id); if (cv.type != ItemType::Curve) continue;
    edit::nextNumber(sc, ItemType::Curve);
    Item& n = sc.addItem(ItemType::Curve, par(id), cv.name + " mirror"); int nid = n.id;
    n.visible = cv.visible;
    if (cv.round) { n.round = cv.round; n.roundSet = true; }
    if (cv.fair) { n.fair = cv.fair; n.fairSet = true; }
    if (cv.para) { n.para = true; n.raw.insert("para", true); }
    std::vector<int> an; for (int a : cv.anchors) an.push_back(cpA(a));
    Item& m = *sc.item(nid); m.anchors = an;
    for (auto& [aid, o] : cv.ovr) if (amap.count(aid)) {
      HandleOverride h; if (o.in) h.in = flipV(*o.in); if (o.out) h.out = flipV(*o.out);
      m.ovr[amap[aid]] = h;
    }
    imap[id] = nid;
  }
  for (int id : list) {
    const Item s = *sc.item(id); if (s.type != ItemType::Surface) continue;
    edit::nextNumber(sc, ItemType::Surface);
    Item& n = sc.addItem(ItemType::Surface, par(id), s.name + " mirror");
    n.visible = s.visible; n.a = mapId(imap, s.a); n.b = mapId(imap, s.b);
    n.raw.insert("p", s.raw.value("p"));
  }
  for (int id : list) {
    const Item gd = *sc.item(id); if (gd.type != ItemType::Guide || gd.raw.value("kind").toString("line") != "line") continue;
    QJsonArray hits; for (const QJsonValue& h : gd.raw.value("hits").toArray()) { QJsonObject o = h.toObject(); remapKey(o, "c", imap); hits.append(o); }
    // (a guide stays in its cocoon, else the Guides group — where makeGuide puts a new one)
    const Item* gp = parentOf(sc, gd);
    int pid = gp && gp->type == ItemType::Cocoon ? gp->id : gd.parent;
    edit::nextNumber(sc, ItemType::Guide);
    Item& n = sc.addItem(ItemType::Guide, pid, gd.name + " mirror");
    n.visible = gd.visible; n.raw = QJsonObject{{"kind", "line"}, {"hits", hits}, {"p", gd.raw.value("p")}};
  }
  // profile modifiers on the mirrored curves: the profile mirrored across the lock, the rotations and the twist turn the
  // other way; the extras go with them
  for (int id : list) {
    const Item t = *sc.item(id);
    if (t.type != ItemType::Tube || !imap.count(t.raw.value("src").toInt(-1))) continue;
    QJsonObject p = t.raw.value("p").toObject();
    for (const char* k : {"rot0", "rot1", "rot2"}) if (p.value(k).toDouble()) p.insert(k, -p.value(k).toDouble());
    edit::nextNumber(sc, ItemType::Tube);
    Item& n = sc.addItem(ItemType::Tube, par(id), t.name + " mirror"); int nid = n.id;
    n.visible = t.visible;
    n.raw = QJsonObject{{"src", imap[t.raw.value("src").toInt()]}, {"profile", flipProf(t.raw.value("profile").toArray())}, {"p", p}};
    if (t.raw.value("profs").isArray()) {
      QJsonArray ps; for (const QJsonValue& P : t.raw.value("profs").toArray()) ps.append(flipProf(P.toArray()));
      n.raw.insert("profs", ps); if (!ps.isEmpty()) n.raw.insert("profile", ps[0]);
    }
    for (int xid : xmodsOf(sc, "tube", id)) {
      const Item x = *sc.item(xid);
      QJsonObject xp = x.raw.value("p").toObject();
      if (x.raw.value("kind").toString() == "twist" && xp.value("deg").toDouble()) xp.insert("deg", -xp.value("deg").toDouble());
      edit::nextNumber(sc, ItemType::XMod);
      Item& y = sc.addItem(ItemType::XMod, par(xid), x.name + " mirror");
      y.visible = x.visible;
      y.raw = QJsonObject{{"tube", nid}, {"kind", x.raw.value("kind")}, {"p", xp}, {"mask", x.raw.value("mask").isArray() ? x.raw.value("mask") : QJsonValue()}};
    }
  }
  for (int id : list) {
    const Item m = *sc.item(id); if (m.type != ItemType::Mesh || m.raw.value("symBaked").toBool() || !m.poly) continue;
    auto poly = std::make_shared<PolyMesh>();
    QJsonArray P, F;
    for (auto& p : m.poly->pos) { Vec3 q = mirrorPt(p, ax, center); poly->pos.push_back(q); P.append(QJsonArray{q.x, q.y, q.z}); }
    for (int f = 0; f < m.poly->faceCount(); f++) {
      std::vector<int> v(m.poly->faceVerts.begin() + m.poly->faceStart[size_t(f)], m.poly->faceVerts.begin() + m.poly->faceStart[size_t(f) + 1]);
      std::reverse(v.begin(), v.end()); poly->addFace(v.data(), int(v.size()));
      QJsonArray q; for (int i : v) q.append(i); F.append(q);
    }
    edit::nextNumber(sc, ItemType::Mesh);
    Item& n = sc.addItem(ItemType::Mesh, par(id), m.name + " mirror");
    n.poly = poly;
    n.raw = QJsonObject{{"data", QJsonObject{{"pos", P}, {"faces", F}}}, {"pending", false}, {"srcSurfs", QJsonArray()}, {"srcCrowns", QJsonArray()}, {"symBaked", false}};
  }
  sym.insert("on", false); sc.item(gid)->raw.insert("sym", sym);
  sc.reindex();
  (void)sel;
  return QObject::tr("Symmetry applied — the mirrored half is in \"%1\"").arg(sc.item(mg)->name);
}

QString duplicateSelection(Scene& sc, Selection& sel) {
  const std::vector<int> order = outlinerOrder(sc);
  std::set<int> selSet(sel.items.begin(), sel.items.end());
  std::vector<int> roots;
  for (int id : order) {
    if (!selSet.count(id)) continue;
    bool under = false; for (const Item* p = parentOf(sc, *sc.item(id)); p; p = parentOf(sc, *p)) if (selSet.count(p->id)) under = true;
    if (!under) roots.push_back(id);
  }
  if (roots.empty() && !sel.anchors.empty())
    for (int id : order) { const Item* it = sc.item(id); if (it->type == ItemType::Curve) for (int a : it->anchors) if (sel.hasAnchor(a)) { roots.push_back(id); break; } }
  if (roots.empty()) return QObject::tr("!Nothing to duplicate");
  std::set<int> inSet;
  std::function<void(int)> mark = [&](int id) { inSet.insert(id); const Item* it = sc.item(id); if (it && it->type == ItemType::Group) for (int c : it->children) mark(c); };
  for (int r : roots) mark(r);
  for (int id : std::vector<int>(inSet.begin(), inSet.end())) {
    const Item* it = sc.item(id); if (it->type != ItemType::Surface) continue;
    for (int c : {it->a, it->b}) if (sc.item(c) && !inSet.count(c)) { roots.push_back(c); mark(c); }
  }
  // a curve goes with its modifier and the modifier's extras
  std::vector<int> tubes; for (int id : order) if (sc.item(id)->type == ItemType::Tube) tubes.push_back(id);
  for (int id : std::vector<int>(inSet.begin(), inSet.end())) if (sc.item(id)->type == ItemType::Curve)
    for (int t : tubes) if (sc.item(t)->raw.value("src").toInt(-1) == id) {
      std::vector<int> ms{t}; for (int x : xmodsOf(sc, "tube", t)) ms.push_back(x);
      for (int m : ms) if (!inSet.count(m)) { roots.push_back(m); mark(m); }
    }
  std::set<int> rootSet(roots.begin(), roots.end());

  std::map<int, int> amap, imap;
  std::vector<int> newAnchors;
  auto cpA = [&](int a) {
    auto f = amap.find(a); if (f != amap.end()) return f->second;
    const Anchor A = *sc.anchor(a);
    Anchor& n = sc.addAnchor(A.pos); n.smooth = A.smooth; n.pass = A.pass;
    for (const char* k : {"at", "ps", "sc"}) if (A.raw.contains(k) && !A.raw.value(k).isNull()) n.raw.insert(k, A.raw.value(k));
    newAnchors.push_back(n.id);
    return amap[a] = n.id;
  };
  std::vector<int> newSurfs, newGuides, newTubes, newCrowns, newX, newMasked;
  std::function<int(int, int, int)> copy = [&](int id, int parent, int afterId) -> int {
    const Item it = *sc.item(id);
    QString name = it.name + " copy";
    if (it.type == ItemType::Group) {
      int n = cloneItem(sc, id, parent, name); place(sc, n, afterId);
      for (int c : it.children) copy(c, n, -1);
      return n;
    }
    int n = -1;
    switch (it.type) {
      case ItemType::Curve: {
        if (it.ring >= 0) return -1;                                   // (disc rings are copied with their disc)
        n = cloneItem(sc, id, parent, name);
        std::vector<int> an; for (int a : it.anchors) an.push_back(cpA(a));
        Item& c = *sc.item(n); c.anchors = an; c.ovr.clear();
        for (auto& [aid, o] : it.ovr) if (amap.count(aid)) c.ovr[amap[aid]] = o;
        if (it.raw.value("rib").isObject()) {                          // a flat lock: its settings on the copied anchors; free of the two anchors
          QJsonObject rib = it.raw.value("rib").toObject(), k0 = rib.value("k").toObject(), k;
          rib.insert("ends", QJsonValue());
          for (int a : it.anchors) if (k0.contains(QString::number(a))) k.insert(QString::number(amap[a]), k0.value(QString::number(a)));
          rib.insert("k", k); c.raw.insert("rib", rib);
        }
        break;
      }
      case ItemType::Surface: n = cloneItem(sc, id, parent, name); newSurfs.push_back(n); break;
      case ItemType::Guide:
        if (it.raw.value("kind").toString() == "disc") {
          QJsonArray rs = it.raw.value("rings").toArray(); if (rs.size() < 2) return -1;
          n = cloneItem(sc, id, parent, name);
          QJsonArray nr;
          for (const QJsonValue& r : rs) {
            const Item* rc = sc.item(r.toInt()); if (!rc) continue;
            int c = cloneItem(sc, rc->id, rc->parent, rc->name + " copy");
            Item& ci = *sc.item(c); ci.ring = n; ci.raw.insert("ring", n); ci.ovr.clear();
            std::vector<int> an; for (int a : rc->anchors) { Anchor& A = sc.addAnchor(sc.anchor(a)->pos); A.smooth = sc.anchor(a)->smooth; an.push_back(A.id); newAnchors.push_back(A.id); }
            sc.item(c)->anchors = an;
            nr.append(c);
          }
          sc.item(n)->raw.insert("rings", nr);
        } else { n = cloneItem(sc, id, parent, name); newGuides.push_back(n); }
        break;
      case ItemType::Mesh: n = cloneItem(sc, id, parent, name); break;
      case ItemType::Crown: {
        n = cloneItem(sc, id, parent, name);
        QJsonObject p = it.raw.value("p").toObject(); p.insert("sym", QJsonValue());   // (symmetry pairs are the original's anchors)
        sc.item(n)->raw.insert("p", p); newCrowns.push_back(n);
        break;
      }
      case ItemType::XMod:
        if (it.raw.value("mesh").isDouble() && rootSet.count(it.raw.value("mesh").toInt())) return -1;   // (copied with its mesh)
        n = cloneItem(sc, id, parent, name); newX.push_back(n);
        break;
      case ItemType::Tube: n = cloneItem(sc, id, parent, name); newTubes.push_back(n); break;
      case ItemType::Cap: case ItemType::Skin: case ItemType::PInf: n = cloneItem(sc, id, parent, name); break;
      case ItemType::Tip: case ItemType::Even: case ItemType::Shell: n = cloneItem(sc, id, parent, name); newMasked.push_back(n); break;
      case ItemType::IMesh: {
        n = cloneItem(sc, id, parent, name); sc.item(n)->headTag = false; sc.item(n)->raw.remove("headTag");
        { Item& c = *sc.item(n); if (c.base) c.base = std::make_shared<PolyMesh>(*c.base); if (c.poly) c.poly = c.base && it.poly == it.base ? c.base : std::make_shared<PolyMesh>(*c.poly); }   // (its own geometry — an edit of one leaves the other)
        int after = n;
        for (int x : xmodsOf(sc, "mesh", id)) {
          int y = cloneItem(sc, x, parent, sc.item(x)->name + " copy"); sc.item(y)->raw.insert("mesh", n);
          place(sc, y, after); after = y;
        }
        break;
      }
      default: return -1;
    }
    imap[id] = n; place(sc, n, afterId);
    return n;
  };
  std::vector<int> made;
  for (int r : roots) { int n = copy(r, sc.item(r)->parent, r); if (n >= 0) made.push_back(n); }
  for (int id : newMasked) {                                           // (a mask follows its copied curves)
    QJsonObject m = sc.item(id)->raw.value("mask").toObject(); if (m.isEmpty()) continue;
    QJsonObject o; for (auto it = m.begin(); it != m.end(); ++it) o.insert(QString::number(mapId(imap, it.key().toInt())), it.value());
    sc.item(id)->raw.insert("mask", o);
  }
  for (int s : newSurfs) { Item& it = *sc.item(s); it.a = mapId(imap, it.a); it.b = mapId(imap, it.b); }
  for (int t : newTubes) remapKey(sc.item(t)->raw, "src", imap);
  for (int g : newGuides) {
    QJsonArray hits; for (const QJsonValue& h : sc.item(g)->raw.value("hits").toArray()) { QJsonObject o = h.toObject(); remapKey(o, "c", imap); hits.append(o); }
    sc.item(g)->raw.insert("hits", hits);
  }
  for (int k : newCrowns) { remapKey(sc.item(k)->raw, "part", imap); remapKey(sc.item(k)->raw, "low", imap); }
  for (int x : newX) {
    QJsonObject& r = sc.item(x)->raw;
    if (r.value("mesh").isDouble()) { remapKey(r, "mesh", imap); continue; }
    remapKey(r, "tube", imap);
    if (r.value("p").toObject().value("only").isArray()) {
      QJsonObject p = r.value("p").toObject(); QJsonArray on;
      for (const QJsonValue& v : p.value("only").toArray()) on.append(mapId(imap, v.toInt()));
      p.insert("only", on); r.insert("p", p);
    }
  }
  for (int a : newAnchors) {
    Anchor& A = *sc.anchor(a);
    if (A.raw.value("at").isObject()) { QJsonObject t = A.raw.value("at").toObject(); remapKey(t, "c", imap); A.raw.insert("at", t); }
    if (A.pass) { A.pass = std::make_pair(mapId(amap, A.pass->first), mapId(amap, A.pass->second)); A.raw.insert("ps", QJsonArray{A.pass->first, A.pass->second}); }
  }
  sel.items = made;
  sel.anchors = newAnchors;           // the copy's anchors are selected: W / E / the brush move the copy right away
  sc.reindex();
  return QObject::tr("Duplicated %1 item(s) — the copy's anchors are selected").arg(made.size());
}

}  // namespace hm::sym
