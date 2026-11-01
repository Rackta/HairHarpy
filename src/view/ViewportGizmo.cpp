// The Viewport's gizmo (Move E / Rotate R / Scale T): what it moves (the selected anchors, else everything in the
// selected objects: a group's curves and converted meshes, a wall's two curves, …), its axes (World / Local — the
// object's own rotation, it.q, as in Cinema 4D), the drag applied to them, the commit.
#include "scene/Edit.h"
#include "scene/MeshMods.h"
#include "scene/TopoEdit.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <functional>
#include <map>
#include <set>

namespace hm {

namespace {
Quat quatOf(const QJsonValue& v) { QJsonArray a = v.toArray(); return a.size() == 4 ? Quat{a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()} : Quat{}; }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
}  // namespace

std::optional<Viewport::GzTargets> Viewport::gizmoTargets() const {
  // a reference plane active in the 🖼 menu: the gizmo at its pivot (only in its home window)
  if (int id = activeRefPlane(); id >= 0) {
    RefPlane rp = *const_cast<Viewport*>(this)->refPlane(id);
    if (!refs::shownIn(rp, cam_) && (rp.onlyView && cam_.view != rp.view)) return std::nullopt;
    refs::apply(rp, cam_);
    GzTargets t; t.pivot = refs::pivotWorld(rp); t.refPlane = id; return t;
  }
  // the Topology room, a component mode: the selected elements of the active mesh (their centre)
  if (room() == "topo" && topoMode != "object") {
    int m = topoMesh(); if (m < 0) return std::nullopt;
    auto vs = tSelVerts(m); if (vs.empty()) return std::nullopt;
    const auto& W = scene_->item(m)->poly->pos; Vec3 c; for (int v : vs) c += W[size_t(v)];
    GzTargets t; t.topo = m; t.verts = vs; t.pivot = c * (1.0 / vs.size()); return t;
  }
  if (!sel_.anchors.empty()) return GzTargets{sel_.anchors, {}, scene_->anchor(sel_.anchors.back())->pos};
  std::set<int> anchors; std::vector<int> av; std::set<int> meshes, imeshes, groups;
  auto addC = [&](int cid) { if (const Item* c = scene_->item(cid)) for (int a : c->anchors) if (anchors.insert(a).second) av.push_back(a); };
  std::function<void(int)> add = [&](int id) {
    const Item* it = scene_->item(id); if (!it) return;
    switch (it->type) {
      case ItemType::Group: groups.insert(id); for (int c : it->children) add(c); break;
      case ItemType::Curve: addC(id); break;
      case ItemType::Surface: addC(it->a); addC(it->b); break;
      case ItemType::Tube: addC(it->raw.value("src").toInt(-1)); break;
      case ItemType::XMod: if (it->raw.contains("tube") && !it->raw.value("tube").isNull()) add(it->raw.value("tube").toInt()); else if (it->raw.value("mesh").isDouble()) add(it->raw.value("mesh").toInt()); break;
      case ItemType::Crown: add(it->raw.value("part").toInt(-1)); add(it->raw.value("low").toInt(-1)); break;
      case ItemType::Guide: if (it->raw.value("kind").toString() == "disc") for (const QJsonValue& r : it->raw.value("rings").toArray()) addC(r.toInt()); break;
      case ItemType::Mesh: if (it->poly) meshes.insert(id); break;
      case ItemType::IMesh: if (it->poly) imeshes.insert(id); for (int c : it->children) add(c); break;   // (its Transform modifier; what is under it goes along)
      default: break;
    }
  };
  for (int id : sel_.items) add(id);
  auto gpv = [&](int id) { const QJsonArray a = scene_->item(id)->raw.value("gpivot").toArray(); return a.size() == 3 ? Vec3(a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble()) : Vec3(); };   // (a group's own point: 0, 0, 0 unless moved)
  if (av.empty() && meshes.empty() && imeshes.empty()) { if (groups.empty()) return std::nullopt; GzTargets t; t.pivot = gpv(*groups.begin()); t.groups.assign(groups.begin(), groups.end()); return t; }   // (an empty group with a point of its own)
  Box3 b; for (int a : av) b.expand(scene_->anchor(a)->pos);
  for (int m : meshes) b.unite(scene_->item(m)->poly->bounds());
  for (int m : imeshes) b.unite(scene_->item(m)->poly->bounds());
  Vec3 piv = b.center();
  // (Topology: one mesh turns and scales about its pivot — the file's axis when imported there)
  if (room() == "topo" && imeshes.size() == 1 && av.empty() && meshes.empty()) { int m = *imeshes.begin(); piv = meshmods::meshMatrix(*scene_, *scene_->item(m)).apply(meshPivotLocal(m)); }
  else if (room() == "topo" && sel_.items.size() == 1 && imeshes.count(sel_.items[0])) { const int m = sel_.items[0]; piv = meshmods::meshMatrix(*scene_, *scene_->item(m)).apply(meshPivotLocal(m)); }   // (an object with others under it: about its own pivot)
  if (sel_.items.size() == 1 && groups.count(sel_.items[0])) piv = gpv(sel_.items[0]);   // (a group alone picked: at its own point)
  GzTargets t{av, std::vector<int>(meshes.begin(), meshes.end()), piv, std::vector<int>(imeshes.begin(), imeshes.end())}; t.groups.assign(groups.begin(), groups.end()); return t;
}

// local axes: the object's own rotation and its groups' (anchors: their curve's)
Quat Viewport::localFrame() const {
  if (int id = activeRefPlane(); id >= 0) { RefPlane rp = *const_cast<Viewport*>(this)->refPlane(id); refs::apply(rp, cam_); return rp.quat; }
  auto ownQ = [&](const Item* it) { return it ? quatOf(it->raw.value("q")) : Quat{}; };
  auto worldQ = [&](const Item* it) { Quat q = ownQ(it); for (const Item* p = it ? scene_->item(it->parent) : nullptr; p; p = scene_->item(p->parent)) q = ownQ(p) * q; return q.normalized(); };
  if (!sel_.anchors.empty()) {
    auto cs = edit::curvesWith(*scene_, sel_.anchors.back()); const Item* c = nullptr;
    for (int id : cs) if (scene_->item(id)->ring < 0) { c = scene_->item(id); break; }
    if (!c && !cs.empty()) c = scene_->item(cs[0]);
    if (c && c->ring >= 0) c = scene_->item(c->ring);
    return c ? worldQ(c) : Quat{};
  }
  return sel_.items.empty() ? Quat{} : worldQ(scene_->item(sel_.items.back()));
}

// after a rotation (world rotation D) the selected objects' own axes turn with it (anchors don't turn their curve's)
void Viewport::rotateOwnAxes(const Quat& D) {
  if (!sel_.anchors.empty()) return;
  auto ownQ = [&](const Item* it) { return it ? quatOf(it->raw.value("q")) : Quat{}; };
  auto worldQ = [&](const Item* it) { Quat q = ownQ(it); for (const Item* p = it ? scene_->item(it->parent) : nullptr; p; p = scene_->item(p->parent)) q = ownQ(p) * q; return q.normalized(); };
  for (int id : sel_.items) {
    bool under = false; for (int o : sel_.items) if (o != id && scene_->isDescendant(id, o)) under = true;   // (topSelected)
    Item* it = scene_->item(id); if (under || !it || it->type == ItemType::IMesh || it->type == ItemType::Head) continue;
    const Item* p = scene_->item(it->parent); Quat P = p ? worldQ(p) : Quat{};
    Quat own = (P.conj() * D * P * ownQ(it)).normalized();
    it->raw.insert("q", QJsonArray{own.x, own.y, own.z, own.w});
  }
}

// ---------- the mesh's own gizmo: kept in its Transform ----------
namespace {
Item* xformItem(Scene& sc, int m) { for (int x : meshmods::modsOf(sc, m)) if (sc.item(x)->raw.value("kind").toString() == "xform") return sc.item(x); return nullptr; }
// the turn taking a onto b (both unit); opposite — half a turn about `alt` (a line square to a)
Quat turnOnto(const Vec3& a, const Vec3& b, const Vec3& alt) {
  const Vec3 c = a.cross(b); const double s = c.length(), d = a.dot(b);
  if (s < 1e-9) { if (d > 0) return {}; Vec3 ax = alt - a * alt.dot(a); if (ax.lengthSq() < 1e-12) ax = std::abs(a.x) < 0.9 ? Vec3(1, 0, 0).cross(a) : Vec3(0, 1, 0).cross(a); return Quat::fromAxisAngle(ax, M_PI); }
  return Quat::fromAxisAngle(c, std::atan2(s, d));
}
// the points' shape (Jacobi on their covariance): their centre; a point, a line (along `dir`), a plane (square to `n`)
// or a lump (none of them)
struct PtsFit { Vec3 c, dir, n; int kind = 0; };   // (0 a point, 1 a line, 2 a plane, 3 a lump)
PtsFit fitPts(const std::vector<Vec3>& P) {
  PtsFit F; if (P.empty()) return F;
  Vec3 c; for (auto& p : P) c += p; c = c * (1.0 / double(P.size())); F.c = c;
  double A[3][3] = {}; for (auto& p : P) { const Vec3 d = p - c; const double v[3] = {d.x, d.y, d.z}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) A[i][j] += v[i] * v[j]; }
  double V[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int it = 0; it < 50; it++) for (int p = 0; p < 2; p++) for (int q = p + 1; q < 3; q++) {
    if (std::abs(A[p][q]) < 1e-15) continue;
    const double th = 0.5 * std::atan2(2 * A[p][q], A[q][q] - A[p][p]), cs = std::cos(th), sn = std::sin(th);
    for (int k = 0; k < 3; k++) { const double a = A[k][p], b = A[k][q]; A[k][p] = cs * a - sn * b; A[k][q] = sn * a + cs * b; }
    for (int k = 0; k < 3; k++) { const double a = A[p][k], b = A[q][k]; A[p][k] = cs * a - sn * b; A[q][k] = sn * a + cs * b; }
    for (int k = 0; k < 3; k++) { const double a = V[k][p], b = V[k][q]; V[k][p] = cs * a - sn * b; V[k][q] = sn * a + cs * b; }
  }
  int o[3] = {0, 1, 2}; std::sort(o, o + 3, [&](int x, int y) { return A[x][x] > A[y][y]; });   // (the spreads, biggest first)
  const double big = A[o[0]][o[0]], mid = A[o[1]][o[1]], small = A[o[2]][o[2]];
  F.dir = Vec3(V[0][o[0]], V[1][o[0]], V[2][o[0]]).normalized(); F.n = Vec3(V[0][o[2]], V[1][o[2]], V[2][o[2]]).normalized();
  F.kind = big <= 1e-20 ? 0 : mid < big * 1e-3 ? 1 : small <= mid * 0.25 ? 2 : 3;
  return F;
}
}  // namespace

Quat Viewport::meshOwnQ(int m) const {
  const Item* it = scene_->item(m); if (!it) return {};
  Affine M = meshmods::meshMatrix(*scene_, *it); Vec3 z = M.lin({0, 0, 1}), y = M.lin({0, 1, 0});
  return z.lengthSq() > 0 && y.lengthSq() > 0 ? Quat::lookRotation(-z.normalized(), y.normalized()) : Quat{};
}
Quat Viewport::meshGizmoQ(int m) const {
  Item* x = xformItem(*scene_, m); if (!x) return {};
  QJsonArray a = x->raw.value("p").toObject().value("gq").toArray();
  return a.size() == 4 ? Quat{a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()}.normalized() : Quat{};
}
void Viewport::storeMeshGizmo(int m, const std::optional<Vec3>& pivotWorld, const Quat& axesWorld) {
  Item* it = scene_->item(m); if (!it) return;
  if (!xformItem(*scene_, m)) meshmods::setMeshMatrix(*scene_, m, Affine{});      // (as it is: a Transform of nothing — to keep them in)
  Item* x = xformItem(*scene_, m); if (!x) return;
  const Quat rel = (meshOwnQ(m).conj() * axesWorld).normalized();
  QJsonObject p = x->raw.value("p").toObject();
  if (std::abs(rel.w) > 1 - 1e-12) p.remove("gq"); else p.insert("gq", QJsonArray{rel.x, rel.y, rel.z, rel.w});
  x->raw.insert("p", p);
  if (pivotWorld) { setMeshPivot(m, meshmods::meshMatrix(*scene_, *it).inverse().apply(*pivotWorld)); return; }   // (its own undo step)
  scene_->revision++; commit(false); emit topoState(); updateGizmo();
}

// Align: one axis of the gizmo (or the axis square to a plane of it) turned onto the selection — the vertices (X to the
// selected vertex), the edges (along them; a plane — through them), the normal, the view; World — the world's axes.
// Object mode: into the mesh's Transform; a component mode: the gizmo of the selection (Custom) and the mesh's too
QString Viewport::gzAlign(const QString& axis, const QString& to) {
  const int m = topoMesh(); if (room() != "topo" || m < 0) { ui::toast(tr("Select a mesh")); return "!"; }
  // (always the object's own gizmo — its axes and pivot kept in its Transform — not the selection's)
  const Quat Q = gzFrameLocal();
  const Vec3 Po = meshmods::meshMatrix(*scene_, *scene_->item(m)).apply(meshPivotLocal(m));   // (the mesh's pivot: what an axis turns to a vertex from)
  const bool plane = axis.size() == 2; const int k = plane ? (axis == "yz" ? 0 : axis == "xz" ? 1 : 2) : (axis == "y" ? 1 : axis == "z" ? 2 : 0);
  const Vec3 E[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; const Vec3 a = Q.rotate(E[k]);
  Quat N; std::optional<Vec3> newPivot; QString onto;
  auto side = [&](Vec3 t) { t = t.normalized(); return t.dot(a) < 0 ? t * -1.0 : t; };   // (a line, not a way: the nearer side)
  if (to == "world") N = Quat{};
  else {
    std::optional<Vec3> t;
    if (to == "view") { t = side(cam_.quat.rotate({0, 0, 1})); onto = tr("the view"); }
    else if (to == "normal") { Vec3 n = tsel_.id == m && topoMode != "object" ? tSelNormal(m) : Vec3(); if (n.lengthSq() > 0) t = side(n); onto = tr("the normal"); }
    else {
      // the selection (its vertices — of the picked edges / polygons too, world as shown): one vertex — the axis at it, the
      // pivot stays; a line — the axis along it, the pivot to its middle; a plane — the axis into it (another axis already
      // in it: turned about that one — so two get aligned), the plane of the gizmo onto it, the pivot to its centre
      const auto W = tWorld(m); std::vector<Vec3> pts;
      if (tsel_.id == m && topoMode != "object") for (int v : tSelVerts(m)) if (size_t(v) < W.size()) pts.push_back(W[size_t(v)]);
      if (pts.empty()) { ui::toast(tr("Select vertices, edges or polygons to align the object's gizmo to — a component mode")); return "!"; }
      const PtsFit F = fitPts(pts);
      if (F.kind == 0) {
        const Vec3 d = F.c - Po; if (d.lengthSq() < 1e-14) { ui::toast(tr("The vertex is at the object's pivot — nothing to turn the axis to")); return "!"; }
        if (!plane) t = d.normalized();
        else { Vec3 n = a - d.normalized() * a.dot(d.normalized()); if (n.lengthSq() < 1e-12) { ui::toast(tr("That plane already goes at the vertex")); return "!"; } t = n.normalized(); }
        onto = tr("the vertex");
      } else if (F.kind == 1) {
        newPivot = F.c;
        if (!plane) t = side(F.dir);
        else { Vec3 n = a - F.dir * a.dot(F.dir); if (n.lengthSq() < 1e-12) { ui::toast(tr("The selection lies along that axis — no plane through it square to it")); return "!"; } t = n.normalized(); }
        onto = tr("the line of the selection");
      } else if (F.kind == 2) {
        newPivot = F.c;
        if (plane) t = side(F.n);
        else {
          Vec3 in = a - F.n * a.dot(F.n);
          for (int j = 0; j < 3; j++) { const Vec3 b = Q.rotate(E[j]); if (j != k && std::abs(b.dot(F.n)) < 1e-4) { in = F.n.cross(b); break; } }   // (one already in the plane: turned about it)
          if (in.lengthSq() < 1e-12) { ui::toast(tr("The axis stands square to the selection's plane — align a plane of the gizmo instead")); return "!"; }
          t = side(in);
        }
        onto = tr("the plane of the selection");
      } else { ui::toast(tr("The selection lies in no plane — pick vertices or edges a plane goes through")); return "!"; }
    }
    if (!t) { ui::toast(tr("Nothing to align to — select the elements first")); return "!"; }
    N = (turnOnto(a, t->normalized(), Q.rotate(E[(k + 1) % 3])) * Q).normalized();
  }
  const char* names[3] = {"X", "Y", "Z"}; const char* planes[3] = {"YZ", "XZ", "XY"};
  const QString what = plane ? tr("Plane %1").arg(planes[k]) : tr("%1").arg(names[k]);
  tset.gzOrient = "local";
  storeMeshGizmo(m, newPivot, N);
  updateGizmo(); emit topoState();
  const QString msg = to == "world" ? tr("The object's gizmo: the world's axes") : tr("The object's gizmo: %1 aligned to %2").arg(what, onto) + (newPivot ? tr(", its centre moved there") : QString());
  ui::toast(msg); return msg;
}

void Viewport::setGizmoLocal(bool on) { gzLocal_ = on; if (room() == "topo") { setGzOrient(on ? "local" : "world"); return; } updateGizmo(); }
void Viewport::setGzOrient(const QString& o) { tset.gzOrient = o; gzLocal_ = o == "local"; saveTopoPrefs(); updateGizmo(); emit topoState(); }

// (the custom pivot's owner: the mode, the mesh, the selection's version — another selection, the centre again)
QByteArray Viewport::gzKey() const {
  QByteArray k = topoMode.toLatin1() + "|" + QByteArray::number(topoMesh()) + "|" + QByteArray::number(topoMode == "object" ? 0 : tsel_.ver);
  for (int id : sel_.items) k += "," + QByteArray::number(id);
  return k;
}
// the selection's average normal (its polygons' — those of the picked edges / vertices), world
Vec3 Viewport::tSelNormal(int m) const {
  const Item* it = scene_->item(m); if (!it) return {};
  auto T = topo::ttopo(it->base ? it->base : it->poly); const auto W = tWorld(m); Vec3 n;
  for (int f : tTouchedFaces(m)) { const int s = T->fo[size_t(f)], e = T->fo[size_t(f) + 1];
    for (int k = s; k < e; k++) { const Vec3& a = W[size_t(T->fv[size_t(k)])]; const Vec3& b = W[size_t(T->fv[size_t(k + 1 < e ? k + 1 : s)])];
      n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)); } }   // (Newell)
  return n.lengthSq() > 0 ? n.normalized() : n;
}
// the gizmo's axes: World; Local — the mesh's own (its Transform's turn); Normal — Z along the selection's normal;
// Camera — the view's; Custom — where an Alt drag put them
Quat Viewport::gzFrame() const {
  if (room() != "topo") return gzLocal_ ? localFrame() : Quat{};
  const QString& o = tset.gzOrient;
  if (o == "camera") return cam_.quat;
  if (o == "custom") { if (topoMode == "object" && gzCustom_.key != gzKey()) return gzFrameLocal(); return gzCustom_.q; }   // (Object mode: the mesh's own gizmo, not the selection's)
  if (o == "normal") {
    const int m = topoMesh(); Vec3 n = m >= 0 && topoMode != "object" ? tSelNormal(m) : Vec3();
    if (n.lengthSq() < 1e-12) return Quat{};
    return Quat::lookRotation(-n, std::abs(n.y) < 0.95 ? Vec3(0, 1, 0) : Vec3(1, 0, 0));
  }
  if (o == "local") return gzFrameLocal();
  return Quat{};
}
// Local: the mesh's own axes and its gizmo's turn kept in its Transform
Quat Viewport::gzFrameLocal() const {
  const int m = topoMesh();
  if (m >= 0) { Affine M = meshmods::meshMatrix(*scene_, *scene_->item(m)); Vec3 z = M.lin({0, 0, 1}), y = M.lin({0, 1, 0});
      if (z.lengthSq() > 0 && y.lengthSq() > 0) return (Quat::lookRotation(-z.normalized(), y.normalized()) * meshGizmoQ(m)).normalized(); }   // (the axes kept in its Transform)
  return localFrame();
}
void Viewport::setAxisLock(int k, bool locked) { gz_.lock[k] = locked; overlayRev_++; requestUpdate(); }

void Viewport::updateGizmo() {
  if (drag_ == Drag::Gizmo) return;
  Gizmo::Mode m = tool_ == "move" ? Gizmo::Mode::Move : tool_ == "rotate" ? Gizmo::Mode::Rotate : tool_ == "gizmo" ? Gizmo::Mode::Scale : Gizmo::Mode::None;
  auto tg = m == Gizmo::Mode::None ? std::nullopt : gizmoTargets();
  if (!tg) { gz_.mode = Gizmo::Mode::None; requestUpdate(); return; }
  gz_.mode = m; gz_.pivot = tg->pivot; gz_.quat = gzFrame();
  if (room() == "topo" && tset.gzOrient == "custom" && gzCustom_.hasPivot && gzCustom_.key == gzKey()) gz_.pivot = gzCustom_.pivot;   // (Custom: the pivot put there too)
  requestUpdate();
}

std::optional<QPointF> Viewport::gizmoScreen() const {
  if (!gz_.visible()) return std::nullopt;
  ScreenPt s = projector().toScreen(gz_.pivot); return QPointF(s.x, s.y);
}

std::vector<Gizmo::Line> Viewport::gizmoLines() const {
  if (room() == "topo" && tset.gzOrient == "camera" && drag_ != Drag::Gizmo) const_cast<Gizmo&>(gz_).quat = cam_.quat;   // (Camera: the axes follow the view)
  return gz_.lines(cam_, paneW(), paneH());
}

// Alt + a handle (the Topology room, Move / Rotate): the gizmo itself moved / turned, the model left — Custom after
bool Viewport::gizmoPressCustom(const QPointF& m) {
  if (!gz_.visible() || gz_.mode == Gizmo::Mode::Scale) return false;
  Gizmo::Handle h = gz_.pick(cam_, paneW(), paneH(), m.x(), m.y()); if (!h.valid()) return false;
  gd_ = {}; gdCustom_ = true; gz_.begin(h, cam_, paneW(), paneH(), m.x(), m.y()); drag_ = Drag::Gizmo;
  ui::hud(tr("Alt: the gizmo itself — its place and axes (Custom)"));
  return true;
}

// a drag in free space (off the mesh): the gizmo's free handle — Move: in the view's plane, Rotate: the view ring,
// Scale: uniform
bool Viewport::gizmoPressFree(const QPointF& m) {
  if (!gz_.visible()) return false;
  return gizmoPress(m, false, gz_.mode == Gizmo::Mode::Rotate ? "E" : "XYZ");
}

bool Viewport::gizmoPress(const QPointF& m, bool ctrl, const QString& forced) {
    if (!gz_.visible()) return false;
  double W = paneW(), H = paneH();
  Gizmo::Handle h = forced.isEmpty() ? gz_.pick(cam_, W, H, m.x(), m.y()) : Gizmo::Handle{forced}; if (!h.valid()) return false;
  std::shared_ptr<PolyMesh> pre; TSel preSel = tsel_;
  if (ctrl && room() == "topo" && topoMode != "object") {   // (Ctrl + a handle: extruded first, the new part moved)
    const int am = topoMesh(); if (am < 0) return false; pre = baseSp(*scene_->item(am));
    if (!tExtrudeSel(am)) return true;
  }
  auto tg = gizmoTargets(); if (!tg) return false;
  gd_ = {}; gd_.list = tg->anchors; gd_.meshes = tg->meshes;
  if (tg->topo >= 0) {                                       // (the selected elements: from where they are, as shown)
    const Item* it = scene_->item(tg->topo); tgz_ = {}; tgz_.on = true; tgz_.m = tg->topo; tgz_.verts = tg->verts;
    tgz_.base = (it->base ? it->base : it->poly)->pos; tgz_.uv0 = (it->base ? it->base : it->poly)->cornerUV; for (int v : tg->verts) tgz_.w0.push_back(it->poly->pos[size_t(v)]);
    if (tset.sym) tgz_.pairs = symFirstPairs(tgz_.m, tgz_.base);
    tgz_.preExtrude = pre; tgz_.preSel = preSel; tgz_.q0 = gz_.quat; tgz_.p0 = gz_.pivot;
    auto T = topo::ttopo(baseSp(*it)); const auto& Wp = it->poly->pos;
    std::map<int, int> at; for (size_t i = 0; i < tgz_.verts.size(); i++) at[tgz_.verts[i]] = int(i);
    if (tset.gzWhat == "each" && tsel_.id == tg->topo) {      // (Components: each its own centre and — Normal — its own axes)
      const char key = tKey(); const bool nrm = tset.gzOrient == "normal";
      auto faceN = [&](int f) { Vec3 n; const int s = T->fo[size_t(f)], e = T->fo[size_t(f) + 1];
        for (int k = s; k < e; k++) { const Vec3& a = Wp[size_t(T->fv[size_t(k)])]; const Vec3& b = Wp[size_t(T->fv[size_t(k + 1 < e ? k + 1 : s)])]; n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)); } return n; };
      auto frame = [&](Vec3 n) { if (!nrm || n.lengthSq() < 1e-24) return gz_.quat; n = n.normalized(); return Quat::lookRotation(-n, std::abs(n.y) < 0.95 ? Vec3(0, 1, 0) : Vec3(1, 0, 0)); };
      auto add = [&](const std::vector<int>& vs, const Vec3& n) { GzComp c; for (int v : vs) if (at.count(v)) { c.idx.push_back(at[v]); c.c += Wp[size_t(v)]; }
        if (c.idx.empty()) return; c.c = c.c * (1.0 / double(c.idx.size())); c.q = frame(n); tgz_.comps.push_back(std::move(c)); };
      if (key == 'f') for (int f : tsel_.f) { std::vector<int> vs; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) vs.push_back(T->fv[size_t(k)]); add(vs, faceN(f)); }
      else if (key == 'e') for (int e : tsel_.e) { Vec3 n; for (int f : {T->ef0[size_t(e)], T->ef1[size_t(e)]}) if (f >= 0) n += faceN(f).normalized(); add({T->ea[size_t(e)], T->eb[size_t(e)]}, n); }
      else for (int v : tsel_.v) { Vec3 n; for (int j = T->voff[size_t(v)]; j < T->voff[size_t(v) + 1]; j++) { const int e = T->vE[size_t(j)]; for (int f : {T->ef0[size_t(e)], T->ef1[size_t(e)]}) if (f >= 0) n += faceN(f).normalized(); } add({v}, n); }
    }
    if (tset.gzSlide && gz_.mode == Gizmo::Mode::Move) {      // (Slide: each vertex's edges off the selection)
      for (int v : tgz_.verts) { std::vector<int> nb; for (int j = T->voff[size_t(v)]; j < T->voff[size_t(v) + 1]; j++) { const int w = T->other(T->vE[size_t(j)], v); if (!at.count(w)) nb.push_back(w); } tgz_.nbs.push_back(nb); }
    }
  }
  if (tg->refPlane >= 0) { RefPlane* rp = refPlane(tg->refPlane); refs::apply(*rp, cam_); rpd_ = {}; rpd_.id = rp->id; rpd_.m0 = rp->matrix(); rpd_.piv0 = refs::pivotWorld(*rp); }
  if (!gd_.list.empty()) { auto rel = releaseFrozenAround(*scene_, gd_.list); if (!rel.empty()) { std::vector<int> r; for (auto& x : rel) r.push_back(x.anchor); markCurves(curvesTouching(*scene_, r)); } }   // (as for a drag)
  for (int a : gd_.list) gd_.starts.push_back(scene_->anchor(a)->pos);
  hsym_ = hairSymTwins(gd_.list);                           // (the Hair room's Symmetry: the anchors' twins follow mirrored)
  for (int id : gd_.meshes) gd_.meshStarts.push_back(scene_->item(id)->poly->pos);
  gd_.imeshes = tg->imeshes; for (int id : gd_.imeshes) gd_.imStarts.push_back(meshmods::meshMatrix(*scene_, *scene_->item(id)));
  gd_.groups = tg->groups; for (int id : gd_.groups) { const QJsonArray a = scene_->item(id)->raw.value("gpivot").toArray(); gd_.gpStarts.push_back(Vec3(a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble())); }
  gz_.begin(h, cam_, W, H, m.x(), m.y());
  drag_ = Drag::Gizmo;
  return true;
}

void Viewport::gizmoMove(const QPointF& m, Qt::KeyboardModifiers mods) {
  bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
  // (Shift: Move in steps of the set length — but one anchor in the Hair room slides along the nearest curve instead)
  gz_.moveStep = room() != "topo" && gz_.mode == Gizmo::Mode::Move && gd_.list.size() == 1 && sel_.anchors.size() == 1 && gd_.meshes.empty() ? 0 : tset.gzStep;
  Affine M = gz_.drag(cam_, paneW(), paneH(), m.x(), m.y(), shift, ctrl);
  if (gdCustom_) { requestUpdate(); return; }                 // (only the gizmo moves)
  if (gz_.mode == Gizmo::Mode::Scale) {                      // (Scale: the change in percent — each axis that changes; all alike — one number)
    const double* s = gz_.scaleNow; auto pc = [](double v) { return QString::number(v * 100, 'f', std::abs(v * 100 - std::round(v * 100)) < 0.05 ? 0 : 1) + "%"; };
    QStringList parts; for (int k = 0; k < 3; k++) if (std::abs(s[k] - 1) > 1e-9) parts << QString("XYZ")[k] + QString(" ") + pc(s[k]);
    const bool same = std::abs(s[0] - s[1]) < 1e-9 && std::abs(s[1] - s[2]) < 1e-9;
    ui::hud(same ? tr("Scale: %1").arg(pc(s[0])) : parts.isEmpty() ? tr("Scale: 100%") : tr("Scale: %1").arg(parts.join(" · ")));
  }
  if (rpd_.id >= 0) {                                        // a reference plane: the whole plane, or (pivot unlocked) its pivot
    if (RefPlane* rp = refPlane(rpd_.id)) {
      if (refPivEdit_) { Vec3 l = m4Apply(m4Inverse(rpd_.m0), M.apply(rpd_.piv0)); rp->piv[0] = l.x; rp->piv[1] = l.y; }
      else { m4Decompose(m4Mul(m4FromAffine(M), rpd_.m0), rp->pos, rp->quat, rp->scl); refs::capture(*rp, cam_); }
      gd_.moved = true; refRev_++; requestUpdate();
    }
    return;
  }
  if (tgz_.on) { topoGizmoApply(M); gd_.moved = true; requestUpdate(); return; }
  for (size_t i = 0; i < gd_.list.size(); i++) scene_->anchor(gd_.list[i])->pos = M.apply(gd_.starts[i]);
  if (!hsym_.start.empty()) hairSymApply(hsym_);
  // frozen handles turn / stretch with the anchors
  if (std::abs(gd_.lastM.det()) > 1e-12) {
    Affine inc = Affine::mul(M, gd_.lastM.inverse());
    std::set<int> ids(gd_.list.begin(), gd_.list.end());
    for (auto& [cid, c] : scene_->items) if (c.type == ItemType::Curve) for (auto& [aid, o] : c.ovr) if (ids.count(aid)) { if (o.in) o.in = inc.lin(*o.in); if (o.out) o.out = inc.lin(*o.out); }
  }
  gd_.lastM = M;
  for (size_t i = 0; i < gd_.meshes.size(); i++) { auto& P = scene_->item(gd_.meshes[i])->poly->pos; for (size_t k = 0; k < P.size(); k++) P[k] = M.apply(gd_.meshStarts[i][k]); }
  if (!gd_.meshes.empty()) staticRev_++;
  for (size_t i = 0; i < gd_.imeshes.size(); i++) meshmods::setMeshMatrix(*scene_, gd_.imeshes[i], Affine::mul(M, gd_.imStarts[i]));   // (the Transform modifier: remade in the next frame)
  for (size_t i = 0; i < gd_.groups.size(); i++) if (Item* gi = scene_->item(gd_.groups[i])) { const Vec3 p = M.apply(gd_.gpStarts[i]); gi->raw.insert("gpivot", QJsonArray{p.x, p.y, p.z}); gd_.moved = true; }   // (a group's own point goes with it)
  // Shift with Move on one anchor: it sits on the curve nearest to where the gizmo puts it and slides along it
  bool one = gz_.mode == Gizmo::Mode::Move && gd_.list.size() == 1 && sel_.anchors.size() == 1 && gd_.meshes.empty();
  if (one && shift) {
    int a = gd_.list[0]; Vec3 p = scene_->anchor(a)->pos;
    struct R { double d; Vec3 q; int c; }; std::optional<R> best, cur;
    for (const Item* k : scene_->curves()) {
      if (k->ring >= 0 || std::find(k->anchors.begin(), k->anchors.end(), a) != k->anchors.end() || !scene_->isVisible(*k)) continue;
      auto it = hair_.curves.find(k->id); if (it == hair_.curves.end() || it->second.p.size() < 2) continue;
      const auto& S = it->second.p; R r{1e300, {}, k->id};
      for (size_t i = 0; i + 1 < S.size(); i++) { Vec3 ab = S[i + 1] - S[i]; double l2 = ab.lengthSq(), t = l2 > 0 ? clamp((p - S[i]).dot(ab) / l2, 0, 1) : 0; Vec3 q = S[i] + ab * t; double d = q.distanceTo(p); if (d < r.d) { r.d = d; r.q = q; } }
      if (!best || r.d < best->d) best = r;
      if (k->id == gd_.snap) cur = r;
    }
    if (cur && best && cur->d <= best->d * 1.1 + 0.01 * projectStep(*scene_)) best = cur;
    if (best) {
      if (best->c != gd_.snap) { gd_.snap = best->c; ad_.snap = best->c; overlayRev_++; ui::hud(tr("Sliding along %1 (release Shift — free move)").arg(scene_->item(best->c)->name)); }
      scene_->anchor(a)->pos = best->q;
    }
  } else if (gd_.snap >= 0) { gd_.snap = -1; ad_.snap = -1; overlayRev_++; ui::hud({}); }
  if (!gd_.list.empty()) { auto ids = gd_.list; const auto tw = hsym_.twinIds(); ids.insert(ids.end(), tw.begin(), tw.end()); markCurves(curvesTouching(*scene_, ids)); }
  gd_.moved = true; scene_->revision++;
  if (gz_.mode == Gizmo::Mode::Move && gd_.list.size() == 1 && !sel_.anchors.empty()) { int wc = findWeldCandidate(gd_.list[0]); if (wc != weldCand_) { weldCand_ = wc; anchors_ = anchorList(*scene_, ds_, sel_, weldCand_); overlayRev_++; } }
  requestUpdate();
}

void Viewport::gizmoRelease(const QPointF& m) {
  drag_ = Drag::None; gz_.active = {};
  if (gz_.mode == Gizmo::Mode::Scale) ui::hud({});           // (the percent shown while scaling: gone)
  if (gdCustom_) {                                             // (the gizmo's new place and axes kept: Custom)
    if (room() == "topo" && topoMode == "object" && topoMesh() >= 0) {   // (Object mode: the mesh's own gizmo — into its Transform)
      gdCustom_ = false; ui::hud({}); tset.gzOrient = "local"; storeMeshGizmo(topoMesh(), gz_.pivot, gz_.quat); return; }
    gdCustom_ = false; gzCustom_.q = gz_.quat; gzCustom_.pivot = gz_.pivot; gzCustom_.hasPivot = true; gzCustom_.key = gzKey(); ui::hud({});
    tset.gzOrient = "custom"; saveTopoPrefs(); emit topoState(); updateGizmo(); return;
  }
  if (gd_.snap >= 0) { ui::hud({}); ad_.snap = -1; gd_.snap = -1; overlayRev_++; }
  { const QString what = gz_.mode == Gizmo::Mode::Rotate ? tr("Rotate") : gz_.mode == Gizmo::Mode::Scale ? tr("Scale") : tr("Move");   // (the history's name for the step)
    QStringList names; for (int id : sel_.items) if (const Item* it = scene_->item(id)) names << it->name;
    stepLabel_ = rpd_.id >= 0 ? tr("%1: reference plane").arg(what) : tgz_.on ? tr("%1: %2 vertices").arg(what).arg(tgz_.verts.size()) : !sel_.anchors.empty() ? tr("%1: %2 anchor(s)").arg(what).arg(sel_.anchors.size())
               : names.size() == 1 ? tr("%1: %2").arg(what, names[0]) : names.isEmpty() ? what : tr("%1: %2 items").arg(what).arg(names.size()); }
  if (!gd_.moved) { stepLabel_.clear(); if (tgz_.on && tgz_.preExtrude) { topoWrite(tgz_.m, tgz_.preExtrude); tsel_ = tgz_.preSel; tsel_.ver++; topoRev_++; emit topoState(); ui::hud({}); }   // (an extrude let go unmoved: none)
    rpd_ = {}; tgz_ = {}; updateGizmo(); return; }
  if (tgz_.on) { int m = tgz_.m; tgz_ = {}; gd_ = {}; if (Item* it = scene_->item(m)) topoWrite(m, it->base ? it->base : it->poly); commit(false); emit topoState(); updateGizmo(); return; }
  if (rpd_.id >= 0) { rpd_ = {}; gd_ = {}; commit(false); updateGizmo(); return; }
  beforeFrame();
  HairChanges ch = hair_.finishDraft(*scene_);
  dirtyWalls_.insert(ch.walls.begin(), ch.walls.end()); if (!ch.cocoons.empty()) ringsDirty_ = true;
  // converted meshes: the change baked into their data (what the file keeps)
  for (int id : gd_.meshes) {
    Item* it = scene_->item(id); QJsonObject d = it->raw.value("data").toObject(); QJsonArray P;
    for (auto& p : it->poly->pos) P.append(QJsonArray{p.x, p.y, p.z}); d.insert("pos", P); it->raw.insert("data", d);
  }
  if (gz_.mode == Gizmo::Mode::Rotate) { rotateOwnAxes(gz_.rotationSoFar());
    for (int id : sel_.items) if (const Item* x = scene_->item(id); x && x->type == ItemType::IMesh) turnGroupsUnder(id, gz_.rotationSoFar()); }   // (the groups under a turned object: their axes turn too)
  bool weldOne = gz_.mode == Gizmo::Mode::Move && gd_.list.size() == 1 && gd_.meshes.empty() && !sel_.anchors.empty();
  int a = weldOne ? gd_.list[0] : -1;
  weldCand_ = -1; gd_ = {};
  commit(false);
  updateGizmo();
  if (a >= 0) checkWeld(a, m);
}

// Ctrl + a gizmo handle: the selection extruded where it is — polygons: the region lifted off (its own vertices), walls
// round its border; edges: each pulled out into a polygon; the new part selected, the gizmo moving it (one undo step
// with the move; let go unmoved — nothing made)
bool Viewport::tExtrudeSel(int m) {
  Item* it = scene_->item(m); if (!it || tsel_.id != m) return false;
  const PolyMesh& B = *baseSp(*it); auto T = topo::ttopo(baseSp(*it)); tedit::MeshEd E = tedit::MeshEd::from(B);
  const char key = tKey();
  if (key == 'f' && !tsel_.f.empty()) {
    std::set<int> S(tsel_.f.begin(), tsel_.f.end()); std::map<std::pair<int, int>, int> uses;   // (undirected edge → how many region polygons use it)
    for (int f : S) { const auto& V = E.f[size_t(f)].v; for (size_t k = 0; k < V.size(); k++) { int a = V[k], b = V[(k + 1) % V.size()]; uses[{std::min(a, b), std::max(a, b)}]++; } }
    std::vector<std::pair<int, int>> rim;                      // (the region's border, as its polygons run it)
    for (int f : S) { const auto& V = E.f[size_t(f)].v; for (size_t k = 0; k < V.size(); k++) { int a = V[k], b = V[(k + 1) % V.size()]; if (uses[{std::min(a, b), std::max(a, b)}] == 1) rim.push_back({a, b}); } }
    std::map<int, int> nv; for (int f : S) for (int& v : E.f[size_t(f)].v) { auto q = nv.find(v); if (q == nv.end()) q = nv.emplace(v, E.addVert(E.pos[size_t(v)])).first; v = q->second; }
    for (auto [a, b] : rim) { const int g = E.f[size_t(*S.begin())].g, mt = E.f[size_t(*S.begin())].mat; E.addFace({nv[b], nv[a], a, b}, g, mt); }
    ttDone(m, E, B, {}, 'f', {S.begin(), S.end()}, {}, false);
    ui::hud(gz_.mode == Gizmo::Mode::Scale ? tr("Inset: %1 polygon%2 — scale it").arg(S.size()).arg(S.size() == 1 ? "" : "s") : tr("Extruded: %1 polygon%2 — move it").arg(S.size()).arg(S.size() == 1 ? "" : "s")); return true;
  }
  if (key == 'e' && !tsel_.e.empty()) {
    std::map<int, int> nv; std::vector<std::pair<int, int>> ne;
    auto dup = [&](int v) { auto q = nv.find(v); if (q == nv.end()) q = nv.emplace(v, E.addVert(E.pos[size_t(v)])).first; return q->second; };
    for (int e : tsel_.e) { const int a = T->ea[size_t(e)], b = T->eb[size_t(e)]; const int na = dup(a), nb = dup(b);
      bool ab = false; const int f0 = T->ef0[size_t(e)];               // (the polygon on it runs a → b: the new one runs it b → a)
      if (f0 >= 0) { const auto& V = E.f[size_t(f0)].v; for (size_t k = 0; k < V.size(); k++) if (V[k] == a && V[(k + 1) % V.size()] == b) ab = true; }
      E.addFace(ab ? std::vector<int>{b, a, na, nb} : std::vector<int>{a, b, nb, na}); ne.push_back({na, nb}); }
    ttDone(m, E, B, {}, 'e', {}, ne, false);
    ui::hud(tr("Extruded: %1 edge%2 — move them").arg(ne.size()).arg(ne.size() == 1 ? "" : "s")); return true;
  }
  ui::toast(tr("Ctrl + drag extrudes polygons or edges — select some")); return false;
}

}  // namespace hm

namespace hm {
// a group's own point put at p (the Settings' Position, ⌖ To the head's centre); carry — what is inside goes along (as Move
// does it), else only the point moves (the symmetry inside then mirrors across it). One undo step
void Viewport::setGroupPoint(int gid, const Vec3& p, bool carry) {
  Item* g = scene_->item(gid); if (!g || g->type != ItemType::Group) return;
  const QJsonArray a0 = g->raw.value("gpivot").toArray(); const Vec3 from = a0.size() == 3 ? Vec3(a0[0].toDouble(), a0[1].toDouble(), a0[2].toDouble()) : Vec3();
  const Vec3 d = p - from; if (d.lengthSq() < 1e-24 && a0.size() == 3) return;
  stepLabel_ = carry ? tr("Group moved: %1").arg(g->name) : tr("Group point moved: %1").arg(g->name);   // (the history's name for the step)
  g->raw.insert("gpivot", QJsonArray{p.x, p.y, p.z});
  if (carry && d.lengthSq() > 0) {
    const Selection keep = sel_; sel_.items = {gid}; sel_.anchors.clear(); auto tg = gizmoTargets(); sel_ = keep;
    if (tg) {
      for (int a : tg->anchors) scene_->anchor(a)->pos += d;
      if (!tg->anchors.empty()) markCurves(curvesTouching(*scene_, tg->anchors));
      for (int id : tg->meshes) { Item* it = scene_->item(id); for (auto& q : it->poly->pos) q += d;
        QJsonObject dd = it->raw.value("data").toObject(); QJsonArray P; for (auto& q : it->poly->pos) P.append(QJsonArray{q.x, q.y, q.z}); dd.insert("pos", P); it->raw.insert("data", dd); }
      if (!tg->meshes.empty()) staticRev_++;
      Affine T; T.t = d; for (int id : tg->imeshes) meshmods::setMeshMatrix(*scene_, id, Affine::mul(T, meshmods::meshMatrix(*scene_, *scene_->item(id))));
      for (int id : tg->groups) if (id != gid) if (Item* gi = scene_->item(id)) { const QJsonArray a = gi->raw.value("gpivot").toArray(); const Vec3 q = Vec3(a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble()) + d; gi->raw.insert("gpivot", QJsonArray{q.x, q.y, q.z}); }
    }
  }
  scene_->revision++; beforeFrame();
  HairChanges ch = hair_.finishDraft(*scene_);
  dirtyWalls_.insert(ch.walls.begin(), ch.walls.end()); if (!ch.cocoons.empty()) ringsDirty_ = true;
  allWalls_ = true; commit(false); updateGizmo(); emit sceneEdited();
}
}  // namespace hm
