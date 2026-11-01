// The Topology room's selection walks and keepers: Loop / Ring (an edge loop, an edge ring, a polygon loop / ring, the
// vertices along them — stopping before a picked element, an n-gon, a triangle when asked), the polygon loop's direction
// from where in the polygon the pointer is, the selection's mirror (Symmetry on: every pick mirrored too), the
// selection sets kept with a mesh, and the Soft Selection (the vertices near the selection moved less, the further the
// less).
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <cmath>
#include <functional>
#include <queue>
#include <set>
#include <unordered_set>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
// the segment a–b's distance from p (on the screen)
double segDist(double px, double py, double ax, double ay, double bx, double by) {
  double dx = bx - ax, dy = by - ay, L = dx * dx + dy * dy, t = L > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / L, 0.0, 1.0) : 0;
  return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}
}  // namespace

// the edge of polygon f a loop crosses: a quad — the pair of opposite sides the pointer is relatively nearer to (about
// as near to both — the long way round: across the pair further apart, so a thin strip is walked along); else the
// nearest side
int Viewport::loopEdgeAt(int m, int f, const QPointF& mp) const {
  const Item* it = scene_->item(m); if (!it || f < 0) return -1;
  auto T = topo::ttopo(baseSp(*it)); if (f >= T->nf) return -1;
  const auto W = tWorld(m); Projector P = projector();
  const int s = T->fo[size_t(f)], n = T->faceSize(f);
  std::vector<double> d(static_cast<size_t>(n));
  for (int k = 0; k < n; k++) { ScreenPt a = P.toScreen(W[size_t(T->fv[size_t(s + k)])]), b = P.toScreen(W[size_t(T->fv[size_t(s + (k + 1) % n)])]); d[size_t(k)] = segDist(mp.x(), mp.y(), a.x, a.y, b.x, b.y); }
  int best = 0; for (int k = 1; k < n; k++) if (d[size_t(k)] < d[size_t(best)]) best = k;
  if (n == 4) {
    const double wa = d[0] + d[2], wb = d[1] + d[3], ra = std::min(d[0], d[2]) / std::max(1e-9, wa), rb = std::min(d[1], d[3]) / std::max(1e-9, wb);
    const bool pairA = std::abs(ra - rb) < 0.12 ? wa > wb : ra < rb;
    best = pairA ? (d[0] <= d[2] ? 0 : 2) : (d[1] <= d[3] ? 1 : 3);
  }
  return T->ce[size_t(s + best)];
}

// Loop / Ring from the element under the pointer, in the mode's elements
std::vector<int> Viewport::tLoopOf(int m, const TopoPick& r, const QPointF& mp, bool ring) const {
  const Item* it = scene_->item(m); if (!it) return {};
  auto Tp = topo::ttopo(baseSp(*it)); const auto& T = *Tp; const char key = tKey();
  const bool mine = tsel_.id == m;
  auto badF = [&](int f) { const int n = T.faceSize(f); return (tset.loopStopTri && n == 3) || (tset.loopStopNgon && n > 4); };
  auto other = [&](int e, int f) { return T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; };
  auto opp = [&](int f, int e) { const int s = T.fo[size_t(f)], n = T.faceSize(f); if (n % 2) return -1;   // (the side across: quads, even n-gons)
    for (int k = 0; k < n; k++) if (T.ce[size_t(s + k)] == e) return T.ce[size_t(s + (k + n / 2) % n)]; return -1; };
  // ---- an edge loop ----
  auto edgeLoop = [&](int e0) {
    std::vector<int> out{e0}; std::set<int> seen{e0};
    for (int v0 : {T.ea[size_t(e0)], T.eb[size_t(e0)]}) {
      int e = e0, v = v0;
      for (;;) {
        if (tset.loopStopSel && mine && key == 'v' && tsel_.v.count(v) && e != e0) break;
        bool stop = false; for (int i = T.vfo[size_t(v)]; i < T.vfo[size_t(v) + 1]; i++) if (badF(T.vF[size_t(i)])) stop = true;
        if (stop) break;
        const int s = T.voff[size_t(v)], t = T.voff[size_t(v) + 1]; int nx = -1, c = 0;
        if (T.ef1[size_t(e)] < 0) { for (int j = s; j < t; j++) { int x = T.vE[size_t(j)]; if (x != e && T.ef1[size_t(x)] < 0) { nx = x; c++; } } }   // (along the rim)
        else if (t - s == 4) for (int j = s; j < t; j++) { int x = T.vE[size_t(j)]; if (x == e) continue;
          bool share = false; for (int f : {T.ef0[size_t(x)], T.ef1[size_t(x)]}) if (f >= 0 && (f == T.ef0[size_t(e)] || f == T.ef1[size_t(e)])) share = true;
          if (!share) { nx = x; c++; } }
        if (c != 1 || seen.count(nx)) break;
        if (tset.loopStopSel && mine && key == 'e' && tsel_.e.count(nx)) break;
        seen.insert(nx); out.push_back(nx); v = T.other(nx, v); e = nx;
      }
    }
    return out;
  };
  // ---- an edge ring: across each polygon to its opposite side ----
  auto edgeRing = [&](int e0) {
    std::vector<int> out{e0}; std::set<int> seenE{e0}, seenF;
    for (int f0 : {T.ef0[size_t(e0)], T.ef1[size_t(e0)]}) {
      int f = f0, e = e0;
      while (f >= 0 && !seenF.count(f)) {
        seenF.insert(f); if (badF(f)) break;
        const int nx = opp(f, e); if (nx < 0 || seenE.count(nx)) break;
        if (tset.loopStopSel && mine && key == 'e' && tsel_.e.count(nx)) break;
        seenE.insert(nx); out.push_back(nx); f = other(nx, f); e = nx;
      }
    }
    return out;
  };
  // ---- a polygon loop across e0 ----
  auto faceLoop = [&](int f0, int e0) {
    std::vector<int> out{f0}; std::set<int> seen{f0};
    for (int start : {e0, opp(f0, e0)}) {
      int f = f0, e = start;
      while (e >= 0) {
        const int g = other(e, f); if (g < 0 || seen.count(g)) break;
        if (tset.loopStopSel && mine && key == 'f' && tsel_.f.count(g)) break;
        if (badF(g)) break;
        seen.insert(g); out.push_back(g);
        const int ne = opp(g, e); f = g; e = ne;              // (a triangle / an odd n-gon: taken, the loop ends in it)
      }
    }
    return out;
  };
  auto vertsOf = [&](const std::vector<int>& es) { std::set<int> vs; for (int e : es) { vs.insert(T.ea[size_t(e)]); vs.insert(T.eb[size_t(e)]); } return std::vector<int>(vs.begin(), vs.end()); };
  if (key == 'f') {
    if (r.f < 0) return {};
    int e = loopEdgeAt(m, r.f, mp); if (e < 0) return {r.f};
    if (ring) { const int s = T.fo[size_t(r.f)], n = T.faceSize(r.f); for (int k = 0; k < n; k++) if (T.ce[size_t(s + k)] == e) { e = T.ce[size_t(s + (k + 1) % n)]; break; } }   // (the other way)
    return faceLoop(r.f, e);
  }
  if (r.e < 0) return {};
  if (key == 'e') return ring ? edgeRing(r.e) : edgeLoop(r.e);
  return vertsOf(ring ? edgeRing(r.e) : edgeLoop(r.e));
}

// a click on a, a double click on b: the walk from a that reaches b (a polygon loop; an edge loop or ring; the vertices
// along an edge loop) — then every so many along it, as far apart as a and b, both ways (a closed loop: round once);
// it stops before a picked element (stop) or where the walk can't go on (a pole, an n-gon, the rim) → empty: b not on one
std::vector<int> Viewport::tPatternWalk(int m, char key, int a, int b, const std::set<int>& stop) const {
  const Item* it = scene_->item(m); if (!it) return {};
  auto Tp = topo::ttopo(baseSp(*it)); const auto& T = *Tp;
  auto other = [&](int e, int f) { return T.ef0[size_t(e)] == f ? T.ef1[size_t(e)] : T.ef0[size_t(e)]; };
  auto opp = [&](int f, int e) { const int s = T.fo[size_t(f)], n = T.faceSize(f); if (n != 4) return -1;
    for (int k = 0; k < n; k++) if (T.ce[size_t(s + k)] == e) return T.ce[size_t(s + (k + 2) % n)]; return -1; };
  auto shareFace = [&](int x, int y) { for (int f : {T.ef0[size_t(x)], T.ef1[size_t(x)]}) if (f >= 0 && (f == T.ef0[size_t(y)] || f == T.ef1[size_t(y)])) return true; return false; };
  // (the edge after e at v along a loop: a valence-4 vertex — the one not beside e; the rim — the next rim edge)
  auto loopNext = [&](int e, int v) {
    const int s = T.voff[size_t(v)], t = T.voff[size_t(v) + 1]; int nx = -1, c = 0;
    if (T.ef1[size_t(e)] < 0) { for (int j = s; j < t; j++) { int x = T.vE[size_t(j)]; if (x != e && T.ef1[size_t(x)] < 0) { nx = x; c++; } } }
    else if (t - s == 4) for (int j = s; j < t; j++) { int x = T.vE[size_t(j)]; if (x != e && !shareFace(x, e)) { nx = x; c++; } }
    return c == 1 ? nx : -1;
  };
  struct Walk { std::vector<int> seq; bool closed = false; };
  using Dir = std::function<Walk()>;
  std::vector<std::pair<Dir, Dir>> dirs;                         // (a direction and the one opposite it)
  if (key == 'f') {
    if (T.faceSize(a) != 4) return {};
    auto walkF = [&, a](int e0) { return [&, a, e0] { Walk w; int f = a, e = e0;
      while (e >= 0) { const int g = other(e, f); if (g < 0) break; if (g == a) { w.closed = true; break; }
        if (stop.count(g) || T.faceSize(g) != 4) break; w.seq.push_back(g); e = opp(g, e); f = g; }
      return w; }; };
    for (int k = T.fo[size_t(a)]; k < T.fo[size_t(a) + 1]; k++) { const int e = T.ce[size_t(k)]; dirs.push_back({walkF(e), walkF(opp(a, e))}); }
  } else if (key == 'e') {
    auto walkL = [&, a](int v0) { return [&, a, v0] { Walk w; int e = a, v = v0;
      for (;;) { const int nx = loopNext(e, v); if (nx < 0) break; if (nx == a) { w.closed = true; break; } if (stop.count(nx)) break;
        w.seq.push_back(nx); v = T.other(nx, v); e = nx; }
      return w; }; };
    auto walkR = [&, a](int f0) { return [&, a, f0] { Walk w; int f = f0, e = a;
      while (f >= 0) { const int nx = opp(f, e); if (nx < 0) break; if (nx == a) { w.closed = true; break; } if (stop.count(nx)) break;
        w.seq.push_back(nx); f = other(nx, f); e = nx; }
      return w; }; };
    dirs.push_back({walkL(T.ea[size_t(a)]), walkL(T.eb[size_t(a)])}); dirs.push_back({walkL(T.eb[size_t(a)]), walkL(T.ea[size_t(a)])});
    dirs.push_back({walkR(T.ef0[size_t(a)]), walkR(T.ef1[size_t(a)])}); dirs.push_back({walkR(T.ef1[size_t(a)]), walkR(T.ef0[size_t(a)])});
  } else {
    auto walkV = [&, a](int x0) { return [&, a, x0] { Walk w; if (x0 < 0) return w; int e = x0, v = T.other(x0, a);
      for (;;) { if (v == a) { w.closed = true; break; } if (stop.count(v)) break; w.seq.push_back(v);
        const int nx = loopNext(e, v); if (nx < 0) break; v = T.other(nx, v); e = nx; }
      return w; }; };
    for (int j = T.voff[size_t(a)]; j < T.voff[size_t(a) + 1]; j++) { const int x = T.vE[size_t(j)]; dirs.push_back({walkV(x), walkV(loopNext(x, a))}); }
  }
  for (auto& [fw, bw] : dirs) {
    Walk w = fw(); auto at = std::find(w.seq.begin(), w.seq.end(), b); if (at == w.seq.end()) continue;
    const size_t step = size_t(at - w.seq.begin()) + 1; std::vector<int> out{a};
    for (size_t k = step - 1; k < w.seq.size(); k += step) out.push_back(w.seq[k]);
    if (!w.closed) { Walk o = bw(); for (size_t k = step - 1; k < o.seq.size(); k += step) out.push_back(o.seq[k]); }
    return out;
  }
  return {};
}

// the elements' mirrors across the mesh's X = 0 (those with one)
std::vector<int> Viewport::tSymList(int m, const std::vector<int>& list, char key) const {
  ProfScope prof_("tSymList");
  const Item* it = scene_->item(m); if (!it) return {};
  auto Bs = baseSp(*it); auto T = topo::ttopo(Bs);
  if (symC_.B != Bs.get()) { symC_ = {}; symC_.B = Bs.get(); }
  symC_.pairs = symFirstPairs(m, Bs->pos);   // (the settings' first mirror — kept by the symmetry's own cache)
  const auto& pr = symC_.pairs; auto mv = [&](int v) { return size_t(v) < pr.size() ? pr[size_t(v)] : -1; };
  std::vector<int> out;
  if (key == 'v') { for (int v : list) if (int w = mv(v); w >= 0 && w != v) out.push_back(w); return out; }
  if (key == 'e') { for (int e : list) if (e < T->ne) { int a = mv(T->ea[size_t(e)]), b = mv(T->eb[size_t(e)]); if (a >= 0 && b >= 0) if (int x = T->edgeOf(a, b); x >= 0 && x != e) out.push_back(x); } return out; }
  auto hashOf = [](std::vector<int>& vs) { std::sort(vs.begin(), vs.end()); quint64 h = 1469598103934665603ull; for (int v : vs) h = (h ^ quint64(unsigned(v))) * 1099511628211ull; return h; };
  auto cornersOf = [&](int f) { std::vector<int> vs; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) vs.push_back(T->fv[size_t(k)]); std::sort(vs.begin(), vs.end()); return vs; };
  if (symC_.faces.empty()) { symC_.faces.reserve(size_t(T->nf)); for (int f = 0; f < T->nf; f++) { auto vs = cornersOf(f); symC_.faces.emplace(hashOf(vs), f); } }
  for (int f : list) if (f < T->nf) { std::vector<int> vs; bool ok = true;
    for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1] && ok; k++) { int w = mv(T->fv[size_t(k)]); ok = w >= 0; vs.push_back(w); }
    if (!ok) continue; auto [a, b] = symC_.faces.equal_range(hashOf(vs));
    for (auto g = a; g != b; ++g) if (g->second != f && cornersOf(g->second) == vs) { out.push_back(g->second); break; } }
  return out;
}

// ---------- selection sets ----------
QString Viewport::tSelSetCreate() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it || topoMode == "object" || tsel_.id != m) { ui::toast(tr("Select polygons, edges or vertices first")); return {}; }
  const char key = tKey(); const std::set<int>& S = key == 'f' ? tsel_.f : key == 'e' ? tsel_.e : tsel_.v;
  if (S.empty()) { ui::toast(tr("Nothing selected to keep")); return {}; }
  auto T = topo::ttopo(baseSp(*it)); const int N = key == 'f' ? T->nf : key == 'e' ? T->ne : T->n;
  std::vector<char> a(size_t(N), 0); for (int x : S) if (x < N) a[size_t(x)] = 1;
  QJsonArray sets = it->raw.value("selSets").toArray(); int k = 1; for (const QJsonValue& s : sets) if (s.toObject().value("k").toString() == QString(QChar(key))) k++;
  const QString kind = key == 'f' ? tr("Polygons") : key == 'e' ? tr("Edges") : tr("Vertices"), name = QString("%1 %2").arg(kind).arg(k);
  sets.append(QJsonObject{{"k", QString(QChar(key))}, {"n", N}, {"d", topo::smEnc(a)}, {"name", name}});
  it->raw.insert("selSets", sets); scene_->revision++; commit(false); emit topoState();
  ui::toast(tr("Selection set \"%1\" kept: %2 — in the mesh's settings, Selection sets").arg(name).arg(S.size()));
  return name;
}
std::vector<int> Viewport::tSelSetList(int m, int idx, char* key) const {
  const Item* it = scene_->item(m); if (!it) return {};
  const QJsonArray sets = it->raw.value("selSets").toArray(); if (idx < 0 || idx >= sets.size()) return {};
  const QJsonObject s = sets[idx].toObject(); const QString k = s.value("k").toString(); const char kc = k.isEmpty() ? 'f' : k[0].toLatin1(); if (key) *key = kc;
  auto T = topo::ttopo(baseSp(*it)); const int N = kc == 'f' ? T->nf : kc == 'e' ? T->ne : T->n;
  if (s.value("n").toInt() != N) return {};                     // (made for other geometry)
  auto a = topo::smDec(s.value("d").toString(), N); std::vector<int> out; for (size_t i = 0; i < a.size(); i++) if (a[i]) out.push_back(int(i));
  return out;
}
void Viewport::tSelSetApply(int m, int idx, const QString& op) {
  char key = 0; auto list = tSelSetList(m, idx, &key);
  if (list.empty()) { ui::toast(tr("This set was made for other geometry (the mesh's polygons changed since)")); return; }
  if (sel_.items.size() != 1 || sel_.items[0] != m) { sel_ = {}; sel_.items = {m}; onSelChange(); }
  const QString mode = key == 'f' ? "poly" : key == 'e' ? "edge" : "vert";
  if (topoMode != mode) setTopoMode(mode, true);
  tselApply(m, list, op, key);
}
bool Viewport::tSelSetTouched(int m, int idx) const {
  if (tsel_.id != m || topoMode == "object") return false;
  char key = 0; const auto list = tSelSetList(m, idx, &key); if (list.empty() || key != tKey()) return false;
  const std::set<int>& S = key == 'f' ? tsel_.f : key == 'e' ? tsel_.e : tsel_.v; if (S.empty()) return false;
  for (int x : list) if (S.count(x)) return true;
  return false;
}
void Viewport::tSelSetDelete(int m, int idx) {
  Item* it = scene_->item(m); if (!it) return;
  QJsonArray sets = it->raw.value("selSets").toArray(); if (idx < 0 || idx >= sets.size()) return;
  sets.removeAt(idx); if (sets.isEmpty()) it->raw.remove("selSets"); else it->raw.insert("selSets", sets);
  ssHover = {}; scene_->revision++; commit(false); emit topoState();
}

// ---------- Soft Selection ----------
// the vertices near the selection (surface — joined to it, the distance along the edges; volume — any of the mesh within
// the radius), each with the selected vertex it follows and how much (1 at the selection, 0 at the radius — smoothly)
const Viewport::SoftW& Viewport::softOf(int m) const {
  ProfScope prof_("softOf");
  const Item* it = scene_->item(m); const PolyMesh* B = it ? baseOf(*it) : nullptr;
  // (made once per selection and shape of the mesh — not again on every step of a drag, whose live copy is another mesh)
  const qint64 nv = B ? qint64(B->pos.size()) : -1, nf = B ? B->faceCount() : -1;
  if (softW_.m == m && softW_.ver == tsel_.ver && softW_.nv == nv && softW_.nf == nf && softW_.R == tset.softR && softW_.mode == tset.softMode) return softW_;
  softW_ = {}; softW_.m = m; softW_.ver = tsel_.ver; softW_.B = B; softW_.nv = nv; softW_.nf = nf; softW_.R = tset.softR; softW_.mode = tset.softMode;
  if (!it || !tset.soft) return softW_;
  const auto S = tSelVerts(m); if (S.empty()) return softW_;
  const auto W = tWorld(m); const double R = std::max(1e-6, tset.softR); const int N = int(W.size());
  std::vector<double> dist(size_t(N), 1e300); std::vector<int> src(size_t(N), -1);
  std::vector<char> sel(size_t(N), 0); for (int v : S) if (v < N) { sel[size_t(v)] = 1; dist[size_t(v)] = 0; src[size_t(v)] = v; }
  if (tset.softMode == "volume") {
    // (one selected vertex per cell of an eighth of the radius stands for it — the falloff needs no finer distance —
    // in a k-d tree: each vertex near the selection finds the nearest in a few steps)
    Box3 bx; for (int v : S) bx.expand(W[size_t(v)]);
    const double h = R / 8; const long long o = 1LL << 20;
    std::unordered_map<long long, int> cellRep; cellRep.reserve(S.size());
    for (int s : S) { const Vec3& q = W[size_t(s)]; cellRep.emplace((((long long)std::floor(q.x / h) + o) & 0x1FFFFF) << 42 | (((long long)std::floor(q.y / h) + o) & 0x1FFFFF) << 21 | (((long long)std::floor(q.z / h) + o) & 0x1FFFFF), s); }
    std::vector<int> rep; rep.reserve(cellRep.size()); for (auto& [k, s] : cellRep) rep.push_back(s);
    // (the tree: the array split at its median along x, y, z in turn — the node is the middle element)
    std::function<void(int, int, int)> build = [&](int lo, int hi, int ax) {
      if (hi - lo <= 1) return; const int mid = (lo + hi) / 2;
      std::nth_element(rep.begin() + lo, rep.begin() + mid, rep.begin() + hi, [&](int a, int b) { const Vec3& A = W[size_t(a)]; const Vec3& B2 = W[size_t(b)]; return ax == 0 ? A.x < B2.x : ax == 1 ? A.y < B2.y : A.z < B2.z; });
      build(lo, mid, (ax + 1) % 3); build(mid + 1, hi, (ax + 1) % 3); };
    build(0, int(rep.size()), 0);
    std::function<void(int, int, int, const Vec3&, double&, int&)> near = [&](int lo, int hi, int ax, const Vec3& p, double& bd, int& bs) {
      if (hi <= lo) return; const int mid = (lo + hi) / 2; const Vec3& q = W[size_t(rep[size_t(mid)])];
      const double d = p.distanceTo(q); if (d < bd) { bd = d; bs = rep[size_t(mid)]; }
      const double diff = ax == 0 ? p.x - q.x : ax == 1 ? p.y - q.y : p.z - q.z;
      if (diff < 0) { near(lo, mid, (ax + 1) % 3, p, bd, bs); if (-diff < bd) near(mid + 1, hi, (ax + 1) % 3, p, bd, bs); }
      else { near(mid + 1, hi, (ax + 1) % 3, p, bd, bs); if (diff < bd) near(lo, mid, (ax + 1) % 3, p, bd, bs); } };
    for (int v = 0; v < N; v++) { if (sel[size_t(v)]) continue; const Vec3& p = W[size_t(v)];
      if (p.x < bx.min.x - R || p.y < bx.min.y - R || p.z < bx.min.z - R || p.x > bx.max.x + R || p.y > bx.max.y + R || p.z > bx.max.z + R) continue;
      double bd = R; int bs = -1; near(0, int(rep.size()), 0, p, bd, bs);   // (nothing nearer than the radius: left)
      if (bs >= 0) { dist[size_t(v)] = bd; src[size_t(v)] = bs; } }
  } else {
    auto T = topo::ttopo(baseSp(*it));
    using QE = std::pair<double, int>; std::priority_queue<QE, std::vector<QE>, std::greater<QE>> heap;
    // (from the selection's edge only: a vertex inside it has no unselected neighbour to reach)
    for (int v : S) { if (v >= T->n) continue; bool rim = false;
      for (int j = T->voff[size_t(v)]; j < T->voff[size_t(v) + 1] && !rim; j++) rim = !sel[size_t(T->other(T->vE[size_t(j)], v))];
      if (rim) heap.push({0, v}); }
    while (!heap.empty()) {
      auto [d, x] = heap.top(); heap.pop(); if (d > dist[size_t(x)]) continue;
      for (int j = T->voff[size_t(x)]; j < T->voff[size_t(x) + 1]; j++) { int y = T->other(T->vE[size_t(j)], x); double nd = d + W[size_t(x)].distanceTo(W[size_t(y)]);
        if (nd < R && nd < dist[size_t(y)]) { dist[size_t(y)] = nd; src[size_t(y)] = src[size_t(x)]; heap.push({nd, y}); } }
    }
  }
  for (int v = 0; v < N; v++) if (!sel[size_t(v)] && dist[size_t(v)] < R && src[size_t(v)] >= 0) {
    double t = 1 - dist[size_t(v)] / R; softW_.vs.push_back(v); softW_.near.push_back(src[size_t(v)]); softW_.w.push_back(t * t * (3 - 2 * t)); }
  return softW_;
}
void Viewport::softKey(bool down) {
  if (room() != "topo") return;
  if (down) { softAdj_ = {}; softAdj_.on = true; softAdj_.r0 = tset.softR; return; }
  if (!softAdj_.on) return;
  const bool moved = softAdj_.moved; softAdj_ = {};
  if (!moved) { tset.soft = !tset.soft; ui::hud(tset.soft ? tr("Soft Selection on — radius %1 cm (hold B and move the pointer sideways to change it)").arg(tset.softR, 0, 'g', 3) : tr("Soft Selection off"), 1600); }
  saveTopoPrefs(); topoRev_++; emit topoState(); requestUpdate();
}

}  // namespace hm
