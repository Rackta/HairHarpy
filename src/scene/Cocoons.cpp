#include "scene/Cocoons.h"
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace hm {

namespace {

const double PI = 3.14159265358979323846, INF = std::numeric_limits<double>::infinity();
const int COCOON_NS = 120, TAIL_MAX = 4;   // (CMASK_N — Cocoons.h)
const double COCOON_FADE = 3, COCOON_BEND = 2, COCOON_TIP = 1.5;

int jsRound(double x) { return int(std::floor(x + 0.5)); }
double angleTo(const Vec3& a, const Vec3& b) {
  double d = std::sqrt(a.lengthSq() * b.lengthSq());
  return d == 0 ? PI / 2 : std::acos(clamp(a.dot(b) / d, -1, 1));
}
double r3(double v) { char buf[64]; std::snprintf(buf, sizeof buf, "%.3f", v); return std::strtod(buf, nullptr); }   // +v.toFixed(3)
Vec3 vec(const QJsonValue& v) { QJsonArray a = v.toArray(); return a.size() >= 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3{}; }
Vec3 atF(const std::vector<Vec3>& C, double f) {
  int i = std::clamp(int(std::floor(f)), 0, int(C.size()) - 2);
  return C[i].lerp(C[i + 1], clamp(f - i, 0, 1));
}
// closest point on a polyline, as a fractional index
double nearestF(const std::vector<Vec3>& P, const Vec3& p) {
  double best = 0, bd = INF;
  for (size_t i = 0; i + 1 < P.size(); i++) {
    Vec3 a = P[i], b = P[i + 1] - a; double l2 = b.lengthSq();
    double s = l2 > 0 ? (p - a).dot(b) / l2 : 0; s = s < 0 ? 0 : s > 1 ? 1 : s;
    double d = (a + b * s - p).lengthSq();
    if (d < bd) { bd = d; best = double(i) + s; }
  }
  return best;
}
double cocoonF(const CocoonGeo& G, double s) {
  int i = 0; while (i < G.N - 2 && G.D[i + 1] < s) i++;
  double L = G.D[i + 1] - G.D[i];
  return i + (L > 0 ? clamp((s - G.D[i]) / L, 0, 1) : 0);
}
Vec3 cocoonPt(const CocoonGeo& G, int i, double th) { return G.c[i] + G.u[i] * (std::cos(th) * G.a[i]) + G.v[i] * (std::sin(th) * G.b[i]); }
double cmaskAt(const std::vector<double>& M, double f) {
  if (M.empty()) return 0;
  double x = clamp(f, 0, 1) * (CMASK_N - 1); int i = std::min(CMASK_N - 2, int(std::floor(x)));
  return M[i] + (M[i + 1] - M[i]) * (x - i);
}
std::vector<double> numbers(const QJsonValue& v) { std::vector<double> o; for (const QJsonValue& x : v.toArray()) o.push_back(x.toDouble()); return o; }
std::string joinIds(const std::vector<int>& ids) { std::string s; for (size_t i = 0; i < ids.size(); i++) { if (i) s += ','; s += std::to_string(ids[i]); } return s; }

struct Shape { std::vector<Vec3> c, u; std::vector<double> a, b; };
struct Step { const Item* c; int from; };                       // a path step: the curve, the anchor it is entered at
using Path = std::vector<Step>;

class Ctx {
  struct K;

public:
  Ctx(const Scene& s, const CurveCache& c, const std::map<int, WallMesh>& walls) : sc(s), cc(c), wallOf(walls) {
    curves = sc.curves();
    for (auto& [id, it] : sc.items) if (it.type == ItemType::Surface) surfaces.push_back(&it);
    std::sort(surfaces.begin(), surfaces.end(), [](auto* a, auto* b) { return a->order < b->order; });
    for (auto& [id, it] : sc.items) if (it.type == ItemType::Guide) guides.push_back(&it);
    std::sort(guides.begin(), guides.end(), [](auto* a, auto* b) { return a->order < b->order; });
    Box3 hb; for (auto& [id, it] : sc.items) if (it.headTag && it.poly) hb.unite(it.poly->bounds());
    headH = hb.empty() ? 1 : hb.size().y; if (!(headH > 0)) headH = 1;
    levelH = projectStep(sc);
    QJsonObject st = sc.fileRaw.value("settings").toObject();
    devDefault = st.value("dev").toDouble(0.3);
    shellModsOff = sc.fileRaw.value("shellModsOff").toBool();
    for (const Item* c : curves) for (int a : c->anchors) if (!c->junc && !c->hasPath() && c->ring < 0) at[a].push_back(c);
    for (auto& [id, it] : sc.items) if (it.type == ItemType::Cocoon) kOrder.push_back(&it);
    std::sort(kOrder.begin(), kOrder.end(), [](auto* a, auto* b) { return a->order < b->order; });
    for (const Item* k : kOrder) {
      K& x = ks[k->id]; x.it = k;
      QJsonObject sh = k->raw.value("shape").toObject();
      if (sh.value("c").isArray()) {
        for (const QJsonValue& v : sh.value("c").toArray()) x.shape.c.push_back(vec(v));
        for (const QJsonValue& v : sh.value("u").toArray()) x.shape.u.push_back(vec(v));
        x.shape.a = numbers(sh.value("a")); x.shape.b = numbers(sh.value("b"));
      }
      QJsonObject live = k->raw.value("live").toObject();
      x.live = !live.isEmpty();
      for (const QJsonValue& e : live.value("ends").toArray()) { std::vector<int> v; for (const QJsonValue& q : e.toArray()) v.push_back(q.toInt()); x.ends.push_back(v); }
      x.hasLock = live.value("lock").isArray();
      for (const QJsonValue& q : live.value("lock").toArray()) x.lock.push_back(q.toInt());
    }
  }

  std::map<int, CocoonResult> results;

  // everything (as a project opens: the live ones reshaped from their strands, then all built)
  void rebuildAll() {
    for (const Item* k : kOrder) { K& x = ks[k->id]; if (x.live && !x.cidsSet) liveCocoonShape(x); }
    std::set<int> all; for (const Item* k : kOrder) all.insert(k->id);
    rebuild(all);
  }
  // curves changed (an anchor moved): a live cocoon whose strands run along one is reshaped; the reshaped ones and every
  // cocoon taking loops from them (on and on) are rebuilt, parents first — rebuildCocoons. → the rebuilt ids
  std::set<int> update(const std::set<int>& changed) {
    std::set<int> dirty;
    for (const Item* k : kOrder) {
      K& x = ks[k->id]; if (!x.live) continue;
      bool touched = !x.cidsSet;
      for (int id : x.cids) if (changed.count(id) || !sc.item(id)) touched = true;
      if (touched && liveCocoonShape(x)) dirty.insert(k->id);
    }
    if (!dirty.empty()) rebuild(dirty);
    return dirty;
  }
  bool liveState(int id, QJsonObject& shape, QJsonArray& ends) const {
    auto it = ks.find(id); if (it == ks.end() || it->second.shape.c.empty()) return false;
    const K& k = it->second;
    auto v3 = [](const Vec3& p) { return QJsonArray{p.x, p.y, p.z}; };
    QJsonArray c, u, a, b;
    for (auto& p : k.shape.c) c.append(v3(p));
    for (auto& p : k.shape.u) u.append(v3(p));
    for (double x : k.shape.a) a.append(x);
    for (double x : k.shape.b) b.append(x);
    shape = QJsonObject{{"c", c}, {"u", u}, {"a", a}, {"b", b}};
    ends = QJsonArray();
    if (k.live) for (auto& e : k.ends) { QJsonArray q; for (int x : e) q.append(x); ends.append(q); }
    return true;
  }

  void rebuild(std::set<int> set) {
    for (bool more = true; more;) {
      more = false;
      for (const Item* k : kOrder) if (!set.count(k->id)) for (K* d : deps(ks[k->id])) if (set.count(d->it->id)) { set.insert(k->id); more = true; break; }
    }
    std::map<int, int> DEP;
    std::function<int(K&)> depth = [&](K& k) {
      auto it = DEP.find(k.it->id); if (it != DEP.end()) return it->second;
      DEP[k.it->id] = 0; int d = -1; for (K* x : deps(k)) d = std::max(d, depth(*x)); DEP[k.it->id] = d + 1; return d + 1;
    };
    std::vector<K*> list; for (const Item* k : kOrder) if (set.count(k->id)) list.push_back(&ks[k->id]);
    std::stable_sort(list.begin(), list.end(), [&](K* a, K* b) { return depth(*a) < depth(*b); });
    for (K* k : list) k->ringsOk = false;
    for (K* k : list) rebuildOne(*k);
  }

private:
  void rebuildOne(K& x) {
    if (x.shape.c.size() < 3) { results.erase(x.it->id); return; }
    CocoonResult r; r.id = x.it->id; r.geo = geo(x); r.rings = ringsOf(x);
    r.lockMode = x.it->raw.value("ringMode").toString() == "lock";
    if (r.lockMode) {
      auto all = r.rings; auto tails = tailRings(x, r.geo, r.rings); all.insert(all.end(), tails.begin(), tails.end());
      r.lockSegs = lockRingSegs(x, r.geo, all);
    }
    results[x.it->id] = std::move(r);
  }

public:

private:
  struct K {
    const Item* it = nullptr;
    Shape shape;
    bool live = false, hasLock = false, cidsSet = false, geoOk = false, ringsOk = false;
    std::vector<std::vector<int>> ends;
    std::vector<int> lock, cids;
    CocoonGeo g;
    std::vector<CocoonRing> rings;
    std::vector<double> foldS;           // (k._foldS: the loops Convert forces in where the lock folds)
  };

public:
  // ---------- Convert to geometry ----------
  bool lockInput(int id, LockInput& out) {
    auto it = ks.find(id); if (it == ks.end()) return false;
    K& k = it->second; if (k.shape.c.size() < 3) return false;
    out = {}; out.id = id; out.G = geo(k);
    out.h = conv(k).first; if (!(out.h > 0)) out.h = levelH;
    out.rings = ringsOf(k);
    auto tails = tailRings(k, out.G, out.rings); out.rings.insert(out.rings.end(), tails.begin(), tails.end());
    std::stable_sort(out.rings.begin(), out.rings.end(), [](const CocoonRing& a, const CocoonRing& b) { return a.s < b.s; });
    for (const Item* s : lockWalls(k)) out.walls.push_back({s->id, s->a, s->b});
    if (k.live && !k.ends.empty() && k.ends[0].size() >= 2) {
      int t = k.ends[0][1]; bool all = true;
      for (auto& e : k.ends) if (e.size() < 2 || e[1] != t) all = false;
      if (all) for (const Item* c : curves) if (std::find(c->anchors.begin(), c->anchors.end(), t) != c->anchors.end()) { out.tipP.push_back(apos(t)); break; }
    }
    return true;
  }
  std::vector<int> depIds(int id) { std::vector<int> o; auto it = ks.find(id); if (it != ks.end()) for (K* d : deps(it->second)) o.push_back(d->it->id); return o; }
  bool dependsOn(int a, int b) {
    std::set<int> seen; std::vector<int> st{a};
    while (!st.empty()) { int x = st.back(); st.pop_back(); if (!seen.insert(x).second) continue; for (int d : depIds(x)) { if (d == b) return true; st.push_back(d); } }
    return false;
  }
  int lockGroupId(int id) { auto it = ks.find(id); if (it == ks.end()) return -1; const Item* g = lockGroup(it->second); return g ? g->id : -1; }
  bool trialShape(std::vector<std::vector<int>>& ends, const std::vector<int>& lock, int* host, QJsonObject* shapeOut) {
    K T; T.live = true; T.ends = ends; T.lock = lock; T.hasLock = true; T.it = nullptr;
    Polys P = livePolys(T.ends, T.lock, T.hasLock); ends = T.ends;
    if (P.polys.size() < 2) return false;
    auto U = cocoonShape(P.polys); if (!U) return false;
    T.shape = *U;
    if (shapeOut) {
      auto v3 = [](const Vec3& p) { return QJsonArray{p.x, p.y, p.z}; };
      QJsonArray c, u, a, b; for (auto& p : T.shape.c) c.append(v3(p)); for (auto& p : T.shape.u) u.append(v3(p));
      for (double x : T.shape.a) a.append(x); for (double x : T.shape.b) b.append(x);
      *shapeOut = QJsonObject{{"c", c}, {"u", u}, {"a", a}, {"b", b}};
    }
    if (host) {
      const CocoonGeo& G = geo(T);
      auto depth = [&](const Item* k) { int d = 0; for (const Item* p = parentOf(k); p && p->type == ItemType::Cocoon; p = parentOf(p)) d++; return d; };
      const Item* best = nullptr; int bs = 0, bd = -1;
      for (const Item* Pi : kOrder) {
        K& Pk = ks[Pi->id]; if (Pk.shape.c.size() < 3 || !Pi->visible) continue;
        if (!inCocoon(Pk, G.c[size_t(std::min(2, G.N - 1))], 1.15)) continue;   // (its root — a couple of stations in — inside P)
        int s = 0; for (int i = 0; i < G.N; i++) { if (inCocoon(Pk, G.c[size_t(i)], 1.15)) s++; else if (i > 4) break; }
        int d = depth(Pi);
        if (s > bs || (s == bs && d > bd)) { best = Pi; bs = s; bd = d; }
      }
      *host = bs >= 3 && best ? best->id : -1;
    }
    return true;
  }
  std::vector<int> strandsOf(int id) {
    auto it = ks.find(id); if (it == ks.end() || !it->second.live) return {};
    K& k = it->second; auto e = k.ends; return livePolys(e, k.lock, k.hasLock).cids;
  }
  CocoonGeo geoOf(int id) { auto it = ks.find(id); if (it == ks.end() || it->second.shape.c.size() < 3) return {}; return geo(it->second); }
  double loopStepOf(int id) { auto it = ks.find(id); if (it == ks.end()) return levelH; double h = conv(it->second).first; return h > 0 ? h : levelH; }
  void setFolds(int id, const std::vector<double>& s) {
    auto it = ks.find(id); if (it == ks.end()) return;
    it->second.foldS = s;
    for (const Item* c : kOrder) if (c->id == id || dependsOn(c->id, id)) ks[c->id].ringsOk = false;
  }
  const std::vector<double>& foldsOf(int id) { static const std::vector<double> none; auto it = ks.find(id); return it == ks.end() ? none : it->second.foldS; }

private:

  const Scene& sc; const CurveCache& cc;
  std::vector<const Item*> curves, surfaces, guides, kOrder;
  const std::map<int, WallMesh>& wallOf;
  std::map<int, K> ks;
  std::unordered_map<int, std::vector<const Item*>> at;          // anchor → the hair curves holding it
  double headH = 1, levelH = 1, devDefault = 0.3;
  bool shellModsOff = false;

  // ---------- small helpers ----------
  const CurveSampling* smp(int id) const { auto i = cc.find(id); return i == cc.end() ? nullptr : &i->second; }
  const Vec3& apos(int id) const { static Vec3 z; const Anchor* a = sc.anchor(id); return a ? a->pos : z; }
  const Item* parentOf(const Item* it) const { return it && it->parent >= 0 ? sc.item(it->parent) : nullptr; }
  static bool segNet(const Item& h) { return !(h.junc || h.hasPath() || h.ring >= 0 || !h.conn.isEmpty() || h.closed || h.hasRib); }
  static int idx(const std::vector<int>& A, int a) { for (size_t i = 0; i < A.size(); i++) if (A[i] == a) return int(i); return -1; }
  static int lastIdx(const std::vector<int>& A, int a) { for (int i = int(A.size()) - 1; i >= 0; i--) if (A[i] == a) return i; return -1; }
  std::vector<std::pair<int, std::vector<int>>> pathOf(const Item& c) const {
    std::vector<std::pair<int, std::vector<int>>> o;
    for (auto& s : c.path) o.push_back({s[0], {}});
    return o;
  }
  K* cocoonParent(const K& k) { const Item* p = parentOf(k.it); return p && p->type == ItemType::Cocoon ? &ks[p->id] : nullptr; }
  std::vector<K*> deps(const K& k) {
    std::vector<K*> out;
    auto add = [&](K* x) { if (x && std::find(out.begin(), out.end(), x) == out.end()) out.push_back(x); };
    add(cocoonParent(k));
    for (const QJsonValue& v : k.it->raw.value("links").toArray()) { int id = v.toInt(); const Item* L = sc.item(id); if (L && L->type == ItemType::Cocoon && id != k.it->id) add(&ks[id]); }
    return out;
  }

  // groupConv: the group's loop settings, step / dev filled from the project's when missing
  std::pair<double, double> groupConv(const Item* g) const {
    QJsonObject c = g->raw.value("conv").toObject();
    double step = c.value("step").toDouble(0);
    if (!(step > 0)) step = c.value("density").toDouble(0) > 0 ? headH / c.value("density").toDouble() : levelH;
    double dev = c.contains("dev") && !c.value("dev").isNull() ? c.value("dev").toDouble() : devDefault;
    return {step, dev};
  }
  const Item* lockGroup(const K& k) const {
    const Item* g = !k.cids.empty() ? sc.item(k.cids.front()) : nullptr;
    while (g && g->type != ItemType::Group) g = parentOf(g);
    if (g) return g;
    QString src = k.it->raw.value("src").toString();
    if (!src.isEmpty()) for (auto& [id, it] : sc.items) if (it.type == ItemType::Group && it.name == src) return &it;
    return nullptr;
  }
  // cocoonConv: its lock's group's (a live one), else the trunk's own copy, else its nearest group's
  std::pair<double, double> conv(K& k) {
    K* t = &k; while (K* p = cocoonParent(*t)) t = p;
    for (K* x : {&k, t}) if (x->live) if (const Item* g = lockGroup(*x)) return groupConv(g);
    if (t->it->raw.value("conv").isObject()) return groupConv(t->it);
    for (const Item* p = parentOf(t->it); p; p = parentOf(p))
      if (p->type == ItemType::Group || (p->type == ItemType::Cocoon && p->raw.value("conv").isObject())) return groupConv(p);
    return {levelH, devDefault};
  }

  const CocoonGeo& geo(K& k) {
    if (k.geoOk) return k.g;
    CocoonGeo& G = k.g; G = {};
    int N = int(k.shape.c.size()); G.N = N; G.c = k.shape.c; G.a = k.shape.a; G.b = k.shape.b;
    for (int i = 0; i < N; i++) G.t.push_back((G.c[std::min(i + 1, N - 1)] - G.c[std::max(i - 1, 0)]).normalized());
    for (int i = 0; i < N; i++) { Vec3 x = k.shape.u[i]; G.u.push_back((x - G.t[i] * x.dot(G.t[i])).normalized()); G.v.push_back(G.t[i].cross(G.u[i])); }
    G.D.assign(N, 0); for (int i = 1; i < N; i++) G.D[i] = G.D[i - 1] + G.c[i].distanceTo(G.c[i - 1]);
    k.geoOk = true;
    return G;
  }

  // ---------- the curve network: paths through the forks ----------
  Vec3 leaveDir(const Item& c, int a) const {
    const CurveSampling* s = smp(c.id); std::vector<Vec3> A;
    const std::vector<Vec3>* P = s && s->p.size() > 1 ? &s->p : nullptr;
    if (!P) { for (int x : c.anchors) A.push_back(apos(x)); P = &A; }
    int n = int(P->size()), k = std::min(3, n - 1);
    Vec3 d = c.anchors.front() == a ? (*P)[k] - (*P)[0] : (*P)[n - 1 - k] - (*P)[n - 1];
    return d.lengthSq() > 1e-14 ? d.normalized() : d;
  }
  static int other(const Item& c, int a) { return c.anchors.front() == a ? c.anchors.back() : c.anchors.front(); }
  std::vector<Path> curvePaths(const std::vector<const Item*>& sel) const {   // (all: hidden curves too)
    std::vector<const Item*> net; for (const Item* c : curves) if (segNet(*c) && c->anchors.size() >= 2) net.push_back(c);
    std::unordered_map<int, std::vector<const Item*>> ends;
    for (const Item* c : net) for (int a : {c->anchors.front(), c->anchors.back()}) { auto& l = ends[a]; if (std::find(l.begin(), l.end(), c) == l.end()) l.push_back(c); }
    using Walk = std::vector<std::pair<const Item*, int>>;
    std::function<std::vector<Walk>(const Item*, int, std::set<const Item*>&, int)> walk = [&](const Item* c, int a, std::set<const Item*>& used, int depth) {
      Vec3 din = -leaveDir(*c, a);
      std::vector<const Item*> next;
      auto it = ends.find(a);
      if (it != ends.end()) for (const Item* e : it->second) if (e != c && !used.count(e) && leaveDir(*e, a).dot(din) > 0) next.push_back(e);
      if (next.empty() || depth > 60) return std::vector<Walk>{Walk{}};
      std::vector<Walk> out;
      for (const Item* e : next) {
        used.insert(e);
        for (auto& rest : walk(e, other(*e, a), used, depth + 1)) { Walk w{{e, a}}; w.insert(w.end(), rest.begin(), rest.end()); out.push_back(std::move(w)); if (out.size() > 400) break; }
        used.erase(e);
        if (out.size() > 400) break;
      }
      return out;
    };
    const Item* c0 = sel.back(); int A0 = c0->anchors.front(), A1 = c0->anchors.back();
    std::set<const Item*> u1{c0}, u2{c0};
    auto fw = walk(c0, A1, u1, 0), bw = walk(c0, A0, u2, 0);
    std::vector<Path> out; std::set<std::string> seen;
    for (auto& b : bw) for (auto& f : fw) {
      std::set<const Item*> ids{c0}; for (auto& x : b) ids.insert(x.first); for (auto& x : f) ids.insert(x.first);
      if (ids.size() != 1 + b.size() + f.size()) continue;
      bool all = true; for (const Item* c : sel) if (!ids.count(c)) all = false; if (!all) continue;
      Path steps;
      for (auto it = b.rbegin(); it != b.rend(); ++it) steps.push_back({it->first, other(*it->first, it->second)});
      steps.push_back({c0, A0});
      for (auto& x : f) steps.push_back({x.first, x.second});
      std::vector<int> key; for (auto& s : steps) key.push_back(s.c->id);
      if (!seen.insert(joinIds(key)).second) continue;
      out.push_back(steps);
      if (out.size() >= 200) break;
    }
    for (auto& p : out) {                                        // the root first: the higher of the path's two ends
      int e0 = p.front().from, eN = other(*p.back().c, p.back().from);
      if (apos(eN).y > apos(e0).y) { std::reverse(p.begin(), p.end()); for (auto& s : p) s.from = other(*s.c, s.from); }
    }
    return out;
  }
  std::vector<Path> pathsFor(const std::vector<const Item*>& sel) const {
    std::vector<Path> out; std::set<std::string> seen;
    for (auto it = sel.rbegin(); it != sel.rend(); ++it) for (auto& p : curvePaths({*it})) {
      std::vector<int> key; for (auto& s : p) key.push_back(s.c->id);
      if (seen.insert(joinIds(key)).second) out.push_back(p);
    }
    return out;
  }
  static std::vector<int> pathAnchorIds(const Path& p) {
    std::vector<int> out;
    for (auto& s : p) {
      std::vector<int> A = s.c->anchors; if (A.front() != s.from) std::reverse(A.begin(), A.end());
      for (int a : A) if (out.empty() || out.back() != a) out.push_back(a);
    }
    return out;
  }
  // lockEnds: the strands of the lock [root, tip, …between]; a lock splitting into branches — the main branch's (all: every strand)
  struct Ends { std::vector<std::vector<int>> main, all; };
  Ends lockEnds(const std::vector<const Item*>& cs) const {
    std::set<const Item*> set(cs.begin(), cs.end());
    std::vector<const Item*> net; for (const Item* c : cs) if (segNet(*c)) net.push_back(c);
    std::vector<std::vector<int>> out; std::set<std::string> seen;
    auto push = [&](const std::vector<int>& ids) {
      if (ids.size() < 2 || ids.front() == ids.back() || !seen.insert(joinIds(ids)).second) return;
      std::vector<int> e{ids.front(), ids.back()}; e.insert(e.end(), ids.begin() + 1, ids.end() - 1); out.push_back(e);
    };
    if (!net.empty()) {
      auto all = pathsFor(net); std::vector<Path> P;
      for (auto& p : all) { bool ok = true; for (auto& s : p) if (!set.count(s.c)) ok = false; if (ok) P.push_back(p); }
      if (P.empty()) P = all;
      for (auto& p : P) push(pathAnchorIds(p));
    }
    for (const Item* c : cs) if (!segNet(*c)) { auto ids = c->anchors; if (!(apos(ids.front()).y >= apos(ids.back()).y)) std::reverse(ids.begin(), ids.end()); push(ids); }
    std::vector<int> tipOrder; std::map<int, std::vector<std::vector<int>>> byTip;
    for (auto& e : out) { if (!byTip.count(e[1])) tipOrder.push_back(e[1]); byTip[e[1]].push_back(e); }
    if (byTip.size() > 1) {
      const std::vector<std::vector<int>>* best = nullptr;
      for (int t : tipOrder) { auto& l = byTip[t]; if (!best || l.size() > best->size() || (l.size() == best->size() && l[0].size() > (*best)[0].size())) best = &l; }
      return {*best, out};
    }
    return {out, out};
  }

  // the sample index of a curve's anchor k; the curve's points from its anchor i to its anchor j
  static int ancSample(const Item& c, const CurveSampling& s, int k) {
    if (k <= 0) return 0;
    if (k >= int(c.anchors.size()) - 1) return int(s.p.size()) - 1;
    for (size_t q = 0; q < s.seg.size(); q++) if (s.seg[q] >= k) return int(q);
    return 0;
  }
  std::vector<Vec3> curveStretch(const Item& c, int i, int j) const {
    const CurveSampling* s = smp(c.id); if (!s) return {};
    int a = ancSample(c, *s, i), b = ancSample(c, *s, j);
    std::vector<Vec3> P(s->p.begin() + std::min(a, b), s->p.begin() + std::max(a, b) + 1);
    if (a > b) std::reverse(P.begin(), P.end());
    return P;
  }

  // ---------- live cocoons: their strands' polylines as the curves are now ----------
  struct Polys { std::vector<std::vector<Vec3>> polys; std::vector<int> cids; };
  Polys livePolys(std::vector<std::vector<int>>& ends, const std::vector<int>& lock, bool hasLock) {
    Polys R; std::unordered_set<int> cidSet;
    auto has = [&](int a) { return at.count(a) > 0; };
    auto posIn = [&](int a) -> const Vec3* { auto it = at.find(a); return it == at.end() || it->second.empty() ? nullptr : &apos(a); };
    // an end whose anchor is gone: on along the curves (the one going on most straight) to a real end
    auto out = [&](int a, int b) {
      int from = b, cur = a;
      for (int g = 0; g < 50; g++) {
        const Vec3 *pa = posIn(cur), *pf = posIn(from); if (!pa || !pf) return cur;
        Vec3 dir = (*pa - *pf).normalized(); const Item* bc = nullptr; int bj = 0, bdl = 0; double bd = -INF;
        for (const Item* c : at[cur]) {
          int i = idx(c->anchors, cur);
          for (int j : {i - 1, i + 1}) {
            if (j < 0 || j >= int(c->anchors.size()) || c->anchors[j] == from) continue;
            double dd = (apos(c->anchors[j]) - *pa).normalized().dot(dir);
            if (dd > bd) { bd = dd; bc = c; bj = j; bdl = j - i; }
          }
        }
        if (!bc || bd < 0) return cur;
        const auto& A = bc->anchors; int end = bdl > 0 ? int(A.size()) - 1 : 0;
        from = A[end - bdl]; cur = A[end];
      }
      return cur;
    };
    auto fixEnd = [&](const std::vector<int>& e) {
      int r = e[0], t = e[1]; if (has(r) && has(t)) return e;
      std::vector<int> ids; if (has(r)) ids.push_back(r);
      for (size_t q = 2; q < e.size(); q++) if (has(e[q])) ids.push_back(e[q]);
      if (has(t)) ids.push_back(t);
      if (ids.size() < 2) return e;
      int nr = has(r) ? r : out(ids[0], ids[1]), nt = has(t) ? t : out(ids.back(), ids[ids.size() - 2]);
      if (nr == nt) return e;
      std::vector<int> o{nr, nt}; for (int id : ids) if (id != nr && id != nt) o.push_back(id);
      return o;
    };
    bool goneLock = false; for (int id : lock) if (!sc.item(id)) goneLock = true;
    auto reEnd = [&](const std::vector<int>& e) {
      if (!goneLock || e.size() <= 2) return e;
      int t = e[1], lv = e.back(); if (!has(t) || !has(lv)) return e;
      int nt = out(t, lv); if (nt == t) return e;
      std::vector<int> o{e[0], nt}; o.insert(o.end(), e.begin() + 2, e.end()); o.push_back(t);
      return o;
    };
    auto meetEnd = [](std::vector<std::vector<int>> o) {
      while (o.size() >= 2) {
        bool all = true;
        for (auto& e : o) if (!(e.size() > 2 && e[1] == o[0][1] && e.back() == o[0].back())) all = false;
        if (!all) break;
        int v = o[0].back();
        for (auto& e : o) { std::vector<int> n{e[0], v}; n.insert(n.end(), e.begin() + 2, e.end() - 1); e = n; }
      }
      return o;
    };
    { std::vector<std::vector<int>> fx; for (auto& e : ends) fx.push_back(reEnd(fixEnd(e))); fx = meetEnd(fx); ends = fx; }
    (void)hasLock;
    std::unordered_map<int, std::vector<Path>> pathsAt;
    for (auto& e : ends) {
      int r = e[0], t = e[1]; std::set<int> want(e.begin() + 2, e.end());
      auto it = at.find(r); if (it == at.end() || !has(t)) continue;
      const auto& cs = it->second;
      std::vector<Vec3> pts; std::vector<int> used; bool havePts = false;
      for (const Item* c : cs) if (!segNet(*c)) {                         // (the old logic: one curve holds both)
        int i = idx(c->anchors, r), j = idx(c->anchors, t); const CurveSampling* s = smp(c->id);
        if (i >= 0 && j >= 0 && s && s->p.size() >= 2) { pts = curveStretch(*c, i, j); used = {c->id}; havePts = true; break; }
      }
      if (!havePts) {
        auto pi = pathsAt.find(r);
        if (pi == pathsAt.end()) {
          std::vector<Path> P; std::set<std::string> seen;
          for (const Item* c : cs) if (segNet(*c)) for (auto& p : curvePaths({c})) {
            std::vector<int> key; for (auto& s : p) key.push_back(s.c->id);
            if (seen.insert(joinIds(key)).second) P.push_back(p);
          }
          pi = pathsAt.emplace(r, std::move(P)).first;
        }
        int best = -1;
        for (auto& p : pi->second) {
          struct Sg { const Item* c; int k0, k1; }; std::vector<Sg> segs;
          for (auto& s : p) { int i0 = idx(s.c->anchors, s.from), d = i0 == 0 ? 1 : -1; for (int q = i0; q + d >= 0 && q + d < int(s.c->anchors.size()); q += d) segs.push_back({s.c, q, q + d}); }
          std::vector<int> ids{segs.empty() ? -1 : segs[0].c->anchors[segs[0].k0]}; for (auto& s : segs) ids.push_back(s.c->anchors[s.k1]);
          int i = idx(ids, r), j = lastIdx(ids, t);
          if (i < 0 || j < 0 || i == j) continue;
          bool rev = i > j; if (rev) std::swap(i, j);
          int score = 0; for (int q = i + 1; q < j; q++) if (want.count(ids[q])) score++;
          if (score <= best) continue;
          std::vector<Vec3> Q; std::vector<int> u;
          for (int q = i; q < j; q++) { auto st = curveStretch(*segs[q].c, segs[q].k0, segs[q].k1); Q.insert(Q.end(), Q.empty() ? st.begin() : st.begin() + std::min<size_t>(1, st.size()), st.end()); u.push_back(segs[q].c->id); }
          if (rev) std::reverse(Q.begin(), Q.end());
          if (Q.size() >= 2) { pts = Q; used = u; best = score; havePts = true; }
        }
      }
      if (havePts && pts.size() >= 2) { R.polys.push_back(pts); for (int id : used) if (cidSet.insert(id).second) R.cids.push_back(id); }
    }
    return R;
  }

  // cocoonShape: the spine (the mean of the strands running its whole length) and the ovals fitted to the strands
  std::optional<Shape> cocoonShape(const std::vector<std::vector<Vec3>>& polys) const {
    auto len = [](const std::vector<Vec3>& Q) { double L = 0; for (size_t i = 1; i < Q.size(); i++) L += Q[i].distanceTo(Q[i - 1]); return L; };
    auto resample = [](const std::vector<Vec3>& Q, int n) {
      std::vector<double> L{0}; for (size_t i = 1; i < Q.size(); i++) L.push_back(L.back() + Q[i].distanceTo(Q[i - 1]));
      double T = L.back() ? L.back() : 1; std::vector<Vec3> out; size_t j = 0;
      for (int k = 0; k < n; k++) { double s = T * k / (n - 1); while (j < Q.size() - 2 && L[j + 1] < s) j++; out.push_back(Q[j].lerp(Q[j + 1], L[j + 1] > L[j] ? clamp((s - L[j]) / (L[j + 1] - L[j]), 0, 1) : 0)); }
      return out;
    };
    const int N = COCOON_NS; double lv = levelH;
    std::vector<double> lens; double mx = 0; for (auto& Q : polys) { lens.push_back(len(Q)); mx = std::max(mx, lens.back()); }
    std::vector<std::vector<Vec3>> R; for (size_t i = 0; i < polys.size(); i++) if (lens[i] >= 0.85 * mx) R.push_back(resample(polys[i], N));
    std::vector<Vec3> S(N); for (int k = 0; k < N; k++) { Vec3 s; for (auto& r : R) s += r[k]; S[k] = s / double(R.size()); }
    double rad = 0; for (auto& Q : polys) for (auto& q : Q) rad = std::max(rad, q.distanceTo(S[std::clamp(jsRound(nearestF(S, q)), 0, N - 1)]));
    double reach = 2 * rad + lv;
    struct Raw { Vec3 c, u, t; double a, b; bool ok = false; };
    std::vector<Raw> raw(N);
    auto crossAt = [&](const std::vector<Vec3>& Q, const Vec3& o, const Vec3& t, double lim, Vec3& best) {
      double bd = lim; bool found = false;
      for (size_t j = 0; j + 1 < Q.size(); j++) {
        double d0 = (Q[j] - o).dot(t), d1 = (Q[j + 1] - o).dot(t); if (d0 == d1 || d0 * d1 > 0) continue;
        Vec3 q = Q[j].lerp(Q[j + 1], d0 / (d0 - d1)); double dd = q.distanceTo(o);
        if (dd < bd) { bd = dd; best = q; found = true; }
      }
      return found;
    };
    for (int i = 0; i < N; i++) {
      Vec3 o = S[i], t = (S[std::min(i + 1, N - 1)] - S[std::max(i - 1, 0)]).normalized();
      std::vector<Vec3> pts; for (auto& Q : polys) { Vec3 b; if (crossAt(Q, o, t, reach, b)) pts.push_back(b); }
      Vec3 e1 = std::abs(t.y) < 0.9 ? Vec3(0, 1, 0).cross(t).normalized() : Vec3(1, 0, 0).cross(t).normalized(), e2 = t.cross(e1);
      if (pts.size() >= 4) {
        Vec3 m0; for (auto& p : pts) m0 += p; m0 = m0 / double(pts.size());
        std::vector<double> ds; for (auto& p : pts) ds.push_back(p.distanceTo(m0));
        auto sd = ds; std::sort(sd.begin(), sd.end()); double med = sd[sd.size() / 2];
        std::vector<Vec3> keep; for (size_t q = 0; q < pts.size(); q++) if (ds[q] <= 3 * med + 0.05 * lv) keep.push_back(pts[q]);
        if (keep.size() >= 2) pts = keep;
      }
      if (pts.size() < 2) continue;
      Vec3 c; for (auto& p : pts) c += p; c = c / double(pts.size());
      double xx = 0, xy = 0, yy = 0; for (auto& p : pts) { Vec3 d = p - c; double x = d.dot(e1), y = d.dot(e2); xx += x * x; xy += x * y; yy += y * y; }
      double ang = 0.5 * std::atan2(2 * xy, xx - yy);
      Vec3 u = (e1 * std::cos(ang) + e2 * std::sin(ang)).normalized(), v = t.cross(u);
      double u0 = INF, u1 = -INF, v0 = INF, v1 = -INF;
      for (auto& p : pts) { Vec3 d = p - c; double x = d.dot(u), y = d.dot(v); u0 = std::min(u0, x); u1 = std::max(u1, x); v0 = std::min(v0, y); v1 = std::max(v1, y); }
      Vec3 ccen = c + u * ((u0 + u1) / 2) + v * ((v0 + v1) / 2); double mr = 0.02 * lv;
      raw[i] = {ccen, u, t, std::max((u1 - u0) / 2, mr), std::max((v1 - v0) / 2, mr), true};
    }
    bool any = false; for (auto& r : raw) if (r.ok) any = true;
    if (!any) return std::nullopt;
    for (int i = 0; i < N; i++) if (!raw[i].ok) {
      auto okAt = [&](int q) { return q >= 0 && q < N && raw[q].ok; };
      int j = 1; while (!okAt(i - j) && !okAt(i + j) && j < N) j++;
      Raw src = okAt(i - j) ? raw[i - j] : raw[i + j];
      src.c = S[i]; src.t = (S[std::min(i + 1, N - 1)] - S[std::max(i - 1, 0)]).normalized(); src.ok = true; raw[i] = src;
    }
    struct F_ { Vec3 c, u; double a, b; }; std::vector<F_> F;
    for (int i = 0; i < N; i++) {
      Vec3 u = raw[i].u; double a = raw[i].a, b = raw[i].b;
      if (!F.empty()) { Vec3 pu = F[i - 1].u; if (std::abs(u.dot(pu)) < std::sqrt(0.5)) { u = raw[i].t.cross(u); std::swap(a, b); } if (u.dot(pu) < 0) u = -u; }
      F.push_back({raw[i].c, u, a, b});
    }
    const int Wn = 6; double minR = 0.04 * lv;
    auto sm = [&](auto g, int i) { int w = std::min({Wn, i, N - 1 - i}); double s = 0; int n = 0; for (int q = i - w; q <= i + w; q++) { s += g(q); n++; } return s / n; };
    Shape out; std::vector<double> A, B;
    for (int i = 0; i < N; i++) {
      out.c.push_back({r3(sm([&](int q) { return F[q].c.x; }, i)), r3(sm([&](int q) { return F[q].c.y; }, i)), r3(sm([&](int q) { return F[q].c.z; }, i))});
      out.u.push_back({r3(sm([&](int q) { return F[q].u.x; }, i)), r3(sm([&](int q) { return F[q].u.y; }, i)), r3(sm([&](int q) { return F[q].u.z; }, i))});
      A.push_back(std::max(sm([&](int q) { return F[q].a; }, i), minR)); B.push_back(std::max(sm([&](int q) { return F[q].b; }, i), minR));
    }
    std::vector<double> ra = A, rb = B; const auto& fc = out.c;
    for (int i = 0; i < N; i++) {
      Vec3 t = (fc[std::min(i + 1, N - 1)] - fc[std::max(i - 1, 0)]).normalized(), u0 = out.u[i];
      Vec3 u = (u0 - t * u0.dot(t)).normalized(), v = t.cross(u), o = fc[i];
      double nearL = 1.6 * std::max(A[i], B[i]), a0 = INF, a1 = -INF, b0 = INF, b1 = -INF; int m = 0;
      for (auto& Q : polys) { Vec3 b; if (crossAt(Q, o, t, std::min(reach, nearL), b)) { Vec3 d = b - o; double x = d.dot(u), y = d.dot(v); a0 = std::min(a0, x); a1 = std::max(a1, x); b0 = std::min(b0, y); b1 = std::max(b1, y); m++; } }
      if (m >= 2) { ra[i] = std::max((a1 - a0) / 2, minR); rb[i] = std::max((b1 - b0) / 2, minR); }
    }
    std::vector<double> big; for (int i = 0; i < N; i++) big.push_back(std::max(ra[i], rb[i]));
    std::sort(big.begin(), big.end()); double floorR = 0.25 * big[big.size() / 2];
    for (int i = 0; i < N; i++) { ra[i] = std::max({ra[i], floorR, rb[i] / 3}); rb[i] = std::max({rb[i], floorR, ra[i] / 3}); }
    int iS = int(std::floor(0.75 * (N - 1)));
    for (int i = iS + 1; i < N; i++) { double x = double(i - iS) / (N - 1 - iS), cap = std::sqrt(std::max(0.0, 1 - x * x)); ra[i] = std::max(ra[i], ra[iS] * cap); rb[i] = std::max(rb[i], rb[iS] * cap); }
    const int w = 4;
    auto avg = [&](const std::vector<double>& r) { std::vector<double> o(N); for (int i = 0; i < N; i++) { double s = 0; int n = 0; for (int q = std::max(0, i - w); q <= std::min(N - 1, i + w); q++) { s += r[q]; n++; } o[i] = 1.04 * s / n; } return o; };
    auto ea = avg(ra), eb = avg(rb);
    for (int i = 0; i < N; i++) { double tipK = clamp((N - 1 - i) / 3.0, 0, 1), k = 0.15 + 0.85 * tipK; out.a.push_back(r3(ea[i] * k)); out.b.push_back(r3(eb[i] * k)); }
    return out;
  }

  bool liveCocoonShape(K& k) {
    Polys P = livePolys(k.ends, k.lock, k.hasLock);
    k.cids = P.cids; k.cidsSet = true;
    if (P.polys.size() < 2) return false;
    auto U = cocoonShape(P.polys); if (!U) return false;
    k.shape = *U; k.geoOk = false; k.ringsOk = false;
    return true;
  }

  bool inCocoon(K& P, const Vec3& p, double slack) {
    const CocoonGeo& G = geo(P); int j = std::clamp(jsRound(nearestF(G.c, p)), 0, G.N - 1); Vec3 d = p - G.c[j];
    if (std::abs(d.dot(G.t[j])) > std::max(G.D[std::min(j + 1, G.N - 1)] - G.D[std::max(j - 1, 0)], 1e-6)) return false;
    return std::hypot(d.dot(G.u[j]) / std::max(G.a[j], 1e-6), d.dot(G.v[j]) / std::max(G.b[j], 1e-6)) <= slack;
  }

  // ---------- the lock's walls ----------
  std::vector<const Item*> lockWallsOwn(K& k) {
    std::vector<int> Cv; std::set<int> C;
    auto addC = [&](int id) { if (sc.item(id) && C.insert(id).second) Cv.push_back(id); };
    bool haveC = false;
    if (k.live) {
      std::vector<int> cids = k.cidsSet && !k.cids.empty() ? k.cids : [&] { auto e = k.ends; return livePolys(e, k.lock, k.hasLock).cids; }();
      for (int id : cids) addC(id); for (int id : k.lock) addC(id);
      if (!k.hasLock) if (const Item* g = lockGroup(k)) for (const Item* c : curves) if (sc.isDescendant(c->id, g->id)) addC(c->id);
      haveC = !C.empty();
    }
    if (!haveC) {
      QString src = k.it->raw.value("src").toString(); const Item* g = nullptr;
      for (auto& [id, it] : sc.items) if (it.type == ItemType::Group && it.name == src) { g = &it; break; }
      if (!g) return {};
      for (const Item* c : curves) if (sc.isDescendant(c->id, g->id)) addC(c->id);
    }
    std::unordered_set<int> AN; for (int id : C) if (const Item* c = sc.item(id)) for (int a : c->anchors) AN.insert(a);
    std::unordered_set<int> endA; if (k.live) for (auto& e : k.ends) { endA.insert(e[0]); endA.insert(e[1]); }
    auto pastEnd = [&](int id) { const Item* c = sc.item(id); if (!c) return false; for (int a : c->anchors) if (endA.count(a)) return true; return false; };
    auto pathOn = [&](const std::vector<std::pair<int, std::vector<int>>>& P) {
      int i = 0, j = int(P.size()) - 1;
      while (i <= j && !C.count(P[i].first) && pastEnd(P[i].first)) i++;
      while (j >= i && !C.count(P[j].first) && pastEnd(P[j].first)) j--;
      if (i > j) return false;
      for (int q = i; q <= j; q++) if (!C.count(P[q].first)) return false;
      return true;
    };
    auto on = [&](int id) {
      const Item* c = sc.item(id); if (!c) return false;
      if (c->hasPath()) return pathOn(pathOf(*c));
      if (C.count(c->id)) return true;
      if (c->junc && c->anchors.size() > 1) { for (int a : c->anchors) if (!AN.count(a)) return false; return true; }
      return false;
    };
    std::vector<const Item*> out; for (const Item* s : surfaces) if (on(s->a) && on(s->b)) out.push_back(s);
    return out;
  }
  std::vector<const Item*> lockWalls(K& k) {
    auto base = lockWallsOwn(k); if (base.empty() || k.shape.c.size() < 3) return base;
    std::set<int> G, have; for (auto* s : base) { G.insert(s->parent); have.insert(s->id); }
    std::vector<const Item*> cand; for (const Item* s : surfaces) if (!have.count(s->id) && G.count(s->parent)) cand.push_back(s);
    if (cand.empty()) return base;
    std::set<int> taken;
    for (const Item* o : kOrder) if (o != k.it && ks[o->id].shape.c.size()) for (auto* s : lockWallsOwn(ks[o->id])) taken.insert(s->id);
    for (const Item* s : cand) {
      if (taken.count(s->id)) continue;
      std::vector<Vec3> pts;
      for (int id : {s->a, s->b}) if (const CurveSampling* m = smp(id)) for (size_t i = 0; i < m->p.size(); i += 4) pts.push_back(m->p[i]);
      int in = 0; for (auto& p : pts) if (inCocoon(k, p, 1.6)) in++;
      if (!pts.empty() && in >= 0.6 * pts.size()) base.push_back(s);
    }
    return base;
  }

  // ---------- guides on a cocoon ----------
  struct GuideOn { double s; Vec3 n; };
  bool guideOn(const Item* g) const {
    if (g->raw.value("off").toBool()) return false;
    for (const Item* p = parentOf(g); p; p = parentOf(p)) if (p->raw.value("guidesOff").toBool()) return false;
    return true;
  }
  const Item* modCocoon(const Item* m) const { const Item* p = parentOf(m); return p && p->type == ItemType::Cocoon ? p : nullptr; }
  std::optional<GuideOn> guideOnCocoon(const Item* g, const CocoonGeo& G) const {
    int N = G.N;
    auto sAt = [&](double f) { int i = std::min(int(std::floor(f)), N - 2); return G.D[i] + (G.D[i + 1] - G.D[i]) * (f - i); };
    auto orient = [&](const Vec3& n, double f) { int i = std::clamp(jsRound(f), 0, N - 1); return n.dot(G.t[i]) < 0 ? -n : n; };
    QJsonObject p = g->raw.value("p").toObject();
    if (g->raw.value("on").isObject()) {                       // (a line drawn across the cocoon)
      QJsonObject on = g->raw.value("on").toObject(); QJsonArray q = on.value("d").toArray();
      double L = G.D[N - 1], s = clamp(on.value("s").toDouble(), 0, 1) * L, f = cocoonF(G, s); int i = std::clamp(jsRound(f), 0, N - 1);
      Vec3 d = G.u[i] * q.at(0).toDouble() + G.v[i] * q.at(1).toDouble() + G.t[i] * q.at(2).toDouble();
      if (d.lengthSq() < 1e-12) return std::nullopt;
      d = d.normalized();
      Vec3 t = G.t[i], n = t - d * t.dot(d); if (n.lengthSq() < 1e-8) return std::nullopt;
      return GuideOn{s, orient(n.normalized(), f)};
    }
    if (g->raw.value("kind").toString() == "disc") {
      std::vector<const Item*> rs;
      for (const QJsonValue& v : g->raw.value("rings").toArray()) if (const Item* c = sc.item(v.toInt())) rs.push_back(c);
      if (rs.size() < 2) return std::nullopt;
      for (auto* c : rs) { const CurveSampling* m = smp(c->id); if (!m || m->p.size() < 4) return std::nullopt; }
      Vec3 C; int n = 0; for (auto* c : rs) for (int a : c->anchors) { C += apos(a); n++; } C = C / double(std::max(n, 1));
      Vec3 Nn; const auto& o = rs[0]->anchors;
      for (size_t i = 0; i < o.size(); i++) { const Vec3 &a = apos(o[i]), &b = apos(o[(i + 1) % o.size()]); Nn.x += (a.y - b.y) * (a.z + b.z); Nn.y += (a.z - b.z) * (a.x + b.x); Nn.z += (a.x - b.x) * (a.y + b.y); }
      if (Nn.lengthSq() < 1e-16) Nn = {0, 1, 0};
      Nn = Nn.normalized();
      double best = -1, bd = INF; bool found = false;
      for (int i = 0; i < N - 1; i++) {
        double d0 = (G.c[i] - C).dot(Nn), d1 = (G.c[i + 1] - C).dot(Nn); if (d0 == d1 || d0 * d1 > 0) continue;
        double f = i + d0 / (d0 - d1); double dd = G.c[i].lerp(G.c[i + 1], f - i).distanceTo(C);
        if (dd < bd) { bd = dd; best = f; found = true; }
      }
      if (!found || bd > 1.5 * std::max(p.value("rOut").toDouble(0), 1e-6)) return std::nullopt;
      return GuideOn{sAt(best), orient(Nn, best)};
    }
    std::vector<Vec3> pts;                                     // (a line on the lock's curves: its hits)
    for (const QJsonValue& v : g->raw.value("hits").toArray()) {
      QJsonObject h = v.toObject(); const CurveSampling* m = smp(h.value("c").toInt()); if (!m) continue;
      const auto& L = m->arc; double d = clamp(h.value("t").toDouble(), 0, 1) * L.back(); size_t i = 0; while (i < L.size() - 2 && L[i + 1] < d) i++;
      double gl = L[i + 1] - L[i]; pts.push_back(m->p[i].lerp(m->p[i + 1], gl > 0 ? clamp((d - L[i]) / gl, 0, 1) : 0));
    }
    if (pts.size() < 2) return std::nullopt;
    Vec3 P; for (auto& q : pts) P += q; P = P / double(pts.size());
    double f = clamp(nearestF(G.c, P), 0, N - 1); int i = std::clamp(jsRound(f), 0, N - 1);
    Vec3 d = (pts.back() - pts.front()).normalized(), t = G.t[i], n = t - d * t.dot(d); if (n.lengthSq() < 1e-8) return std::nullopt;
    return GuideOn{sAt(f), orient(n.normalized(), f)};
  }

  double overlap(const CocoonGeo& G, const CocoonGeo& PG, int i, double h) const {
    int j = std::clamp(jsRound(nearestF(PG.c, G.c[i])), 0, PG.N - 1); Vec3 d = G.c[i] - PG.c[j];
    double x = d.dot(PG.u[j]), y = d.dot(PG.v[j]), L = std::hypot(x, y);
    if (L < 1e-9) return -1;
    double rp = 1 / std::hypot(x / L / std::max(PG.a[j], 1e-6), y / L / std::max(PG.b[j], 1e-6));
    double cx = -d.dot(G.u[i]), cy = -d.dot(G.v[i]), Lc = std::hypot(cx, cy); if (Lc == 0) Lc = 1;
    double rc = 1 / std::hypot(cx / Lc / std::max(G.a[i], 1e-6), cy / Lc / std::max(G.b[i], 1e-6));
    return L - (rp + rc + h);
  }

  // ---------- the rings ----------
  const std::vector<CocoonRing>& ringsOf(K& k) {
    if (!k.ringsOk) { k.rings = foldRings(k, cocoonRings(k)); k.ringsOk = true; }
    return k.rings;
  }
  // the loops a fold forced in: each halfway between the two rings round it, its plane halfway between theirs (taken from
  // a source's loops — still of that stretch: r.run)
  std::vector<CocoonRing> foldRings(K& k, std::vector<CocoonRing> rs) {
    if (k.foldS.empty() || rs.size() < 2) return rs;
    const CocoonGeo& G = geo(k);
    std::stable_sort(rs.begin(), rs.end(), [](const CocoonRing& a, const CocoonRing& b) { return a.s < b.s; });
    for (double s : k.foldS) {
      int j = -1; for (size_t q = 0; q < rs.size(); q++) if (rs[q].s > s) { j = int(q); break; }
      if (j <= 0) continue;
      const CocoonRing A = rs[j - 1], B = rs[j];
      Vec3 nA = A.pn ? *A.pn : A.n, nB0 = B.pn ? *B.pn : B.n, nB = nB0.dot(nA) < 0 ? -nB0 : nB0;
      double f = cocoonF(G, s); int i = std::min(int(std::floor(f)), G.N - 2);
      CocoonRing r; r.s = s; r.p = atF(G.c, f); r.n = G.t[i].lerp(G.t[i + 1], f - i).normalized();
      r.pp = r.p; r.pn = (nA + nB).normalized(); r.fold = true;
      if (!A.run.empty() && A.run == B.run) { r.run = A.run; r.prio = A.prio; r.par = true; }
      rs.insert(rs.begin() + j, r);
    }
    return rs;
  }

  std::vector<CocoonRing> cocoonRings(K& k) {
    const CocoonGeo G = geo(k);
    auto [h, dev0] = conv(k); if (!(h > 0)) h = levelH;
    double dev = clamp(dev0, 0, 1); int N = G.N; double L = G.D[N - 1];
    // its shell modifiers
    struct Mod { const Item* it; std::vector<double> cmask, emask; double dens = 1, str = 1; };
    std::vector<Mod> tips, pinfs, etips; std::vector<const Item*> mguides;
    if (!shellModsOff) for (int id : k.it->children) {
      const Item* m = sc.item(id); if (!m) continue;
      bool isGuide = m->type == ItemType::Guide;
      if (!(m->type == ItemType::Tip || m->type == ItemType::Even || m->type == ItemType::PInf || isGuide)) continue;
      if (isGuide ? m->raw.value("off").toBool() : !m->visible) continue;
      QJsonObject p = m->raw.value("p").toObject();
      Mod x{m, numbers(m->raw.value("cmask")), numbers(m->raw.value("emask")), p.value("dens").toDouble(1.5), p.contains("str") && !p.value("str").isNull() ? p.value("str").toDouble() : 1};
      if (m->type == ItemType::Tip && !x.cmask.empty()) tips.push_back(x);
      if (m->type == ItemType::Tip && !x.emask.empty()) etips.push_back(x);
      if (m->type == ItemType::PInf && !x.cmask.empty()) pinfs.push_back(x);
      if (isGuide) mguides.push_back(m);
    }
    auto pinfAt = [&](double s) { double v = 0; for (auto& m : pinfs) v += m.str * cmaskAt(m.cmask, s / L); return clamp(v, -1, 1); };
    struct Gd { double s; Vec3 n; double R, bl, dens; };
    std::vector<Gd> gds;
    std::vector<const Item*> gl = mguides;
    if (!shellModsOff) for (const Item* g : guides) {
      if (g->raw.value("kind").toString() != "disc" || modCocoon(g) || !guideOn(g)) continue;
      QJsonArray cc = g->raw.value("p").toObject().value("cc").toArray();
      if (!cc.isEmpty()) { bool inL = false; for (const QJsonValue& x : cc) if (x.toObject().value("id").toInt() == k.it->id) inL = true; if (g->raw.value("p").toObject().value("ccOnly").toBool() ? !inL : inL) continue; }
      gl.push_back(g);
    }
    for (const Item* g : gl) {
      auto o = guideOnCocoon(g, G); if (!o) continue;
      QJsonObject p = g->raw.value("p").toObject();
      gds.push_back({o->s, o->n, std::max(p.value("radius").toDouble(0) * headH, 1e-6), p.contains("blend") && !p.value("blend").isNull() ? p.value("blend").toDouble() : 1, p.contains("density") && !p.value("density").isNull() ? p.value("density").toDouble() : 1});
    }
    auto zone = [](const Gd& o, double s) { double d = std::abs(s - o.s); if (d >= o.R) return d < 1e-9 ? 1.0 : 0.0; return clamp((1 - d / o.R) / std::max(o.bl, 1e-3), 0, 1); };
    auto evenE = [&](double s) { double e = 0; for (auto& m : etips) e = std::max(e, cmaskAt(m.emask, s / L)); return clamp(e, 0, 1); };
    std::vector<double> bk(N, 0), bendI(N, 0);
    for (int i = 1; i < N - 1; i++) bk[i] = angleTo(G.t[i - 1], G.t[i + 1]) / std::max(G.D[i + 1] - G.D[i - 1], 1e-9);
    for (int i = 0; i < N; i++) { double s = 0; int n = 0; for (int j = std::max(1, i - 4); j <= std::min(N - 2, i + 4); j++) { s += bk[j]; n++; } bendI[i] = clamp((n ? s / n : 0) * h / (PI / 6), 0, 1); }
    QJsonObject kr = k.it->raw;
    double bendX = (kr.contains("bendD") && !kr.value("bendD").isNull() ? kr.value("bendD").toDouble() : COCOON_BEND) - 1;
    double tipX = (kr.contains("tipD") && !kr.value("tipD").isNull() ? kr.value("tipD").toDouble() : COCOON_TIP) - 1, TIP = std::min(L / 3, 5 * h);
    auto bendAt = [&](double s) { return bendI[std::clamp(jsRound(cocoonF(G, s)), 0, N - 1)]; };
    auto shapeF = [&](double s) {
      double e = evenE(s), b = bendAt(s), x = clamp((s - (L - TIP)) / std::max(TIP, 1e-6), 0, 1);
      return (1 + bendX * b * (1 - e)) * (1 + tipX * x * x * (3 - 2 * x) * (1 - e));
    };
    auto densF = [&](double s) { double f = shapeF(s); for (auto& m : tips) f *= 1 + (m.dens - 1) * cmaskAt(m.cmask, s / L); for (auto& o : gds) f *= 1 + (o.dens - 1) * zone(o, s); return std::max(f, 0.05); };
    auto stepAt = [&](double s) { return h / densF(s) * std::max(1 + dev * (1 - 2 * bendAt(s)) * (1 - evenE(s)), 0.25); };
    auto tanAt = [&](double s) { double f = cocoonF(G, s); int i = std::min(int(std::floor(f)), N - 2); return G.t[i].lerp(G.t[i + 1], f - i).normalized(); };
    auto nrmAt = [&](double s) {
      Vec3 n = tanAt(s);
      for (auto& o : gds) { double w = zone(o, s); if (w > 0) n = n.lerp(o.n.dot(n) < 0 ? -o.n : o.n, w).normalized(); }
      return n;
    };
    auto plane = [&](double s) { CocoonRing r; r.s = s; r.p = atF(G.c, cocoonF(G, s)); r.n = nrmAt(s); return r; };
    auto own = [&](double s0, double s1) {
      const int M = 400; std::vector<double> g{0}; double cnt = 0;
      for (int q = 1; q <= M; q++) { double a = s0 + (s1 - s0) * (q - 1) / M, b = s0 + (s1 - s0) * q / M; g.push_back(g[q - 1] + (b - a) / ((stepAt(a) + stepAt(b)) / 2)); cnt += (b - a) * densF((a + b) / 2) / h; }
      int n = std::max(1, jsRound(cnt)); std::vector<double> out;
      for (int r = 0; r <= n; r++) { double tg = g[M] * r / n; int q = 1; while (q < M && g[q] < tg) q++; double gg = g[q] - g[q - 1]; out.push_back(s0 + (s1 - s0) * (q - 1 + clamp((tg - g[q - 1]) / (gg ? gg : 1), 0, 1)) / M); }
      return out;
    };
    std::vector<double> cuts; for (auto& o : gds) if (o.s > 1e-6 && o.s < L - 1e-6) cuts.push_back(o.s);
    std::sort(cuts.begin(), cuts.end());
    auto ownAll = [&](double s0, double s1) {
      std::vector<double> P{s0}; for (double s : cuts) if (s > s0 + 1e-6 && s < s1 - 1e-6) P.push_back(s); P.push_back(s1);
      std::vector<double> out;
      for (size_t i = 0; i + 1 < P.size(); i++) { auto r = own(P[i], P[i + 1]); if (!out.empty()) r.erase(r.begin()); out.insert(out.end(), r.begin(), r.end()); }
      return out;
    };
    auto planes = [&](const std::vector<double>& ss) { std::vector<CocoonRing> o; for (double s : ss) o.push_back(plane(s)); return o; };

    std::vector<K*> srcs; for (K* d : deps(k)) if (d->shape.c.size() >= 3) srcs.push_back(d);
    if (srcs.empty()) return planes(ownAll(0, L));

    auto parAdj = [](const CocoonRing* a, const CocoonRing* b) { return a && b && a->ri && b->ri && std::abs(*a->ri - *b->ri) == 1 && a->prio.value_or(-1) == b->prio.value_or(-1); };
    auto parRings = [&](int i0, int i1, K& P, const CocoonGeo& PG) {
      std::vector<CocoonRing> rings;
      const auto PRS = ringsOf(P);
      for (int ri = 0; ri < int(PRS.size()); ri++) {
        const CocoonRing& R = PRS[ri];
        int jR = std::clamp(jsRound(cocoonF(PG, R.s)), 0, PG.N - 1); double reachR = 2.5 * std::max(PG.a[jR], PG.b[jR]) + 0.5 * h;
        Vec3 QP = R.pp ? *R.pp : R.p, QN = R.pn ? *R.pn : R.n;
        for (int i = i0; i < std::min(i1, N - 1); i++) {
          double d0 = (G.c[i] - QP).dot(QN), d1 = (G.c[i + 1] - QP).dot(QN); if (d0 == d1 || d0 * d1 > 0) continue;
          double f = i + d0 / (d0 - d1); Vec3 ti = G.t[i].lerp(G.t[i + 1], f - i).normalized();
          if (G.c[i].lerp(G.c[i + 1], f - i).distanceTo(R.p) > reachR) continue;
          if (std::abs(QN.dot(ti)) < 0.5) break;
          CocoonRing r; r.s = G.D[i] + (G.D[i + 1] - G.D[i]) * (f - i); r.p = G.c[i].lerp(G.c[i + 1], f - i); r.n = ti; r.par = true; r.pp = QP; r.pn = QN; r.ri = ri; r.fold = R.fold;
          rings.push_back(r); break;
        }
      }
      std::stable_sort(rings.begin(), rings.end(), [](auto& x, auto& y) { return x.s < y.s; });
      for (int r = int(rings.size()) - 1; r > 0; r--) if (rings[r].s - rings[r - 1].s < (parAdj(&rings[r - 1], &rings[r]) ? 0.2 : 0.5) * h) rings.erase(rings.begin() + r);
      for (int r = int(rings.size()) - 1; r > 0; r--) {
        CocoonRing A = rings[r - 1], B = rings[r]; double gp = B.s - A.s, st = h; int m = jsRound(gp / st) - 1;
        if (gp < 1.5 * st || m < 1) continue;
        std::vector<CocoonRing> add; for (int q = 1; q <= m; q++) { auto x = plane(A.s + gp * q / (m + 1)); x.par = true; add.push_back(x); }
        rings.insert(rings.begin() + r, add.begin(), add.end());
      }
      return rings;
    };

    std::vector<CocoonRing> PR; int iOut = 0;
    struct RootIn { K* P; int prio; };
    struct SrcA { K* P; std::vector<double> A; std::vector<CocoonRing> X; };
    std::vector<RootIn> rootIn; std::vector<SrcA> srcA;
    for (int prio = 0; prio < int(srcs.size()); prio++) {
      K& P = *srcs[prio]; const CocoonGeo PG = geo(P);
      std::function<bool(int)> inside = [&](int i) { return overlap(G, PG, i, h) <= 0; };
      std::vector<double> Aw;
      if (!pinfs.empty()) {
        Aw.assign(N, 0);
        for (int i = 0; i < N; i++) { double ov = overlap(G, PG, i, h), pm = pinfAt(G.D[i]); Aw[i] = ov <= 0 ? clamp(1 + pm, 0, 1) : clamp(2 * pm * std::max(0.0, 1 - ov / (4 * h)), 0, 1); }
        std::vector<CocoonRing> X;
        for (int i = 0; i < N;) { if (Aw[i] <= 0.001) { i++; continue; } int j = i; while (j < N && Aw[j] > 0.001) j++; for (auto& r : parRings(i, j, P, PG)) if (r.pp) X.push_back(r); i = j; }
        srcA.push_back({&P, Aw, X});
        inside = [Aw](int i) { return Aw[i] >= 0.999; };
      }
      if (inside(0)) rootIn.push_back({&P, prio});
      for (int i = 0; i < N;) {
        if (!inside(i)) { i++; continue; } int j = i; while (j < N && inside(j)) j++;
        if (!i || G.D[std::min(j, N - 1)] - G.D[i] >= h) {
          auto rs = parRings(i, j, P, PG);
          if (!rs.empty()) { for (auto& r : rs) { r.prio = prio; r.run = std::to_string(prio) + ":" + std::to_string(i); } PR.insert(PR.end(), rs.begin(), rs.end()); iOut = std::max(iOut, j); }
        }
        i = j;
      }
    }
    auto isCut = [&](double s) { for (double c : cuts) if (std::abs(c - s) < 1e-6) return true; return false; };
    auto pinfStrength = [&](int i) { return std::abs(pinfAt(G.D[i])) > 0.002 ? 1.0 : 0.0; };
    auto pinfSnap = [&](std::vector<CocoonRing> rs) {
      if (srcA.empty()) return rs;
      std::vector<CocoonRing> out;
      for (auto& r : rs) {
        if (!r.run.empty() || r.pp) { out.push_back(r); continue; }
        int i = std::clamp(jsRound(cocoonF(G, r.s)), 0, N - 1); double a = 0; SrcA* o = nullptr;
        for (auto& q : srcA) if (q.A[i] > a) { a = q.A[i]; o = &q; }
        a = std::min(a, pinfStrength(i));
        bool onGuide = isCut(r.s) || r.s <= 1e-6 || r.s >= L - 1e-6;
        if (a <= 0.001 || !o || onGuide) { out.push_back(r); continue; }
        const CocoonRing* x = nullptr; double bd = h; for (auto& X : o->X) { double d = std::abs(X.s - r.s); if (d < bd) { bd = d; x = &X; } }
        if (!x) { out.push_back(r); continue; }
        auto n = plane(r.s + (x->s - r.s) * a); n.snap = CocoonRing::Snap{*x->pp, *x->pn, a}; out.push_back(n);
      }
      std::stable_sort(out.begin(), out.end(), [](auto& p, auto& q) { return p.s < q.s; });
      auto special = [&](const CocoonRing& r) { return !r.run.empty() || r.pp || isCut(r.s); };
      auto keepFirst = [&](const CocoonRing& p, const CocoonRing& q) { return special(q) && !special(p) ? false : true; };
      std::vector<CocoonRing> acc;
      for (auto& r : out) { if (!acc.empty() && r.s - acc.back().s < 0.3 * h) { if (!keepFirst(acc.back(), r)) acc.back() = r; } else acc.push_back(r); }
      return acc;
    };
    auto pinfBlend = [&](std::vector<CocoonRing> rs) {
      if (srcA.empty()) return rs;
      for (auto& r : rs) {
        int i = std::clamp(jsRound(cocoonF(G, r.s)), 0, N - 1); double a = 0; K* P = nullptr;
        for (auto& o : srcA) if (o.A[i] > a) { a = o.A[i]; P = o.P; }
        Vec3 QN;
        if (r.snap) { a = r.snap->a; QN = r.snap->pn; }
        else if (r.pp && r.pn) continue;
        else if (r.s <= 1e-6 || r.s >= L - 1e-6) continue;
        else {
          a = std::min(a, pinfStrength(i));
          if (a <= 0.001 || !P) continue;
          const CocoonRing* best = nullptr; double bd = INF;
          for (auto& R : ringsOf(*P)) { double d = (R.p - r.p).lengthSq(); if (d < bd) { bd = d; best = &R; } }
          if (!best) continue; QN = best->pn ? *best->pn : best->n;
        }
        Vec3 q = QN.dot(r.n) < 0 ? -QN : QN, n = r.n * (1 - a) + q * a;
        if (n.lengthSq() < 1e-12) continue;
        r.pp = r.p; r.pn = n.normalized();
      }
      return rs;
    };
    auto pinfFinal = [&](std::vector<CocoonRing> rs) { return pinfBlend(pinfSnap(std::move(rs))); };
    if (PR.empty()) return pinfFinal(planes(ownAll(0, L)));

    std::stable_sort(PR.begin(), PR.end(), [](auto& x, auto& y) { return x.s < y.s; });
    { std::vector<CocoonRing> o;
      for (auto& r : PR) { if (!o.empty() && r.s - o.back().s < (parAdj(&o.back(), &r) ? 0.2 : 0.5) * h) { if (*r.prio < *o.back().prio) o.back() = r; } else o.push_back(r); }
      PR = o; }
    std::vector<CocoonRing> rings;
    auto rootRing = [&]() -> std::optional<CocoonRing> {
      Vec3 t0 = (PR[0].p - G.c[0]).normalized();
      double lim0 = 2.5 * std::max({G.a[0], G.b[0], 1e-3}) + 0.5 * h;
      std::vector<const std::vector<Vec3>*> lockQ; std::set<int> seenC;
      for (auto* sf : lockWalls(k)) for (int id : {sf->a, sf->b}) if (seenC.insert(id).second) if (const CurveSampling* m = smp(id)) if (m->p.size() > 1) lockQ.push_back(&m->p);
      auto cuts2 = [&](const Vec3& QP, const Vec3& QN) {
        int n = 0;
        for (auto* Q : lockQ) for (size_t j = 0; j + 1 < Q->size(); j++) {
          double d0 = ((*Q)[j] - QP).dot(QN), d1 = ((*Q)[j + 1] - QP).dot(QN); if (d0 == d1 || d0 * d1 > 0) continue;
          if ((*Q)[j].lerp((*Q)[j + 1], d0 / (d0 - d1)).distanceTo(G.c[0]) <= lim0) { n++; break; }
        }
        return n >= 2;
      };
      struct Best { double sx; int prio; Vec3 QP, QN; }; std::optional<Best> best;
      for (auto& ri : rootIn) {
        const CocoonGeo PG = geo(*ri.P);
        for (auto& R : ringsOf(*ri.P)) {
          Vec3 QP = R.pp ? *R.pp : R.p, QN = R.pn ? *R.pn : R.n; double c = QN.dot(t0);
          if (std::abs(c) < 0.35) continue;
          double sx = -(G.c[0] - QP).dot(QN) / c; if (sx > 0 || sx < -0.75 * h) continue;
          int jR = std::clamp(jsRound(cocoonF(PG, R.s)), 0, PG.N - 1);
          if ((G.c[0] + t0 * sx).distanceTo(R.p) > 2.5 * std::max(PG.a[jR], PG.b[jR]) + 0.5 * h) continue;
          if (best && sx < best->sx) continue;
          if (!cuts2(QP, QN)) continue;
          if (!best || sx > best->sx || (sx == best->sx && ri.prio < best->prio)) best = Best{sx, ri.prio, QP, QN};
        }
      }
      if (!best) return std::nullopt;
      auto r = plane(0); r.par = true; r.pp = best->QP; r.pn = best->QN; r.prio = best->prio; return r;
    };
    if (PR[0].s > 0.5 * h) {
      std::vector<double> ss;
      if (PR[0].s >= 1.5 * h) { ss = ownAll(0, PR[0].s); ss.pop_back(); } else ss = {0};
      for (double s : ss) { std::optional<CocoonRing> rr; if (s == 0) rr = rootRing(); rings.push_back(rr ? *rr : plane(s)); }
    }
    for (auto& r : PR) {
      if (!rings.empty()) {
        const CocoonRing& lr = rings.back(); double a = lr.s, b = r.s; bool fromRoot = rings.size() == 1 && a == 0, sameRun = !lr.run.empty() && lr.run == r.run;
        if (!sameRun && b - a > 1.5 * (fromRoot ? h : stepAt((a + b) / 2))) { auto ss = ownAll(a, b); for (size_t q = 1; q + 1 < ss.size(); q++) rings.push_back(plane(ss[q])); }
      }
      if (rings.empty() || r.s - rings.back().s >= (parAdj(&rings.back(), &r) ? 0.2 : 0.5) * h) rings.push_back(r);
    }
    if (iOut >= N && L - rings.back().s < 1.5 * h) return pinfFinal(rings);
    CocoonRing last = rings.back(); double sOut = iOut >= N ? last.s : G.D[iOut], F = COCOON_FADE * h;
    double gap = rings.size() > 1 ? last.s - rings[rings.size() - 2].s : stepAt(last.s), s = last.s;
    auto sm = [](double x) { x = clamp(x, 0, 1); return x * x * (3 - 2 * x); };
    auto paintF = [&](double x) { double f = 1; for (auto& m : tips) f *= 1 + (m.dens - 1) * cmaskAt(m.cmask, x / L); for (auto& o : gds) f *= 1 + (o.dens - 1) * zone(o, x); return std::max(f, 0.05); };
    for (int guard = 0; guard < 10000; guard++) {
      double w = sm((s - sOut) / F), g0 = gap / paintF(s), st = g0 + (stepAt(s) - g0) * w;
      if (L - s < 1.5 * st) { rings.push_back(plane(L)); break; }
      s += st; rings.push_back(plane(s));
    }
    if (!etips.empty()) {
      int j = int(rings.size()) - 1;
      while (j > 0 && rings[j - 1].run.empty() && !rings[j - 1].pp && evenE(rings[j - 1].s) >= 0.99) j--;
      if (int(rings.size()) - 1 - j >= 2 && evenE(rings[j].s) >= 0.99) {
        double s0 = rings[j].s; int n = std::max(1, jsRound((L - s0) / stepAt((s0 + L) / 2)));
        rings.resize(j); for (int q = 0; q <= n; q++) rings.push_back(plane(s0 + (L - s0) * q / n));
      }
    }
    return pinfFinal(rings);
  }

  // a forked lock's side branch running on a little past the cocoon's tip: the cocoon's rings go on along it
  std::vector<CocoonRing> tailRings(K& k, const CocoonGeo& G, const std::vector<CocoonRing>& rings) {
    if (!k.live || rings.size() < 2) return {};
    std::vector<int> ids = k.lock;
    if (!k.hasLock) { ids.clear(); if (const Item* g = lockGroup(k)) for (const Item* c : curves) if (sc.isDescendant(c->id, g->id)) ids.push_back(c->id); }
    std::vector<const Item*> cs;
    for (int id : ids) { const Item* c = sc.item(id); const CurveSampling* m = smp(id); if (c && c->type == ItemType::Curve && !c->junc && !c->hasPath() && c->ring < 0 && c->anchors.size() >= 2 && m && m->p.size() >= 2) cs.push_back(c); }
    if (cs.size() < 3) return {};
    Ends all = lockEnds(cs);
    std::set<std::string> main; for (auto& e : k.ends) main.insert(joinIds(e));
    std::vector<std::vector<int>> side; for (auto& e : all.all) if (!main.count(joinIds(e))) side.push_back(e);
    if (side.empty()) return {};
    auto polys = livePolys(side, {}, false).polys;
    auto [h0, dev] = conv(k); double h = h0 > 0 ? h0 : levelH; (void)dev;
    std::vector<CocoonRing> out; std::set<std::string> tips;
    for (auto& Q : polys) {
      char tk[128]; std::snprintf(tk, sizeof tk, "%.4f,%.4f,%.4f", Q.back().x, Q.back().y, Q.back().z);
      if (!tips.insert(tk).second) continue;
      std::vector<double> A{0}; for (size_t j = 1; j < Q.size(); j++) A.push_back(A.back() + Q[j].distanceTo(Q[j - 1]));
      struct X { double s, reach; }; std::vector<X> xs;
      for (auto& R : rings) {
        int i = std::clamp(jsRound(cocoonF(G, R.s)), 0, G.N - 1); double reach = R.lim ? *R.lim : 2.5 * std::max({G.a[i], G.b[i], 1e-3});
        Vec3 PP = R.pp ? *R.pp : R.p, PN = R.pn ? *R.pn : R.n;
        for (size_t j = 0; j + 1 < Q.size(); j++) {
          double d0 = (Q[j] - PP).dot(PN), d1 = (Q[j + 1] - PP).dot(PN); if (d0 == d1 || d0 * d1 > 0) continue;
          double f = d0 / (d0 - d1); if (Q[j].lerp(Q[j + 1], f).distanceTo(R.p) > reach) continue;
          xs.push_back({A[j] + (A[j + 1] - A[j]) * f, reach}); break;
        }
      }
      if (xs.size() < 2) continue;
      std::stable_sort(xs.begin(), xs.end(), [](auto& x, auto& y) { return x.s < y.s; });
      std::vector<double> gs; size_t from = xs.size() > 6 ? xs.size() - 6 : 0;
      for (size_t i = from + 1; i < xs.size(); i++) { double g = xs[i].s - xs[i - 1].s; if (g > 1e-6) gs.push_back(g); }
      std::sort(gs.begin(), gs.end());
      X lastX = xs.back(); double gap = gs.empty() ? 0 : gs[gs.size() >> 1], rem = A.back() - lastX.s;
      if (!(gap > 1e-6) || rem < 1.5 * gap || rem > TAIL_MAX * h) continue;
      int m = jsRound(rem / gap);
      for (int q = 1; q < m; q++) {
        double s = lastX.s + rem * q / m; size_t j = 1; while (j < A.size() - 1 && A[j] < s) j++;
        double gl = A[j] - A[j - 1]; Vec3 p = Q[j - 1].lerp(Q[j], clamp((s - A[j - 1]) / (gl ? gl : 1), 0, 1)), n = (Q[j] - Q[j - 1]).normalized();
        CocoonRing r; r.s = G.D[G.N - 1]; r.p = p; r.n = n; r.lim = std::max(lastX.reach, h); r.tail = true; r.gap = gap;
        out.push_back(r);
      }
    }
    return out;
  }

  // the rings projected onto the lock: where each ring's plane cuts the walls' triangles, near the ring
  std::vector<Vec3> lockRingSegs(K& k, const CocoonGeo& G, const std::vector<CocoonRing>& rings) {
    struct Mesh { std::vector<Vec3> P; std::vector<uint32_t> I; Vec3 bc; double br; };
    std::vector<Mesh> meshes;
    for (const Item* sf : lockWalls(k)) {
      auto it = wallOf.find(sf->id); if (it == wallOf.end()) continue;
      if (!sf->visible) continue;
      const WallMesh& w = it->second; Mesh m;
      for (auto& p : w.pos) m.P.push_back({double(float(p.x)), double(float(p.y)), double(float(p.z))});   // (as the GPU positions: floats)
      std::vector<std::vector<int>> faces; for (auto& q : w.quads) faces.push_back({q[0], q[1], q[2], q[3]});
      m.I = polyFlatIndex(faces, int(w.pos.size()));
      Box3 b; for (auto& p : m.P) b.expand(p); m.bc = b.center(); double r2 = 0; for (auto& p : m.P) r2 = std::max(r2, (p - m.bc).lengthSq()); m.br = std::sqrt(r2);
      meshes.push_back(std::move(m));
    }
    std::vector<Vec3> seg;
    std::map<const Mesh*, std::vector<const CocoonRing*>> tailOn;
    auto nearTail = [&](const Mesh* g, const Vec3& m) {
      auto it = tailOn.find(g); if (it == tailOn.end()) return false;
      for (auto* T : it->second) if (std::abs((m - T->p).dot(T->n)) < 0.5 * *T->gap && m.distanceTo(T->p) < *T->lim) return true;
      return false;
    };
    std::vector<const CocoonRing*> order;
    for (auto& r : rings) if (r.tail) order.push_back(&r);
    for (auto& r : rings) if (!r.tail) order.push_back(&r);
    for (const CocoonRing* R : order) {
      int i = std::clamp(jsRound(cocoonF(G, R->s)), 0, G.N - 1); double lim = R->lim ? *R->lim : 2.5 * std::max({G.a[i], G.b[i], 1e-3}), lim2 = lim * lim;
      Vec3 PP = R->pp ? *R->pp : R->p, PN = R->pn ? *R->pn : R->n;
      for (const Mesh& g : meshes) {
        if (g.bc.distanceTo(R->p) > lim + g.br) continue;
        size_t n0 = seg.size();
        for (size_t t = 0; t + 2 < g.I.size(); t += 3) {
          const Vec3 &a = g.P[g.I[t]], &b = g.P[g.I[t + 1]], &c = g.P[g.I[t + 2]];
          double da = (a - PP).dot(PN), db = (b - PP).dot(PN), dc = (c - PP).dot(PN);
          Vec3 o[3]; int no = 0;
          const Vec3* pq[3][2] = {{&a, &b}, {&b, &c}, {&c, &a}}; double dd[3][2] = {{da, db}, {db, dc}, {dc, da}};
          for (int e = 0; e < 3; e++) if ((dd[e][0] < 0) != (dd[e][1] < 0)) o[no++] = pq[e][0]->lerp(*pq[e][1], dd[e][0] / (dd[e][0] - dd[e][1]));
          if (no != 2) continue;
          Vec3 mid = (o[0] + o[1]) * 0.5;
          if ((mid - R->p).lengthSq() > lim2 || (!R->tail && nearTail(&g, mid))) continue;
          seg.push_back(o[0]); seg.push_back(o[1]);
        }
        if (R->tail && seg.size() > n0) tailOn[&g].push_back(R);
      }
    }
    return seg;
  }

public:
  // ---------- how a guide is shown ----------
  // cocoonRingPts: a ring (place p, plane n) round the cocoon's shell, n points
  std::vector<Vec3> ringPts(const CocoonGeo& G, double s, const Vec3& P, const Vec3& n, int count) const {
    int i0 = std::clamp(jsRound(cocoonF(G, s)), 0, G.N - 1); std::vector<Vec3> out;
    for (int j = 0; j < count; j++) {
      double th = j * 2 * PI / count; std::optional<Vec3> best;
      for (int d = 0; d < 30 && !best; d++) for (int i : {i0 + d, i0 - d - 1}) {
        if (i < 0 || i >= G.N - 1) continue;
        Vec3 a = cocoonPt(G, i, th), b = cocoonPt(G, i + 1, th); double d0 = (a - P).dot(n), d1 = (b - P).dot(n);
        if (d0 == d1 || d0 * d1 > 0) continue; best = a.lerp(b, d0 / (d0 - d1)); break;
      }
      out.push_back(best ? *best : cocoonPt(G, i0, th));
    }
    return out;
  }
  // a line drawn across a cocoon — the ring it makes, a little outside the shell, closed (cocoonGuideLine); an older
  // line on the lock's curves — through its hits
  std::vector<Vec3> guideLineOf(int gid) {
    const Item* g = sc.item(gid); if (!g || g->raw.value("kind").toString() == "disc") return {};
    if (g->raw.value("on").isObject()) {
      const Item* k = modCocoon(g); if (!k) return {};
      CocoonGeo G = geoOf(k->id); if (G.N < 3) return {};
      auto on = guideOnCocoon(g, G); if (!on) return {};
      double f = cocoonF(G, clamp(g->raw.value("on").toObject().value("s").toDouble(), 0, 1) * G.D[G.N - 1]);
      Vec3 P = atF(G.c, f); auto pts = ringPts(G, on->s, P, on->n, 64);
      for (auto& q : pts) q = P + (q - P) * 1.08;
      if (!pts.empty()) pts.push_back(pts[0]);
      return pts;
    }
    std::vector<Vec3> pts;
    for (const QJsonValue& v : g->raw.value("hits").toArray()) {
      QJsonObject h = v.toObject(); const CurveSampling* m = smp(h.value("c").toInt()); if (!m || m->arc.size() < 2) continue;
      const auto& L = m->arc; double d = clamp(h.value("t").toDouble(), 0, 1) * L.back(); size_t i = 0; while (i < L.size() - 2 && L[i + 1] < d) i++;
      double gl = L[i + 1] - L[i]; pts.push_back(m->p[i].lerp(m->p[i + 1], gl > 0 ? clamp((d - L[i]) / gl, 0, 1) : 0));
    }
    return pts;
  }

  // the influence zone of a guide (updateZoneViz): on its cocoon(s) — the stretch of the axis within the radius and rings
  // round the shell at its edges and where Blend starts; an older line on the lock's curves — along them. w — the weight
  // (1 full … 0 none) at each end
  std::vector<CocoonSystem::ZoneLine> zoneOf(int gid, double rad, double blend) {
    std::vector<CocoonSystem::ZoneLine> out; const Item* g = sc.item(gid); if (!g || rad <= 0) return out;
    auto zw = [&](double x) { x = std::min(x, rad * 0.995); if (x >= rad) return 0.0; double x0 = rad * (1 - clamp(blend, 0, 1)); if (x <= x0) return 1.0; double t = (x - x0) / std::max(rad - x0, 1e-9); return 1 - t * t * (3 - 2 * t); };
    std::vector<int> ks;
    if (const Item* k = modCocoon(g)) ks.push_back(k->id);
    else if (g->raw.value("kind").toString() == "disc") {             // (a scene disc: every cocoon it crosses — its list)
      QJsonObject p = g->raw.value("p").toObject(); QJsonArray L = p.value("cc").toArray();
      for (auto& [id, it] : sc.items) if (it.type == ItemType::Cocoon) {
        bool in = false; for (const QJsonValue& x : L) if (x.toObject().value("id").toInt() == id) in = true;
        if (L.isEmpty() || (p.value("ccOnly").toBool() ? in : !in)) ks.push_back(id);
      }
    }
    if (!ks.empty()) {
      for (int kid : ks) {
        CocoonGeo G = geoOf(kid); if (G.N < 3) continue; auto o = guideOnCocoon(g, G); if (!o) continue;
        for (int i = 0; i < G.N - 1; i++) { double x0 = std::abs(G.D[i] - o->s), x1 = std::abs(G.D[i + 1] - o->s); if (std::min(x0, x1) > rad) continue; out.push_back({G.c[i], G.c[i + 1], zw(x0), zw(x1)}); }
        double L = G.D[G.N - 1], full = rad * (1 - clamp(blend, 0, 1));
        for (auto [x, w] : {std::pair<double, double>{rad, zw(rad)}, {full, zw(0)}}) for (int sg : {-1, 1}) {
          double s = o->s + sg * x; if (x <= 0 || s < 0 || s > L) continue;
          double f = cocoonF(G, s); int i = std::clamp(jsRound(f), 0, G.N - 1); Vec3 P = atF(G.c, f);
          auto R = ringPts(G, s, P, G.t[i], 48); for (auto& q : R) q = P + (q - P) * 1.05;
          for (size_t j = 0; j < R.size(); j++) out.push_back({R[j], R[(j + 1) % R.size()], w, w});
        }
      }
      return out;
    }
    std::vector<Vec3> lo, hi; const int N = 48;
    for (const QJsonValue& v : g->raw.value("hits").toArray()) {
      QJsonObject h = v.toObject(); const CurveSampling* m = smp(h.value("c").toInt()); if (!m || m->arc.size() < 2) continue;
      double tot = m->arc.back() ? m->arc.back() : 1, ht = h.value("t").toDouble(), t0 = clamp(ht - rad / tot, 0, 1), t1 = clamp(ht + rad / tot, 0, 1);
      auto pt = [&](double t) { const auto& L = m->arc; double d = clamp(t, 0, 1) * L.back(); size_t i = 0; while (i < L.size() - 2 && L[i + 1] < d) i++; double gl = L[i + 1] - L[i]; return m->p[i].lerp(m->p[i + 1], gl > 0 ? clamp((d - L[i]) / gl, 0, 1) : 0); };
      Vec3 prev; double pw = 0;
      for (int q = 0; q <= N; q++) { double t = t0 + (t1 - t0) * q / N; Vec3 pp = pt(t); double w = zw(std::abs(t - ht) * tot); if (q) out.push_back({prev, pp, pw, w}); prev = pp; pw = w; }
      lo.push_back(pt(t0)); hi.push_back(pt(t1));
    }
    bool disc = g->raw.value("kind").toString() == "disc"; double w0 = zw(rad);
    for (auto* side : {&lo, &hi}) for (size_t i = 0; i + 1 < side->size() + (disc ? 1 : 0); i++) out.push_back({(*side)[i], (*side)[(i + 1) % side->size()], w0, w0});
    return out;
  }
};

}  // namespace

std::vector<CocoonResult> buildCocoons(const Scene& sc, const CurveCache& cc, const std::vector<WallMesh>& walls) {
  std::map<int, WallMesh> wm; for (auto& w : walls) wm[w.surfaceId] = w;
  CocoonSystem cs(sc, cc, wm); cs.rebuildAll();
  std::vector<CocoonResult> out; for (auto& [id, r] : cs.results()) out.push_back(r);
  return out;
}

struct CocoonSystem::Impl : Ctx { using Ctx::Ctx; };
CocoonSystem::CocoonSystem(const Scene& sc, const CurveCache& cc, const std::map<int, WallMesh>& walls) : d(std::make_unique<Impl>(sc, cc, walls)) {}
CocoonSystem::~CocoonSystem() = default;
void CocoonSystem::rebuildAll() { d->rebuildAll(); }
std::set<int> CocoonSystem::update(const std::set<int>& changedCurves) { return d->update(changedCurves); }
void CocoonSystem::rebuild(const std::set<int>& ids) { d->rebuild(ids); }
const std::map<int, CocoonResult>& CocoonSystem::results() const { return d->results; }
bool CocoonSystem::liveState(int id, QJsonObject& shape, QJsonArray& ends) const { return d->liveState(id, shape, ends); }
bool CocoonSystem::lockInput(int id, LockInput& out) { return d->lockInput(id, out); }
std::vector<int> CocoonSystem::deps(int id) { return d->depIds(id); }
bool CocoonSystem::dependsOn(int a, int b) { return d->dependsOn(a, b); }
int CocoonSystem::lockGroup(int id) { return d->lockGroupId(id); }
double CocoonSystem::loopStep(int id) { return d->loopStepOf(id); }
CocoonGeo CocoonSystem::geo(int id) { return d->geoOf(id); }
std::vector<int> CocoonSystem::strandCurves(int id) { return d->strandsOf(id); }
bool CocoonSystem::trialShape(std::vector<std::vector<int>>& ends, const std::vector<int>& lock, int* host, QJsonObject* shape) { return d->trialShape(ends, lock, host, shape); }
void CocoonSystem::setFolds(int id, const std::vector<double>& s) { d->setFolds(id, s); }
const std::vector<double>& CocoonSystem::folds(int id) { return d->foldsOf(id); }
std::vector<Vec3> CocoonSystem::guideLine(int gid) { return d->guideLineOf(gid); }
std::vector<CocoonSystem::ZoneLine> CocoonSystem::zone(int gid, double rad, double blend) { return d->zoneOf(gid, rad, blend); }
double cocoonStation(const CocoonGeo& G, double s) { return cocoonF(G, s); }
Vec3 cocoonPoint(const CocoonGeo& G, int i, double th) { return cocoonPt(G, i, th); }

}  // namespace hm
