#include "io/ProjectIO.h"
#include "io/MeshImport.h"
#include "scene/Topo.h"
#include "scene/Refs.h"
#include "scene/Cocoons.h"
#include "scene/Walls.h"
#include "scene/Crown.h"
#include <QSaveFile>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <set>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace hm {

namespace {

Vec3 vec3(const QJsonValue& v) {
  QJsonArray a = v.toArray();
  return a.size() >= 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3{};
}

// a mesh source { ext, text | b64 | ref } → its bytes. ref (pfFetch): a file of its own — in the project folder first
// (rel, inside <project>_files: the project moved with its folder), then its absolute path (file)
QByteArray sourceBytes(const QJsonObject& src, const QString& projectPath, QString* error) {
  if (src.value("text").isString()) return src.value("text").toString().toUtf8();
  if (src.value("b64").isString()) return QByteArray::fromBase64(src.value("b64").toString().toLatin1());
  if (src.value("ref").isObject()) {
    QJsonObject ref = src.value("ref").toObject();
    QString rel = ref.value("rel").toString(), file = ref.value("file").toString();
    QFileInfo pf(projectPath);
    QStringList tries;
    if (!rel.isEmpty()) tries << pf.dir().filePath(pf.completeBaseName() + "_files/" + rel);
    if (!file.isEmpty()) tries << file;
    for (const QString& cand : tries) { QFile f(cand); if (f.open(QIODevice::ReadOnly)) return f.readAll(); }
    if (error) *error = QStringLiteral("Missing mesh file: %1 — shown empty; the reference is kept").arg(rel.isEmpty() ? file : rel);
    return {};
  }
  if (error) *error = QStringLiteral("The mesh has no source");
  return {};
}

void readMesh(Item& it, const QJsonObject& src, const QString& projectPath, LoadResult& res) {
  QString err;
  QByteArray bytes = sourceBytes(src, projectPath, &err);
  if (bytes.isEmpty()) { res.warnings << QStringLiteral("%1: %2").arg(it.name, err); return; }
  it.poly = importMesh(src.value("ext").toString(), bytes, &err, src.value("flat").toBool());
  if (!it.poly) { res.warnings << QStringLiteral("%1: %2").arg(it.name, err); return; }
  if (src.value("split").toBool()) splitPartsIntoGroups(*it.poly);
  applySourceOptions(*it.poly, src);                           // (the import's choices: UVs, creases, hard edges)
}

}  // namespace

std::shared_ptr<PolyMesh> readMeshSource(const QJsonObject& src, const QString& projectPath, QString* error) {
  QByteArray bytes = sourceBytes(src, projectPath, error); if (bytes.isEmpty()) return nullptr;
  auto m = importMesh(src.value("ext").toString(), bytes, error, src.value("flat").toBool());
  if (m && src.value("split").toBool()) splitPartsIntoGroups(*m);
  if (m) applySourceOptions(*m, src);
  return m;
}

Anchor anchorFromJson(const QJsonObject& r) {
  Anchor a; a.id = r.value("id").toInt(); a.pos = vec3(r.value("p")); a.smooth = r.value("s").toDouble(1); a.raw = r;
  QJsonArray ps = r.value("ps").toArray();
  if (ps.size() == 2) a.pass = std::make_pair(ps[0].toInt(), ps[1].toInt());
  return a;
}

Item itemFromJson(const QJsonObject& o, const QString& path, LoadResult& res) {
  Item it;
  it.id = o.value("id").toInt();
  it.typeStr = o.value("type").toString(); it.type = itemTypeFromString(it.typeStr);
  it.name = o.value("name").toString();
  it.parent = o.value("parent").isNull() || o.value("parent").isUndefined() ? -1 : o.value("parent").toInt();
  it.visible = o.value("visible").toBool(true);
  it.raw = o;
  for (const QJsonValue& c : o.value("children").toArray()) it.children.push_back(c.toInt());
  it.color = o.value("color").toString();
  it.expanded = o.value("expanded").toBool();
  switch (it.type) {
    case ItemType::Curve: {
      for (const QJsonValue& a : o.value("anchors").toArray()) it.anchors.push_back(a.toInt());
      for (const QJsonValue& e : o.value("ovr").toArray()) {
        QJsonArray kv = e.toArray(); if (kv.size() < 2) continue;
        QJsonObject h = kv[1].toObject(); HandleOverride ho;
        if (h.value("in").isArray()) ho.in = vec3(h.value("in"));
        if (h.value("out").isArray()) ho.out = vec3(h.value("out"));
        it.ovr[kv[0].toInt()] = ho;
      }
      it.closed = o.value("closed").toBool(); it.junc = o.value("junc").toBool();
      it.seg = o.value("seg").toBool(); it.para = o.value("para").toBool();
      it.ring = o.contains("ring") ? o.value("ring").toInt(-1) : -1;
      it.conn = o.value("conn").toString();
      for (const QJsonValue& s : o.value("sharp").toArray()) it.sharp.push_back(s.toInt());
      it.roundSet = o.contains("round") && !o.value("round").isNull(); it.fairSet = o.contains("fair") && !o.value("fair").isNull();
      it.round = o.value("round").toDouble(0); it.fair = o.value("fair").toDouble(0);
      for (const QJsonValue& s : o.value("path").toArray()) { QJsonArray a = s.toArray(); if (a.size() >= 3) it.path.push_back({a[0].toInt(), a[1].toInt(), a[2].toInt()}); }
      it.hasRib = o.value("rib").isObject();
      break;
    }
    case ItemType::Surface: it.a = o.value("a").toInt(-1); it.b = o.value("b").toInt(-1); break;
    case ItemType::Mesh: {
      QJsonObject d = o.value("data").toObject();
      auto m = std::make_shared<PolyMesh>();
      for (const QJsonValue& p : d.value("pos").toArray()) {
        if (p.isArray()) m->pos.push_back(vec3(p));
        else { QJsonObject q = p.toObject(); m->pos.push_back({q.value("x").toDouble(), q.value("y").toDouble(), q.value("z").toDouble()}); }
      }
      std::vector<int> face;
      for (const QJsonValue& fv : d.value("faces").toArray()) {
        face.clear(); for (const QJsonValue& x : fv.toArray()) face.push_back(x.toInt());
        if (face.size() >= 3) m->addFace(face.data(), int(face.size()));
      }
      if (m->faceCount()) it.poly = m;
      break;
    }
    case ItemType::IMesh:
      it.headTag = o.value("headTag").toBool();
      readMesh(it, o.value("src").toObject(), path, res);
      break;
    default: break;
  }
  return it;
}

LoadResult loadProject(const QString& path, Scene& scene) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) { LoadResult res; res.error = QStringLiteral("Cannot open the file"); return res; }
  return loadProjectData(f.readAll(), path, scene);
}

LoadResult loadProjectData(const QByteArray& bytes, const QString& path, Scene& scene) {
  LoadResult res;
  QJsonParseError pe{};
  QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
  if (doc.isNull()) { res.error = QStringLiteral("Not a project file (%1)").arg(pe.errorString()); return res; }
  QJsonObject P = doc.object();
  if (P.value("app").toString() != "anime-hair-modeler") { res.error = QStringLiteral("Not a HairHarpy project"); return res; }
  if (P.value("preset").toBool()) { res.error = QStringLiteral("This is a preset — migrate it into a project instead of opening it"); return res; }

  scene.clear();
  scene.path = path;

  for (const QJsonValue& v : P.value("anchors").toArray()) { Anchor a = anchorFromJson(v.toObject()); scene.anchors.emplace(a.id, std::move(a)); }

  int order = 0;
  for (const QJsonValue& v : P.value("items").toArray()) {
    Item it = itemFromJson(v.toObject(), path, res); it.order = order++;
    scene.items.emplace(it.id, std::move(it));
  }
  for (const QJsonValue& r : P.value("roots").toArray()) scene.roots.push_back(r.toInt());

  // new ids go on from the highest one (items, anchors, an older head's) — as the HTML version's UID
  QJsonObject head = P.value("head").toObject();
  int maxId = head.value("id").toInt(0);
  for (auto& [id, it] : scene.items) maxId = std::max(maxId, id);
  for (auto& [id, a] : scene.anchors) maxId = std::max(maxId, id);
  // (the other rooms' items and anchors too: their ids come from the same counter)
  for (const QJsonValue& room : P.value("rooms").toObject()) {
    for (const QJsonValue& o : room.toObject().value("items").toArray()) maxId = std::max(maxId, o.toObject().value("id").toInt());
    for (const QJsonValue& o : room.toObject().value("anchors").toArray()) maxId = std::max(maxId, o.toObject().value("id").toInt());
  }
  scene.nextId = maxId + 1; scene.nextOrder = order;

  // an older project's head ({ id, name, visible, src } outside the item list): an imported mesh marked as the head, the
  // first in the Imported group — as loadProject turns it
  if (!head.isEmpty() && head.value("src").isObject()) {
    int g = -1; for (auto& [id, it] : scene.items) if (it.type == ItemType::Group && it.raw.value("importGroup").toBool()) { g = id; break; }
    Item& h = scene.addItem(ItemType::IMesh, g, head.value("name").toString("Head"));
    h.visible = head.value("visible").toBool(true); h.headTag = true;
    // (its record as the HTML version keeps it from now on: the file and its look carried over)
    h.raw = QJsonObject{{"id", h.id}, {"type", "imesh"}, {"src", head.value("src")}, {"headTag", true}};
    if (head.value("look").isObject()) h.raw.insert("look", head.value("look"));
    std::vector<int>& arr = g >= 0 ? scene.item(g)->children : scene.roots;
    arr.pop_back(); arr.insert(arr.begin(), h.id);
    readMesh(h, head.value("src").toObject(), path, res);
  }

  // the references (planes: ids of their own, as the HTML version's RP_UID)
  { QJsonObject R = P.value("refs").toObject(); scene.refBoard = R.value("board").toObject();
    for (const QJsonValue& v : R.value("planes").toArray()) { QString view = v.toObject().value("view").toString(); bool ok = false; for (const char* k : refs::VIEWS) if (view == k) ok = true;
      if (ok) scene.refPlanes.push_back(refs::fromJson(v.toObject(), scene.nextRefId++)); } }

  QJsonObject cam = P.value("camera").toObject();
  if (!cam.isEmpty()) {
    scene.camera.valid = true;
    scene.camera.view = cam.value("view").toString("persp");
    scene.camera.pos = vec3(cam.value("pos"));
    QJsonArray q = cam.value("quat").toArray();
    if (q.size() == 4) scene.camera.quat = Quat{q[0].toDouble(), q[1].toDouble(), q[2].toDouble(), q[3].toDouble()}.normalized();
    scene.camera.zoom = cam.value("zoom").toDouble(1);
    scene.camera.pivot = vec3(cam.value("pivot"));
  }

  // what isn't parsed stays as it was for saving (the big parsed arrays are dropped from this copy)
  for (const char* k : {"anchors", "items", "roots"}) P.remove(QLatin1String(k));
  scene.fileRaw = P;
  scene.reindex();
  crown::contPivots(scene);                                         // (crown continuations made before they had a point: given one)
  scene.revision++;
  res.ok = true;
  return res;
}

// ---------- saving ----------

namespace {
QByteArray js(const QJsonValue& v) {                         // one value as JSON (compact)
  if (v.isObject()) return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
  if (v.isArray()) return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
  QByteArray a = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
  return a.mid(1, a.size() - 2);
}
QJsonArray vec3j(const Vec3& p) { return QJsonArray{p.x, p.y, p.z}; }
}  // namespace

void sceneRecords(const Scene& sc, const SaveExtras& x, QJsonArray& items, QJsonArray& anchors) {
  std::vector<const Item*> order;
  for (auto& [id, it] : sc.items) order.push_back(&it);
  std::sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->order < b->order; });
  // the items: as read, with what this version keeps itself written over (snapshotRaw's fields)
  std::set<int> anchorSeen;
  for (const Item* it : order) {
    QJsonObject o = it->raw;
    o.insert("id", it->id); o.insert("type", it->typeStr); o.insert("name", it->name);
    o.insert("parent", it->parent >= 0 ? QJsonValue(it->parent) : QJsonValue(QJsonValue::Null)); o.insert("visible", it->visible);
    switch (it->type) {
      case ItemType::Group: {
        QJsonArray ch; for (int c : it->children) ch.append(c);
        o.insert("children", ch); o.insert("color", it->color.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(it->color));
        o.insert("expanded", it->expanded);
        if (!o.contains("logic") || o.value("logic").isNull()) o.insert("logic", 1);   // (a file from before the new logic)
        break;
      }
      case ItemType::Cocoon: {
        QJsonArray ch; for (int c : it->children) ch.append(c); o.insert("children", ch);
        o.insert("expanded", it->expanded);
        QJsonObject shape; QJsonArray ends;
        if (x.cocoons && x.cocoons->liveState(it->id, shape, ends)) {
          o.insert("shape", shape);
          if (o.value("live").isObject()) { QJsonObject live = o.value("live").toObject(); live.insert("ends", ends); o.insert("live", live); }
        }
        break;
      }
      case ItemType::Curve: {
        QJsonArray an;
        for (int a : it->anchors) {
          an.append(a);
          if (!anchorSeen.insert(a).second) continue;
          const Anchor* A = sc.anchor(a); if (!A) continue;
          QJsonObject r = A->raw; r.insert("id", a); r.insert("p", vec3j(A->pos)); r.insert("s", A->smooth);
          anchors.append(r);
        }
        o.insert("anchors", an);
        QJsonArray ovr;
        for (auto& [k, h] : it->ovr) ovr.append(QJsonArray{k, QJsonObject{{"in", h.in ? QJsonValue(vec3j(*h.in)) : QJsonValue(QJsonValue::Null)},
                                                                           {"out", h.out ? QJsonValue(vec3j(*h.out)) : QJsonValue(QJsonValue::Null)}}});
        o.insert("ovr", ovr);
        auto flag = [&](const char* k, bool on) { if (on) o.insert(k, true); else o.remove(k); };
        flag("closed", it->closed); flag("junc", it->junc); flag("seg", it->seg);
        if (!it->sharp.empty()) { QJsonArray s; for (int a : it->sharp) if (std::find(it->anchors.begin(), it->anchors.end(), a) != it->anchors.end()) s.append(a); o.insert("sharp", s); } else o.remove("sharp");
        if (it->roundSet) o.insert("round", it->round); else o.remove("round");
        if (it->fairSet) o.insert("fair", it->fair); else o.remove("fair");
        if (it->hasPath()) { QJsonArray p; for (auto& s : it->path) p.append(QJsonArray{s[0], s[1], s[2]}); o.insert("path", p); } else o.remove("path");
        break;
      }
      case ItemType::Surface: o.insert("a", it->a); o.insert("b", it->b); break;
      case ItemType::IMesh:                                    // (edited in the Topology room: its geometry as a packed source)
        if (o.value("src").toObject().value("live").toBool() && it->base) o.insert("src", QJsonObject{{"ext", "pmesh"}, {"b64", QString::fromLatin1(topo::packPMesh(*it->base).toBase64())}, {"blobKind", "b64"}});
        if (it->headTag) o.insert("headTag", true); else o.remove("headTag");
        break;
      default: break;
    }
    items.append(o);
  }
}

QJsonObject roomRecord(const Scene& sc) {
  QJsonArray items, anchors, roots; sceneRecords(sc, {}, items, anchors);
  for (int r : sc.roots) roots.append(r);
  return QJsonObject{{"roots", roots}, {"anchors", anchors}, {"items", items}, {"counters", sc.fileRaw.value("counters").isObject() ? sc.fileRaw.value("counters") : QJsonValue(QJsonObject())}};
}

QByteArray projectJson(const Scene& sc, const SaveExtras& x) {
  const QJsonObject& F = sc.fileRaw;
  QJsonArray items, anchors; sceneRecords(sc, x, items, anchors);
  QJsonArray roots; for (int r : sc.roots) roots.append(r);

  // settings as loadProject leaves them: the loop step the walls use (when the saved one is off by over 2 %), the logic
  // outside the groups
  QJsonObject settings = F.value("settings").toObject();
  { double st = settings.value("step").toDouble(0), u = usedStep(sc);
    if (u > 0 && (!(st > 0) || std::abs(st / u - 1) > 0.02)) settings.insert("step", u);
    if (settings.value("rootLogic").toInt() != 2) {
      bool outside = false;
      for (auto& [id, s] : sc.items) if (s.type == ItemType::Surface) {
        bool inGroup = false; for (const Item* p = s.parent >= 0 ? sc.item(s.parent) : nullptr; p; p = p->parent >= 0 ? sc.item(p->parent) : nullptr) if (p->type == ItemType::Group) inGroup = true;
        if (!inGroup) outside = true;
      }
      settings.insert("rootLogic", outside ? 1 : 2);
    }
    QJsonArray cc; for (int id : x.convChecked) cc.append(id); settings.insert("convChecked", cc); }

  // the keys in projectData()'s order (the thumb near the start: the File window reads it from there)
  std::vector<std::pair<QString, QJsonValue>> top = {
    {"app", F.value("app")}, {"version", F.value("version")},
    {"thumb", x.thumb.isEmpty() ? F.value("thumb") : QJsonValue(x.thumb)},
    {"roots", roots}, {"anchors", anchors}, {"items", items},
    {"conv", F.value("conv")}, {"counters", F.value("counters")}};
  if (F.value("shellModsOff").toBool()) top.push_back({"shellModsOff", true});
  for (const char* k : {"weld", "lockWeld", "weldOk", "weldSnap"}) if (F.contains(k)) top.push_back({k, F.value(k)});
  top.push_back({"head", QJsonValue(QJsonValue::Null)});
  top.push_back({"settings", settings});
  top.push_back({"camera", x.camera.isEmpty() ? F.value("camera") : QJsonValue(x.camera)});
  { // the references: the board as kept, the planes as they are now
    QJsonArray planes; for (const RefPlane& rp : sc.refPlanes) planes.append(refs::toJson(rp));
    top.push_back({"refs", QJsonObject{{"board", sc.refBoard.isEmpty() ? QJsonValue(QJsonObject{{"items", QJsonArray()}, {"pan", QJsonObject{{"x", 0}, {"y", 0}}}, {"zoom", 1}}) : QJsonValue(sc.refBoard)}, {"planes", planes}}});
  }
  if (F.contains("credits")) top.push_back({"credits", F.value("credits")});
  if (F.contains("rooms")) top.push_back({"rooms", F.value("rooms")});
  std::set<QString> done; for (auto& [k, v] : top) done.insert(k);
  for (auto it = F.begin(); it != F.end(); ++it) if (!done.count(it.key()) && it.key() != "head") top.push_back({it.key(), it.value()});

  QByteArray out = "{";
  bool first = true;
  for (auto& [k, v] : top) {
    if (v.isUndefined()) continue;
    if (!first) out += ',';
    first = false;
    out += js(QJsonValue(k)); out += ':'; out += js(v);
  }
  out += '}';
  return out;
}

// the project written whole next to it first (<name>.saving), then put in the old one's place — Windows refuses that
// while something still holds the old file (an antivirus reading the last save — hundreds of MB —, a backup, an
// indexer): tried again for up to 10 s; refused still — the written copy kept and named in the error, nothing lost
bool saveProject(const QString& path, const Scene& sc, const SaveExtras& x, QString* error) {
  const QString tmp = path + ".saving";
  { QFile f(tmp); if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { if (error) *error = f.errorString(); return false; }
    const QByteArray data = projectJson(sc, x);
    if (f.write(data) != data.size() || !f.flush()) { if (error) *error = f.errorString(); f.close(); QFile::remove(tmp); return false; }
    f.close(); }
#ifdef Q_OS_WIN
  const std::wstring from = QDir::toNativeSeparators(tmp).toStdWString(), to = QDir::toNativeSeparators(path).toStdWString(); DWORD e = 0;
  for (int i = 0; i < 40; i++) {
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    e = GetLastError(); if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) break;
    Sleep(250); }
  if (error) *error = QString("%1\n\nThe project is saved next to it as %2 — close what holds %3 (or rename that file yourself).")
                        .arg(qt_error_string(int(e)), QFileInfo(tmp).fileName(), QFileInfo(path).fileName());
  return false;
#else
  QFile::remove(path); if (QFile::rename(tmp, path)) return true;
  if (error) *error = QString("Could not replace %1; the project is saved as %2").arg(QFileInfo(path).fileName(), QFileInfo(tmp).fileName());
  return false;
#endif
}

}  // namespace hm
