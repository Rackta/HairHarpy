// ✂ Scissors. Curve: RMB on a curve picks it (Shift — add; or select curves before), then a LMB stroke across them →
// the pie: Split into two curves / Split, delete the upper part / … the lower part (cut::cutCurves). Split ends: a click
// on a curve — from there to its tip it becomes a Y: the curve and a new branch sharing its trunk, their ends spread
// apart by splitR screen px (Ctrl+RMB drag), growing from 0 at the fork; across the lock a wall on one side stays with
// the curve, a wall on the other side goes over to the new branch; a tip shared with other curves splits too.
#include "scene/CurveMath.h"
#include "scene/Cut.h"
#include "scene/Edit.h"
#include "scene/Network.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QMouseEvent>
#include <cmath>
#include <deque>

namespace hm {

namespace {
int idxOf(const std::vector<int>& v, int x) { auto it = std::find(v.begin(), v.end(), x); return it == v.end() ? -1 : int(it - v.begin()); }
Vec3 nearestOn(const std::vector<Vec3>& P, const Vec3& p) {
  Vec3 best; double bd = 1e300;
  for (size_t i = 0; i + 1 < P.size(); i++) { Vec3 ab = P[i + 1] - P[i]; double l2 = ab.lengthSq(), t = l2 > 0 ? std::clamp((p - P[i]).dot(ab) / l2, 0.0, 1.0) : 0; Vec3 q = P[i] + ab * t; double d = (q - p).length(); if (d < bd) { bd = d; best = q; } }
  return best;
}
// a curve as the plan reads it (the tip at its first anchor: seen backwards — the same id, walls, shape)
struct CView { int id = -1; std::vector<int> A; std::vector<Vec3> P; std::vector<int> seg; std::vector<double> L; bool rev = false; };
CView viewOf(const Scene& sc, const CurveCache& cc, int id, bool rev) {
  CView v; v.id = id; v.rev = rev; const Item* c = sc.item(id); v.A = c->anchors;
  CurveSampling s; auto it = cc.find(id); if (it != cc.end() && !it->second.p.empty()) s = it->second; else s = sampleCurveFull(sc, *c);
  v.P = s.p; v.seg = s.seg; v.L = s.arc;
  if (rev) {
    int nA = int(v.A.size()); double tot = v.L.empty() ? 0 : v.L.back();
    std::reverse(v.A.begin(), v.A.end()); std::reverse(v.P.begin(), v.P.end()); std::reverse(v.seg.begin(), v.seg.end());
    for (int& s2 : v.seg) s2 = nA - 2 - s2; std::reverse(v.L.begin(), v.L.end()); for (double& x : v.L) x = tot - x;
  }
  return v;
}
}  // namespace

struct Viewport::SplitPlan {
  CView c; int J = -1, seg = 0; double u = 0, sJ = 0, tot = 0, rw = 0; int si = 0;
  Vec3 w; std::vector<int> stay, move; std::map<int, int> side;
};

// which end of the curve is the strand's tip — by the paths through it (the root is the higher end of a path); { atStart,
// free } — the tip at its first anchor; that end free (the strand ends on this curve, not a fork)
static std::pair<bool, bool> splitTipEnd(Scene& sc, const CurveCache& cc, int cid) {
  const Item* c = sc.item(cid); const auto& A = c->anchors; int last = A.back();
  if (!edit::isNewLogic(sc, *c) || !segNet(*c)) return {false, true};
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  auto P = net.curvePaths({cid}, false); int end = A[0];
  if (!P.empty()) { for (auto& st : P[0]) if (st.c == cid) { end = st.from == A[0] ? last : A[0]; break; } }
  if (P.empty()) end = sc.anchor(A[0])->pos.y < sc.anchor(last)->pos.y ? A[0] : last;
  bool fr = false;
  if (!P.empty()) { for (auto& p : P) if (p.back().c == cid) fr = true; }
  else { int n = 0; for (const Item* h : sc.curves()) if (segNet(*h) && idxOf(h->anchors, end) >= 0) n++; fr = n <= 1; }
  return {end == A[0], fr};
}

std::optional<Viewport::SplitPlan> Viewport::splitPlan(const PickedCurve& ch0, const QPointF& m, QString* err) const {
  const Item* c0 = scene_->item(ch0.id); if (!c0) return std::nullopt;
  auto [atStart, fr] = splitTipEnd(*scene_, hair_.curves, ch0.id);
  if (!fr) { *err = tr("%1 runs into a fork, not to the tip — click the curve that reaches the tip").arg(c0->name); return std::nullopt; }
  if (c0->ring >= 0 || c0->closed || c0->anchors.size() < 2) { *err = tr("Only an open curve can split its end"); return std::nullopt; }
  SplitPlan pl; pl.c = viewOf(*scene_, hair_.curves, ch0.id, atStart);
  const CView& c = pl.c; int n = int(c.A.size()), ns = int(c.P.size()); if (ns < 2) { *err = tr("Only an open curve can split its end"); return std::nullopt; }
  int i = std::min(ch0.sample, ns - 2); double t = ch0.t; int seg = ch0.seg;
  if (atStart) { i = ns - 2 - std::min(ch0.sample, ns - 2); t = 1 - ch0.t; seg = n - 2 - ch0.seg; }
  const auto& L = c.L; double tot = L.empty() || L.back() == 0 ? 1 : L.back();
  double sClick = L[size_t(i)] + (L[size_t(i + 1)] - L[size_t(i)]) * t;
  Projector Pj = projector(); int J = -1;                              // an anchor within 10 px of the click
  for (int k = 0; k < n; k++) { ScreenPt s = Pj.toScreen(scene_->anchor(c.A[size_t(k)])->pos); if (s.z <= 1 && std::hypot(s.x - m.x(), s.y - m.y()) < 10) { J = k; break; } }
  if (J == n - 1 || sClick > tot * 0.98) { *err = tr("Click the curve before its tip — it splits from there to the tip"); return std::nullopt; }
  pl.sJ = J >= 0 ? L[std::min<size_t>(size_t(J) * 24, L.size() - 1)] : sClick;
  Vec3 P = J >= 0 ? scene_->anchor(c.A[size_t(J)])->pos : c.P[size_t(i)].lerp(c.P[size_t(i + 1)], t);
  pl.si = J >= 0 ? std::min(J * 24, ns - 2) : i;
  Vec3 tan = (c.P[size_t(pl.si + 1)] - c.P[size_t(pl.si)]).normalized();
  // the walls: the two most opposite — from one to the other; one (an edge curve) — the new branch goes outwards; none —
  // across the curve on the screen
  std::vector<const Item*> ws; for (auto& [id, s] : scene_->items) if (s.type == ItemType::Surface && s.a != s.b && (s.a == c.id || s.b == c.id)) ws.push_back(&s);
  std::sort(ws.begin(), ws.end(), [](auto* a, auto* b) { return a->order < b->order; });
  std::vector<std::optional<Vec3>> dirs;
  for (auto* s : ws) {
    int o = s->a == c.id ? s->b : s->a; auto it = hair_.curves.find(o); std::optional<Vec3> d;
    if (it != hair_.curves.end() && it->second.p.size() > 1) { Vec3 v = nearestOn(it->second.p, P) - P; v = v - tan * v.dot(tan); if (v.lengthSq() > 1e-14) d = v.normalized(); }
    else { Vec3 v; v = v - tan * v.dot(tan); }
    dirs.push_back(d);
  }
  std::vector<int> ok; for (size_t k = 0; k < ws.size(); k++) if (dirs[k]) ok.push_back(int(k));
  std::optional<Vec3> w;
  if (ok.size() >= 2) { int bi = ok[0], bj = ok[1]; double bd = 1e300;
    for (int a : ok) for (int b : ok) if (a < b && dirs[size_t(a)]->dot(*dirs[size_t(b)]) < bd) { bd = dirs[size_t(a)]->dot(*dirs[size_t(b)]); bi = a; bj = b; }
    Vec3 x = *dirs[size_t(bj)] - *dirs[size_t(bi)]; if (x.lengthSq() < 1e-10) x = *dirs[size_t(bj)]; w = x.normalized(); }
  else if (ok.size() == 1) w = -*dirs[size_t(ok[0])];
  if (!w) { Vec3 x = tan.cross(cam_.dir()); if (x.lengthSq() < 1e-10) x = Vec3(1, 0, 0).cross(tan); if (x.lengthSq() < 1e-10) x = Vec3(0, 1, 0); w = x.normalized(); }
  pl.w = *w;
  std::deque<int> q;
  for (size_t k = 0; k < ws.size(); k++) {
    bool two = dirs[k] ? dirs[k]->dot(pl.w) > 0 : false; (two ? pl.move : pl.stay).push_back(ws[k]->id);
    int o = ws[k]->a == c.id ? ws[k]->b : ws[k]->a; if (!pl.side.count(o)) { pl.side[o] = two ? 2 : 1; q.push_back(o); }
  }
  std::vector<const Item*> all; for (auto& [id, s] : scene_->items) if (s.type == ItemType::Surface) all.push_back(&s);
  std::sort(all.begin(), all.end(), [](auto* a, auto* b) { return a->order < b->order; });
  while (!q.empty()) {                                                 // the rest of the lock: by the walls joining them
    int id = q.front(); q.pop_front();
    for (auto* s : all) { if (s->a != id && s->b != id) continue; int o = s->a == id ? s->b : s->a; if (o == c.id || pl.side.count(o)) continue; pl.side[o] = pl.side[id]; q.push_back(o); }
  }
  pl.rw = splitR * worldPerPixelAt(scene_->anchor(c.A.back())->pos);
  pl.J = J; pl.seg = seg; pl.u = std::clamp((i - seg * 24 + t) / 24, 0.0, 1.0); pl.tot = tot;
  return pl;
}

void Viewport::splitEnds(const PickedCurve& ch, const QPointF& m) {
  QString err; auto pl = splitPlan(ch, m, &err); splitViz_.clear(); ui::hud({});
  if (!pl) { ui::toast(err); return; }
  runEdit([&](Scene& sc, Selection& sel, const CurveCache& cc) -> QString {
    Network net(sc, cc, sel.items, sel.anchors);
    int cid = pl->c.id;
    // (the tip at the curve's first anchor: the curve turned to run root → tip first, then planned again on it)
    if (pl->c.rev) {
      int nA = int(sc.item(cid)->anchors.size()), ns = int(pl->c.P.size());
      net.reverseCurves({cid});
      PickedCurve c2 = ch; c2.sample = std::min(ns - 2 - std::min(ch.sample, ns - 2), ns - 2); c2.t = 1 - ch.t; c2.seg = nA - 2 - ch.seg;
      QString e2; auto p2 = splitPlan(c2, m, &e2); if (!p2) return "!" + e2;
      if (p2->c.rev) return QString("!") + tr("The curve could not be turned root → tip");
      pl = p2;
    }
    Item* c = sc.item(cid); const Vec3 w = pl->w; const double rw = pl->rw;
    int J = pl->J >= 0 ? c->anchors[size_t(pl->J)] : net.insertKeepShape(cid, pl->seg, pl->u);
    c = sc.item(cid); sc.reindex();
    CurveSampling cs = sampleCurveFull(sc, *c);                        // (arc lengths with the new anchor)
    std::vector<int>& A = c->anchors; int n = int(A.size()), iJ = idxOf(A, J), E = A.back();
    const auto& L = cs.arc; double tot = L.empty() || L.back() == 0 ? 1 : L.back(), sJ = L[std::min<size_t>(size_t(iJ) * 24, L.size() - 1)];
    auto fOf = [&](int k) { return std::clamp((L[std::min<size_t>(size_t(k) * 24, L.size() - 1)] - sJ) / std::max(tot - sJ, 1e-9), 0.0, 1.0); };
    Item& nb = net.makeCurve(c->parent); int nid = nb.id; c = sc.item(cid);
    nb.round = c->round; nb.roundSet = c->roundSet; nb.fair = c->fair; nb.fairSet = c->fairSet; nb.seg = c->seg; nb.visible = c->visible;
    { auto& arr = c->parent >= 0 ? sc.item(c->parent)->children : sc.roots; arr.erase(std::remove(arr.begin(), arr.end(), nid), arr.end()); arr.insert(std::find(arr.begin(), arr.end(), cid) + 1, nid); }
    std::vector<int> nA(A.begin(), A.begin() + iJ + 1);               // the trunk: the very same anchors
    for (int a : nA) if (auto o = c->ovr.find(a); o != c->ovr.end()) nb.ovr[a] = o->second;
    std::set<int> touched{cid, nid};
    std::vector<int> ins(A.begin(), A.end());
    for (int k = iJ + 1; k < n - 1; k++) {                             // the branches' own anchors
      int a = A[size_t(k)]; double f = fOf(k); std::optional<HandleOverride> o; if (auto oi = c->ovr.find(a); oi != c->ovr.end()) o = oi->second;
      if (edit::curvesWith(sc, a).size() > 1) {                        // welded to another curve: this one gets its own
        Anchor& a1 = sc.addAnchor(sc.anchor(a)->pos); a1.smooth = sc.anchor(a)->smooth; A[size_t(k)] = a1.id;
        if (o) { c->ovr.erase(a); c->ovr[a1.id] = *o; } a = a1.id;
      }
      Anchor& a2 = sc.addAnchor(sc.anchor(a)->pos + w * (rw * f)); a2.smooth = sc.anchor(a)->smooth; nA.push_back(a2.id);
      if (o) nb.ovr[a2.id] = *o;
      sc.anchor(a)->pos = sc.anchor(a)->pos - w * (rw * f);
    }
    sc.reindex();
    // the tip: the curve keeps E (moved to its side), the new branch gets its own; curves sharing E — by their side
    Anchor& E2 = sc.addAnchor(sc.anchor(E)->pos + w * rw); E2.smooth = sc.anchor(E)->smooth; nA.push_back(E2.id); int e2 = E2.id;
    if (auto o = c->ovr.find(E); o != c->ovr.end()) nb.ovr[e2] = o->second;
    Vec3 Ebefore = sc.anchor(A[size_t(n - 2)])->pos + w * (rw * fOf(n - 2));
    for (int hid : edit::curvesWith(sc, E)) {
      if (hid == cid) continue; Item* h = sc.item(hid);
      int j = idxOf(h->anchors, E); int prev = h->anchors[size_t(j == 0 ? 1 : j - 1)];
      bool two = pl->side.count(hid) ? pl->side[hid] == 2 : (sc.anchor(prev)->pos - Ebefore).dot(w) > 0;
      if (two) { h->anchors[size_t(j)] = e2; if (auto o = h->ovr.find(E); o != h->ovr.end()) { HandleOverride hv = o->second; h->ovr.erase(E); h->ovr[e2] = hv; } }
      touched.insert(hid);
    }
    sc.anchor(E)->pos = sc.anchor(E)->pos - w * rw;
    sc.item(nid)->anchors = nA;
    for (int s : pl->move) if (Item* sf = sc.item(s)) { if (sf->a == cid) sf->a = nid; if (sf->b == cid) sf->b = nid; }
    sc.reindex();
    sel.anchors.clear(); sel.items = {cid, nid};
    return tr("%1 split its end: %2 branches off%3").arg(sc.item(cid)->name, sc.item(nid)->name)
         .arg(pl->move.empty() ? QString() : tr(" (%1 wall%2 went over to it)").arg(pl->move.size()).arg(pl->move.size() > 1 ? "s" : ""));
  }, true);
}

bool Viewport::cutPress(QMouseEvent* e) {
  if (tool_ != "cut") return false;
  QPointF m = e->position(); bool shift = e->modifiers() & Qt::ShiftModifier, ctrl = e->modifiers() & Qt::ControlModifier;
  if (cutMode == "ends") {
    if (e->button() == Qt::RightButton && ctrl) { bd_ = {}; bd_.start = m; bd_.r0 = splitR; drag_ = Drag::BrushAdjust; return true; }   // (the spread)
    if (e->button() != Qt::LeftButton) return false;
    if (auto ch = pickCurve(m, true, false)) splitEnds(*ch, m); else ui::toast(tr("Click a curve where its end should split"));
    return true;
  }
  if (e->button() == Qt::RightButton) {                                // scissors: RMB picks the curve to cut
    if (auto ch = pickCurve(m, true, false)) selectItem(ch->id, shift);
    return true;
  }
  if (e->button() != Qt::LeftButton) return false;
  drag_ = Drag::CutStroke; guideAdd_ = shift; return true;
}

void Viewport::cutMove(const QPointF& m) {
  if (drag_ == Drag::CutStroke) { overlayRev_++; requestUpdate(); return; }
  if (tool_ != "cut" || cutMode != "ends" || drag_ != Drag::None) return;
  // the preview: both branches from the fork to the tip
  splitViz_.clear(); auto ch = pickCurve(m, true, false); QString err; std::optional<SplitPlan> pl;
  if (ch) pl = splitPlan(*ch, m, &err);
  if (!pl) { ui::hud(ch ? err : QString()); overlayRev_++; requestUpdate(); return; }
  Projector P = projector(); const CView& c = pl->c;
  for (int sg : {-1, 1}) {
    std::optional<ScreenPt> prev;
    for (int k = pl->si; k < int(c.P.size()); k++) {
      double f = std::clamp((c.L[size_t(k)] - pl->sJ) / std::max(pl->tot - pl->sJ, 1e-9), 0.0, 1.0);
      ScreenPt s = P.toScreen(c.P[size_t(k)] + pl->w * (sg * pl->rw * f));
      if (prev && prev->z < 1 && s.z < 1) { splitViz_.push_back({prev->x, prev->y}); splitViz_.push_back({s.x, s.y}); }
      prev = s;
    }
  }
  ui::hud(tr("%1: click — split the end here (spread %2 px · Ctrl+RMB drag to change)").arg(scene_->item(c.id)->name).arg(int(std::lround(splitR))));
  overlayRev_++; requestUpdate();
}

void Viewport::cutRelease(QMouseEvent* e) {
  QPointF m = e->position(); drag_ = Drag::None; overlayRev_++;
  if (std::hypot(m.x() - start_.x(), m.y() - start_.y()) <= 5) { if (auto ch = pickCurve(m, true, false)) selectItem(ch->id, guideAdd_); return; }   // a click picks a curve
  std::vector<int> sc; for (int id : sel_.items) if (const Item* c = scene_->item(id); c && c->type == ItemType::Curve) sc.push_back(id);
  if (sc.empty()) { ui::toast(tr("Pick the curve first: RMB on it (or select it before), then draw the cut across it")); return; }
  // where the stroke crosses each of the curves — the crossing nearest to the stroke's middle
  Projector P = projector(); std::vector<cut::Hit> hits;
  double x0 = start_.x(), y0 = start_.y(), x1 = m.x(), y1 = m.y(), mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
  for (int id : sc) {
    const Item* c = scene_->item(id); if (c->ring >= 0 || c->closed || !curveVisible(*c)) continue;
    auto it = hair_.curves.find(id); if (it == hair_.curves.end() || it->second.p.size() < 2) continue;
    const auto& S = it->second; std::vector<ScreenPt> sp; for (auto& p : S.p) sp.push_back(P.toScreen(p));
    std::optional<cut::Hit> best; double bd = 1e300;
    for (size_t i = 0; i + 1 < sp.size(); i++) {
      const ScreenPt &a = sp[i], &b = sp[i + 1]; if (a.z > 1 || b.z > 1) continue;
      double rx = x1 - x0, ry = y1 - y0, sx = b.x - a.x, sy = b.y - a.y, den = rx * sy - ry * sx; if (std::abs(den) < 1e-9) continue;
      double t = ((a.x - x0) * sy - (a.y - y0) * sx) / den, v = ((a.x - x0) * ry - (a.y - y0) * rx) / den;
      if (t < 0 || t > 1 || v < 0 || v > 1) continue;
      double d = std::hypot(x0 + rx * t - mx, y0 + ry * t - my);
      if (d < bd) { bd = d; int seg = S.seg[i]; best = cut::Hit{id, seg, std::clamp((double(i) - seg * 24 + v) / 24, 0.0, 1.0)}; }
    }
    if (best) hits.push_back(*best);
  }
  if (hits.empty()) { ui::toast(tr("The cut doesn't cross the selected curve")); return; }
  QString what = hits.size() == 1 ? scene_->item(hits[0].c)->name : tr("%1 curves").arg(hits.size());
  auto act = [this, hits](const QString& mode) { return [this, hits, mode] { runEdit([hits, mode](Scene& s, Selection& sl, const CurveCache& cc) { return cut::cutCurves(s, cc, sl, hits, mode); }, true); }; };
  ui::PieSpec spec; spec.title = tr("✂ Cut %1").arg(what);
  ui::PieItem a1; a1.label = tr("✂ Split into two curves"); a1.act = act("split");
  ui::PieItem a2; a2.label = tr("✂ Split, delete the upper part"); a2.act = act("up"); a2.danger = true;
  ui::PieItem a3; a3.label = tr("✂ Split, delete the lower part"); a3.act = act("down"); a3.danger = true;
  ui::PieItem cl; cl.label = tr("Cancel"); cl.act = [] {};
  spec.top = {a1, a2, a3}; spec.bottom = {cl};
  if (pieHost_) ui::Pie::open(pieHost_, toGlobal(m), spec, false);
}

}  // namespace hm
