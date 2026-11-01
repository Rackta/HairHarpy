#include "scene/AnchorOps.h"
#include "scene/CurveMath.h"
#include "scene/Edit.h"
#include "scene/HairState.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <deque>

namespace hm::aops {

namespace {
int idxOf(const std::vector<int>& v, int x) { auto it = std::find(v.begin(), v.end(), x); return it == v.end() ? -1 : int(it - v.begin()); }
std::vector<const Item*> byOrder(const Scene& sc, ItemType t) { std::vector<const Item*> v; for (auto& [id, it] : sc.items) if (it.type == t) v.push_back(&it); std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->order < b->order; }); return v; }
std::vector<int> curvesWithOrd(const Scene& sc, int a) { std::vector<int> o; for (const Item* c : byOrder(sc, ItemType::Curve)) if (idxOf(c->anchors, a) >= 0) o.push_back(c->id); return o; }
Vec3 P(const Scene& sc, int a) { return sc.anchor(a)->pos; }
const CurveSampling* smp(const CurveCache& cc, int id) { auto it = cc.find(id); return it == cc.end() || it->second.p.size() < 2 ? nullptr : &it->second; }
CurveSampling fresh(const Scene& sc, const CurveCache& cc, int id) { if (auto* s = smp(cc, id)) return *s; return sampleCurveFull(sc, *sc.item(id)); }
// the sample index of anchor k (ancSample) and the curve's points from its anchor i to j (either way)
int ancSample(const CurveSampling& S, int nA, int k) { if (k <= 0) return 0; if (k >= nA - 1) return int(S.p.size()) - 1; for (size_t i = 0; i < S.seg.size(); i++) if (S.seg[i] >= k) return int(i); return 0; }
std::vector<Vec3> curveStretch(const Scene& sc, const CurveSampling& S, int cid, int i, int j) {
  int nA = int(sc.item(cid)->anchors.size()), a = ancSample(S, nA, i), b = ancSample(S, nA, j);
  std::vector<Vec3> o(S.p.begin() + std::min(a, b), S.p.begin() + std::max(a, b) + 1); if (a > b) std::reverse(o.begin(), o.end()); return o;
}
double polyLen(const std::vector<Vec3>& Q) { double L = 0; for (size_t i = 1; i < Q.size(); i++) L += (Q[i] - Q[i - 1]).length(); return L; }
std::vector<Vec3> resampleN(const std::vector<Vec3>& Q, int m) {
  std::vector<double> L{0}; for (size_t k = 1; k < Q.size(); k++) L.push_back(L.back() + (Q[k] - Q[k - 1]).length());
  double T = L.back() > 0 ? L.back() : 1; std::vector<Vec3> o; size_t j = 0;
  for (int k = 0; k < m; k++) { double s = T * k / (m - 1); while (j + 2 < Q.size() && L[j + 1] < s) j++; o.push_back(Q.size() < 2 ? Q[0] : Q[j].lerp(Q[j + 1], L[j + 1] > L[j] ? std::clamp((s - L[j]) / (L[j + 1] - L[j]), 0.0, 1.0) : 0)); }
  return o;
}
double distToSeg(const Vec3& p, const Vec3& a, const Vec3& b) { Vec3 ab = b - a; double l2 = ab.lengthSq(), t = l2 > 0 ? std::clamp((p - a).dot(ab) / l2, 0.0, 1.0) : 0; return (a + ab * t - p).length(); }
double levelH(const Scene& sc) { return projectStep(sc); }
Item& newCurveLook(Item& c) { c.round = 0.6; c.roundSet = true; c.fair = 0.5; c.fairSet = true; return c; }   // (NEW_CURVE)
}  // namespace

// ---- a lock ----
std::vector<int> lockCurvesFrom(const Scene& sc, const std::vector<int>& from, const std::vector<int>& anchors) {
  std::set<int> seed;
  for (int a : anchors) for (int c : curvesWithOrd(sc, a)) if (sc.item(c)->ring < 0) seed.insert(c);
  for (int id : from) {
    const Item* it = sc.item(id); if (!it) continue;
    if (it->type == ItemType::XMod && it->raw.contains("tube") && !it->raw.value("tube").isNull()) it = sc.item(it->raw.value("tube").toInt());
    if (!it) continue;
    if (it->type == ItemType::Curve && it->ring < 0) seed.insert(it->id);
    else if (it->type == ItemType::Surface) { seed.insert(it->a); seed.insert(it->b); }
    else if (it->type == ItemType::Tube) seed.insert(it->raw.value("src").toInt(-1));
    else if (it->type == ItemType::Crown) { seed.insert(it->raw.value("part").toInt(-1)); seed.insert(it->raw.value("low").toInt(-1)); }
  }
  if (seed.empty()) return {};
  // (walls on edge curves: a curve counts with the edge curves running along it, an edge curve as the curves under it)
  for (const Item* c : byOrder(sc, ItemType::Curve)) if (c->hasPath()) for (auto& p : c->path) if (seed.count(p[0])) { seed.insert(c->id); break; }
  std::set<int> ids = seed; bool grew = true; auto walls = byOrder(sc, ItemType::Surface);
  while (grew) { grew = false; for (auto* s : walls) if (ids.count(s->a) != ids.count(s->b)) { ids.insert(s->a); ids.insert(s->b); grew = true; } }
  for (int id : std::vector<int>(ids.begin(), ids.end())) if (const Item* c = sc.item(id); c && c->hasPath()) { for (auto& p : c->path) ids.insert(p[0]); ids.erase(id); }
  std::vector<int> out; for (int id : ids) if (const Item* c = sc.item(id); c && c->type == ItemType::Curve && !c->hasPath()) out.push_back(id);
  return out;
}

// ---- new curves through / between anchors ----
QString curveThroughAnchors(Scene& sc, const CurveCache& cc, const std::vector<int>& list, Selection& sel) {
  std::vector<int> pts; for (size_t i = 0; i < list.size(); i++) if (i == 0 || (P(sc, list[i]) - P(sc, list[i - 1])).lengthSq() > 1e-12) pts.push_back(list[i]);
  if (pts.size() < 2) return "!" + QObject::tr("Pick at least two anchors in different places");
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  Item& c = newCurveLook(net.makeCurve(-1)); int cid = c.id;
  std::vector<int> A; for (int a : pts) { Anchor& n = sc.addAnchor(P(sc, a)); n.smooth = sc.anchor(a)->smooth; A.push_back(n.id); }
  sc.item(cid)->anchors = A; sc.reindex();
  sel.anchors.clear(); sel.items = {cid};
  return QObject::tr("%1: a new curve through %2 anchors (not joined to anything) — at the bottom of the outliner").arg(sc.item(cid)->name).arg(pts.size());
}

QString curveBetween(Scene& sc, const CurveCache& cc, int a, int b, int fallbackGroup, const std::function<std::optional<Vec3>(const Vec3&, double)>& headLift, Selection& sel) {
  if (a == b) return "!";
  auto firstNonRing = [&](int x) { for (int c : curvesWithOrd(sc, x)) if (sc.item(c)->ring < 0) return c; return -1; };
  int host = firstNonRing(a); if (host < 0) host = firstNonRing(b);
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  Item& c = newCurveLook(net.makeCurve(host >= 0 ? sc.item(host)->parent : fallbackGroup)); int cid = c.id;
  Vec3 mid = (P(sc, a) + P(sc, b)) * 0.5;
  if (headLift) if (auto h = headLift(mid, (P(sc, a) - P(sc, b)).length())) mid = *h;
  // an anchor of a crown's parting line: the new curve gets its own one ATTACHED there (a lower connector's — shared)
  auto end = [&](int x) {
    for (int k : curvesWithOrd(sc, x)) { const Item* h = sc.item(k); if (h->conn.isEmpty()) continue; if (h->conn == "low") return x;
      Anchor& n = sc.addAnchor(P(sc, x)); n.raw.insert("at", QJsonObject{{"c", h->id}, {"t", 0}}); return n.id; }
    return x;
  };
  std::map<int, int> E{{a, end(a)}, {b, end(b)}};
  // as long as the curves it joins, with common trunks: from the root of the curve whose anchor lies nearer its root,
  // along it to that anchor, across to the other and on along the other curve to its end
  auto hostOf = [&](int x) { if (E[x] != x) return -1; for (int k : curvesWithOrd(sc, x)) { const Item* h = sc.item(k); if (h->ring < 0 && h->conn.isEmpty() && !h->closed && !h->junc && h->id != cid) return k; } return -1; };
  auto fr = [&](int h, int x) { return h >= 0 ? double(idxOf(sc.item(h)->anchors, x)) / std::max<size_t>(1, sc.item(h)->anchors.size() - 1) : -1.0; };
  int x1 = a, x2 = b, h1 = hostOf(a), h2 = hostOf(b); double f1 = fr(h1, a), f2 = fr(h2, b);
  if (h1 >= 0 && h2 >= 0 ? f2 < f1 : h2 >= 0 ? f2 <= 0.5 : h1 >= 0 ? f1 > 0.5 : false) { std::swap(x1, x2); std::swap(h1, h2); }
  if (h1 >= 0 && h2 >= 0 && h1 == h2) h2 = -1;                          // (both on one curve: only the root part)
  std::vector<int> pre, post;
  if (h1 >= 0) { const auto& A = sc.item(h1)->anchors; pre.assign(A.begin(), A.begin() + idxOf(A, x1)); }
  if (h2 >= 0) { const auto& A = sc.item(h2)->anchors; post.assign(A.begin() + idxOf(A, x2) + 1, A.end()); }
  for (int x : post) if (idxOf(pre, x) >= 0 || x == x1) { post.clear(); break; }
  std::vector<int> L = pre; L.push_back(E[x1]); L.push_back(sc.addAnchor(mid).id); L.push_back(E[x2]); L.insert(L.end(), post.begin(), post.end());
  sc.item(cid)->anchors = L; sc.reindex();
  sel.anchors.clear(); sel.items = {cid};
  return QObject::tr("%1: a new curve between the anchors").arg(sc.item(cid)->name);
}

// ---- rotate junction ----
std::vector<JunctionChoice> junctionChoices(const Scene& sc, int a) {
  std::map<int, int> nb;
  for (const Item* c : byOrder(sc, ItemType::Curve)) { if (c->ring >= 0) continue; const auto& A = c->anchors; int i = idxOf(A, a); if (i < 0) continue;
    if (i > 0) nb[A[size_t(i - 1)]] = 1; if (i + 1 < int(A.size())) nb[A[size_t(i + 1)]] = 1; }
  std::vector<int> nbl; for (auto& [k, v] : nb) nbl.push_back(k);
  if (nbl.size() < 3) return {};
  auto dir = [&](int n) { return (P(sc, n) - P(sc, a)).normalized(); };
  const Anchor* A0 = sc.anchor(a); std::vector<JunctionChoice> out;
  for (const Item* c : byOrder(sc, ItemType::Curve)) {
    if (c->ring >= 0) continue; const auto& A = c->anchors; int i = idxOf(A, a); if (i < 0) continue;
    std::pair<int, int> pair;
    if (i > 0 && i < int(A.size()) - 1) pair = {A[size_t(i - 1)], A[size_t(i + 1)]};
    else {
      int n = A[size_t(i == 0 ? 1 : A.size() - 2)]; Vec3 d = dir(n);
      std::vector<int> cand; for (int x : nbl) if (x != n) cand.push_back(x);
      std::stable_sort(cand.begin(), cand.end(), [&](int x, int y) { return dir(x).dot(d) < dir(y).dot(d); });
      if (cand.empty()) continue;
      size_t k = 0; if (A0->pass && (A0->pass->first == n || A0->pass->second == n) && (A0->pass->first == cand[0] || A0->pass->second == cand[0])) k = 1 % cand.size();
      pair = {n, cand[k]};
    }
    out.push_back({QObject::tr("%1 — smooth through").arg(c->name), pair});
  }
  if (A0->pass) out.push_back({QObject::tr("↺ Automatic"), std::nullopt});
  return out;
}

// ---- ⫘ zip ----
ZipParts zipParts(const Scene& sc, int J) {
  ZipParts out; const int PER = 24;
  for (int cid : curvesWithOrd(sc, J)) {
    const Item* c = sc.item(cid); if (c->ring >= 0 || c->closed || c->junc || c->hasRib || c->hasPath()) continue;
    const auto& A = c->anchors; int i = idxOf(A, J), n = int(A.size());
    std::vector<Vec3> Pp; for (auto& s : sampleCurve(sc, *c, PER)) Pp.push_back(s.p);
    std::vector<ZipPart> ps;
    if (i > 0) { ZipPart z{cid, false, std::vector<int>(A.begin(), A.begin() + i + 1), std::vector<Vec3>(Pp.begin(), Pp.begin() + i * PER + 1)}; std::reverse(z.anchors.begin(), z.anchors.end()); std::reverse(z.pts.begin(), z.pts.end()); ps.push_back(z); }
    if (i < n - 1) ps.push_back({cid, true, std::vector<int>(A.begin() + i, A.end()), std::vector<Vec3>(Pp.begin() + i * PER, Pp.end())});
    auto h = [&](const ZipPart& p) { double s = 0; for (size_t k = 1; k < p.anchors.size(); k++) s += P(sc, p.anchors[k]).y; return s / (p.anchors.size() - 1) - P(sc, J).y; };
    if (ps.size() == 2 && (h(ps[0]) > 0) == (h(ps[1]) > 0)) { if (h(ps[1]) > h(ps[0])) std::swap(ps[0], ps[1]); out.up.push_back(ps[0]); out.down.push_back(ps[1]); }
    else for (auto& p : ps) (h(p) > 0 ? out.up : out.down).push_back(p);
  }
  return out;
}
std::vector<Vec3> zipAverage(const std::vector<ZipPart>& parts) {
  std::vector<std::vector<Vec3>> R; for (auto& p : parts) R.push_back(resampleN(p.pts, 200));
  std::vector<Vec3> o(200); for (int k = 0; k < 200; k++) { Vec3 s; for (auto& r : R) s = s + r[size_t(k)]; o[size_t(k)] = s / double(R.size()); }
  return o;
}
QString zipApply(Scene& sc, const CurveCache& cc, Selection& sel, int J, bool up, int target) {
  auto Z = zipParts(sc, J); auto parts = up ? Z.up : Z.down; if (parts.size() < 2) return "!";
  const ZipPart* tp = nullptr; for (auto& p : parts) if (p.c == target) tp = &p;
  std::vector<int> trunk;
  if (tp) trunk = tp->anchors;
  else {
    const ZipPart* rich = &parts[0]; for (auto& p : parts) if (p.anchors.size() > rich->anchors.size()) rich = &p;
    auto avg = zipAverage(parts); int m = int(avg.size()) - 1;
    std::vector<double> L{0}; for (size_t k = 1; k < rich->pts.size(); k++) L.push_back(L.back() + (rich->pts[k] - rich->pts[k - 1]).length());
    double T = L.back() > 0 ? L.back() : 1;
    trunk.push_back(J);
    for (size_t k = 0; k + 1 < rich->anchors.size(); k++) { double x = L[std::min((k + 1) * 24, L.size() - 1)] / T * m; int j = std::min(int(std::floor(x)), m - 1); trunk.push_back(sc.addAnchor(avg[size_t(j)].lerp(avg[size_t(j + 1)], x - j)).id); }
  }
  std::set<int> keep(trunk.begin(), trunk.end()), dropped;
  for (auto& p : parts) {
    Item* c = sc.item(p.c);
    for (size_t k = 1; k < p.anchors.size(); k++) { int a = p.anchors[k]; if (!keep.count(a)) { dropped.insert(a); c->ovr.erase(a); } }
    const auto A = c->anchors; int i = idxOf(A, J);
    std::vector<int> L;
    if (p.fwd) { L.assign(A.begin(), A.begin() + i); L.insert(L.end(), trunk.begin(), trunk.end()); }
    else { L.assign(trunk.rbegin(), trunk.rend()); L.insert(L.end(), A.begin() + i + 1, A.end()); }
    c->anchors = L;
    if (tp && p.c == tp->c) continue;
    // handles along the run: the target's frozen ones (turned when the curve runs the other way), else automatic
    for (size_t k = 1; k < trunk.size(); k++) { int a = trunk[k]; c->ovr.erase(a);
      if (!tp) continue; const Item* tc = sc.item(tp->c); auto o = tc->ovr.find(a); if (o == tc->ovr.end()) continue;
      HandleOverride h = o->second; if (p.fwd != tp->fwd) std::swap(h.in, h.out); c->ovr[a] = h; }
  }
  sc.reindex();
  // an anchor that went but is still used by another curve (a branch off a zipped part) moves onto the nearest of the run
  Network net(sc, cc, sel.items, sel.anchors);
  for (int a : dropped) if (!curvesWithOrd(sc, a).empty()) {
    int best = trunk[0]; for (int t : trunk) if ((P(sc, t) - P(sc, a)).length() < (P(sc, best) - P(sc, a)).length()) best = t;
    net.weldInto(a, best);
  }
  net.segmentNetwork(); sc.reindex();
  return QObject::tr("Zipped %1 curves %2 the anchor — %3 shared anchors").arg(parts.size()).arg(up ? QObject::tr("above") : QObject::tr("below")).arg(trunk.size());
}

// ---- ⚙ optimize anchors: every inner anchor that can go without the curve leaving its shape (3 % of a loop step or
// 0.3 % of its length) goes — the cheapest first; trunk mates go together; a junction stays ----
QString optimizeAnchors(Scene& sc, const CurveCache& cc, const std::vector<int>& cs, const std::set<int>* only) {
  (void)cc;
  auto all = byOrder(sc, ItemType::Curve);
  std::set<int> touched(cs.begin(), cs.end());
  for (int c : cs) for (auto* h : all) if (!touched.count(h->id)) for (int a : sc.item(c)->anchors) if (idxOf(h->anchors, a) >= 0) { touched.insert(h->id); break; }
  struct Info { std::vector<Vec3> ref; double tol; std::map<int, int> refIdx; };
  std::map<int, Info> info;
  for (int id : touched) { const Item* c = sc.item(id); Info I; for (auto& s : sampleCurve(sc, *c, 8)) I.ref.push_back(s.p);
    I.tol = std::max(polyLen(I.ref) * 0.003, loopStep(sc, *c) * 0.03); for (size_t k = 0; k < c->anchors.size(); k++) I.refIdx[c->anchors[k]] = int(k) * 8; info[id] = I; }
  auto localDev = [&](const Item& c, int i) {                          // c.anchors: the trial list (anchor i of the old one removed)
    const auto& A = c.anchors; const Info& I = info[c.id]; int lo = std::max(0, i - 2), hi = std::min(int(A.size()) - 1, i + 1);
    std::map<int, Handles> H; for (int k = lo; k <= hi; k++) H[k] = curveHandles(sc, c, k);
    std::vector<Vec3> s; for (int k = lo; k < hi; k++) for (int q = 0; q <= 12; q++) s.push_back(bez({P(sc, A[size_t(k)]), H[k].hout, H[k + 1].hin, P(sc, A[size_t(k + 1)])}, q / 12.0));
    double mx = 0; auto r0 = I.refIdx.find(A[size_t(lo)]), r1 = I.refIdx.find(A[size_t(hi)]); if (r0 == I.refIdx.end() || r1 == I.refIdx.end()) return 1e300;
    for (int r = r0->second; r <= r1->second; r++) { const Vec3& p = I.ref[size_t(r)]; double d = 1e300; for (size_t k = 0; k + 1 < s.size() && d > 1e-9; k++) d = std::min(d, distToSeg(p, s[k], s[k + 1])); mx = std::max(mx, d); }
    return mx;
  };
  // the curves anchor a (inner anchor of c) goes from: c alone, c with its trunk mates, or none (a junction — it stays)
  auto groupOf = [&](int cid, int a) -> std::optional<std::vector<int>> {
    const auto& CA = sc.item(cid)->anchors; int i = idxOf(CA, a); std::set<int> nb{CA[size_t(i - 1)], CA[size_t(i + 1)]}; std::vector<int> G{cid};
    for (auto* h : byOrder(sc, ItemType::Curve)) {
      if (h->id == cid || idxOf(h->anchors, a) < 0 || h->hasPath()) continue;
      int k = idxOf(h->anchors, a);
      if (h->ring >= 0 || h->closed || h->junc || !touched.count(h->id) || k <= 0 || k >= int(h->anchors.size()) - 1 || !nb.count(h->anchors[size_t(k - 1)]) || !nb.count(h->anchors[size_t(k + 1)])) return std::nullopt;
      G.push_back(h->id);
    }
    return G;
  };
  struct Res { int g, i; std::vector<int> A; OvrMap O; };
  auto trial = [&](const std::vector<int>& G, int a, bool keepShape, std::vector<Res>& res) {
    res.clear();
    for (int g : G) { Item* it = sc.item(g); std::vector<int> A0 = it->anchors; OvrMap O0 = it->ovr; int i = idxOf(A0, a);
      if (keepShape) edit::removeAnchorFromCurve(sc, *it, a, true); else { it->anchors.erase(it->anchors.begin() + i); it->ovr.erase(a); }
      res.push_back({g, i, it->anchors, it->ovr}); it->anchors = A0; it->ovr = O0; }
    std::vector<std::pair<std::vector<int>, OvrMap>> saved; for (auto& r : res) { Item* it = sc.item(r.g); saved.push_back({it->anchors, it->ovr}); it->anchors = r.A; it->ovr = r.O; }
    double d = 0; for (auto& r : res) d = std::max(d, localDev(*sc.item(r.g), r.i) / info[r.g].tol);
    for (size_t k = 0; k < res.size(); k++) { Item* it = sc.item(res[k].g); it->anchors = saved[k].first; it->ovr = saved[k].second; }
    return d;
  };
  int removed = 0;
  for (int cid : cs) {
    const Item* c = sc.item(cid); if (!c || c->ring >= 0 || c->closed || c->anchors.size() < 3) continue;
    struct C { double d; bool keep; std::vector<int> G; bool ok; }; std::map<int, C> cache;
    for (;;) {
      std::optional<std::pair<int, C>> best;
      const auto& A = sc.item(cid)->anchors;
      for (size_t i = 1; i + 1 < A.size(); i++) {
        int a = A[i]; const Anchor* an = sc.anchor(a); if (an->raw.value("at").isObject() || (only && !only->count(a))) continue;
        if (!cache.count(a)) {
          auto G = groupOf(cid, a); C r{0, false, {}, false};
          if (G) { bool ok = true; for (int g : *G) if (sc.item(g)->anchors.size() <= 2) ok = false;
            if (ok) { std::vector<Res> res; double d = trial(*G, a, false, res); if (d < 1) r = {d, false, *G, true}; else { double d2 = trial(*G, a, true, res); if (d2 < 1) r = {d2, true, *G, true}; } } }
          cache[a] = r;
        }
        const C& r = cache[a]; if (r.ok && (!best || r.d < best->second.d)) best = std::pair{a, r};
      }
      if (!best) break;
      std::vector<Res> res; trial(best->second.G, best->first, best->second.keep, res);
      for (auto& r : res) { Item* it = sc.item(r.g); it->anchors = r.A; it->ovr = r.O; }
      removed++;
      int i = res[0].i; const auto& A2 = sc.item(cid)->anchors;
      for (int k = std::max(0, i - 4); k <= std::min(int(A2.size()) - 1, i + 3); k++) cache.erase(A2[size_t(k)]);
    }
  }
  if (removed) { std::vector<int> si, sa; CurveCache none; Network net(sc, none, si, sa); net.repairPathCurves(); }
  sc.reindex();
  return removed ? QObject::tr("Removed %1 anchor%2 — the shape is kept").arg(removed).arg(removed > 1 ? "s" : "") : "!" + QObject::tr("Nothing to remove without changing the shape");
}

// ---- ⇶ linked anchors ----
std::vector<int> linkSync(Scene& sc, std::map<int, Vec3>& last) {
  std::map<int, int> A;
  for (const Item* c : sc.curves()) for (int a : c->anchors) if (const Anchor* an = sc.anchor(a); an && an->raw.contains("lk") && !an->raw.value("lk").isNull()) A[a] = an->raw.value("lk").toInt();
  if (A.empty()) { last.clear(); return {}; }
  std::set<int> done; std::vector<int> moved;
  for (auto& [a, lk] : A) {
    if (done.count(a)) continue;
    auto fb = A.find(lk); if (fb == A.end() || fb->second != a) { sc.anchor(a)->raw.remove("lk"); continue; }
    int b = lk; done.insert(a); done.insert(b);
    Anchor *pa = sc.anchor(a), *pb = sc.anchor(b);
    if ((pa->pos - pb->pos).lengthSq() > 0) {
      auto la = last.find(a), lb = last.find(b); bool am = la == last.end() || (la->second - pa->pos).lengthSq() > 0, bm = lb == last.end() || (lb->second - pb->pos).lengthSq() > 0;
      if (bm && !am) { pa->pos = pb->pos; moved.push_back(a); } else { pb->pos = pa->pos; moved.push_back(b); }
    }
    last[a] = pa->pos; last[b] = pb->pos;
  }
  return moved;
}

// ---- ⇶ snap to path ----
bool SnapTrack::has(const Scene& sc, int a) const { for (auto& s : steps) if (idxOf(sc.item(s.c)->anchors, a) >= 0) return true; return false; }

std::optional<SnapTrack> snapTrack(Scene& sc, const CurveCache& cc, const std::vector<int>& ids) {
  std::vector<int> cs; for (int id : ids) if (const Item* c = sc.item(id); c && c->type == ItemType::Curve && c->ring < 0 && !c->junc && !c->hasPath() && c->anchors.size() >= 2) cs.push_back(id);
  if (cs.empty()) return std::nullopt;
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  auto end = [&](int c, int a) { const auto& A = sc.item(c)->anchors; return A.front() == a || A.back() == a; };
  auto other = [&](int c, int a) { const auto& A = sc.item(c)->anchors; return A.front() == a ? A.back() : A.front(); };
  auto fr = [&](int c, int a) { for (int h : cs) if (h != c && end(h, a)) return false; return true; };
  int start = -1, sa0 = -1;
  for (int c : cs) for (int a : {sc.item(c)->anchors.front(), sc.item(c)->anchors.back()}) if (fr(c, a) && (sa0 < 0 || P(sc, a).y > P(sc, sa0).y)) { start = c; sa0 = a; }
  if (start < 0) { start = cs[0]; sa0 = sc.item(start)->anchors.front(); }
  SnapTrack T; T.ids = ids; T.steps.push_back({start, sa0}); std::set<int> used{start};
  for (size_t g = 0; g < cs.size(); g++) {
    auto l = T.steps.back(); int at = other(l.c, l.from); Vec3 din = -net.leaveDir(*sc.item(l.c), at);
    std::vector<int> nx; for (int h : cs) if (!used.count(h) && end(h, at)) nx.push_back(h);
    std::stable_sort(nx.begin(), nx.end(), [&](int x, int y) { return net.leaveDir(*sc.item(y), at).dot(din) < net.leaveDir(*sc.item(x), at).dot(din); });
    if (nx.empty()) break; T.steps.push_back({nx[0], at}); used.insert(nx[0]);
  }
  for (size_t q = 0; q < T.steps.size(); q++) {
    int c = T.steps[q].c, from = T.steps[q].from; const auto& A = sc.item(c)->anchors; bool fwd = A.front() == from;
    CurveSampling S = fresh(sc, cc, c); int n = int(S.p.size()), N = int(A.size()), base = int(T.pts.size()) - (q ? 1 : 0);
    for (int i = 0; i < n; i++) { int ii = fwd ? i : n - 1 - i; if (q && !i) continue; T.pts.push_back(S.p[size_t(ii)]); T.meta.push_back({c, std::min(S.seg[size_t(ii)], N - 2)}); }
    for (int j = q ? 1 : 0; j < N; j++) { int k = fwd ? j : N - 1 - j, s = ancSample(S, N, k); T.A.push_back({A[size_t(k)], c, k, base + (fwd ? s : n - 1 - s)}); }
  }
  T.arc = {0}; for (size_t i = 1; i < T.pts.size(); i++) T.arc.push_back(T.arc.back() + (T.pts[i] - T.pts[i - 1]).length());
  QStringList nm; for (auto& s : T.steps) nm << sc.item(s.c)->name;
  T.name = nm.size() > 3 ? QString("%1 … %2 (%3 curves)").arg(nm.first(), nm.last()).arg(nm.size()) : nm.join(" → ");
  return T;
}

namespace {
int snapNearest(const SnapTrack& T, const Vec3& p, int from = 0, int d = 0) {
  int bi = -1; double bd = 1e300; int n = int(T.pts.size());
  for (int i = d > 0 ? from : 0; i < (d < 0 ? from + 1 : n); i++) { double q = (T.pts[size_t(i)] - p).lengthSq(); if (q < bd) { bd = q; bi = i; } }
  return bi;
}
struct Way { bool up; std::vector<SnapSeg> segs; };
std::vector<Way> snapWays(Scene& sc, const CurveCache& cc, int a) {
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  std::vector<Way> out; std::set<std::string> seen;
  auto add = [&](bool up, std::vector<SnapSeg> segs) { if (segs.empty()) return; std::string k = up ? "up" : "down"; for (auto& s : segs) k += std::to_string(s.c) + ":" + std::to_string(s.k1) + ","; if (seen.insert(k).second) out.push_back({up, segs}); };
  for (int cid : curvesWithOrd(sc, a)) {
    const Item* c = sc.item(cid); if (c->ring >= 0 || c->closed || c->junc || c->hasPath() || c->anchors.size() < 2) continue;
    std::vector<NetPath> Ps;
    if (segNet(*c) && c->seg) Ps = net.curvePaths({cid}, true);
    else Ps = {{PathStep{cid, P(sc, c->anchors.front()).y >= P(sc, c->anchors.back()).y ? c->anchors.front() : c->anchors.back()}}};
    for (auto& p : Ps) {
      std::vector<SnapSeg> segs;
      for (auto& st : p) { const auto& H = sc.item(st.c)->anchors; int i0 = idxOf(H, st.from), d = i0 == 0 ? 1 : -1; for (int q = i0; q + d >= 0 && q + d < int(H.size()); q += d) segs.push_back({st.c, q, q + d}); }
      int at = -1; for (size_t k = 0; k < segs.size(); k++) if (sc.item(segs[k].c)->anchors[size_t(segs[k].k0)] == a) { at = int(k); break; }
      bool atEnd = !segs.empty() && sc.item(segs.back().c)->anchors[size_t(segs.back().k1)] == a;
      int i = at >= 0 ? at : atEnd ? int(segs.size()) : -1; if (i < 0) continue;
      std::vector<SnapSeg> upS; for (int k = i - 1; k >= 0; k--) upS.push_back({segs[size_t(k)].c, segs[size_t(k)].k1, segs[size_t(k)].k0});
      add(true, upS); add(false, std::vector<SnapSeg>(segs.begin() + i, segs.end()));
    }
  }
  return out;
}
}  // namespace

std::vector<SnapOption> snapOptions(Scene& sc, const CurveCache& cc, int a, const SnapTrack& T) {
  int n = int(T.pts.size()); if (n < 2) return {};
  int i0 = snapNearest(T, P(sc, a)); const auto& arc = T.arc; std::vector<SnapOption> out;
  double gap0 = (T.pts[size_t(i0)] - P(sc, a)).length(), far = 0.2 * levelH(sc) + 2 * gap0;
  auto offTrack = [&](const Vec3& p) { double m = 1e300; for (int i = 0; i < n; i += 2) m = std::min(m, (T.pts[size_t(i)] - p).lengthSq()); return std::sqrt(m) > far; };
  for (auto& w : snapWays(sc, cc, a)) {
    int d = w.up ? -1 : 1; double LT = d > 0 ? arc[size_t(n - 1)] - arc[size_t(i0)] : arc[size_t(i0)];
    if (LT < 0.05 * levelH(sc)) continue;
    double L = 0, bd = 1e300; int best = -1;
    for (size_t q = 0; q < w.segs.size(); q++) {
      const auto& s = w.segs[q]; auto st = curveStretch(sc, fresh(sc, cc, s.c), s.c, s.k0, s.k1);
      if (offTrack(P(sc, sc.item(s.c)->anchors[size_t(s.k1)]))) break;
      for (size_t j = 1; j < st.size(); j++) L += (st[j] - st[j - 1]).length();
      if (std::abs(L - LT) < bd) { bd = std::abs(L - LT); best = int(q); }
      if (L > 1.4 * LT) break;
    }
    if (best < 0) continue;
    std::vector<SnapSeg> segs(w.segs.begin(), w.segs.begin() + best + 1); int E = sc.item(segs.back().c)->anchors[size_t(segs.back().k1)];
    std::vector<Vec3> cut{P(sc, a)}; for (auto& s : segs) { auto st = curveStretch(sc, fresh(sc, cc, s.c), s.c, s.k0, s.k1); cut.insert(cut.end(), st.begin() + 1, st.end()); }
    int iE = snapNearest(T, P(sc, E), i0, d); std::vector<Vec3> tp(T.pts.begin() + std::min(i0, iE), T.pts.begin() + std::max(i0, iE) + 1);
    QStringList nm; for (auto& s : segs) if (!nm.contains(sc.item(s.c)->name)) nm << sc.item(s.c)->name;
    bool dup = false; for (auto& o : out) if (o.E == E && o.up == w.up && o.segs.size() == segs.size()) { bool same = true; for (size_t k = 0; k < segs.size(); k++) if (o.segs[k].c != segs[k].c) same = false; if (same) dup = true; }
    if (dup) continue;
    out.push_back({d, w.up, segs, E, cut, tp, nm.join(" → "), L < 0.9 * LT});
  }
  return out;
}

QString snapApply(Scene& sc, const CurveCache& cc, Selection& sel, int a, const std::vector<int>& trackIds, const SnapOption& o) {
  auto T = snapTrack(sc, cc, trackIds); if (!T) return "!";
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  struct X { int an, c, k; const SnapSeg* next; };
  std::vector<X> seq{{a, o.segs[0].c, o.segs[0].k0, nullptr}};
  for (size_t q = 0; q < o.segs.size(); q++) seq.push_back({sc.item(o.segs[q].c)->anchors[size_t(o.segs[q].k1)], o.segs[q].c, o.segs[q].k1, q + 1 < o.segs.size() ? &o.segs[q + 1] : nullptr});
  auto users = [&](int an) { int n = 0; for (const Item* c : sc.curves()) if (!c->junc && !c->hasPath() && idxOf(c->anchors, an) >= 0) n++; return n; };
  std::vector<X> keep; for (size_t q = 0; q < seq.size(); q++) { auto& x = seq[q]; if (q == 0 || q + 1 == seq.size() || users(x.an) > 1 || (x.next && x.next->c != x.c)) keep.push_back(x); }
  std::map<int, int> pair; int prev = -1; double lh = levelH(sc);
  auto atA = [&](int an) -> const SnapTrack::Anc* { for (auto& e : T->A) if (e.an == an) return &e; return nullptr; };
  for (auto& x : keep) {
    const SnapTrack::Anc* pe = prev >= 0 ? atA(prev) : nullptr;
    int si2 = snapNearest(*T, P(sc, x.an), pe ? pe->si : 0, pe ? o.d : 0);
    SnapTrack::Anc lo = T->A[0]; std::optional<SnapTrack::Anc> hi; for (auto& e : T->A) { if (e.si <= si2) lo = e; if (e.si >= si2 && !hi) hi = e; } if (!hi) hi = lo;
    double span = std::max(T->arc[size_t(hi->si)] - T->arc[size_t(lo.si)], 1e-9);
    int Q = (T->arc[size_t(si2)] - T->arc[size_t(lo.si)]) <= 0.2 * span ? lo.an : (T->arc[size_t(hi->si)] - T->arc[size_t(si2)]) < 0.2 * span ? hi->an : -1;
    if (Q >= 0) { bool taken = Q == prev; for (auto& [k, v] : pair) if (v == Q) taken = true; if (taken) Q = -1; }
    if (Q < 0 && pe && std::abs(T->arc[size_t(si2)] - T->arc[size_t(pe->si)]) < 0.02 * lh) continue;
    if (Q < 0) { auto m = T->meta[size_t(si2)]; Anchor& nq = sc.addAnchor(T->pts[size_t(si2)]); nq.smooth = 1; Q = nq.id; net.insertOnSegment(m.first, m.second, Q); sc.reindex(); T = snapTrack(sc, cc, trackIds); }
    pair[x.an] = Q; prev = Q;
  }
  std::vector<X> K; for (auto& x : keep) if (pair.count(x.an)) K.push_back(x);
  if (K.size() < 2) return "!" + QObject::tr("⇶ The part and the target don't run along each other");
  struct G { int an; std::optional<SnapTrack::Anc> e; }; std::vector<G> glued;
  for (size_t q = 0; q + 1 < K.size(); q++) {
    auto &x = K[q], &y = K[q + 1]; Item* c = sc.item(y.c); int i = idxOf(c->anchors, x.an), j = idxOf(c->anchors, y.an);
    const SnapTrack::Anc *ex = atA(pair[x.an]), *ey = atA(pair[y.an]); if (i < 0 || j < 0 || !ex || !ey) continue;
    int ip = int(ex - &T->A[0]), iq = int(ey - &T->A[0]);
    std::vector<SnapTrack::Anc> span; if (ip < iq) span.assign(T->A.begin() + ip + 1, T->A.begin() + iq); else { span.assign(T->A.begin() + iq + 1, T->A.begin() + ip); std::reverse(span.begin(), span.end()); }
    std::vector<int> mid;
    for (auto& e : span) { Anchor& n = sc.addAnchor(P(sc, e.an)); n.smooth = sc.anchor(e.an)->smooth; n.raw.insert("lk", e.an); sc.anchor(e.an)->raw.insert("lk", n.id); glued.push_back({n.id, e}); mid.push_back(n.id); }
    int lo = std::min(i, j), hi = std::max(i, j); for (int k = lo + 1; k < hi; k++) c->ovr.erase(c->anchors[size_t(k)]);
    if (i < j) { c->anchors.erase(c->anchors.begin() + i + 1, c->anchors.begin() + j); c->anchors.insert(c->anchors.begin() + i + 1, mid.begin(), mid.end()); }
    else { c->anchors.erase(c->anchors.begin() + j + 1, c->anchors.begin() + i); std::reverse(mid.begin(), mid.end()); c->anchors.insert(c->anchors.begin() + j + 1, mid.begin(), mid.end()); }
  }
  sc.reindex();
  for (auto& x : K) { int Q = pair[x.an]; sc.anchor(x.an)->pos = P(sc, Q); sc.anchor(x.an)->raw.insert("lk", Q); sc.anchor(Q)->raw.insert("lk", x.an);
    const SnapTrack::Anc* e = atA(Q); glued.push_back({x.an, e ? std::optional<SnapTrack::Anc>(*e) : std::nullopt}); for (int c : curvesWithOrd(sc, x.an)) sc.item(c)->ovr.erase(x.an); }
  // the handles along it: the track's frozen ones where it has them (turned when our curve runs the other way)
  for (auto& [an, e] : glued) {
    if (!e) continue; const Item* tc = sc.item(e->c); auto ov = tc->ovr.find(e->an); if (ov == tc->ovr.end()) continue;
    for (int cid : curvesWithOrd(sc, an)) { Item* c = sc.item(cid); if (c->junc || c->hasPath()) continue;
      int i = idxOf(c->anchors, an), ti = idxOf(tc->anchors, e->an);
      int nb = i + 1 < int(c->anchors.size()) ? c->anchors[size_t(i + 1)] : -1, tb = ti + 1 < int(tc->anchors.size()) ? tc->anchors[size_t(ti + 1)] : -1;
      bool same = nb >= 0 && tb >= 0 ? sc.anchor(nb)->raw.value("lk").toInt(-1) == tb : true;
      HandleOverride h = ov->second; if (!same) std::swap(h.in, h.out); c->ovr[an] = h; }
  }
  net.repairPathCurves(); sc.reindex();
  sel.items.clear(); sel.anchors = {a};
  return QObject::tr("⇶ %1 part glued onto %2%3: %4 linked anchor pairs — move either, both follow").arg(o.up ? QObject::tr("Upper") : QObject::tr("Lower"), T->name).arg(o.part ? QObject::tr(" (partly)") : QString()).arg(glued.size());
}

// ---- ⌖ paths ----
std::vector<NetPath> pathsFor(Scene& sc, const CurveCache& cc, const std::vector<int>& sel) {
  std::vector<int> si, sa; Network net(sc, cc, si, sa); std::vector<NetPath> out; std::set<std::string> seen;
  for (auto it = sel.rbegin(); it != sel.rend(); ++it) for (auto& p : net.curvePaths({*it}, false)) { std::string k; for (auto& s : p) k += std::to_string(s.c) + ","; if (seen.insert(k).second) out.push_back(p); }
  return out;
}
std::vector<Vec3> pathPts(const Scene& sc, const CurveCache& cc, const NetPath& p) {
  std::vector<Vec3> o; for (auto& s : p) { auto S = fresh(sc, cc, s.c); if (sc.item(s.c)->anchors.front() == s.from) o.insert(o.end(), S.p.begin(), S.p.end()); else o.insert(o.end(), S.p.rbegin(), S.p.rend()); }
  return o;
}
QString selectPath(Scene& sc, const CurveCache& cc, Selection& sel, const NetPath& p) {
  sel.anchors.clear(); std::vector<int> items; for (int x : sel.items) if (const Item* it = sc.item(x); it && it->type == ItemType::Curve) items.push_back(x);
  bool off = true; for (auto& s : p) if (idxOf(items, s.c) < 0) off = false;
  std::set<int> keep;
  if (off) {
    std::vector<int> cur; for (int x : items) if (segNet(*sc.item(x))) cur.push_back(x);
    auto key = [](const NetPath& q) { std::string k; for (auto& s : q) k += std::to_string(s.c) + ","; return k; };
    for (auto& q : pathsFor(sc, cc, cur)) { if (key(q) == key(p)) continue; bool all = true; for (auto& s : q) if (idxOf(items, s.c) < 0) all = false; if (all) for (auto& s : q) keep.insert(s.c); }
  }
  for (auto& s : p) { if (keep.count(s.c)) continue; items.erase(std::remove(items.begin(), items.end(), s.c), items.end()); if (!off) items.push_back(s.c); }
  sel.items = items;
  QStringList n; for (auto& s : p) n << sc.item(s.c)->name;
  return QObject::tr("Path %1: %2 (%3 selected)").arg(off ? QObject::tr("taken off") : QObject::tr("added"), n.join(" → ")).arg(items.size());
}

// ---- ⤳ routes ----
std::vector<RouteAlt> routeAlternatives(Scene& sc, const CurveCache& cc, int cid) {
  const Item* c = sc.item(cid); std::vector<int> si, sa; Network net(sc, cc, si, sa);
  std::vector<int> cur; if (!c->hasPath()) cur = {cid}; else for (auto& p : c->path) if (sc.item(p[0])) cur.push_back(p[0]);
  std::set<int> curSet(cur.begin(), cur.end()); const auto& L = c->anchors;
  auto key = [](const std::vector<int>& A) { std::string k; for (int a : A) k += std::to_string(a) + ","; return k; };
  std::set<std::string> seen{key(L), key(std::vector<int>(L.rbegin(), L.rend()))};
  std::map<int, int> ends; for (const Item* q : sc.curves()) if (segNet(*q)) for (int a : {q->anchors.front(), q->anchors.back()}) ends[a]++;
  auto users = [&](int a) { int n = 0; for (const Item* q : sc.curves()) if (segNet(*q) && idxOf(q->anchors, a) >= 0) n++; return n; };
  auto fr = [&](int a) { return users(a) <= 1 && ends.count(a) && ends[a] == 1; };
  std::vector<RouteAlt> out;
  for (int s : cur) for (auto& p : net.curvePaths({s}, false)) {
    std::vector<int> N; for (auto& st : p) { auto A = sc.item(st.c)->anchors; if (A.front() != st.from) std::reverse(A.begin(), A.end()); for (int a : A) if (N.empty() || N.back() != a) N.push_back(a); }
    auto near = [&](const std::vector<int>& Q, int a) { int b = 0; for (size_t i = 0; i < Q.size(); i++) if ((P(sc, Q[i]) - P(sc, a)).length() < (P(sc, Q[size_t(b)]) - P(sc, a)).length()) b = int(i); return b; };
    if (near(N, L.front()) > near(N, L.back())) std::reverse(N.begin(), N.end());
    auto cut = [&](const std::vector<int>& A, int a) { int i = idxOf(A, a); if (i >= 0) return i; if (fr(a)) return -1;
      std::vector<int> nodes; for (size_t k = 0; k < A.size(); k++) if (ends.count(A[k])) nodes.push_back(int(k)); if (nodes.empty()) return -1;
      int b = nodes[0]; for (int k : nodes) if ((P(sc, A[size_t(k)]) - P(sc, a)).length() < (P(sc, A[size_t(b)]) - P(sc, a)).length()) b = k; return b; };
    int j1 = cut(N, L.back()); if (j1 >= 0) N.resize(size_t(j1 + 1));
    int j0 = cut(N, L.front()); if (j0 > 0) N.erase(N.begin(), N.begin() + j0);
    if (N.size() < 2 || seen.count(key(N)) || seen.count(key(std::vector<int>(N.rbegin(), N.rend())))) continue; seen.insert(key(N));
    auto st = net.routeStretches(N); if (!st) continue;
    RouteAlt r; r.N = N; r.st = *st; std::vector<int> now; for (auto& x : *st) now.push_back(x.c);
    for (int q : now) { if (curSet.count(q)) r.shared++; else r.add.push_back(q); }
    for (int q : cur) if (idxOf(now, q) < 0) r.drop.push_back(q);
    if (!r.add.empty() || !r.drop.empty()) out.push_back(r);
  }
  std::stable_sort(out.begin(), out.end(), [](const RouteAlt& a, const RouteAlt& b) { if (a.shared != b.shared) return a.shared > b.shared; return a.add.size() + a.drop.size() < b.add.size() + b.drop.size(); });
  if (out.size() > 16) out.resize(16);
  return out;
}

QString setRoute(Scene& sc, const CurveCache& cc, Selection& sel, int cid, const std::vector<int>& N, int wall, char side) {
  Network net(sc, cc, sel.items, sel.anchors);
  auto st = net.routeStretches(N); if (!st) return "!" + QObject::tr("That route can't be laid along the curves");
  Item* c = sc.item(cid);
  bool otherUsers = false; for (auto& [id, s] : sc.items) if (s.type == ItemType::Surface && id != wall && (s.a == cid || s.b == cid)) otherUsers = true;
  bool tubeUse = false; for (auto& [id, t] : sc.items) if (t.type == ItemType::Tube && t.raw.value("src").toInt(-1) == cid) tubeUse = true;
  if (c->hasPath() && (wall < 0 || (!otherUsers && !tubeUse))) {
    c->path.clear(); for (auto& x : *st) { const Item* q = sc.item(x.c); c->path.push_back({x.c, q->anchors[size_t(x.i0)], q->anchors[size_t(x.i1)]}); }
    if (auto L = net.pathAnchors(c->path)) c->anchors = *L;
  } else if (wall >= 0) {                                                // a real curve, or an EC other walls use too: a new EC for this wall
    int e = net.pathCurve(*st, sc.item(wall)->parent); Item* w = sc.item(wall); if (side == 'a') w->a = e; else w->b = e;
  }
  // (the old EC of that side, now used by nothing, goes)
  if (wall >= 0 && sc.item(cid) && sc.item(cid)->junc) {
    bool used = false; for (auto& [id, s] : sc.items) if (s.type == ItemType::Surface && (s.a == cid || s.b == cid)) used = true;
    for (const Item* q : sc.curves()) for (auto& p : q->path) if (p[0] == cid) used = true;
    if (!used && !tubeUse) { sel.items.erase(std::remove(sel.items.begin(), sel.items.end(), cid), sel.items.end()); net.deleteItem(cid); }
  }
  sc.reindex();
  QStringList n; for (auto& x : *st) n << sc.item(x.c)->name;
  return QObject::tr("Route changed: %1").arg(n.join(" → "));
}

// ---- ✂ separate a trunk ----
std::vector<TrunkOpt> trunkSplitOptions(Scene& sc, const CurveCache& cc, int Tid) {
  (void)cc; const Item* T = sc.item(Tid); if (!segNet(*T) || !T->seg || T->anchors.size() < 2) return {};
  std::vector<const Item*> net; for (const Item* c : byOrder(sc, ItemType::Curve)) if (segNet(*c) && c->anchors.size() >= 2) net.push_back(c);
  std::vector<TrunkOpt> out;
  for (int e : {0, 1}) {
    int X = e ? T->anchors.back() : T->anchors.front();
    std::vector<const Item*> br; for (auto* c : net) if (c != T && (c->anchors.front() == X || c->anchors.back() == X)) br.push_back(c);
    bool inner = false; for (auto* c : net) { int i = idxOf(c->anchors, X); if (c != T && i > 0 && i < int(c->anchors.size()) - 1) inner = true; }
    if (br.size() >= 2 && !inner) for (auto* P2 : br) out.push_back({e, X, P2->id});
  }
  return out;
}

QString separateTrunk(Scene& sc, const CurveCache& cc, Selection& sel, int Tid, const TrunkOpt& o) {
  Network net(sc, cc, sel.items, sel.anchors);
  Item* T = sc.item(Tid); const Item* Pc = sc.item(o.P); std::vector<int> A = T->anchors; int n = int(A.size()), far = o.e ? A[0] : A[size_t(n - 1)], X = o.X;
  auto users = [&](int a) { int k = 0; for (const Item* c : sc.curves()) if (segNet(*c) && idxOf(c->anchors, a) >= 0) k++; return k; };
  bool farFree = users(far) == 1;
  int pn = Pc->anchors.front() == X ? Pc->anchors[1] : Pc->anchors[Pc->anchors.size() - 2];
  int tn = o.e ? A[size_t(n - 2)] : A[1];
  Vec3 t = (P(sc, tn) - P(sc, X)).normalized(), side = P(sc, pn) - P(sc, X); side = side - t * side.dot(t);
  std::vector<Vec3> AP; for (int a : A) AP.push_back(P(sc, a)); double L = polyLen(AP), mag = std::min(0.25 * (P(sc, pn) - P(sc, X)).length(), L / 3);
  if (side.lengthSq() > 1e-14) side = side.normalized() * mag;
  std::vector<double> fr(static_cast<size_t>(n)); for (int k = 0; k < n; k++) { double s = 0; for (int j = 1; j <= k; j++) s += (AP[size_t(j)] - AP[size_t(j - 1)]).length(); fr[size_t(k)] = L ? s / L : 0; }
  auto wAt = [&](int k) { double u = o.e ? 1 - fr[size_t(k)] : fr[size_t(k)]; return farFree ? std::sin(std::min(u, 1.0) * M_PI / 2) : std::sin(u * M_PI); };
  std::map<int, int> map;
  for (int k = 0; k < n; k++) { int a = A[size_t(k)]; if (a == far && !farFree) continue;
    Anchor& b = sc.addAnchor(P(sc, a) + side * (a == X ? 0 : wAt(k))); b.smooth = sc.anchor(a)->smooth; map[a] = b.id; }
  Item& Tc = net.makeCurve(T->parent); int tcid = Tc.id; T = sc.item(Tid);
  Tc.seg = true; Tc.round = T->round; Tc.roundSet = T->roundSet; Tc.fair = T->fair; Tc.fairSet = T->fairSet; Tc.visible = T->visible; Tc.name = T->name + " copy";
  for (int a : A) Tc.anchors.push_back(map.count(a) ? map[a] : a);
  for (auto& [k, ov] : T->ovr) if (idxOf(A, k) >= 0) Tc.ovr[map.count(k) ? map[k] : k] = ov;
  { auto& arr = T->parent >= 0 ? sc.item(T->parent)->children : sc.roots; arr.erase(std::remove(arr.begin(), arr.end(), tcid), arr.end()); arr.insert(std::find(arr.begin(), arr.end(), Tid) + 1, tcid); }
  // the branch onto the copy's joint anchor (its frozen handles there with it)
  int X2 = map[X]; Item* Pm = sc.item(o.P); int i = idxOf(Pm->anchors, X); Pm->anchors[size_t(i)] = X2;
  if (auto ov = Pm->ovr.find(X); ov != Pm->ovr.end()) { HandleOverride h = ov->second; Pm->ovr.erase(X); Pm->ovr[X2] = h; }
  sc.reindex();
  // service curves along branch → trunk: along branch → copy
  for (const Item* cc0 : byOrder(sc, ItemType::Curve)) {
    Item* c = sc.item(cc0->id); if (!c->hasPath()) continue;
    const auto Lc = c->anchors; int j = idxOf(Lc, X); if (j < 0) continue;
    int dir = j > 0 && Lc[size_t(j - 1)] == pn ? 1 : j + 1 < int(Lc.size()) && Lc[size_t(j + 1)] == pn ? -1 : 0; if (!dir) continue;
    std::vector<int> L2 = Lc; L2[size_t(j)] = X2;
    for (int q = j + dir; q >= 0 && q < int(L2.size()) && idxOf(A, Lc[size_t(q)]) >= 0; q += dir) L2[size_t(q)] = map.count(Lc[size_t(q)]) ? map[Lc[size_t(q)]] : Lc[size_t(q)];
    auto st = net.routeStretches(L2); if (!st) continue;
    c->path.clear(); for (auto& x : *st) { const Item* q = sc.item(x.c); c->path.push_back({x.c, q->anchors[size_t(x.i0)], q->anchors[size_t(x.i1)]}); }
    if (auto LL = net.pathAnchors(c->path)) c->anchors = *LL; else c->anchors = L2;
  }
  sc.reindex();
  sel.anchors.clear(); sel.items = {tcid};
  return QObject::tr("%1 parted from %2 — a trunk of its own").arg(sc.item(o.P)->name, sc.item(Tid)->name);
}

// ---- ✂ split a wall into its parts ----
std::vector<Zone> wallZones(const Scene& sc, int w) {
  const Item* W = sc.item(w); if (!W) return {};
  const Item *A = sc.item(W->a), *B = sc.item(W->b); if (!A || !B || A == B || A->anchors.size() < 2 || B->anchors.size() < 2) return {};
  std::set<int> sh; for (int a : A->anchors) if (idxOf(B->anchors, a) >= 0) sh.insert(a);
  std::vector<int> jA; for (size_t k = 0; k < A->anchors.size(); k++) if (sh.count(A->anchors[k])) jA.push_back(int(k));
  bool rev = jA.size() >= 2 ? idxOf(B->anchors, A->anchors[size_t(jA.back())]) < idxOf(B->anchors, A->anchors[size_t(jA.front())])
                            : (P(sc, A->anchors.front()) - P(sc, B->anchors.back())).length() < (P(sc, A->anchors.front()) - P(sc, B->anchors.front())).length();
  int nB = int(B->anchors.size());
  auto bAt = [&](int k) { if (sh.count(A->anchors[size_t(k)])) return idxOf(B->anchors, A->anchors[size_t(k)]); return k == 0 ? (rev ? nB - 1 : 0) : (rev ? 0 : nB - 1); };
  std::set<int> ms{0, int(A->anchors.size()) - 1}; ms.insert(jA.begin(), jA.end()); std::vector<int> marks(ms.begin(), ms.end());
  std::vector<Zone> zones;
  for (size_t m = 0; m + 1 < marks.size(); m++) {
    int p = marks[m], q = marks[m + 1], jp = bAt(p), jq = bAt(q);
    bool trunk = sh.count(A->anchors[size_t(p)]) && sh.count(A->anchors[size_t(q)]) && q == p + 1 && std::abs(jq - jp) == 1;
    if (trunk || jp == jq) continue;
    zones.push_back({p, q, jp, jq});
  }
  return zones.size() >= 2 ? zones : std::vector<Zone>{};
}

QString splitWall(Scene& sc, const CurveCache& cc, Selection& sel, int w) {
  auto Z = wallZones(sc, w); const Item* W = sc.item(w);
  if (Z.empty()) return "!" + QObject::tr("%1: its sides never meet in the middle — nothing to split").arg(W->name);
  Network net(sc, cc, sel.items, sel.anchors);
  int A = W->a, B = W->b, par = W->parent; QString wn = W->name, col = W->color; QJsonObject p = W->raw.value("p").toObject(); if (p.isEmpty()) p = QJsonObject{{"segs", 1}, {"bulge", 0.15}, {"flip", false}};
  auto side = [&](int c, int i0, int i1) -> int {
    int lo = std::min(i0, i1), hi = std::max(i0, i1); const auto& CA = sc.item(c)->anchors; std::vector<int> L(CA.begin() + lo, CA.begin() + hi + 1);
    auto st = net.routeStretches(L); std::vector<Network::Stretch> s2;
    if (st) s2 = *st; else if (!sc.item(c)->hasPath()) s2 = {{c, lo, hi}};
    return s2.empty() ? -1 : net.pathCurve(s2, par);
  };
  std::vector<int> made;
  for (size_t k = 0; k < Z.size(); k++) {
    int a = side(A, Z[k].a0, Z[k].a1), b = side(B, Z[k].b0, Z[k].b1); if (a < 0 || b < 0 || a == b) continue;
    int sn = edit::nextNumber(sc, ItemType::Surface); Item& s = sc.addItem(ItemType::Surface, par, QString("Surface %1").arg(sn));
    s.a = a; s.b = b; s.raw = QJsonObject{{"p", p}}; s.name = QString("%1_%2").arg(wn).arg(k + 1); if (!col.isEmpty()) s.color = col; made.push_back(s.id);
  }
  if (made.size() < 2) { for (int s : made) net.deleteItem(s); return "!" + QObject::tr("%1: the parts couldn't be laid on its curves").arg(wn); }
  // (the new walls in the old one's place in its group)
  auto& arr = par >= 0 ? sc.item(par)->children : sc.roots;
  for (int s : made) arr.erase(std::remove(arr.begin(), arr.end(), s), arr.end());
  arr.insert(std::find(arr.begin(), arr.end(), w) + 1, made.begin(), made.end());
  net.deleteItem(w); sc.reindex();
  sel.anchors.clear(); sel.items = made;
  QStringList n; for (int s : made) n << sc.item(s)->name;
  return QObject::tr("%1 split into %2 walls: %3").arg(wn).arg(made.size()).arg(n.join(", "));
}

}  // namespace hm::aops
