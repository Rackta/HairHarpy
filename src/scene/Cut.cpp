#include "scene/Cut.h"
#include "scene/Edit.h"
#include "scene/Network.h"
#include "scene/Walls.h"
#include <optional>
#include <QJsonArray>
#include <algorithm>
#include <set>

namespace hm::cut {

namespace {
std::vector<Item*> byOrder(Scene& sc, ItemType t) {
  std::vector<Item*> v; for (auto& [id, it] : sc.items) if (it.type == t) v.push_back(&it);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->order < b->order; }); return v;
}
int idxOf(const std::vector<int>& v, int x) { auto it = std::find(v.begin(), v.end(), x); return it == v.end() ? -1 : int(it - v.begin()); }
QString pairKey(int x, int y) { return QString("%1_%2").arg(std::min(x, y)).arg(std::max(x, y)); }
int addWall(Scene& sc, int parent, int a, int b, const QJsonObject& p, bool visible) {
  int n = edit::nextNumber(sc, ItemType::Surface);
  Item& s = sc.addItem(ItemType::Surface, parent, QString("Surface %1").arg(n)); s.a = a; s.b = b; s.visible = visible; s.raw = QJsonObject{{"p", p}};
  return s.id;
}
}  // namespace

std::vector<std::array<int, 2>> splitAtAnchor(Scene& sc, const CurveCache& cc, Selection& sel, int J, std::map<int, int>& mergedInto) {
  Network net(sc, cc, sel.items, sel.anchors);
  std::vector<Item*> cut;
  for (Item* c : byOrder(sc, ItemType::Curve)) { int i = idxOf(c->anchors, J); if (c->ring < 0 && !c->closed && i > 0 && i < int(c->anchors.size()) - 1) cut.push_back(c); }
  std::vector<std::pair<int, std::array<int, 2>>> pieces;                // curve id → [first piece, second piece]
  auto pieceOf = [&](int id) -> const std::array<int, 2>* { for (auto& [k, v] : pieces) if (k == id) return &v; return nullptr; };
  for (Item* c0 : cut) {
    int cid = c0->id; Item* c = sc.item(cid);
    std::vector<int> A = c->anchors; int i = idxOf(A, J), n = int(A.size());
    auto ci = cc.find(cid); const std::vector<double>* L = ci != cc.end() && !ci->second.arc.empty() ? &ci->second.arc : nullptr;
    double tot = L ? (L->back() ? L->back() : 1) : 1;
    double cf = L ? std::clamp((*L)[std::min<size_t>(size_t(i) * 24, L->size() - 1)] / tot, 1e-6, 1 - 1e-6) : double(i) / (n - 1);
    std::vector<int> p1(A.begin(), A.begin() + i + 1), p2(A.begin() + i, A.end()); bool firstKeeps = p1.size() >= p2.size();
    Item& nb = net.makeCurve(c->parent); int nid = nb.id; c = sc.item(cid);
    nb.round = c->round; nb.roundSet = c->roundSet; nb.fair = c->fair; nb.fairSet = c->fairSet; nb.seg = c->seg; nb.visible = c->visible; nb.name = c->name + " part";
    nb.anchors = firstKeeps ? p2 : p1; c->anchors = firstKeeps ? p1 : p2;
    std::vector<int> moveO; for (auto& [aid, o] : c->ovr) if (idxOf(nb.anchors, aid) >= 0 && idxOf(c->anchors, aid) < 0) moveO.push_back(aid);
    for (int aid : moveO) { nb.ovr[aid] = c->ovr.find(aid)->second; c->ovr.erase(aid); }
    if (auto o = c->ovr.find(J); o != c->ovr.end()) nb.ovr[J] = o->second;     // (both pieces keep the cut's handles)
    auto& arr = c->parent >= 0 ? sc.item(c->parent)->children : sc.roots;
    arr.erase(std::remove(arr.begin(), arr.end(), nid), arr.end()); arr.insert(std::find(arr.begin(), arr.end(), cid) + 1, nid);
    std::array<int, 2> pr = firstKeeps ? std::array<int, 2>{cid, nid} : std::array<int, 2>{nid, cid};
    pieces.push_back({cid, pr});
    for (Item* g : byOrder(sc, ItemType::Guide)) {                      // a guide's hits follow the pieces
      if (g->raw.value("kind").toString() == "disc") continue;
      QJsonArray hits = g->raw.value("hits").toArray(); bool ch = false;
      for (int k = 0; k < hits.size(); k++) { QJsonObject h = hits[k].toObject(); if (h.value("c").toInt() != cid) continue;
        double t = h.value("t").toDouble(); bool first = t <= cf; h.insert("c", pr[first ? 0 : 1]); h.insert("t", first ? t / cf : (t - cf) / (1 - cf)); hits[k] = h; ch = true; }
      if (ch) g->raw.insert("hits", hits);
    }
  }
  // walls between two cut curves: first pieces together, second pieces together
  for (Item* s : byOrder(sc, ItemType::Surface)) {
    const auto *pa = pieceOf(s->a), *pb = pieceOf(s->b); if (!pa || !pb) continue;
    s->a = (*pa)[0]; s->b = (*pb)[0]; int sid = s->id;
    addWall(sc, s->parent, (*pa)[1], (*pb)[1], s->raw.value("p").toObject(), s->visible); (void)sid;
  }
  // identical pieces (a shared trunk) → one curve
  std::vector<int> ids; for (auto& [k, v] : pieces) { ids.push_back(v[0]); ids.push_back(v[1]); }
  auto same = [](const std::vector<int>& x, const std::vector<int>& y) {
    if (x.size() != y.size()) return false; bool f = true, r = true;
    for (size_t k = 0; k < x.size(); k++) { if (x[k] != y[k]) f = false; if (x[k] != y[y.size() - 1 - k]) r = false; }
    return f || r;
  };
  for (size_t i = 0; i < ids.size(); i++) for (size_t j = i + 1; j < ids.size(); j++) {
    Item *a = sc.item(ids[i]), *b = sc.item(ids[j]); if (!a || !b || !same(a->anchors, b->anchors)) continue;
    int aid = a->id, bid = b->id;
    for (Item* s : byOrder(sc, ItemType::Surface)) { if (s->a == bid) s->a = aid; if (s->b == bid) s->b = aid; }
    for (Item* g : byOrder(sc, ItemType::Guide)) if (g->raw.value("kind").toString() != "disc") {
      QJsonArray hits = g->raw.value("hits").toArray(); for (int k = 0; k < hits.size(); k++) { QJsonObject h = hits[k].toObject(); if (h.value("c").toInt() == bid) { h.insert("c", aid); hits[k] = h; } } g->raw.insert("hits", hits); }
    for (Item* t : byOrder(sc, ItemType::Tube)) if (t->raw.value("src").toInt(-1) == bid) t->raw.insert("src", aid);
    mergedInto[bid] = aid;                                            // (cutCurves finds the piece that stays)
    net.deleteItem(bid);
  }
  std::set<QString> seen;
  for (Item* s : byOrder(sc, ItemType::Surface)) { QString k = pairKey(s->a, s->b); if (s->a == s->b || seen.count(k)) net.deleteItem(s->id); else seen.insert(k); }
  std::vector<std::array<int, 2>> out; for (auto& [k, v] : pieces) out.push_back(v);
  return out;
}

QString cutCurves(Scene& sc, const CurveCache& cc, Selection& sel, const std::vector<Hit>& hits, const QString& mode) {
  int n = 0; std::set<int> cutIds; std::vector<std::pair<int, std::vector<std::array<int, 2>>>> done;
  std::set<int> before; for (Item* c : byOrder(sc, ItemType::Curve)) before.insert(c->id);
  std::map<int, int> mergedInto;
  auto live = [&](int id) { for (int k = 0; k < 50 && !sc.item(id) && mergedInto.count(id); k++) id = mergedInto[id]; return id; };
  struct W { int id, a, b, parent; QJsonObject p; bool visible; };
  std::vector<W> walls; for (Item* s : byOrder(sc, ItemType::Surface)) walls.push_back({s->id, s->a, s->b, s->parent, s->raw.value("p").toObject(), s->visible});
  std::map<int, std::array<int, 2>> pieceOf;                            // original curve id → [root-side piece, tip-side piece]
  Network net(sc, cc, sel.items, sel.anchors);
  for (const Hit& h : hits) {
    Item* c = sc.item(h.c); if (!c || cutIds.count(h.c)) continue;     // (welded along the cut — split with the first one)
    const auto& A = c->anchors;
    int J = h.u < 0.03 ? A[size_t(h.seg)] : h.u > 0.97 ? A[size_t(h.seg + 1)] : -1;
    if (J >= 0 && (J == A.front() || J == A.back())) continue;          // (a curve's end — nothing to cut)
    if (J < 0) J = net.insertKeepShape(h.c, h.seg, h.u);
    sc.reindex();
    auto pairs = splitAtAnchor(sc, cc, sel, J, mergedInto); if (pairs.empty()) continue;
    n++; for (auto& pr : pairs) { cutIds.insert(pr[0]); cutIds.insert(pr[1]); }
    for (auto& pr : pairs) for (int id : pr) if (before.count(id)) { pieceOf[id] = pr; break; }
    done.push_back({J, pairs});
  }
  if (!n) return "!" + QObject::tr("Nothing was cut (the cut must cross a curve away from its ends)");
  std::set<QString> have; for (Item* s : byOrder(sc, ItemType::Surface)) have.insert(pairKey(s->a, s->b));
  for (const W& w : walls) {
    Item* s = sc.item(w.id); bool gone = !s || s->type != ItemType::Surface;
    bool merged = !gone && ((s->a == live(w.a) && s->b == live(w.b)) || (s->a == live(w.b) && s->b == live(w.a)));
    if (!gone && (s->a != w.a || s->b != w.b) && !merged) continue;
    auto fa = pieceOf.find(w.a), fb = pieceOf.find(w.b);
    if (fa == pieceOf.end() && fb == pieceOf.end()) continue;
    std::array<int, 2> pa{-1, -1}, pb{-1, -1};
    if (fa != pieceOf.end()) pa = {live(fa->second[0]), live(fa->second[1])};
    if (fb != pieceOf.end()) pb = {live(fb->second[0]), live(fb->second[1])};
    bool ok = fa != pieceOf.end() && fb != pieceOf.end(); for (int id : {pa[0], pa[1], pb[0], pb[1]}) if (!sc.item(id)) ok = false;
    if (!ok) { if (!gone) net.deleteItem(w.id); continue; }
    // the pieces on the same side of the cut go together (by their free ends — the curves may run opposite ways)
    auto freeEnds = [&](const std::array<int, 2>& pr) {
      const auto &x = sc.item(pr[0])->anchors, &y = sc.item(pr[1])->anchors; int cutA = -1; for (int v : x) if (idxOf(y, v) >= 0) { cutA = v; break; }
      std::array<Vec3, 2> o; for (int i = 0; i < 2; i++) { const auto& B = i ? y : x; o[i] = sc.anchor(B.front() == cutA ? B.back() : B.front())->pos; } return o;
    };
    auto Fa = freeEnds(pa), Fb = freeEnds(pb);
    bool swap = (Fa[0] - Fb[0]).length() + (Fa[1] - Fb[1]).length() > (Fa[0] - Fb[1]).length() + (Fa[1] - Fb[0]).length();
    int b0 = swap ? pb[1] : pb[0], b1 = swap ? pb[0] : pb[1];
    QString k0 = pairKey(pa[0], b0);
    if (!gone) {
      if (pa[0] == b0 || (k0 != pairKey(s->a, s->b) && have.count(k0))) net.deleteItem(w.id);   // (both sides one curve now / another such wall)
      else { s->a = pa[0]; s->b = b0; have.insert(k0); }
    }
    if (pa[1] != b1 && !have.count(pairKey(pa[1], b1))) { addWall(sc, w.parent, pa[1], b1, w.p, w.visible); have.insert(pairKey(pa[1], b1)); }
  }
  // (a wall moved onto a merged trunk piece, its other curve not cut: one side cut — deleted too)
  for (Item* s : byOrder(sc, ItemType::Surface)) if (cutIds.count(s->a) != cutIds.count(s->b)) net.deleteItem(s->id);
  if (mode != "split") for (auto& [J, pairs] : done) for (auto& pr : pairs) {
    const Item *p0 = sc.item(pr[0]), *p1 = sc.item(pr[1]); if (!p0 || !p1) continue;
    auto far = [&](const Item* x) { const auto& B = x->anchors; return sc.anchor(B.front() == J ? B.back() : B.front())->pos.y; };
    const Item* up = far(p0) >= far(p1) ? p0 : p1; const Item* down = up == p0 ? p1 : p0;
    net.deleteItem((mode == "up" ? up : down)->id);                     // (its walls go with it)
  }
  sc.reindex();
  std::vector<int> keep; for (int id : sel.items) if (sc.item(id)) keep.push_back(id); sel.items = keep; sel.anchors.clear();
  return mode == "split" ? QObject::tr("Cut: %1 split in two").arg(n == 1 ? QObject::tr("the curve is") : QObject::tr("%1 curves are").arg(n))
                         : QObject::tr("Cut: the %1 part%2 deleted").arg(mode == "up" ? QObject::tr("upper") : QObject::tr("lower")).arg(n > 1 ? "s" : "");
}

// ---- X junctions ----
namespace {
struct XS { std::vector<std::pair<int, int>> thru; Vec3 n; };   // (the curves through J: id, J's index)
std::optional<XS> xSides(const Scene& sc, int J, const std::vector<int>& cs) {
  XS x; for (int id : cs) { const Item* d = sc.item(id); if (d->ring >= 0 || d->hasRib) continue; int i = idxOf(d->anchors, J); if (i > 0 && i < int(d->anchors.size()) - 1) x.thru.push_back({id, i}); }
  if (x.thru.size() < 2) return std::nullopt;
  const auto& A = sc.item(x.thru[0].first)->anchors; int i0 = x.thru[0].second;
  Vec3 ref = sc.anchor(A[size_t(i0 + 1)])->pos - sc.anchor(A[size_t(i0 - 1)])->pos;
  std::set<int> before, after; Vec3 dir;
  for (auto [id, i] : x.thru) {                                         // each curve's neighbours, oriented the same way
    const auto& D = sc.item(id)->anchors; int n1 = D[size_t(i - 1)], n2 = D[size_t(i + 1)];
    if ((sc.anchor(n2)->pos - sc.anchor(n1)->pos).dot(ref) < 0) std::swap(n1, n2);
    before.insert(n1); after.insert(n2); dir = dir + (sc.anchor(n2)->pos - sc.anchor(n1)->pos).normalized();
  }
  if (before.size() < 2 || after.size() < 2) return std::nullopt;
  x.n = dir.lengthSq() > 1e-12 ? dir.normalized() : ref.normalized(); return x;
}
std::vector<int> curvesWithA(const Scene& sc, int a) { std::vector<int> o; for (const Item* c : sc.curves()) if (idxOf(c->anchors, a) >= 0) o.push_back(c->id); return o; }
// the curves of the lock (joined by walls) around anchor J
std::vector<int> lockAround(Scene& sc, int J) {
  std::set<int> ids; for (int c : curvesWithA(sc, J)) if (sc.item(c)->ring < 0) ids.insert(c);
  bool grew = true; auto walls = byOrder(sc, ItemType::Surface);
  while (grew) { grew = false; for (Item* s : walls) if (ids.count(s->a) != ids.count(s->b)) { ids.insert(s->a); ids.insert(s->b); grew = true; } }
  std::vector<int> out; for (int id : ids) if (const Item* c = sc.item(id); c && c->type == ItemType::Curve && c->ring < 0) out.push_back(id);
  return out;
}
// the stretch i0…i1 of curve c in terms of real curves (an edge curve's stretch → the stretches of the curves its path runs along)
std::vector<Network::Stretch> flatStretch(const Scene& sc, int cid, int i0, int i1) {
  const Item* c = sc.item(cid); if (!c->hasPath()) return {{cid, i0, i1}};
  std::vector<Network::Stretch> out; int pos = 0;
  for (auto& p : c->path) {
    const Item* s = sc.item(p[0]); if (!s) return {{cid, i0, i1}};
    int j0 = idxOf(s->anchors, p[1]), j1 = idxOf(s->anchors, p[2]); if (j0 < 0 || j1 < 0) return {{cid, i0, i1}};
    int len = std::abs(j1 - j0), dir = j1 >= j0 ? 1 : -1, lo = std::max(i0, pos), hi = std::min(i1, pos + len);
    if (hi > lo) out.push_back({s->id, j0 + dir * (lo - pos), j0 + dir * (hi - pos)});
    pos += len;
  }
  if (out.empty()) return {{cid, i0, i1}}; return out;
}
}  // namespace

std::vector<int> xAnchors(const Scene& sc, const std::vector<int>& cs) {
  std::set<int> set(cs.begin(), cs.end()), seen; std::vector<int> out;
  for (int id : cs) { const auto& A = sc.item(id)->anchors;
    for (size_t k = 1; k + 1 < A.size(); k++) { int J = A[k]; if (!seen.insert(J).second) continue;
      std::vector<int> w; for (int c : curvesWithA(sc, J)) if (set.count(c)) w.push_back(c);
      if (xSides(sc, J, w)) out.push_back(J); } }
  return out;
}

bool isX(const Scene& sc, int J) { return xSides(sc, J, curvesWithA(sc, J)).has_value(); }

std::vector<int> wallsAcrossX(const Scene& sc, int J) {
  std::vector<int> out;
  for (auto& [id, s] : sc.items) if (s.type == ItemType::Surface) for (int cid : {s.a, s.b}) { const Item* c = sc.item(cid); if (!c) continue; int i = idxOf(c->anchors, J); if (i > 0 && i < int(c->anchors.size()) - 1) { out.push_back(id); break; } }
  return out;
}

bool separateAtX(Scene& sc, Selection& sel, int J, int* madeOut, int* keptOut) {
  auto cs = lockAround(sc, J); std::vector<int> through; for (int c : curvesWithA(sc, J)) if (idxOf(cs, c) >= 0) through.push_back(c);
  auto xs = xSides(sc, J, through); if (!xs) return false;
  CurveCache none; Network net(sc, none, sel.items, sel.anchors);
  std::vector<int> walls; for (Item* s : byOrder(sc, ItemType::Surface)) for (int c : cs) if (c == s->a || c == s->b) { walls.push_back(s->id); break; }
  std::vector<int> wc; for (int w : walls) for (int id : {sc.item(w)->a, sc.item(w)->b}) if (idxOf(wc, id) < 0 && sc.item(id) && sc.item(id)->type == ItemType::Curve) wc.push_back(id);
  Vec3 Jp = sc.anchor(J)->pos, n = xs->n; auto side = [&](const Vec3& p) { return (p - Jp).dot(n); };
  // where each wall curve is cut: through J — J; else the plane's crossing nearest to J (not too far)
  std::map<int, int> cut; struct Near { int c, seg; double u, dist; }; std::vector<Near> near;
  for (int cid : wc) {
    const Item* c = sc.item(cid); int i = idxOf(c->anchors, J);
    if (i > 0 && i < int(c->anchors.size()) - 1) { cut[cid] = J; continue; }
    if (i >= 0) continue;                                                // (ends at J — it is one side already)
    CurveSampling sm = sampleCurveFull(sc, *c); std::optional<Near> best; double bd = 1e300;
    for (size_t k = 0; k + 1 < sm.p.size(); k++) {
      double d0 = side(sm.p[k]), d1 = side(sm.p[k + 1]); if (d0 == d1 || d0 * d1 > 0) continue;
      double t = d0 / (d0 - d1); Vec3 p = sm.p[k].lerp(sm.p[k + 1], t); double dist = (p - Jp).length();
      if (dist < bd) { bd = dist; int seg = sm.seg[k]; best = Near{cid, seg, std::clamp((double(k) - seg * 24 + t) / 24, 0.0, 1.0), dist}; }
    }
    if (best) near.push_back(*best);
  }
  std::vector<double> ds; for (auto& h : near) ds.push_back(h.dist); std::sort(ds.begin(), ds.end());
  double med = ds.empty() ? 0 : ds[ds.size() >> 1], lim = std::max(3 * med, 4 * projectStep(sc));
  for (auto& h : near) {
    if (h.dist > lim || cut.count(h.c)) continue;
    const auto& A = sc.item(h.c)->anchors; int a = h.u < 0.03 ? A[size_t(h.seg)] : h.u > 0.97 ? A[size_t(h.seg + 1)] : -1;
    if (a >= 0 && (a == A.front() || a == A.back())) continue;
    if (a < 0) {
      a = net.insertKeepShape(h.c, h.seg, h.u); sc.reindex();          // (it goes into every curve along that segment)
      for (auto& o : near) if (&o != &h && !cut.count(o.c) && idxOf(sc.item(o.c)->anchors, a) >= 0) cut[o.c] = a;
    }
    cut[h.c] = a;
  }
  int made = 0, kept = 0;
  // one edge curve per curve part: walls sharing a curve share its part on each side (they stay one lock)
  std::map<QString, int> edges;
  auto edge = [&](int c, int i0, int i1, int parent) {
    auto st = flatStretch(sc, c, i0, i1); QString key;
    for (auto& q : st) { const Item* x = sc.item(q.c); key += QString("%1:%2-%3|").arg(q.c).arg(x->anchors[size_t(q.i0)]).arg(x->anchors[size_t(q.i1)]); }
    auto f = edges.find(key); if (f != edges.end()) return f->second;
    int id = net.pathCurve(st, parent); edges[key] = id; return id;
  };
  for (int wid : walls) {
    Item* s = sc.item(wid); if (!s) continue; const Item *A = sc.item(s->a), *B = sc.item(s->b); if (!A || !B) continue;
    if (!cut.count(A->id) || !cut.count(B->id)) { kept++; continue; }
    int ia = idxOf(A->anchors, cut[A->id]), ib = idxOf(B->anchors, cut[B->id]); if (ia <= 0 || ib <= 0) { kept++; continue; }
    // the parts on the same side of the X go together (the curves may run opposite ways)
    std::array<std::array<int, 2>, 2> pa{{{0, ia}, {ia, int(A->anchors.size()) - 1}}}, pb{{{0, ib}, {ib, int(B->anchors.size()) - 1}}};
    if ((side(sc.anchor(A->anchors[0])->pos) > 0) != (side(sc.anchor(B->anchors[0])->pos) > 0) || (side(sc.anchor(A->anchors[0])->pos) == 0) != (side(sc.anchor(B->anchors[0])->pos) == 0)) std::swap(pb[0], pb[1]);
    int Aid = A->id, Bid = B->id, par = s->parent; QJsonObject p = s->raw.value("p").toObject(); bool vis = s->visible;
    for (int k = 0; k < 2; k++) {
      auto [a0, a1] = pa[size_t(k)]; auto [b0, b1] = pb[size_t(k)]; if (a1 <= a0 || b1 <= b0) continue;
      int x = edge(Aid, a0, a1, par), y = edge(Bid, b0, b1, par);
      if (x != y) { int w = edit::makeSurface(sc, x, y); if (w >= 0) { Item* wi = sc.item(w); QJsonObject wp = wi->raw; wp.insert("p", p); wi->raw = wp; wi->visible = vis; made++; } }
    }
    net.deleteItem(wid);                                                // (its own edge curves go with it, unused now)
  }
  sc.reindex();
  if (madeOut) *madeOut = made; if (keptOut) *keptOut = kept;
  return made > 0;
}

QString separateGroupX(Scene& sc, Selection& sel, int gid) {
  std::set<int> tried; int k = 0;
  for (int guard = 0; guard < 30; guard++) {
    std::vector<int> cs; for (Item* c : byOrder(sc, ItemType::Curve)) if (c->ring < 0 && sc.isDescendant(c->id, gid)) cs.push_back(c->id);
    int J = -1; for (int a : xAnchors(sc, cs)) if (!tried.count(a) && !wallsAcrossX(sc, a).empty()) { J = a; break; }
    if (J < 0) break;
    tried.insert(J); if (separateAtX(sc, sel, J)) k++;
  }
  sel.anchors.clear();
  if (!k) return "!" + QObject::tr("No X junctions with walls across them in this group");
  return QObject::tr("%1 X junction%2: the walls above and below are separate locks now — the curves stay whole").arg(k).arg(k > 1 ? "s" : "");
}

}  // namespace hm::cut
