// ▬ Flat locks in the viewport: the ribbons kept in step with their curves and settings, the root of a lock started from
// anchors following them, the Extrude / Offset / Glue brush; and the drawn curve kept off collision meshes (anchors in
// between, over the mesh).
#include "scene/Rib.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonDocument>
#include <QMouseEvent>
#include <cmath>

namespace hm {

Vec3 Viewport::camDir() const { return cam_.dir(); }

std::vector<int> Viewport::ribCurvesSel() const {
  std::vector<int> out;
  for (const Item* c : scene_->curves()) {
    if (!rib::isRib(*c) || rib::isVolume(*scene_, *c) || !ribs_.count(c->id)) continue;
    bool on = sel_.hasItem(c->id); for (int a : c->anchors) if (sel_.hasAnchor(a)) on = true;
    if (on) out.push_back(c->id);
  }
  return out;
}
bool Viewport::ribBrushOn() const { return ribBrush.on && tool_ != "draw" && !ribCurvesSel().empty(); }

// the root of a lock started from anchors follows their centre
void Viewport::ribFollow() {
  std::vector<int> moved;
  for (auto& [id, c] : scene_->items) if (c.type == ItemType::Curve && rib::isRib(c) && rib::followEnds(*scene_, c)) moved.push_back(c.anchors[0]);
  if (!moved.empty()) { auto t = curvesTouching(*scene_, moved); pendingCurves_.insert(t.begin(), t.end()); }
}

void Viewport::syncRibs() {
  if (overlayRev_ == ribsSeen_) return;
  ribsSeen_ = overlayRev_;
  bool changed = false; std::set<int> alive;
  auto selRibs = ribCurvesSel(); bool brush = ribBrushOn();
  const Vec3 cd = camDir(); const double scale = cam_.sceneScale;
  for (auto& [id, c] : scene_->items) {
    if (c.type != ItemType::Curve || !rib::isRib(c) || !scene_->isVisible(c)) continue;
    auto ci = hair_.curves.find(id); if (ci == hair_.curves.end() || ci->second.p.size() < 2) continue;
    alive.insert(id);
    QString kind = brush && std::find(selRibs.begin(), selRibs.end(), id) != selRibs.end() ? ribBrush.kind : QString();
    QByteArray s = QJsonDocument(c.raw.value("rib").toObject()).toJson(QJsonDocument::Compact) + kind.toUtf8();
    for (int a : c.anchors) { const Vec3& p = scene_->anchor(a)->pos; s.append(reinterpret_cast<const char*>(&p), sizeof p); }
    s.append(reinterpret_cast<const char*>(ci->second.p.data()), qsizetype(ci->second.p.size() * sizeof(Vec3)));
    for (int e : rib::ribEnds(*scene_, c)) { const Vec3& p = scene_->anchor(e)->pos; s.append(reinterpret_cast<const char*>(&p), sizeof p); }
    if (!c.raw.value("rib").toObject().value("view").isArray()) s.append(reinterpret_cast<const char*>(&cd), sizeof cd);
    auto f = ribSig_.find(id); if (f != ribSig_.end() && f->second == s && ribs_.count(id)) continue;
    ribSig_[id] = s; changed = true;
    auto sh = rib::ribShape(*scene_, hair_.curves, c, cd, scale);
    if (!sh) { ribs_.erase(id); ribShapes_.erase(id); continue; }
    RibDraw d; d.rings = sh->rings; d.tube = sh->tube; d.pointTip = sh->F.back().w <= scale * 1e-4;
    if (!sh->tube || sh->extruded) d.col = rib::ribColors(c, *sh, kind);
    ribs_[id] = std::move(d); ribShapes_[id] = std::move(*sh);
  }
  for (auto it = ribs_.begin(); it != ribs_.end();) { if (!alive.count(it->first)) { ribSig_.erase(it->first); ribShapes_.erase(it->first); it = ribs_.erase(it); changed = true; } else ++it; }
  if (changed) ribsRev_++;
}

// the brush on the selected flat locks: Extrude / Glue — Ctrl erase; Offset — Ctrl inverted, Ctrl+Alt erase; Shift smooth
bool Viewport::ribPress(QMouseEvent* e) {
  bool alt = e->modifiers() & Qt::AltModifier, ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  if (e->button() != Qt::LeftButton || !ribBrushOn() || (alt && !(ctrl && ribBrush.kind == "off"))) return false;
  bool ro = ribBrush.kind == "off";
  QString op = shift ? "smooth" : !ro ? (ctrl ? "erase" : "paint") : ctrl ? (alt ? "erase" : "invert") : "paint";
  QPointF m = e->position();
  md_ = {}; md_.op = op; md_.last = m; md_.rib = true; drag_ = Drag::Mask;
  paintRibNow(m, op, 0.2);
  return true;
}
bool Viewport::paintRibNow(const QPointF& m, const QString& op, double amt) {
  auto locks = ribCurvesSel(); std::vector<rib::Shape*> shapes; for (int c : locks) { auto f = ribShapes_.find(c); shapes.push_back(f == ribShapes_.end() ? nullptr : &f->second); }
  Projector P = projector();
  auto w = [&](const Vec3& p) { ScreenPt s = P.toScreen(p); if (s.z > 1) return -1.0; double d = std::hypot(s.x - m.x(), s.y - m.y()); return d < brushR ? brushWeight(d) : -1.0; };
  bool ch = rib::paintRib(*scene_, hair_.curves, locks, shapes, ribBrush.kind, op, amt, ribBrush.strength, ribBrush.intensity, w);
  if (ch) { md_.touched.insert(locks.begin(), locks.end()); liveRibs(); }
  return ch;
}

void Viewport::ribConvert(int c) {
  Vec3 cd = camDir(); double scale = cam_.sceneScale;
  runEdit([c, cd, scale](Scene& s, Selection& sl, const CurveCache& cc) { return rib::convert(s, cc, sl, c, cd, scale); }, true);
}

// the straight way from the last anchor to a new one crosses a collision mesh: anchors go in between, lifted onto the
// mesh's outer side (towards the camera)
std::vector<Vec3> Viewport::collideBetween(const Vec3& a, const Vec3& b, int depth) const {
  std::map<int, double> off; const std::vector<PickMesh> ms = collisionMeshes(false, off);   // (the meshes with ⛨ on — their collision shapes)
  const double scale = cam_.sceneScale;
  if (ms.empty() || depth > 3) return {};
  Vec3 d = b - a; double L = d.length(); if (L < scale * 1e-3) return {};
  auto h = raycast({a, d / L}, ms); if (!h || h->t > L) return {};
  Vec3 mid = (a + b) * 0.5, cd = camDir();
  auto s = raycast({mid - cd * (scale * 4), cd}, ms); if (!s) return {};
  Vec3 n = s->normal; if (n.dot(cd) > 0) n = -n;
  Vec3 p = s->point + n * (off.count(s->itemId) ? off[s->itemId] : 0.2);
  auto A = collideBetween(a, p, depth + 1), B = collideBetween(p, b, depth + 1);
  A.push_back(p); A.insert(A.end(), B.begin(), B.end()); return A;
}

}  // namespace hm
