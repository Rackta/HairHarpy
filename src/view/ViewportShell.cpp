// ◍ Shell modifiers — the 'guide' tool: what shapes a cocoon's rings. Topology: Density and Parent influence, painted
// along a cocoon (m.cmask — CMASK_N weights root → tip; Density's second mask m.emask — even spacing, Ctrl+Shift).
// Guidelines: a line dragged across a cocoon (a ring of it lies in the line's plane; g.on — kept in the cocoon's frame,
// it follows the cocoon) and NURBS discs (two closed ring curves; on a cocoon, or in the scene through every cocoon).
#include "scene/Edit.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QDateTime>
#include <QTimer>
#include <QJsonArray>
#include <QMouseEvent>
#include <cmath>

namespace hm {

namespace {
const double PI = 3.14159265358979323846;
const Item* parentOf(const Scene& sc, const Item* it) { return it && it->parent >= 0 ? sc.item(it->parent) : nullptr; }
const Item* modCocoon(const Scene& sc, const Item* m) { const Item* p = parentOf(sc, m); return p && p->type == ItemType::Cocoon ? p : nullptr; }
int jsRound(double x) { return int(std::floor(x + 0.5)); }
double nearestF(const std::vector<Vec3>& P, const Vec3& p) {
  double best = 0, bd = 1e300;
  for (size_t i = 0; i + 1 < P.size(); i++) { Vec3 a = P[i], b = P[i + 1] - a; double l2 = b.lengthSq(), s = l2 > 0 ? std::clamp((p - a).dot(b) / l2, 0.0, 1.0) : 0, d = (a + b * s - p).lengthSq(); if (d < bd) { bd = d; best = double(i) + s; } }
  return best;
}
Vec3 atF(const std::vector<Vec3>& C, double f) { int i = std::clamp(int(std::floor(f)), 0, int(C.size()) - 2); return C[size_t(i)].lerp(C[size_t(i + 1)], std::clamp(f - i, 0.0, 1.0)); }
std::vector<double> maskOf(const QJsonValue& v) { std::vector<double> o; for (const QJsonValue& x : v.toArray()) o.push_back(x.toDouble()); return o; }
double cmaskAt(const std::vector<double>& M, double f) {
  if (M.size() < 2) return 0; double x = std::clamp(f, 0.0, 1.0) * (CMASK_N - 1); int i = std::min(CMASK_N - 2, int(std::floor(x)));
  return M[size_t(i)] + (M[size_t(i + 1)] - M[size_t(i)]) * (x - i);
}
std::pair<Vec3, Vec3> discBasis(const Vec3& N) {
  Vec3 u = std::abs(N.y) > 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0); u = (u - N * u.dot(N)).normalized(); return {u, N.cross(u)};
}
const QString NO_COCOON = QObject::tr("Shell modifiers go on a cocoon: select a cocoon (or its lock) — no cocoon yet? Make one with ◍ Cocoon");
}  // namespace

bool Viewport::shellBrushActive() const {
  if (tool_ != "guide") return false;
  auto has = [&](ItemType t) { for (auto& [id, it] : scene_->items) if (it.type == t && modCocoon(*scene_, &it)) return true; return false; };
  return (shTab == "tip" && tipBrush.on && has(ItemType::Tip)) || (shTab == "pinf" && pinfBrush.on && has(ItemType::PInf));
}

// the cocoons a shell modifier goes on: the selected cocoons; else the cocoon of a selected shell modifier; else the
// cocoons of the selected curves (their strands) or of the selected groups (their lock)
std::vector<int> Viewport::shellTargets() const {
  std::vector<int> out; auto add = [&](int k) { if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k); };
  for (int id : sel_.items) if (const Item* it = scene_->item(id)) { if (it->type == ItemType::Cocoon) add(id); else if (const Item* k = modCocoon(*scene_, it)) add(k->id); }
  if (!out.empty()) return out;
  CocoonSystem* cs = const_cast<HairState&>(hair_).cocoonSystem(); if (!cs) return out;
  std::vector<const Item*> ks; for (auto& [id, it] : scene_->items) if (it.type == ItemType::Cocoon) ks.push_back(&it);
  std::sort(ks.begin(), ks.end(), [](auto* a, auto* b) { return a->order < b->order; });
  for (int id : sel_.items) for (const Item* k : ks) { auto c = cs->strandCurves(k->id); if (std::find(c.begin(), c.end(), id) != c.end()) add(k->id); }
  if (!out.empty()) return out;
  for (int id : sel_.items) for (const Item* k : ks) {
    int g = cs->lockGroup(k->id); const Item* p = parentOf(*scene_, k);
    if (g >= 0 && (g == id || scene_->isDescendant(id, g)) && !(p && p->type == ItemType::Cocoon)) add(k->id);
  }
  return out;
}

void Viewport::touchShellMods(const std::vector<int>& mods) {
  std::set<int> ks; bool all = false;
  for (int id : mods) if (const Item* m = scene_->item(id)) { if (const Item* k = modCocoon(*scene_, m)) ks.insert(k->id); else if (m->type == ItemType::Guide && m->raw.value("kind").toString() == "disc") all = true; }
  if (all) for (auto& [id, it] : scene_->items) if (it.type == ItemType::Cocoon) ks.insert(id);
  if (!ks.empty()) liveCocoons(ks);
  overlayRev_++;
}

// one Density (or Parent influence) per cocoon: added to the targeted cocoons (one there already — its setting is set
// again, its mask stays)
std::vector<int> Viewport::addShellMods(const QString& type, const QJsonObject& def, const QString& name) {
  auto ks = shellTargets(); if (ks.empty()) { ui::toast(NO_COCOON); return {}; }
  std::vector<int> made; ItemType t = itemTypeFromString(type);
  runEdit([&](Scene& s, Selection& sel, const CurveCache&) {
    for (int kid : ks) {
      Item* k = s.item(kid); int old = -1;
      for (int c : k->children) if (const Item* x = s.item(c); x && x->type == t) { old = c; break; }
      if (old >= 0) { Item* o = s.item(old); QJsonObject p = o->raw.value("p").toObject(); for (auto it = def.begin(); it != def.end(); ++it) p.insert(it.key(), it.value()); o->raw.insert("p", p); made.push_back(old); continue; }
      edit::nextNumber(s, t);
      Item& m = s.addItem(t, kid, QString("%1 (%2)").arg(name, k->name)); m.raw = QJsonObject{{"p", def}};
      s.item(kid)->expanded = true; made.push_back(m.id);
    }
    sel.anchors.clear(); sel.items = made; return QString();
  }, false);
  touchShellMods(made);
  return made;
}

void Viewport::clearMasks(const std::vector<int>& mods) {
  auto has = [](const QJsonValue& v) { for (const QJsonValue& x : v.toArray()) if (std::abs(x.toDouble()) > 1e-4) return true; return false; };
  std::vector<int> ks; for (int id : mods) if (const Item* m = scene_->item(id); m && (has(m->raw.value("cmask")) || has(m->raw.value("emask")) || (m->raw.value("mask").isObject() && !m->raw.value("mask").toObject().isEmpty()))) ks.push_back(id);
  if (ks.empty()) { ui::toast(tr("No mask to clear")); return; }
  runEdit([ks](Scene& s, Selection&, const CurveCache&) { for (int id : ks) { Item* m = s.item(id); m->raw.remove("mask"); m->raw.remove("cmask"); m->raw.remove("emask"); } return QString(); }, false);
  touchShellMods(ks);
  ui::toast(ks.size() > 1 ? tr("%1 masks cleared").arg(ks.size()) : tr("Mask cleared"));
}

QJsonObject Viewport::guideDefaultsNow() const {
  QJsonObject d{{"density", 1}, {"radius", QJsonValue()}, {"blend", 1}, {"local", true}, {"priority", 0}};
  QJsonObject st = scene_->fileRaw.value("settings").toObject().value("guideDefaults").toObject();
  for (auto it = st.begin(); it != st.end(); ++it) d.insert(it.key(), it.value());
  if (!d.value("radius").isDouble()) {
    Box3 hb; for (auto& [id, it] : scene_->items) if (it.headTag && it.poly) hb.unite(it.poly->bounds());
    double headH = hb.empty() ? scene_->bounds().size().y : hb.size().y; if (!(headH > 0)) headH = 1;
    double step = convValue("step").toDouble(projectStep(*scene_));
    d.insert("radius", std::clamp(4 * step / headH, 0.002, 0.5));
  }
  return d;
}
void Viewport::setGuideDefaults(const QJsonObject& d) {
  QJsonObject S = scene_->fileRaw.value("settings").toObject(); S.insert("guideDefaults", d); scene_->fileRaw.insert("settings", S);
}

// ---- the mask brushes ----
// a mask slot is under the brush where the cocoon is there on the screen (its axis or one of 4 points round its shell);
// op: paint / erase / smooth; the selected modifiers of that kind, else those on the targeted cocoons, else all
bool Viewport::paintCocoonMask(const QPointF& m, const QString& op, double amt, const QString& type, double strength, const QString& field) {
  ItemType t = itemTypeFromString(type); CocoonSystem* cs = hair_.cocoonSystem(); if (!cs) return false;
  std::vector<int> all, sel; for (auto& [id, it] : scene_->items) if (it.type == t && modCocoon(*scene_, &it)) { all.push_back(id); if (sel_.hasItem(id)) sel.push_back(id); }
  auto ks = shellTargets(); std::vector<int> list = sel;
  if (list.empty()) { if (!ks.empty()) { for (int id : all) if (std::find(ks.begin(), ks.end(), modCocoon(*scene_, scene_->item(id))->id) != ks.end()) list.push_back(id); } else list = all; }
  Projector P = projector(); bool changed = false;
  for (int mid : list) {
    Item* md = scene_->item(mid); const Item* k = modCocoon(*scene_, md);
    CocoonGeo G = cs->geo(k->id); if (G.N < 2) continue;
    if (op == "smooth" && !md->raw.value(field).isArray()) continue;           // (nothing painted there — nothing to smooth)
    std::vector<double> M = maskOf(md->raw.value(field)); if (int(M.size()) != CMASK_N) M.assign(CMASK_N, 0);
    std::vector<double> was = M; double L = G.D[size_t(G.N - 1)];
    for (int q = 0; q < CMASK_N; q++) {
      int i = std::clamp(jsRound(cocoonStation(G, L * q / (CMASK_N - 1))), 0, G.N - 1); double d = 1e300;
      for (const Vec3& p : {G.c[size_t(i)], cocoonPoint(G, i, 0), cocoonPoint(G, i, PI / 2), cocoonPoint(G, i, PI), cocoonPoint(G, i, 1.5 * PI)}) {
        ScreenPt s = P.toScreen(p); if (s.z < 1) d = std::min(d, std::hypot(s.x - m.x(), s.y - m.y()));
      }
      if (d >= brushR) continue; double w = brushWeight(d);
      if (op == "smooth") M[size_t(q)] += ((was[size_t(std::max(0, q - 1))] + was[size_t(q)] + was[size_t(std::min(CMASK_N - 1, q + 1))]) / 3 - was[size_t(q)]) * w * 0.6;
      else M[size_t(q)] += ((op == "erase" ? 0 : strength) - M[size_t(q)]) * std::min(1.0, w * amt * 2.5);
    }
    bool ch = false; for (int q = 0; q < CMASK_N; q++) if (std::abs(M[size_t(q)] - was[size_t(q)]) > 1e-4) ch = true;
    if (!ch) continue;
    QJsonArray a; for (double x : M) a.append(x); md->raw.insert(field, a);
    md_.touched.insert(k->id); changed = true;
  }
  if (changed) { liveCocoons(md_.touched); overlayRev_++; }
  return changed;
}

bool Viewport::shellPress(QMouseEvent* e) {
  if (tool_ != "guide" || e->button() != Qt::LeftButton) return false;
  bool alt = e->modifiers() & Qt::AltModifier, ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  QPointF m = e->position();
  bool tip = shTab == "tip" && shellBrushActive(), pinf = shTab == "pinf" && shellBrushActive();
  // (Parent influence: signed — Ctrl inverted, Ctrl+Alt erase; Density: Ctrl+Shift — its Even spacing mask, +Alt — erasing it)
  if ((tip || pinf) && (!alt || (ctrl && (pinf || (tip && shift))))) {
    QString op = tip && ctrl && shift ? (alt ? "evenErase" : "even") : shift ? "smooth" : pinf ? (ctrl ? (alt ? "erase" : "invert") : "paint") : ctrl ? "erase" : "paint";
    md_ = {}; md_.op = op; md_.last = m; drag_ = Drag::Mask;
    shellMove(m); md_.last = m;
    return true;
  }
  if (alt) return false;
  if (shTab == "line" || shTab == "disc") { drag_ = Drag::GuideStroke; guideAdd_ = shift || ctrl; return true; }
  return false;
}

void Viewport::shellMove(const QPointF& m) {
  if (drag_ == Drag::GuideStroke) { overlayRev_++; requestUpdate(); return; }
  if (drag_ != Drag::Mask) return;
  double dd = std::hypot(m.x() - md_.last.x(), m.y() - md_.last.y()); md_.last = m;
  double amt = md_.moved ? std::min(1.0, dd / 25) * 0.35 : 0.2;
  const QString& op = md_.op; bool ch = false;
  if (md_.rib) ch = paintRibNow(m, op, amt);
  else if (tool_ == "modifier") ch = paintWeights(m, op, amt);
  else if (shTab == "tip") {
    if (op == "even") ch = paintCocoonMask(m, "paint", amt, "tip", tipBrush.strength, "emask");
    else if (op == "evenErase") ch = paintCocoonMask(m, "erase", amt, "tip", 0, "emask");
    else if (op == "smooth") { bool a = paintCocoonMask(m, "smooth", amt, "tip", 0), b = paintCocoonMask(m, "smooth", amt, "tip", 0, "emask"); ch = a || b; }
    else ch = paintCocoonMask(m, op, amt, "tip", tipBrush.strength);
  } else {
    if (op == "paint") ch = paintCocoonMask(m, "paint", amt, "pinf", -pinfBrush.strength);
    else if (op == "invert") ch = paintCocoonMask(m, "paint", amt, "pinf", pinfBrush.strength);
    else ch = paintCocoonMask(m, op, amt, "pinf", 0);
  }
  md_.moved = true; (void)ch;
  overlayRev_++; requestUpdate();
}

void Viewport::shellRelease(const QPointF& m) {
  if (drag_ == Drag::Mask) {
    drag_ = Drag::None;
    if (!md_.touched.empty()) commitLive();
    md_ = {}; return;
  }
  if (drag_ != Drag::GuideStroke) return;
  drag_ = Drag::None; overlayRev_++;
  bool click = std::hypot(m.x() - start_.x(), m.y() - start_.y()) <= 5;
  // a click on a guide / disc makes it the active one (the curve selection stays)
  int g = click ? pickGuide(m) : -1;
  if (g >= 0) {
    std::vector<int> keep; for (int id : sel_.items) if (const Item* x = scene_->item(id); x && x->type == ItemType::Curve) keep.push_back(id);
    keep.push_back(g); sel_.anchors.clear(); sel_.items = keep; onSelChange(); return;
  }
  if (shTab == "disc") { if (click) placeDisc(m); return; }
  if (!click) { createCocoonGuide(start_, m); return; }                    // (a line across a cocoon)
  if (auto ch = pickCurve(m, true, false)) selectItem(ch->id, guideAdd_);
  requestUpdate();
}

// ---- guide lines ----
// a line across the targeted cocoons: a guide on EACH — where the stroke crosses it on the screen (the nearest crossing),
// else at its point nearest the stroke's middle. Several made at once are linked (g.link): their settings go together
void Viewport::createCocoonGuide(const QPointF& A, const QPointF& B) {
  CocoonSystem* cs = hair_.cocoonSystem(); if (!cs) return;
  auto ks = shellTargets(); std::vector<std::pair<int, CocoonGeo>> geo;
  for (int k : ks) { CocoonGeo G = cs->geo(k); if (G.N >= 3) geo.push_back({k, G}); }
  if (geo.empty()) { bool any = false; for (auto& [id, it] : scene_->items) if (it.type == ItemType::Cocoon) any = true;
    ui::toast(any ? tr("Select the cocoon first (in the Cocoons section or in the viewport), then drag the line across it") : NO_COCOON); return; }
  Projector P = projector(); QPointF M = (A + B) / 2;
  struct Pick { int k; double f, z, d; };
  std::vector<std::pair<Pick, CocoonGeo*>> picks;
  for (auto& [k, G] : geo) {
    std::vector<ScreenPt> sp; for (auto& c : G.c) sp.push_back(P.toScreen(c));
    std::optional<Pick> near, cross;
    for (int i = 0; i < G.N - 1; i++) {
      const ScreenPt &p = sp[size_t(i)], &q = sp[size_t(i + 1)]; if (p.z > 1 || q.z > 1) continue;
      double rx = B.x() - A.x(), ry = B.y() - A.y(), sx = q.x - p.x, sy = q.y - p.y, den = rx * sy - ry * sx;
      if (std::abs(den) > 1e-9) {
        double u = ((p.x - A.x()) * sy - (p.y - A.y()) * sx) / den, v = ((p.x - A.x()) * ry - (p.y - A.y()) * rx) / den;
        if (u >= 0 && u <= 1 && v >= 0 && v <= 1) { double z = p.z + (q.z - p.z) * v; if (!cross || z < cross->z) cross = Pick{k, i + v, z, 0}; }
      }
      double l2 = sx * sx + sy * sy; if (l2 == 0) l2 = 1;
      double v = std::clamp(((M.x() - p.x) * sx + (M.y() - p.y) * sy) / l2, 0.0, 1.0), d = std::hypot(p.x + sx * v - M.x(), p.y + sy * v - M.y());
      if (!near || d < near->d) near = Pick{k, i + v, p.z + (q.z - p.z) * v, d};
    }
    if (cross || near) picks.push_back({cross ? *cross : *near, &G});
  }
  if (picks.empty()) { ui::toast(tr("The selected cocoon is not in view")); return; }
  QString link = picks.size() > 1 ? "gl" + QString::number(QDateTime::currentMSecsSinceEpoch(), 36) : QString();
  std::vector<int> made, cks; QStringList along, ruled;
  Vec3 n = cam_.dir(); QJsonObject gp = guideDefaultsNow();
  runEdit([&](Scene& s, Selection& sel, const CurveCache&) {
    for (auto& [h, Gp] : picks) {
      const CocoonGeo& G = *Gp; int i = std::clamp(jsRound(h.f), 0, G.N - 1);
      Vec3 Pp = atF(G.c, h.f); auto WA = planeHit(ray(A), Pp, n), WB = planeHit(ray(B), Pp, n); if (!WA || !WB) continue;
      int fl = std::min(int(std::floor(h.f)), G.N - 2); Vec3 d = (*WB - *WA).normalized();
      double sv = (G.D[size_t(fl)] + (G.D[size_t(fl + 1)] - G.D[size_t(fl)]) * (h.f - fl)) / G.D[size_t(G.N - 1)];
      Item* k = s.item(h.k);
      if (std::abs(d.dot(G.t[size_t(i)])) > 0.95) { along << k->name; continue; }
      int num = edit::nextNumber(s, ItemType::Guide);
      Item& g = s.addItem(ItemType::Guide, h.k, QString("Guide %1").arg(num));
      g.raw = QJsonObject{{"kind", "line"}, {"hits", QJsonArray()}, {"on", QJsonObject{{"s", sv}, {"d", QJsonArray{d.dot(G.u[size_t(i)]), d.dot(G.v[size_t(i)]), d.dot(G.t[size_t(i)])}}}}, {"p", gp}};
      if (!link.isEmpty()) { g.raw.insert("link", link); g.raw.insert("linkOn", true); }
      s.item(h.k)->expanded = true; made.push_back(g.id); cks.push_back(h.k);
      // where it takes the loops of its parent / a linked cocoon, theirs rule: the guide changes nothing there
      auto R = hair_.cocoons().find(h.k);
      if (R != hair_.cocoons().end() && !R->second.rings.empty()) {
        double tgt = sv * G.D[size_t(G.N - 1)]; const CocoonRing* b = nullptr;
        for (auto& r : R->second.rings) if (!b || std::abs(r.s - tgt) < std::abs(b->s - tgt)) b = &r;
        if (b && b->par) ruled << k->name;
      }
    }
    if (made.empty()) return QString("!") + (along.isEmpty() ? tr("The selected cocoon is not in view") : tr("Drag the line across the cocoon, not along it"));
    if (made.size() == 1) { s.item(made[0])->raw.remove("link"); s.item(made[0])->raw.remove("linkOn"); }
    // (the cocoons stay selected — the next line goes on them too)
    sel.anchors.clear(); sel.items.clear(); for (int k : cks) sel.items.push_back(k); for (int g : made) sel.items.push_back(g);
    QStringList names; for (int k : cks) names << s.item(k)->name;
    return (made.size() > 1 ? tr("%1 linked guide lines — on %2").arg(made.size()).arg(names.join(", ")) : tr("Guide line on %1 — a ring lies in its plane").arg(names[0]))
         + (ruled.isEmpty() ? QString() : tr(". Where %1 take%2 the loops of a parent / linked cocoon, theirs rule there").arg(ruled.join(", ")).arg(ruled.size() > 1 ? "" : "s"))
         + (along.isEmpty() ? QString() : tr(". Along %1, not across — none there").arg(along.join(", ")));
  }, false);
  if (!made.empty()) touchShellMods(made);
}

// ---- NURBS discs: an annulus between an outer and an inner ring (closed curves with 6 anchors each) ----
int Viewport::guidesGroupForNew() {
  for (auto& [id, it] : scene_->items) if (it.type == ItemType::Group && it.raw.value("guidesGroup").toBool()) {
    if (!it.visible || it.raw.value("guides").toString() == "hide") {
      if (!it.visible) for (int c : it.children) if (Item* x = scene_->item(c); x && x->ring < 0) x->visible = false;
      it.visible = true; if (it.raw.value("guides").toString() == "hide") it.raw.insert("guides", "show");
      ui::toast(tr("The Guides group was hidden — it is shown again (the older guides stay hidden)"));
    }
    return id;
  }
  edit::nextNumber(*scene_, ItemType::Group);
  Item& g = scene_->addItem(ItemType::Group, -1, "Guides"); g.expanded = false; g.color = "#3fbf7a";
  g.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "show"}, {"conv", QJsonValue()}, {"guidesGroup", true}, {"meshesGroup", false}, {"importGroup", false},
                      {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}};
  return g.id;
}

int Viewport::makeDisc(const Vec3& C, const Vec3& Nn, double rOut, double rIn, int parent) {
  Scene& s = *scene_;
  int G = parent >= 0 && s.item(parent) && s.item(parent)->type == ItemType::Cocoon ? parent : guidesGroupForNew();
  int num = edit::nextNumber(s, ItemType::Guide);
  QJsonObject p = guideDefaultsNow(); p.insert("rOut", rOut); p.insert("rIn", rIn);
  Item& g = s.addItem(ItemType::Guide, G, QString("Disc %1").arg(num));
  // (a new disc starts switched off — ⏻: placed and shaped first, then switched on)
  g.raw = QJsonObject{{"kind", "disc"}, {"hits", QJsonArray()}, {"p", p}, {"off", true}};
  int gid = g.id; QString gname = g.name;
  auto [u, v] = discBasis(Nn.normalized());
  QJsonArray rings;
  for (auto [r, nm] : {std::pair<double, const char*>{rOut, "outer"}, {rIn, "inner"}}) {
    edit::nextNumber(s, ItemType::Curve);
    Item& c = s.addItem(ItemType::Curve, G, gname + " " + nm); c.closed = true; c.ring = gid; c.raw.insert("ring", gid);
    for (int k = 0; k < 6; k++) c.anchors.push_back(s.addAnchor(C + u * (r * std::cos(k * PI / 3)) + v * (r * std::sin(k * PI / 3))).id);
    rings.append(c.id);
  }
  s.item(gid)->raw.insert("rings", rings);
  s.reindex();
  return gid;
}

int Viewport::discOnCocoon(int k, double f) {
  CocoonSystem* cs = hair_.cocoonSystem(); if (!cs) return -1;
  CocoonGeo G = cs->geo(k); if (G.N < 2) return -1;
  int i = std::clamp(jsRound(f), 0, G.N - 1); double r = std::max({G.a[size_t(i)], G.b[size_t(i)], sceneScale(*scene_) * 0.01});
  int g = -1; QString kn = scene_->item(k)->name;
  runEdit([&](Scene&, Selection& sel, const CurveCache&) { g = makeDisc(G.c[size_t(i)], G.t[size_t(i)], r * 1.6, r * 0.25, k); sel.anchors.clear(); sel.items = {g};
    return tr("NURBS disc on %1 — switched off: place it (E / R / T), then ⏻ in the Cocoons section switches it on — a ring lies on it").arg(kn); }, true);
  return g;
}

// a scene disc: level, through all the cocoons (the box of their axes) at height y (none — the middle), a little wider
// than the box; in the Guides group
int Viewport::sceneDiscAt(std::optional<double> y) {
  CocoonSystem* cs = hair_.cocoonSystem(); Box3 box; bool any = false;
  if (cs) for (auto& [id, it] : scene_->items) if (it.type == ItemType::Cocoon) { CocoonGeo G = cs->geo(id); if (G.N >= 3) { any = true; for (auto& p : G.c) box.expand(p); } }
  if (!any) { ui::toast(NO_COCOON); return -1; }
  Vec3 c = box.center(), sz = box.size(); double r = 0.6 * std::hypot(sz.x, sz.z) + sceneScale(*scene_) * 0.02;
  if (y) c.y = *y;
  int g = -1;
  runEdit([&](Scene& s, Selection& sel, const CurveCache&) { g = makeDisc(c, Vec3(0, 1, 0), r, r * 0.05, -1); sel.anchors.clear(); sel.items = {g};
    return tr("%1 in the scene — switched off: place it (E / R / T), then ⏻ in the outliner (Guides) switches it on — a ring lies on it on every cocoon it crosses").arg(s.item(g)->name); }, true);
  return g;
}

void Viewport::placeDisc(const QPointF& m) {
  std::optional<Vec3> P;
  if (auto h = pickSurface(m)) P = h->point;
  else if (auto ch = pickCurve(m, true, false)) { auto it = hair_.curves.find(ch->id); if (it != hair_.curves.end() && ch->sample >= 0 && ch->sample < int(it->second.p.size())) P = it->second.p[size_t(ch->sample)]; }
  if (!P) P = planeHit(ray(m), cam_.pivot, cam_.dir());
  if (!P) return;
  auto ks = shellTargets(); if (ks.empty()) { sceneDiscAt(P->y); return; }
  CocoonSystem* cs = hair_.cocoonSystem(); int bk = -1; double bf = 0, bd = 1e300;
  for (int k : ks) { CocoonGeo G = cs->geo(k); if (G.N < 2) continue; double f = std::clamp(nearestF(G.c, *P), 0.0, double(G.N - 1)), d = (atF(G.c, f) - *P).length(); if (d < bd) { bd = d; bk = k; bf = f; } }
  if (bk >= 0) discOnCocoon(bk, bf);
}

// "Create": a disc across the middle of each targeted cocoon; none targeted — a scene disc at the middle height
void Viewport::createDiscs() {
  auto ks = shellTargets(); if (ks.empty()) { sceneDiscAt(std::nullopt); return; }
  CocoonSystem* cs = hair_.cocoonSystem();
  for (int k : ks) { CocoonGeo G = cs->geo(k); discOnCocoon(k, (G.N - 1) / 2.0); }
}

// ---- what is drawn ----
std::vector<Viewport::MaskSeg> Viewport::maskLines() const {
  std::vector<MaskSeg> out; CocoonSystem* cs = const_cast<HairState&>(hair_).cocoonSystem(); if (!cs) return out;
  QString tab = tool_ == "guide" && (shTab == "tip" || shTab == "pinf") ? shTab : QString();
  // (whose: the selected modifiers only; none selected — those of the tab on the targeted cocoons; nothing — all of the tab)
  std::vector<const Item*> all, selM;
  for (auto& [id, it] : scene_->items) if ((it.type == ItemType::Tip || it.type == ItemType::PInf) && modCocoon(*scene_, &it) && (it.raw.value("cmask").isArray() || it.raw.value("emask").isArray())) { all.push_back(&it); if (sel_.hasItem(id)) selM.push_back(&it); }
  std::vector<int> ks = selM.empty() && !tab.isEmpty() ? shellTargets() : std::vector<int>{};
  std::vector<const Item*> list = selM;
  if (list.empty()) for (const Item* m : all) if (m->typeStr == tab && (ks.empty() || std::find(ks.begin(), ks.end(), modCocoon(*scene_, m)->id) != ks.end())) list.push_back(m);
  for (const Item* m : list) {
    CocoonGeo G = cs->geo(modCocoon(*scene_, m)->id); if (G.N < 2) continue; double L = G.D[size_t(G.N - 1)];
    bool pin = m->type == ItemType::PInf;
    auto draw = [&](const std::vector<double>& M, const float c0[3], std::function<void(double, float*)> c1, bool off) {
      auto at = [&](int i) { return off ? G.c[size_t(i)].lerp(cocoonPoint(G, i, 0), 0.45) : G.c[size_t(i)]; };
      for (int i = 0; i < G.N - 1; i++) {
        double w0 = cmaskAt(M, G.D[size_t(i)] / L), w1 = cmaskAt(M, G.D[size_t(i + 1)] / L); if (std::abs(w0) < 0.02 && std::abs(w1) < 0.02) continue;
        MaskSeg s; s.a = at(i); s.b = at(i + 1);
        for (auto [w, col] : {std::pair<double, float*>{w0, s.ca}, {w1, s.cb}}) { float e[3]; c1(w, e); double a = std::min(1.0, std::abs(w)); for (int j = 0; j < 3; j++) col[j] = float(c0[j] + (e[j] - c0[j]) * a); }
        out.push_back(s);
      }
    };
    if (m->raw.value("cmask").isArray()) {
      static const float pinC0[3] = {0.2f, 0.12f, 0.12f}, tipC0[3] = {0.25f, 0.2f, 0.05f};
      draw(maskOf(m->raw.value("cmask")), pin ? pinC0 : tipC0, [pin](double w, float* e) { if (pin) { if (w < 0) { e[0] = 1; e[1] = 0.3f; e[2] = 0.25f; } else { e[0] = 0.35f; e[1] = 1; e[2] = 0.45f; } } else { e[0] = 1; e[1] = 0.85f; e[2] = 0.2f; } }, false);
    }
    if (m->raw.value("emask").isArray()) { static const float c0[3] = {0.05f, 0.18f, 0.22f}; draw(maskOf(m->raw.value("emask")), c0, [](double, float* e) { e[0] = 0.3f; e[1] = 0.9f; e[2] = 1; }, true); }
  }
  return out;
}

std::vector<Viewport::GuideDraw> Viewport::guideLines() const {
  std::vector<GuideDraw> out; CocoonSystem* cs = const_cast<HairState&>(hair_).cocoonSystem();
  if (!ds_.showGuides) return out;
  for (auto& [id, g] : scene_->items) {
    if (g.type != ItemType::Guide || !scene_->isVisible(g)) continue;
    bool sel = sel_.hasItem(id), off = g.raw.value("off").toBool();
    if (g.raw.value("kind").toString() == "disc") {
      for (const QJsonValue& r : g.raw.value("rings").toArray()) { auto it = hair_.curves.find(r.toInt()); if (it == hair_.curves.end()) continue;
        GuideDraw d; d.pts = it->second.p; d.closed = false; d.sel = sel || sel_.hasItem(r.toInt()); d.off = off; d.id = id; out.push_back(d); }
    } else if (cs) {
      GuideDraw d; d.pts = cs->guideLine(id); if (d.pts.size() < 2) continue; d.sel = sel; d.off = off; d.id = id; out.push_back(d);
    }
  }
  return out;
}

int Viewport::pickGuide(const QPointF& m) const {
  Projector P = projector(); int best = -1; double bd = 7;
  for (auto& d : guideLines()) {
    std::vector<ScreenPt> sp; for (auto& p : d.pts) sp.push_back(P.toScreen(p));
    for (size_t i = 0; i + 1 < sp.size(); i++) {
      const ScreenPt &a = sp[i], &b = sp[i + 1]; if (a.z > 1 || b.z > 1) continue;
      double sx = b.x - a.x, sy = b.y - a.y, l2 = sx * sx + sy * sy, t = l2 > 0 ? std::clamp(((m.x() - a.x) * sx + (m.y() - a.y) * sy) / l2, 0.0, 1.0) : 0;
      double dd = std::hypot(a.x + sx * t - m.x(), a.y + sy * t - m.y()); if (dd < bd) { bd = dd; best = d.id; }
    }
  }
  return best;
}

}  // namespace hm

namespace hm {
void Viewport::showZone(int guide) {
  zoneG_ = guide; zoneUntil_ = QDateTime::currentMSecsSinceEpoch() + 1500; overlayRev_++; requestUpdate();
  QTimer::singleShot(1600, this, [this] { overlayRev_++; requestUpdate(); });
}
std::vector<CocoonSystem::ZoneLine> Viewport::zoneLines() const {
  if (zoneG_ < 0 || QDateTime::currentMSecsSinceEpoch() > zoneUntil_) return {};
  const Item* g = scene_->item(zoneG_); auto* cs = const_cast<HairState&>(hair_).cocoonSystem(); if (!g || !cs) return {};
  Box3 hb; for (auto& [id, it] : scene_->items) if (it.headTag && it.poly) hb.unite(it.poly->bounds());
  double headH = hb.empty() ? scene_->bounds().size().y : hb.size().y; if (!(headH > 0)) headH = 1;
  QJsonObject p = g->raw.value("p").toObject();
  return cs->zone(zoneG_, p.value("radius").toDouble(0.05) * headH, p.value("blend").isDouble() ? p.value("blend").toDouble() : 1);
}
}  // namespace hm