#include "app/Scheme.h"
#include "scene/Edit.h"
#include "scene/Walls.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QHelpEvent>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace hm {

namespace {
const double PI = 3.14159265358979323846, INF = std::numeric_limits<double>::infinity();
double clampd(double v, double a, double b) { return std::max(a, std::min(b, v)); }
double polyLen(const std::vector<Vec3>& Q) { double L = 0; for (size_t i = 1; i < Q.size(); i++) L += (Q[i] - Q[i - 1]).length(); return L; }
std::vector<Vec3> resample(const std::vector<Vec3>& Q, int n) {          // ccResample
  std::vector<double> L{0}; for (size_t i = 1; i < Q.size(); i++) L.push_back(L[i - 1] + (Q[i] - Q[i - 1]).length());
  double T = L.back() > 0 ? L.back() : 1; std::vector<Vec3> out; size_t j = 0;
  for (int k = 0; k < n; k++) {
    double s = T * k / (n - 1);
    while (j + 2 < Q.size() && L[j + 1] < s) j++;
    if (Q.size() < 2) { out.push_back(Q.empty() ? Vec3() : Q[0]); continue; }
    double f = L[j + 1] > L[j] ? clampd((s - L[j]) / (L[j + 1] - L[j]), 0, 1) : 0;
    out.push_back(Q[j] + (Q[j + 1] - Q[j]) * f);
  }
  return out;
}
Vec3 atF(const std::vector<Vec3>& C, double f) { int i = int(clampd(std::floor(f), 0, double(C.size()) - 2)); return C[i] + (C[i + 1] - C[i]) * clampd(f - i, 0, 1); }
long jsRound(double x) { return long(std::floor(x + 0.5)); }
double wrap(double a) { while (a < 0) a += 2 * PI; while (a >= 2 * PI) a -= 2 * PI; return a; }
double wrapPi(double a) { a = wrap(a); return a > PI ? a - 2 * PI : a; }
int idxOf(const std::vector<int>& v, int x) { auto it = std::find(v.begin(), v.end(), x); return it == v.end() ? -1 : int(it - v.begin()); }

std::map<int, SchemeLayout>& cache() { static std::map<int, SchemeLayout> c; return c; }

// the plain curves of a group a scheme draws
std::vector<const Item*> schemeCurves(const Scene& sc, const Item& g) {
  std::vector<const Item*> cs;
  for (int id : g.children) if (const Item* c = sc.item(id); c && c->type == ItemType::Curve && c->ring < 0 && !c->junc && !c->hasPath() && c->anchors.size() >= 2) cs.push_back(c);
  return cs;
}

const SchemeLayout& schemeLayout(Scene& sc, const CurveCache& cc, const Item& g, const std::vector<const Item*>& cs) {
  QString key;
  for (const Item* c : cs) key += QString("%1:%2-%3,").arg(c->id).arg(c->anchors.front()).arg(c->anchors.back());
  key += "/";
  for (int id : g.children) if (const Item* w = sc.item(id); w && w->type == ItemType::Surface) key += QString("%1-%2,").arg(w->a).arg(w->b);
  auto hit = cache().find(g.id); if (hit != cache().end() && hit->second.key == key) return hit->second;

  const int n = int(cs.size());
  std::map<int, int> ci; for (int i = 0; i < n; i++) ci[cs[size_t(i)]->id] = i;
  auto inSet = [&](int c) { return ci.count(c) > 0; };
  auto samples = [&](const Item* c) -> std::vector<Vec3> { auto it = cc.find(c->id); if (it != cc.end() && !it->second.p.empty()) return it->second.p; std::vector<Vec3> v; for (int a : c->anchors) v.push_back(sc.anchor(a)->pos); return v; };
  auto arcLen = [&](const Item* c) { auto it = cc.find(c->id); return it != cc.end() && !it->second.arc.empty() ? it->second.arc.back() : 0.0; };
  std::map<int, int> dir; std::map<int, double> xAt;
  auto xg = [&](int a) { auto it = xAt.find(a); return it == xAt.end() ? 0.0 : it->second; };
  // the edges, oriented root → tip (by the paths through the forks; a curve outside the network — its higher end first)
  std::vector<int> net; for (const Item* c : cs) if (segNet(*c) && c->seg) net.push_back(c->id);
  if (!net.empty()) for (auto& p : edit::pathsFor(sc, cc, net, true)) {
    bool all = true; for (auto& s : p) if (!inSet(s.c)) all = false; if (!all) continue;
    double d = 0;
    for (auto& s : p) {
      const Item* c = sc.item(s.c); if (!dir.count(s.c)) dir[s.c] = s.from;
      xAt[s.from] = std::max(xg(s.from), d); d += arcLen(c);
      int to = c->anchors.front() == s.from ? c->anchors.back() : c->anchors.front(); xAt[to] = std::max(xg(to), d);
    }
  }
  for (const Item* c : cs) if (!dir.count(c->id)) {
    int a = c->anchors.front(), b = c->anchors.back(), f = sc.anchor(a)->pos.y >= sc.anchor(b)->pos.y ? a : b, t = f == a ? b : a;
    dir[c->id] = f; if (!xAt.count(f)) xAt[f] = 0; xAt[t] = std::max(xg(t), xg(f) + arcLen(c));
  }
  double mx = 1e-9; for (auto& [k, v] : xAt) mx = std::max(mx, v);
  auto endOf = [&](const Item* c) { return dir[c->id] == c->anchors.front() ? c->anchors.back() : c->anchors.front(); };
  std::vector<std::vector<Vec3>> polys;
  for (const Item* c : cs) { auto Q = samples(c); if (dir[c->id] != c->anchors.front()) std::reverse(Q.begin(), Q.end()); polys.push_back(Q); }
  std::vector<int> order(static_cast<size_t>(n)); for (int i = 0; i < n; i++) order[size_t(i)] = i;
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return polyLen(polys[size_t(b)]) < polyLen(polys[size_t(a)]); });
  order.resize(size_t(std::min(n, std::max(2, int(std::ceil(n / 3.0))))));
  const int M = 60;
  // the through-wall: both sides run the whole lock and never come together — the widest such
  auto byAnchors = [&](const Item* c) {
    std::vector<const Item*> out; const auto& A = c->anchors;
    for (size_t k = 0; k + 1 < A.size(); k++) {
      const Item* q = nullptr;
      for (const Item* x : cs) { int i = idxOf(x->anchors, A[k]); if (i >= 0 && ((i + 1 < int(x->anchors.size()) && x->anchors[size_t(i + 1)] == A[k + 1]) || (i > 0 && x->anchors[size_t(i - 1)] == A[k + 1]))) { q = x; break; } }
      if (q && std::find(out.begin(), out.end(), q) == out.end()) out.push_back(q);
    }
    return out;
  };
  auto routeOf = [&](const Item* c) -> std::vector<const Item*> {
    if (!c) return {};
    if (c->hasPath()) { std::vector<const Item*> v; for (auto& p : c->path) if (const Item* x = sc.item(p[0])) v.push_back(x); return v; }
    if (c->junc && !inSet(c->id)) return byAnchors(c);
    return {c};
  };
  auto sidePoly = [&](std::vector<const Item*> R) -> std::optional<std::vector<Vec3>> {
    if (R.empty()) return std::nullopt; for (auto* c : R) if (!inSet(c->id)) return std::nullopt;
    std::stable_sort(R.begin(), R.end(), [&](const Item* a, const Item* b) { return xg(dir[a->id]) < xg(dir[b->id]); });
    double lo = xg(dir[R[0]->id]) / mx, hi = xg(endOf(R.back())) / mx;
    if (lo > 0.05 || hi < 0.95) return std::nullopt;
    std::vector<Vec3> Q; for (auto* c : R) { auto& P = polys[size_t(ci[c->id])]; Q.insert(Q.end(), P.begin(), P.end()); } return Q;
  };
  struct Thru { const Item* w; std::set<int> A, B; std::vector<Vec3> ra, rb; };
  std::optional<Thru> thru; double thruSep = 0;
  for (int id : g.children) if (const Item* w = sc.item(id); w && w->type == ItemType::Surface) {
    auto A = routeOf(sc.item(w->a)), B = routeOf(sc.item(w->b)); auto pa = sidePoly(A), pb = sidePoly(B); if (!pa || !pb) continue;
    auto ra = resample(*pa, 40), rb = resample(*pb, 40); double sep = INF; for (int i = 3; i < 37; i++) sep = std::min(sep, (ra[size_t(i)] - rb[size_t(i)]).length());
    if (sep > thruSep) { thruSep = sep; Thru t; t.w = w; for (auto* c : A) t.A.insert(c->id); for (auto* c : B) t.B.insert(c->id); t.ra = resample(*pa, M); t.rb = resample(*pb, M); thru = t; }
  }
  std::vector<std::vector<Vec3>> R; if (thru) R = {thru->ra, thru->rb}; else for (int i : order) R.push_back(resample(polys[size_t(i)], M));
  std::vector<Vec3> spine(M); for (int k = 0; k < M; k++) { Vec3 s; for (auto& r : R) s = s + r[size_t(k)]; spine[size_t(k)] = s / double(R.size()); }
  auto near = [&](const Vec3& p) { int bi = 0; double bd = INF; for (int i = 0; i < M; i++) { double d = (spine[size_t(i)] - p).lengthSq(); if (d < bd) { bd = d; bi = i; } } return bi; };
  std::vector<Vec3> T(M); for (int i = 0; i < M; i++) T[size_t(i)] = (spine[size_t(std::min(i + 1, M - 1))] - spine[size_t(std::max(i - 1, 0))]).normalized();
  Vec3 e1 = std::abs(T[0].y) < 0.9 ? Vec3(0, 1, 0) : Vec3(1, 0, 0); std::vector<Vec3> F1, F2;
  for (int i = 0; i < M; i++) { e1 = (e1 - T[size_t(i)] * e1.dot(T[size_t(i)])).normalized(); F1.push_back(e1); F2.push_back(T[size_t(i)].cross(e1)); }
  auto ang = [&](const Vec3& p) { int i = near(p); Vec3 d = p - spine[size_t(i)]; return std::atan2(d.dot(F2[size_t(i)]), d.dot(F1[size_t(i)])); };
  std::vector<double> all; for (auto& Q : polys) for (size_t k = 0; k < Q.size(); k++) if (k % 3 == 0) all.push_back(ang(Q[k]));
  std::sort(all.begin(), all.end());
  std::vector<std::pair<double, double>> gaps; for (size_t i = 0; i < all.size(); i++) { double a = all[i], b = i + 1 < all.size() ? all[i + 1] : all[0] + 2 * PI; gaps.push_back({b - a, (a + b) / 2}); }
  std::stable_sort(gaps.begin(), gaps.end(), [](auto& x, auto& y) { return y.first < x.first; });
  std::vector<double> cuts; for (size_t i = 0; i < gaps.size() && i < 5; i++) cuts.push_back(gaps[i].second);
  if (cuts.empty()) cuts.push_back(-PI);
  // columns: where a station falls along the middle line
  auto spineFrac = [&](const Vec3& p) {
    int i = near(p), j = std::min(i + 1, M - 1), k = std::max(i - 1, 0);
    auto seg = [&](int a, int b) { Vec3 ab = spine[size_t(b)] - spine[size_t(a)]; double L2 = ab.lengthSq(); return L2 > 1e-12 ? clampd((p - spine[size_t(a)]).dot(ab) / L2, 0, 1) : 0.0; };
    double tf = seg(i, j), tb = seg(k, i); return tf > 0 || i == 0 ? (i + tf) / (M - 1) : (k + tb) / (M - 1);
  };
  for (auto& [id, v] : xAt) if (const Anchor* a = sc.anchor(id)) v = spineFrac(a->pos) * mx;
  std::vector<double> xs; for (auto& [k, v] : xAt) xs.push_back(v / mx);
  std::sort(xs.begin(), xs.end()); xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
  std::vector<std::vector<double>> cols;
  for (double x : xs) { if (cols.empty() || x - cols.back().back() > 0.02) cols.push_back({x}); else cols.back().push_back(x); }
  auto colOf = [&](double x) { for (size_t i = 0; i < cols.size(); i++) if (std::find(cols[i].begin(), cols[i].end(), x) != cols[i].end()) return int(i); return -1; };
  const int NC = std::max(int(cols.size()), 2);
  auto cx = [&](int i) { if (i < 0 || i >= int(cols.size())) return 1.0; double s = 0; for (double v : cols[size_t(i)]) s += v; return s / cols[size_t(i)].size(); };
  SchemeLayout out; out.key = key; out.NC = NC;
  auto& items = out.items; std::map<int, int> node;
  auto nodeOf = [&](int a) {
    auto f = node.find(a); if (f != node.end()) return f->second;
    SchemeLayout::Node v; v.id = a; v.p = sc.anchor(a)->pos; v.col = std::max(0, colOf(xg(a) / mx)); v.ang = ang(v.p);
    items.push_back(v); int i = int(items.size()) - 1; node[a] = i; out.nodes.push_back(i); return i;
  };
  for (int k = 0; k < n; k++) {
    const Item* c = cs[size_t(k)]; int f = dir[c->id], t = endOf(c);
    int A = nodeOf(f), B = nodeOf(t); items[size_t(A)].cs.push_back(c->id); items[size_t(B)].cs.push_back(c->id);
    std::vector<int> ch{A}; const auto& Q = polys[size_t(k)]; double L = polyLen(Q), x0 = cx(items[size_t(A)].col), x1 = cx(items[size_t(B)].col);
    int lo = std::min(items[size_t(A)].col, items[size_t(B)].col), hi = std::max(items[size_t(A)].col, items[size_t(B)].col);
    for (int kk = lo + 1; kk < hi; kk++) {
      double fr = clampd((cx(kk) - x0) / ((x1 - x0) != 0 ? (x1 - x0) : 1), 0, 1), s = 0, tgt = fr * L; Vec3 q = Q[0];
      for (size_t j = 1; j < Q.size(); j++) { double d = (Q[j] - Q[j - 1]).length(); if (s + d >= tgt) { q = Q[j - 1] + (Q[j] - Q[j - 1]) * ((tgt - s) / (d != 0 ? d : 1)); break; } s += d; q = Q[j]; }
      SchemeLayout::Node d; d.col = kk; d.p = q; d.ang = ang(q); d.kind = 'd'; d.c = c->id; items.push_back(d); ch.push_back(int(items.size()) - 1);
    }
    ch.push_back(B); if (items[size_t(A)].col > items[size_t(B)].col) std::reverse(ch.begin(), ch.end());
    out.chains.push_back({c->id, ch});
  }
  std::vector<std::vector<int>> nb(items.size());
  for (auto& o : out.chains) for (size_t i = 1; i < o.ch.size(); i++) { nb[size_t(o.ch[i])].push_back(o.ch[i - 1]); nb[size_t(o.ch[i - 1])].push_back(o.ch[i]); }
  std::vector<std::vector<int>> byCol(static_cast<size_t>(NC)); for (size_t i = 0; i < items.size(); i++) byCol[size_t(std::min(items[i].col, NC - 1))].push_back(int(i));
  auto idx = [&] { for (auto& L : byCol) for (size_t i = 0; i < L.size(); i++) items[size_t(L[i])].i = int(i); };
  // the tracks between columns c and c + 1 and the crossings among them
  auto xOf = [&](int c) {
    if (c < 0 || c >= NC - 1) return 0;
    std::vector<std::pair<int, int>> sg;
    for (auto& o : out.chains) for (size_t k = 0; k + 1 < o.ch.size(); k++) { auto& a = items[size_t(o.ch[k])]; auto& b = items[size_t(o.ch[k + 1])]; if (a.col == c && b.col == c + 1) sg.push_back({a.i, b.i}); }
    int m = 0; for (size_t a = 0; a < sg.size(); a++) for (size_t b = a + 1; b < sg.size(); b++) if ((sg[a].first - sg[b].first) * (sg[a].second - sg[b].second) < 0) m++;
    return m;
  };
  auto crossings = [&] { int m = 0; for (int c = 0; c < NC - 1; c++) m += xOf(c); return m; };
  if (thru) {
    auto ai = [&](const Vec3& q, int i) { Vec3 d = q - spine[size_t(i)]; return std::atan2(d.dot(F2[size_t(i)]), d.dot(F1[size_t(i)])); };
    std::vector<double> cutI, sgn;
    for (int i = 0; i < M; i++) { double aA = ai(thru->ra[size_t(i)], i), dl = wrapPi(ai(thru->rb[size_t(i)], i) - aA); cutI.push_back(aA + dl / 2); sgn.push_back(dl > 0 ? -1 : 1); }
    for (auto& v : items) { int i = near(v.p); Vec3 d = v.p - spine[size_t(i)]; v.tseed = wrap(sgn[size_t(i)] * (std::atan2(d.dot(F2[size_t(i)]), d.dot(F1[size_t(i)])) - cutI[size_t(i)])); }
    // in each column: the lock's cross-section, ordered round its centre and unrolled from the top side to the bottom one
    auto crossAt = [](const std::vector<Vec3>& Q, const Vec3& o, const Vec3& t, double reach) -> std::optional<Vec3> {
      std::optional<Vec3> best; double bd = reach;
      for (size_t j = 0; j + 1 < Q.size(); j++) {
        double d0 = (Q[j] - o).dot(t), d1 = (Q[j + 1] - o).dot(t); if (d0 == d1 || d0 * d1 > 0) continue;
        Vec3 q = Q[j] + (Q[j + 1] - Q[j]) * (d0 / (d0 - d1)); double dd = (q - o).length(); if (dd < bd) { bd = dd; best = q; }
      }
      return best;
    };
    auto pa = resample(thru->ra, 200), pb = resample(thru->rb, 200);
    double lh = projectStep(sc);
    for (int c = 0; c < NC; c++) {
      double f = clampd(cx(c), 0, 1); int i = int(jsRound(f * (M - 1))); Vec3 o = atF(spine, f * (M - 1)), t = T[size_t(i)];
      auto top = crossAt(pa, o, t, INF), bot = crossAt(pb, o, t, INF); if (!top || !bot) continue;
      double w = (*top - *bot).length(), reach = std::max(4 * w, 4 * lh);
      struct P { int c; Vec3 q; double u = 0, v = 0, a = 0; }; std::vector<P> pts;
      for (int k = 0; k < n; k++) { int cid = cs[size_t(k)]->id; if (thru->A.count(cid) || thru->B.count(cid)) continue; auto q = crossAt(polys[size_t(k)], o, t, reach); if (q) pts.push_back({cid, *q}); }
      pts.push_back({-1, *top}); pts.push_back({-2, *bot});
      Vec3 ctr; for (auto& p : pts) ctr = ctr + p.q; ctr = ctr / double(pts.size());
      Vec3 u1 = *top - ctr; u1 = u1 - t * u1.dot(t); if (u1.lengthSq() < 1e-12) continue; u1 = u1.normalized(); Vec3 u2 = t.cross(u1);
      for (auto& p : pts) { Vec3 d = p.q - ctr; p.u = d.dot(u1); p.v = d.dot(u2); p.a = std::atan2(p.v, p.u); }
      std::stable_sort(pts.begin(), pts.end(), [](const P& a, const P& b) { return a.a < b.a; });
      int it = 0, ib = 0, N = int(pts.size()); for (int k = 0; k < N; k++) { if (pts[size_t(k)].c == -1) it = k; if (pts[size_t(k)].c == -2) ib = k; }
      auto arc = [&](int d) { std::vector<P> o2; int k = it; for (int s = 0; s < N; s++) { o2.push_back(pts[size_t(k)]); if (k == ib) break; k = (k + d + N) % N; } return o2; };
      auto a1 = arc(1), a2 = arc(-1);
      auto len = [](const std::vector<P>& a) { double s = 0; for (size_t k = 1; k < a.size(); k++) s += std::hypot(a[k].u - a[k - 1].u, a[k].v - a[k - 1].v); return s; };
      const auto& use = (a1.size() < a2.size() || (a1.size() == a2.size() && len(a1) < len(a2))) ? a2 : a1;
      double Lr = len(use); if (Lr == 0) Lr = 1;
      std::map<int, double> ring; double s = 0;
      for (size_t k = 0; k < use.size(); k++) { if (k) s += std::hypot(use[k].u - use[k - 1].u, use[k].v - use[k - 1].v); if (use[k].c >= 0) ring[use[k].c] = 2 * PI * s / Lr; }
      for (int vi : byCol[size_t(c)]) {
        auto& v = items[size_t(vi)]; std::vector<double> ok;
        if (v.kind == 'd') { if (ring.count(v.c)) ok.push_back(ring[v.c]); } else for (int x : v.cs) if (ring.count(x)) ok.push_back(ring[x]);
        if (!ok.empty()) { double sm = 0; for (double x : ok) sm += x; v.tseed = sm / ok.size(); }
      }
    }
    for (auto& v : items) {
      if (v.kind == 'd') v.pin = thru->A.count(v.c) ? -1 : thru->B.count(v.c) ? 1 : 0;
      else { bool a = false, b = false; for (int c : v.cs) { a |= thru->A.count(c) > 0; b |= thru->B.count(c) > 0; } v.pin = a && !b ? -1 : b && !a ? 1 : 0; }
    }
  }
  bool thruOn = false;
  auto pinOf = [&](int v) { return thruOn ? items[size_t(v)].pin : 0; };
  // for each cut (the through-wall first, then the widest empty angles): sorted by the angle, barycentre sweeps, then
  // swaps of neighbours in a column wherever they cut the crossings; the cut with the fewest kept
  std::vector<std::vector<int>> best; int bestN = std::numeric_limits<int>::max(); bool bestThru = false;
  std::vector<std::pair<bool, double>> tries; if (thru) tries.push_back({true, 0}); for (double c : cuts) tries.push_back({false, c});
  for (auto& [isWall, cut] : tries) {
    thruOn = isWall;
    for (auto& L : byCol) {
      for (int v : L) items[size_t(v)].seed = thruOn ? items[size_t(v)].tseed : wrap(items[size_t(v)].ang - cut);
      std::stable_sort(L.begin(), L.end(), [&](int a, int b) { int pa = pinOf(a), pb = pinOf(b); if (pa != pb) return pa < pb; return items[size_t(a)].seed < items[size_t(b)].seed; });
    }
    idx();
    auto bc = byCol; int bn = crossings();
    for (int it = 0; it < 24 && bn; it++) {
      int d = it % 2 ? -1 : 1; std::vector<int> ord;
      if (d > 0) for (int c = 1; c < NC; c++) ord.push_back(c); else for (int c = NC - 2; c >= 0; c--) ord.push_back(c);
      for (int c : ord) {
        auto& L = byCol[size_t(c)]; int ref = c - d;
        for (int v : L) { double s = 0; int m = 0; for (int u : nb[size_t(v)]) if (items[size_t(u)].col == ref) { s += items[size_t(u)].i; m++; } items[size_t(v)].k = m ? s / m : items[size_t(v)].i; }
        std::stable_sort(L.begin(), L.end(), [&](int a, int b) { int pa = pinOf(a), pb = pinOf(b); if (pa != pb) return pa < pb; if (items[size_t(a)].k != items[size_t(b)].k) return items[size_t(a)].k < items[size_t(b)].k; return items[size_t(a)].i < items[size_t(b)].i; });
        for (size_t i = 0; i < L.size(); i++) items[size_t(L[i])].i = int(i);
      }
      int m = crossings(); if (m < bn) { bn = m; bc = byCol; }
    }
    byCol = bc; idx();
    for (int pass = 0; pass < 12 && bn; pass++) {
      bool better = false;
      for (int c = 0; c < NC; c++) { auto& L = byCol[size_t(c)];
        for (size_t i = 0; i + 1 < L.size(); i++) {
          if (pinOf(L[i]) != pinOf(L[i + 1])) continue;
          int b0 = xOf(c - 1) + xOf(c); std::swap(L[i], L[i + 1]); items[size_t(L[i])].i = int(i); items[size_t(L[i + 1])].i = int(i + 1);
          int b1 = xOf(c - 1) + xOf(c);
          if (b1 < b0) { bn -= b0 - b1; better = true; } else { std::swap(L[i], L[i + 1]); items[size_t(L[i])].i = int(i); items[size_t(L[i + 1])].i = int(i + 1); }
        } }
      if (!better) break;
    }
    if (bn < bestN || (bn == bestN && thruOn)) { bestN = bn; best = byCol; bestThru = thruOn; }
    if (!bestN && (thruOn || !thru)) break;
  }
  byCol = best; idx(); thruOn = bestThru;
  // lanes: each at the mean lane of its neighbours, kept in order with a gap of one; the through-wall's sides on the first
  // and the last lane (after the first passes they hug the lock)
  int R0 = 0; for (auto& L : byCol) R0 = std::max(R0, int(L.size())); R0 += 1;
  for (auto& L : byCol) for (size_t i = 0; i < L.size(); i++) { auto& v = items[size_t(L[i])]; int p = pinOf(L[i]); v.lane = p < 0 ? 0 : p > 0 ? R0 : int(jsRound((i + 0.5) * R0 / L.size())); }
  auto hug = [&](const std::vector<int>& L, int i) -> std::optional<int> {
    int p = pinOf(L[size_t(i)]); if (!p) return std::nullopt;
    int j = i; do j -= p; while (j >= 0 && j < int(L.size()) && pinOf(L[size_t(j)]) == p);
    if (j < 0 || j >= int(L.size())) return std::nullopt; return items[size_t(L[size_t(j)])].lane + p;
  };
  for (int it = 0; it < 40; it++) for (auto& L : byCol) {
    for (int vi : L) { auto& v = items[size_t(vi)]; if (!nb[size_t(vi)].empty()) { double s = 0; for (int u : nb[size_t(vi)]) s += items[size_t(u)].lane; v.t = s / nb[size_t(vi)].size(); v.hasT = true; } }
    for (size_t i = 0; i < L.size(); i++) { auto& v = items[size_t(L[i])]; auto h = it >= 20 ? hug(L, int(i)) : std::nullopt; int p = pinOf(L[i]);
      v.lane = h ? *h : p < 0 ? 0 : p > 0 ? R0 : int(jsRound(v.hasT ? v.t : v.lane)); }
    for (size_t i = 1; i < L.size(); i++) if (items[size_t(L[i])].lane <= items[size_t(L[i - 1])].lane && pinOf(L[i]) <= 0) items[size_t(L[i])].lane = items[size_t(L[i - 1])].lane + 1;
    for (int i = int(L.size()) - 2; i >= 0; i--) if (items[size_t(L[size_t(i)])].lane >= items[size_t(L[size_t(i + 1)])].lane && pinOf(L[size_t(i)]) >= 0) items[size_t(L[size_t(i)])].lane = items[size_t(L[size_t(i + 1)])].lane - 1;
  }
  std::vector<int> used; for (auto& v : items) used.push_back(v.lane); std::sort(used.begin(), used.end()); used.erase(std::unique(used.begin(), used.end()), used.end());
  for (auto& v : items) v.lane = int(std::lower_bound(used.begin(), used.end(), v.lane) - used.begin());
  out.R = std::max(1, int(used.size()) - 1); out.crossings = bestN; out.thru = thruOn && thru ? thru->w->name : QString();
  cache()[g.id] = out;
  return cache()[g.id];
}
}  // namespace

QString schemeDump(Scene& sc, const CurveCache& cc, int gid) {
  const Item* g = sc.item(gid); if (!g) return "no group";
  auto cs = schemeCurves(sc, *g); if (cs.size() < 2) return "no scheme";
  cache().erase(gid);
  const SchemeLayout& L = schemeLayout(sc, cc, *g, cs);
  QStringList o; o << QString("NC=%1 R=%2 crossings=%3 thru=%4").arg(L.NC).arg(L.R).arg(L.crossings).arg(L.thru);
  QStringList st; for (int i : L.nodes) st << QString("%1:%2:%3").arg(L.items[size_t(i)].id).arg(L.items[size_t(i)].col).arg(L.items[size_t(i)].lane);
  o << st.join(" ");
  QStringList ch; for (auto& c : L.chains) { QStringList q; for (int i : c.ch) q << QString("%1/%2").arg(L.items[size_t(i)].col).arg(L.items[size_t(i)].lane); ch << QString("%1[%2]").arg(c.c).arg(q.join(" ")); }
  o << ch.join(" ");
  return o.join("\n");
}

bool SchemeView::hasScheme(const Scene& sc, const Item& g) {
  for (int c : g.children) if (const Item* x = sc.item(c); x && x->type == ItemType::Crown) return false;   // (a crown's group: its curves fan from the parting — no scheme of locks to lay out)
  return schemeCurves(sc, g).size() >= 2;
}

SchemeView::SchemeView(Viewport* vp, Scene* sc, int gid, int width, QWidget* parent) : QWidget(parent), vp_(vp), sc_(sc), gid_(gid) {
  setMouseTracking(true);
  const Item* g = sc->item(gid);
  L_ = &schemeLayout(*sc, vp->hair().curves, *g, schemeCurves(*sc, *g));
  W_ = std::max(180, width - 24); H_ = std::max(60, std::min(26 * (L_->R + 1), 420));
  QJsonObject sp = g->raw.value("spos").toObject();
  for (auto it = sp.begin(); it != sp.end(); ++it) { QJsonArray a = it.value().toArray(); if (a.size() == 2) sp_[it.key().toInt()] = {a[0].toDouble(), a[1].toDouble()}; }
  // curves between the same two stations with no column between: they part in a lens, one lane-fraction apart each
  std::map<std::pair<int, int>, std::vector<size_t>> twins;
  for (size_t o = 0; o < L_->chains.size(); o++) { auto& ch = L_->chains[o].ch; if (ch.size() != 2) continue; int a = L_->items[size_t(ch[0])].id, b = L_->items[size_t(ch[1])].id; twins[{std::min(a, b), std::max(a, b)}].push_back(o); }
  double gapY = std::min(13.0, 0.5 * H_ / L_->R);
  for (auto& [k, list] : twins) { if (list.size() < 2) continue;
    std::stable_sort(list.begin(), list.end(), [&](size_t a, size_t b) { return QString::localeAwareCompare(sc->item(L_->chains[a].c)->name, sc->item(L_->chains[b].c)->name) < 0; });
    for (size_t k2 = 0; k2 < list.size(); k2++) lens_[list[k2]] = (double(k2) - (list.size() - 1) / 2.0) * gapY; }
  for (int id : g->children) if (const Item* x = sc->item(id)) {
    if (x->type == ItemType::Surface) ws_.push_back(id);
    else if (x->type == ItemType::Curve && (x->hasPath() || x->junc) && x->ring < 0) ecs_.push_back(id);
  }
  layoutAll();
}

QPointF SchemeView::nodePt(int item) const {
  const auto& v = L_->items[size_t(item)];
  if (v.kind == 'n') { auto it = sp_.find(v.id); if (it != sp_.end()) return {10 + it->second.first * (W_ - 20), 8 + it->second.second * H_}; }
  return {10 + v.col * double(W_ - 20) / std::max(L_->NC - 1, 1), 8 + v.lane * double(H_) / L_->R};
}

std::vector<QPointF> SchemeView::trackQ(size_t o) const {
  const auto& ch = L_->chains[o].ch; std::vector<QPointF> Q;
  for (size_t i = 0; i < ch.size(); i++) { const auto& v = L_->items[size_t(ch[i])]; Q.push_back(!i || i + 1 == ch.size() ? nodePt(ch[i]) : QPointF(10 + v.col * double(W_ - 20) / std::max(L_->NC - 1, 1), 8 + v.lane * double(H_) / L_->R)); }
  auto f = lens_.find(o);
  if (f != lens_.end() && f->second != 0) {
    double off = f->second; QPointF a = Q[0], b = Q[1]; double dx = b.x() - a.x(), dd = dx != 0 ? dx : 1, e = std::min(14.0, std::abs(dx) * 0.3) * (dx < 0 ? -1 : 1);
    Q = {a, {a.x() + e, a.y() + off + (b.y() - a.y()) * e / dd}, {b.x() - e, b.y() + off - (b.y() - a.y()) * e / dd}, b};
  }
  return Q;
}

std::vector<int> SchemeView::routeIds(int curve) const {
  std::vector<int> out; const Item* c = sc_->item(curve); if (!c) return out;
  auto isTrack = [&](int id) { for (auto& t : tracks_) if (t.c == id) return true; return false; };
  if (isTrack(curve)) return {curve};
  if (c->hasPath()) { for (auto& p : c->path) out.push_back(p[0]); return out; }
  const auto& A = c->anchors;
  for (size_t k = 0; k + 1 < A.size(); k++) for (auto& t : tracks_) {
    const Item* q = sc_->item(t.c); int i = idxOf(q->anchors, A[k]);
    if (i >= 0 && ((i + 1 < int(q->anchors.size()) && q->anchors[size_t(i + 1)] == A[k + 1]) || (i > 0 && q->anchors[size_t(i - 1)] == A[k + 1]))) { if (std::find(out.begin(), out.end(), t.c) == out.end()) out.push_back(t.c); break; }
  }
  return out;
}

// a wall's side on the scheme, left to right: each curve's track cut to the stretch between its ends there
std::vector<QPointF> SchemeView::sidePts(int curve) const {
  std::vector<QPointF> o; const Item* c = sc_->item(curve); if (!c) return o;
  std::map<int, double> nodeX; for (int i : L_->nodes) nodeX[L_->items[size_t(i)].id] = nodePt(i).x();
  struct S { int id, a0 = -1, a1 = -1; }; std::vector<S> route;
  auto isTrack = [&](int id) -> const Track* { for (auto& t : tracks_) if (t.c == id) return &t; return nullptr; };
  if (isTrack(curve)) route.push_back({curve});
  else if (c->hasPath()) for (auto& p : c->path) route.push_back({p[0], p[1], p[2]});
  else { const auto& A = c->anchors;
    for (size_t k = 0; k + 1 < A.size(); k++) for (auto& t : tracks_) { const Item* q = sc_->item(t.c); int i = idxOf(q->anchors, A[k]);
      if (i >= 0 && ((i + 1 < int(q->anchors.size()) && q->anchors[size_t(i + 1)] == A[k + 1]) || (i > 0 && q->anchors[size_t(i - 1)] == A[k + 1]))) {
        if (!route.empty() && route.back().id == t.c && route.back().a1 == A[k]) route.back().a1 = A[k + 1]; else route.push_back({t.c, A[k], A[k + 1]}); break; } } }
  for (auto& s : route) {
    const Track* t = isTrack(s.id); const Item* q = sc_->item(s.id); if (!t || !q) continue;
    auto xOn = [&](int aid) -> std::optional<double> {
      if (aid < 0) return std::nullopt; auto f = nodeX.find(aid); if (f != nodeX.end()) return f->second;
      int nn = int(q->anchors.size()) - 1, i = idxOf(q->anchors, aid); auto x0 = nodeX.find(q->anchors.front()), x1 = nodeX.find(q->anchors.back());
      if (i < 0 || x0 == nodeX.end() || x1 == nodeX.end()) return std::nullopt; return x0->second + (x1->second - x0->second) * i / nn;
    };
    auto xa = xOn(s.a0), xb = xOn(s.a1);
    double lo = xa && xb ? std::min(*xa, *xb) : -INF, hi = xa && xb ? std::max(*xa, *xb) : INF;
    for (auto& p : t->q) if (p.x() >= lo - 0.5 && p.x() <= hi + 0.5) o.push_back(p);
  }
  std::stable_sort(o.begin(), o.end(), [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
  std::vector<QPointF> u; for (size_t i = 0; i < o.size(); i++) if (!i || std::abs(o[i].x() - o[i - 1].x()) > 0.01 || std::abs(o[i].y() - o[i - 1].y()) > 0.01) u.push_back(o[i]);
  return u;
}

QPainterPath SchemeView::wallPath(int w, double* area) const {
  const Item* W = sc_->item(w); if (area) *area = 0; if (!W) return {};
  auto A = sidePts(W->a), B = sidePts(W->b); if (A.empty() || B.empty()) return {};
  double x0 = std::max(A.front().x(), B.front().x()), x1 = std::min(A.back().x(), B.back().x());
  std::vector<QPointF> P; for (auto& q : A) if (q.x() >= x0 && q.x() <= x1) P.push_back(q);
  size_t na = P.size(); std::vector<QPointF> bb; for (auto& q : B) if (q.x() >= x0 && q.x() <= x1) bb.push_back(q);
  if (!na || bb.empty()) return {};
  P.insert(P.end(), bb.rbegin(), bb.rend());
  double ar = 0; for (size_t i = 0; i < P.size(); i++) { const QPointF &q = P[i], &r = P[(i + 1) % P.size()]; ar += q.x() * r.y() - r.x() * q.y(); }
  if (area) *area = std::abs(ar) / 2;
  QPainterPath path; path.moveTo(P[0]); for (size_t i = 1; i < P.size(); i++) path.lineTo(P[i]); path.closeSubpath();
  return path;
}

void SchemeView::layoutAll() {
  tracks_.clear(); for (size_t o = 0; o < L_->chains.size(); o++) tracks_.push_back({L_->chains[o].c, trackQ(o)});
  zones_.clear();
  std::vector<std::tuple<int, QPainterPath, double>> z;
  for (int w : ws_) { if (sc_->item(w)->name == L_->thru) continue; double ar = 0; QPainterPath p = wallPath(w, &ar); if (ar > 1) z.push_back({w, p, ar}); }
  std::stable_sort(z.begin(), z.end(), [](auto& a, auto& b) { return std::get<2>(b) < std::get<2>(a); });   // (the big ones first — a small one on top stays reachable)
  for (auto& [w, p, a] : z) zones_.push_back({w, p});
  // the chips: the walls, the service curves; then ≡ List (↺ Layout)
  chips_.clear();
  QFont f = font(); f.setPixelSize(11); QFontMetrics fm(f);
  double y = 4 + H_ + 16, x0 = 10, xr = 10 + W_;   // (chips under the field)
  auto row = [&](const QString& label, const std::vector<int>& list, bool wall) {
    if (list.empty()) return;
    y += 4; chips_.push_back({-1, QRectF(x0, y, xr - x0, 15), label, wall}); y += 18;
    double x = x0;
    for (int id : list) { QString t = sc_->item(id)->name; double w = fm.horizontalAdvance(t) + 12;
      if (x > x0 && x + w > xr) { x = x0; y += 18; }
      chips_.push_back({id, QRectF(x, y, w, 16), t, wall}); x += w + 3; }
    y += 18;
  };
  row(tr("Surfaces"), ws_, true);
  row(tr("Service curves"), ecs_, false);
  y += 5; listBtn_ = QRectF(x0, y, fm.horizontalAdvance(tr("≡ List")) + 18, 20);
  resetBtn_ = sp_.empty() ? QRectF() : QRectF(listBtn_.right() + 6, y, fm.horizontalAdvance(tr("↺ Layout")) + 18, 20);
  h_ = y + 20 + 6;
  setMinimumHeight(int(h_)); updateGeometry();
}

QSize SchemeView::sizeHint() const { return QSize(W_ + 16, int(h_)); }

void SchemeView::paintEvent(QPaintEvent*) {
  QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
  p.translate(10, 4);                                  // (the field: the box's padding 4 px 6 px 6 px 10 px)
  const Selection& S = vp_->selection();
  if (band_ >= 0) for (auto& [w, path] : zones_) if (w == band_) { p.setOpacity(0.25); p.fillPath(path, QColor(0x2f, 0x8f, 0x74)); p.setOpacity(1); }
  for (auto& [w, path] : zones_) if (S.hasItem(w)) p.fillPath(path, QColor(255, 138, 30, 107));
  for (auto& t : tracks_) {
    bool sel = S.hasItem(t.c), on = hov_.count(t.c) > 0;
    QColor col = on && !sel ? QColor(0xf3, 0xc9, 0xa0) : sel ? QColor(0xff, 0x8a, 0x1e) : QColor(0x8b, 0x83, 0xe6);
    p.setOpacity(hov_.empty() || on ? 1 : 0.15);
    QPen pen(col, sel ? 4.2 : 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); p.setPen(pen); p.setBrush(Qt::NoBrush);
    QPolygonF poly; for (auto& q : t.q) poly << q; p.drawPolyline(poly);
  }
  p.setOpacity(1);
  for (int i : L_->nodes) {
    const auto& v = L_->items[size_t(i)]; bool big = v.cs.size() >= 3; QPointF c = (dragNode_ == i) ? nodePt(i) : nodePt(i);
    p.setPen(QPen(sp_.count(v.id) ? QColor(0x9f, 0xd3, 0xff) : QColor(0xe6, 0xe6, 0xe6), big ? 2 : 1.5)); p.setBrush(QColor(0x25, 0x25, 0x25));
    double r = big ? 5 : 4; p.drawEllipse(c, r, r);
  }
  p.resetTransform();
  QFont f = font(); f.setPixelSize(11); p.setFont(f);
  for (size_t k = 0; k < chips_.size(); k++) {
    const Chip& c = chips_[k];
    if (c.id < 0) { p.setPen(QColor(0x8a, 0x8a, 0x8a)); p.drawText(c.r, Qt::AlignLeft | Qt::AlignVCenter, c.text); continue; }
    bool on = S.hasItem(c.id), hv = int(k) == hovChip_ || (c.wall && band_ == c.id);
    bool rel = false; if (!on) for (int id : S.items) if (const Item* x = sc_->item(id)) { if (x->type == ItemType::Curve) { const Item* w = sc_->item(c.id); if (w && w->type == ItemType::Surface && (w->a == id || w->b == id)) rel = true; } if (x->type == ItemType::Surface && (x->a == c.id || x->b == c.id)) rel = true; }
    QColor bg = on ? QColor(0x3a, 0x55, 0x85) : rel ? QColor(0x4d, 0x3b, 0x2c) : QColor(0, 0, 0, 0);
    QColor bd = hv ? QColor(0x6f, 0x7f, 0x9f) : rel ? QColor(0xa0, 0x70, 0x4a) : QColor(0x3a, 0x3a, 0x3a);
    p.setPen(bd); p.setBrush(bg); p.drawRoundedRect(c.r.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    p.setPen(on || hv || rel ? QColor(Qt::white) : QColor(0xbb, 0xbb, 0xbb)); p.drawText(c.r, Qt::AlignCenter, c.text);
  }
  for (auto [r, t] : {std::pair{listBtn_, tr("≡ List")}, std::pair{resetBtn_, tr("↺ Layout")}}) {
    if (r.isNull()) continue;
    p.setPen(QColor(0x55, 0x55, 0x55)); p.setBrush(QColor(0x33, 0x33, 0x33)); p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    p.setPen(QColor(0xdd, 0xdd, 0xdd)); p.drawText(r, Qt::AlignCenter, t);
  }
}

namespace {
double segDist(const QPointF& p, const QPointF& a, const QPointF& b) {
  QPointF ab = b - a; double L2 = ab.x() * ab.x() + ab.y() * ab.y(); double t = L2 > 0 ? clampd(QPointF::dotProduct(p - a, ab) / L2, 0, 1) : 0;
  QPointF q = a + ab * t; return std::hypot(p.x() - q.x(), p.y() - q.y());
}
}  // namespace

int SchemeView::trackAt(const QPointF& m) const {
  QPointF p = m - QPointF(10, 4);
  for (int k = int(tracks_.size()) - 1; k >= 0; k--) { auto& q = tracks_[size_t(k)].q; for (size_t i = 1; i < q.size(); i++) if (segDist(p, q[i - 1], q[i]) <= 5) return k; }
  return -1;
}
int SchemeView::stationAt(const QPointF& m) const {
  QPointF p = m - QPointF(10, 4);
  for (int k = int(L_->nodes.size()) - 1; k >= 0; k--) { int i = L_->nodes[size_t(k)]; QPointF c = nodePt(i); if (std::hypot(p.x() - c.x(), p.y() - c.y()) <= 6) return i; }
  return -1;
}
int SchemeView::zoneAt(const QPointF& m) const {
  QPointF p = m - QPointF(10, 4);
  for (int k = int(zones_.size()) - 1; k >= 0; k--) if (zones_[size_t(k)].second.contains(p)) return zones_[size_t(k)].first;
  return -1;
}
int SchemeView::chipAt(const QPointF& m) const { for (size_t k = 0; k < chips_.size(); k++) if (chips_[k].id >= 0 && chips_[k].r.contains(m)) return int(k); return -1; }

void SchemeView::hover(const std::set<int>& ids, int band) {
  if (ids == hov_ && band == band_) return;
  hov_ = ids; band_ = band;
  vp_->setHoverCurves(ids);
  update();
}

void SchemeView::mouseMoveEvent(QMouseEvent* e) {
  QPointF m = e->position();
  if (dragNode_ >= 0 && (e->buttons() & Qt::LeftButton)) {
    if (!moved_ && (m - pressAt_).manhattanLength() < 3) return; moved_ = true;
    double x = clampd(m.x() - 10, 4, W_ - 4), y = clampd(m.y() - 4, 4, H_ + 12);
    sp_[L_->items[size_t(dragNode_)].id] = {(x - 10) / (W_ - 20), (y - 8) / H_};
    for (size_t o = 0; o < L_->chains.size(); o++) tracks_[o].q = trackQ(o);
    update(); return;
  }
  int ch = chipAt(m); hovChip_ = ch;
  if (ch >= 0) {
    const Chip& c = chips_[size_t(ch)];
    if (c.wall) { const Item* w = sc_->item(c.id); std::set<int> s; for (int x : routeIds(w->a)) s.insert(x); for (int x : routeIds(w->b)) s.insert(x); hover(s, c.id); }
    else { auto r = routeIds(c.id); hover(std::set<int>(r.begin(), r.end())); }
    return;
  }
  if (int st = stationAt(m); st >= 0) { const auto& v = L_->items[size_t(st)]; hover(std::set<int>(v.cs.begin(), v.cs.end())); setCursor(Qt::SizeAllCursor); return; }
  unsetCursor();
  if (int t = trackAt(m); t >= 0) { hover({tracks_[size_t(t)].c}); setCursor(Qt::PointingHandCursor); return; }
  if (int z = zoneAt(m); z >= 0) { const Item* w = sc_->item(z); std::set<int> s; for (int x : routeIds(w->a)) s.insert(x); for (int x : routeIds(w->b)) s.insert(x); hover(s, z); setCursor(Qt::PointingHandCursor); return; }
  hover({});
}

void SchemeView::leaveEvent(QEvent*) { hovChip_ = -1; hover({}); }

void SchemeView::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  pressAt_ = e->position(); moved_ = false; dragNode_ = stationAt(pressAt_);
}

void SchemeView::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  QPointF m = e->position(); bool add = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
  int dn = dragNode_; dragNode_ = -1;
  if (dn >= 0) {
    if (moved_) {                                         // (kept in the group, undoable)
      QJsonObject o; for (auto& [id, xy] : sp_) o.insert(QString::number(id), QJsonArray{xy.first, xy.second});
      int gid = gid_; vp_->runEdit([gid, o](Scene& s, Selection&, const CurveCache&) { s.item(gid)->raw.insert("spos", o); return QString(); }, false);
      return;
    }
    // a click: its curves selected
    std::vector<int> sel = add ? vp_->selection().items : std::vector<int>{};
    for (int c : L_->items[size_t(dn)].cs) if (std::find(sel.begin(), sel.end(), c) == sel.end()) sel.push_back(c);
    vp_->setSelectedItems({}); vp_->setSelectedItems(sel); return;
  }
  if ((m - pressAt_).manhattanLength() > 4) return;
  if (listBtn_.contains(m)) { int gid = gid_; sc_->item(gid)->raw.insert("view", "list"); if (changed) QMetaObject::invokeMethod(this, [f = changed] { f(); }, Qt::QueuedConnection); return; }
  if (!resetBtn_.isNull() && resetBtn_.contains(m)) { int gid = gid_; vp_->runEdit([gid](Scene& s, Selection&, const CurveCache&) { s.item(gid)->raw.insert("spos", QJsonValue()); return QString(); }, false); return; }
  if (int ch = chipAt(m); ch >= 0) { vp_->selectItem(chips_[size_t(ch)].id, add); return; }
  if (int t = trackAt(m); t >= 0) { vp_->selectItem(tracks_[size_t(t)].c, add); return; }
  if (int z = zoneAt(m); z >= 0) { vp_->selectItem(z, add); return; }
}

void SchemeView::mouseDoubleClickEvent(QMouseEvent* e) {
  QPointF m = e->position();
  if (int st = stationAt(m); st >= 0) {                  // (back to where the layout puts it)
    int a = L_->items[size_t(st)].id; if (!sp_.count(a)) return; sp_.erase(a);
    QJsonObject o; for (auto& [id, xy] : sp_) o.insert(QString::number(id), QJsonArray{xy.first, xy.second});
    int gid = gid_; vp_->runEdit([gid, o](Scene& s, Selection&, const CurveCache&) { s.item(gid)->raw.insert("spos", o); return QString(); }, false);
    return;
  }
  if (int ch = chipAt(m); ch >= 0 && rename) { int id = chips_[size_t(ch)].id; auto f = rename; QMetaObject::invokeMethod(this, [f, id] { f(id); }, Qt::QueuedConnection); }
}

bool SchemeView::event(QEvent* e) {
  if (e->type() == QEvent::ToolTip) {
    auto* h = static_cast<QHelpEvent*>(e); QPointF m = h->pos(); QString tip;
    if (int ch = chipAt(m); ch >= 0) { const Item* x = sc_->item(chips_[size_t(ch)].id); tip = x->name + (x->junc ? QString() : tr("\nDouble-click or F2 — rename")); }
    else if (int st = stationAt(m); st >= 0) { QStringList n; for (int c : L_->items[size_t(st)].cs) n << sc_->item(c)->name; tip = n.join(", ") + tr("\nDrag — move the station on the scheme"); }
    else if (int t = trackAt(m); t >= 0) tip = sc_->item(tracks_[size_t(t)].c)->name;
    else if (int z = zoneAt(m); z >= 0) tip = sc_->item(z)->name;
    else if (listBtn_.contains(m)) tip = tr("Show this group as the usual list");
    else if (!resetBtn_.isNull() && resetBtn_.contains(m)) tip = tr("Put every station moved by hand back where the layout puts it (double-click a station — just that one)");
    if (tip.isEmpty()) QToolTip::hideText(); else QToolTip::showText(h->globalPos(), tip, this);
    return true;
  }
  return QWidget::event(e);
}

}  // namespace hm
