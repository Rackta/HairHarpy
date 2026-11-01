// ◠ The crown in the viewport: its mesh kept in step with the connectors, crown curves and the lock curves / walls
// attached to it; symmetric crowns and attached anchors kept so every frame; Ctrl + drag slides an anchor along its
// connector; a lock curve's end dropped near a connector — attach it?; the Crown tool's click places a crown.
#include "scene/Braid.h"
#include "scene/Crown.h"
#include "scene/Edit.h"
#include "scene/Topo.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMouseEvent>
#include <cmath>

namespace hm {

namespace {
const CurveSampling* smp(const CurveCache& cc, int id) { auto it = cc.find(id); return it == cc.end() || it->second.p.size() < 2 ? nullptr : &it->second; }
std::vector<PickMesh> headMeshes(const Scene& sc) {
  std::vector<PickMesh> out;
  for (auto& [id, it] : sc.items) if (it.headTag && it.poly && sc.isVisible(it)) {
    PickMesh pm; pm.itemId = id; pm.pos = &it.poly->pos; pm.box = it.poly->bounds();
    for (int f = 0; f < it.poly->faceCount(); f++) { int s = it.poly->faceStart[f], e = it.poly->faceStart[f + 1]; for (int q = s + 1; q < e - 1; q++) { pm.tris.push_back(it.poly->faceVerts[s]); pm.tris.push_back(it.poly->faceVerts[q]); pm.tris.push_back(it.poly->faceVerts[q + 1]); } }
    out.push_back(std::move(pm));
  }
  return out;
}
}  // namespace

crown::HeadHit Viewport::headHitFn() const {
  // (through each head's BVH — the crown and its continuation cast thousands of rays: one by one over a sculpted head's
  // ~900 000 triangles that was minutes)
  auto bvhs = std::make_shared<std::vector<std::shared_ptr<const topo::Bvh>>>();
  for (auto& [id, it] : scene_->items) if (it.headTag && it.poly && scene_->isVisible(it)) if (auto b = bvhOf(id, false)) bvhs->push_back(b);
  double far = cam_.sceneScale * 20;
  auto cast = [bvhs](const Vec3& o, const Vec3& n) -> std::optional<Vec3> {
    double bt = 1e300; for (const auto& b : *bvhs) if (auto h = b->ray(o, n, bt)) bt = h->first;
    if (bt >= 1e300) return std::nullopt; return o + n * bt;
  };
  return [bvhs, far, cast](const Vec3& o, const Vec3& d, bool outermost) -> std::optional<Vec3> {
    if (bvhs->empty()) return std::nullopt;
    Vec3 n = d.normalized();
    if (outermost) return cast(o + n * far, -n);   // (the last hit: from the far end back)
    return cast(o, n);
  };
}

void Viewport::createCrownAt(const std::optional<QPointF>& m) {
  // (no mesh marked as the head: the crown would be fitted round the whole model — a body, a scene — far too big)
  bool head = false; for (auto& [id, it] : scene_->items) if ((it.headTag || it.type == ItemType::Head) && it.poly) head = true;
  if (!head) { ui::toast(tr("Mark the head first: select the head mesh — ☺ Head in its settings. The crown is fitted on the head")); return; }
  std::optional<Vec3> target, normal;
  if (m) {
    auto hm = headMeshes(*scene_); auto h = hm.empty() ? std::nullopt : raycast(ray(*m), hm);
    if (!h) { ui::toast(tr("Click on the head (or press Create in the panel)")); return; }
    target = h->point; normal = h->normal;
  }
  auto hit = headHitFn(); Vec3 cen = cam_.sceneCenter; double scale = cam_.sceneScale; bool sym = crownSym, bangs = crownBangs;
  // (the head's collision a capsule — placed by hand or fitted: the crown from its top)
  std::vector<Vec3> coll;
  for (auto& [id, it] : scene_->items) if ((it.headTag || it.type == ItemType::Head) && it.poly && scene_->isVisible(it) && collType(it) == "capsule") {
    if (const int cap = capsuleItem(id); cap >= 0) coll = capsuleWorld(cap); else coll = capsuleOf(id).pos;
    if (!coll.empty()) break;
  }
  int made = -1;
  ui::ThinkScope th(crownCont ? tr("Making the crown and its continuation — fitted on the head") : tr("Making the crown — fitted on the head"), 0); th.now();   // (on a heavy head — seconds)
  runEdit([&](Scene& s, Selection& sl, const CurveCache&) { QString msg; made = crown::createCrown(s, target, normal, hit, cen, scale, sym, sl, &msg, coll.empty() ? nullptr : &coll, bangs); return msg; }, true);
  if (crownCont && made >= 0 && scene_->item(made)) crownContinue(made);   // (made with its continuation)
}

void Viewport::crownContinue(int k) {
  const Item* ki = scene_->item(k); if (!ki) return;
  const double len = ki->raw.value("p").toObject().value("contLen").toDouble(2.0); auto hit = headHitFn();
  ui::ThinkScope th(tr("Making the crown's continuation — its walls straight down from the lower connector"), 0); th.now();
  runEdit([&](Scene& s, Selection& sl, const CurveCache& cc) { return crown::addContinuation(s, cc, k, len, hit, sl); }, true);
}

void Viewport::crownSymAsk(int k, const QPoint& global) {
  ui::PieSpec s; s.title = tr("Make the crown symmetric");
  double cx = cam_.sceneCenter.x, scale = cam_.sceneScale;
  for (int master : {1, -1}) {
    ui::PieItem it; it.label = master > 0 ? tr("⇋ Left (+X) → Right (−X)") : tr("⇋ Right (−X) → Left (+X)");
    it.act = [this, k, master, cx, scale] { runEdit([&](Scene& sc, Selection& sl, const CurveCache& cc) { CurveCache c2 = cc; return crown::makeSymmetric(sc, c2, k, master, cx, scale, &sl); }, true); };
    s.top.push_back(it);
  }
  ui::PieItem no; no.label = tr("Cancel"); no.act = [this] { emit brushChanged(); }; s.bottom.push_back(no);
  ui::Pie::open(pieHost_, global, s, false);
}

// the built crowns: rebuilt where their connectors, crown curves, the lock curves and walls on them changed
void Viewport::syncCrowns() {
  if (overlayRev_ == crownsSeen_) return;
  crownsSeen_ = overlayRev_;
  bool changed = false; std::set<int> alive;
  for (int kid : crown::crowns(*scene_)) {
    const Item* k = scene_->item(kid);
    const Item *part = scene_->item(k->raw.value("part").toInt(-1)), *low = scene_->item(k->raw.value("low").toInt(-1)); if (!part || !low) continue;
    alive.insert(kid);
    QByteArray s = QJsonDocument(k->raw).toJson(QJsonDocument::Compact);
    for (const Item* c : scene_->curves()) {
      bool rel = c->id == part->id || c->id == low->id || c->parent == k->parent;
      if (!rel && !c->anchors.empty()) for (int a : {c->anchors.front(), c->anchors.back()}) if (crown::onConnector(*scene_, a, *low) || crown::onConnector(*scene_, a, *part)) rel = true;
      if (!rel) continue;
      s.append(QByteArray::number(c->id)); if (auto S = smp(hair_.curves, c->id)) s.append(reinterpret_cast<const char*>(S->p.data()), qsizetype(S->p.size() * sizeof(Vec3)));
      for (auto& [sid, sf] : scene_->items) if (sf.type == ItemType::Surface && (sf.a == c->id || sf.b == c->id)) s.append(QJsonDocument(sf.raw.value("p").toObject()).toJson(QJsonDocument::Compact));
    }
    auto f = crownSig_.find(kid);
    if (f != crownSig_.end() && f->second == s) continue;
    crownSig_[kid] = s; changed = true;
    auto m = crown::crownData(*scene_, hair_.curves, *k);
    if (m) crowns_[kid] = std::move(*m); else crowns_.erase(kid);
  }
  for (auto it = crowns_.begin(); it != crowns_.end();) { if (!alive.count(it->first)) { crownSig_.erase(it->first); it = crowns_.erase(it); changed = true; } else ++it; }
  for (auto it = crownSig_.begin(); it != crownSig_.end();) { if (!alive.count(it->first)) it = crownSig_.erase(it); else ++it; }
  if (changed) crownsRev_++;
}

// every frame: a symmetric crown's moved anchor moves its pair; attached anchors follow their connector
void Viewport::crownFrame() {
  { auto mb = braid::syncConnectors(*scene_, hair_.curves, cam_.sceneCenter);   // (the braids' end rings carried along with their ends)
    if (!mb.empty()) { auto t = curvesTouching(*scene_, mb); pendingCurves_.insert(t.begin(), t.end()); } }
  if (crown::crowns(*scene_).empty() && attLast_.empty()) {
    bool any = false; for (auto& [id, a] : scene_->anchors) if (a.raw.value("at").isObject()) { any = true; break; }
    if (!any) return;
  }
  auto m1 = crown::symSync(*scene_, crownSymLast_, cam_.sceneCenter.x);
  if (!m1.empty()) { auto t = curvesTouching(*scene_, m1); pendingCurves_.insert(t.begin(), t.end()); }
  QString msg;
  auto m2 = crown::updateAttachments(*scene_, hair_.curves, attLast_, cam_.sceneScale, &msg);
  if (!m2.empty()) { auto t = curvesTouching(*scene_, m2); pendingCurves_.insert(t.begin(), t.end()); }
  if (!msg.isEmpty()) ui::toast(msg);
}

bool Viewport::crownPress(QMouseEvent* e, int a) {
  bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  if (a >= 0 && ctrl) {                                      // Ctrl + drag on an anchor sitting on a connector: it slides along it
    const Anchor* A = scene_->anchor(a); int on = -1;
    if (A->raw.value("at").isObject()) on = A->raw.value("at").toObject().value("c").toInt(-1);
    else for (int c : edit::curvesWith(*scene_, a)) if (!scene_->item(c)->conn.isEmpty()) { on = c; break; }
    if (auto S = on >= 0 ? smp(hair_.curves, on) : nullptr) { sl_ = {}; sl_.a = a; sl_.poly = S->p; drag_ = Drag::Slide; return true; }
  }
  // an old-style attached anchor of a lock curve (not a crown curve): a plain drag makes it an anchor of its connector first
  if (a >= 0 && !ctrl && !shift && scene_->anchor(a)->raw.value("at").isObject()) {
    int h = scene_->anchor(a)->raw.value("at").toObject().value("c").toInt(-1); const Item* hi = scene_->item(h);
    bool crownC = false; for (int k : crown::crowns(*scene_)) for (auto& r : crown::crownCurves(*scene_, hair_.curves, *scene_->item(k))) for (int c : edit::curvesWith(*scene_, a)) if (c == r.c) crownC = true;
    if (hi && !hi->conn.isEmpty() && !crownC) { runEdit([&](Scene& s, Selection&, const CurveCache& cc) { crown::insertIntoConnector(s, cc, h, a); return QString(); }, true); }
    return false;
  }
  if (a < 0 && tool_ == "crown" && !ctrl && e->button() == Qt::LeftButton) { drag_ = Drag::CrownPlace; return true; }
  return false;
}

void Viewport::slideMove(const QPointF& m) {
  Projector P = projector(); const auto& Q = sl_.poly; std::optional<Vec3> best; double bd = 1e300, bl = 0, acc = 0;
  for (size_t i = 0; i + 1 < Q.size(); i++) {
    ScreenPt a = P.toScreen(Q[i]), b = P.toScreen(Q[i + 1]); double seg = Q[i].distanceTo(Q[i + 1]);
    if (a.z <= 1 && b.z <= 1) {
      double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy, t = l2 > 0 ? clamp(((m.x() - a.x) * vx + (m.y() - a.y) * vy) / l2, 0, 1) : 0;
      double dd = std::hypot(a.x + vx * t - m.x(), a.y + vy * t - m.y());
      if (dd < bd) { bd = dd; best = Q[i].lerp(Q[i + 1], t); bl = acc + seg * t; }
    }
    acc += seg;
  }
  if (!best) return;
  Anchor* A = scene_->anchor(sl_.a); A->pos = *best;
  if (A->raw.value("at").isObject()) { QJsonObject at = A->raw.value("at").toObject(); at.insert("t", bl / (acc ? acc : 1)); A->raw.insert("at", at); }
  attLast_[sl_.a] = *best;
  sl_.moved = true; scene_->revision++;
  markCurves(curvesTouching(*scene_, {sl_.a}));
}

// after an anchor END was dragged near a connector (and not near another anchor): offer to attach it
void Viewport::checkAttach(int a, const QPointF& m) {
  if (!crown::attachable(*scene_, a)) return;
  Projector P = projector(); ScreenPt s = P.toScreen(scene_->anchor(a)->pos);
  int best = -1; double bd = 7;
  for (const Item* c : scene_->curves()) {
    if (c->conn.isEmpty() || std::find(c->anchors.begin(), c->anchors.end(), a) != c->anchors.end() || !curveShown(*scene_, ds_, sel_, *c)) continue;
    auto S = smp(hair_.curves, c->id); if (!S) continue;
    if (auto h = nearestOnScreen(P, S->p, s.x, s.y); h && h->d < bd) { bd = h->d; best = c->id; }
  }
  if (best < 0) return;
  QMenu menu; menu.setTitle(tr("Near a crown connector"));
  QAction* at = menu.addAction(tr("⛓ Attach to %1").arg(scene_->item(best)->name)); menu.addSeparator(); menu.addAction(tr("Don't attach"));
  QAction* got = weldChoice.isEmpty() ? menu.exec(toGlobal(m)) : (weldChoice == "attach" ? at : nullptr);
  if (got != at) return;
  runEdit([&](Scene& sc, Selection&, const CurveCache& cc) { return crown::connectToConnector(sc, cc, a, best); }, true);
}

// a curve just drawn: an end put on a crown connector (within a few pixels of it on the screen) becomes part of it —
// on one of its anchors that anchor is taken (shared), else the end goes into the connector there. A lock then grows
// out of the crown: its walls' roots lie on the connector and the crown takes their columns
QString Viewport::autoAttachEnds(int curve) {
  Item* c = scene_->item(curve); if (!c || c->anchors.size() < 2 || !c->conn.isEmpty() || c->ring >= 0) return {};
  Projector P = projector(); int n = 0;
  for (int end = 0; end < 2; end++) {
    const int a = end ? c->anchors.back() : c->anchors.front(); if (!crown::attachable(*scene_, a)) continue;
    const ScreenPt s = P.toScreen(scene_->anchor(a)->pos); if (s.z >= 1) continue;
    int bestC = -1, bestA = -1; double bd = 9, ba = 10;
    for (const Item* k : scene_->curves()) {
      if (k->conn.isEmpty() || k->id == curve || !curveShown(*scene_, ds_, sel_, *k)) continue;
      for (int x : k->anchors) { const ScreenPt q = P.toScreen(scene_->anchor(x)->pos); const double d = std::hypot(q.x - s.x, q.y - s.y); if (q.z < 1 && d < ba) { ba = d; bestA = x; } }
      auto S = smp(hair_.curves, k->id); if (!S) continue;
      if (auto h = nearestOnScreen(P, S->p, s.x, s.y); h && h->d < bd) { bd = h->d; bestC = k->id; }
    }
    if (bestA >= 0 && bestA != a && std::find(c->anchors.begin(), c->anchors.end(), bestA) == c->anchors.end()) {   // (on a connector's anchor: that one shared)
      (end ? c->anchors.back() : c->anchors.front()) = bestA; scene_->anchors.erase(a); n++; continue; }
    if (bestC >= 0) { crown::connectToConnector(*scene_, hair_.curves, a, bestC); n++; }
  }
  if (!n) return {};
  scene_->reindex(); scene_->revision++;
  return n == 2 ? tr("Both ends attached to the crown's connectors") : tr("Attached to the crown's connector — the lock grows out of the crown");
}

}  // namespace hm
