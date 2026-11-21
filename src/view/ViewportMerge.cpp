// The outliner's pie (Shift+RMB over the outliner): an empty group at the selection's centre (its own point — the gizmo
// stands there while the group alone is picked, and moves it); the picked meshes merged into one — in world space, each
// one's polygroups kept (named after it when it had none), the UVs and materials when every one of them has them; the
// new mesh's pivot where the user chose
#include "scene/Edit.h"
#include "scene/MeshMods.h"
#include "scene/Topo.h"
#include "view/SceneGeometry.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <map>
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

namespace {
bool plainGroup(const Item& g) { return g.type == ItemType::Group && !g.raw.value("meshesGroup").toBool() && !g.raw.value("guidesGroup").toBool() && !g.raw.value("importGroup").toBool(); }
}  // namespace

// (a picked group stands for the meshes inside it, sub-groups too — merged with the picked meshes, each once)
std::vector<int> Viewport::mergeableMeshes() const {
  std::vector<int> o; std::set<int> seen;
  auto add = [&](int id) { if (const Item* it = scene_->item(id); it && it->type == ItemType::IMesh && baseOf(*it) && baseOf(*it)->faceCount() > 0 && !it->raw.value("tghost").toBool() && seen.insert(id).second) o.push_back(id); };
  std::function<void(const Item&)> walk = [&](const Item& g) { for (int c : g.children) if (const Item* x = scene_->item(c)) { if (x->type == ItemType::Group) walk(*x); else add(c); } };
  for (int id : sel_.items) if (const Item* it = scene_->item(id)) { if (plainGroup(*it)) walk(*it); else add(id); }
  return o;
}

// the first picked group: the merged mesh takes its name and its place
int Viewport::mergeGroup() const {
  for (int id : sel_.items) if (const Item* it = scene_->item(id); it && plainGroup(*it)) return id;
  return -1;
}

QString Viewport::mergeMeshes(const std::vector<int>& ids, const Vec3& pivot, const std::optional<Quat>& axes) {
  std::vector<const Item*> L; for (int id : ids) if (const Item* it = scene_->item(id); it && baseOf(*it)) L.push_back(it);
  if (L.size() < 2) { ui::toast(tr("Pick two meshes or more to merge")); return "!"; }
  bool uv = true, mat = true; for (const Item* it : L) { const PolyMesh& B = *baseOf(*it); uv = uv && !B.cornerUV.empty(); mat = mat && int(B.faceMat.size()) == B.faceCount(); }
  PolyMesh d;
  bool ownN = false; for (const Item* it : L) { const PolyMesh& B = *baseOf(*it); ownN = ownN || (B.cornerNormal.size() == B.faceVerts.size() && !B.cornerNormal.empty()); }   // (the normals of their own: every mesh then brings per-corner ones)
  struct Off { QJsonArray sets; decltype(topo::ttopo(std::shared_ptr<PolyMesh>())) st; int v0, f0; }; std::vector<Off> offs; QJsonArray hidG;
  // the UV sets (the project's, by name): each polygon kept in its set, with its set's layout and the others'; a mesh with none — its polygons in
  // "UV Set 1" as the list showed them. Before, the merged mesh had no sets: all its shells fell into "UV Set 1" on each other, 2026-11-11
  std::vector<QString> setNames; bool sets = false;   // (a mesh with none: its default set — its material's, as the list shows it)
  if (uv) { for (const Item* it : L) sets = sets || topo::hasUvSets(*baseOf(*it));
    if (sets) for (const Item* it : L) { const PolyMesh& B = *baseOf(*it); std::vector<QString> ns = topo::hasUvSets(B) ? B.uvSetNames : std::vector<QString>{topo::defaultUvSetName(B)};
      for (const QString& n : ns) if (std::find(setNames.begin(), setNames.end(), n) == setNames.end()) setNames.push_back(n); }
    if (setNames.size() > 15) sets = false; }                     // (more than a polygon can hold: none kept — as before)
  if (sets) { d.uvSetNames = setNames; d.setUV.assign(setNames.size(), {}); }
  std::map<QString, std::set<QString>> setMats;                   // (each set's mats, by name — as the meshes listed them)
  bool vc = false; for (const Item* it : L) { const PolyMesh& B = *baseOf(*it); vc = vc || (!B.vcol.empty() && B.vcol.size() == B.pos.size()); }   // (the polypaint kept: white for a mesh without)
  for (const Item* it : L) {
    const PolyMesh& B = *baseOf(*it); const Affine M = meshmods::meshMatrix(*scene_, *it); const int v0 = int(d.pos.size()), g0 = int(d.groupNames.size()), m0 = int(d.matNames.size());
    { QJsonArray ss = it->raw.value("selSets").toArray(); offs.push_back({ss, ss.isEmpty() ? nullptr : topo::ttopo(baseSp(*it)), v0, d.faceCount()}); }   // (the sets and their topology kept now: the meshes are gone below)
    for (const QJsonValue& h : it->raw.value("hidGroups").toArray()) hidG.append(g0 + h.toInt());   // (a hidden polygroup stays hidden)
    // (what the meshes carry besides the polygons: the hard edges, creases, UV seams, the normals — into the world space too)
    for (auto& h : B.hardEdges) d.hardEdges.push_back({v0 + h[0], v0 + h[1]});
    for (auto& c : B.creases) d.creases.push_back({v0 + c.a, v0 + c.b, c.w});
    for (auto& s : B.seams) d.seams.push_back({v0 + s[0], v0 + s[1]});
    if (ownN) { const bool has = B.cornerNormal.size() == B.faceVerts.size() && !B.cornerNormal.empty(); const std::vector<Vec3> H = has ? std::vector<Vec3>{} : hardCornerNormals(B);
      for (size_t k = 0; k < B.faceVerts.size(); k++) { const Vec3 n0 = has ? Vec3(B.cornerNormal[k][0], B.cornerNormal[k][1], B.cornerNormal[k][2]) : H[k]; const Vec3 n = M.lin(n0).normalized();
        d.cornerNormal.push_back({float(n.x), float(n.y), float(n.z)}); } }
    for (const Vec3& p : B.pos) d.pos.push_back(M.apply(p));
    if (vc) { const bool has = !B.vcol.empty() && B.vcol.size() == B.pos.size(); for (size_t v = 0; v < B.pos.size(); v++) d.vcol.push_back(has ? B.vcol[v] : std::array<float, 3>{1, 1, 1}); }
    if (B.groupNames.empty()) d.groupNames.push_back(it->name); else d.groupNames.insert(d.groupNames.end(), B.groupNames.begin(), B.groupNames.end());
    if (mat) { d.matNames.insert(d.matNames.end(), B.matNames.begin(), B.matNames.end()); for (size_t k = 0; k < B.matNames.size(); k++) { d.matColor.push_back(k < B.matColor.size() ? B.matColor[k] : QString()); d.matAlpha.push_back(k < B.matAlpha.size() ? B.matAlpha[k] : 0); d.matMaps.push_back(k < B.matMaps.size() ? B.matMaps[k] : std::array<PolyMesh::Map, 2>{}); } }
    const bool bs = sets && topo::hasUvSets(B); std::vector<int> toS;   // (its sets' places in the merged list)
    if (sets) { if (bs) for (const QString& n : B.uvSetNames) toS.push_back(int(std::find(setNames.begin(), setNames.end(), n) - setNames.begin())); else toS.push_back(int(std::find(setNames.begin(), setNames.end(), topo::defaultUvSetName(B)) - setNames.begin()));
      for (const QJsonValue& v : it->raw.value("uvSets").toArray()) { const QJsonObject o = v.toObject(); for (const QJsonValue& mn : o.value("mats").toArray()) setMats[bs ? o.value("name").toString() : topo::defaultUvSetName(B)].insert(mn.toString()); } }
    for (int f = 0; f < B.faceCount(); f++) {
      std::vector<int> vs; for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) { vs.push_back(v0 + B.faceVerts[size_t(k)]); if (uv) d.cornerUV.push_back(B.cornerUV[size_t(k)]);
        if (sets) for (size_t s = 0; s < setNames.size(); s++) { int ks = -1; for (size_t q = 0; q < toS.size(); q++) if (toS[q] == int(s)) ks = int(q);
          d.setUV[s].push_back(bs && ks >= 0 && size_t(ks) < B.setUV.size() ? B.setUV[size_t(ks)][size_t(k)] : B.cornerUV[size_t(k)]); } }   // (its set's own layout; a set it is not in — the one it shows)
      if (sets) { uint32_t bits = 0; int live = -1;
        if (bs) { for (size_t q = 0; q < toS.size(); q++) if (B.faceSets[size_t(f)] >> q & 1u) bits |= 1u << toS[q]; const int l = B.faceLive[size_t(f)]; live = l >= 0 && size_t(l) < toS.size() ? toS[size_t(l)] : -1; }
        else { bits = 1u << toS[0]; live = toS[0]; }
        d.faceSets.push_back(bits); d.faceLive.push_back(int8_t(live)); }
      d.addFace(vs.data(), int(vs.size()));
      d.faceGroup.push_back(g0 + (B.groupNames.empty() || size_t(f) >= B.faceGroup.size() ? 0 : B.faceGroup[size_t(f)]));
      if (mat) d.faceMat.push_back(B.faceMat[size_t(f)] < 0 ? -1 : m0 + B.faceMat[size_t(f)]); }
  }
  const Item* first = L[0]; const QJsonObject keep = first->raw;
  const int gid = mergeGroup(); const Item* grp = scene_->item(gid);   // (a picked group: its name, its place)
  const QString name = grp ? grp->name : first->name; const int parent = grp ? grp->parent : first->parent;
  // (its place in the list: where the first one (the group) stood — counted among the ones that stay — not at the end)
  const int at = grp ? gid : first->id;
  int place = 0; { const std::vector<int>& P0 = scene_->item(parent) ? scene_->item(parent)->children : scene_->roots;
    for (int c : P0) { if (c == at) break; if (std::find(ids.begin(), ids.end(), c) == ids.end()) place++; } }
  sel_ = {}; sel_.items = ids; edit::deleteItems(*scene_, hair_.curves, sel_);   // (the meshes merged gone, their modifiers with them)
  if ((grp = scene_->item(gid))) {                                // (the group left without a single object: gone with its sub-groups)
    std::function<bool(const Item&)> bare = [&](const Item& g) { for (int c : g.children) { const Item* x = scene_->item(c); if (!x || x->type != ItemType::Group || !bare(*x)) return false; } return true; };
    if (bare(*grp)) { sel_ = {}; sel_.items = {gid}; edit::deleteItems(*scene_, hair_.curves, sel_); } }
  const int id = topoNewMesh(scene_->item(parent) ? parent : -1, name, d); if (id < 0) return "!";
  if (const Item* nw = scene_->item(id); nw && nw->parent == parent) { std::vector<int>& P = scene_->item(parent) ? scene_->item(parent)->children : scene_->roots;
    P.erase(std::remove(P.begin(), P.end(), id), P.end()); P.insert(P.begin() + std::min<size_t>(size_t(place), P.size()), id); scene_->reindex(); }
  Item* ni = scene_->item(id);
  for (const char* k : {"look", "smoothN", "twire", "mat", "matPreset", "matOff", "unlit", "pgView"}) if (keep.contains(k)) ni->raw.insert(k, keep.value(k));   // (it looks as the first one did)
  if (ownN) ni->raw.insert("smoothN", true);   // (the normals of their own are seen with smooth shading)
  if (!hidG.isEmpty()) ni->raw.insert("hidGroups", hidG);
  if (sets) { QJsonArray us; for (const QString& n : setNames) { QJsonArray ms; for (const QString& mn : setMats[n]) ms.append(mn); us.append(QJsonObject{{"name", n}, {"mats", ms}}); } ni->raw.insert("uvSets", us); }   // (the list of its sets as the meshes had them)
  // the selection sets: each carried over by the polygons / vertices / edges (a vertex pair) it held, into the merged mesh's numbering
  { const auto NT = topo::ttopo(baseSp(*ni)); QJsonArray sets;
    for (const Off& o : offs) { const QJsonArray& src = o.sets; if (src.isEmpty()) continue;
      const auto& ST = o.st;
      for (const QJsonValue& sv : src) { const QJsonObject s = sv.toObject(); const QString k = s.value("k").toString(); const char kc = k.isEmpty() ? 'f' : k[0].toLatin1();
        const int N0 = kc == 'f' ? ST->nf : kc == 'e' ? ST->ne : ST->n, N1 = kc == 'f' ? NT->nf : kc == 'e' ? NT->ne : NT->n; if (s.value("n").toInt() != N0) continue;   // (made for other geometry)
        const std::vector<char> a = topo::smDec(s.value("d").toString(), N0); std::vector<char> b(size_t(N1), 0); bool any = false;
        for (int i = 0; i < N0 && size_t(i) < a.size(); i++) { if (!a[size_t(i)]) continue; int j = -1;
          if (kc == 'f') j = o.f0 + i; else if (kc == 'v') j = o.v0 + i; else j = NT->edgeOf(o.v0 + ST->ea[size_t(i)], o.v0 + ST->eb[size_t(i)]);
          if (j >= 0 && j < N1) { b[size_t(j)] = 1; any = true; } }
        if (any) sets.append(QJsonObject{{"k", k}, {"n", N1}, {"d", topo::smEnc(b)}, {"name", s.value("name").toString()}}); } }
    if (!sets.isEmpty()) ni->raw.insert("selSets", sets); }
  ni->raw.insert("pivot", QJsonArray{pivot.x, pivot.y, pivot.z});
  if (axes && std::abs(axes->normalized().w) < 1 - 1e-12) {   // (the gizmo's axes of the mesh its pivot came from: kept in a Transform of nothing)
    if (!xformOfMesh(*scene_, id)) meshmods::setMeshMatrix(*scene_, id, Affine{});
    if (Item* x = xformOfMesh(*scene_, id)) { const Quat q = axes->normalized(); QJsonObject p = x->raw.value("p").toObject(); p.insert("gq", QJsonArray{q.x, q.y, q.z, q.w}); x->raw.insert("p", p); }
  }
  sel_ = {}; sel_.items = {id}; onSelChange(); scene_->revision++; emit sceneEdited(); commit(true); emit topoState();
  const QString msg = tr("%1 meshes merged into %2: %3 polygons").arg(L.size()).arg(name).arg(d.faceCount()); ui::toast(msg); return msg;
}

}  // namespace hm
