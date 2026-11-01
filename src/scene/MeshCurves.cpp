#include "scene/MeshCurves.h"
#include "scene/MeshMods.h"
#include "scene/CurveMath.h"
#include "scene/Edit.h"
#include "scene/Walls.h"
#include <QObject>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace hm::meshc {

namespace {
const double PI = 3.14159265358979323846;
double jsRound(double x) { return std::floor(x + 0.5); }
double angleTo(const Vec3& a, const Vec3& b) { double d = std::sqrt(a.lengthSq() * b.lengthSq()); return d == 0 ? PI / 2 : std::acos(clamp(a.dot(b) / d, -1, 1)); }
double segDist3(const Vec3& p, const Vec3& a, const Vec3& b) { Vec3 ab = b - a; double l2 = ab.lengthSq(), t = l2 > 0 ? clamp((p - a).dot(ab) / l2, 0, 1) : 0; return p.distanceTo(a + ab * t); }
template <class T> bool has(const std::vector<T>& v, const T& x) { return std::find(v.begin(), v.end(), x) != v.end(); }
// an ordered set (a JS Set: iteration in insertion order)
struct OSet { std::vector<int> v; std::set<int> s; bool add(int x) { if (!s.insert(x).second) return false; v.push_back(x); return true; } bool has(int x) const { return s.count(x) > 0; } size_t size() const { return v.size(); } };
}  // namespace

// ---------- topology ----------
std::shared_ptr<const Topo> topoOf(const PolyMesh& m) {
  static std::mutex mu; static std::map<const PolyMesh*, std::pair<size_t, std::shared_ptr<const Topo>>> cache;
  std::lock_guard<std::mutex> lk(mu);
  auto it = cache.find(&m); size_t sig = m.pos.size() * 1000003 + m.faceVerts.size();
  if (it != cache.end() && it->second.first == sig) return it->second.second;
  auto T = std::make_shared<Topo>(); T->n = int(m.pos.size()); T->nf = m.faceCount();
  auto addE = [&](int a, int b) { if (a == b) return; long long k = T->key(a, b); if (!T->E.count(k)) { T->E[k] = int(T->ea.size()); T->ea.push_back(a); T->eb.push_back(b); } };
  for (int f = 0; f < T->nf; f++) { int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) addE(m.faceVerts[size_t(k)], m.faceVerts[size_t(k + 1 < e ? k + 1 : s)]); }
  T->ne = int(T->ea.size()); T->eFaces.assign(size_t(T->ne), {}); T->vE.assign(size_t(T->n), {}); T->fEdges.assign(size_t(T->nf), {});
  for (int f = 0; f < T->nf; f++) { int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
    for (int k = s; k < e; k++) { int x = T->edge(m.faceVerts[size_t(k)], m.faceVerts[size_t(k + 1 < e ? k + 1 : s)]); if (x < 0) continue; if (!has(T->eFaces[size_t(x)], f)) { T->eFaces[size_t(x)].push_back(f); T->fEdges[size_t(f)].push_back(x); } } }
  for (int e = 0; e < T->ne; e++) { T->vE[size_t(T->ea[size_t(e)])].push_back(e); T->vE[size_t(T->eb[size_t(e)])].push_back(e); }
  cache[&m] = {sig, T};
  return T;
}

// the next edge of an edge loop at vertex v: across a vertex with 4 edges (inside) or along the border through a vertex
// with 3 edges; anything else (a pole, a fork, a corner) ends the loop
int loopNext(const Topo& T, int e, int v) {
  const auto& es = T.vE[size_t(v)]; const auto& ef = T.eFaces[size_t(e)];
  bool inside = es.size() == 4 && std::all_of(es.begin(), es.end(), [&](int x) { return T.eFaces[size_t(x)].size() == 2; });
  int nb = 0; for (int x : es) if (T.eFaces[size_t(x)].size() == 1) nb++;
  bool border = es.size() == 3 && ef.size() == 1 && nb == 2;
  if (!inside && !border) return -1;
  std::vector<int> c;
  for (int x : es) { if (x == e) continue; bool sh = false; for (int f : T.eFaces[size_t(x)]) if (has(ef, f)) sh = true; if (!sh) c.push_back(x); }
  return c.size() == 1 ? c[0] : -1;
}

Loop edgeLoop(const Topo& T, int e0) {
  std::vector<int> out{e0}; std::set<int> seen{e0}; bool closed = false;
  for (int dir : {0, 1}) {
    int e = e0, v = dir ? T.eb[size_t(e0)] : T.ea[size_t(e0)];
    for (;;) {
      int nx = loopNext(T, e, v); if (nx < 0) break;
      if (seen.count(nx)) { closed = true; break; }
      seen.insert(nx); if (dir) out.push_back(nx); else out.insert(out.begin(), nx);
      v = T.other(nx, v); e = nx;
    }
    if (closed) break;
  }
  int v = out.size() == 1 ? T.ea[size_t(out[0])] : (T.ea[size_t(out[0])] == T.ea[size_t(out[1])] || T.ea[size_t(out[0])] == T.eb[size_t(out[1])] ? T.eb[size_t(out[0])] : T.ea[size_t(out[0])]);
  Loop L; L.edges = out; L.verts = {v};
  for (int e : out) { v = T.other(e, v); L.verts.push_back(v); }
  if (closed) L.verts.pop_back();
  L.closed = closed;
  return L;
}

std::vector<int> edgePath(const Topo& T, const std::vector<Vec3>& W, int from, int to) {
  std::vector<double> dist(size_t(T.n), 1e300); std::vector<int> prevE(size_t(T.n), -1);
  std::vector<std::pair<double, int>> heap;
  auto push = [&](double d, int v) { heap.push_back({d, v}); size_t i = heap.size() - 1; while (i > 0) { size_t p = (i - 1) >> 1; if (heap[p].first <= heap[i].first) break; std::swap(heap[p], heap[i]); i = p; } };
  auto pop = [&] { auto top = heap[0]; auto last = heap.back(); heap.pop_back();
    if (!heap.empty()) { heap[0] = last; size_t i = 0; for (;;) { size_t l = 2 * i + 1, r = l + 1, k = i; if (l < heap.size() && heap[l].first < heap[k].first) k = l; if (r < heap.size() && heap[r].first < heap[k].first) k = r; if (k == i) break; std::swap(heap[k], heap[i]); i = k; } }
    return top; };
  for (int v : {T.ea[size_t(from)], T.eb[size_t(from)]}) { dist[size_t(v)] = 0; push(0, v); }
  int g0 = T.ea[size_t(to)], g1 = T.eb[size_t(to)], end = -1;
  while (!heap.empty()) {
    auto [d, v] = pop(); if (d > dist[size_t(v)]) continue;
    if (v == g0 || v == g1) { end = v; break; }
    for (int e : T.vE[size_t(v)]) { int u = T.other(e, v); double nd = d + W[size_t(v)].distanceTo(W[size_t(u)]); if (nd < dist[size_t(u)]) { dist[size_t(u)] = nd; prevE[size_t(u)] = e; push(nd, u); } }
  }
  if (end < 0) return {};
  std::vector<int> out{to}; for (int v = end; prevE[size_t(v)] >= 0;) { int e = prevE[size_t(v)]; out.push_back(e); v = T.other(e, v); }
  return out;
}

std::vector<Chain> edgeChains(const Topo& T, const std::vector<int>& sel) {
  std::set<int> set(sel.begin(), sel.end());
  std::vector<int> dv; std::map<int, int> deg;
  for (int e : sel) for (int v : {T.ea[size_t(e)], T.eb[size_t(e)]}) { if (!deg.count(v)) dv.push_back(v); deg[v]++; }
  std::set<int> used; std::vector<Chain> out;
  auto walk = [&](int v0, int e0) {
    std::vector<int> vs{v0}; int v = v0, e = e0;
    for (;;) {
      used.insert(e); v = T.other(e, v); vs.push_back(v);
      if (deg[v] != 2) break;
      int nx = -1; for (int x : T.vE[size_t(v)]) if (set.count(x) && !used.count(x)) { nx = x; break; }
      if (nx < 0) break; e = nx;
    }
    return vs;
  };
  for (int v : dv) if (deg[v] != 2) for (int e : T.vE[size_t(v)]) if (set.count(e) && !used.count(e)) out.push_back({walk(v, e), false});
  for (int e : sel) if (!used.count(e)) { auto vs = walk(T.ea[size_t(e)], e); vs.pop_back(); out.push_back({vs, true}); }
  return out;
}

namespace {
// as few anchors as keep the curve on the polyline: Douglas–Peucker, then the vertices the smooth curve through the
// kept ones misses are added; turns over 50° are kept as corners (sharp → smoothness 0)
struct FitPt { Vec3 p; bool sharp = false; };
std::vector<FitPt> fitPolyline(const std::vector<Vec3>& pts, bool closed) {
  std::vector<Vec3> P = pts; if (closed) P.push_back(pts[0]);
  const int N = int(P.size());
  auto sharpAt = [&](int i) {
    if (!closed && (i == 0 || i == N - 1)) return false;
    int a = closed ? (i - 1 + N - 1) % (N - 1) : i - 1, b = closed ? (i + 1) % (N - 1) : i + 1;
    Vec3 u = P[size_t(i)] - P[size_t(a)], w = P[size_t(b)] - P[size_t(i)];
    return u.lengthSq() > 0 && w.lengthSq() > 0 && angleTo(u, w) > 50 * PI / 180;
  };
  auto mark = [&](const Vec3& p, int i) { return FitPt{p, sharpAt(i)}; };
  if (N <= 2 || (closed && N <= 4)) { std::vector<FitPt> o; for (size_t i = 0; i < pts.size(); i++) o.push_back(mark(pts[i], int(i))); return o; }
  double L = 0; for (int i = 1; i < N; i++) L += P[size_t(i)].distanceTo(P[size_t(i - 1)]);
  const double tol = std::max(L * 0.003, 1e-7); std::vector<char> keep(size_t(N), 0); keep[0] = keep[size_t(N - 1)] = 1;
  if (closed) { keep[size_t(jsRound((N - 1) / 3.0))] = 1; keep[size_t(jsRound(2 * (N - 1) / 3.0))] = 1; }
  for (int i = 0; i < N - 1; i++) if (sharpAt(i)) keep[size_t(i)] = 1;
  if (closed && keep[0]) keep[size_t(N - 1)] = 1;
  std::function<void(int, int)> dp = [&](int i, int j) {
    if (j - i < 2) return;
    int k = -1; double md = tol; for (int q = i + 1; q < j; q++) { double d = segDist3(P[size_t(q)], P[size_t(i)], P[size_t(j)]); if (d > md) { md = d; k = q; } }
    if (k >= 0) { keep[size_t(k)] = 1; dp(i, k); dp(k, j); }
  };
  { std::vector<int> ids; for (int i = 0; i < N; i++) if (keep[size_t(i)]) ids.push_back(i); for (size_t s = 0; s + 1 < ids.size(); s++) dp(ids[s], ids[s + 1]); }
  for (int iter = 0; iter < 12; iter++) {
    std::vector<int> ids; for (int i = 0; i < N; i++) if (keep[size_t(i)]) ids.push_back(i);
    std::vector<int> list = ids; if (closed) list.pop_back();
    Scene tmp; Item c; c.id = 1; c.type = ItemType::Curve; c.closed = closed; c.roundSet = c.fairSet = true;
    for (size_t k = 0; k < list.size(); k++) { Anchor a; a.id = 100 + int(k); a.pos = P[size_t(list[k])]; a.smooth = sharpAt(list[k]) ? 0 : 1; tmp.anchors.emplace(a.id, a); c.anchors.push_back(a.id); }
    tmp.items.emplace(1, c); tmp.reindex();
    auto sm = sampleCurve(tmp, *tmp.item(1), 12);
    bool added = false;
    for (size_t s = 0; s + 1 < ids.size(); s++) {
      if (ids[s + 1] - ids[s] < 2) continue;
      std::vector<Vec3> seg;
      for (size_t k = 0; k < sm.size(); k++) if (sm[k].seg == int(s)) { seg.push_back(sm[k].p); if (k + 1 < sm.size() && sm[k + 1].seg != int(s)) seg.push_back(sm[k + 1].p); }
      if (s + 2 == ids.size() && !sm.empty()) seg.push_back(sm.back().p);
      int k = -1; double md = tol * 1.5;
      for (int q = ids[s] + 1; q < ids[s + 1]; q++) {
        double d = 1e300; for (size_t j = 0; j + 1 < seg.size(); j++) d = std::min(d, segDist3(P[size_t(q)], seg[j], seg[j + 1]));
        if (d > md) { md = d; k = q; }
      }
      if (k >= 0) { keep[size_t(k)] = 1; added = true; }
    }
    if (!added) { std::vector<FitPt> o; for (int i : list) o.push_back(mark(P[size_t(i)], i)); return o; }
  }
  std::vector<FitPt> o; for (int i = 0; i < N; i++) if (keep[size_t(i)] && (!closed || i < N - 1)) o.push_back(mark(P[size_t(i)], i));
  return o;
}
int fitAnchor(Scene& sc, const FitPt& p) { Anchor& a = sc.addAnchor(p.p); if (p.sharp) a.smooth = 0; return a.id; }

bool serviceGroup(const Scene& sc, const Item* x) { for (const Item* p = x; p; p = p->parent >= 0 ? sc.item(p->parent) : nullptr) if (p->raw.value("importGroup").toBool() || p->raw.value("guidesGroup").toBool() || p->raw.value("meshesGroup").toBool()) return true; return false; }
int newGroup(Scene& sc, const QString& name) {
  edit::nextNumber(sc, ItemType::Group);
  Item& g = sc.addItem(ItemType::Group, -1, name); g.expanded = true;
  g.raw = QJsonObject{{"sym", QJsonValue()}, {"guides", "auto"}, {"conv", QJsonValue()}, {"guidesGroup", false}, {"meshesGroup", false}, {"importGroup", false},
                      {"labels", QJsonValue()}, {"guidesOff", false}, {"logic", 2}, {"view", QJsonValue()}, {"side", QJsonValue()}, {"spos", QJsonValue()}};
  return g.id;
}
}  // namespace

QString edgesToCurves(Scene& sc, Selection& sel, int meshId, const std::vector<int>& edges) {
  Item* it = sc.item(meshId); if (!it || !it->poly || edges.empty()) return QObject::tr("!Select edges first (Edges tool, V)");
  auto Tp = topoOf(*it->poly); const Topo& T = *Tp; const auto& W = it->poly->pos;
  auto chains = edgeChains(T, edges);
  // the group: the selected group (not a service one), else "<mesh> paths" (made once)
  int g = -1;
  for (auto r = sel.items.rbegin(); r != sel.items.rend(); ++r) if (const Item* x = sc.item(*r); x && x->type == ItemType::Group) { if (!serviceGroup(sc, x)) g = x->id; break; }
  if (g < 0) { int pg = it->raw.value("pathGroup").toInt(-1); if (sc.item(pg) && sc.item(pg)->type == ItemType::Group) g = pg; }
  if (g < 0) { QString nm = it->name + " paths"; g = newGroup(sc, nm); sc.item(meshId)->raw.insert("pathGroup", g); }
  std::map<int, int> ends; std::vector<int> made;
  auto endAnchor = [&](int v) { auto f = ends.find(v); if (f != ends.end()) return f->second; int a = sc.addAnchor(W[size_t(v)]).id; ends[v] = a; return a; };
  for (auto& ch : chains) {
    std::vector<Vec3> pts; for (int v : ch.verts) pts.push_back(W[size_t(v)]);
    auto fit = fitPolyline(pts, ch.closed); if (fit.size() < 2) continue;
    Item& c = edit::makeUserCurve(sc, g); int cid = c.id;
    std::vector<int> an;
    for (size_t k = 0; k < fit.size(); k++) an.push_back(ch.closed ? fitAnchor(sc, fit[k]) : k == 0 ? endAnchor(ch.verts.front()) : k + 1 == fit.size() ? endAnchor(ch.verts.back()) : fitAnchor(sc, fit[k]));
    if (an.front() == an.back()) an.back() = sc.addAnchor(sc.anchor(an.front())->pos).id;
    Item& ci = *sc.item(cid); ci.anchors = an; if (ch.closed) ci.closed = true;
    made.push_back(cid);
  }
  if (made.empty()) return QObject::tr("!Nothing to convert");
  sc.item(g)->expanded = true; sel.anchors.clear(); sel.items = made;
  sc.reindex();
  return QObject::tr("%1 curve%2 from %3 edge%4 → %5").arg(made.size()).arg(made.size() == 1 ? "" : "s").arg(edges.size()).arg(edges.size() == 1 ? "" : "s").arg(sc.item(g)->name);
}

std::vector<int> polylinesToCurves(Scene& sc, const std::vector<std::vector<Vec3>>& lines, const QString& groupName) {
  std::vector<int> made; int g = -1;
  for (const auto& L0 : lines) {
    std::vector<Vec3> L; for (const Vec3& p : L0) if (L.empty() || p.distanceTo(L.back()) > 1e-9) L.push_back(p);
    if (L.size() < 2) continue;
    double len = 0; for (size_t i = 1; i < L.size(); i++) len += L[i].distanceTo(L[i - 1]);
    const bool closed = L.size() > 3 && L.front().distanceTo(L.back()) < len * 1e-4; if (closed) L.pop_back();
    auto fit = fitPolyline(L, closed); if (fit.size() < 2) continue;
    if (g < 0) g = newGroup(sc, groupName);
    Item& c = edit::makeUserCurve(sc, g); int cid = c.id; std::vector<int> an;
    for (auto& f : fit) an.push_back(fitAnchor(sc, f));
    Item& ci = *sc.item(cid); ci.anchors = an; if (closed) ci.closed = true; made.push_back(cid);
  }
  if (!made.empty()) sc.reindex();
  return made;
}

// ---------- Mesh → curves & surfaces ----------
namespace {
struct Fam { std::vector<int> ls; int cl = 0; double cmean = 0, mean = 0; };
struct Families { OSet vs; std::vector<Loop> loops; std::unordered_map<int, int> loopOf; std::vector<int> col; int across = 0; Fam fam[2]; std::vector<double> LN; };
double loopLen(const std::vector<Vec3>& W, const Loop& L) { double s = 0; for (size_t i = 0; i + (L.closed ? 0 : 1) < L.verts.size(); i++) s += W[size_t(L.verts[i])].distanceTo(W[size_t(L.verts[(i + 1) % L.verts.size()])]); return s; }

std::optional<Families> loopFamilies(const Topo& T, const std::vector<Vec3>& W, const std::vector<int>& E) {
  Families F; OSet fs;
  for (int e : E) { F.vs.add(T.ea[size_t(e)]); F.vs.add(T.eb[size_t(e)]); for (int f : T.eFaces[size_t(e)]) fs.add(f); }
  int quads = 0; for (int f : fs.v) if (T.fEdges[size_t(f)].size() == 4) quads++;
  if (quads < double(fs.size()) * 0.6) return std::nullopt;
  for (int e : E) if (!F.loopOf.count(e)) { Loop L = edgeLoop(T, e); for (int x : L.edges) F.loopOf[x] = int(F.loops.size()); F.loops.push_back(L); }
  const int nl = int(F.loops.size());
  std::vector<std::vector<std::pair<int, int>>> w(static_cast<size_t>(nl));
  auto inc = [&](int a, int b) { for (auto& p : w[size_t(a)]) if (p.first == b) { p.second++; return; } w[size_t(a)].push_back({b, 1}); };
  auto link = [&](int a, int b) { if (a != b) { inc(a, b); inc(b, a); } };
  for (int v : F.vs.v) {
    const auto& es = T.vE[size_t(v)];
    if (es.size() == 4 && std::all_of(es.begin(), es.end(), [&](int x) { return T.eFaces[size_t(x)].size() == 2; })) { int o = loopNext(T, es[0], v); for (int x : es) if (x != es[0] && x != o) link(F.loopOf[es[0]], F.loopOf[x]); }
    else if (es.size() == 3) { std::vector<int> bd, ins; for (int x : es) (T.eFaces[size_t(x)].size() == 1 ? bd : ins).push_back(x); if (bd.size() == 2 && ins.size() == 1) link(F.loopOf[bd[0]], F.loopOf[ins[0]]); }
  }
  for (auto& L : F.loops) F.LN.push_back(loopLen(W, L));
  F.col.assign(size_t(nl), -1);
  for (int i = 0; i < nl; i++) {
    const Loop& L = F.loops[size_t(i)]; if (!L.closed || L.edges.size() < 3) continue;
    std::vector<double> cr; for (auto& [j, k] : w[size_t(i)]) for (int q = 0; q < k; q++) cr.push_back(F.LN[size_t(j)]);
    std::sort(cr.begin(), cr.end()); double med = cr.empty() ? 0 : cr[cr.size() >> 1];
    F.col[size_t(i)] = med && F.LN[size_t(i)] < med ? 0 : 1;
  }
  bool hasRings = std::find(F.col.begin(), F.col.end(), 0) != F.col.end();
  if (hasRings) {
    for (int pass = 0; pass < 50; pass++) {
      int ch = 0;
      for (int i = 0; i < nl; i++) {
        if (F.col[size_t(i)] >= 0) continue;
        int r = 0, l = 0; for (auto& [j, k] : w[size_t(i)]) { if (F.col[size_t(j)] == 0) r += k; else if (F.col[size_t(j)] == 1) l += k; }
        if (r || l) { F.col[size_t(i)] = r >= l ? 1 : 0; ch++; }
      }
      if (!ch) break;
    }
    for (int& c : F.col) if (c < 0) c = 1;
  } else {
    std::fill(F.col.begin(), F.col.end(), -1);
    for (int s = 0; s < nl; s++) {
      if (F.col[size_t(s)] >= 0) continue; F.col[size_t(s)] = 0; std::vector<int> q{s};
      for (size_t qi = 0; qi < q.size(); qi++) { int a = q[qi]; for (auto& [b, k] : w[size_t(a)]) if (F.col[size_t(b)] < 0) { F.col[size_t(b)] = 1 - F.col[size_t(a)]; q.push_back(b); } }
    }
  }
  auto avg = [&](const std::vector<int>& l) { double s = 0; for (int i : l) s += F.LN[size_t(i)]; return s / std::max<size_t>(1, l.size()); };
  for (int c : {0, 1}) {
    Fam& f = F.fam[c]; std::vector<int> cl;
    for (int i = 0; i < nl; i++) if (F.col[size_t(i)] == c && F.loops[size_t(i)].edges.size() >= 2) { f.ls.push_back(i); if (F.loops[size_t(i)].closed && F.loops[size_t(i)].edges.size() >= 3) cl.push_back(i); }
    f.cl = int(cl.size()); f.cmean = avg(cl); f.mean = avg(f.ls);
  }
  F.across = hasRings ? 0 : (F.fam[0].cl || F.fam[1].cl) ? (F.fam[0].cl ? 0 : 1) : (F.fam[0].mean <= F.fam[1].mean ? 0 : 1);
  return F;
}

std::vector<Vec3> faceNormals(const Topo& T, const PolyMesh& m) {
  std::vector<Vec3> fn(static_cast<size_t>(T.nf));
  for (int f = 0; f < T.nf; f++) { int s = m.faceStart[size_t(f)], e = m.faceStart[size_t(f) + 1];
    for (int k = s + 1; k + 1 < e; k++) { const Vec3 &a = m.pos[size_t(m.faceVerts[size_t(s)])], &b = m.pos[size_t(m.faceVerts[size_t(k)])], &c = m.pos[size_t(m.faceVerts[size_t(k + 1)])]; fn[size_t(f)] += (b - a).cross(c - a); } }
  for (auto& n : fn) n = n.normalized();
  return fn;
}
std::vector<std::vector<int>> meshComponents(const Topo& T) {
  std::vector<int> up(static_cast<size_t>(T.n)); for (int i = 0; i < T.n; i++) up[size_t(i)] = i;
  std::function<int(int)> find = [&](int i) { while (up[size_t(i)] != i) { up[size_t(i)] = up[size_t(up[size_t(i)])]; i = up[size_t(i)]; } return i; };
  for (int e = 0; e < T.ne; e++) { int x = find(T.ea[size_t(e)]), y = find(T.eb[size_t(e)]); if (x != y) up[size_t(x)] = y; }
  std::map<int, int> idx; std::vector<std::vector<int>> out;
  for (int e = 0; e < T.ne; e++) { int r = find(T.ea[size_t(e)]); auto f = idx.find(r); if (f == idx.end()) { idx[r] = int(out.size()); out.push_back({e}); } else out[size_t(f->second)].push_back(e); }
  return out;
}
// the average dihedral angle (degrees) along a line of vertices (a border — 180)
double creaseOf(const Topo& T, const std::vector<Vec3>& fn, const std::vector<int>& l) {
  double s = 0; int k = 0;
  for (size_t i = 0; i + 1 < l.size(); i++) { int e = T.edge(l[i], l[i + 1]); if (e < 0) continue; const auto& f = T.eFaces[size_t(e)];
    s += f.size() == 2 ? std::acos(clamp(fn[size_t(f[0])].dot(fn[size_t(f[1])]), -1, 1)) * 180 / PI : 180; k++; }
  return k ? s / k : 0;
}
struct Pos { int k; double arc; };
struct Picked { std::vector<int> keep; bool closed; std::vector<double> bulge; };
Picked pickRingLines(const std::vector<Vec3>& W, const std::vector<int>& rv, const std::vector<Pos>& pos, double per, bool closed, const std::vector<double>& cre, const M2C& opt) {
  const int N = int(pos.size());
  std::vector<int> keep; for (int k = 0; k < N; k++) keep.push_back(k);
  if (N > opt.maxC) {
    std::vector<int> kk; for (int k : keep) if (cre[size_t(k)] >= opt.angle) kk.push_back(k); keep = kk;
    if (int(keep.size()) > opt.maxC) { std::stable_sort(keep.begin(), keep.end(), [&](int a, int b) { return cre[size_t(b)] < cre[size_t(a)]; }); keep.resize(static_cast<size_t>(opt.maxC)); std::sort(keep.begin(), keep.end()); }
    if (int(keep.size()) < (closed ? 3 : 2)) {
      int cnt = std::min(N, std::max(closed ? 3 : 2, opt.count)); std::set<int> used; keep.clear();
      for (int j = 0; j < cnt; j++) {
        double t = closed ? pos[0].arc + per * j / cnt : pos[0].arc + (pos[size_t(N - 1)].arc - pos[0].arc) * j / (cnt - 1);
        int bk = -1; double bd = 1e300;
        for (int k = 0; k < N; k++) { if (used.count(k)) continue; double d = std::abs(pos[size_t(k)].arc - t); if (closed) d = std::min(d, per - d); if (d < bd) { bd = d; bk = k; } }
        if (bk >= 0) { used.insert(bk); keep.push_back(bk); }
      }
      std::sort(keep.begin(), keep.end());
    }
  }
  closed = closed && keep.size() >= 3;
  Vec3 cen; for (int v : rv) cen += W[size_t(v)]; cen = cen / double(rv.size());
  std::vector<double> bulge;
  for (size_t j = 0; j + (closed ? 0 : 1) < keep.size(); j++) {
    int i1 = pos[size_t(keep[j])].k, i2 = pos[size_t(keep[(j + 1) % keep.size()])].k;
    if (i2 <= i1) i2 += int(rv.size());
    const Vec3 &A = W[size_t(rv[size_t(i1)])], &B = W[size_t(rv[size_t(i2) % rv.size()])]; double w = A.distanceTo(B), best = 0;
    if (w > 1e-9) for (int i = i1 + 1; i < i2; i++) {
      const Vec3& p = W[size_t(rv[size_t(i) % rv.size()])]; double d = segDist3(p, A, B); Vec3 mid = (A + B) * 0.5;
      double sgn = p.distanceTo(cen) >= mid.distanceTo(cen) ? 1 : -1;
      if (d > std::abs(best)) best = sgn * d;
    }
    bulge.push_back(w > 1e-9 ? clamp(best / w, -1, 1) : 0);
  }
  return {keep, closed, bulge};
}

struct Line { std::vector<Vec3> pts; std::vector<QString> keys; QString k0, k1; bool hasKeys = false; };
struct LockRes { std::vector<Line> lines; bool closed = false, oriented = false; std::vector<double> bulge; };

std::vector<LockRes> lockTree(const Topo& T, const std::vector<Vec3>& W, const Families& F, const M2C& opt, const std::vector<Vec3>& fn, int ci, const std::vector<Vec3>* hp) {
  const auto& loops = F.loops; const int across = F.across;
  std::unordered_map<int, int> ringOf; std::vector<const Loop*> rings;
  for (size_t i = 0; i < loops.size(); i++) { const Loop& L = loops[i]; if (F.col[i] == across && L.closed && L.edges.size() >= 3) { for (int v : L.verts) if (!ringOf.count(v)) ringOf[v] = int(rings.size()); rings.push_back(&L); } }
  if (rings.size() < 2) return {};
  struct Cl { std::vector<int> verts; OSet rings; };
  std::set<int> cl; std::vector<Cl> clusters;
  for (int v : F.vs.v) if (!ringOf.count(v) && !cl.count(v)) {
    Cl c; std::vector<int> q{v}; cl.insert(v);
    while (!q.empty()) { int a = q.back(); q.pop_back(); c.verts.push_back(a); for (int e : T.vE[size_t(a)]) { int b = T.other(e, a); auto r = ringOf.find(b); if (r != ringOf.end()) c.rings.add(r->second); else if (!cl.count(b)) { cl.insert(b); q.push_back(b); } } }
    clusters.push_back(std::move(c));
  }
  struct End { int ring; std::vector<int> verts; bool open = false; Vec3 c; };
  std::vector<End> ends;
  for (auto& c : clusters) if (c.rings.size() == 1) ends.push_back({c.rings.v[0], c.verts});
  for (size_t i = 0; i < rings.size(); i++) if (std::all_of(rings[i]->edges.begin(), rings[i]->edges.end(), [&](int e) { return T.eFaces[size_t(e)].size() == 1; })) ends.push_back({int(i), {}, true});
  if (ends.size() < 2) return {};
  for (auto& e : ends) { const auto& pts = e.verts.empty() ? rings[size_t(e.ring)]->verts : e.verts; Vec3 c; for (int v : pts) c += W[size_t(v)]; e.c = c / double(pts.size()); }
  auto rootScore = [&](const End& e) { if (hp && !hp->empty()) { double m = 1e300; for (auto& p : *hp) m = std::min(m, (p - e.c).lengthSq()); return m; } return -e.c.y; };
  size_t rootI = 0; for (size_t i = 0; i < ends.size(); i++) if (rootScore(ends[i]) < rootScore(ends[rootI])) rootI = i;
  const int rootRing = ends[rootI].ring;
  std::unordered_map<int, int> hop; std::vector<int> q = rings[size_t(rootRing)]->verts; for (int v : q) hop[v] = 0;
  for (int h = 1; !q.empty(); h++) { std::vector<int> nq; for (int a : q) for (int e : T.vE[size_t(a)]) { int b = T.other(e, a); if (!hop.count(b)) { hop[b] = h; nq.push_back(b); } } q = nq; }
  auto isLong = [&](int e) { auto f = F.loopOf.find(e); return f != F.loopOf.end() && F.col[size_t(f->second)] != across; };
  auto dir = [&](int a, int b) { return (W[size_t(b)] - W[size_t(a)]).normalized(); };
  auto traceUp = [&](int v0) {
    std::vector<int> path{v0}; std::set<int> on{v0}; int prev = -1, v = v0;
    for (int guard = 0; (ringOf.count(v) ? ringOf[v] : -1) != rootRing && guard < 5000; guard++) {
      if (!hop.count(v)) break; int hv = hop[v]; struct C { int e, u; }; std::vector<C> cands;
      for (int e : T.vE[size_t(v)]) if (isLong(e)) { int u = T.other(e, v); if (!on.count(u) && hop.count(u) && hop[u] <= hv) cands.push_back({e, u}); }
      if (cands.empty()) break;
      const C* next = nullptr;
      if (prev >= 0) {
        int n = T.vE[size_t(v)].size() == 4 ? loopNext(T, T.edge(prev, v), v) : -1;
        for (auto& c : cands) if (c.e == n) { next = &c; break; }
        if (!next) { Vec3 d0 = dir(prev, v); next = &cands[0]; for (auto& c : cands) if (d0.dot(dir(v, c.u)) > d0.dot(dir(v, next->u))) next = &c; }
      } else { next = &cands[0]; for (auto& c : cands) if (hop[c.u] < hop[next->u]) next = &c; }
      prev = v; v = next->u; path.push_back(v); on.insert(v);
    }
    std::reverse(path.begin(), path.end());
    return path;
  };
  std::vector<size_t> tips; for (size_t i = 0; i < ends.size(); i++) if (i != rootI) tips.push_back(i);
  std::vector<std::vector<std::vector<int>>> traced; for (size_t t : tips) { std::vector<std::vector<int>> ls; for (int v : rings[size_t(ends[t].ring)]->verts) ls.push_back(traceUp(v)); traced.push_back(ls); }
  // a lock takes only the lines reaching the root, one per root vertex
  struct Pk { size_t tip; size_t ti; Picked pk; std::vector<std::vector<int>> paths; };
  std::vector<Pk> picked;
  for (size_t ti = 0; ti < tips.size(); ti++) {
    const End& tip = ends[tips[ti]]; const auto& rv = rings[size_t(tip.ring)]->verts; const auto& lines = traced[ti]; double arc = 0;
    std::vector<Pos> pos; for (size_t k = 0; k < rv.size(); k++) { if (k) arc += W[size_t(rv[k])].distanceTo(W[size_t(rv[k - 1])]); pos.push_back({int(k), arc}); }
    std::set<int> tops; std::vector<int> ok;
    for (size_t k = 0; k < lines.size(); k++) { int p0 = lines[k][0]; if ((ringOf.count(p0) ? ringOf[p0] : -1) == rootRing && !tops.count(p0)) { tops.insert(p0); ok.push_back(int(k)); } }
    std::vector<int> use = ok; if (use.size() < 3) { use.clear(); for (size_t k = 0; k < lines.size(); k++) use.push_back(int(k)); }
    std::vector<Pos> up; std::vector<double> cr; for (int k : use) { up.push_back(pos[size_t(k)]); cr.push_back(creaseOf(T, fn, lines[size_t(k)])); }
    Picked pk = pickRingLines(W, rv, up, loopLen(W, *rings[size_t(tip.ring)]), true, cr, opt);
    if (pk.keep.size() >= 2) { std::vector<std::vector<int>> paths; for (int j : pk.keep) paths.push_back(lines[size_t(use[size_t(j)])]); picked.push_back({tips[ti], ti, pk, paths}); }
  }
  // how many lines run through each vertex: where the count changes, lines part (a fork) — an anchor there
  std::unordered_map<int, int> use; for (auto& x : picked) for (auto& p : x.paths) for (int v : p) use[v]++;
  int H = 1; for (auto& x : picked) for (auto& p : x.paths) H = std::max(H, int(p.size()) - 1);
  const int s = std::max(1, int(jsRound(H / 10.0)));
  std::vector<LockRes> out;
  for (auto& x : picked) {
    LockRes r; r.oriented = true; r.closed = x.pk.closed; r.bulge = x.pk.bulge;
    for (auto& path : x.paths) {
      Line l; l.hasKeys = true;
      for (size_t i = 0; i < path.size(); i++) {
        int v = path[i], u = use[v];
        bool part = (i > 0 && use[path[i - 1]] != u) || (i + 1 < path.size() && use[path[i + 1]] != u);
        if (i == 0 || i + 1 == path.size() || T.vE[size_t(v)].size() != 4 || part || (hop.count(v) ? hop[v] : 0) % s == 0) { l.pts.push_back(W[size_t(v)]); l.keys.push_back("v" + QString::number(v)); }
      }
      const End& tip = ends[x.tip];
      if (!tip.open) { l.pts.push_back(tip.c); l.keys.push_back(QString("t%1_%2").arg(ci).arg(x.ti)); }
      r.lines.push_back(l);
    }
    out.push_back(r);
  }
  return out;
}

std::optional<LockRes> lockFromQuads(const Topo& T, const std::vector<Vec3>& W, const Families& F, const M2C& opt, const std::vector<Vec3>& fn) {
  const auto& loops = F.loops; const int across = F.across;
  std::vector<std::optional<std::vector<int>>> lines;
  for (int i : F.fam[1 - across].ls) if (!loops[size_t(i)].closed) lines.push_back(loops[size_t(i)].verts);
  std::vector<const Loop*> rings; for (int i : F.fam[across].ls) rings.push_back(&loops[size_t(i)]);
  if (lines.size() < 2 || rings.empty()) return std::nullopt;
  auto dirIn = [&](const std::vector<int>& l, bool end) { size_t n = l.size(); return end ? (W[size_t(l[n - 1])] - W[size_t(l[n - 2])]).normalized() : (W[size_t(l[0])] - W[size_t(l[1])]).normalized(); };
  for (int pass = 0; pass < 50; pass++) {
    std::vector<int> atOrder; std::map<int, std::vector<std::pair<int, bool>>> at;
    for (size_t i = 0; i < lines.size(); i++) { if (!lines[i]) continue; for (bool end : {false, true}) { int v = end ? lines[i]->back() : lines[i]->front(); if (!at.count(v)) atOrder.push_back(v); at[v].push_back({int(i), end}); } }
    struct B { double s; std::pair<int, bool> A, Bq; }; std::optional<B> best;
    for (int v : atOrder) { auto& list = at[v];
      for (auto& A : list) for (auto& Bq : list) { if (A.first == Bq.first) continue;
        double sv = -dirIn(*lines[size_t(A.first)], A.second).dot(dirIn(*lines[size_t(Bq.first)], Bq.second));
        if (sv > 0.82 && (!best || sv > best->s)) best = B{sv, A, Bq}; } }
    if (!best) break;
    auto a = *lines[size_t(best->A.first)]; if (!best->A.second) std::reverse(a.begin(), a.end());
    auto b = *lines[size_t(best->Bq.first)]; if (best->Bq.second) std::reverse(b.begin(), b.end());
    a.insert(a.end(), b.begin() + 1, b.end()); lines[size_t(best->A.first)] = a; lines[size_t(best->Bq.first)] = std::nullopt;
  }
  std::vector<std::vector<int>> L; for (auto& l : lines) if (l) L.push_back(*l);
  std::unordered_map<int, int> lineAt; for (size_t i = 0; i < L.size(); i++) for (int v : L[i]) if (!lineAt.count(v)) lineAt[v] = int(i);
  struct R { const Loop* r; std::vector<int> ord; std::vector<Pos> pos; double per; }; std::optional<R> best;
  for (const Loop* r : rings) {
    std::vector<int> seen; std::vector<Pos> pos; double arc = 0;
    for (size_t k = 0; k < r->verts.size(); k++) { int v = r->verts[k]; if (k) arc += W[size_t(v)].distanceTo(W[size_t(r->verts[k - 1])]); auto li = lineAt.find(v); if (li != lineAt.end() && !has(seen, li->second)) { seen.push_back(li->second); pos.push_back({int(k), arc}); } }
    double per = loopLen(W, *r);
    if (!best || seen.size() > best->ord.size() || (seen.size() == best->ord.size() && per > best->per)) best = R{r, seen, pos, per};
  }
  if (!best || best->ord.size() < 2) return std::nullopt;
  std::vector<double> cr; for (int li : best->ord) cr.push_back(creaseOf(T, fn, L[size_t(li)]));
  Picked pk = pickRingLines(W, best->r->verts, best->pos, best->per, best->r->closed && best->ord.size() >= 3, cr, opt);
  if (pk.keep.size() < 2) return std::nullopt;
  LockRes res; res.closed = pk.closed; res.bulge = pk.bulge;
  for (int k : pk.keep) { const auto& l = L[size_t(best->ord[size_t(k)])]; Line ln; for (int v : l) ln.pts.push_back(W[size_t(v)]); ln.k0 = "v" + QString::number(l.front()); ln.k1 = "v" + QString::number(l.back()); res.lines.push_back(ln); }
  return res;
}

std::optional<LockRes> lockFromSlices(const Topo& T, const std::vector<Vec3>& W, const std::vector<int>& E, const M2C& opt, int ci, double step) {
  OSet vset; for (int e : E) { vset.add(T.ea[size_t(e)]); vset.add(T.eb[size_t(e)]); }
  const auto& vs = vset.v; if (vs.size() < 6 || vs.size() > 20000) return std::nullopt;
  auto geo = [&](int src) {
    std::vector<double> d(size_t(T.n), 1e300); std::vector<std::pair<double, int>> q{{0, src}}; d[size_t(src)] = 0;
    while (!q.empty()) {
      size_t bi = 0; for (size_t i = 1; i < q.size(); i++) if (q[i].first < q[bi].first) bi = i;
      auto [dv, v] = q[bi]; q[bi] = q.back(); q.pop_back(); if (dv > d[size_t(v)]) continue;
      for (int e : T.vE[size_t(v)]) { int u = T.other(e, v); double nd = dv + W[size_t(v)].distanceTo(W[size_t(u)]); if (nd < d[size_t(u)]) { d[size_t(u)] = nd; q.push_back({nd, u}); } }
    }
    return d;
  };
  auto far = [&](const std::vector<double>& d) { int m = vs[0]; for (int v : vs) if (d[size_t(v)] > d[size_t(m)] && d[size_t(v)] < 1e299) m = v; return m; };
  int A = far(geo(vs[0])); auto d = geo(A); int B = far(d); double Lm = d[size_t(B)]; if (!(Lm > 0)) return std::nullopt;
  int K = int(clamp(jsRound(Lm / (step > 0 ? step : Lm / 12)), 4, 60));
  struct Lv { std::vector<Vec3> pts; Vec3 c; }; std::vector<Lv> levels;
  for (int j = 1; j < K; j++) {
    double t = Lm * j / K; Lv L;
    for (int e : E) { int a = T.ea[size_t(e)], b = T.eb[size_t(e)]; double da = d[size_t(a)], db = d[size_t(b)]; if ((da - t) * (db - t) < 0) L.pts.push_back(W[size_t(a)].lerp(W[size_t(b)], (t - da) / (db - da))); }
    if (L.pts.size() >= 3) { Vec3 c; for (auto& p : L.pts) c += p; L.c = c / double(L.pts.size()); levels.push_back(L); }
  }
  if (levels.size() < 2) return std::nullopt;
  const int n = int(levels.size()), cnt = std::max(3, opt.count); std::vector<std::vector<Vec3>> lines(static_cast<size_t>(cnt)); std::vector<double> spread;
  std::optional<Vec3> u;
  for (int j = 0; j < n; j++) {
    const Lv& L = levels[size_t(j)]; Vec3 t = (levels[size_t(std::min(j + 1, n - 1))].c - levels[size_t(std::max(j - 1, 0))].c).normalized();
    if (!u) u = std::abs(t.y) < 0.9 ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    *u = (*u - t * u->dot(t)).normalized(); Vec3 w = t.cross(*u);
    struct Q { double x, y; Vec3 p; }; std::vector<Q> poly;
    for (auto& p : L.pts) { Vec3 q = p - L.c; poly.push_back({q.dot(*u), q.dot(w), p}); }
    std::stable_sort(poly.begin(), poly.end(), [](const Q& a, const Q& b) { return std::atan2(a.y, a.x) < std::atan2(b.y, b.x); });
    double sp = 0; for (auto& q : poly) sp += std::hypot(q.x, q.y); spread.push_back(sp / double(poly.size()));
    for (int k = 0; k < cnt; k++) {
      double th = 2 * PI * k / cnt, dx = std::cos(th), dy = std::sin(th); std::optional<std::pair<double, Vec3>> best;
      for (size_t i = 0; i < poly.size(); i++) {
        const Q &a = poly[i], &b = poly[(i + 1) % poly.size()]; double ex = b.x - a.x, ey = b.y - a.y, den = dx * ey - dy * ex; if (std::abs(den) < 1e-12) continue;
        double tt = (a.x * ey - a.y * ex) / den, sv = (a.x * dy - a.y * dx) / den;
        if (tt > 0 && sv >= 0 && sv <= 1 && (!best || tt > best->first)) best = std::make_pair(tt, a.p.lerp(b.p, sv));
      }
      if (!best) { const Q* bq = &poly[0]; double bd = 1e300; for (auto& q : poly) { double dd = std::abs(std::atan2(q.y, q.x) - th); dd = std::min(dd, 2 * PI - dd); if (dd < bd) { bd = dd; bq = &q; } } best = std::make_pair(0.0, bq->p); }
      lines[size_t(k)].push_back(best->second);
    }
  }
  bool tipEnd = spread[size_t(n - 1)] < spread[0];
  Vec3 tip = W[size_t(tipEnd ? B : A)]; QString key = QString("tip%1").arg(ci);
  LockRes r; r.closed = true;
  for (auto& pts : lines) { Line l; if (tipEnd) { l.pts = pts; l.pts.push_back(tip); l.k1 = key; } else { l.pts = {tip}; l.pts.insert(l.pts.end(), pts.begin(), pts.end()); l.k0 = key; } r.lines.push_back(l); }
  return r;
}

// curves (root → tip, the root — the wider end unless the lock comes oriented) and walls of one lock; points with the same
// key become one shared anchor (byKey is shared by the locks of one part: forks get welded trunks)
std::pair<int, int> buildLockCurves(Scene& sc, int g, LockRes res, std::map<QString, int>& byKey) {
  auto& L = res.lines;
  if (!res.oriented) {
    auto flip = [](Line& l) { std::reverse(l.pts.begin(), l.pts.end()); std::swap(l.k0, l.k1); };
    Vec3 f0 = L[0].pts.front(), f1 = L[0].pts.back();
    for (size_t i = 1; i < L.size(); i++) { Vec3 a = L[i].pts.front(), b = L[i].pts.back(); if (a.distanceTo(f0) + b.distanceTo(f1) > a.distanceTo(f1) + b.distanceTo(f0)) flip(L[i]); }
    auto spr = [&](bool end) { Vec3 c; std::vector<Vec3> ps; for (auto& l : L) ps.push_back(end ? l.pts.back() : l.pts.front()); for (auto& p : ps) c += p; c = c / double(ps.size()); double s = 0; for (auto& p : ps) s += p.distanceTo(c); return std::make_pair(s / double(ps.size()), c.y); };
    auto s0 = spr(false), s1 = spr(true);
    if (std::abs(s0.first - s1.first) > 0.1 * std::max(s0.first, s1.first) ? s0.first < s1.first : s0.second < s1.second) for (auto& l : L) flip(l);
  }
  std::vector<int> made;
  for (auto& l : L) {
    std::vector<FitPt> fit; if (l.hasKeys) for (auto& p : l.pts) fit.push_back({p, false}); else fit = fitPolyline(l.pts, false);
    if (fit.size() < 2) continue;
    std::vector<QString> keys = l.keys; if (!l.hasKeys) { keys.assign(fit.size(), QString()); keys.front() = l.k0; keys.back() = l.k1; }
    Item& c = edit::makeUserCurve(sc, g); int cid = c.id;
    std::vector<int> an; std::set<int> seen;
    for (size_t k = 0; k < fit.size(); k++) {
      int a;
      if (keys[k].isEmpty()) a = fitAnchor(sc, fit[k]);
      else { auto f = byKey.find(keys[k]); if (f == byKey.end()) { a = sc.addAnchor(fit[k].p).id; byKey[keys[k]] = a; } else a = f->second; }
      if (seen.insert(a).second) an.push_back(a);                      // (a curve never runs through one anchor twice)
    }
    if (an.size() < 2) { sc.removeFromParent(cid); sc.items.erase(cid); continue; }
    sc.item(cid)->anchors = an; made.push_back(cid);
  }
  sc.reindex();
  int w = 0;
  auto wall = [&](int a, int b, double bulge) { int s = edit::makeSurface(sc, a, b); if (s >= 0) { QJsonObject p = sc.item(s)->raw.value("p").toObject(); p.insert("bulge", bulge); sc.item(s)->raw.insert("p", p); w++; } };
  for (size_t i = 0; i + 1 < made.size(); i++) wall(made[i], made[i + 1], i < res.bulge.size() ? res.bulge[i] : 0);
  if (res.closed && made.size() >= 3) wall(made.back(), made.front(), made.size() - 1 < res.bulge.size() ? res.bulge[made.size() - 1] : 0);
  return {int(made.size()), w};
}
}  // namespace

QString meshToCurves(Scene& sc, Selection& sel, int meshId, const M2C& opt) {
  Item* it = sc.item(meshId); if (!it || !it->poly) return QObject::tr("!Select an imported or a converted mesh");
  int tris = 0; for (int f = 0; f < it->poly->faceCount(); f++) tris += it->poly->faceStart[size_t(f) + 1] - it->poly->faceStart[size_t(f)] - 2;
  if (tris >= meshmods::heavyTris()) return QObject::tr("!%1: Over 300 000 triangles: no wireframe and not editable (it can be moved, rotated, scaled)").arg(it->name);
  auto Tp = topoOf(*it->poly); const Topo& T = *Tp; const auto& W = it->poly->pos;
  auto fn = faceNormals(T, *it->poly); auto comps = meshComponents(T);
  // points on the head (for "the root — the end nearest the head")
  std::vector<Vec3> hp;
  if (opt.root == "head") for (auto& [id, h] : sc.items) if (h.headTag && h.poly) { size_t st = std::max<size_t>(1, h.poly->pos.size() / 15000); for (size_t i = 0; i < h.poly->pos.size(); i += st) hp.push_back(h.poly->pos[i]); }
  QString mname = it->name;
  int g = newGroup(sc, mname + " → curves");
  int nq = 0, ns = 0, nc = 0, nw = 0, skip = 0; double step = projectStep(sc);
  for (size_t ci = 0; ci < comps.size(); ci++) {
    const auto& E = comps[ci]; if (E.size() < 3) continue;
    auto F = loopFamilies(T, W, E);
    std::vector<LockRes> list; bool quad = false;
    if (F) { list = lockTree(T, W, *F, opt, fn, int(ci), opt.root == "head" ? &hp : nullptr); if (list.empty()) if (auto r = lockFromQuads(T, W, *F, opt, fn)) list.push_back(*r); quad = !list.empty(); }
    if (list.empty()) if (auto r = lockFromSlices(T, W, E, opt, int(ci), step)) list.push_back(*r);
    list.erase(std::remove_if(list.begin(), list.end(), [](const LockRes& r) { return r.lines.size() < 2; }), list.end());
    if (list.empty()) { skip++; continue; }
    std::map<QString, int> byKey;
    for (auto& r : list) { auto [c, w] = buildLockCurves(sc, g, r, byKey); nc += c; nw += w; if (quad) nq++; else ns++; }
  }
  if (!nc) { sc.removeFromParent(g); sc.items.erase(g); return QObject::tr("!No locks found in this mesh"); }
  sc.item(g)->expanded = false; sel.anchors.clear(); sel.items = {g};
  sc.reindex();
  return QObject::tr("%1: %2 lock%3 (%4 by edge loops%5), %6 curves, %7 walls%8").arg(sc.item(g)->name).arg(nq + ns).arg(nq + ns == 1 ? "" : "s").arg(nq)
      .arg(ns ? QObject::tr(", %1 by slices").arg(ns) : QString()).arg(nc).arg(nw).arg(skip ? QObject::tr(" · %1 part%2 skipped").arg(skip).arg(skip == 1 ? "" : "s") : QString());
}

}  // namespace hm::meshc
