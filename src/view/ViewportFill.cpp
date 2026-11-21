// ◎ Fill the area under the pointer (Shift+RMB → Detect the fill outline here). A hole in the walls round the pointer
// (a closed loop of free stretches — anchor to anchor, exactly one shown wall along them) is filled like Fill hole; else
// rays go out on the screen in every direction, each stopping at the first curve it meets — the two hits on opposite
// sides lying closest together give the sides of the area (new logic: the facing paths through the forks). The outline
// is shown; confirmed, the area gets a NURBS wall (two plain curves — the usual wall; edge curves — a wall on them).
// Curves selected: the outline found is one they take part in (a hole with them on its rim; the area with them on its sides); the
// pointer picks among those — none round it: the area right by the nearest selected curve on the pointer's side.
#include "scene/Edit.h"
#include "scene/Network.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <cmath>
#include <functional>

namespace hm {

namespace {
int idxOf(const std::vector<int>& v, int x) { auto it = std::find(v.begin(), v.end(), x); return it == v.end() ? -1 : int(it - v.begin()); }
std::vector<const Item*> byOrder(const Scene& sc, ItemType t) { std::vector<const Item*> v; for (auto& [id, it] : sc.items) if (it.type == t) v.push_back(&it); std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->order < b->order; }); return v; }
struct Ray2 { double t, z, ang; int key, c; double si; };
// rays from m in 32 directions against screen segments: the first hit of each (a nearer one to the camera wins a tie)
template <class Seg> std::vector<Ray2> castRays(const QPointF& m, const std::vector<Seg>& segs) {
  std::vector<Ray2> out;
  for (int k = 0; k < 32; k++) {
    double ang = k * M_PI / 16, dx = std::cos(ang), dy = std::sin(ang); std::optional<Ray2> best;
    for (auto& s : segs) {
      double ex = s.b.x - s.a.x, ey = s.b.y - s.a.y, den = dx * ey - dy * ex; if (std::abs(den) < 1e-9) continue;
      double wx = s.a.x - m.x(), wy = s.a.y - m.y(), t = (wx * ey - wy * ex) / den, u = (wx * dy - wy * dx) / den;
      if (t <= 0 || u < 0 || u > 1) continue;
      double z = s.a.z + (s.b.z - s.a.z) * u;
      if (!best || t < best->t - 3 || (std::abs(t - best->t) <= 3 && z < best->z)) best = Ray2{t, z, ang, s.key, s.c, s.i + u};
    }
    if (best) out.push_back(*best);
  }
  return out;
}
struct Seg2 { ScreenPt a, b; int key = -1, c = -1; double i = 0; };
}  // namespace

bool Viewport::startFillAt(const QPoint& global) {
  fillTitle.clear();
  QPointF m = fromGlobal(global); if (m.x() < 0 || m.y() < 0 || m.x() > paneW() || m.y() > paneH()) { ui::toast(tr("Open the pie over the viewport, on the area to fill")); return false; }
  fillTitle.clear();
  beforeFrame(); Projector Pj = projector(); const Scene& sc = *scene_;
  // (curves selected: the outline found is one they take part in — the pointer still picks among those)
  std::set<int> selC; for (int id : sel_.items) if (const Item* it = sc.item(id); it && it->type == ItemType::Curve) selC.insert(id);
  // (a curve made of pieces — a path through the forks: its pieces count; a selected piece: the whole it is part of too, for the walls)
  for (auto& [id, it] : sc.items) if (it.type == ItemType::Curve && it.hasPath()) { bool whole = selC.count(id) > 0, piece = false;
    for (auto& p : it.path) if (selC.count(p[0])) piece = true;
    if (whole) for (auto& p : it.path) selC.insert(p[0]); if (piece) selC.insert(id); }
  // (the selected plain curves — what an outline is scored by: the one holding the most of them wins, a hole or an area alike —
  //  21.11: two selected, the pointer in a hole that had only one of them on its rim — that hole was taken, the other ignored)
  std::set<int> selB; for (int id : selC) if (const Item* it = sc.item(id); it && !it->hasPath()) selB.insert(id);
  std::map<std::pair<int, int>, std::vector<int>> baseOn;   // (an anchor-to-anchor step → the plain curves running along it)
  for (auto& [id, it] : sc.items) if (it.type == ItemType::Curve && !it.hasPath()) for (size_t k = 0; k + 1 < it.anchors.size(); k++) baseOn[{std::min(it.anchors[k], it.anchors[k + 1]), std::max(it.anchors[k], it.anchors[k + 1])}].push_back(id);
  struct Hole { std::vector<int> P, V; int parent = -1, n = 0; std::vector<std::vector<Vec3>> viz; QString title; }; std::optional<Hole> hole;
  // ---- a hole in the walls round the pointer ----
  struct E { int a, b; std::vector<int> walls, cs; std::vector<Vec3> pts; };
  std::vector<E> es; std::map<std::pair<int, int>, int> by;
  auto key = [](int a, int b) { return std::pair{std::min(a, b), std::max(a, b)}; };
  for (const Item* s : byOrder(sc, ItemType::Surface)) {
    if (!sc.isVisible(*s)) continue;
    for (int id : {s->a, s->b}) { const Item* c = sc.item(id); if (!c || c->type != ItemType::Curve) continue;
      for (size_t k = 0; k + 1 < c->anchors.size(); k++) { int a = c->anchors[k], b = c->anchors[k + 1]; if (a == b) continue;
        auto f = by.find(key(a, b)); int ei; if (f == by.end()) { ei = int(es.size()); es.push_back({a, b, {}, {}, {}}); by[key(a, b)] = ei; } else ei = f->second;
        if (idxOf(es[size_t(ei)].walls, s->id) < 0) es[size_t(ei)].walls.push_back(s->id); if (idxOf(es[size_t(ei)].cs, id) < 0) es[size_t(ei)].cs.push_back(id); } }
  }
  std::vector<int> fr; for (size_t i = 0; i < es.size(); i++) if (es[i].walls.size() == 1) fr.push_back(int(i));
  if (!fr.empty()) {
    std::vector<Seg2> segs;
    for (int ei : fr) { E& e = es[size_t(ei)]; int cid = -1; for (int c : e.cs) { auto it = hair_.curves.find(c); if (it != hair_.curves.end() && it->second.p.size() > 1) { cid = c; break; } } if (cid < 0) continue;
      const Item* c = sc.item(cid); const auto& S = hair_.curves.find(cid)->second; int k = -1;
      for (size_t i = 0; i + 1 < c->anchors.size(); i++) if ((c->anchors[i] == e.a && c->anchors[i + 1] == e.b) || (c->anchors[i] == e.b && c->anchors[i + 1] == e.a)) { k = int(i); break; }
      if (k < 0) continue;
      std::vector<Vec3> P{sc.anchor(c->anchors[size_t(k)])->pos}; for (size_t q = 0; q < S.p.size(); q++) if (S.seg[q] == k) P.push_back(S.p[q]); P.push_back(sc.anchor(c->anchors[size_t(k + 1)])->pos);
      if (c->anchors[size_t(k)] != e.a) std::reverse(P.begin(), P.end());
      e.pts = P;
      std::vector<ScreenPt> sp; for (auto& p : P) sp.push_back(Pj.toScreen(p));
      for (size_t i = 0; i + 1 < sp.size(); i++) if (sp[i].z < 1 && sp[i + 1].z < 1) segs.push_back({sp[i], sp[i + 1], ei, -1, 0}); }
    auto hits = castRays(m, segs);
    if (hits.size() >= 16) {
      std::set<int> hitSet; for (auto& h : hits) hitSet.insert(h.key);
      auto selEdge = [&](int ei) { for (int c : es[size_t(ei)].cs) if (selC.count(c)) return true; for (int c : baseOn[key(es[size_t(ei)].a, es[size_t(ei)].b)]) if (selB.count(c)) return true; return false; };
      const Ray2* first = &hits[0]; for (auto& h : hits) { const bool a = selEdge(h.key), b = selEdge(first->key); if ((a && !b) || (a == b && h.t < first->t)) first = &h; }
      int fe = first->key;
      std::map<int, std::vector<std::pair<int, int>>> adj; std::vector<int> adjOrder;
      for (int ei : fr) for (auto [x, y] : {std::pair{es[size_t(ei)].a, es[size_t(ei)].b}, std::pair{es[size_t(ei)].b, es[size_t(ei)].a}}) adj[x].push_back({y, ei});
      struct Loop { std::vector<int> P, es; int selN, score; double L; }; std::optional<Loop> bestLoop; int steps = 0;
      std::vector<int> path{es[size_t(fe)].a, es[size_t(fe)].b}, pes; std::set<int> used;
      std::function<void(int)> walk = [&](int v) {
        if (++steps > 200000) return;
        auto it = adj.find(v); if (it == adj.end()) return;
        for (auto [to, e] : it->second) {
          if (used.count(e) || e == fe) continue;
          if (to == es[size_t(fe)].a) {
            std::vector<int> loop = pes; loop.push_back(e); loop.push_back(fe);
            int score = 0; double L = 0; std::set<int> sl; for (int x : loop) { if (hitSet.count(x)) score++; L += (sc.anchor(es[size_t(x)].a)->pos - sc.anchor(es[size_t(x)].b)->pos).length(); for (int c : baseOn[key(es[size_t(x)].a, es[size_t(x)].b)]) if (selB.count(c)) sl.insert(c); }
            const int selN = int(sl.size());
            if (!bestLoop || selN > bestLoop->selN || (selN == bestLoop->selN && (score > bestLoop->score || (score == bestLoop->score && L < bestLoop->L)))) { std::vector<int> P2 = path; bestLoop = Loop{P2, loop, selN, score, L}; }
            continue;
          }
          if (idxOf(path, to) >= 0 || path.size() > 300) continue;
          used.insert(e); path.push_back(to); pes.push_back(e); walk(to); path.pop_back(); pes.pop_back(); used.erase(e);
        }
      };
      walk(es[size_t(fe)].b);
      if (!selC.empty() && bestLoop && bestLoop->selN == 0) bestLoop.reset();   // (no selected curve on it: the area between curves instead)
      if (bestLoop && bestLoop->P.size() >= 3) {
        // the curve of each step: the one the step before had, while it runs on (fewer corners), else any (an ordinary first)
        const auto& P = bestLoop->P; int n = int(P.size()); std::vector<int> V;
        for (int i = 0; i < n; i++) {
          const E& e = es[size_t(by[key(P[size_t(i)], P[size_t((i + 1) % n)])])]; std::vector<int> cs = e.cs;
          std::stable_sort(cs.begin(), cs.end(), [&](int x, int y) { return int(sc.item(x)->junc) < int(sc.item(y)->junc); });
          V.push_back(i && idxOf(cs, V.back()) >= 0 ? V.back() : cs[0]);
        }
        if (V[size_t(n - 1)] != V[0] && idxOf(es[size_t(by[key(P[size_t(n - 1)], P[0])])].cs, V[0]) >= 0) V[size_t(n - 1)] = V[0];
        Hole h; h.P = P; h.V = V; h.n = bestLoop->selN; h.parent = sc.item(es[size_t(fe)].walls[0])->parent;
        for (int x : bestLoop->es) h.viz.push_back(es[size_t(x)].pts.empty() ? std::vector<Vec3>{sc.anchor(es[size_t(x)].a)->pos, sc.anchor(es[size_t(x)].b)->pos} : es[size_t(x)].pts);
        QStringList names; for (int c : V) if (!names.contains(sc.item(c)->name)) names << sc.item(c)->name;
        h.title = tr("Fill the hole: %1").arg(names.join(" · ")); hole = h;
      }
    }
  }
  auto openHole = [&](const Hole& h) {
    fillViz_ = h.viz; overlayRev_++; requestUpdate();
    ui::PieSpec s; s.title = h.title; fillTitle = s.title;
    ui::PieItem ok; ok.label = tr("✔ Fill the highlighted outline");
    ok.act = [this, P = h.P, V = h.V, parent = h.parent] { fillViz_.clear(); runEdit([&](Scene& s2, Selection& sl, const CurveCache& cc) { int w = -1; QString msg = edit::fillCycle(s2, cc, P, V, parent, &w); if (w >= 0) { sl.anchors.clear(); sl.items = {w}; } return msg; }, true); };
    ui::PieItem no; no.label = tr("Cancel"); no.act = [this] { fillViz_.clear(); overlayRev_++; requestUpdate(); };
    s.top = {ok}; s.bottom = {no}; s.onEnd = [this] { fillViz_.clear(); overlayRev_++; requestUpdate(); };
    if (pieHost_) ui::Pie::open(pieHost_, global, s, false);
    return true;
  };
  if (hole && (selB.empty() || hole->n >= int(selB.size()))) return openHole(*hole);   // (nothing selected, or every selected curve on its rim)
  // ---- the area between two curves ----
  // (only the group being worked on — the groups of what is selected; nothing selected — every group)
  std::set<int> G;
  std::function<void(const Item*)> down = [&](const Item* x) { G.insert(x->id); for (int c : x->children) if (const Item* ci = sc.item(c); ci && ci->type == ItemType::Group) down(ci); };
  for (int id : sel_.items) if (const Item* it = sc.item(id)) { if (it->type == ItemType::Group) down(it); else if (const Item* p = sc.item(it->parent); p && p->type == ItemType::Group) G.insert(p->id); }
  for (int a : sel_.anchors) for (int c : edit::curvesWith(sc, a)) if (sc.item(c)->parent >= 0) G.insert(sc.item(c)->parent);
  std::vector<int> fc;
  for (const Item* c : byOrder(sc, ItemType::Curve)) {
    if (c->ring >= 0 || !c->conn.isEmpty() || c->junc || c->hasPath() || c->hasRib || !sc.isVisible(*c)) continue;
    auto it = hair_.curves.find(c->id); if (it == hair_.curves.end() || it->second.p.size() < 2) continue;
    if (!G.empty() && !G.count(c->parent)) continue; fc.push_back(c->id);
  }
  std::vector<Seg2> segs;
  for (int cid : fc) { const auto& S = hair_.curves.find(cid)->second; std::vector<ScreenPt> sp; for (auto& p : S.p) sp.push_back(Pj.toScreen(p));
    for (size_t i = 0; i + 1 < sp.size(); i++) if (sp[i].z < 1 && sp[i + 1].z < 1) segs.push_back({sp[i], sp[i + 1], -1, cid, double(i)}); }
  struct Pair { double s; int a, b; };
  // the pairs of curves facing each other across point q, nearest first
  auto pairsAt = [&](const QPointF& q) {
  const QPointF m = q; auto H = castRays(m, segs);
  std::vector<Pair> pairs;
  for (size_t i = 0; i < H.size(); i++) for (size_t j = i + 1; j < H.size(); j++) {
    double da = std::abs(H[i].ang - H[j].ang), dd = std::min(da, 2 * M_PI - da);
    if (dd < 2 * M_PI / 3 || H[i].c == H[j].c) continue;
    if (sc.item(H[i].c)->parent != sc.item(H[j].c)->parent) continue;      // (both sides in one group — one lock)
    pairs.push_back({H[i].t + H[j].t + 0.001 * (H[i].z + H[j].z), H[i].c, H[j].c});
  }
  std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.s < y.s; });
  return pairs; };
  std::vector<Pair> pairs = pairsAt(m);
  if (pairs.empty() && selC.empty()) { ui::toast(tr("No outline around the pointer — there must be curves on both sides of it")); return false; }
  auto sideIds = [&](int id) { std::vector<int> o; const Item* c = sc.item(id); if (!c) return o; o.push_back(id); for (auto& p : c->path) o.push_back(p[0]); return o; };
  auto wallBetween = [&](int x, int y) { for (const Item* w : byOrder(sc, ItemType::Surface)) { auto a = sideIds(w->a), b = sideIds(w->b); if ((idxOf(a, x) >= 0 && idxOf(b, y) >= 0) || (idxOf(a, y) >= 0 && idxOf(b, x) >= 0)) return true; } return false; };
  // the two sides of the area between curves A and B
  auto sidesFor = [&](int A, int B) {
  std::vector<std::vector<Network::Stretch>> sides;
  const Item *IA = sc.item(A), *IB = sc.item(B);
  if (IA->seg && IB->seg) {                                            // new logic: the facing paths through the forks
    std::vector<int> si, sa; Network net(*scene_, hair_.curves, si, sa);
    auto PA = net.curvePaths({A}, false), PB = net.curvePaths({B}, false);
    std::map<const void*, std::vector<Vec3>> R;
    auto pts = [&](const NetPath& p) { edit::Path ep; for (auto& s : p) ep.push_back({s.c, s.from}); return edit::pathPts(sc, hair_.curves, ep); };
    auto res = [&](const NetPath& p) -> const std::vector<Vec3>& { auto f = R.find(&p); if (f != R.end()) return f->second;
      auto Q = pts(p); std::vector<double> L{0}; for (size_t i = 1; i < Q.size(); i++) L.push_back(L.back() + (Q[i] - Q[i - 1]).length());
      double T = L.back() > 0 ? L.back() : 1; std::vector<Vec3> o; size_t j = 0;
      for (int k = 0; k < 40; k++) { double s = T * k / 39; while (j + 2 < Q.size() && L[j + 1] < s) j++; o.push_back(Q.size() < 2 ? Q[0] : Q[j].lerp(Q[j + 1], L[j + 1] > L[j] ? std::clamp((s - L[j]) / (L[j + 1] - L[j]), 0.0, 1.0) : 0)); }
      return R[&p] = o; };
    std::vector<NetPath> keep; keep.reserve(PA.size() * PB.size() * 2);   // (the trimmed paths, kept for res's cache and bp / bq)
    const NetPath *bp = nullptr, *bq = nullptr; double bd = 1e300; size_t bn = 0;   // (bn: the selected curves on the two paths — the most win)
    for (auto& p0 : PA) for (auto& q0 : PB) {
      // (one stem forking: the part both paths run along — before the fork, after they meet again — is no side; the area lies
      // between the branches. 20.11: C_3-4 and C_3-4b off one fork, C_3-5 on to the tip — the outline C_3-4 ↔ C_3-4b + C_3-5)
      size_t lo = 0; while (lo < p0.size() && lo < q0.size() && p0[lo].c == q0[lo].c && p0[lo].from == q0[lo].from) lo++;
      size_t hp = p0.size(), hq = q0.size(); while (hp > lo && hq > lo && p0[hp - 1].c == q0[hq - 1].c && p0[hp - 1].from == q0[hq - 1].from) { hp--; hq--; }
      if (hp <= lo || hq <= lo) continue;
      NetPath pt(p0.begin() + long(lo), p0.begin() + long(hp)), qt(q0.begin() + long(lo), q0.begin() + long(hq));
      auto holds = [](const NetPath& x, int c) { for (auto& s : x) if (s.c == c) return true; return false; };
      if (!holds(pt, A) || !holds(qt, B)) continue;                    // (the two curves the pointer is between: one on each side)
      keep.push_back(std::move(pt)); const NetPath& p = keep.back(); keep.push_back(std::move(qt)); const NetPath& q = keep.back();
      bool share = false; for (auto& s : p) for (auto& t : q) if (s.c == t.c) share = true; if (share) continue;
      bool walled = false; for (auto& s : p) for (auto& t : q) if (wallBetween(s.c, t.c)) walled = true; if (walled) continue;
      bool out = false; for (auto& s : p) if (sc.item(s.c)->parent != IA->parent) out = true; for (auto& t : q) if (sc.item(t.c)->parent != IB->parent) out = true; if (out) continue;
      const auto &a = res(p), &b = res(q); double d = 0; for (int k = 0; k < 40; k++) d += (a[size_t(k)] - b[size_t(k)]).length();
      std::set<int> in; for (auto& s : p) if (selC.count(s.c)) in.insert(s.c); for (auto& t : q) if (selC.count(t.c)) in.insert(t.c);
      if (!bp || in.size() > bn || (in.size() == bn && d < bd)) { bd = d; bp = &p; bq = &q; bn = in.size(); }
    }
    if (bp) for (auto* p : {bp, bq}) { std::vector<Network::Stretch> st; for (auto& s : *p) { const Item* c = sc.item(s.c); int i0 = idxOf(c->anchors, s.from); st.push_back({s.c, i0, i0 == 0 ? int(c->anchors.size()) - 1 : 0}); } sides.push_back(st); }
  }
  if (sides.empty()) for (int id : {A, B}) sides.push_back({{id, 0, int(sc.item(id)->anchors.size()) - 1}});
  return sides; };
  std::vector<std::vector<Network::Stretch>> sides;
  if (selC.empty()) {
    const int A = pairs[0].a, B = pairs[0].b;
    if (wallBetween(A, B)) { ui::toast(tr("Every outline around the pointer has its wall already")); return false; }
    sides = sidesFor(A, B);
  } else {                                     // (the nearest unfilled outline holding the most of the selected curves)
    size_t bestN = 0;
    auto tryPair = [&](int a, int b) {
      if (a == b || wallBetween(a, b)) return;
      auto sd = sidesFor(a, b); if (sd.size() < 2) return;
      std::set<int> in; for (auto& side : sd) for (auto& st : side) if (selB.count(st.c) || selC.count(st.c)) in.insert(st.c);
      if (in.size() > bestN) { bestN = in.size(); sides = sd; } };
    auto pickFrom = [&](const std::vector<Pair>& ps) { for (size_t k = 0; k < std::min<size_t>(ps.size(), 24); k++) tryPair(ps[k].a, ps[k].b); };
    pickFrom(pairs);
    // (the pairs the selection itself makes — two selected curves, or a selected one and a curve the pointer's rays meet: the
    //  outline they bound is found even when the rays' nearest pair is another one)
    { std::set<int> fcs(fc.begin(), fc.end()), hit; std::vector<int> SB; for (int c : selB) if (fcs.count(c)) SB.push_back(c);
      for (size_t k = 0; k < std::min<size_t>(pairs.size(), 24); k++) { hit.insert(pairs[k].a); hit.insert(pairs[k].b); }
      for (size_t i = 0; i < SB.size(); i++) for (size_t j = i + 1; j < SB.size(); j++) if (sc.item(SB[i])->parent == sc.item(SB[j])->parent) tryPair(SB[i], SB[j]);
      if (bestN < SB.size()) for (int a : SB) for (int h : hit) if (!selB.count(h) && sc.item(a)->parent == sc.item(h)->parent) tryPair(a, h); }
    if (hole && hole->n >= int(bestN)) return openHole(*hole);   // (the hole round the pointer holds as many of them)
    // (none round the pointer: the area right by the nearest selected curve, on the pointer's side)
    if (sides.empty()) {
      std::optional<QPointF> nb; double nd = 1e300;
      for (auto& s : segs) { if (!selC.count(s.c)) continue;
        const double ex = s.b.x - s.a.x, ey = s.b.y - s.a.y, L2 = ex * ex + ey * ey;
        const double u = L2 > 1e-12 ? std::clamp(((m.x() - s.a.x) * ex + (m.y() - s.a.y) * ey) / L2, 0.0, 1.0) : 0.0;
        const QPointF p(s.a.x + ex * u, s.a.y + ey * u); const double d = std::hypot(p.x() - m.x(), p.y() - m.y());
        if (d > 1e-6 && d < nd) { nd = d; nb = p + (m - p) * (std::min(8.0, d * 0.5) / d); } }
      if (nb) pickFrom(pairsAt(*nb));
    }
    if (sides.empty()) { ui::toast(tr("The selected curves bound no unfilled outline around the pointer — clear the selection to detect without them")); return false; }
  }
  // the outline shown
  fillViz_.clear();
  for (auto& side : sides) { std::vector<Vec3> P;
    for (auto& s : side) { const auto& S = hair_.curves.find(s.c)->second; int lo = std::min(s.i0, s.i1) * 24, hi = std::min(std::max(s.i0, s.i1) * 24, int(S.p.size()) - 1);
      std::vector<Vec3> q(S.p.begin() + lo, S.p.begin() + hi + 1); if (s.i0 > s.i1) std::reverse(q.begin(), q.end()); P.insert(P.end(), q.begin(), q.end()); }
    fillViz_.push_back(P); }
  overlayRev_++; requestUpdate();
  auto nameOf = [&](const std::vector<Network::Stretch>& sd) { if (sd.size() == 1 && std::min(sd[0].i0, sd[0].i1) == 0 && std::max(sd[0].i0, sd[0].i1) == int(sc.item(sd[0].c)->anchors.size()) - 1) return sc.item(sd[0].c)->name;
    QStringList n; for (auto& s : sd) if (!n.contains(sc.item(s.c)->name)) n << sc.item(s.c)->name; return n.join(" + "); };
  ui::PieSpec s; s.title = tr("Fill: %1 ↔ %2").arg(nameOf(sides[0]), nameOf(sides[1])); fillTitle = s.title; for (auto& sd : sides) for (auto& st : sd) fillTitle += QString(" #%1").arg(st.c);
  ui::PieItem ok; ok.label = tr("✔ Fill the highlighted outline");
  ok.act = [this, sides] {
    fillViz_.clear();
    runEdit([&](Scene& s2, Selection& sl, const CurveCache& cc) -> QString {
      Network net(s2, cc, sl.items, sl.anchors); int parent = s2.item(sides[0][0].c)->parent;
      int x = net.pathCurve(sides[0], parent), y = net.pathCurve(sides[1], parent);
      if (x == y) return "!" + tr("Nothing to fill there");
      if (!s2.item(x)->hasPath() && !s2.item(y)->hasPath()) { QString m2; auto made = edit::createSurfaces(s2, cc, {x, y}, false, false, &m2); if (made.empty()) return "!" + m2; sl.anchors.clear(); sl.items = made;
        return tr("Filled: a wall between %1 and %2").arg(s2.item(x)->name, s2.item(y)->name); }
      int w = edit::makeSurface(s2, x, y); if (w < 0) return "!" + tr("This area is filled already");
      sl.anchors.clear(); sl.items = {w}; s2.reindex();
      return tr("Filled: a wall along the highlighted outline (the curves stay whole)");
    }, true);
  };
  ui::PieItem no; no.label = tr("Cancel"); no.act = [this] { fillViz_.clear(); overlayRev_++; requestUpdate(); };
  s.top = {ok}; s.bottom = {no}; s.onEnd = [this] { fillViz_.clear(); overlayRev_++; requestUpdate(); };
  if (pieHost_) ui::Pie::open(pieHost_, global, s, false);
  return true;
}

}  // namespace hm

namespace hm {

// (tests) curve id selected, the pointer 14 px off its middle on the screen (side ±1): which outline Fill finds, and whether id is on it
QString Viewport::fillTestNear(int id, int side, int select) {
  beforeFrame(); auto it = hair_.curves.find(id); if (it == hair_.curves.end() || it->second.p.size() < 3) return "no curve";
  if (select != 2) setSelectedItems(select ? std::vector<int>{id} : std::vector<int>{}); beforeFrame();   // (2: the selection kept)
  const Projector Pj = projector(); const auto& P = it->second.p; const size_t k = P.size() / 2;
  const ScreenPt a = Pj.toScreen(P[k - 1]), b = Pj.toScreen(P[k + 1]), c = Pj.toScreen(P[k]);
  const double dx = b.x - a.x, dy = b.y - a.y, L = std::hypot(dx, dy); if (L < 1e-6 || c.z >= 1) return "off screen";
  const QPointF q(c.x - dy / L * 14 * side, c.y + dx / L * 14 * side);
  const bool ok = startFillAt(mapToGlobal((q + paneOrigin()).toPoint()));
  auto onIt = [&] { std::vector<int> ids{id}; if (const Item* c = scene_->item(id)) for (auto& p : c->path) ids.push_back(p[0]); for (int x : ids) if (fillTitle.contains(QString("#%1 ").arg(x)) || fillTitle.endsWith(QString("#%1").arg(x))) return true; return false; };
  if (ui::Pie* pp = ui::Pie::current()) pp->close();
  QStringList si; for (int x : sel_.items) si << QString::number(x);
  return QString("%1 %2 [sel %6]: %3 '%4' — %5").arg(id).arg(side).arg(ok).arg(fillTitle).arg(onIt() || fillTitle.startsWith(tr("Fill the hole")) ? "ON" : "MISSING").arg(si.join(QChar(0x2c)));
}

}  // namespace hm
