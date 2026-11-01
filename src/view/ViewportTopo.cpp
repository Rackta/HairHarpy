// The Topology room in the viewport (the HTML version's rooms + "Topology room: modes, Cursor, polygroups"): the room
// switch (each room a scene of its own, Meshes shared), the modes, the Cursor — LMB one element, Shift add, Ctrl
// deselect (a drag — a box), double click the loop (a polygon inside a closed frame of selected ones — everything
// inside), Ctrl+Shift the shortest path (Object mode: the polygroup under the cursor), a drag on empty space a box; a
// drag on an element moves the selection across the screen; the gizmo on the selection; what is drawn over the active
// mesh. Snap (the other meshes' surface, their masks) and Symmetry (the mesh's own X = 0) after every move.
#include "io/ProjectIO.h"
#include "scene/MeshMods.h"
#include "scene/Uv.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QSettings>
#include <cmath>
#include <functional>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
QColor tgColor(int gi) { return QColor::fromHslF(std::fmod(gi * 137.5, 360.0) / 360.0, 0.55, 0.58); }
const QColor TFRONT(0x9e, 0x6b, 0xc4), TBACK(0xe3, 0x85, 0x30), TEDGE(0x46, 0x2e, 0x59), TEDGE_E(0x1a, 0x0d, 0x24);
double segDist(double px, double py, double ax, double ay, double bx, double by) {
  double vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy, t = l2 > 0 ? clamp(((px - ax) * vx + (py - ay) * vy) / l2, 0, 1) : 0;
  return std::hypot(ax + vx * t - px, ay + vy * t - py);
}
std::set<int> hidOf(const Item& m) { std::set<int> h; for (const QJsonValue& v : m.raw.value("hidGroups").toArray()) h.insert(v.toInt()); return h; }
}  // namespace

// ---------- rooms ----------
std::set<int> Viewport::sharedIds(const std::map<int, Item>& items) const {
  std::set<int> ids; int g = -1;
  for (auto& [id, it] : items) if (it.type == ItemType::Group && it.raw.value("meshesGroup").toBool()) { g = id; break; }
  if (g < 0) return ids;
  std::function<void(int)> add = [&](int id) { if (!ids.insert(id).second) return; auto f = items.find(id); if (f != items.end()) for (int c : f->second.children) add(c); };
  add(g);
  for (auto& [id, it] : items) if (it.type == ItemType::XMod && it.raw.value("mesh").isDouble() && ids.count(it.raw.value("mesh").toInt())) ids.insert(id);
  return ids;
}

void Viewport::switchRoom(const QString& name) {
  if (name == room() || (name != "hair" && name != "topo")) return;
  if (drawing_ >= 0) finishDrawing();
  if (ui::Pie::current()) ui::Pie::closeCurrent();
  if (scut_) cancelSurfCut();
  drag_ = Drag::None; td_ = {}; tgz_ = {};
  RoomState& cur = rooms_[room()];
  cur.valid = true; cur.items = scene_->items; cur.anchors = scene_->anchors; cur.roots = scene_->roots;
  cur.hist = history_; cur.idx = histIdx_; cur.sel = sel_; cur.tool = tool_; cur.counters = scene_->fileRaw.value("counters");
  const std::set<int> sh = sharedIds(scene_->items); int sg = -1;
  for (int id : sh) if (const Item* it = scene_->item(id); it && it->type == ItemType::Group && it->raw.value("meshesGroup").toBool()) sg = id;
  RoomState T = rooms_[name];
  const std::set<int> mine = sharedIds(T.items);
  std::map<int, Item> items;
  for (auto& [id, it] : T.items) if (!mine.count(id) && !sh.count(id)) items.emplace(id, it);
  for (int id : sh) if (const Item* it = scene_->item(id)) items.emplace(id, *it);
  std::vector<int> roots; if (sg >= 0 && std::find(scene_->roots.begin(), scene_->roots.end(), sg) != scene_->roots.end()) roots.push_back(sg);
  for (int r : T.roots) if (!mine.count(r) && !sh.count(r) && items.count(r)) roots.push_back(r);
  scene_->items = std::move(items); scene_->anchors = T.anchors; scene_->roots = roots; scene_->room = name;
  if (T.counters.isObject()) scene_->fileRaw.insert("counters", T.counters); else if (name != "hair") scene_->fileRaw.insert("counters", QJsonObject());
  if (scene_->iso) scene_->iso.reset();                      // (an isolation starts over in the other room)
  tgIso_.reset(); tcIso_.reset(); tsel_ = {}; tgPick = {}; tgSel = {}; tgList = {}; thov_ = {}; tgHover_ = {};
  sel_ = {}; for (int id : T.sel.items) if (scene_->item(id)) sel_.items.push_back(id);
  for (int a : T.sel.anchors) if (scene_->anchor(a)) sel_.anchors.push_back(a);
  scene_->reindex(); scene_->revision++;
  history_ = T.hist; histIdx_ = T.idx;
  if (histIdx_ < 0 || history_.empty()) { history_ = {snapshot()}; histIdx_ = 0; history_[0].label = tr("Project opened"); history_[0].weak = false; }   // (a room's first visit: its starting point)
  if (name == "topo") topoMode = "object";
  if (name == "hair") { uvParked_.clear(); for (int i = 0; i < 4; i++) if (paneCamera(i).view == "uv") { uvParked_.push_back(i); setPaneView(i, "persp"); } }
  else { for (int i : uvParked_) if (!uvPaneShown()) setPaneView(i, "uv"); uvParked_.clear(); }   // (one UV Editor at most)
  QString tool = T.valid ? T.tool : QString("cursor");
  if (name == "topo" && tool != "cursor" && tool != "move" && tool != "rotate" && tool != "gizmo" && tool != "tmask") tool = "cursor";
  if (name == "hair" && tool == "tmask") tool = "cursor";
  tool_.clear(); setTool(tool);
  meshSig_.clear(); pickCache_.clear(); bvhCache_.clear();
  reloadHair(); staticRev_++; topoRev_++;
  emit sceneEdited(); onSelChange();
  emit roomChanged(name);
  ui::toast(name == "topo" ? tr("Topology room") : tr("Hair room"));
}

void Viewport::loadRooms() {
  rooms_.clear(); scene_->room = "hair"; tsel_ = {}; tgPick = {}; tgSel = {}; tgList = {}; tgIso_.reset(); tcIso_.reset();
  const QJsonObject R = scene_->fileRaw.value("rooms").toObject();
  for (auto it = R.begin(); it != R.end(); ++it) {
    if (it.key() == "hair" || it.key() != "topo") continue;
    QJsonObject d = it.value().toObject(); RoomState S; S.valid = true; LoadResult res; int order = scene_->nextOrder;
    for (const QJsonValue& v : d.value("items").toArray()) { Item x = itemFromJson(v.toObject(), scene_->path, res); x.order = order++; S.items.emplace(x.id, std::move(x)); }
    for (const QJsonValue& v : d.value("anchors").toArray()) { Anchor a = anchorFromJson(v.toObject()); S.anchors.emplace(a.id, a); }
    for (const QJsonValue& r : d.value("roots").toArray()) S.roots.push_back(r.toInt());
    S.counters = d.value("counters"); S.tool = "cursor";
    scene_->nextOrder = order;
    rooms_[it.key()] = std::move(S);
  }
  emit roomChanged("hair");
}

// P.rooms: the room shown now from the scene, the others from what they keep (their Meshes left out — kept with Hair)
QJsonObject Viewport::roomsJson() const {
  QJsonObject out;
  for (const QString name : {QString("topo")}) {
    Scene tmp; tmp.fileRaw = scene_->fileRaw; tmp.path = scene_->path;
    if (name == room()) { tmp.items = scene_->items; tmp.anchors = scene_->anchors; tmp.roots = scene_->roots; }
    else { auto f = rooms_.find(name); if (f == rooms_.end() || !f->second.valid) continue;
      tmp.items = f->second.items; tmp.anchors = f->second.anchors; tmp.roots = f->second.roots; tmp.fileRaw.insert("counters", f->second.counters); }
    std::set<int> sh = sharedIds(tmp.items);
    for (int id : sh) tmp.items.erase(id);
    tmp.roots.erase(std::remove_if(tmp.roots.begin(), tmp.roots.end(), [&](int r) { return sh.count(r) > 0; }), tmp.roots.end());
    out.insert(name, roomRecord(tmp));
  }
  return out;
}

// (saving while another room is shown: the Hair room's own items with Meshes as they are now)
void Viewport::hairItemsFor(Scene& out) const {
  if (room() == "hair") return;
  auto f = rooms_.find("hair"); if (f == rooms_.end() || !f->second.valid) return;
  const RoomState& H = f->second; const std::set<int> sh = sharedIds(scene_->items), mine = sharedIds(H.items);
  int sg = -1; for (int id : sh) if (const Item* it = scene_->item(id); it && it->raw.value("meshesGroup").toBool()) sg = id;
  std::map<int, Item> items; for (auto& [id, it] : H.items) if (!mine.count(id) && !sh.count(id)) items.emplace(id, it);
  for (int id : sh) items.emplace(id, *scene_->item(id));
  std::vector<int> roots; if (sg >= 0) roots.push_back(sg); for (int r : H.roots) if (!mine.count(r) && !sh.count(r) && items.count(r)) roots.push_back(r);
  out.items = std::move(items); out.anchors = H.anchors; out.roots = roots;
  if (H.counters.isObject()) out.fileRaw.insert("counters", H.counters);
}

// ---------- the active mesh, the selection ----------
std::vector<int> Viewport::topoMeshes() const {
  std::vector<int> o;
  for (auto& [id, it] : scene_->items) if (it.type == ItemType::IMesh && it.poly && scene_->isVisible(it) && !it.raw.value("tref").toBool()) o.push_back(id);
  return o;
}
int Viewport::topoMesh() const {
  if (room() != "topo") return -1;
  for (auto it = sel_.items.rbegin(); it != sel_.items.rend(); ++it) if (const Item* m = scene_->item(*it); m && m->type == ItemType::IMesh) return m->poly && scene_->isVisible(*m) ? *it : -1;
  return -1;
}
bool Viewport::topoHeavy(int m) const { const Item* it = scene_->item(m); return it && meshmods::heavy(*it); }

void Viewport::setTopoMode(const QString& k0, bool quiet) {
  if (drag_ == Drag::Topo) return;
  QString k = k0;
  if (k != "object" && topoHeavy(topoMesh())) { if (!quiet) ui::toast(tr("A high-poly mesh (over 300 000 triangles) is not edited — Object mode only")); k = "object"; }
  topoMode = k; thov_ = {}; if (k != "object") lastEditMode_ = k;
  if (k != "object" && topoMesh() < 0 && !quiet) ui::toast(tr("Select a mesh first — its %1 are picked then").arg(k == "poly" ? tr("polygons") : k == "edge" ? tr("edges") : tr("vertices")));
  updateGizmo(); staticRev_++; topoRev_++; emit topoState(); requestUpdate();
}

void Viewport::tselApply(int m, const std::vector<int>& list, const QString& op, char key, bool sym) {
  ProfScope prof_("tselApply");
  if (!key) key = tKey();
  if (tsel_.id != m) { int ver = tsel_.ver + 1; tsel_ = {}; tsel_.id = m; tsel_.ver = ver; if (const Item* it = scene_->item(m)) tsel_.B = baseOf(*it); }
  std::set<int>& s = key == 'f' ? tsel_.f : key == 'e' ? tsel_.e : tsel_.v;
  std::vector<int> mir; if (sym && tset.sym && room() == "topo" && !list.empty()) mir = tSymList(m, list, key);   // (Symmetry: the mirrored ones too)
  if (op == "set") { s.clear(); s.insert(list.begin(), list.end()); s.insert(mir.begin(), mir.end()); }
  else for (const std::vector<int>* L : std::initializer_list<const std::vector<int>*>{&list, &mir}) { auto h = s.end(); for (int x : *L) { if (op == "add") h = std::next(s.insert(h, x)); else s.erase(x); } }   // (hinted: a polygroup's sorted list goes in at once)
  tsel_.ver++; topoRev_++; updateGizmo(); emit topoState(); requestUpdate(); tselNote();
}
void Viewport::tselClear() { tsel_.f.clear(); tsel_.e.clear(); tsel_.v.clear(); tsel_.last = -1; tsel_.ver++; topoRev_++; updateGizmo(); emit topoState(); requestUpdate(); tselNote(); }
// Ctrl+I: the selection turned inside out — every component of the mode not picked (the hidden left); Object — the meshes
QString Viewport::tInvertSel() {
  if (room() != "topo") return {};
  if (topoMode == "object") { std::vector<int> out; for (int m : topoMeshes()) if (!sel_.hasItem(m)) out.push_back(m); sel_ = {}; sel_.items = out; onSelChange(); ui::hud(tr("Inverted: %1 mesh%2").arg(out.size()).arg(out.size() == 1 ? "" : "es"), 900); return {}; }
  const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  auto T = topo::ttopo(baseSp(*it)); const std::vector<char> hid = meshmods::hiddenFaces(*it, *baseSp(*it)); const char key = tKey();
  const std::set<int> cur = tsel_.id == m ? (key == 'f' ? tsel_.f : key == 'e' ? tsel_.e : tsel_.v) : std::set<int>{};
  std::set<int> out;
  for (int f = 0; f < T->nf; f++) { if (!hid.empty() && hid[size_t(f)]) continue;
    if (key == 'f') { if (!cur.count(f)) out.insert(f); }
    else for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int x = key == 'e' ? T->ce[size_t(k)] : T->fv[size_t(k)]; if (!cur.count(x)) out.insert(x); } }
  tselApply(m, {out.begin(), out.end()}, "set", key); ui::hud(tr("Inverted: %1").arg(out.size()), 900); return {};
}
// Ctrl+A: everything of the mode — every polygon / edge / vertex of the active mesh (the hidden left); Object — every mesh
QString Viewport::tSelectAll() {
  if (room() != "topo") return {};
  if (topoMode == "object") { sel_ = {}; sel_.items = topoMeshes(); onSelChange(); return tr("All meshes"); }
  const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  auto T = topo::ttopo(baseSp(*it)); const std::vector<char> hid = meshmods::hiddenFaces(*it, *baseSp(*it)); const char key = tKey(); std::set<int> out;
  for (int f = 0; f < T->nf; f++) { if (!hid.empty() && hid[size_t(f)]) continue;
    if (key == 'f') out.insert(f); else for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) out.insert(key == 'e' ? T->ce[size_t(k)] : T->fv[size_t(k)]); }
  tselApply(m, {out.begin(), out.end()}, "set", key); ui::hud(tr("All: %1").arg(out.size()), 900); return {};
}

std::vector<int> Viewport::tSelVerts(int m, char key) const {
  const Item* it = scene_->item(m); if (!it || tsel_.id != m) return {};
  auto T = topo::ttopo(baseSp(*it)); if (!T) return {};
  if (!key) key = tKey();
  // (made once per selection: the panels, the gizmo and the Transform table all ask for it)
  const size_t nSel = tsel_.f.size() * 7 + tsel_.e.size() * 3 + tsel_.v.size();
  if (selVC_.m == m && selVC_.ver == tsel_.ver && selVC_.key == key && selVC_.T == T.get() && selVC_.n == nSel) return selVC_.r;
  selVC_.m = m; selVC_.ver = tsel_.ver; selVC_.key = key; selVC_.T = T.get(); selVC_.r = topo::selVerts(*T, key, key == 'f' ? tsel_.f : key == 'e' ? tsel_.e : tsel_.v); selVC_.keep = T; selVC_.n = nSel;
  return selVC_.r;
}

// faces → the mode's elements, selected (Object mode: switched to Polygon first; a high-poly mesh stays in Object mode)
int Viewport::tSelectFaces(int m, const std::vector<int>& faces, const QString& op) {
  const Item* it = scene_->item(m); if (!it) return 0;
  if (sel_.items.size() != 1 || sel_.items[0] != m) { sel_ = {}; sel_.items = {m}; onSelChange(); }
  if (topoHeavy(m)) { tselApply(m, faces, op, 'f'); return int(faces.size()); }
  if (topoMode == "object") setTopoMode("poly");
  auto T = topo::ttopo(baseSp(*it)); const char key = tKey(); std::set<int> out;
  if (key == 'f') out.insert(faces.begin(), faces.end());
  else for (int f : faces) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) out.insert(key == 'e' ? T->ce[size_t(k)] : T->fv[size_t(k)]);
  tselApply(m, {out.begin(), out.end()}, op);
  return int(out.size());
}

bool Viewport::topoDeselect() {
  if (room() != "topo") return false;
  if (topoMode != "object") { if (tsel_.f.size() + tsel_.e.size() + tsel_.v.size()) tselClear(); return true; }
  if (!tgPick.set.empty() || (topoHeavy(topoMesh()) && !tsel_.f.empty())) { if (tsel_.id >= 0) tselApply(tsel_.id, {}, "set", 'f'); tgPick.set.clear(); topoRev_++; emit topoState(); return true; }
  if (!sel_.empty()) { sel_ = {}; onSelChange(); }
  return true;
}
bool Viewport::topoEscape() {
  if (room() != "topo") return false;
  if (tool_ == "tmask") { setTool("cursor"); return true; }
  bool any = false;
  if (topoMode != "object" && tsel_.f.size() + tsel_.e.size() + tsel_.v.size()) { tselClear(); any = true; }
  if (topoMode == "object" && (!tgPick.set.empty() || (topoHeavy(topoMesh()) && !tsel_.f.empty()))) { if (tsel_.id >= 0) tselApply(tsel_.id, {}, "set", 'f'); tgPick.set.clear(); topoRev_++; emit topoState(); any = true; }
  return any;
}

// the polygroups picked for an action: Ctrl+Shift picks, the list's picked rows (folders — all inside), the list's last pick
std::set<int> Viewport::smPickedGroups(int m) const {
  std::set<int> gs; const Item* it = scene_->item(m); if (!it) return gs;
  const PolyMesh* B = baseOf(*it); if (!B) return gs;
  if (tgPick.id == m) gs.insert(tgPick.set.begin(), tgPick.set.end());
  if (tgList.id == m && !tgList.set.empty()) {
    auto tree = topo::tgTree(it->raw.value("gTree"), int(B->groupNames.size()), it->raw.value("gOrder"));
    for (qint64 id : tgList.set) if (topo::TgNode* n = topo::tgFind(tree, id)) for (int g : topo::tgUnder(*n)) gs.insert(g);
  }
  if (gs.empty() && tgSel.id == m && tgSel.gi >= 0) gs.insert(tgSel.gi);
  return gs;
}

// ---------- picking ----------
std::vector<Vec3> Viewport::tWorld(int m) const { const Item* it = scene_->item(m); return it && it->poly ? it->poly->pos : std::vector<Vec3>{}; }

const Viewport::PickCache* Viewport::pickCacheOf(int m) const {
  const Item* it = scene_->item(m); if (!it || !it->poly) return nullptr;
  auto B = baseSp(*it); QByteArray hid = meshmods::hiddenSig(*it);
  PickCache& c = pickCache_[m];
  if (c.poly.lock() == it->poly && c.base.lock() == B && c.hid == hid) return &c;
  c = {}; c.poly = it->poly; c.base = B; c.hid = hid;
  const std::vector<char> h = meshmods::hiddenFaces(*it, *B); const PolyMesh& b = *B;
  for (int f = 0; f < b.faceCount(); f++) {
    if (!h.empty() && h[size_t(f)]) continue;
    int s = b.faceStart[size_t(f)], e = b.faceStart[size_t(f) + 1];
    for (int k = s + 1; k + 1 < e; k++) { c.tris.push_back(uint32_t(b.faceVerts[size_t(s)])); c.tris.push_back(uint32_t(b.faceVerts[size_t(k)])); c.tris.push_back(uint32_t(b.faceVerts[size_t(k + 1)])); c.tri2f.push_back(f); }
  }
  c.box = it->poly->bounds();
  return &c;
}
const topo::Bvh* Viewport::pickBvhOf(int m) const {
  const PickCache* c0 = pickCacheOf(m); if (!c0) return nullptr;
  PickCache& c = pickCache_[m]; const auto& W = scene_->item(m)->poly->pos;
  double sum = double(W.size()); for (size_t i = 0; i < W.size(); i++) sum += W[i].x * 1.3 + W[i].y * 2.7 + W[i].z * 4.1 + double(i % 7) * W[i].x;   // (the vertices moved since: made again)
  if (!c.bvh || c.bvhSum != sum) { c.bvh = std::make_shared<topo::Bvh>(); c.bvh->build(W, c.tris); c.bvhSum = sum; }
  return c.bvh.get();
}

// the polygon under the cursor, its nearest edge and corner on the screen
std::optional<Viewport::TopoPick> Viewport::tPick(const QPointF& mp, const std::vector<int>& list) const {
  ProfScope prof_("tPick");
  std::vector<PickMesh> pms; std::vector<int> ids;
  // (from the camera's near plane: a polygon passing nearer than it is cut off the view — a close-up's — not picked)
  Ray R = ray(mp); { const double tn = cam_.nearPlane() / std::max(1e-6, R.d.dot(cam_.dir())); R.o = R.o + R.d * tn; }
  std::optional<RayHit> h;
  for (int m : list) {
    if (auto hh = heavyRay(m, R)) { if (*hh && (!h || (*hh)->t < h->t)) h = *hh; continue; }   // (a heavy mesh: its hierarchy)
    const PickCache* c = pickCacheOf(m); if (!c) continue; PickMesh pm; pm.itemId = m; pm.pos = &scene_->item(m)->poly->pos; pm.trisRef = &c->tris; pm.box = c->box; pms.push_back(std::move(pm)); }
  if (auto hb = raycast(R, pms); hb && (!h || hb->t < h->t)) h = hb;
  if (!h || h->tri < 0) return std::nullopt;
  if (const int hf = heavyFace(h->itemId, h->tri); hf >= 0 && heavy_.count(h->itemId) && heavy_.at(h->itemId).bvh) {   // (a high-poly mesh: its polygon and nearest corner — no pick cache, no topology made)
    const Item* it = scene_->item(h->itemId); const PolyMesh& B = *baseSp(*it); const auto& W = it->poly->pos;
    TopoPick r; r.m = h->itemId; r.f = hf; r.point = h->point; Projector P = projector(); double dv = 1e300;
    if (size_t(hf) < size_t(B.faceCount()) && W.size() == B.pos.size()) for (int k = B.faceStart[size_t(hf)]; k < B.faceStart[size_t(hf) + 1]; k++) {
      const ScreenPt a = P.toScreen(W[size_t(B.faceVerts[size_t(k)])]); const double d0 = std::hypot(a.x - mp.x(), a.y - mp.y()); if (d0 < dv) { dv = d0; r.v = B.faceVerts[size_t(k)]; } }
    return r;
  }
  const PickCache* c = pickCacheOf(h->itemId); if (!c || size_t(h->tri) >= c->tri2f.size()) return std::nullopt;
  const Item* it = scene_->item(h->itemId); auto T = topo::ttopo(baseSp(*it)); const auto& W = it->poly->pos;
  TopoPick r; r.m = h->itemId; r.f = c->tri2f[size_t(h->tri)]; r.point = h->point;
  Projector P = projector(); double de = 1e300, dv = 1e300;
  for (int k = T->fo[size_t(r.f)]; k < T->fo[size_t(r.f) + 1]; k++) {
    int nk = k + 1 < T->fo[size_t(r.f) + 1] ? k + 1 : T->fo[size_t(r.f)];
    ScreenPt a = P.toScreen(W[size_t(T->fv[size_t(k)])]), b = P.toScreen(W[size_t(T->fv[size_t(nk)])]);
    double d0 = std::hypot(a.x - mp.x(), a.y - mp.y()); if (d0 < dv) { dv = d0; r.v = T->fv[size_t(k)]; }
    double d1 = segDist(mp.x(), mp.y(), a.x, a.y, b.x, b.y); if (d1 < de) { de = d1; r.e = T->ce[size_t(k)]; }
  }
  return r;
}

// the elements of the active mesh inside the screen box that are seen — not hidden by the mesh's own polygons (a
// polygon seen from behind counts: one-sided cards show both sides); X-ray ≥ 0.5 or Select through — all
std::vector<int> Viewport::tBoxPick(int m, double x0, double y0, double x1, double y1, char key) const {
  return tAreaPick(m, x0, y0, x1, y1, key, [](double, double) { return true; });
}
std::vector<int> Viewport::tAreaPick(int m, double x0, double y0, double x1, double y1, char key, const std::function<bool(double, double)>& in) const {
  const Item* it = scene_->item(m); if (!it || !it->poly) return {};
  auto B = baseSp(*it); auto T = topo::ttopo(B); const auto& P = it->poly->pos; const std::vector<char> hid = meshmods::hiddenFaces(*it, *B);
  Projector pr = projector(); const bool ortho = cam_.ortho(); const Vec3 eye = cam_.pos, dir = cam_.dir();
  const bool xray = tset.selThrough || scene_->fileRaw.value("settings").toObject().value("view").toObject().value("xray").toDouble(0.2) >= 0.5;
  std::vector<char> shown(size_t(T->nf), 0);
  for (int f = 0; f < T->nf; f++) shown[size_t(f)] = hid.empty() || !hid[size_t(f)];
  // (seen: a ray from the eye to the point meets nothing of the mesh before it but the element's own polygons)
  const topo::Bvh* bvh = xray ? nullptr : pickBvhOf(m); const PickCache* pc = xray ? nullptr : pickCacheOf(m);
  const double diag = it->poly->bounds().size().length(), eps = std::max(1e-9, diag * 2e-4);
  auto seen = [&](const Vec3& p, const std::function<bool(int)>& own) {
    if (!bvh || !pc) return true;
    Vec3 o = ortho ? p - dir * (diag * 4 + 1) : eye; Vec3 d = p - o; double L = d.length(); if (L <= eps) return true; d = d * (1 / L);
    if (!ortho) { const double tn = cam_.nearPlane() / std::max(1e-6, d.dot(dir)); if (tn >= L - eps) return true; o = o + d * tn; L -= tn; }   // (what lies nearer than the near plane is cut off the view: it hides nothing)
    auto h = bvh->ray(o, d, L - eps); if (!h) return true;
    const int hf = size_t(h->second) < pc->tri2f.size() ? pc->tri2f[size_t(h->second)] : -1; return hf >= 0 && own(hf); };
  auto inside = [&](const Vec3& p) { ScreenPt s = pr.toScreen(p); return s.z <= 1 && s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1 && in(s.x, s.y); };
  auto hasV = [&](int f, int v) { for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) if (T->fv[size_t(k)] == v) return true; return false; };
  std::vector<int> out;
  if (key == 'f') { for (int f = 0; f < T->nf; f++) if (shown[size_t(f)]) { Vec3 c; int s = T->fo[size_t(f)], e = T->fo[size_t(f) + 1]; for (int k = s; k < e; k++) c += P[size_t(T->fv[size_t(k)])]; c = c * (1.0 / (e - s));
      if (inside(c) && seen(c, [f](int g) { return g == f; })) out.push_back(f); } }
  else if (key == 'e') { for (int e = 0; e < T->ne; e++) { const int f0 = T->ef0[size_t(e)], f1 = T->ef1[size_t(e)];
      if (!shown[size_t(f0)] && (f1 < 0 || !shown[size_t(f1)])) continue; const Vec3 c = (P[size_t(T->ea[size_t(e)])] + P[size_t(T->eb[size_t(e)])]) * 0.5;
      if (inside(c) && seen(c, [f0, f1](int g) { return g == f0 || g == f1; })) out.push_back(e); } }
  else { std::vector<char> vF(size_t(T->n), 0); for (int f = 0; f < T->nf; f++) if (shown[size_t(f)]) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) vF[size_t(T->fv[size_t(k)])] = 1;
    for (int v = 0; v < T->n; v++) if (vF[size_t(v)] && inside(P[size_t(v)]) && seen(P[size_t(v)], [&, v](int g) { return hasV(g, v); })) out.push_back(v); }
  return out;
}

// ---------- the Cursor ----------
// (→ handled; the hair tools' handling skipped in this room)
bool Viewport::topoPress(QMouseEvent* e) {
  const QPointF mp = e->position(); const bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  // the snap mask's brush: LMB paints (Ctrl erases), Shift — a lasso; Ctrl+RMB drag — its size
  if (tool_ == "tmask" && (e->button() == Qt::LeftButton || (e->button() == Qt::RightButton && ctrl))) {
    if (e->button() == Qt::RightButton) { bd_ = {}; bd_.start = mp; bd_.r0 = brushR; bd_.h0 = brushHard; drag_ = Drag::BrushAdjust; return true; }
    int m = topoMesh(); if (m < 0) { ui::toast(tr("Select the mesh whose snap mask you paint")); return true; }
    td_ = {}; td_.m = m; td_.erase = ctrl; td_.x0 = mp.x(); td_.y0 = mp.y(); td_.last = mp;
    if (shift || smo.lasso) { td_.type = 5; td_.pts = {{mp.x(), mp.y()}}; drag_ = Drag::Topo; ui::hud(ctrl ? tr("Lasso — erase the snap mask inside") : tr("Lasso — add to the snap mask inside")); return true; }
    auto a = smArr(m); const PolyMesh* B = baseOf(*scene_->item(m));
    ProfScope prof_("maskPress"); if (!smo.thru) td_.occ = meshOccluders(m);                 // (what may hide it: not painted there)
    td_.px = maskProxy(m);                                     // (a high-poly mesh: painted on its proxy)
    if (td_.px) td_.parr = !a.empty() ? pxMaskOf(m, *td_.px) : std::vector<char>(size_t(td_.px->mesh->faceCount()), ctrl ? 1 : 0);
    if (td_.px) td_.parr0 = td_.parr;                          // (what the stroke changes — only that goes onto the mesh)
    else td_.arr = !a.empty() ? a : std::vector<char>(size_t(B->faceCount()), ctrl ? 1 : 0);   // (no mask yet: erasing starts from all)
    td_.type = 4; drag_ = Drag::Topo; smPaint(mp); return true;
  }
  // the move brush: LMB a stroke (Shift — smooth, Shift+Ctrl — relax); Ctrl+RMB drag — its size
  if (tool_ == "treveal" && e->button() == Qt::RightButton && ctrl) { bd_ = {}; bd_.start = mp; bd_.r0 = brushR; bd_.h0 = brushHard; drag_ = Drag::BrushAdjust; return true; }   // (the Reveal Brush: Ctrl+RMB drag — its size, as the brushes)
  if (tool_ == "tbrush" && e->button() == Qt::RightButton && !ctrl && !shift) { tbModePie(e->globalPosition().toPoint()); return true; }   // (RMB: the brush's modes)
  if (tool_ == "tbrush" && (e->button() == Qt::LeftButton || (e->button() == Qt::RightButton && ctrl))) {
    if (e->button() == Qt::RightButton) { bd_ = {}; bd_.start = mp; bd_.r0 = brushR; bd_.h0 = brushHard; drag_ = Drag::BrushAdjust; return true; }
    tbPress(mp, shift, ctrl); return true;
  }
  if (isModelTool() && e->button() == Qt::LeftButton && topoToolPress(e)) return true;   // (the modelling tools)
  if (e->button() != Qt::LeftButton) return e->button() == Qt::RightButton && !shift;   // (RMB: nothing of the hair tools here)
  if (tool_ != "cursor" && tool_ != "move" && tool_ != "rotate" && tool_ != "gizmo" && tool_ != "tchain" && tool_ != "tweld" && tool_ != "tshaper" && tool_ != "tslide" && tool_ != "tnormals" && tool_ != "tbridge" && tool_ != "tmirror" && tool_ != "tfixmesh" && tool_ != "toptimize") return true;
  if ((tool_ == "move" || tool_ == "rotate" || tool_ == "gizmo") && gizmoPress(mp, ctrl && !shift)) return true;   // (the gizmo's handle takes it; Ctrl — extruded first)
  // (Scale + Ctrl, a drag anywhere off its handles: polygons inserted — the selection inset in place, then scaled evenly
  // about its middle: Ctrl+drag in — a ring of new polygons round it)
  if (tool_ == "gizmo" && ctrl && !shift && gz_.visible() && topoMode != "object" && tsel_.id == topoMesh() && (!tsel_.f.empty() || !tsel_.e.empty()) && gizmoPress(mp, true, "XYZ")) return true;
  if ((tool_ == "move" || tool_ == "rotate" || tool_ == "gizmo") && !ctrl && !shift && gz_.visible()) {   // (a drag in free space — off the mesh: the gizmo's centre / view ring)
    const int am = topoMesh(); const bool off = topoMode == "object" ? !tPick(mp, tPickable()) : (am >= 0 && !tPick(mp, {am}));
    if (off && gizmoPressFree(mp)) return true; }
  auto r = tPick(mp, tPickable()); if (r) cam_.pivot = r->point;
  // Fill / Outline: a click picks a region / an open outline (no drag)
  if (tool_ == "cursor" && (tset.selTool == "fill" || tset.selTool == "outline" || tset.selTool == "loop" || tset.selTool == "ring" || tset.selTool == "hard")) return topoClickTool(mp, shift, ctrl);
  // what a drag from here draws — the selection type's: the Select Cursor paints the selection, a rectangle, a lasso
  // (the Move / Rotate / Scale tools: the rectangle)
  auto box = [&](const QString& mode) { td_ = {}; td_.mode = mode; td_.x0 = mp.x(); td_.y0 = mp.y(); td_.empty = !r; td_.last = mp; drag_ = Drag::Topo;
    const QString st = selectsAsCursor() ? tset.selTool : QString("rect");
    td_.type = st == "lasso" ? 6 : st == "cursor" ? 7 : 1; if (td_.type == 6) td_.pts = {{mp.x(), mp.y()}}; };
  // Object mode, Ctrl+Shift: the polygroup under the cursor picked (again — unpicked); a selected reference takes part
  if (topoMode == "object" && ctrl && shift) {
    int am = topoMesh(); std::vector<int> list = topoMeshes(); if (am >= 0 && scene_->item(am)->raw.value("tref").toBool()) list.push_back(am);
    auto rg = tPick(mp, list);
    if (rg) {
      if (rg->m != topoMesh()) { sel_ = {}; sel_.items = {rg->m}; onSelChange(); }
      ProfScope prof_("groupPick");
      const Item* it = scene_->item(rg->m); const PolyMesh* B = baseOf(*it); int gi = B->faceGroup[size_t(rg->f)];
      if (tgPick.id != rg->m || tgPick.B != B) { tgPick = {}; tgPick.id = rg->m; tgPick.B = B; }
      bool had = tgPick.set.count(gi); if (had) tgPick.set.erase(gi); else tgPick.set.insert(gi);
      tselApply(rg->m, topo::groupFaces(*B, gi), had ? "remove" : "add", 'f', false);
      tgSel = had ? TgSel{} : TgSel{rg->m, gi}; tgHover_ = {}; emit topoState();
      ui::hud(tr("%1 %2 · %3 polygroup%4").arg(B->groupNames[size_t(gi)], had ? tr("unpicked") : tr("picked")).arg(tgPick.set.size()).arg(tgPick.set.size() == 1 ? "" : "s"), 1000);
      return true;
    }
  }
  if (topoMode == "object") {
    if (!r) { box(ctrl ? "remove" : shift ? "add" : "set"); if (ctrl && td_.type == 7) td_.clearAll = true; return true; }   // (Ctrl over empty space: the selection off — the brush only; Rectangle / Lasso: Ctrl takes off what they hold)
    const int id = r->m;
    if (ctrl) { sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), id), sel_.items.end()); onSelChange(); return true; }
    if (shift) { sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), id), sel_.items.end()); sel_.items.push_back(id); }
    else if (!sel_.hasItem(id)) sel_.items = {id};
    sel_.anchors.clear(); onSelChange();
    td_ = {}; td_.type = 2; td_.x0 = mp.x(); td_.y0 = mp.y(); td_.n = cam_.dir(); td_.ref = r->point; td_.hit0 = planeHit(ray(mp), r->point, td_.n);
    for (int m : topoMeshes()) if (sel_.hasItem(m)) { td_.list.push_back(m); td_.starts.push_back(meshmods::meshMatrix(*scene_, *scene_->item(m))); }
    drag_ = Drag::Topo; return true;
  }
  // a component mode: only the active mesh (another one is picked in the Object mode)
  if (topoMesh() < 0) {                                         // (nothing picked yet: a click on a mesh picks it — its elements then)
    if (auto rm = tPick(mp, topoMeshes())) { sel_ = {}; sel_.items = {rm->m}; onSelChange(); ui::hud(tr("Editing %1").arg(scene_->item(rm->m)->name), 900); return true; }
    ui::toast(tr("Click a mesh to edit it")); return true; }
  if (!r) { box(ctrl ? "remove" : shift ? "add" : "set"); if (ctrl && td_.type == 7) td_.clearAll = true; return true; }   // (Ctrl + a stroke over empty space: everything deselected)   // (Ctrl over empty space: the selection off — the brush only; Rectangle / Lasso: Ctrl takes off what they hold)
  const int m = r->m; const char key = tKey(); const int el = key == 'f' ? r->f : key == 'e' ? r->e : r->v;
  if (tsel_.id != m) tselApply(m, {}, "set");
  std::set<int>& sel = key == 'f' ? tsel_.f : key == 'e' ? tsel_.e : tsel_.v;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const bool dbl = tclick_.id == m && tclick_.key == key && tclick_.el == el && now - tclick_.t < 400;
  // (the selection before this click is kept: the first click of a double click replaces it)
  TClick pc = tclick_; tclick_ = dbl ? TClick{} : TClick{m, key, el, now, std::vector<int>(sel.begin(), sel.end()), tsel_.last};
  if (ctrl && shift) {                                         // the shortest path from the last picked
    int last = tsel_.last; auto T = topo::ttopo(baseSp(*scene_->item(m)));
    if (last >= 0 && last != el && !sel.empty()) { auto p = topo::path(*T, tWorld(m), key, last, el); if (p) tselApply(m, *p, "add"); else { ui::toast(tr("No path between them (different parts of the mesh)")); tselApply(m, {el}, "add"); } }
    else tselApply(m, {el}, "add");
    tsel_.last = el; return true;
  }
  if (ctrl) { box("remove"); td_.onEl = TDrag::OnEl{m, el, r->f, r->e, dbl}; return true; }
  if (dbl) { tselApply(m, pc.prev, "set"); TopoPick r2 = *r; if (key == 'f') r2.e = loopEdgeAt(m, r->f, mp); topoDouble(m, r2, key, pc.lastBefore, pc.prev); return true; }
  // on what isn't selected (or with Shift): a click picks it, a drag draws the selection type's gesture; on the
  // selection — a drag moves it
  if (!sel.count(el) || shift) { box(shift ? "add" : "set"); td_.onEl = TDrag::OnEl{m, el, r->f, r->e, false}; return true; }
  tsel_.last = el;
  if (tool_ == "tbridge") { tt_.m = m; tt_.press = mp; tt_.moved = false; tt_.ctrl = tt_.shift = false; tt_.adjDone = 0; tt_.adj0 = 1; tt_.drag = 17; td_ = {}; td_.type = 20; drag_ = Drag::Topo; return true; }   // (Bridge: a drag from the selection — the bridge, never the selection moved)
  td_ = {}; td_.type = 3; td_.m = m; td_.x0 = mp.x(); td_.y0 = mp.y(); td_.n = cam_.dir(); td_.ref = r->point; td_.hit0 = planeHit(ray(mp), r->point, td_.n);
  drag_ = Drag::Topo;
  return true;
}

// double click: the loop through the element; a polygon inside a closed frame of selected polygons — everything inside
void Viewport::topoDouble(int m, const TopoPick& r, char key, int a, const std::vector<int>& prev) {
  auto T = topo::ttopo(baseSp(*scene_->item(m)));
  const int b = key == 'f' ? r.f : key == 'e' ? r.e : r.v;
  // (after a click on a picked element a: the loop through a and this one — every so many as far apart as they are)
  if (a >= 0 && a != b && std::find(prev.begin(), prev.end(), a) != prev.end()) {
    std::set<int> stop(prev.begin(), prev.end()); stop.erase(a);
    auto L = tPatternWalk(m, key, a, b, stop);
    if (!L.empty()) { tselApply(m, L, "add"); tsel_.last = b; ui::hud(tr("Loop: %1 picked").arg(L.size()), 1200); return; }
  }
  if (key == 'f') {                                             // (alone: the whole piece, up to its break)
    const Item* it = scene_->item(m); const std::vector<char> hid = meshmods::hiddenFaces(*it, *baseSp(*it));
    std::vector<char> seen(size_t(T->nf), 0); std::vector<int> L, st{r.f}; seen[size_t(r.f)] = 1;
    while (!st.empty()) { int f = st.back(); st.pop_back(); L.push_back(f);
      for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { int e = T->ce[size_t(k)], g = T->ef0[size_t(e)] == f ? T->ef1[size_t(e)] : T->ef0[size_t(e)];
        if (g >= 0 && !seen[size_t(g)] && (hid.empty() || !hid[size_t(g)])) { seen[size_t(g)] = 1; st.push_back(g); } } }
    tselApply(m, L, "add"); ui::hud(tr("Piece: %1 polygon%2").arg(L.size()).arg(L.size() == 1 ? "" : "s"), 1200);
  } else if (key == 'e') { auto L = topo::edgeLoop(*T, r.e); tselApply(m, L, "add"); ui::hud(tr("Edge loop: %1").arg(L.size()), 1200); }
  else { auto L = topo::edgeLoop(*T, r.e); std::set<int> vs; for (int e : L) { vs.insert(T->ea[size_t(e)]); vs.insert(T->eb[size_t(e)]); } tselApply(m, {vs.begin(), vs.end()}, "add"); ui::hud(tr("Vertex loop: %1").arg(vs.size()), 1200); }
  tsel_.last = key == 'f' ? r.f : key == 'e' ? r.e : r.v;
}

bool Viewport::topoDblClick(QMouseEvent*) { return room() == "topo"; }   // (the double click is told by the press times)

void Viewport::topoHover(const QPointF& mp, Qt::KeyboardModifiers mods) {
  if (tool_ == "tbrush") { overlayRev_++; requestUpdate(); return; }   // (the brush: its circles drawn with the overlay at the pointer)
  const bool over = (tool_ == "cursor" || tool_ == "move" || tool_ == "rotate" || tool_ == "gizmo" || tool_ == "tchain" || tool_ == "tweld" || tool_ == "tshaper" || tool_ == "tslide" || tool_ == "tnormals" || tool_ == "tbridge" || tool_ == "tmirror" || tool_ == "tfixmesh" || tool_ == "toptimize") && !ui::Pie::current();
  // Object mode, Ctrl+Shift over the selected mesh: its polygroup under the cursor lights up
  if (over && topoMode == "object" && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
    int am = topoMesh(); auto r = am >= 0 ? tPick(mp, {am}) : std::nullopt;
    TgHover h; if (r) { h.id = am; h.gi = baseOf(*scene_->item(am))->faceGroup[size_t(r->f)]; }
    if (h.id != tgHover_.id || h.gi != tgHover_.gi) { tgHover_ = h; topoRev_++; requestUpdate(); if (h.id >= 0) ui::hud(tr("Polygroup: %1 — click to select it").arg(baseOf(*scene_->item(am))->groupNames[size_t(h.gi)])); else ui::hud({}); }
    return;
  }
  if (tgHover_.id >= 0) { tgHover_ = {}; topoRev_++; ui::hud({}); requestUpdate(); }
  // Fill / Outline: the region / the open outline a click would pick, shown under the pointer
  if (over && tool_ == "cursor" && (tset.selTool == "fill" || tset.selTool == "outline" || tset.selTool == "loop" || tset.selTool == "ring" || tset.selTool == "hard")) {
    if (tset.selTool == "hard") {                                // (Hard edges: the line of hard edges nearest the pointer lit)
      THover h; h.key = 'e'; int am = topoMesh(); if (am < 0) if (auto r = tPick(mp, topoMeshes())) am = r->m;
      const int e = am >= 0 && !topoHeavy(am) ? hardEdgeNear(am, mp) : -1; if (e >= 0) { h.m = am; h.e = e; }
      if (h.m != thov_.m || h.e != thov_.e || thov_.ver != tsel_.ver) { if (h.m >= 0) h.rim = hardLine(h.m, e); h.ver = tsel_.ver; thov_ = h; topoRev_++; requestUpdate(); }
      return; }
    const bool loop = tset.selTool == "loop" || tset.selTool == "ring";
    THover h; h.inv = tset.selTool == "fill" && (mods & Qt::ControlModifier); h.key = topoMode == "object" ? 'f' : tKey();
    std::optional<TopoPick> pick = tPick(mp, tPickable());
    if (!pick && tset.selTool == "outline") { const int am = topoMesh(); if (int e = am >= 0 ? rimEdgeNear(am, mp) : -1; e >= 0) { TopoPick p; p.m = am; p.e = e; pick = p; } }
    if (pick && !topoHeavy(pick->m)) { h.m = pick->m; h.f = pick->f; h.e = pick->e; if (loop && h.key == 'f') h.e = loopEdgeAt(h.m, h.f, mp); }
    if (h.m != thov_.m || h.f != thov_.f || h.e != thov_.e || thov_.ver != tsel_.ver || h.inv != thov_.inv || h.key != thov_.key) {
      if (h.m >= 0 && loop) {
        auto L = tLoopOf(h.m, *pick, mp, tset.selTool == "ring");
        if (h.key == 'f') h.region = L; else if (h.key == 'e') h.rim = L; else h.pts = L;
      } else if (h.m >= 0) {
        if (tset.selTool == "fill") h.region = fillRegion(h.m, h.f, h.inv);
        else if (tsel_.id == h.m && !tsel_.f.empty()) { auto T = topo::ttopo(baseSp(*scene_->item(h.m)));   // (picked polygons: their border)
          for (int e = 0; e < T->ne; e++) { bool a = tsel_.f.count(T->ef0[size_t(e)]) > 0, b = T->ef1[size_t(e)] >= 0 && tsel_.f.count(T->ef1[size_t(e)]) > 0; if (a != b) h.rim.push_back(e); } }
        else h.rim = rimAll(h.m);                               // (over the mesh: all its borders lit)
      }
      h.ver = tsel_.ver; thov_ = h; topoRev_++; requestUpdate();
    }
    return;
  }
  THover h; int m = topoMesh();
  if (over && m >= 0 && topoMode != "object") if (auto r = tPick(mp, {m})) { h.m = m; if (topoMode == "poly") h.f = r->f; else if (topoMode == "edge") h.e = r->e; else h.v = r->v; }
  if (h.m != thov_.m || h.f != thov_.f || h.e != thov_.e || h.v != thov_.v) { thov_ = h; topoRev_++; requestUpdate(); }
}

bool Viewport::topoMove(const QPointF& mp, Qt::KeyboardModifiers mods) {
  if (softAdj_.on && drag_ == Drag::None) {                  // (B held: the pointer's sideways drag — the soft radius)
    if (!softAdj_.has0) { softAdj_.has0 = true; softAdj_.x0 = mp.x(); return true; }
    const double dx = mp.x() - softAdj_.x0; if (!softAdj_.moved && std::abs(dx) < 4) return true;
    softAdj_.moved = true; tset.soft = true; tset.softR = std::clamp(softAdj_.r0 * std::exp(dx / 120), 0.01, 1e5);
    ui::hud(tr("Soft Selection radius: %1 cm").arg(tset.softR, 0, 'g', 3)); topoRev_++; requestUpdate(); return true;
  }
  if (isModelTool() && (drag_ == Drag::None || (drag_ == Drag::Topo && td_.type == 20)) && topoToolMove(mp, mods)) return true;
  if (drag_ != Drag::Topo) { if (drag_ == Drag::None) topoHover(mp, mods); return false; }
  TDrag& d = td_;
  if (d.type == 8) { tbMove(mp); return true; }                // (the move brush's stroke)
  if (d.type == 5) {                                            // (the lasso's outline as it goes)
    auto [lx, ly] = d.pts.back(); if (std::hypot(mp.x() - lx, mp.y() - ly) >= 3) d.pts.push_back({mp.x(), mp.y()});
    lasso_ = d.pts; overlayRev_++; requestUpdate(); return true;
  }
  if (d.type == 4) {                                            // (painted along the way: steps of a third of the radius)
    int n = std::max(1, int(std::ceil(std::hypot(mp.x() - d.last.x(), mp.y() - d.last.y()) / std::max(2.0, brushR / 3))));
    for (int i = 1; i <= n; i++) smPaint(d.last + (mp - d.last) * (double(i) / n));
    d.last = mp; overlayRev_++; requestUpdate(); return true;
  }
  if (!d.moved && std::hypot(mp.x() - d.x0, mp.y() - d.y0) < 4) return true;
  if (!d.moved) {
    d.moved = true; thov_ = {}; topoRev_++;
    if (d.type == 3) {                                          // the drag starts: the selection's vertices, from where they are
      Item* it = scene_->item(d.m); d.verts = tSelVerts(d.m); d.base = baseOf(*it)->pos; d.uv0 = baseOf(*it)->cornerUV;
      d.inv = meshmods::meshMatrix(*scene_, *it).inverse();
      if (tset.sym) d.pairs = symFirstPairs(d.m, d.base);
    }
  }
  if (d.type == 1) { lasso_ = {{d.x0, d.y0}, {mp.x(), d.y0}, {mp.x(), mp.y()}, {d.x0, mp.y()}}; overlayRev_++; requestUpdate(); return true; }
  if (d.type == 6) {                                            // (the selection lasso's outline as it goes)
    auto [lx, ly] = d.pts.back(); if (std::hypot(mp.x() - lx, mp.y() - ly) >= 3) d.pts.push_back({mp.x(), mp.y()});
    lasso_ = d.pts; overlayRev_++; requestUpdate(); return true;
  }
  if (d.type == 7) {                                            // (the Select Cursor's drag: painted along the way, the brush's circle shown)
    const double R = 14; int n = std::max(1, int(std::ceil(std::hypot(mp.x() - d.last.x(), mp.y() - d.last.y()) / (R / 2))));
    if (d.clearAll) {                                           // (Ctrl, from empty space: the whole selection off, once)
      if (!d.erase) { d.erase = true;
        if (topoMode == "object") { if (!sel_.empty()) { sel_ = {}; onSelChange(); } }
        else if (tsel_.f.size() + tsel_.e.size() + tsel_.v.size()) tselClear();
        ui::hud(tr("Selection cleared"), 900); }
    } else {
      if (!d.erase) { d.erase = true; tPaintAt(d.last, td_.mode); }   // (the press point too, once)
      for (int i = 1; i <= n; i++) tPaintAt(d.last + (mp - d.last) * (double(i) / n), td_.mode);
    }
    td_.last = mp; lasso_.clear(); for (int i = 0; i < 24; i++) { double a = i * M_PI / 12; lasso_.push_back({mp.x() + std::cos(a) * R, mp.y() + std::sin(a) * R}); }
    overlayRev_++; requestUpdate(); return true;
  }
  auto h = planeHit(ray(mp), d.ref, d.n); if (!h || !d.hit0) return true;
  Vec3 w = *h - *d.hit0;
  if ((mods & Qt::ShiftModifier) && tset.gzStep > 0 && (d.type == 2 || d.type == 3)) { const double s = tset.gzStep; w = Vec3(std::round(w.x / s) * s, std::round(w.y / s) * s, std::round(w.z / s) * s); }   // (Shift: in steps, as the gizmo)
  if (d.type == 2) {
    Affine T; T.t = w; d.w = w;
    for (size_t i = 0; i < d.list.size(); i++) meshmods::setMeshMatrix(*scene_, d.list[i], Affine::mul(T, d.starts[i]));
    requestUpdate(); return true;
  }
  if (d.type == 3) {
    // (the element snap: the picked vertex nearest the press taken hold of — it goes onto the element under the pointer,
    // the rest with it by the same offset)
    bool snapped = false;
    if (tset.esnap) { Item* mi = scene_->item(d.m);
      if (mi && d.handle < 0) { const Affine M = meshmods::meshMatrix(*scene_, *mi); Projector Pj = projector(); double hb = 1e300;
        for (int v : d.verts) { const Vec3 wv = M.apply(d.base[size_t(v)]); const ScreenPt s = Pj.toScreen(wv); const double dd = std::hypot(s.x - d.x0, s.y - d.y0); if (s.z < 1 && dd < hb) { hb = dd; d.handle = v; d.handleW = wv; } } }
      if (d.handle >= 0) { const std::set<int> mv(d.verts.begin(), d.verts.end()); esnapHit_ = elemSnap(mp, d.m, mv, d.handleW); if (esnapHit_) { w = *esnapHit_ - d.handleW; snapped = true; } overlayRev_++; } }
    Vec3 l = d.inv.lin(w); std::vector<Vec3> P = d.base;
    for (int v : d.verts) P[size_t(v)] = d.base[size_t(v)] + l;
    { const bool s0 = tset.snap; if (snapped) tset.snap = false; topoAfterMove(d.m, P, d.verts, d.base, tset.sym ? &d.pairs : nullptr); tset.snap = s0; }   // (snapped to an element: not laid onto the surface on top)
    if (tset.keepUv && !d.uv0.empty()) { auto U = uvKeep(d.m, d.base, d.uv0, P); topoLive(d.m, P, &U); } else topoLive(d.m, P);
    ui::hud(tr("Moving %1 vert%2").arg(d.verts.size()).arg(d.verts.size() == 1 ? "ex" : "ices"));
  }
  return true;
}

bool Viewport::topoRelease(const QPointF& mp) {
  if (drag_ != Drag::Topo) return false;
  if (td_.type == 20) { drag_ = Drag::None; td_ = {}; if (esnapHit_) { esnapHit_.reset(); overlayRev_++; } return topoToolRelease(mp); }   // (a modelling tool's press / drag)
  drag_ = Drag::None; TDrag d = td_; td_ = {}; if (esnapHit_) { esnapHit_.reset(); overlayRev_++; }
  if (d.type == 8) { tbRelease(); return true; }
  if (d.type == 4) { ProfScope prof_("maskRelease"); if (Item* it = scene_->item(d.m)) smSet(*it, d.px ? pxApply(*d.px, d.parr0, d.parr, [&] { auto b = smArr(d.m); if (b.empty() && d.erase) b.assign(d.px->f2p.size(), 1); return b; }(), faceLocked(d.m)) : d.arr); commit(false); emit topoState(); return true; }   // (the stroke — one undo step)
  if (d.type == 5) { lasso_.clear(); overlayRev_++; ui::hud({}); td_ = d; smLasso(); td_ = {}; return true; }
  if (d.type == 1 || d.type == 6 || d.type == 7) {
    lasso_.clear(); overlayRev_++;
    if (!d.moved) {
      if (d.onEl) { const auto& o = *d.onEl; const char key = tKey();   // (a click on an element: picked / taken off)
        if (d.mode != "remove") { tselApply(o.m, {o.el}, d.mode == "add" ? "add" : "set"); tsel_.last = o.el; return true; }
        if (o.dbl) { auto T = topo::ttopo(baseSp(*scene_->item(o.m))); std::vector<int> L = key == 'e' ? topo::edgeLoop(*T, o.e) : key == 'f' ? topo::faceLoop(*T, o.f, o.e) : std::vector<int>{o.el}; tselApply(o.m, L, "remove"); }
        else tselApply(o.m, {o.el}, "remove");
        return true; }
      if (d.mode == "set") {
        if (topoMode == "object") { if (!tgPick.set.empty()) { tgPick.set.clear(); emit topoState(); } if (!sel_.empty()) { sel_ = {}; onSelChange(); } }
        else if (tsel_.f.size() + tsel_.e.size() + tsel_.v.size()) tselClear();
      }
      requestUpdate(); return true;
    }
    if (d.type == 7) { ui::hud({}); return true; }              // (painted as it went)
    double x0 = std::min(d.x0, mp.x()), x1 = std::max(d.x0, mp.x()), y0 = std::min(d.y0, mp.y()), y1 = std::max(d.y0, mp.y());
    std::function<bool(double, double)> in = [](double, double) { return true; };
    if (d.type == 6) { d.pts.push_back({mp.x(), mp.y()}); x0 = y0 = 1e300; x1 = y1 = -1e300;   // (the lasso: its box, then inside it)
      for (auto [px, py] : d.pts) { x0 = std::min(x0, px); x1 = std::max(x1, px); y0 = std::min(y0, py); y1 = std::max(y1, py); }
      auto pts = d.pts; in = [pts](double x, double y) { return insidePolygon(pts, x, y); }; }
    if (topoMode == "object") {
      Projector P = projector(); std::vector<int> hit;
      for (int m : topoMeshes()) { ScreenPt s = P.toScreen(scene_->item(m)->poly->bounds().center()); if (s.z < 1 && s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1 && in(s.x, s.y)) hit.push_back(m); }
      if (d.mode == "set") sel_.items = hit;
      else for (int id : hit) { sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), id), sel_.items.end()); if (d.mode == "add") sel_.items.push_back(id); }
      sel_.anchors.clear(); onSelChange(); return true;
    }
    int m = topoMesh(); if (m < 0) return true;
    auto list = tAreaPick(m, x0, y0, x1, y1, tKey(), in); tselApply(m, list, d.mode);
    ui::hud(tr("%1 %2").arg(list.size()).arg(d.mode == "remove" ? tr("deselected") : tr("selected")), 900);
    return true;
  }
  if (!d.moved) return true;
  if (d.type == 2) { commit(false); emit topoState(); return true; }
  if (d.type == 3) { ui::hud({}); if (Item* it = scene_->item(d.m)) { auto B = baseSp(*it); topoWrite(d.m, B); } commit(false); emit topoState(); }
  return true;
}

// the edited vertices' new places in P: Snap — each onto the nearest point of the other shown meshes' surface (their
// masked polygons, where they have a snap mask); Symmetry — their pairs mirrored (a pair edited too keeps its own place)
void Viewport::topoAfterMove(int m, std::vector<Vec3>& P, const std::vector<int>& verts, const std::vector<Vec3>& base, const std::vector<int>* pairs) {
  Item* it = scene_->item(m); if (!it) return;
  std::vector<int> all = verts;                                 // (Soft Selection: those near follow their nearest picked one, weighed)
  if (tset.soft && tsel_.id == m) { const SoftW& S = softOf(m);
    for (size_t i = 0; i < S.vs.size(); i++) { const int v = S.vs[i], s = S.near[i]; P[size_t(v)] = base[size_t(v)] + (P[size_t(s)] - base[size_t(s)]) * S.w[i]; all.push_back(v); } }
  if (tset.snap) {                                              // (each moved vertex onto the surface where it is seen — whatever was dragged)
    // (only one that lay on it before the move: one off it — inside the mouth, in its slit, under a fold, apart from the
    // reference — keeps its place over the surface, moved with the rest: never laid onto it, the lips never sewn together)
    Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse(); const PolyMesh& Bm = *baseOf(*it);
    const double on = Bm.bounds().size().length() / std::sqrt(double(std::max(1, Bm.faceCount()))) * 0.15;   // (on the surface: within a sixth of an edge or so)
    for (int v : verts) { bool onIt = true;   // (one not on the surface before the move: moved as it is, not snapped)
      if (verts.size() > 2 && size_t(v) < base.size()) { const Vec3 b = M.apply(base[size_t(v)]); onIt = ttSnapMoved(b, m).distanceTo(b) <= on; }   // (one or two dragged: onto it, wherever they were)
      if (onIt) P[size_t(v)] = Mi.apply(ttSnapMoved(M.apply(P[size_t(v)]), m)); }
  }
  if (verts.size() <= 64 && ghostAny() && !it->raw.value("tghost").toBool()) {   // (Topology Ghost: a moved vertex near a ghost vertex sits on it)
    const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse(); Projector Pr = projector();
    for (int v : verts) { const ScreenPt s = Pr.toScreen(M.apply(P[size_t(v)])); Vec3 g; if (s.z < 1 && ghostVertAt(QPointF(s.x, s.y), ttools.penWeld, g)) P[size_t(v)] = Mi.apply(g); }
  }
  if (pairs && tset.sym) symApply(symSet(m, base), P, all, base);   // (Symmetry: every mirror / turn of its settings — the twins follow; a point on the plane kept on it)
}

// a BVH of a mesh as shown (world): its masked polygons only (masked — the snap mask on), cached while it stays
std::shared_ptr<const topo::Bvh> Viewport::bvhOf(int m, bool masked) const {
  const Item* it = scene_->item(m); if (!it || !it->poly) return nullptr;
  // (kept per mesh and per kind — the masked one and the whole one apart, not made again in turn; the mask known by its
  // version, decoded only when it is made)
  const bool useMask = masked && !it->raw.value("smOff").toBool() && it->raw.contains("smask");
  const PickCache* pc = pickCacheOf(m); if (!pc) return nullptr;
  QString tag = (useMask ? "m" + QString::number(it->raw.value("smRev").toDouble(), 'f', 0) + ":" + QString::number(it->raw.value("smask").toString().size()) : QString("-")) + "|" + QString::fromLatin1(pc->hid) + "|" + QString::number(geomStamp_.count(m) ? geomStamp_.at(m) : 0);
  BvhC& c = bvhCache_[m * 2 + (useMask ? 1 : 0)];
  if (c.b && c.poly.lock() == it->poly && c.tag == tag) return c.b;
  const auto a = useMask ? smArr(m) : std::vector<char>{};
  std::vector<uint32_t> tris;
  if (!a.empty()) { for (size_t t = 0; t < pc->tri2f.size(); t++) if (size_t(pc->tri2f[t]) < a.size() && a[size_t(pc->tri2f[t])]) for (int j = 0; j < 3; j++) tris.push_back(pc->tris[t * 3 + size_t(j)]); }
  else tris = pc->tris;
  auto b = std::make_shared<topo::Bvh>(); b->build(it->poly->pos, tris);
  c = {it->poly, tag, b};
  return b;
}
// a drag: the positions shown (a copy of the base — the undo step keeps the one before)
void Viewport::topoLive(int m, const std::vector<Vec3>& pos, const std::vector<std::array<float, 2>>* uv) {
  Item* it = scene_->item(m); if (!it || !it->base) return;
  // (the Transform's pivot pinned first: its centre was the box's — the moving points would move it)
  for (int x : meshmods::modsOf(*scene_, m)) { Item* xi = scene_->item(x); if (xi->raw.value("kind").toString() != "xform") continue;
    QJsonObject p = xi->raw.value("p").toObject(); if (p.value("cx").isDouble()) break;
    QJsonArray pv = it->raw.value("pivot").toArray(); Vec3 c = pv.size() == 3 ? Vec3(pv[0].toDouble(), pv[1].toDouble(), pv[2].toDouble()) : it->base->bounds().center();
    p.insert("cx", c.x); p.insert("cy", c.y); p.insert("cz", c.z); xi->raw.insert("p", p); break; }
  liveApply(m, &pos, uv);
}

// a drag's change shown: the base edited in place (copied once — the undo step keeps the one before); the shown copy
// (poly) changed only where vertices / corners changed and the renderer told which, so its GPU copies are patched
// there instead of made again (only the Transform among the mesh's modifiers, no weights shown; a UV change — nothing
// hidden: the shown copy's corners are the base's then). Else the shown copy made anew, as any change
QString Viewport::testDrag(int steps, double dist) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "no mesh";
  auto verts = tSelVerts(m); if (verts.empty()) return "nothing selected";
  const std::vector<Vec3> base = baseOf(*it)->pos; const auto uv0 = baseOf(*it)->cornerUV;
  QElapsedTimer all; all.start(); qint64 tMove = 0, tFrame = 0;
  for (int i = 1; i <= steps; i++) {
    QElapsedTimer t; t.start();
    std::vector<Vec3> P = base; for (int v : verts) P[size_t(v)].x += dist * i;
    topoAfterMove(m, P, verts, base, nullptr);
    if (tset.keepUv && !uv0.empty()) { auto U = uvKeep(m, base, uv0, P); topoLive(m, P, &U); } else topoLive(m, P);
    tMove += t.nsecsElapsed(); t.restart();
    const int f0 = framesDrawn; requestUpdate(); QElapsedTimer to; to.start();
    while (framesDrawn == f0 && to.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    tFrame += t.nsecsElapsed();
  }
  QElapsedTimer t; t.start(); topoWrite(m, baseSp(*it)); commit(false); const double tEnd = t.nsecsElapsed() / 1e6;
  return QString("%1 ms a step (the move %2 ms, the frame %3 ms), release %4 ms").arg(all.elapsed() / double(steps), 0, 'f', 1)
      .arg(tMove / 1e6 / steps, 0, 'f', 1).arg(tFrame / 1e6 / steps, 0, 'f', 1).arg(tEnd, 0, 'f', 1);
}

void Viewport::liveApply(int m, const std::vector<Vec3>* pos, const std::vector<std::array<float, 2>>* uv) {
  Item* it = scene_->item(m); if (!it || !it->base) return;
  geomStamp_[m]++;
  const std::shared_ptr<PolyMesh> oldBase = it->base, oldPoly = it->poly;
  bool fresh = true; for (const Snapshot& s : history_) { auto f = s.items.find(m); if (f != s.items.end() && f->second.base == it->base) { fresh = false; break; } }
  static const bool noPatch = qEnvironmentVariableIsSet("HM_NO_PATCH");   // (checks: the whole mesh made again, to compare)
  bool simple = !noPatch && it->poly && it->poly->pos.size() == it->base->pos.size() && it->poly->col.empty() && meshSig_.count(m);
  for (int x : meshmods::modsOf(*scene_, m)) { const Item* xi = scene_->item(x); if (xi->visible && xi->raw.value("kind").toString() != "xform") simple = false; }
  if (uv && !meshmods::hiddenFaces(*it, *it->base).empty()) simple = false;
  if (!fresh) {
    auto nb = std::make_shared<PolyMesh>(*it->base); topo::shareTopo(nb, it->base); it->base = nb; if (uvc_.base.lock() == oldBase) uvc_.base = nb;
    if (simple) {                                               // (the shown copy goes along: the renderer keeps its GPU copies)
      it->poly = oldPoly.get() == oldBase.get() ? nb : std::make_shared<PolyMesh>(*oldPoly);
      auto f = meshSig_.find(m); const QByteArray o = QByteArray::number(quintptr(oldBase.get()));
      if (f != meshSig_.end() && f->second.startsWith(o)) f->second = QByteArray::number(quintptr(nb.get())) + f->second.mid(o.size()); else simple = false;
    }
  }
  std::vector<int> moved, corners;
  if (pos && pos->size() == it->base->pos.size()) { const auto& O = it->base->pos; for (size_t v = 0; v < O.size(); v++) if (O[v].x != (*pos)[v].x || O[v].y != (*pos)[v].y || O[v].z != (*pos)[v].z) moved.push_back(int(v)); it->base->pos = *pos; }
  if (uv && uv->size() == it->base->faceVerts.size()) { const auto& O = it->base->cornerUV;
    if (O.size() == uv->size()) { for (size_t k = 0; k < O.size(); k++) if (O[k] != (*uv)[k]) corners.push_back(int(k)); } else { simple = false; }
    it->base->cornerUV = *uv; uvRev_++; }
  if (pos) heavyStale(m);
  if (simple) {
    PolyMesh& S = *it->poly;
    if (&S != it->base.get()) { Affine M = meshmods::meshMatrix(*scene_, *it); for (int v : moved) S.pos[size_t(v)] = M.apply(it->base->pos[size_t(v)]);
      if (S.cornerUV.size() == it->base->cornerUV.size()) for (int k : corners) S.cornerUV[size_t(k)] = it->base->cornerUV[size_t(k)]; else if (!corners.empty()) simple = false; }
    if (simple) {
      if (auto pc = pickCache_.find(m); pc != pickCache_.end()) for (int v : moved) pc->second.box.expand(S.pos[size_t(v)]);
      liveMoves_.push_back({m, it->poly, oldPoly.get() != it->poly.get() ? oldPoly.get() : nullptr, std::move(moved), std::move(corners)});
      topoRev_++; requestUpdate(); return;
    }
  }
  meshSig_.erase(m); staticRev_++; topoRev_++; requestUpdate();
}

// ---------- the gizmo on the selected elements ----------
void Viewport::topoGizmoApply(const Affine& M) {
  Item* it = scene_->item(tgz_.m); if (!it) return;
  Affine Mi = meshmods::meshMatrix(*scene_, *it).inverse(); std::vector<Vec3> P = tgz_.base;
  const Vec3 d = M.apply(tgz_.p0) - tgz_.p0;                     // (the drag's move, world)
  if (!tgz_.nbs.empty() && gz_.mode == Gizmo::Mode::Move) {     // (Slide: each vertex along its edge that goes the drag's way, all by one distance)
    const double L = d.length(); const Vec3 dn = L > 0 ? d * (1 / L) : Vec3();
    std::vector<Vec3> W(tgz_.base.size()); { const Affine Mw = Mi.inverse(); for (size_t v = 0; v < W.size(); v++) W[v] = Mw.apply(tgz_.base[v]); }
    for (size_t i = 0; i < tgz_.verts.size(); i++) { const Vec3& p = tgz_.w0[i]; Vec3 bu; double bc = 0.2, len = 0;
      for (int w : tgz_.nbs[i]) { Vec3 u = W[size_t(w)] - p; const double l = u.length(); if (l <= 0) continue; u = u * (1 / l); const double c = u.dot(dn); if (c > bc) { bc = c; bu = u; len = l; } }
      P[size_t(tgz_.verts[i])] = Mi.apply(bc > 0.2 ? p + bu * std::min(L, len * 0.98) : p); }
  } else if (!tgz_.comps.empty()) {                              // (Components: each about its own centre, on its own axes; a shared vertex — their mean)
    const Quat qg = tgz_.q0; const Vec3 dl = qg.conj().rotate(d);
    std::vector<Vec3> sum(tgz_.verts.size()); std::vector<int> cnt(tgz_.verts.size(), 0);
    for (const auto& c : tgz_.comps) for (int i : c.idx) {
      const Vec3 y = qg.rotate(c.q.conj().rotate(tgz_.w0[size_t(i)] - c.c)); const Vec3 y2 = c.q.rotate(qg.conj().rotate(M.lin(y)));
      sum[size_t(i)] += c.c + y2 + c.q.rotate(dl); cnt[size_t(i)]++; }
    for (size_t i = 0; i < tgz_.verts.size(); i++) P[size_t(tgz_.verts[i])] = Mi.apply(cnt[i] ? sum[i] * (1.0 / cnt[i]) : M.apply(tgz_.w0[i]));
  } else
  for (size_t i = 0; i < tgz_.verts.size(); i++) P[size_t(tgz_.verts[i])] = Mi.apply(M.apply(tgz_.w0[i]));
  topoAfterMove(tgz_.m, P, tgz_.verts, tgz_.base, tset.sym ? &tgz_.pairs : nullptr);
  if (tset.keepUv && !tgz_.uv0.empty()) { auto U = uvKeep(tgz_.m, tgz_.base, tgz_.uv0, P); topoLive(tgz_.m, P, &U); } else topoLive(tgz_.m, P);
}

// ---------- what is drawn ----------
bool Viewport::topoTwoTone(int id) const { return room() == "topo" && topoMode != "object" && !tset.keepUv && id == topoMesh() && !(uvo.c3d && uv::has(*baseOf(*scene_->item(id)))); }
double Viewport::topoOpacity(int id) const {   // (the selected meshes' opacity — and each mesh's own, its Display: Transparency)
  if (room() != "topo") return 1; const Item* it = scene_->item(id); const double own = it ? std::clamp(it->raw.value("meshOp").toDouble(1.0), 0.1, 1.0) : 1.0;
  return std::min(own, sel_.hasItem(id) ? tdisp.op : 1.0); }
double Viewport::topoPdo(int id) const { return room() == "topo" && tdisp.pdoOn && sel_.hasItem(id) ? tdisp.pdo * 0.05 : 0; }
bool Viewport::topoWire(const Item& it) const {   // (the mesh's wire: off until switched on — or W: every mesh that isn't a reference)
  return (it.raw.value("twire").toBool(false) || (showWire && room() == "topo" && !it.raw.value("tref").toBool())) && !topoTwoTone(it.id); }

const Viewport::TopoDraw& Viewport::topoDraw() {
  QByteArray sig = QByteArray::number(topoRev_) + "|" + QByteArray::number(staticRev_) + "|" + QByteArray::number(scene_->revision) + "|" + QByteArray::number(hairInspKey()) + "|" + room().toLatin1() + topoMode.toLatin1() + QByteArray::number(tool_ == "tmask") + "|" + QByteArray::number(int(tset.uv)) + QByteArray::number(uvStamp_) + "|" + QByteArray::number(tt_.rev) + tool_.toLatin1() + "|" + QByteArray::number(topoMesh()) + (tool_ == "tnormals" ? QByteArray("|c") + QByteArray::number(std::lround(cam_.pos.x * 20)) + "," + QByteArray::number(std::lround(cam_.pos.y * 20)) + "," + QByteArray::number(std::lround(cam_.pos.z * 20)) + "," + QByteArray::number(std::lround(cam_.dir().x * 200)) + QByteArray::number(std::lround(cam_.dir().y * 200)) : QByteArray());   // (Normals: only the ones facing the camera — made anew as it turns)   // (another mesh made the active one: its tools drawn anew)
  if (sig == topoSig_) return topoDraw_;
  topoSig_ = sig; topoDraw_ = {};
  ProfScope prof_("topoDraw");
  TopoDraw& D = topoDraw_;
  auto faceTris = [&](const TopoDraw::Tris& proto, const std::vector<Vec3>& P, const topo::TTopo& T, const std::vector<int>& faces) {
    TopoDraw::Tris t = proto;   // (a list kept from before an edit may hold polygons / vertices no longer there: those left out)
    auto at = [&](int k) -> const Vec3* { const int v = T.fv[size_t(k)]; return v >= 0 && size_t(v) < P.size() ? &P[size_t(v)] : nullptr; };
    for (int f : faces) { if (f < 0 || f >= T.nf) continue; const int s = T.fo[size_t(f)];
      for (int k = s + 1; k + 1 < T.fo[size_t(f) + 1]; k++) { const Vec3 *a = at(s), *b = at(k), *c = at(k + 1); if (!a || !b || !c) continue; t.p.push_back(*a); t.p.push_back(*b); t.p.push_back(*c); } }
    if (!t.p.empty()) D.tris.push_back(std::move(t));
  };
  auto edgeLines = [&](const QColor& c, float w, const std::vector<Vec3>& P, const topo::TTopo& T, const std::vector<int>& es) {
    TopoDraw::Lines l; l.c = c; l.w = w;
    for (int e : es) { if (e < 0 || e >= T.ne) continue; const int a = T.ea[size_t(e)], b = T.eb[size_t(e)]; if (a < 0 || b < 0 || size_t(a) >= P.size() || size_t(b) >= P.size()) continue; l.p.push_back(P[size_t(a)]); l.p.push_back(P[size_t(b)]); }
    if (!l.p.empty()) D.lines.push_back(std::move(l));
  };
  // the polygroups in their colours (a mesh's Polygroups display — both rooms)
  for (int m : topoMeshes()) {
    const Item* gi = scene_->item(m); if (!gi->raw.value("pgView").toBool() || meshmods::heavy(*gi)) continue;
    const PolyMesh* GB = baseOf(*gi); if (int(GB->faceGroup.size()) != GB->faceCount()) continue;
    auto GT = topo::ttopo(baseSp(*gi)); std::map<int, std::vector<int>> by; for (int f = 0; f < GB->faceCount(); f++) by[GB->faceGroup[size_t(f)]].push_back(f);
    for (auto& [g, fs] : by) { TopoDraw::Tris pr; pr.c = tgColor(g); pr.op = 0.55f; faceTris(pr, gi->poly->pos, *GT, fs); }
  }
  // the Inspector's highlights (both rooms): n-gons violet, non-planar teal, interior pieces pink (and a marker), open
  // edges yellow, non-manifold edges red
  if (insp.any()) for (int m : topoMeshes()) {
    const Item* ii = scene_->item(m); if (!ii->poly || meshmods::heavy(*ii)) continue;
    // (no checks on a high-poly mesh, a reference, or anything in a group marked Skip in the Inspector)
    if (ii->raw.value("tref").toBool() || baseOf(*ii)->faceCount() > 200000) continue;
    { bool off = false; for (const Item* g = scene_->item(ii->parent); g && !off; g = scene_->item(g->parent)) off = g->raw.value("inspOff").toBool(); if (off) continue; }
    const InspC& c = inspOf(m); auto IT = topo::ttopo(baseSp(*ii)); const auto& IP = ii->poly->pos;
    if (insp.ngons && !c.ngons.empty()) { TopoDraw::Tris pr; pr.c = QColor(0xb0, 0x5c, 0xff); pr.op = 0.5f; faceTris(pr, IP, *IT, c.ngons); }
    if (insp.doubles && !c.doubles.empty()) { TopoDraw::Tris pr; pr.c = QColor(0xff, 0x40, 0x90); pr.op = 0.55f; faceTris(pr, IP, *IT, c.doubles); }   // (double walls: hot pink)
    if (insp.nonPlanar && !c.nonPlanar.empty()) { TopoDraw::Tris pr; pr.c = QColor(0x2e, 0xd0, 0xd0); pr.op = 0.45f; faceTris(pr, IP, *IT, c.nonPlanar); }
    if (insp.interior && !c.interior.empty()) { TopoDraw::Tris pr; pr.c = QColor(0xff, 0x3b, 0xa8); pr.op = 0.55f; faceTris(pr, IP, *IT, c.interior);
      TopoDraw::Pts pt; pt.size = 16; pt.xray = true; for (const Vec3& q : c.interiorPts) { pt.p.push_back(q); pt.c.push_back(QColor(0xff, 0x3b, 0xa8)); } D.pts.push_back(std::move(pt)); }
    if (insp.open) edgeLines(QColor(0xff, 0xd2, 0x3a), 3.f, IP, *IT, c.open);
    if (insp.nonManifold) edgeLines(QColor(0xff, 0x3a, 0x3a), 4.f, IP, *IT, c.nonMan);
    if (insp.hard) edgeLines(QColor(0x3a, 0xc8, 0xff), 3.f, IP, *IT, c.hard);
  }
  hairInspectDraw(D);                                          // (the Hair room: the Maranspector's own)
  if (room() == "hair" && tset.sym && symo.planes) hairSymPlanes(D);   // (the Hair room's Symmetry: its planes / its axis)
  if (room() != "topo") return D;
  const int am = topoMesh();
  ghostDraw(D);                                                // (Topology Ghost: the source's polygons on the targets)
  lobeGhostDraw(D);                                            // (the Lobe Brush: the retopology of one lobe shown on the others)
  if (!revealFaces_.empty()) { const auto& Gs = ghosts(); TopoDraw::Tris rt; rt.c = QColor(0x5a, 0x9c, 0xff); rt.op = 0.45f;   // (the Reveal Brush's stroke: what it will make)
    for (auto [gi, f] : revealFaces_) { if (size_t(gi) >= Gs.size()) continue; const Ghost& G = Gs[size_t(gi)]; const int s = G.fo[size_t(f)], e = G.fo[size_t(f) + 1];
      for (int k = s + 1; k + 1 < e; k++) { rt.p.push_back(G.P[size_t(G.fv[size_t(s)])]); rt.p.push_back(G.P[size_t(G.fv[size_t(k)])]); rt.p.push_back(G.P[size_t(G.fv[size_t(k + 1)])]); } }
    if (tset.sym) {                                             // (Symmetry: their mirror shown too — across the built mesh's X = 0, none: the world's)
      Affine M, Mi; const int bm = topoMesh(); if (const Item* bi = bm >= 0 ? scene_->item(bm) : nullptr; bi && !bi->raw.value("tghost").toBool() && !topoHeavy(bm)) { M = meshmods::meshMatrix(*scene_, *bi); Mi = M.inverse(); }
      const size_t n0 = rt.p.size(); for (size_t i = 0; i < n0; i += 3) for (int j : {0, 2, 1}) { Vec3 l = Mi.apply(rt.p[i + size_t(j)]); l.x = -l.x; rt.p.push_back(M.apply(l)); } }
    D.tris.push_back(std::move(rt)); }
  // the object's UV stretch (in every mode): each polygon by its UV area to its 3D area against the mesh's — red squeezed,
  // blue stretched, grey as the rest
  if (uvo.s3d) for (int m : topoMeshes()) {
    if (!uvDispShown(m)) continue; const Item* si = scene_->item(m); const PolyMesh& SB = *baseOf(*si); if (!uv::has(SB) || meshmods::heavy(*si)) continue;
    auto ST = topo::ttopo(baseSp(*si)); double au = 0, a3 = 0; std::vector<double> r(size_t(SB.faceCount()), 0);
    for (int f = 0; f < SB.faceCount(); f++) { const double x = std::abs(uv::faceArea(SB, f)), y = uv::faceArea3(SB, f); au += x; a3 += y; r[size_t(f)] = y > 0 ? x / y : 0; }
    const double k = au > 0 && a3 > 0 ? a3 / au : 1; std::map<int, std::vector<int>> bins;   // (−4 … 4: squeezed … stretched)
    for (int f = 0; f < SB.faceCount(); f++) { const double q = r[size_t(f)] * k; const int b = q <= 0 ? -4 : std::clamp(int(std::lround(std::log2(q) * 3)), -4, 4); bins[b].push_back(f); }
    for (auto& [b, fs] : bins) { const double w = std::abs(b) / 4.0; TopoDraw::Tris pr; pr.op = float(0.85 * uvo.dispOp);
      pr.c = stretchColor(std::pow(2.0, b / 3.0)); (void)w;
      faceTris(pr, si->poly->pos, *ST, fs); }
  }
  // the snap masks: green on every mesh with one (the brush's mesh; with Show — the selected ones, or all while Snap is on)
  // (the references too; bright green, nearly opaque — over a polypaint as well)
  for (auto& [m, itr] : scene_->items) {
    const Item* it = &itr; if (it->type != ItemType::IMesh || !it->poly || !scene_->isVisible(*it) || it->raw.value("smask").toString().isEmpty()) continue;
    bool off = it->raw.value("smOff").toBool();
    if (!(tool_ == "tmask" ? m == am : !off && smo.show && (sel_.hasItem(m) || tset.snap))) continue;
    // (made once per mask — kept while the rest of the overlay is made again; a stroke being painted: its own array)
    const bool painting = drag_ == Drag::Topo && td_.type == 4 && td_.m == m;
    const auto px = painting ? td_.px : maskProxy(m);         // (a high-poly mesh: the green on its proxy's triangles)
    const QByteArray key = QByteArray::number(quintptr(px.get())) + "|" + (px ? pxVis(m, *px).key : QByteArray()) + "|" + QByteArray::number(quintptr(it->poly.get())) + "|" + QByteArray::number(geomStamp_.count(m) ? geomStamp_.at(m) : 0) + "|" + QByteArray::number(int(off))
                           + (painting ? "|p" + QByteArray::number(maskPaintRev_) : "|" + QByteArray::number(it->raw.value("smRev").toDouble(), 'f', 0));
    auto& C = maskDrawCache_[m];
    if (C.first != key && px) {
      const std::vector<char>& pm = painting ? td_.parr : pxMaskOf(m, *px); const Affine M = meshmods::meshMatrix(*scene_, *it);
      TopoDraw::Tris pr; pr.c = off ? QColor(0xe0, 0xe0, 0xe0) : QColor(0x2e, 0xff, 0x5a); pr.op = off ? 0.33f : 0.72f; pr.lift = 0.004f;
      const auto& P = px->mesh->pos; const auto& I = px->bvh->idx;
      const PxVis& V = pxVis(m, *px); for (size_t t = 0; t < pm.size() && t * 3 + 2 < I.size(); t++) if (pm[t] && (V.hid.empty() || !V.hid[t])) for (int j = 0; j < 3; j++) pr.p.push_back(M.apply(P[I[t * 3 + size_t(j)]]));
      C = {key, std::move(pr)};
    }
    if (C.first != key) {
      const std::vector<char> a = painting ? td_.arr : smArr(m); std::vector<int> fs; for (size_t f = 0; f < a.size(); f++) if (a[f]) fs.push_back(int(f));
      TopoDraw::Tris pr; pr.c = off ? QColor(0xe0, 0xe0, 0xe0) : QColor(0x2e, 0xff, 0x5a); pr.op = off ? 0.33f : 0.72f;
      const size_t n0 = D.tris.size(); { const PolyMesh& MB = *baseSp(*it); const auto& MP = it->poly->pos; TopoDraw::Tris t = pr;   // (the polygons straight from the mesh — no topology made for it)
        if (MP.size() == MB.pos.size()) for (int f : fs) { const int s = MB.faceStart[size_t(f)], e = MB.faceStart[size_t(f) + 1]; for (int k = s + 1; k + 1 < e; k++) { t.p.push_back(MP[size_t(MB.faceVerts[size_t(s)])]); t.p.push_back(MP[size_t(MB.faceVerts[size_t(k)])]); t.p.push_back(MP[size_t(MB.faceVerts[size_t(k + 1)])]); } }
        if (!t.p.empty()) D.tris.push_back(std::move(t)); }
      C = {key, D.tris.size() > n0 ? D.tris.back() : TopoDraw::Tris{}}; if (D.tris.size() > n0) D.tris.pop_back();
    }
    if (!C.second.p.empty()) D.tris.push_back(C.second);
  }
  topoToolDraw(D);                                             // (the modelling tools' previews)
  // Fill / Outline: what a click would pick (orange) — on any mesh under the pointer
  // (Loop / Ring: yellow — the click confirms; Fill with Ctrl: red — the piece taken off)
  if (tool_ == "cursor" && (tset.selTool == "fill" || tset.selTool == "outline" || tset.selTool == "loop" || tset.selTool == "ring" || tset.selTool == "hard") && thov_.m >= 0)
    if (const Item* hi = scene_->item(thov_.m); hi && hi->poly) {
      auto HT = topo::ttopo(baseSp(*hi)); const auto& HP = hi->poly->pos;
      const QColor pc = thov_.inv ? QColor(0xff, 0x55, 0x55) : (tset.selTool == "loop" || tset.selTool == "ring") ? QColor(0xff, 0xd8, 0x3a) : QColor(0xff, 0x9a, 0x2e);
      if (!thov_.region.empty()) { TopoDraw::Tris pr; pr.c = pc; pr.op = 0.34f; faceTris(pr, HP, *HT, thov_.region); }
      if (!thov_.rim.empty()) edgeLines(pc, 3.f, HP, *HT, thov_.rim);
      if (!thov_.pts.empty()) { TopoDraw::Pts pt; pt.size = 8; for (int v : thov_.pts) if (v < HT->n) { pt.p.push_back(HP[size_t(v)]); pt.c.push_back(pc); } D.pts.push_back(std::move(pt)); }
    }

  if (am < 0) return D;
  const Item* it = scene_->item(am); auto B = baseSp(*it); std::shared_ptr<const topo::TTopo> Tc; auto Tg = [&]() -> const topo::TTopo& { if (!Tc) Tc = topo::ttopo(B); return *Tc; }; const auto& P = it->poly->pos;   // (its topology made only when something needs it — not for a high-poly mesh in the Object mode)
  if (tsel_.id == am && tsel_.B != B.get()) {                 // (the geometry changed under the selection: the indices mean something else — dropped)
    if (tsel_.B && (tsel_.B->pos.size() != B->pos.size() || tsel_.B->faceCount() != B->faceCount())) { tsel_.f.clear(); tsel_.e.clear(); tsel_.v.clear(); tsel_.last = -1; tsel_.ver++; }
    tsel_.B = B.get();
  }
  const bool heavy = meshmods::heavy(*it) || meshmods::wireless(*it);   // (too many to draw every edge / vertex: the selection and the hover only)
  // Object mode: the picked polygroups' polygons (a high-poly mesh: what its actions selected) lit
  // (a high-poly mesh with its LOD ready: the polygroups lit on the LOD's triangles — not on the millions of its own)
  std::shared_ptr<const PolyMesh> lod; if (auto px = maskProxy(am)) lod = px->mesh; else if (heavy) lod = lodOf(am);   // (the mask's proxy first)
  const Affine LM = lod ? meshmods::meshMatrix(*scene_, *it) : Affine{};
  const std::set<int> hidG = hidOf(*it);                        // (a hidden polygroup: never lit)
  auto lodTris = [&](const TopoDraw::Tris& proto, const std::set<int>& gs0) {
    std::set<int> gs; for (int g : gs0) if (!hidG.count(g)) gs.insert(g); if (gs.empty()) return;
    TopoDraw::Tris t = proto; t.lift = 0.004f; const auto& I = lod->faceVerts;
    for (size_t k = 0; k * 3 + 2 < I.size() && k < lod->faceGroup.size(); k++) if (gs.count(lod->faceGroup[k])) for (int j = 0; j < 3; j++) t.p.push_back(LM.apply(lod->pos[size_t(I[k * 3 + j])]));
    if (!t.p.empty()) D.tris.push_back(std::move(t));
  };
  if (topoMode == "object" && tsel_.id == am && !tsel_.f.empty() && (heavy || (tgPick.id == am && !tgPick.set.empty()))) { TopoDraw::Tris pr; pr.c = Qt::white; pr.op = 0.28f;
    if (lod && tgPick.id == am && !tgPick.set.empty()) lodTris(pr, tgPick.set); else faceTris(pr, P, Tg(), {tsel_.f.begin(), tsel_.f.end()}); }
  // the polygroup picked in the list (its colour) and — fainter — the same place on the topologically identical parts
  if (lod && tgSel.id == am && tgSel.gi >= 0 && !hidG.count(tgSel.gi)) { TopoDraw::Tris pr; pr.c = tgColor(tgSel.gi); pr.op = 0.42f; lodTris(pr, {tgSel.gi}); }
  else if (tgSel.id == am && tgSel.gi >= 0 && !hidG.count(tgSel.gi) && size_t(tgSel.gi) < B->groupNames.size()) {
    auto own = topo::groupFaces(*B, tgSel.gi); std::set<int> mine(own.begin(), own.end());
    TopoDraw::Tris pr; pr.c = tgColor(tgSel.gi); pr.op = 0.42f; faceTris(pr, P, Tg(), own);
    std::vector<int> sim; for (int f : topo::similarFaces(B, tgSel.gi)) if (!mine.count(f)) sim.push_back(f);
    pr.op = 0.2f; faceTris(pr, P, Tg(), sim);
  }
  if (tgHover_.id == am && tgHover_.gi >= 0 && !hidG.count(tgHover_.gi)) { TopoDraw::Tris pr; pr.c = tgColor(tgHover_.gi); pr.op = 0.35f; if (lod) lodTris(pr, {tgHover_.gi}); else faceTris(pr, P, Tg(), topo::groupFaces(*B, tgHover_.gi)); }
  // the UV mode: the seams (red) and where the UV breaks (the shells' borders, teal) on the active mesh
  if (uvMode() && !heavy) {
    auto sf = uv::seamFlags(*B, Tg()); std::vector<int> se, br;
    for (int e = 0; e < Tg().ne; e++) if (sf[size_t(e)]) se.push_back(e);
    if (uv::has(*B)) { auto pts = uv::points(*B, Tg()); for (int e : uv::borderEdges(*B, Tg(), pts)) if (!sf[size_t(e)]) br.push_back(e); }
    edgeLines(QColor(0x5f, 0xd3, 0xc4), 2.f, P, Tg(), br); edgeLines(QColor(0xff, 0x5a, 0x4f), 3.f, P, Tg(), se);
  }
  if (tset.sym && symo.planes && am >= 0) topoSymPlanes(D, am);   // (Symmetry: its planes / its axis over the active mesh)
  if (uvMode() && !heavy && tset.sym && uvo.symTint) { const auto mx = uvMinusXFaces(); std::vector<int> fs; for (int f = 0; f < B->faceCount() && size_t(f) < mx.size(); f++) if (mx[size_t(f)]) fs.push_back(f);   // (Symmetry, the UV mode: the −X half burgundy, as in the UV Editor)
    if (!fs.empty()) { TopoDraw::Tris pr; pr.c = QColor(0x9c, 0x1a, 0x32); pr.op = 0.5f; faceTris(pr, P, Tg(), fs); } }
  if (uvMode() && !heavy && uvPicked.id == am && uvPicked.mat == -2 && uvPicked.set >= 0 && topo::hasUvSets(*B)) {   // (the UV Set worked in: what is not in it darkened on the model)
    TopoDraw::Tris pr; pr.c = QColor(0x08, 0x08, 0x0c); pr.op = 0.5f; std::vector<int> fs; for (int f = 0; f < B->faceCount(); f++) if (!(size_t(f) < B->faceSets.size() && (B->faceSets[size_t(f)] >> uvPicked.set & 1u))) fs.push_back(f);
    faceTris(pr, P, Tg(), fs); }
  if (topoMode == "object") return D;
  if (tsel_.id != am) { int ver = tsel_.ver + 1; tsel_ = {}; tsel_.id = am; tsel_.ver = ver; tsel_.B = B.get(); }
  const std::vector<char> hidF = meshmods::hiddenFaces(*it, *B); auto shown = [&](int f) { return hidF.empty() || !hidF[size_t(f)]; };
  if (!heavy) { D.edgesOf = am; D.edgesC = topoMode == "edge" ? TEDGE_E : TEDGE; D.edgesW = topoMode == "edge" ? 2.f : 1.f; }   // (its own wire: the renderer's)
  // (a huge selection — a high-poly mesh selected whole: every n-th of it drawn, a couple of hundred thousand at most)
  auto capped = [heavy](const std::set<int>& s) { const size_t lim = heavy ? 200000 : SIZE_MAX, k = s.size() > lim ? (s.size() + lim - 1) / lim : 1; std::vector<int> o; o.reserve(std::min(s.size(), lim) + 1); size_t i = 0; for (int x : s) if (i++ % k == 0) o.push_back(x); return o; };
  if (topoMode == "edge") edgeLines(Qt::white, 2.5f, P, Tg(), capped(tsel_.e));
  if (topoMode == "poly") { TopoDraw::Tris pr; pr.c = Qt::white; pr.op = 0.3f; faceTris(pr, P, Tg(), capped(tsel_.f)); }
  if (topoMode == "vert") {
    TopoDraw::Pts pt; pt.size = 5;
    if (!heavy) D.pointsOf = am;                                 // (all of them: kept by the renderer, patched as they move)
    else for (int v : capped(tsel_.v)) { pt.p.push_back(P[size_t(v)]); pt.c.push_back(Qt::white); }
    if (!pt.p.empty()) D.pts.push_back(std::move(pt));
  }
  if (ssHover.m == am && ssHover.idx >= 0) {                  // (a selection set pointed at in the panel: cyan)
    char k = 0; auto L = tSelSetList(am, ssHover.idx, &k); const QColor cy(0x4c, 0xd6, 0xf0);
    if (k == 'f') { TopoDraw::Tris pr; pr.c = cy; pr.op = 0.4f; faceTris(pr, P, Tg(), L); }
    else if (k == 'e') edgeLines(cy, 3.f, P, Tg(), L);
    else { TopoDraw::Pts pt; pt.size = 8; for (int v : L) if (v < Tg().n) { pt.p.push_back(P[size_t(v)]); pt.c.push_back(cy); } D.pts.push_back(std::move(pt)); }
  }
  if (tset.soft && tsel_.id == am) {                           // (Soft Selection: how much each vertex near the selection follows — yellow to red)
    const SoftW& S = softOf(am); TopoDraw::Pts pt; pt.size = 6;
    const size_t stp = heavy && S.vs.size() > 200000 ? (S.vs.size() + 199999) / 200000 : 1;   // (a high-poly mesh: every n-th shown)
    for (size_t i = 0; i < S.vs.size(); i += stp) { const double w = S.w[i]; pt.p.push_back(P[size_t(S.vs[i])]); pt.c.push_back(QColor::fromRgbF(1.f, float(0.85 - 0.7 * w), float(0.2 * (1 - w)))); }
    if (!pt.p.empty()) D.pts.push_back(std::move(pt));
  }
  if (thov_.m == am && drag_ != Drag::Topo && !(tool_ == "cursor" && (tset.selTool == "fill" || tset.selTool == "outline" || tset.selTool == "loop" || tset.selTool == "ring" || tset.selTool == "hard"))) {                // the element under the cursor
    if (thov_.f >= 0 && thov_.f < Tg().nf) { TopoDraw::Tris pr; pr.c = Qt::white; pr.op = 0.12f; faceTris(pr, P, Tg(), {thov_.f}); }
    if (thov_.e >= 0 && thov_.e < Tg().ne) edgeLines(Qt::white, 2.f, P, Tg(), {thov_.e});
    if (thov_.v >= 0 && thov_.v < Tg().n) { TopoDraw::Pts pt; pt.size = 9; pt.p = {P[size_t(thov_.v)]}; pt.c = {Qt::white}; D.pts.push_back(pt); }
  }
  return D;
}

}  // namespace hm
