#include "io/Migrate.h"
#include "io/ProjectIO.h"
#include <QJsonArray>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace hm {

std::optional<HeadFit> headFit(const Scene& sc, const QJsonObject& P, const QString& projectPath) {
  Box3 db; for (auto& [id, it] : sc.items) if (it.headTag && it.poly) db.unite(it.poly->bounds());
  if (db.empty()) return std::nullopt;
  // the source's head: an older file's P.head, else its meshes marked ☺ (a file of the new logic keeps its head so)
  Box3 sb;
  if (P.value("head").toObject().value("src").isObject()) { if (auto m = readMeshSource(P.value("head").toObject().value("src").toObject(), projectPath)) sb = m->bounds(); }
  else for (const QJsonValue& v : P.value("items").toArray()) { QJsonObject o = v.toObject();
    if (o.value("type").toString() == "imesh" && o.value("headTag").toBool() && o.value("src").isObject()) if (auto m = readMeshSource(o.value("src").toObject(), projectPath)) sb.unite(m->bounds()); }
  if (sb.empty()) return std::nullopt;
  Vec3 ss = sb.size(), ds = db.size(); double k = std::cbrt((ds.x / ss.x) * (ds.y / ss.y) * (ds.z / ss.z));
  auto top = [](const Box3& b) { return Vec3((b.min.x + b.max.x) / 2, b.max.y, (b.min.z + b.max.z) / 2); };
  Vec3 from = top(sb), to = top(db);
  if (!(k > 0) || !std::isfinite(k) || (std::abs(k - 1) < 1e-3 && (from - to).length() < 1e-3 * ds.y)) return std::nullopt;
  return HeadFit{k, from, to};
}

MigrateResult migrateData(Scene& sc, const QJsonObject& P, const QString& name, bool wrap, const std::optional<HeadFit>& fit) {
  MigrateResult r;
  if (P.value("app").toString() != "anime-hair-modeler" || !P.value("items").isArray()) { r.message = QObject::tr("Not a HairHarpy project"); return r; }
  std::map<int, QJsonObject> byId; for (const QJsonValue& v : P.value("items").toArray()) { QJsonObject o = v.toObject(); byId[o.value("id").toInt()] = o; }
  std::set<int> keep;
  std::function<void(int)> walk = [&](int id) {
    auto it = byId.find(id); if (it == byId.end()) return; const QJsonObject& o = it->second; QString t = o.value("type").toString();
    if (t == "head" || t == "guide" || t == "mesh" || t == "imesh" || (t == "xmod" && o.contains("mesh") && !o.value("mesh").isNull())) return;
    if (t == "group" && (o.value("guidesGroup").toBool() || o.value("meshesGroup").toBool() || o.value("importGroup").toBool())) return;
    keep.insert(id); if (t == "group") for (const QJsonValue& c : o.value("children").toArray()) walk(c.toInt());
  };
  std::vector<int> roots;
  if (P.value("roots").isArray()) for (const QJsonValue& v : P.value("roots").toArray()) roots.push_back(v.toInt());
  else for (auto& [id, o] : byId) if (o.value("parent").isNull() || o.value("parent").isUndefined()) roots.push_back(id);
  for (int id : roots) walk(id);
  auto has = [&](const QJsonValue& v) { return keep.count(v.toInt(-1)) > 0; };
  for (bool ch = true; ch;) {                                  // things whose sources stayed behind go too
    ch = false;
    for (int id : std::vector<int>(keep.begin(), keep.end())) {
      const QJsonObject& o = byId[id]; QString t = o.value("type").toString();
      if ((t == "surface" && !(has(o.value("a")) && has(o.value("b")))) || (t == "tube" && !has(o.value("src"))) || (t == "xmod" && !has(o.value("tube")))
          || (t == "crown" && !(has(o.value("part")) && has(o.value("low"))))) { keep.erase(id); ch = true; }
    }
  }
  bool any = false; for (int id : keep) if (byId[id].value("type").toString() != "group") any = true;
  if (!any) { r.message = QObject::tr("Nothing to migrate: no curves, surfaces or modifiers in %1").arg(name); return r; }
  std::map<int, int> imap, amap; std::vector<int> used; std::set<int> usedSet;
  for (int id : keep) { imap[id] = sc.nextId++; const QJsonObject& o = byId[id]; if (o.value("type").toString() == "curve") for (const QJsonValue& a : o.value("anchors").toArray()) if (usedSet.insert(a.toInt()).second) used.push_back(a.toInt()); }
  for (int a : used) amap[a] = sc.nextId++;
  auto R = [&](const QJsonValue& v) -> QJsonValue { auto it = imap.find(v.toInt(-1)); return it == imap.end() ? QJsonValue(QJsonValue::Null) : QJsonValue(it->second); };
  auto RA = [&](int a) { auto it = amap.find(a); return it == amap.end() ? a : it->second; };
  auto fitArr = [&](const QJsonArray& p) { Vec3 v(p[0].toDouble(), p[1].toDouble(), p[2].toDouble()); v = (v - fit->from) * fit->k + fit->to; return QJsonArray{v.x, v.y, v.z}; };
  int topId = -1;
  if (wrap) topId = sc.nextId++;
  std::vector<QJsonObject> made;
  for (int id : keep) {
    QJsonObject o = byId[id]; QString t = o.value("type").toString();
    o.insert("id", imap[id]);
    int par = o.value("parent").toInt(-1); o.insert("parent", !o.value("parent").isNull() && keep.count(par) ? QJsonValue(imap[par]) : (wrap ? QJsonValue(topId) : QJsonValue(QJsonValue::Null)));
    if (t == "group") { QJsonArray ch; for (const QJsonValue& c : o.value("children").toArray()) if (keep.count(c.toInt())) ch.append(imap[c.toInt()]); o.insert("children", ch); }
    if (t == "curve") {
      QJsonArray an; for (const QJsonValue& a : o.value("anchors").toArray()) an.append(RA(a.toInt())); o.insert("anchors", an);
      QJsonArray ov; for (const QJsonValue& e : o.value("ovr").toArray()) { QJsonArray kv = e.toArray(); if (kv.size() < 2) continue; QJsonObject h = kv[1].toObject();
        if (fit) for (const char* s : {"in", "out"}) if (h.value(s).isArray()) { QJsonArray a = h.value(s).toArray(); for (int i = 0; i < a.size(); i++) a[i] = a[i].toDouble() * fit->k; h.insert(s, a); }
        ov.append(QJsonArray{RA(kv[0].toInt()), h}); }
      o.insert("ovr", ov);
      if (fit && o.value("asrc").isArray()) { QJsonArray as; for (const QJsonValue& p : o.value("asrc").toArray()) as.append(fitArr(p.toArray())); o.insert("asrc", as); }
    }
    if (fit) {
      auto len = [&](const char* key, std::initializer_list<const char*> keys) { if (!o.value(key).isObject()) return; QJsonObject q = o.value(key).toObject();
        for (const char* k : keys) if (q.value(k).isDouble()) q.insert(k, q.value(k).toDouble() * fit->k); o.insert(key, q); };
      if (t == "group") len("conv", {"step"});
      if (t == "tube") len("p", {"r0", "r1", "r2", "rRef"});
      if (t == "xmod") len("p", {"amp", "ampRef", "r0", "r1", "r2", "r3", "radRef"});
    }
    if (t == "surface") { o.insert("a", R(o.value("a"))); o.insert("b", R(o.value("b"))); }
    if (t == "tube") o.insert("src", R(o.value("src")));
    if (t == "xmod") {
      o.insert("tube", R(o.value("tube")));
      QJsonObject p = o.value("p").toObject();
      if (p.contains("only") && !p.value("only").isNull()) {
        if (p.value("only").isArray()) { QJsonArray a; for (const QJsonValue& x : p.value("only").toArray()) if (imap.count(x.toInt(-1))) a.append(imap[x.toInt()]); p.insert("only", a); }
        else p.insert("only", R(p.value("only")));
        o.insert("p", p);
      }
    }
    if (t == "crown") { o.insert("part", R(o.value("part"))); o.insert("low", R(o.value("low"))); }
    made.push_back(o);
  }
  std::vector<int> tops; for (int id : roots) if (keep.count(id)) tops.push_back(imap[id]);
  // the records into the scene (as a project opens them; the order: as they were in the file)
  std::sort(made.begin(), made.end(), [&](const QJsonObject& a, const QJsonObject& b) { return a.value("id").toInt() < b.value("id").toInt(); });
  LoadResult lr;
  if (wrap) {
    QJsonArray ch; for (int t : tops) ch.append(t);
    QJsonObject top{{"id", topId}, {"type", "group"}, {"name", name}, {"parent", QJsonValue()}, {"visible", true}, {"children", ch}, {"color", QJsonValue()}, {"expanded", true},
                    {"sym", QJsonValue()}, {"guides", "auto"}, {"conv", QJsonValue()}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false}, {"labels", QJsonValue()}, {"logic", 2}};
    Item g = itemFromJson(top, sc.path, lr); g.order = sc.nextOrder++; sc.items.emplace(g.id, std::move(g)); sc.roots.push_back(topId);
  } else for (int t : tops) sc.roots.push_back(t);
  for (const QJsonObject& o : made) { Item it = itemFromJson(o, sc.path, lr); it.order = sc.nextOrder++; sc.items.emplace(it.id, std::move(it)); }
  std::map<int, QJsonObject> PA; for (const QJsonValue& v : P.value("anchors").toArray()) PA[v.toObject().value("id").toInt()] = v.toObject();
  for (int aid : used) {
    auto it = PA.find(aid); if (it == PA.end()) continue;
    QJsonObject n = it->second; n.insert("id", amap[aid]);
    if (fit && n.value("p").isArray()) n.insert("p", fitArr(n.value("p").toArray()));
    if (n.value("ps").isArray()) { QJsonArray ps; for (const QJsonValue& x : n.value("ps").toArray()) ps.append(RA(x.toInt())); n.insert("ps", ps); }
    if (n.value("at").isObject()) { QJsonObject at = n.value("at").toObject(); auto c = imap.find(at.value("c").toInt(-1)); if (c != imap.end()) { at.insert("c", c->second); n.insert("at", at); } else n.remove("at"); }
    Anchor a = anchorFromJson(n); sc.anchors.emplace(a.id, std::move(a));
  }
  sc.reindex(); sc.revision++;
  int nc = 0; for (int id : keep) if (byId[id].value("type").toString() == "curve") nc++;
  r.ok = true; r.select = wrap ? std::vector<int>{topId} : tops;
  r.message = QObject::tr("Migrated %1 curve%2 from %3").arg(nc).arg(nc == 1 ? "" : "s").arg(name) + (wrap ? QObject::tr(" into the group \"%1\"").arg(name) : QString())
              + (fit ? QObject::tr(" — fitted to the head (scale ×%1)").arg(QString::number(fit->k, 'g', 3)) : QString());
  return r;
}

}  // namespace hm
