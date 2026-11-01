// ◎ Fill the area under the pointer (Shift+RMB → Detect the fill outline here). A hole in the walls round the pointer
// (a closed loop of free stretches — anchor to anchor, exactly one shown wall along them) is filled like Fill hole; else
// rays go out on the screen in every direction, each stopping at the first curve it meets — the two hits on opposite
// sides lying closest together give the sides of the area (new logic: the facing paths through the forks). The outline
// is shown; confirmed, the area gets a NURBS wall (two plain curves — the usual wall; edge curves — a wall on them).
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
  QPointF m = fromGlobal(global); if (m.x() < 0 || m.y() < 0 || m.x() > paneW() || m.y() > paneH()) { ui::toast(tr("Open the pie over the viewport, on the area to fill")); return false; }
  beforeFrame(); Projector Pj = projector(); const Scene& sc = *scene_;
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
      const Ray2* first = &hits[0]; for (auto& h : hits) if (h.t < first->t) first = &h;
      int fe = first->key;
      std::map<int, std::vector<std::pair<int, int>>> adj; std::vector<int> adjOrder;
      for (int ei : fr) for (auto [x, y] : {std::pair{es[size_t(ei)].a, es[size_t(ei)].b}, std::pair{es[size_t(ei)].b, es[size_t(ei)].a}}) adj[x].push_back({y, ei});
      struct Loop { std::vector<int> P, es; int score; double L; }; std::optional<Loop> bestLoop; int steps = 0;
      std::vector<int> path{es[size_t(fe)].a, es[size_t(fe)].b}, pes; std::set<int> used;
      std::function<void(int)> walk = [&](int v) {
        if (++steps > 200000) return;
        auto it = adj.find(v); if (it == adj.end()) return;
        for (auto [to, e] : it->second) {
          if (used.count(e) || e == fe) continue;
          if (to == es[size_t(fe)].a) {
            std::vector<int> loop = pes; loop.push_back(e); loop.push_back(fe);
            int score = 0; double L = 0; for (int x : loop) { if (hitSet.count(x)) score++; L += (sc.anchor(es[size_t(x)].a)->pos - sc.anchor(es[size_t(x)].b)->pos).length(); }
            if (!bestLoop || score > bestLoop->score || (score == bestLoop->score && L < bestLoop->L)) { std::vector<int> P2 = path; bestLoop = Loop{P2, loop, score, L}; }
            continue;
          }
          if (idxOf(path, to) >= 0 || path.size() > 300) continue;
          used.insert(e); path.push_back(to); pes.push_back(e); walk(to); path.pop_back(); pes.pop_back(); used.erase(e);
        }
      };
      walk(es[size_t(fe)].b);
      if (bestLoop && bestLoop->P.size() >= 3) {
        // the curve of each step: the one the step before had, while it runs on (fewer corners), else any (an ordinary first)
        const auto& P = bestLoop->P; int n = int(P.size()); std::vector<int> V;
        for (int i = 0; i < n; i++) {
          const E& e = es[size_t(by[key(P[size_t(i)], P[size_t((i + 1) % n)])])]; std::vector<int> cs = e.cs;
          std::stable_sort(cs.begin(), cs.end(), [&](int x, int y) { return int(sc.item(x)->junc) < int(sc.item(y)->junc); });
          V.push_back(i && idxOf(cs, V.back()) >= 0 ? V.back() : cs[0]);
        }
        if (V[size_t(n - 1)] != V[0] && idxOf(es[size_t(by[key(P[size_t(n - 1)], P[0])])].cs, V[0]) >= 0) V[size_t(n - 1)] = V[0];
        fillViz_.clear(); for (int x : bestLoop->es) fillViz_.push_back(es[size_t(x)].pts.empty() ? std::vector<Vec3>{sc.anchor(es[size_t(x)].a)->pos, sc.anchor(es[size_t(x)].b)->pos} : es[size_t(x)].pts);
        overlayRev_++; requestUpdate();
        int parent = sc.item(es[size_t(fe)].walls[0])->parent;
        QStringList names; for (int c : V) if (!names.contains(sc.item(c)->name)) names << sc.item(c)->name;
        ui::PieSpec s; s.title = tr("Fill the hole: %1").arg(names.join(" · "));
        ui::PieItem ok; ok.label = tr("✔ Fill the highlighted outline");
        ok.act = [this, P, V, parent] { fillViz_.clear(); runEdit([&](Scene& s2, Selection& sl, const CurveCache& cc) { int w = -1; QString msg = edit::fillCycle(s2, cc, P, V, parent, &w); if (w >= 0) { sl.anchors.clear(); sl.items = {w}; } return msg; }, true); };
        ui::PieItem no; no.label = tr("Cancel"); no.act = [this] { fillViz_.clear(); overlayRev_++; requestUpdate(); };
        s.top = {ok}; s.bottom = {no}; s.onEnd = [this] { fillViz_.clear(); overlayRev_++; requestUpdate(); };
        if (pieHost_) ui::Pie::open(pieHost_, global, s, false);
        return true;
      }
    }
  }
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
  auto H = castRays(m, segs);
  std::optional<std::pair<Ray2, Ray2>> best; double bsc = 1e300;
  for (size_t i = 0; i < H.size(); i++) for (size_t j = i + 1; j < H.size(); j++) {
    double da = std::abs(H[i].ang - H[j].ang), dd = std::min(da, 2 * M_PI - da);
    if (dd < 2 * M_PI / 3 || H[i].c == H[j].c) continue;
    if (sc.item(H[i].c)->parent != sc.item(H[j].c)->parent) continue;      // (both sides in one group — one lock)
    double s = H[i].t + H[j].t + 0.001 * (H[i].z + H[j].z); if (!best || s < bsc) { bsc = s; best = std::pair{H[i], H[j]}; }
  }
  if (!best) { ui::toast(tr("No outline around the pointer — there must be curves on both sides of it")); return false; }
  int A = best->first.c, B = best->second.c;
  auto sideIds = [&](int id) { std::vector<int> o; const Item* c = sc.item(id); if (!c) return o; o.push_back(id); for (auto& p : c->path) o.push_back(p[0]); return o; };
  auto wallBetween = [&](int x, int y) { for (const Item* w : byOrder(sc, ItemType::Surface)) { auto a = sideIds(w->a), b = sideIds(w->b); if ((idxOf(a, x) >= 0 && idxOf(b, y) >= 0) || (idxOf(a, y) >= 0 && idxOf(b, x) >= 0)) return true; } return false; };
  if (wallBetween(A, B)) { ui::toast(tr("Every outline around the pointer has its wall already")); return false; }
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
    const NetPath *bp = nullptr, *bq = nullptr; double bd = 1e300;
    for (auto& p : PA) for (auto& q : PB) {
      bool share = false; for (auto& s : p) for (auto& t : q) if (s.c == t.c) share = true; if (share) continue;
      bool walled = false; for (auto& s : p) for (auto& t : q) if (wallBetween(s.c, t.c)) walled = true; if (walled) continue;
      bool out = false; for (auto& s : p) if (sc.item(s.c)->parent != IA->parent) out = true; for (auto& t : q) if (sc.item(t.c)->parent != IB->parent) out = true; if (out) continue;
      const auto &a = res(p), &b = res(q); double d = 0; for (int k = 0; k < 40; k++) d += (a[size_t(k)] - b[size_t(k)]).length();
      if (!bp || d < bd) { bd = d; bp = &p; bq = &q; }
    }
    if (bp) for (auto* p : {bp, bq}) { std::vector<Network::Stretch> st; for (auto& s : *p) { const Item* c = sc.item(s.c); int i0 = idxOf(c->anchors, s.from); st.push_back({s.c, i0, i0 == 0 ? int(c->anchors.size()) - 1 : 0}); } sides.push_back(st); }
  }
  if (sides.empty()) for (int id : {A, B}) sides.push_back({{id, 0, int(sc.item(id)->anchors.size()) - 1}});
  // the outline shown
  fillViz_.clear();
  for (auto& side : sides) { std::vector<Vec3> P;
    for (auto& s : side) { const auto& S = hair_.curves.find(s.c)->second; int lo = std::min(s.i0, s.i1) * 24, hi = std::min(std::max(s.i0, s.i1) * 24, int(S.p.size()) - 1);
      std::vector<Vec3> q(S.p.begin() + lo, S.p.begin() + hi + 1); if (s.i0 > s.i1) std::reverse(q.begin(), q.end()); P.insert(P.end(), q.begin(), q.end()); }
    fillViz_.push_back(P); }
  overlayRev_++; requestUpdate();
  auto nameOf = [&](const std::vector<Network::Stretch>& sd) { if (sd.size() == 1 && std::min(sd[0].i0, sd[0].i1) == 0 && std::max(sd[0].i0, sd[0].i1) == int(sc.item(sd[0].c)->anchors.size()) - 1) return sc.item(sd[0].c)->name;
    QStringList n; for (auto& s : sd) if (!n.contains(sc.item(s.c)->name)) n << sc.item(s.c)->name; return n.join(" + "); };
  ui::PieSpec s; s.title = tr("Fill: %1 ↔ %2").arg(nameOf(sides[0]), nameOf(sides[1]));
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
