#include "scene/Edit.h"
#include "scene/CurveMath.h"
#include "scene/Cocoons.h"
#include "scene/HairState.h"
#include "scene/Network.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <functional>
#include <map>
#include <algorithm>
#include <set>
#include <unordered_set>

namespace hm::edit {

std::vector<int> curvesWith(const Scene& sc, int a) {
  std::vector<int> out;
  for (const Item* c : sc.curves()) if (std::find(c->anchors.begin(), c->anchors.end(), a) != c->anchors.end()) out.push_back(c->id);
  return out;
}

bool isRingAnchor(const Scene& sc, int a) {
  for (const Item* c : sc.curves()) if (c->ring >= 0 && std::find(c->anchors.begin(), c->anchors.end(), a) != c->anchors.end()) return true;
  return false;
}

std::pair<double, double> fitHandles(const std::vector<Vec3>& pts, const Vec3& P0, const Vec3& P3, const Vec3& d1, const Vec3& d2) {
  std::vector<double> L{0}; for (size_t k = 1; k < pts.size(); k++) L.push_back(L.back() + pts[k].distanceTo(pts[k - 1]));
  double tot = L.back() ? L.back() : 1;
  double c11 = 0, c12 = 0, c22 = 0, x1 = 0, x2 = 0;
  for (size_t k = 0; k < pts.size(); k++) {
    double t = L[k] / tot, u = 1 - t, b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
    Vec3 A1 = d1 * b1, A2 = d2 * b2, r = pts[k] - P0 * (b0 + b1) - P3 * (b2 + b3);
    c11 += A1.dot(A1); c12 += A1.dot(A2); c22 += A2.dot(A2); x1 += r.dot(A1); x2 += r.dot(A2);
  }
  double det = c11 * c22 - c12 * c12, chord = P0.distanceTo(P3) / 3, a = chord, b = chord;
  if (std::abs(det) > 1e-14) { a = (x1 * c22 - x2 * c12) / det; b = (c11 * x2 - c12 * x1) / det; }
  if (!(a > 1e-6)) a = chord; if (!(b > 1e-6)) b = chord;
  return {a, b};
}

void removeAnchorFromCurve(Scene& sc, Item& c, int a, bool keep) {
  auto& A = c.anchors; int i = int(std::find(A.begin(), A.end(), a) - A.begin()); if (i >= int(A.size())) return;
  int n = int(A.size());
  if (keep && n > 2) {
    auto segs = curveSegs(sc, c); std::vector<Handles> Hs; for (int k = 0; k < n; k++) Hs.push_back(curveHandles(sc, c, k));
    auto setO = [&](int an, bool in, const Vec3& v) { auto& o = c.ovr[an]; if (in) o.in = v; else o.out = v; };
    auto freeze = [&](int k, bool in) { const Anchor* an = sc.anchor(A[k]); double sm = std::max(an->smooth, 0.05); setO(A[k], in, ((in ? Hs[k].hin : Hs[k].hout) - an->pos) / sm); };
    if (i > 0 && i < n - 1) {
      if (i - 1 > 0) freeze(i - 1, true);
      if (i + 1 < n - 1) freeze(i + 1, false);
      std::vector<Vec3> pts; for (const BezSeg& s : {segs[size_t(i - 1)], segs[size_t(i)]}) for (int k = 0; k <= 24; k++) pts.push_back(bez(s, k / 24.0));
      const Anchor *A0 = sc.anchor(A[i - 1]), *A3 = sc.anchor(A[i + 1]);
      Vec3 d1 = segs[size_t(i - 1)][1] - A0->pos; if (d1.lengthSq() < 1e-14) d1 = pts[1] - A0->pos; d1 = d1.normalized();
      Vec3 d2 = segs[size_t(i)][2] - A3->pos; if (d2.lengthSq() < 1e-14) d2 = pts[pts.size() - 2] - A3->pos; d2 = d2.normalized();
      auto [la, lb] = fitHandles(pts, A0->pos, A3->pos, d1, d2);
      setO(A[i - 1], false, d1 * (la / std::max(A0->smooth, 0.05)));
      setO(A[i + 1], true, d2 * (lb / std::max(A3->smooth, 0.05)));
    } else if (i == 0) freeze(1, false);
    else freeze(n - 2, true);
  }
  A.erase(A.begin() + i); c.ovr.erase(a);
}

QString deleteAnchors(Scene& sc, const CurveCache& samples, Selection& sel, std::vector<int> list, bool keep, int drawing) {
  list.erase(std::remove_if(list.begin(), list.end(), [&](int a) { return isRingAnchor(sc, a); }), list.end());   // (a disc keeps its 6 + 6 points)
  if (list.empty()) return QObject::tr("Disc points can't be deleted — use Reset deformations");
  std::set<int> touched; std::unordered_set<int> gone(list.begin(), list.end());
  std::vector<int> sitems, sanchors;
  Network net(sc, samples, sitems, sanchors);
  // (service curves along other curves — c.path — aren't edited themselves: laid anew along their curves below)
  struct EC { int c; std::vector<int> L; }; std::vector<EC> ecs;
  for (const Item* c : sc.curves()) if (c->hasPath()) { EC e{c->id, {}}; for (int a : c->anchors) if (!gone.count(a)) e.L.push_back(a); ecs.push_back(e); }
  for (int a : list) for (int cid : curvesWith(sc, a)) { Item* c = sc.item(cid); if (c->hasPath()) continue; removeAnchorFromCurve(sc, *c, a, keep); touched.insert(cid); }
  { std::vector<int> kill; for (const Item* c : sc.curves()) if (!c->hasPath() && c->anchors.size() < 2 && c->id != drawing) kill.push_back(c->id); for (int id : kill) net.deleteItem(id); }
  // a service curve: along its curves again; its route broken — it goes
  for (auto& [cid, L] : ecs) {
    Item* c = sc.item(cid); if (!c) continue;
    auto st = L.size() >= 2 ? net.routeStretches(L) : std::nullopt;
    if (st) {
      c->path.clear(); for (auto& s : *st) { const Item* q = sc.item(s.c); c->path.push_back({s.c, q->anchors[size_t(s.i0)], q->anchors[size_t(s.i1)]}); }
      auto pa = net.pathAnchors(c->path); c->anchors = pa ? *pa : L;
    } else net.deleteItem(cid);
  }
  // a Y branch whose own anchors are all gone runs only along its host's trunk — no curve of its own any more
  auto segOn = [&](int x, int y, int self) {
    for (const Item* h : sc.curves()) {
      if (h->id == self || h->ring >= 0 || h->junc || h->hasPath()) continue;
      for (size_t k = 0; k + 1 < h->anchors.size(); k++) if ((h->anchors[k] == x && h->anchors[k + 1] == y) || (h->anchors[k] == y && h->anchors[k + 1] == x)) return true;
    }
    return false;
  };
  for (int id : touched) {
    const Item* c = sc.item(id); if (!c || c->ring >= 0 || c->anchors.size() < 2) continue;
    bool all = true; for (size_t k = 1; k < c->anchors.size(); k++) if (!segOn(c->anchors[k - 1], c->anchors[k], id)) { all = false; break; }
    if (all) net.deleteItem(id);
  }
  net.pruneAnchors();
  // the edited curves stay selected (so they don't vanish with Auto guides)
  sel.anchors.clear(); sel.items.clear(); for (int id : touched) if (sc.item(id)) sel.items.push_back(id);
  sc.reindex();
  return {};
}

void recalcHandles(Scene& sc, const std::vector<int>& anchors) {
  std::unordered_set<int> ids(anchors.begin(), anchors.end());
  for (auto& [id, c] : sc.items) if (c.type == ItemType::Curve) for (int a : anchors) c.ovr.erase(a);
  for (int a : anchors) if (Anchor* an = sc.anchor(a)) an->smooth = 1;
}

std::vector<int> anchorsOf(const Scene& sc, const std::vector<int>& curves) {
  std::vector<int> out; std::unordered_set<int> seen;
  for (int id : curves) if (const Item* c = sc.item(id)) for (int a : c->anchors) if (seen.insert(a).second) out.push_back(a);
  return out;
}

int mergeCoincident(Scene& sc, const CurveCache& samples, Selection& sel, const std::vector<int>& curves, double tol) {
  std::vector<int> list = anchorsOf(sc, curves);
  auto sharesCurve = [&](int a, int b) { for (const Item* c : sc.curves()) if (std::find(c->anchors.begin(), c->anchors.end(), a) != c->anchors.end() && std::find(c->anchors.begin(), c->anchors.end(), b) != c->anchors.end()) return true; return false; };
  std::vector<int> si, sa; Network net(sc, samples, si, sa);
  int n = 0;
  for (size_t i = 0; i < list.size(); i++) for (size_t j = i + 1; j < list.size(); j++) {
    int a = list[i], b = list[j]; if (a < 0 || b < 0) continue;
    const Anchor *A = sc.anchor(a), *B = sc.anchor(b); if (!A || !B || A->pos.distanceTo(B->pos) > tol || sharesCurve(a, b)) continue;
    net.weldInto(b, a); list[j] = -1; n++;
  }
  if (n) { sel.anchors.clear(); sc.reindex(); }
  return n;
}

std::vector<int> unweldAnchor(Scene& sc, int a, const std::vector<int>& selItems, std::vector<int>* made) {
  auto users = curvesWith(sc, a); if (users.size() < 2 || isRingAnchor(sc, a)) return {};
  std::vector<int> selU; for (int c : users) if (std::find(selItems.begin(), selItems.end(), c) != selItems.end()) selU.push_back(c);
  if (selU.empty()) return {};
  std::vector<int> keep = selU.size() == users.size() ? std::vector<int>(selU.begin() + 1, selU.end()) : selU;   // (all users selected → the first keeps the old one)
  const Anchor A0 = *sc.anchor(a);
  for (int cid : keep) {
    Anchor& n = sc.addAnchor(A0.pos); n.smooth = A0.smooth; if (made) made->push_back(n.id);
    Item* c = sc.item(cid); for (int& x : c->anchors) if (x == a) x = n.id;
    auto o = c->ovr.find(a); if (o != c->ovr.end()) { HandleOverride h = o->second; c->ovr.erase(a); c->ovr[n.id] = h; }
  }
  sc.reindex();
  return keep;
}

void deleteItems(Scene& sc, const CurveCache& samples, Selection& sel) {
  std::vector<int> ids = sel.items;
  Network net(sc, samples, sel.items, sel.anchors);
  for (int id : ids) if (sc.item(id)) net.deleteItem(id);
  sel.items.clear();
  net.dropOrphanServiceCurves();                             // (Hole curves left by walls deleted before)
  net.pruneAnchors();
  sc.reindex();
}

// ======================= walls =======================
namespace {
int nextNum(Scene& sc, ItemType t, const QString& base, const QString& key) {
  QRegularExpression re("^" + QRegularExpression::escape(base) + " (\\d+)$"); int mx = 0;
  for (auto& [id, it] : sc.items) if (it.type == t) { auto m = re.match(it.name); if (m.hasMatch()) mx = std::max(mx, m.captured(1).toInt()); }
  QJsonObject cnt = sc.fileRaw.value("counters").toObject(); cnt.insert(key, mx + 1); sc.fileRaw.insert("counters", cnt);
  return mx + 1;
}
const std::vector<Vec3>* smp(const CurveCache& cc, int id) { auto it = cc.find(id); return it == cc.end() || it->second.p.size() < 2 ? nullptr : &it->second.p; }
// n points evenly along a polyline (resample / ccResample)
std::vector<Vec3> resampleN(const std::vector<Vec3>& Q, int n) {
  std::vector<double> L{0}; for (size_t i = 1; i < Q.size(); i++) L.push_back(L.back() + Q[i].distanceTo(Q[i - 1]));
  double T = L.back() ? L.back() : 1; std::vector<Vec3> out; size_t j = 0;
  for (int k = 0; k < n; k++) {
    double s = T * k / (n - 1); while (j + 2 < Q.size() && L[j + 1] < s) j++;
    double g = L[j + 1] - L[j]; out.push_back(Q[j].lerp(Q[j + 1], g > 0 ? clamp((s - L[j]) / g, 0, 1) : 0));
  }
  return out;
}
// the longest run of 2+ consecutive anchors common to both curves (same direction) → its length (0: none)
int sharedRun(const Item& c1, const Item& c2) {
  int best = 0;
  for (size_t i = 0; i < c1.anchors.size(); i++) {
    auto it = std::find(c2.anchors.begin(), c2.anchors.end(), c1.anchors[i]); if (it == c2.anchors.end()) continue;
    size_t j = size_t(it - c2.anchors.begin()); int n = 1;
    while (i + n < c1.anchors.size() && j + n < c2.anchors.size() && c2.anchors[j + n] == c1.anchors[i + n]) n++;
    if (n >= 2) best = std::max(best, n);
  }
  return best;
}
std::vector<int> trunkMates(const Scene& sc, int c) {
  std::vector<int> out{c}; std::vector<const Item*> cs; for (const Item* h : sc.curves()) if (!h->junc && h->ring < 0 && h->conn.isEmpty()) cs.push_back(h);
  for (size_t i = 0; i < out.size(); i++) for (const Item* h : cs) if (std::find(out.begin(), out.end(), h->id) == out.end() && sharedRun(*sc.item(out[i]), *h)) out.push_back(h->id);
  return out;
}
double curveGap(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
  auto pa = resampleN(a, 40), pb = resampleN(b, 40);
  auto one = [](const std::vector<Vec3>& P, const std::vector<Vec3>& Q) { double s = 0; for (auto& p : P) { double m = 1e300; for (auto& q : Q) m = std::min(m, (q - p).lengthSq()); s += std::sqrt(m); } return s / P.size(); };
  return (one(pa, pb) + one(pb, pa)) / 2;
}
// two trunks selected: the wall between the branches facing each other — the closest pair
std::vector<int> resolveTrunks(const Scene& sc, const CurveCache& cc, const std::vector<int>& list) {
  if (list.size() < 2) return list;
  struct G { std::vector<int> mates, sel; }; std::vector<G> groups;
  for (int c : list) { G* g = nullptr; for (auto& x : groups) if (std::find(x.mates.begin(), x.mates.end(), c) != x.mates.end()) g = &x; if (g) g->sel.push_back(c); else groups.push_back({trunkMates(sc, c), {c}}); }
  if (groups.size() != 2 || groups[0].mates.size() < 2 || groups[1].mates.size() < 2) return list;
  double bd = 1e300; int ba = -1, bb = -1;
  for (int a : groups[0].mates) for (int b : groups[1].mates) {
    const auto *pa = smp(cc, a), *pb = smp(cc, b); if (!pa || !pb) continue;
    double d = curveGap(*pa, *pb); if (d < bd) { bd = d; ba = a; bb = b; }
  }
  return ba >= 0 ? std::vector<int>{ba, bb} : list;
}
}  // namespace

bool segNetCurve(const Item& c) { return !(c.junc || c.hasPath() || c.ring >= 0 || !c.conn.isEmpty() || c.closed || c.hasRib); }
bool isNewLogic(const Scene& sc, const Item& it) { return groupLogic(sc, it) == 2; }

bool hasSurface(const Scene& sc, int c1, int c2) {
  for (auto& [id, s] : sc.items) if (s.type == ItemType::Surface && ((s.a == c1 && s.b == c2) || (s.a == c2 && s.b == c1))) return true;
  return false;
}

int makeSurface(Scene& sc, int c1, int c2) {
  if (hasSurface(sc, c1, c2)) return -1;
  if (!sc.item(c1) || !sc.item(c2)) return -1;   // (a curve gone — undone: no wall to nothing)
  int n = nextNum(sc, ItemType::Surface, "Surface", "surface");
  Item& s = sc.addItem(ItemType::Surface, sc.item(c1)->parent, QString("Surface %1").arg(n));
  s.a = c1; s.b = c2; s.raw.insert("p", QJsonObject{{"segs", 1}, {"bulge", 0.15}, {"flip", false}});
  return s.id;
}

bool twoTrunks(const Scene& sc, const CurveCache& cc, const std::vector<int>& list) { return resolveTrunks(sc, cc, list) != list; }

std::vector<int> createSurfaces(Scene& sc, const CurveCache& cc, std::vector<int> list, bool closed, bool exact, QString* msg) {
  auto res = exact ? list : resolveTrunks(sc, cc, list);
  if (res != list) {
    if (hasSurface(sc, res[0], res[1]) && list.size() == 2) { if (msg) *msg = QObject::tr("The facing branches already have a wall — made it between the selected curves"); }
    else { list = res; closed = false; if (msg) *msg = QObject::tr("Wall between the facing branches: %1 ↔ %2").arg(sc.item(list[0])->name, sc.item(list[1])->name); }
  }
  std::vector<int> made;
  for (size_t i = 0; i + 1 < list.size(); i++) { int s = makeSurface(sc, list[i], list[i + 1]); if (s >= 0) made.push_back(s); }
  if (closed && list.size() >= 3) { int s = makeSurface(sc, list.back(), list.front()); if (s >= 0) made.push_back(s); }
  if (made.empty() && msg) *msg = QObject::tr("Surfaces between these curves already exist");
  return made;
}

std::vector<Path> pathsFor(Scene& sc, const CurveCache& cc, const std::vector<int>& sel, bool all) {
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  std::vector<Path> out; std::set<std::vector<int>> seen;
  for (auto it = sel.rbegin(); it != sel.rend(); ++it) for (auto& p : net.curvePaths({*it}, all)) {
    std::vector<int> k; Path q; for (auto& s : p) { k.push_back(s.c); q.push_back({s.c, s.from}); }
    if (seen.insert(k).second) out.push_back(q);
  }
  return out;
}

std::vector<Vec3> pathPts(const Scene& sc, const CurveCache& cc, const Path& p) {
  std::vector<Vec3> out;
  for (auto& s : p) { const auto* P = smp(cc, s.c); if (!P) continue; const Item* c = sc.item(s.c);
    if (c->anchors.front() == s.from) out.insert(out.end(), P->begin(), P->end()); else out.insert(out.end(), P->rbegin(), P->rend()); }
  return out;
}

static double pathLen(const CurveCache& cc, const Path& p) { double s = 0; for (auto& x : p) { auto it = cc.find(x.c); if (it != cc.end() && !it->second.arc.empty()) s += it->second.arc.back(); } return s; }

std::optional<std::vector<Path>> pathCover(Scene& sc, const CurveCache& cc, const std::vector<int>& scIds, bool loose) {
  std::vector<int> sel; for (int id : scIds) { const Item* c = sc.item(id); if (c && segNetCurve(*c) && c->anchors.size() >= 2) sel.push_back(id); }
  if (sel.size() < 2 || sel.size() != scIds.size()) return std::nullopt;
  std::set<int> set(sel.begin(), sel.end()), left(sel.begin(), sel.end());
  std::vector<Path> P; for (auto& p : pathsFor(sc, cc, sel)) { bool ok = true; for (auto& s : p) if (!set.count(s.c)) ok = false; if (ok) P.push_back(p); }
  std::vector<Path> out;
  while (!left.empty()) {
    const Path* best = nullptr; int bn = 0;
    for (auto& p : P) { int n = 0; for (auto& s : p) if (left.count(s.c)) n++; if (n > bn || (n == bn && best && pathLen(cc, p) > pathLen(cc, *best))) { best = &p; bn = n; } }
    Path one;
    if (!best && loose) { int c = *left.begin(); for (int id : sel) if (left.count(id)) { c = id; break; } one = {{c, sc.item(c)->anchors.front()}}; best = &one; }
    if (!best) return std::nullopt;
    out.push_back(*best); for (auto& s : *best) left.erase(s.c);
  }
  return out;
}

std::vector<int> wallsAlongPaths(Scene& sc, const CurveCache& cc, const std::vector<Path>& paths, QString* msg) {
  int n = int(paths.size()); std::vector<std::vector<Vec3>> R; for (auto& p : paths) R.push_back(resampleN(pathPts(sc, cc, p), 40));
  auto gap = [&](int i, int j) { double s = 0; for (size_t k = 0; k < R[size_t(i)].size(); k++) s += R[size_t(i)][k].distanceTo(R[size_t(j)][k]); return s; };
  int start = 0; double far = -1;
  for (int i = 0; i < n; i++) { double tot = 0; for (int j = 0; j < n; j++) if (j != i) tot += gap(i, j); if (tot > far) { far = tot; start = i; } }
  std::vector<int> order{start}; std::set<int> used{start};
  while (int(order.size()) < n) { int l = order.back(), b = -1; for (int j = 0; j < n; j++) if (!used.count(j) && (b < 0 || gap(l, j) < gap(l, b))) b = j; order.push_back(b); used.insert(b); }
  int parent = sc.item(paths[0][0].c)->parent;
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  std::vector<int> ecs;
  for (int i : order) {
    std::vector<Network::Stretch> st;
    for (auto& s : paths[size_t(i)]) { const Item* c = sc.item(s.c); int i0 = int(std::find(c->anchors.begin(), c->anchors.end(), s.from) - c->anchors.begin()); st.push_back({s.c, i0, i0 == 0 ? int(c->anchors.size()) - 1 : 0}); }
    ecs.push_back(net.pathCurve(st, parent));
  }
  std::vector<int> made;
  for (size_t i = 0; i + 1 < ecs.size(); i++) { if (ecs[i] == ecs[i + 1]) continue; int s = makeSurface(sc, ecs[i], ecs[i + 1]); if (s >= 0) made.push_back(s); }
  sc.reindex();
  if (msg) {
    QStringList names; for (int e : ecs) names << sc.item(e)->name;
    *msg = made.empty() ? QObject::tr("Walls between these paths already exist") : QObject::tr("%1 wall%2 along %3 paths: %4").arg(made.size()).arg(made.size() == 1 ? "" : "s").arg(n).arg(names.join(" ↔ "));
  }
  return made;
}

// the hole bounded by pieces of the selected curves: the shortest closed loop along their segments using every one,
// split at two corners into two paths of about equal length — two service curves (HC), a wall between them
QString fillHole(Scene& sc, const CurveCache& cc, const std::vector<int>& sel, int* wall) {
  std::vector<int> cs; for (int id : sel) { const Item* c = sc.item(id); if (c && c->type == ItemType::Curve && c->anchors.size() >= 2) cs.push_back(id); }
  if (cs.size() < 2) return QObject::tr("Select the curves around the hole (at least 2)");
  struct E { int to, c; }; std::map<int, std::vector<E>> adj;
  for (int id : cs) { const auto& A = sc.item(id)->anchors; for (size_t k = 0; k + 1 < A.size(); k++) { adj[A[k]].push_back({A[k + 1], id}); adj[A[k + 1]].push_back({A[k], id}); } }
  auto len = [&](int a, int b) { return sc.anchor(a)->pos.distanceTo(sc.anchor(b)->pos); };
  struct Best { std::vector<int> path, via; double L; }; std::optional<Best> best; int count = 0, steps = 0;
  std::vector<int> path, via;
  std::function<void(int, double)> dfs = [&](int start, double L) {
    if (count > 20000 || ++steps > 300000 || (best && L >= best->L)) return;
    int v = path.back();
    auto it = adj.find(v); if (it == adj.end()) return;
    for (auto& e : it->second) {
      if (path.size() > 1 && e.to == path[path.size() - 2] && e.c == via.back()) continue;
      double L2 = L + len(v, e.to);
      if (e.to == start && path.size() >= 3) {
        count++;
        std::set<int> cv(via.begin(), via.end()); cv.insert(e.c);
        bool all = true; for (int x : cs) if (!cv.count(x)) all = false;
        if (all && (!best || L2 < best->L)) { std::vector<int> v2 = via; v2.push_back(e.c); best = Best{path, v2, L2}; }
        continue;
      }
      if (std::find(path.begin(), path.end(), e.to) != path.end()) continue;
      path.push_back(e.to); via.push_back(e.c); dfs(start, L2); path.pop_back(); via.pop_back();
    }
  };
  std::vector<int> starts; std::set<int> seenS; for (int id : cs) for (int a : sc.item(id)->anchors) if (seenS.insert(a).second) starts.push_back(a);
  for (int s : starts) { path = {s}; via.clear(); dfs(s, 0); }
  if (!best) return QObject::tr("No closed loop through these curves — weld them at the corners of the hole first");
  return fillCycle(sc, cc, best->path, best->via, sc.item(cs[0])->parent, wall);
}

// the loop P (anchors round it; V — the curve of each step) split at two corners into two paths of about equal length:
// two junction curves (HC) along them and a wall between
QString fillCycle(Scene& sc, const CurveCache& cc, const std::vector<int>& P, const std::vector<int>& V, int parent, int* wall) {
  auto len = [&](int a, int b) { return sc.anchor(a)->pos.distanceTo(sc.anchor(b)->pos); };
  int n = int(P.size());
  std::vector<int> corners; for (int i = 0; i < n; i++) if (V[size_t(i)] != V[size_t((i + n - 1) % n)]) corners.push_back(i);
  if (corners.size() < 2) corners = {0, n / 2};
  std::vector<double> cum{0}; for (int i = 0; i < n; i++) cum.push_back(cum.back() + len(P[size_t(i)], P[size_t((i + 1) % n)]));
  int su = 0, sw = 0; double sd = 1e300;
  for (int u : corners) for (int w : corners) { if (w <= u) continue; double a = cum[size_t(w)] - cum[size_t(u)], b = cum.back() - a, d = std::abs(a - b); if (d < sd) { sd = d; su = u; sw = w; } }
  std::vector<int> X, Y; for (int i = su; i <= sw; i++) X.push_back(P[size_t(i)]);
  for (int i = su;; i = (i + n - 1) % n) { Y.push_back(P[size_t(i)]); if (i == sw) break; }
  std::set<int> tips, inWalls; for (auto& [id, s] : sc.items) if (s.type == ItemType::Surface) { inWalls.insert(s.a); inWalls.insert(s.b); }
  for (const Item* h : sc.curves()) if (inWalls.count(h->id) && !h->junc && h->ring < 0 && !h->anchors.empty()) tips.insert(h->anchors.back());
  if (tips.count(X.front()) && !tips.count(X.back())) { std::reverse(X.begin(), X.end()); std::reverse(Y.begin(), Y.end()); }
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  auto mk = [&](const std::vector<int>& list) { Item& c = net.makeCurve(parent); c.anchors = list; c.junc = true; c.name = net.serviceName("HC"); return c.id; };
  int cx = mk(X), cy = mk(Y);
  int sn = nextNum(sc, ItemType::Surface, "Surface", "surface");
  Item& s = sc.addItem(ItemType::Surface, parent, QString("Surface %1").arg(sn)); s.a = cx; s.b = cy;
  s.raw.insert("p", QJsonObject{{"segs", 1}, {"bulge", 0}, {"flip", false}});
  if (wall) *wall = s.id;
  sc.reindex();
  return QObject::tr("Hole filled: %1 corner%2, a wall between two curves along its edges (Bulge 0 — set it in the surface)").arg(corners.size()).arg(corners.size() == 1 ? "" : "s");
}

// a new curve halfway between two curves (as many anchors as the denser one); ends both share stay shared
int averageCurve(Scene& sc, const CurveCache& cc, int id1, int id2) {
  const auto *s1 = smp(cc, id1), *s2 = smp(cc, id2); if (!s1 || !s2) return -1;
  const Item *c1 = sc.item(id1), *c2 = sc.item(id2);
  int n = int(std::max(c1->anchors.size(), c2->anchors.size()));
  auto P1 = resampleN(*s1, n), P2 = resampleN(*s2, n); auto a2 = c2->anchors;
  if (P1[0].distanceTo(P2[0]) + P1[size_t(n - 1)].distanceTo(P2[size_t(n - 1)]) > P1[0].distanceTo(P2[size_t(n - 1)]) + P1[size_t(n - 1)].distanceTo(P2[0])) { std::reverse(P2.begin(), P2.end()); std::reverse(a2.begin(), a2.end()); }
  int parent = c1->parent; auto a1 = c1->anchors;
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  Item& c = net.makeCurve(parent); c.name = "Average curve";
  for (int i = 0; i < n; i++) c.anchors.push_back(sc.addAnchor((P1[size_t(i)] + P2[size_t(i)]) * 0.5).id);
  if (a1.front() == a2.front()) c.anchors.front() = a1.front();
  if (a1.back() == a2.back()) c.anchors.back() = a1.back();
  // (next to the first curve in its group)
  auto& ch = parent >= 0 ? sc.item(parent)->children : sc.roots;
  ch.erase(std::remove(ch.begin(), ch.end(), c.id), ch.end());
  auto at = std::find(ch.begin(), ch.end(), id1); ch.insert(at == ch.end() ? ch.end() : at + 1, c.id);
  net.pruneAnchors(); sc.reindex();
  return c.id;
}

// ======================= cocoons =======================
std::vector<int> lockCurves(const Scene& sc, const Selection& sel) {
  std::vector<int> out; std::set<int> seen;
  std::function<void(const Item*)> add = [&](const Item* c) {
    if (!c || c->type != ItemType::Curve || c->ring >= 0) return;
    if (c->hasPath()) { for (auto& p : c->path) add(sc.item(p[0])); return; }
    if (!c->junc && seen.insert(c->id).second) out.push_back(c->id);
  };
  for (int id : sel.items) {
    const Item* it = sc.item(id); if (!it) continue;
    if (it->type == ItemType::Curve) add(it);
    else if (it->type == ItemType::Surface) { add(sc.item(it->a)); add(sc.item(it->b)); }
    else if (it->type == ItemType::Group) for (const Item* c : sc.curves()) if (sc.isDescendant(c->id, it->id)) add(c);
  }
  std::vector<int> ok; for (int id : out) if (sc.item(id)->anchors.size() >= 2) ok.push_back(id);
  return ok;
}

std::vector<std::vector<int>> lockEnds(Scene& sc, const CurveCache& cc, const std::vector<int>& cs, int* dropped) {
  std::set<int> set(cs.begin(), cs.end()); std::vector<int> net; for (int id : cs) if (segNetCurve(*sc.item(id))) net.push_back(id);
  std::vector<std::vector<int>> out; std::set<std::vector<int>> seen;
  auto push = [&](const std::vector<int>& ids) {
    if (ids.size() < 2 || ids.front() == ids.back() || !seen.insert(ids).second) return;
    std::vector<int> e{ids.front(), ids.back()}; e.insert(e.end(), ids.begin() + 1, ids.end() - 1); out.push_back(e);
  };
  auto pathAnchorIds = [&](const Path& p) {
    std::vector<int> o;
    for (auto& s : p) { const Item* c = sc.item(s.c); std::vector<int> A = c->anchors; if (A.front() != s.from) std::reverse(A.begin(), A.end()); for (int a : A) if (o.empty() || o.back() != a) o.push_back(a); }
    return o;
  };
  if (!net.empty()) {
    auto all = pathsFor(sc, cc, net, true); std::vector<Path> P;
    for (auto& p : all) { bool ok = true; for (auto& s : p) if (!set.count(s.c)) ok = false; if (ok) P.push_back(p); }
    if (P.empty()) P = all;
    for (auto& p : P) push(pathAnchorIds(p));
  }
  for (int id : cs) {
    const Item* c = sc.item(id); if (segNetCurve(*c)) continue;
    std::vector<int> ids = c->anchors;
    if (!(sc.anchor(ids.front())->pos.y >= sc.anchor(ids.back())->pos.y)) std::reverse(ids.begin(), ids.end());
    push(ids);
  }
  // a lock splitting into branches: its main branch only — the tip most strands go to (a tie: more anchors)
  std::vector<int> tipOrder; std::map<int, std::vector<size_t>> byTip;
  for (size_t i = 0; i < out.size(); i++) { if (!byTip.count(out[i][1])) tipOrder.push_back(out[i][1]); byTip[out[i][1]].push_back(i); }
  if (byTip.size() > 1) {
    int bt = -1; for (int t : tipOrder) { auto& l = byTip[t]; if (bt < 0 || l.size() > byTip[bt].size() || (l.size() == byTip[bt].size() && out[l[0]].size() > out[byTip[bt][0]].size())) bt = t; }
    std::vector<std::vector<int>> best; for (size_t i : byTip[bt]) best.push_back(out[i]);
    if (dropped) *dropped = int(out.size() - best.size());
    return best;
  }
  if (dropped) *dropped = 0;
  return out;
}

namespace {
std::vector<Vec3> taubin(const std::vector<Vec3>& P, const std::vector<bool>& fixed, bool closed, int iters) {
  std::vector<Vec3> X = P; const int n = int(X.size());
  for (int it = 0; it < iters * 2; it++) {
    double k = it % 2 ? -0.53 : 0.5; std::vector<Vec3> Y = X;
    for (int i = 0; i < n; i++) {
      if (fixed[size_t(i)]) continue;
      int ia = closed ? (i + n - 1) % n : i - 1, ib = closed ? (i + 1) % n : i + 1; if (ia < 0 || ib >= n) continue;
      Y[size_t(i)] = Y[size_t(i)] + ((X[size_t(ia)] + X[size_t(ib)]) * 0.5 - X[size_t(i)]) * k;
    }
    X = Y;
  }
  return X;
}
int evenIters(double v) { v = std::clamp(v, 0.0, 1.0); return int(std::floor(v * v * 60 + 0.5)); }
}  // namespace

std::vector<int> evenAnchors(Scene& sc, const std::vector<int>& list, double v, double scale) {
  std::vector<int> touched;
  for (int cid : list) {
    Item* c = sc.item(cid); if (!c || c->ring >= 0 || c->anchors.size() < 3) continue;
    const int n = int(c->anchors.size()); std::vector<bool> fixed(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
      int a = c->anchors[size_t(i)]; bool shared = false;
      for (const Item* h : sc.curves()) if (h != c && !h->junc && std::find(h->anchors.begin(), h->anchors.end(), a) != h->anchors.end()) { shared = true; break; }
      const Anchor* A = sc.anchor(a);
      fixed[size_t(i)] = (!c->closed && (i == 0 || i == n - 1)) || (A && A->raw.contains("at") && !A->raw.value("at").isNull()) || shared;
    }
    std::vector<Vec3> src; for (const QJsonValue& q : c->raw.value("asrc").toArray()) { QJsonArray a = q.toArray(); src.push_back(Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble())); }
    bool valid = int(src.size()) == n;
    if (valid) { auto X = taubin(src, fixed, c->closed, evenIters(c->raw.value("asm").toDouble(0))); for (int i = 0; i < n; i++) if ((X[size_t(i)] - sc.anchor(c->anchors[size_t(i)])->pos).length() >= 1e-6 * scale) { valid = false; break; } }
    if (!valid) { src.clear(); for (int a : c->anchors) src.push_back(sc.anchor(a)->pos); }
    auto X = taubin(src, fixed, c->closed, evenIters(v));
    for (int i = 0; i < n; i++) sc.anchor(c->anchors[size_t(i)])->pos = X[size_t(i)];
    // (saved only while it is on, as the HTML version writes it)
    if (v) { QJsonArray s; for (auto& p : src) s.append(QJsonArray{p.x, p.y, p.z}); c->raw.insert("asrc", s); c->raw.insert("asm", v); }
    else { c->raw.remove("asm"); c->raw.remove("asrc"); }
    touched.insert(touched.end(), c->anchors.begin(), c->anchors.end());
  }
  return touched;
}

QString detachGroup(Scene& sc, int gid, Selection& sel) {
  const Item* g = sc.item(gid); if (!g) return "!";
  std::set<int> inside;
  std::function<void(const std::vector<int>&)> walk = [&](const std::vector<int>& ids) { for (int id : ids) if (const Item* it = sc.item(id)) { if (it->type == ItemType::Group) walk(it->children); else if (it->type == ItemType::Curve) inside.insert(id); } };
  walk(g->children);
  std::set<int> outA; for (const Item* c : sc.curves()) if (!inside.count(c->id)) outA.insert(c->anchors.begin(), c->anchors.end());
  std::vector<std::pair<int, int>> map;                       // (old → new, in the order they were met)
  auto mapped = [&](int a) { for (auto& [o, n] : map) if (o == a) return n; return -1; };
  for (int cid : inside) {
    Item* c = sc.item(cid); bool ch = false;
    for (int& a : c->anchors) { if (!outA.count(a)) continue;
      int nId = mapped(a); if (nId < 0) { Vec3 p = sc.anchor(a)->pos; double s = sc.anchor(a)->smooth; Anchor& n = sc.addAnchor(p); n.smooth = s; nId = n.id; map.push_back({a, nId}); }
      a = nId; ch = true; }
    if (ch) for (auto& [o, n] : map) { auto f = c->ovr.find(o); if (f != c->ovr.end()) { HandleOverride h = f->second; c->ovr.erase(o); c->ovr[n] = h; } }
  }
  // ends attached to a connector (crown) of another group are let go too
  int att = 0;
  for (int cid : inside) for (int a : sc.item(cid)->anchors) { Anchor* A = sc.anchor(a);
    if (A && A->raw.value("at").isObject()) { int h = A->raw.value("at").toObject().value("c").toInt(-1); if (sc.item(h) && !inside.count(h)) { A->raw.remove("at"); A->raw.remove("attPos"); att++; map.push_back({a, a}); } } }
  if (map.empty()) return "!" + QObject::tr("%1 isn't welded to other groups").arg(g->name);
  sc.reindex(); sel.anchors.clear();
  int n = int(map.size()) - att;
  return QObject::tr("%1: %2 shared anchor%3 detached%4").arg(g->name).arg(n).arg(n == 1 ? "" : "s")
         .arg(att ? QObject::tr(", %1 end%2 let go of another group's connector").arg(att).arg(att == 1 ? "" : "s") : QString());
}

void matchProjectStep(Scene& sc, int gid) {
  double st = projectStep(sc);
  std::function<void(int)> down = [&](int id) { Item* x = sc.item(id); QJsonObject c = groupConv(sc, x); c.insert("step", st); x->raw.insert("conv", c);
    for (int k : x->children) if (const Item* ci = sc.item(k); ci && ci->type == ItemType::Group) down(k); };
  down(gid);
}

int nextNumber(Scene& sc, ItemType t) { return nextNum(sc, t, itemTypeName(t), itemTypeKey(t)); }

void refreshCounters(Scene& sc) {
  QJsonObject cnt = sc.fileRaw.value("counters").toObject();
  for (const QString& k : cnt.keys()) { ItemType t = itemTypeFromString(k); if (t != ItemType::Unknown) nextNum(sc, t, itemTypeName(t), k); }
}

QJsonObject groupConv(Scene& sc, Item* g) {
  QJsonObject S = sc.fileRaw.value("settings").toObject();
  const char* KEYS[] = {"step", "dev"};   // (what the conversion reads — the old logic's others gone)
  QJsonObject c = g ? g->raw.value("conv").toObject() : QJsonObject{};
  if (!c.contains("step") || c.value("step").isNull()) c.insert("step", c.value("density").toDouble(0) > 0 ? sc.bounds().size().y / c.value("density").toDouble() : projectStep(sc));
  for (const char* kk : KEYS) if (!c.contains(kk)) c.insert(kk, S.value(kk));
  if (g) g->raw.insert("conv", c);
  return c;
}

int makeLiveCocoon(Scene& sc, const CurveCache& cc, CocoonSystem* cs, Selection& sel, QString* msg) {
  auto lock = lockCurves(sc, sel);
  if (lock.size() < 2) { if (msg) *msg = QObject::tr("◍ Select a lock — its group, its curves or its walls — then press Cocoon"); return -1; }
  int dropped = 0; auto ends = lockEnds(sc, cc, lock, &dropped);
  if (ends.size() < 2) { if (msg) *msg = QObject::tr("◍ The lock needs at least two strands (paths root → tip) for a cocoon"); return -1; }
  // its place in the tree: a cocoon selected with the lock — its branch; else the cocoon it grows out of
  int parent = -1; for (int id : sel.items) if (const Item* x = sc.item(id); x && x->type == ItemType::Cocoon) parent = id;
  int host = -1; QJsonObject shape;
  if (!cs || !cs->trialShape(ends, lock, parent < 0 ? &host : nullptr, &shape)) { if (msg) *msg = QObject::tr("◍ These strands never run side by side — no shape for a cocoon"); return -1; }
  if (parent < 0) parent = host;
  const Item* g = sc.item(lock[0]); while (g && g->type != ItemType::Group) g = sc.item(g->parent);
  QStringList has; for (auto& [id, it] : sc.items) if (it.type == ItemType::Cocoon) has << it.name;
  QString base = QString("%1 cocoon").arg(g ? g->name : "Lock"), nm = base; for (int n = 1; has.contains(nm);) nm = QString("%1 %2").arg(base).arg(++n);
  int cn = nextNum(sc, ItemType::Cocoon, "Cocoon", "cocoon"); (void)cn;
  Item& k = sc.addItem(ItemType::Cocoon, parent >= 0 ? parent : (g ? g->parent : -1), nm); k.expanded = true;
  QJsonArray E; for (auto& e : ends) { QJsonArray q; for (int a : e) q.append(a); E.append(q); }
  QJsonArray L; for (int id : lock) L.append(id);
  k.raw = QJsonObject{{"src", g ? QJsonValue(g->name) : QJsonValue()}, {"shape", shape}, {"conv", QJsonValue()}, {"live", QJsonObject{{"ends", E}, {"lock", L}}},
                      {"ringMode", "lock"}, {"op", QJsonValue()}, {"ringsOff", false}};
  if (parent < 0) {
    // (a trunk: the loop settings of its group — the group's own filled in as groupConv does — and its place right after it)
    if (g) k.raw.insert("conv", groupConv(sc, sc.item(g->id)));
    else k.raw.insert("conv", groupConv(sc, nullptr));
    if (g) { auto& ch = g->parent >= 0 ? sc.item(g->parent)->children : sc.roots; ch.erase(std::remove(ch.begin(), ch.end(), k.id), ch.end());
      auto at = std::find(ch.begin(), ch.end(), g->id); ch.insert(at == ch.end() ? ch.end() : at + 1, k.id); }
  } else sc.item(parent)->expanded = true;
  sel.anchors.clear(); sel.items = {k.id};
  if (msg) *msg = QObject::tr("◍ %1: %2 strands of %3 curves%4 — it follows the lock's shape; branches added later stay out").arg(nm).arg(ends.size()).arg(lock.size())
                  .arg(dropped ? QObject::tr(" — the main branch only (%1 strands of other branches left out)").arg(dropped) : QString());
  return k.id;
}

Item& makeUserCurve(Scene& sc, int parent) {
  QRegularExpression re("^Curve (\\d+)$"); int mx = 0;
  for (auto& [id, it] : sc.items) if (it.type == ItemType::Curve) { auto m = re.match(it.name); if (m.hasMatch()) mx = std::max(mx, m.captured(1).toInt()); }
  QJsonObject cnt = sc.fileRaw.value("counters").toObject(); cnt.insert("curve", mx + 1); sc.fileRaw.insert("counters", cnt);
  Item& c = sc.addItem(ItemType::Curve, parent, QString("Curve %1").arg(mx + 1));
  c.round = 0.6; c.fair = 0.5; c.roundSet = c.fairSet = true;   // (NEW_CURVE)
  return c;
}

int targetGroup(const Scene& sc, const Selection& sel) {
  const Item* it = sel.items.empty() ? nullptr : sc.item(sel.items.back());
  if (!it || it->type == ItemType::Head) return sc.iso && sc.iso->group >= 0 && sc.item(sc.iso->group) ? sc.iso->group : -1;
  return it->type == ItemType::Group ? it->id : it->parent;
}

}  // namespace hm::edit
