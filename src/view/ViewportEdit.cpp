// The Viewport's edits: the Curve tool (drawing), deleting, recalculating, merging, reversing, hiding and isolating —
// each one an undo step, as the HTML version's Tools / Keyboard.
#include "scene/Crown.h"
#include "scene/Edit.h"
#include "scene/MeshMods.h"
#include "scene/Network.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QElapsedTimer>
#include <QSettings>
#include <QJsonArray>
#include <QRegularExpression>
#include <functional>
#include <iterator>
#include <set>

namespace hm {

void Viewport::afterEdit(bool topology) {
  scene_->revision++;
  commit(topology);
}

void Viewport::runEdit(const std::function<QString(Scene&, Selection&, const CurveCache&)>& fn, bool topology) {
  QString msg = fn(*scene_, sel_, hair_.curves);
  if (msg.startsWith('!')) { ui::toast(msg.mid(1)); return; }        // (nothing done)
  if (!msg.isEmpty()) stepLabel_ = msg;                   // (the history's name for the step)
  afterEdit(topology); emit sceneEdited();
  if (!msg.isEmpty()) ui::toast(msg);
}

// ---------- the Curve tool ----------
// ---------- collision (the Hair room): what the drawn curves lie on ----------
// a mesh's collision (raw coll { type, d, mesh }): its own geometry lifted by d (the default, 2 mm), a capsule round it
// (lifted by d too) or another mesh standing for it (a low proxy, hidden or not). The head collides while shown, a mesh
// with ⛨ on even hidden
double Viewport::collOffset(const Item& it) { const QJsonObject c = it.raw.value("coll").toObject(); return std::max(0.0, c.value("d").toDouble(0.2)); }
QString Viewport::collType(const Item& it) { const QString t = it.raw.value("coll").toObject().value("type").toString("offset"); return t == "capsule" || t == "custom" ? t : QString("offset"); }

// a capsule round the mesh: along its longest spread (the points' main axis), round enough to hold every point; kept
// while the mesh is the same
const Viewport::CapMesh& Viewport::capsuleOf(int id) const {
  const Item* it = scene_->item(id); auto& C = capCache_[id]; const PolyMesh* p = it ? it->poly.get() : nullptr;
  // (its settings, shares of the fitted capsule: the radius, the caps' reach along the axis, the flattening across — along its
  // second and third spread, the straight part's height; a shift along the axis)
  const QJsonObject co = it ? it->raw.value("coll").toObject() : QJsonObject();
  auto pc = [&](const char* k) { return std::max(0.0, co.value(k).toDouble(100) / 100); };
  const std::array<double, 6> prm{std::max(0.02, pc("rs")), std::max(0.02, pc("cs")), std::max(0.02, pc("fu")), std::max(0.02, pc("fw")), pc("hs"), co.value("sh").toDouble(0)};
  if (C.src == p && C.n == (p ? p->pos.size() : 0) && (!p || C.box == p->bounds().size().lengthSq()) && C.prm == prm) return C;
  C = {}; C.src = p; C.n = p ? p->pos.size() : 0; C.prm = prm; if (!p || p->pos.empty()) return C; C.box = p->bounds().size().lengthSq();
  // (a head marked with its body: the capsule round the head only — above the neck)
  std::vector<Vec3> pts; { Box3 hb; if (it->headTag) {
      // (the top of the skull: rays down along a line front to back over the middle of the model's top eighth — the third
      // highest they meet, antennae and ear tips left out; the head under it)
      Box3 B; for (auto& q : p->pos) B.expand(q); Vec3 bc; int n = 0; const double y0 = B.max.y - 0.12 * B.size().y; for (auto& q : p->pos) if (q.y >= y0) { bc += q; n++; } bc = n ? bc / double(n) : B.center();
      auto hit = headHitFn(); std::vector<double> tops; const double R = B.size().y * 0.06;
      for (int j = -8; j <= 8; j++) if (auto h = hit(Vec3(bc.x, B.max.y + B.size().y * 0.1, bc.z + j * R / 8), Vec3(0, -1, 0), false)) tops.push_back(h->y);
      std::sort(tops.rbegin(), tops.rend()); const double top = tops.empty() ? B.max.y : tops[std::min<size_t>(2, tops.size() - 1)];
      hb = crown::headRegion(*scene_, top);
    }
    for (auto& q : p->pos) if (hb.empty() || (q.x >= hb.min.x && q.y >= hb.min.y && q.z >= hb.min.z && q.x <= hb.max.x && q.y <= hb.max.y && q.z <= hb.max.z)) pts.push_back(q);
    if (pts.size() < 4) pts = p->pos; }
  Vec3 cen; for (auto& q : pts) cen += q; cen = cen * (1.0 / double(pts.size()));
  Vec3 ax(1, 0.001, 0.002);                                      // (the main spread: the capsule's axis)
  for (int k = 0; k < 24; k++) { Vec3 n; for (auto& q : pts) { const Vec3 d = q - cen; n += d * d.dot(ax); } if (n.lengthSq() < 1e-24) break; ax = n.normalized(); }
  Vec3 u = std::abs(ax.y) < 0.9 ? ax.cross(Vec3(0, 1, 0)).normalized() : ax.cross(Vec3(1, 0, 0)).normalized();
  for (int k = 0; k < 24; k++) { Vec3 n; for (auto& q : pts) { Vec3 d = q - cen; d = d - ax * d.dot(ax); n += d * d.dot(u); } n = n - ax * n.dot(ax); if (n.lengthSq() < 1e-24) break; u = n.normalized(); }   // (the second spread, across)
  const Vec3 w = ax.cross(u);
  double t0 = 1e300, t1 = -1e300, r = 0;
  for (auto& q : pts) { const Vec3 d = q - cen; const double t = d.dot(ax); t0 = std::min(t0, t); t1 = std::max(t1, t); r = std::max(r, (d - ax * t).length()); }
  const double straight = std::max(0.0, (t1 - t0) - 2 * r);      // (the fitted straight part)
  const double R = r * prm[0], capL = R * prm[1], half = straight * 0.5 * prm[4], mid = (t0 + t1) * 0.5 + prm[5];
  const Vec3 e0 = cen + ax * (mid - half), e1 = cen + ax * (mid + half);
  const int S = 32, H = 8;                                       // (round: 32 sides; each cap: 8 rings)
  std::vector<std::pair<Vec3, double>> rings;                    // (each ring: its centre, its share of the radius)
  for (int i = H; i >= 1; i--) { const double ph = M_PI / 2 * double(i) / H; rings.push_back({e0 - ax * (capL * std::sin(ph)), std::cos(ph)}); }
  rings.push_back({e0, 1.0}); rings.push_back({e1, 1.0});
  for (int i = 1; i <= H; i++) { const double ph = M_PI / 2 * double(i) / H; rings.push_back({e1 + ax * (capL * std::sin(ph)), std::cos(ph)}); }
  C.pos.push_back(e0 - ax * capL);                               // (the ends' tips)
  for (auto& [c0, k] : rings) for (int s = 0; s < S; s++) { const double a = 2 * M_PI * s / S; C.pos.push_back(c0 + (u * (std::cos(a) * prm[2]) + w * (std::sin(a) * prm[3])) * (R * k)); }
  C.pos.push_back(e1 + ax * capL);
  const uint32_t tipA = 0, tipB = uint32_t(C.pos.size() - 1), NR = uint32_t(rings.size());
  auto at = [&](uint32_t ring, int s) { return 1 + ring * S + uint32_t((s + S) % S); };
  for (int s = 0; s < S; s++) { C.tris.insert(C.tris.end(), {tipA, at(0, s + 1), at(0, s)}); C.tris.insert(C.tris.end(), {tipB, at(NR - 1, s), at(NR - 1, s + 1)}); }
  for (uint32_t k = 0; k + 1 < NR; k++) for (int s = 0; s < S; s++) { const uint32_t a = at(k, s), b = at(k, s + 1), c = at(k + 1, s + 1), d = at(k + 1, s); C.tris.insert(C.tris.end(), {a, b, c, a, c, d}); }
  for (auto& q : C.pos) C.bx.expand(q);
  return C;
}

int Viewport::capsuleItem(int owner) const {
  for (auto& [id, it] : scene_->items) if (it.raw.value("collCap").toInt(-1) == owner && it.poly) return id;
  return -1;
}
std::vector<Vec3> Viewport::capsuleWorld(int cap) const {
  const Item* it = scene_->item(cap); if (!it || !it->poly) return {};
  return it->poly->pos;                                           // (the shown copy: the gizmo's moves in it already)
}
int Viewport::makeCapsuleItem(int owner, bool refit) {
  const Item* ow = scene_->item(owner); if (!ow) return -1;
  capCache_.erase(owner); const CapMesh& C = capsuleOf(owner); if (C.pos.size() < 3) return -1;
  PolyMesh d; d.pos = C.pos; d.faceStart = {0};
  for (size_t t = 0; t + 2 < C.tris.size(); t += 3) { for (int k = 0; k < 3; k++) d.faceVerts.push_back(int(C.tris[t + size_t(k)])); d.faceStart.push_back(int(d.faceVerts.size())); }
  d.faceGroup.assign(size_t(d.faceCount()), 0); d.groupNames = {"Capsule"};
  int cap = capsuleItem(owner);
  if (cap >= 0) {                                                // (there already: its shape anew; refit — its moves / turns dropped too)
    if (refit) for (int x : meshmods::modsOf(*scene_, cap)) if (scene_->item(x) && scene_->item(x)->raw.value("kind").toString() == "xform") { scene_->removeFromParent(x); scene_->items.erase(x); }
    Item* ci = scene_->item(cap); auto nb = std::make_shared<PolyMesh>(std::move(d)); ci->base = nb; ci->poly = nb;
    if (ci->parent != owner) { scene_->removeFromParent(cap); ci->parent = owner; scene_->item(owner)->children.push_back(cap); }   // (an older one beside the mesh: under it now)
  } else {
    cap = topoNewMesh(owner, ow->name + tr(" · collision capsule"), d, owner); if (cap < 0) return -1;
    scene_->item(cap)->raw.insert("collCap", owner);
  }
  scene_->reindex(); scene_->revision++; staticRev_++; overlayRev_++; requestUpdate();
  return cap;
}

// the collision shapes: the head's (while shown) and those of the meshes with ⛨ on — each as its type says; off: the lift
// off each one (by the mesh the shape stands for)
std::vector<PickMesh> Viewport::collisionMeshes(bool withHead, std::map<int, double>& off) const {
  std::vector<PickMesh> list;
  for (auto& [id, it] : scene_->items) {
    if (!it.poly) continue;
    const bool head = (it.headTag || it.type == ItemType::Head) && scene_->isVisible(it), collide = it.raw.value("collide").toBool();
    if (!(withHead && head) && !collide) continue;
    const QString type = collType(it); off[id] = collOffset(it);
    PickMesh pm; pm.itemId = id;
    if (type == "capsule") {
      if (const int cap = capsuleItem(id); cap >= 0) {               // (placed by hand: where the gizmo put it)
        auto& w = capWorld_[cap]; w = capsuleWorld(cap); const PolyMesh& cp = *scene_->item(cap)->poly; pm.pos = &w;
        for (int f = 0; f < cp.faceCount(); f++) { int s0 = cp.faceStart[f], e0 = cp.faceStart[f + 1]; for (int k = s0 + 1; k < e0 - 1; k++) { pm.tris.push_back(uint32_t(cp.faceVerts[s0])); pm.tris.push_back(uint32_t(cp.faceVerts[k])); pm.tris.push_back(uint32_t(cp.faceVerts[k + 1])); } }
        for (auto& q : w) pm.box.expand(q); list.push_back(std::move(pm)); continue; }
      const CapMesh& C = capsuleOf(id); if (C.pos.empty()) continue; pm.pos = &C.pos; pm.trisRef = &C.tris; pm.box = C.bx; list.push_back(std::move(pm)); continue; }
    const Item* src = &it;
    if (type == "custom") { const Item* o = scene_->item(it.raw.value("coll").toObject().value("mesh").toInt(-1)); if (o && o->poly && o != &it) src = o; }
    const PolyMesh& p = *src->poly; pm.pos = &p.pos; pm.box = p.bounds();
    for (int f = 0; f < p.faceCount(); f++) { int s = p.faceStart[f], e = p.faceStart[f + 1]; for (int k = s + 1; k < e - 1; k++) { pm.tris.push_back(uint32_t(p.faceVerts[s])); pm.tris.push_back(uint32_t(p.faceVerts[k])); pm.tris.push_back(uint32_t(p.faceVerts[k + 1])); } }
    list.push_back(std::move(pm));
  }
  return list;
}

// a point on the head (or on an imported mesh with Collision on — even hidden), lifted off its collision shape toward
// the side the ray came from; else on the plane facing the camera through the last point (the pivot)
std::optional<Vec3> Viewport::placePoint(const QPointF& m) const {
  std::map<int, double> off; const auto list = collisionMeshes(true, off);
  Ray r = ray(m);
  if (auto h = raycast(r, list)) { Vec3 n = h->normal; if (n.dot(r.d) > 0) n = -n; return h->point + n * (off.count(h->itemId) ? off[h->itemId] : 0.2); }
  const Item* c = drawing_ >= 0 ? scene_->item(drawing_) : nullptr;
  Vec3 ref = c && !c->anchors.empty() ? scene_->anchor(c->anchors.back())->pos : cam_.pivot;
  return planeHit(r, ref, cam_.dir());
}

void Viewport::addDrawPoint(const QPointF& m) {
  auto p = placePoint(m); if (!p) return;
  Item* c = drawing_ >= 0 ? scene_->item(drawing_) : nullptr;
  // anchors of other curves selected before (Lock mode): 2 — a flat lock between them, 3 or more — a volume branch
  std::vector<int> selA; for (int a : sel_.anchors) if (std::find(selA.begin(), selA.end(), a) == selA.end()) selA.push_back(a);
  const bool lock = ribDrawOn(); const std::vector<int> pair = lock && !c && selA.size() >= 2 ? selA : std::vector<int>{};
  if (!c) {
    int parent = edit::targetGroup(*scene_, sel_);
    if (!pair.empty()) for (int h : edit::curvesWith(*scene_, pair[0])) if (scene_->item(h)->ring < 0) { parent = scene_->item(h)->parent; break; }
    Item& n = edit::makeUserCurve(*scene_, parent);
    drawing_ = n.id; c = &n;
    sel_.items = {n.id}; sel_.anchors.clear();
    emit sceneEdited();
    if (lock) {
      Vec3 cd = camDir(); std::optional<Vec3> d; if (!pair.empty()) { d = rib::trunkDir(*scene_, pair); if (!d) d = cd; }
      std::vector<int> ends = pair.size() > 2 ? rib::ringOrder(*scene_, pair, d) : pair;
      QJsonArray ea; for (int a : ends) ea.append(a);
      QJsonObject rb{{"k", QJsonObject()}, {"smooth", 0.4}, {"tip", 0.25}, {"view", QJsonArray{cd.x, cd.y, cd.z}}, {"ends", ends.empty() ? QJsonValue() : QJsonValue(ea)}};
      if (!ends.empty()) {
        // the root: at the centre of the anchors, as wide as they span (the first click only starts the lock); it leaves
        // along their trunks
        auto R0 = rib::ribRoot(*scene_, ends, d); Anchor& r = scene_->addAnchor(R0.mid); c->anchors.push_back(r.id);
        QJsonObject k; k.insert(QString::number(r.id), QJsonObject{{"w", R0.w0}, {"r", 0}}); rb.insert("k", k);
        if (d) { HandleOverride h; h.out = *d * std::max(R0.w0 * 2.5, cam_.sceneScale * 0.01); c->ovr[r.id] = h; }
        c->raw.insert("rib", rb); c->hasRib = true;
        scene_->reindex(); scene_->revision++; pendingCurves_.insert(c->id); onSelChange(); requestUpdate();
        ui::toast(ends.size() > 2 ? tr("A volume branch starts from the %1 anchors — click on to place its next points, Esc — finish").arg(ends.size())
                                  : tr("The lock starts between the two anchors — click on to place its next points, Esc — finish"));
        return;
      }
      c->raw.insert("rib", rb); c->hasRib = true;
    }
  }
  if (!c->anchors.empty()) for (const Vec3& q : collideBetween(scene_->anchor(c->anchors.back())->pos, *p)) c->anchors.push_back(scene_->addAnchor(q).id);
  Anchor& a = scene_->addAnchor(*p); c->anchors.push_back(a.id);
  // a flat lock: the width — the circle's radius at that place (started from anchors: the root's width)
  if (rib::isRib(*c)) {
    QJsonObject rb = c->raw.value("rib").toObject(), k = rb.value("k").toObject(); auto e = rib::ribEnds(*scene_, *c);
    double w = 0; if (!e.empty()) { auto d = rib::trunkDir(*scene_, e); w = rib::ribRoot(*scene_, e, d ? d : std::optional<Vec3>(camDir())).w0; } else w = ribR * worldPerPixelAt(*p);
    k.insert(QString::number(a.id), QJsonObject{{"w", w}, {"r", 0}}); rb.insert("k", k); c->raw.insert("rib", rb);
  }
  scene_->reindex(); scene_->revision++;
  pendingCurves_.insert(c->id); onSelChange(); requestUpdate();
}

void Viewport::finishDrawing() {
  int id = drawing_; drawing_ = -1; ghost_.reset(); overlayRev_++;
  Item* c = scene_->item(id); if (!c) return;
  if (c->anchors.size() < 2) {
    std::vector<int> si, sa; Network(*scene_, hair_.curves, si, sa).deleteItem(id); Network(*scene_, hair_.curves, si, sa).pruneAnchors();
    sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), id), sel_.items.end());
    scene_->reindex(); reloadHair(); emit sceneEdited(); onSelChange();
    ui::toast(tr("A curve needs at least 2 points"));
    return;
  }
  const QString att = autoAttachEnds(id);                         // (begun / ended on a crown connector: attached to it)
  afterEdit(true);
  if (!att.isEmpty()) ui::toast(att);
}

std::optional<Vec3> Viewport::liftOffHead(const Vec3& mid, double span) const {
  std::vector<PickMesh> list; Box3 hb;
  for (auto& [id, it] : scene_->items) if (it.headTag && it.poly && scene_->isVisible(it)) {
    PickMesh pm; pm.itemId = id; pm.pos = &it.poly->pos; const auto& p = *it.poly;
    for (int f = 0; f < p.faceCount(); f++) { int s = p.faceStart[size_t(f)], e = p.faceStart[size_t(f + 1)]; for (int k = s + 1; k < e - 1; k++) { pm.tris.push_back(uint32_t(p.faceVerts[size_t(s)])); pm.tris.push_back(uint32_t(p.faceVerts[size_t(k)])); pm.tris.push_back(uint32_t(p.faceVerts[size_t(k + 1)])); } }
    pm.box = p.bounds(); hb.unite(pm.box); list.push_back(std::move(pm));
  }
  if (list.empty()) return std::nullopt;
  Vec3 C = scene_->bounds().center(), d = mid - C; if (d.lengthSq() < 1e-12) d = {0, 1, 0}; d = d.normalized();
  Ray r{mid + d * sceneScale(*scene_), -d}; auto h = raycast(r, list);
  if (h && (h->point - C).dot(d) > (mid - C).dot(d)) return h->point + d * (0.06 * span);
  return std::nullopt;
}

bool Viewport::escape() {
  if (topoEscape()) return true;                             // (Topology: the mask brush off, then the selection's elements / picked polygroups)
  if (mergePick) { setMergePick(false); return true; }        // (◎ Pick: cancelled)
  if (surfCutKey(Qt::Key_Escape)) return true;              // (the scissors' surface cut: cancelled)
  if (drawing_ >= 0) { finishDrawing(); setTool("cursor"); return true; }
  if (tool_ == "draw") { setTool("cursor"); return true; }
  if (tool_ == "edge" && !edges.list.empty()) { clearEdges(); return true; }
  return false;
}

// ---------- deleting, recalculating, merging, reversing ----------
void Viewport::deleteSelected() {
  if (sel_.empty() && activeRefPlane() >= 0) {               // (a reference plane active in the 🖼 menu: Del deletes it)
    int id = activeRefPlane(); auto& L = scene_->refPlanes; L.erase(std::remove_if(L.begin(), L.end(), [id](const RefPlane& r) { return r.id == id; }), L.end());
    refPlane_ = -1; refPlanesEdited(true); return;
  }
  if (!sel_.anchors.empty()) { deleteAnchors(sel_.anchors, false); return; }
  if (sel_.items.empty()) return;
  edit::deleteItems(*scene_, hair_.curves, sel_);
  if (drawing_ >= 0 && !scene_->item(drawing_)) drawing_ = -1;
  afterEdit(true);
}

void Viewport::deleteAnchors(const std::vector<int>& anchors, bool keepShape) {
  QString msg = edit::deleteAnchors(*scene_, hair_.curves, sel_, anchors, keepShape, drawing_);
  if (!msg.isEmpty()) { ui::toast(msg); return; }
  afterEdit(true);
}

void Viewport::recalcAnchors(const std::vector<int>& anchors) {
  edit::recalcHandles(*scene_, anchors);
  afterEdit(true);
  ui::toast(tr("Handles recalculated"));
}

void Viewport::mergeCoincidentCurves(const std::vector<int>& curves) {
  int n = edit::mergeCoincident(*scene_, hair_.curves, sel_, curves, cam_.sceneScale * 1e-3);
  if (!n) { ui::toast(tr("No anchors at identical places")); return; }
  afterEdit(true);
  ui::toast(tr("Merged %1 anchor%2").arg(n).arg(n > 1 ? "s" : ""));
}

void Viewport::reverseCurves(const std::vector<int>& curves) {
  Network(*scene_, hair_.curves, sel_.items, sel_.anchors).reverseCurves(curves);
  afterEdit(true);
  ui::toast(curves.size() == 1 ? tr("Curve direction reversed") : tr("%1 curves reversed").arg(curves.size()));
}

// a new group "Group N" (N past the highest), its point at 0, 0, 0; made in the Hair room it gets a colour of its own — the
// first of the palette no other group has (its curves and walls are tinted with it)
Item& Viewport::newGroup(Scene& s, int par, bool topoRoom) {
  QRegularExpression re("^Group (\\d+)$"); int mx = 0; std::set<QString> used;
  for (auto& [id, it] : s.items) if (it.type == ItemType::Group) { auto m = re.match(it.name); if (m.hasMatch()) mx = std::max(mx, m.captured(1).toInt()); if (!it.color.isEmpty()) used.insert(it.color.toLower()); }
  QJsonObject c = s.fileRaw.value("counters").toObject(); c.insert("group", mx + 1); s.fileRaw.insert("counters", c);
  Item& g = s.addItem(ItemType::Group, par, QString("Group %1").arg(mx + 1)); g.expanded = true;
  g.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "auto"}, {"conv", QJsonValue()}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false},
                      {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}, {"gpivot", QJsonArray{0, 0, 0}}};
  if (topoRoom) g.raw.insert("side", "scene");                  // (made in the Topology room: a group for meshes — the outliner's, not the Curves column's)
  else {
    static const char* PAL[] = {"#e0604f", "#4a9fe0", "#5cc46a", "#e0b040", "#b070e0", "#40c8c0", "#e07ab0", "#9ac040", "#e08a40", "#6a7ae8", "#c05050", "#50a0a0"};
    const int n = int(std::size(PAL)); int k = 0; while (k < n && used.count(PAL[k])) k++;
    g.color = PAL[k < n ? k : mx % n];
  }
  return g;
}

void Viewport::createEmptyGroup(const Vec3* pivot) {
  const int par = room() == "topo" ? meshesGroup(*scene_) : -1;   // (the Topology room: inside Meshes — every mesh in one group in the Hair room)
  Item& g = newGroup(*scene_, par, room() == "topo");
  if (pivot) g.raw.insert("gpivot", QJsonArray{pivot->x, pivot->y, pivot->z});
  sel_.items = {g.id}; sel_.anchors.clear();
  afterEdit(false); emit sceneEdited();
}

// ---------- hide, isolate ----------
void Viewport::toggleHide() {
  if (sel_.items.empty()) return;
  for (int id : sel_.items) if (Item* it = scene_->item(id)) it->visible = !it->visible;
  afterEdit(false); emit sceneEdited(); staticRev_++; allWalls_ = true;
}

// the group being worked in: the selected group, or the group holding the selection / the selected anchors
int Viewport::workingGroup() const {
  const Item* it = sel_.items.empty() ? nullptr : scene_->item(sel_.items.back());
  if (!it && !sel_.anchors.empty()) {
    auto cs = edit::curvesWith(*scene_, sel_.anchors.back()); const Item* pick = nullptr;
    for (int id : cs) { const Item* c = scene_->item(id); if (c->conn.isEmpty() && c->ring < 0) { pick = c; break; } }
    if (!pick && !cs.empty()) pick = scene_->item(cs[0]);
    it = pick;
  }
  if (it && it->type == ItemType::Curve && it->ring >= 0) it = scene_->item(it->ring);
  for (const Item* p = it ? (it->type == ItemType::Group ? it : scene_->item(it->parent)) : nullptr; p; p = scene_->item(p->parent))
    if (!p->raw.value("guidesGroup").toBool() && !p->raw.value("meshesGroup").toBool()) return p->id;
  return -1;
}

void Viewport::toggleIsolate(bool withHead) {
  if (soloOff()) return;
  if (scene_->iso && !scene_->iso->solo) { scene_->iso.reset(); sceneChanged(false); emit sceneEdited(); return; }
  Scene::Isolation iso; iso.head = withHead;
  std::function<void(int)> add = [&](int id) {
    const Item* it = scene_->item(id); if (!it || iso.keep.count(id)) return; iso.keep.insert(id);
    if (it->type == ItemType::Group) for (int c : it->children) add(c);
    if (it->type == ItemType::Surface) { add(it->a); add(it->b); }
    if (it->type == ItemType::Guide) for (const QJsonValue& r : it->raw.value("rings").toArray()) add(r.toInt());
    if (it->type == ItemType::Tube) add(it->raw.value("src").toInt(-1));
    if (it->type == ItemType::XMod) add(it->raw.contains("tube") && !it->raw.value("tube").isNull() ? it->raw.value("tube").toInt() : it->raw.value("mesh").toInt(-1));
    if (it->type == ItemType::Crown) { add(it->raw.value("part").toInt(-1)); add(it->raw.value("low").toInt(-1)); }
  };
  for (int id : sel_.items) add(id);
  for (int a : sel_.anchors) for (int c : edit::curvesWith(*scene_, a)) add(c);
  if (iso.keep.empty()) { ui::toast(tr("Select something to isolate")); return; }
  scene_->iso = iso; sceneChanged(false); emit sceneEdited();
}

void Viewport::toggleIsolateGroup() {
  if (soloOff()) return;
  if (scene_->iso && !scene_->iso->solo) { scene_->iso.reset(); sceneChanged(false); emit sceneEdited(); return; }
  int g = workingGroup(); if (g < 0) { ui::toast(tr("Select something inside a group (or the group itself)")); return; }
  Scene::Isolation iso; iso.group = g; scene_->iso = iso;
  sceneChanged(false); emit sceneEdited();
  ui::toast(tr("Only \"%1\" is shown — Ctrl+Shift+H again to show everything").arg(scene_->item(g)->name));
}

// ---- ◎ Solo: the group you work in is the only one shown; select something in another group — it switches to that one;
// nothing selected — the last group stays. An isolation of your own (Ctrl+H, Shift+H, Ctrl+Shift+H) wins while it is on ----
bool Viewport::soloFollow() {
  auto& iso = scene_->iso; bool was = iso.has_value();
  if (iso && iso->solo && iso->group >= 0 && !scene_->item(iso->group)) iso.reset();   // (its group was deleted)
  if (!solo_ || (iso && !iso->solo)) return was != iso.has_value();
  // what is selected also belongs to the shown group (an anchor shared by its curve and another group's) — stay
  if (iso && iso->group >= 0) {
    int G = iso->group;
    for (int id : sel_.items) if (scene_->isDescendant(id, G)) return false;
    for (int a : sel_.anchors) for (int c : edit::curvesWith(*scene_, a)) if (scene_->isDescendant(c, G)) return false;
  }
  // (the Topology room: the selected meshes alone — or with the references under them; a folder only when asked)
  if (room() == "topo" && soloTopo != "folder") {
    std::set<int> ms; for (int id : sel_.items) if (const Item* it = scene_->item(id); it && (it->type == ItemType::IMesh || it->type == ItemType::Mesh)) ms.insert(id);
    if (ms.empty()) return was != iso.has_value();                // (nothing selected: what is shown stays)
    if (iso && iso->solo && iso->group < 0 && std::all_of(ms.begin(), ms.end(), [&](int id) { return iso->keep.count(id) > 0; })) return false;
    Scene::Isolation s; s.solo = true; s.keep = ms;
    if (soloTopo == "high") for (int r : highPolyOf(ms)) s.keep.insert(r);
    iso = s; return true;
  }
  int g = workingGroup(); if (g < 0 || (iso && iso->group == g)) return was != iso.has_value();
  Scene::Isolation s; s.group = g; s.solo = true; iso = s;
  return true;
}

void Viewport::setSoloTopo(const QString& k) {
  soloTopo = k; QSettings().setValue("soloTopo", k);
  if (solo_ && scene_->iso && scene_->iso->solo) scene_->iso.reset();
  if (solo_) { soloFollow(); sceneChanged(false); emit sceneEdited(); }
  ui::hud(k == "mesh" ? tr("Solo: only the selected object") : k == "folder" ? tr("Solo: the selected object's folder") : tr("Solo: the object and its high-poly"), 1200);
}

std::set<int> Viewport::highPolyOf(const std::set<int>& meshes) const {
  std::set<int> out; std::vector<Vec3> pts; Box3 box;
  for (int m : meshes) if (const Item* it = scene_->item(m); it && it->poly) { const auto& P = it->poly->pos; const size_t st = std::max<size_t>(1, P.size() / 96);
    for (size_t i = 0; i < P.size(); i += st) pts.push_back(P[i]); box.unite(it->poly->bounds()); }
  if (pts.empty()) return out;
  const double tol = std::max(1e-6, box.size().length() * 0.04);
  for (auto& [id, x] : scene_->items) {
    if (meshes.count(id) || x.type != ItemType::IMesh || !x.poly || !x.raw.value("tref").toBool()) continue;
    const Box3 rb = x.poly->bounds(); if (rb.empty() || rb.max.x < box.min.x - tol || rb.min.x > box.max.x + tol || rb.max.y < box.min.y - tol || rb.min.y > box.max.y + tol || rb.max.z < box.min.z - tol || rb.min.z > box.max.z + tol) continue;
    std::function<double(const Vec3&)> dist;
    auto h = heavy_.find(id);
    if (h != heavy_.end() && h->second.bvh && h->second.src.lock() == (x.base ? x.base : x.poly)) { const Affine M = meshmods::meshMatrix(*scene_, x), Mi = M.inverse();
      dist = [&, M, Mi](const Vec3& p) { double d = 0; if (auto q = h->second.bvh->closest(Mi.apply(p), nullptr, &d)) return M.apply(*q).distanceTo(p); return 1e300; }; }
    else if ((long long)x.poly->faceVerts.size() - 2LL * x.poly->faceCount() <= 600000) { if (auto b = bvhOf(id, false)) dist = [b](const Vec3& p) { double d = 1e300; b->closest(p, nullptr, &d); return d; }; }
    if (!dist) { out.insert(id); continue; }                     // (too big to look into: its box over the mesh is enough)
    int near = 0; for (const Vec3& p : pts) if (dist(p) < tol) near++;
    if (near * 4 >= int(pts.size())) out.insert(id); }           // (a quarter of the mesh on it or more)
  return out;
}

bool Viewport::soloOff() { if (!solo_) return false; setSolo(false); ui::toast(tr("Solo off — everything is shown")); return true; }

void Viewport::setSolo(bool on) {
  solo_ = on; QSettings().setValue("solo", on);
  if (on) { soloFollow(); if (!scene_->iso) ui::toast(tr("Solo: select something in a group — only that group will be shown")); }
  else if (scene_->iso && scene_->iso->solo) scene_->iso.reset();
  sceneChanged(false); emit sceneEdited(); emit soloChanged(on);
}


// ---------- Ctrl+Shift with the Cursor (the Hair room): the group of what is under the pointer ----------
// the nearest group above the curve / wall / mesh under the pointer (not a service group: guides, meshes, imported)
int Viewport::groupUnder(const QPointF& m) const {
  int id = -1;
  if (auto ch = pickCurve(m, false, false)) { const Item* c = scene_->item(ch->id); id = c && c->ring >= 0 ? c->ring : ch->id; }
  else if (auto h = pickSurface(m)) id = h->itemId;
  for (const Item* p = id >= 0 && scene_->item(id) ? scene_->item(scene_->item(id)->parent) : nullptr; p; p = scene_->item(p->parent))
    if (p->type == ItemType::Group && !p->raw.value("guidesGroup").toBool() && !p->raw.value("meshesGroup").toBool() && !p->raw.value("importGroup").toBool()) return p->id;
  return -1;
}

void Viewport::groupHover(const QPointF& m, Qt::KeyboardModifiers mods) {
  const bool on = room() == "hair" && tool_ == "cursor" && drag_ == Drag::None && (mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier) && !ui::Pie::current();
  const int g = on ? groupUnder(m) : -1;
  if (g == hoverGroup_) return;
  hoverGroup_ = g; overlayRev_++; allWalls_ = true;
  ui::hud(g >= 0 ? tr("Group: %1 — click to select it").arg(scene_->item(g)->name) : QString());
  requestUpdate();
}
}  // namespace hm

namespace hm {
void Viewport::refreshSceneBox() {
  const Box3 b = scene_->bounds(); if (b.empty()) return;
  const Vec3 s = b.size(); const double sc = std::max({s.x, s.y, s.z}) > 0 ? std::max({s.x, s.y, s.z}) : 1.0;
  auto set = [&](Camera& c) { c.sceneCenter = b.center(); c.sceneScale = sc; };
  set(cam_); for (auto& c : panes_) set(c);
}
}  // namespace hm
