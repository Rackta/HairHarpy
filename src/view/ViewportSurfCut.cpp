// ✂ Scissors, NURBS surface mode. A click on a wall picks it (from then on only that wall and its two curves count). The
// cut is placed like a curve: the first click on one of the wall's curves starts it — on an anchor there (within 10 px)
// or at a new anchor put in keeping the curve's shape; clicks on the wall add anchors (they stick to it); a click on one
// of the wall's curves ends it. Then: drag its anchors (the ones in between over the wall, the ends along the wall's
// curves), RMB on the cut inserts an anchor, double click deletes one. Enter — cut: a new curve along the cut (it runs
// along the first curve's anchors up to the start, crosses the wall and goes on along the second curve to its end) and
// the wall becomes two, (A, cut) and (cut, B). The cut never leaves the wall: its anchors are kept in the wall's own
// coordinates (r — along its rows, t — across) and the path between them runs in those coordinates.
// Begun on the wall itself (or on its top / bottom edge) instead of on a curve: the cut runs along the wall from edge to
// edge — clicks put its anchors; an end not brought to an edge is carried there (straight on, as the cut goes); a click
// on the far edge, Enter or Shift+RMB — cut: a new curve along it, the wall split in two beside it. On a wall growing out
// of a crown the new curve's root goes onto the crown's lower connector.
#include "scene/CurveMath.h"
#include "scene/Crown.h"
#include "scene/Edit.h"
#include "scene/Network.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QMouseEvent>
#include <cmath>
#include <variant>

namespace hm {

namespace {
const int VSTART = -100, VEND = -101;                                  // (the ends not put in yet, in the anchor lists)
int idxOf(const std::vector<int>& v, int x) { auto it = std::find(v.begin(), v.end(), x); return it == v.end() ? -1 : int(it - v.begin()); }

// the wall as a grid of points: R rows × M across (t = 0 — curve a, t = M − 1 — curve b); dr / dt — mean spacing
struct Grid {
  int R = 0, M = 0; const std::vector<Vec3>* pos = nullptr; int base = 0; double dr = 1e-3, dt = 1e-3, width = 1e-3;
  const Vec3& at(int k, int j) const { return (*pos)[size_t(base + k * M + j)]; }
};
std::optional<Grid> wallGrid(const HairState& hs, int sf) {
  auto it = hs.walls.find(sf); if (it == hs.walls.end() || it->second.grids.empty()) return std::nullopt;
  const auto& g = it->second.grids[0]; if (g.R < 2 || g.M < 2) return std::nullopt;
  Grid G; G.R = g.R; G.M = g.M; G.pos = &it->second.pos; G.base = g.start;
  double dr = 0, w = 0;
  for (int k = 0; k < G.R - 1; k++) dr += ((G.at(k, 0) - G.at(k + 1, 0)).length() + (G.at(k, G.M - 1) - G.at(k + 1, G.M - 1)).length()) / 2;
  for (int k = 0; k < G.R; k++) w += (G.at(k, 0) - G.at(k, G.M - 1)).length();
  dr /= std::max(1, G.R - 1); w /= G.R;
  G.dr = dr ? dr : 1e-3; G.width = w ? w : 1e-3; G.dt = G.width / (G.M - 1);
  return G;
}
Vec3 gridAt(const Grid& G, double r, double t) {
  int k = std::clamp(int(std::floor(r)), 0, G.R - 2), j = std::clamp(int(std::floor(t)), 0, G.M - 2); double a = std::clamp(r - k, 0.0, 1.0), b = std::clamp(t - j, 0.0, 1.0);
  return G.at(k, j).lerp(G.at(k, j + 1), b).lerp(G.at(k + 1, j).lerp(G.at(k + 1, j + 1), b), a);
}
// the closest point of triangle abc to p and its barycentric coordinates (Ericson)
Vec3 closestOnTri(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, double& u, double& v, double& w) {
  Vec3 ab = b - a, ac = c - a, ap = p - a; double d1 = ab.dot(ap), d2 = ac.dot(ap);
  if (d1 <= 0 && d2 <= 0) { u = 1; v = 0; w = 0; return a; }
  Vec3 bp = p - b; double d3 = ab.dot(bp), d4 = ac.dot(bp);
  if (d3 >= 0 && d4 <= d3) { u = 0; v = 1; w = 0; return b; }
  double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) { double t = d1 / (d1 - d3); u = 1 - t; v = t; w = 0; return a + ab * t; }
  Vec3 cp = p - c; double d5 = ab.dot(cp), d6 = ac.dot(cp);
  if (d6 >= 0 && d5 <= d6) { u = 0; v = 0; w = 1; return c; }
  double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) { double t = d2 / (d2 - d6); u = 1 - t; v = 0; w = t; return a + ac * t; }
  double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { double t = (d4 - d3) / ((d4 - d3) + (d5 - d6)); u = 0; v = 1 - t; w = t; return b + (c - b) * t; }
  double den = 1 / (va + vb + vc); v = vb * den; w = vc * den; u = 1 - v - w; return a + ab * v + ac * w;
}
// the wall coordinates of the point of the wall nearest to p
struct Q { double r = 0, t = 0; };
std::optional<Q> gridParam(const Grid& G, const Vec3& p) {
  std::optional<Q> best; double bd = 1e300;
  for (int k = 0; k < G.R - 1; k++) for (int j = 0; j < G.M - 1; j++) {
    int C[4][2] = {{k, j}, {k, j + 1}, {k + 1, j + 1}, {k + 1, j}};
    for (auto tri : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
      const Vec3 &a = G.at(C[tri[0]][0], C[tri[0]][1]), &b = G.at(C[tri[1]][0], C[tri[1]][1]), &c = G.at(C[tri[2]][0], C[tri[2]][1]);
      if ((b - a).cross(c - a).length() / 2 < 1e-12) continue;
      double u, v, w; Vec3 q = closestOnTri(p, a, b, c, u, v, w); double d = (q - p).lengthSq(); if (d >= bd) continue;
      bd = d; best = Q{u * C[tri[0]][0] + v * C[tri[1]][0] + w * C[tri[2]][0], u * C[tri[0]][1] + v * C[tri[1]][1] + w * C[tri[2]][1]};
    }
  }
  return best;
}
double edgeR(const Grid& G, int j, const Vec3& p) {
  double best = 0, bd = 1e300;
  for (int k = 0; k < G.R - 1; k++) { const Vec3& a = G.at(k, j); Vec3 ab = G.at(k + 1, j) - a; double l2 = ab.lengthSq(), s = l2 > 0 ? std::clamp((p - a).dot(ab) / l2, 0.0, 1.0) : 0;
    double d = (a + ab * s - p).lengthSq(); if (d < bd) { bd = d; best = k + s; } }
  return best;
}
// a path through wall coordinates (Catmull-Rom in lengths along / across): dense points, idx — where each control point is
struct WPath { std::vector<Q> dense; std::vector<int> idx; };
WPath wallPath(const Grid& G, const std::vector<Q>& ctl) {
  std::vector<std::array<double, 2>> X; for (auto& c : ctl) X.push_back({c.r * G.dr, c.t * G.dt});
  WPath o;
  auto cr = [](const std::array<double, 2>& p0, const std::array<double, 2>& p1, const std::array<double, 2>& p2, const std::array<double, 2>& p3, double s, int i) {
    return 0.5 * (2 * p1[i] + (p2[i] - p0[i]) * s + (2 * p0[i] - 5 * p1[i] + 4 * p2[i] - p3[i]) * s * s + (3 * p1[i] - p0[i] - 3 * p2[i] + p3[i]) * s * s * s); };
  for (size_t i = 0; i + 1 < X.size(); i++) {
    auto &p0 = X[i ? i - 1 : 0], &p1 = X[i], &p2 = X[i + 1], &p3 = X[std::min(i + 2, X.size() - 1)];
    int n = std::clamp(int(std::ceil(std::hypot(p2[0] - p1[0], p2[1] - p1[1]) / (0.35 * std::min(G.dr, G.dt)))), 4, 400);
    o.idx.push_back(int(o.dense.size()));
    for (int s = 0; s < n; s++) o.dense.push_back({std::clamp(cr(p0, p1, p2, p3, double(s) / n, 0) / G.dr, 0.0, double(G.R - 1)), std::clamp(cr(p0, p1, p2, p3, double(s) / n, 1) / G.dt, 0.0, double(G.M - 1))});
  }
  o.idx.push_back(int(o.dense.size())); o.dense.push_back(ctl.back());
  return o;
}
double distToSeg(const Vec3& p, const Vec3& a, const Vec3& b) { Vec3 ab = b - a; double l2 = ab.lengthSq(), t = l2 > 0 ? std::clamp((p - a).dot(ab) / l2, 0.0, 1.0) : 0; return (a + ab * t - p).length(); }
}  // namespace

// ---- the plan of the cut ----
namespace {
struct Frame { std::vector<int> OA, OB, pre, post; int pL = 0, pi = 0, qL = 0, qi = 0; bool swapped = false; };
struct Build { Frame F; std::vector<Vec3> mids; std::vector<Q> mp; std::vector<Vec3> d3; std::vector<int> at, idx; bool swapped = false;
               std::vector<int> anchors; std::map<int, std::pair<std::optional<Vec3>, std::optional<Vec3>>> h; };   // (h: the handles as offsets — in / out)
struct Ctx {
  const Scene& sc; const Viewport::SurfCutState& P; const Grid& G;
  Vec3 pos(int id) const { if (id == VSTART) return P.start->pos; if (id == VEND) return P.end->pos; if (id <= -1000) return mids[size_t(-1000 - id)]; return sc.anchor(id)->pos; }
  double smooth(int id) const { return id < 0 ? 1 : sc.anchor(id)->smooth; }
  std::vector<Vec3> mids;
};
bool curveRev(const Scene& sc, const Item& A, const Item& B) {
  auto p = [&](int a) { return sc.anchor(a)->pos; };
  const auto &a = A.anchors, &b = B.anchors;
  return (p(a.front()) - p(b.front())).length() + (p(a.back()) - p(b.back())).length() > (p(a.front()) - p(b.back())).length() + (p(a.back()) - p(b.front())).length();
}
std::variant<Frame, QString> cutFrame(const Scene& sc, const Viewport::SurfCutState& P) {
  const Item *A = sc.item(P.A), *B = sc.item(P.B); Frame F; F.OA = A->anchors; std::vector<int> LB = B->anchors;
  struct Ins { int side, seg; double u; int id; }; std::vector<Ins> ins;
  for (auto [E, id] : {std::pair{&P.start, VSTART}, std::pair{&P.end, VEND}}) if (*E && (*E)->a < 0) ins.push_back({(*E)->side, (*E)->seg, (*E)->u, id});
  std::stable_sort(ins.begin(), ins.end(), [](const Ins& x, const Ins& y) { return x.seg != y.seg ? x.seg > y.seg : x.u > y.u; });
  for (auto& i : ins) { auto& L = i.side ? LB : F.OA; L.insert(L.begin() + i.seg + 1, i.id); }
  F.OB = LB; if (curveRev(sc, *A, *B)) std::reverse(F.OB.begin(), F.OB.end());
  int sa = P.start->a >= 0 ? P.start->a : VSTART, ea = P.end->a >= 0 ? P.end->a : VEND;
  if (sa == ea) return QObject::tr("Start and end the cut at two different places");
  auto where = [&](int a, int& L, int& i) { if ((i = idxOf(F.OA, a)) >= 0) { L = 0; return true; } if ((i = idxOf(F.OB, a)) >= 0) { L = 1; return true; } return false; };
  if (!where(sa, F.pL, F.pi) || !where(ea, F.qL, F.qi)) return QObject::tr("The cut must start and end on the wall's curves");
  auto Ls = [&](int L) -> const std::vector<int>& { return L ? F.OB : F.OA; };
  auto u = [&](int L, int i) { return double(i) / (Ls(L).size() - 1); };
  if (u(F.qL, F.qi) < u(F.pL, F.pi) || (u(F.qL, F.qi) == u(F.pL, F.pi) && F.qi < F.pi)) { std::swap(F.pL, F.qL); std::swap(F.pi, F.qi); F.swapped = true; }
  QString bad = QObject::tr("The cut must run along the wall: from a place nearer the root to one nearer the tip");
  if ((F.pL == F.qL && F.qi <= F.pi) || u(F.qL, F.qi) - u(F.pL, F.pi) < (F.pL == F.qL ? 1e-6 : -1e-9)) return bad;   // (straight across — fine)
  F.pre.assign(Ls(F.pL).begin(), Ls(F.pL).begin() + F.pi + 1); F.post.assign(Ls(F.qL).begin() + F.qi, Ls(F.qL).end());
  for (int x : F.post) if (idxOf(F.pre, x) >= 0) return bad;
  return F;
}
// the handle of host curve h at its anchor a facing its neighbour nb (a not put in yet — its own handles)
std::optional<Vec3> handleToward(const Scene& sc, const Viewport::SurfCutState& P, int h, int a, int nb) {
  if (a < 0) { const auto& E = a == VSTART ? *P.start : *P.end; auto f = E.vh.find(nb); if (f == E.vh.end()) return std::nullopt; return f->second; }
  const Item* c = sc.item(h); int i = idxOf(c->anchors, a); if (i < 0) return std::nullopt;
  Handles H = curveHandles(sc, *c, i); return i > 0 && c->anchors[size_t(i - 1)] == nb ? H.hin : H.hout;
}
// handles making the cubic from P0 (path position x0) to P3 (x1) follow the path d3 between them
std::pair<Vec3, Vec3> pathHandles(const std::vector<Vec3>& d3, int x0, int x1, const Vec3& P0, const Vec3& P3) {
  auto tan = [&](int x) -> std::optional<Vec3> { int i = std::clamp(x, 0, int(d3.size()) - 1); Vec3 t = d3[size_t(std::min(i + 1, int(d3.size()) - 1))] - d3[size_t(std::max(i - 1, 0))]; if (t.lengthSq() <= 1e-16) return std::nullopt; return t.normalized(); };
  double ch = (P0 - P3).length(); auto t0 = tan(x0), t1 = tan(x1);
  if (!t0 || !t1) return {(P3 - P0) * (1.0 / 3), (P0 - P3) * (1.0 / 3)};
  std::vector<Vec3> pts{P0}; for (int i = x0 + 1; i < x1; i++) pts.push_back(d3[size_t(i)]); pts.push_back(P3);
  Vec3 d2 = -*t1; double a = ch / 3, b = ch / 3;
  if (pts.size() > 2) { auto ab = edit::fitHandles(pts, P0, P3, *t0, d2); a = ab.first; b = ab.second; }
  return {*t0 * std::clamp(a, 0.05 * ch, ch), d2 * std::clamp(b, 0.05 * ch, ch)};
}
// the cut's curve: anchors (pre … mids … post) and the handles of its stretch over the wall (fitted to the path); at the
// two fork anchors the side running along A / B takes the host's own handle
void cutCurveHandles(Ctx& X, Build& B, bool fit) {
  const Frame& F = B.F; B.anchors = F.pre; for (size_t k = 0; k < B.mids.size(); k++) B.anchors.push_back(-1000 - int(k)); B.anchors.insert(B.anchors.end(), F.post.begin(), F.post.end());
  B.h.clear(); X.mids = B.mids;
  int k0 = int(F.pre.size()) - 1, k1 = k0 + int(B.mids.size()) + 1;
  auto host = [&](int L) { return L ? X.P.B : X.P.A; };
  auto put = [&](int a, bool in, std::optional<Vec3> v) { if (!v || !(X.smooth(a) > 0)) return; auto& e = B.h[a]; (in ? e.first : e.second) = *v; };
  if (k0 > 0) { auto h = handleToward(X.sc, X.P, host(F.pL), B.anchors[size_t(k0)], B.anchors[size_t(k0 - 1)]); put(B.anchors[size_t(k0)], true, h ? std::optional<Vec3>(*h - X.pos(B.anchors[size_t(k0)])) : std::nullopt); }
  if (k1 + 1 < int(B.anchors.size())) { auto h = handleToward(X.sc, X.P, host(F.qL), B.anchors[size_t(k1)], B.anchors[size_t(k1 + 1)]); put(B.anchors[size_t(k1)], false, h ? std::optional<Vec3>(*h - X.pos(B.anchors[size_t(k1)])) : std::nullopt); }
  for (int s = k0; s < k1; s++) {
    int a = B.anchors[size_t(s)], b = B.anchors[size_t(s + 1)];
    auto H = fit ? pathHandles(B.d3, B.at[size_t(s - k0)], B.at[size_t(s - k0 + 1)], X.pos(a), X.pos(b)) : std::pair{(X.pos(b) - X.pos(a)) * (1.0 / 3), (X.pos(a) - X.pos(b)) * (1.0 / 3)};
    put(a, false, H.first); put(b, true, H.second);
  }
}
// the Bézier segments of the cut's stretch over the wall (k0 … k1)
std::vector<BezSeg> cutSegs(Ctx& X, const Build& B) {
  int k0 = int(B.F.pre.size()) - 1, k1 = k0 + int(B.mids.size()) + 1; std::vector<BezSeg> out;
  for (int s = k0; s < k1; s++) {
    int a = B.anchors[size_t(s)], b = B.anchors[size_t(s + 1)]; Vec3 pa = X.pos(a), pb = X.pos(b);
    auto fa = B.h.find(a), fb = B.h.find(b);
    Vec3 ho = fa != B.h.end() && fa->second.second ? pa + *fa->second.second : pa, hi = fb != B.h.end() && fb->second.first ? pb + *fb->second.first : pb;
    out.push_back({pa, ho, hi, pb});
  }
  return out;
}
Q endParam(const Grid& G, const Viewport::SurfCutEnd& E) { return {edgeR(G, E.side ? G.M - 1 : 0, E.pos), double(E.side ? G.M - 1 : 0)}; }
std::variant<Build, QString> cutBuild(const Scene& sc, const Viewport::SurfCutState& P, const Grid& G, double scale) {
  auto Fr = cutFrame(sc, P); if (auto* e = std::get_if<QString>(&Fr)) return *e;
  Build B; B.F = std::get<Frame>(Fr); B.swapped = B.F.swapped;
  std::vector<Q> ctl{endParam(G, *P.start)}; for (auto& q : P.pts) ctl.push_back({q.r, q.t}); ctl.push_back(endParam(G, *P.end));
  if (B.F.swapped) std::reverse(ctl.begin(), ctl.end());
  WPath W = wallPath(G, ctl); for (auto& q : W.dense) B.d3.push_back(gridAt(G, q.r, q.t));
  // the placed anchors, plus one in the middle of every stretch where the curve through them still leaves the path by
  // more than 1% of the wall's width — until it follows the path
  double tol = std::max(0.01 * G.width, 1e-5 * scale);
  std::vector<int> ci(W.idx.begin() + 1, W.idx.end() - 1);
  Ctx X{sc, P, G, {}};
  for (int pass = 0; pass < 12; pass++) {
    B.mids.clear(); for (int i : ci) B.mids.push_back(B.d3[size_t(i)]);
    B.at = {0}; B.at.insert(B.at.end(), ci.begin(), ci.end()); B.at.push_back(int(W.dense.size()) - 1);
    cutCurveHandles(X, B, true); auto segs = cutSegs(X, B);
    std::vector<int> add;
    for (size_t s = 0; s + 1 < B.at.size(); s++) {
      int lo = B.at[s], hi = B.at[s + 1]; if (hi - lo < 3) continue;
      double dev = 0;
      for (int w = 1; w < 8; w++) { Vec3 x = bez(segs[s], w / 8.0); double dm = 1e300; for (int i = lo; i < hi; i++) dm = std::min(dm, distToSeg(x, B.d3[size_t(i)], B.d3[size_t(i + 1)])); dev = std::max(dev, dm); }
      if (dev > tol) add.push_back((lo + hi) >> 1);
    }
    if (add.empty() || ci.size() > 200) break;
    ci.insert(ci.end(), add.begin(), add.end()); std::sort(ci.begin(), ci.end());
  }
  for (int i : ci) B.mp.push_back(W.dense[size_t(i)]);
  B.idx = W.idx;
  return B;
}
}  // namespace

namespace {
std::vector<Q> edgeCtl(const Grid& G, const std::vector<Viewport::SurfCutQ>& pts, const std::optional<Viewport::SurfCutQ>& hover);
struct Fold { std::vector<Q> stem, leg1, leg2; };   // (a "V" cut as a "Y": the stem from the edge to the apex, the two legs from the apex to the other edge)
std::optional<Fold> edgeFold(const Grid& G, const std::vector<Viewport::SurfCutQ>& pts, const std::optional<Viewport::SurfCutQ>& hover);
}

std::optional<RayHit> Viewport::pickWall(const QPointF& m, int sf) const {
  auto it = hair_.walls.find(sf); if (it == hair_.walls.end()) return std::nullopt;
  PickMesh pm; pm.itemId = sf; pm.pos = &it->second.pos; for (auto& q : it->second.quads) for (int k : {0, 1, 2, 0, 2, 3}) pm.tris.push_back(uint32_t(q[size_t(k)]));
  for (auto& p : it->second.pos) pm.box.expand(p);
  return raycast(ray(m), {pm});
}

// the wall's curve under the cursor (within maxPx): an end of the cut — on an anchor (one within 10 px of the cursor),
// else at a place not put in yet (its handles: by de Casteljau)
std::optional<Viewport::SurfCutEnd> Viewport::edgePick(const QPointF& m, double maxPx) const {
  const auto& P = *scut_; auto G = wallGrid(hair_, P.sf); if (!G) return std::nullopt;
  auto wh = pickWall(m, P.sf); double near = 0.3 * G->width; Projector Pj = projector();
  struct Best { double d; int side, c, i; double t; }; std::optional<Best> best;
  for (auto [side, cid] : {std::pair{0, P.A}, std::pair{1, P.B}}) {
    auto it = hair_.curves.find(cid); if (it == hair_.curves.end()) continue; const auto& S = it->second;
    std::vector<ScreenPt> sp; for (auto& p : S.p) sp.push_back(Pj.toScreen(p));
    for (size_t i = 0; i + 1 < sp.size(); i++) {
      const ScreenPt &a = sp[i], &b = sp[i + 1]; if (a.z > 1 || b.z > 1) continue;
      double vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy, t = l2 > 0 ? std::clamp(((m.x() - a.x) * vx + (m.y() - a.y) * vy) / l2, 0.0, 1.0) : 0;
      double d = std::hypot(a.x + vx * t - m.x(), a.y + vy * t - m.y());
      if (d >= maxPx || (best && d >= best->d)) continue;
      if (wh && (S.p[i].lerp(S.p[i + 1], t) - wh->point).length() > near) continue;   // (a stretch lying elsewhere, seen through the wall)
      best = Best{d, side, cid, int(i), t};
    }
  }
  if (!best) return std::nullopt;
  const auto& S = hair_.curves.find(best->c)->second; const Item* c = scene_->item(best->c);
  int seg = S.seg[size_t(best->i)]; double u = std::clamp((best->i - seg * 24 + best->t) / 24, 0.0, 1.0);
  Vec3 at = S.p[size_t(best->i)].lerp(S.p[size_t(best->i + 1)], best->t);
  int a = -1; double ad = 10;
  for (int x : c->anchors) { const Vec3& xp = scene_->anchor(x)->pos; if ((xp - at).length() > near) continue; ScreenPt s = Pj.toScreen(xp); if (s.z > 1) continue; double d = std::hypot(s.x - m.x(), s.y - m.y()); if (d < ad) { ad = d; a = x; } }
  if (a < 0 && u < 0.02) a = c->anchors[size_t(seg)]; if (a < 0 && u > 0.98) a = c->anchors[size_t(seg + 1)];
  SurfCutEnd E; E.side = best->side; E.c = best->c;
  if (a >= 0) { E.a = a; E.pos = scene_->anchor(a)->pos; return E; }
  BezSeg s = curveSegs(*scene_, *c)[size_t(seg)]; auto l = [&](const Vec3& x, const Vec3& y) { return x.lerp(y, u); };
  Vec3 q0 = l(s[0], s[1]), q1 = l(s[1], s[2]), q2 = l(s[2], s[3]), r0 = l(q0, q1), r1 = l(q1, q2);
  E.seg = seg; E.u = u; E.pos = l(r0, r1); E.vh[c->anchors[size_t(seg)]] = r0; E.vh[c->anchors[size_t(seg + 1)]] = r1;
  return E;
}

void Viewport::startSurfCut(int sf) {
  if (!wallGrid(hair_, sf)) { ui::toast(tr("This wall has no surface to cut")); return; }
  cancelSurfCut();
  const Item* w = scene_->item(sf); SurfCutState P; P.sf = sf; P.A = w->a; P.B = w->b; scut_ = P;
  sel_.anchors.clear(); sel_.items = {sf}; onSelChange(); drawSurfCut(); emit brushChanged();
}

void Viewport::cancelSurfCut() { scut_.reset(); scutDrag_ = -1; scutViz_.clear(); ui::hud({}); overlayRev_++; requestUpdate(); }

bool Viewport::surfCutReady() const {
  if (!scut_ || !scut_->start || !scut_->end) return false;
  auto G = wallGrid(hair_, scut_->sf); if (!G) return false;
  return std::holds_alternative<Build>(cutBuild(*scene_, *scut_, *G, sceneScale(*scene_)));
}

void Viewport::drawSurfCut() {
  scutViz_.clear();
  if (!scut_ || !scene_->item(scut_->sf)) { cancelSurfCut(); return; }
  const auto& P = *scut_; auto G = wallGrid(hair_, P.sf); if (!G) return;
  Projector Pj = projector();
  auto put2 = [&](const std::vector<Vec3>& line, QColor c) { std::vector<std::pair<double, double>> o; for (auto& p : line) { ScreenPt s = Pj.toScreen(p); if (s.z < 1) o.push_back({s.x, s.y}); } if (o.size() >= 2) scutViz_.push_back({o, c, false}); };
  auto dot = [&](const Vec3& p, QColor c, double r) { ScreenPt s = Pj.toScreen(p); if (s.z >= 1) return; std::vector<std::pair<double, double>> o; for (int i = 0; i < 12; i++) { double t = 2 * M_PI * i / 12; o.push_back({s.x + r * std::cos(t), s.y + r * std::sin(t)}); } scutViz_.push_back({o, c, true}); };
  QString hud; std::optional<Build> built; QString err;
  if (P.start && P.end) {
    auto R = cutBuild(*scene_, P, *G, sceneScale(*scene_));
    if (auto* b = std::get_if<Build>(&R)) {
      built = *b; Ctx X{*scene_, P, *G, b->mids}; Build B = *b; cutCurveHandles(X, B, true);
      std::vector<Vec3> line; for (auto& s : cutSegs(X, B)) for (int k = 0; k < 24; k++) line.push_back(bez(s, k / 24.0));
      line.push_back(X.pos(B.anchors[size_t(int(B.F.pre.size()) - 1 + int(B.mids.size()) + 1)]));
      put2(line, QColor(0xff, 0x5a, 0x5a));
      for (auto& p : b->mids) dot(p, QColor(0xff, 0xbf, 0xbf), 3);
    } else err = std::get<QString>(R);
  } else if (P.edge) {                                                  // (from edge to edge: the ends carried to the edges, dim)
    if (auto F = edgeFold(*G, P.pts, P.hoverPt)) {                   // (a "V": shown as the "Y" it makes)
      for (const auto* c : {&F->stem, &F->leg1, &F->leg2}) { std::vector<Vec3> line; for (auto& q : wallPath(*G, *c).dense) line.push_back(gridAt(*G, q.r, q.t)); put2(line, QColor(0xff, 0x5a, 0x5a)); }
      std::vector<Vec3> tri{gridAt(*G, F->stem.back().r, F->stem.back().t), gridAt(*G, F->leg1.back().r, F->leg1.back().t), gridAt(*G, F->leg2.back().r, F->leg2.back().t), gridAt(*G, F->stem.back().r, F->stem.back().t)};
      put2(tri, QColor(0xff, 0x5a, 0x5a, 110));                       // (the triangle taken out)
      dot(gridAt(*G, F->stem.front().r, F->stem.front().t), QColor(0xff, 0x99, 0x99), 4.5);
    }
    const auto ctl = edgeFold(*G, P.pts, P.hoverPt) ? std::vector<Q>{} : edgeCtl(*G, P.pts, P.hoverPt);
    if (ctl.size() >= 2) { std::vector<Vec3> line; for (auto& q : wallPath(*G, ctl).dense) line.push_back(gridAt(*G, q.r, q.t)); put2(line, QColor(0xff, 0x5a, 0x5a));
      dot(gridAt(*G, ctl.front().r, ctl.front().t), QColor(0xff, 0x99, 0x99), 4.5); dot(gridAt(*G, ctl.back().r, ctl.back().t), QColor(0xff, 0x99, 0x99), 4.5); }
    if (P.hoverPt) dot(gridAt(*G, P.hoverPt->r, P.hoverPt->t), QColor(0xff, 0x99, 0x99), 4.5);
  } else if (P.start) {
    std::vector<Q> ctl{endParam(*G, *P.start)}; for (auto& q : P.pts) ctl.push_back({q.r, q.t});
    if (P.hoverEnd) ctl.push_back(endParam(*G, *P.hoverEnd)); else if (P.hoverPt) ctl.push_back({P.hoverPt->r, P.hoverPt->t});
    if (ctl.size() > 1) { std::vector<Vec3> line; for (auto& q : wallPath(*G, ctl).dense) line.push_back(gridAt(*G, q.r, q.t)); put2(line, QColor(0xff, 0x5a, 0x5a)); }
    if (P.hoverEnd) dot(P.hoverEnd->pos, QColor(0xff, 0x99, 0x99), 4.5); else if (P.hoverPt) dot(gridAt(*G, P.hoverPt->r, P.hoverPt->t), QColor(0xff, 0x99, 0x99), 4.5);
  }
  if (P.start) dot(P.start->pos, QColor(0xff, 0x40, 0x40), 6.5);
  for (auto& q : P.pts) dot(gridAt(*G, q.r, q.t), QColor(0xff, 0x73, 0x73), 5.5);
  if (P.end) dot(P.end->pos, QColor(0xff, 0x40, 0x40), 6.5);
  const Item* sf = scene_->item(P.sf);
  ui::hud(P.edge ? tr("Click on the wall to add anchors · a click on the other edge, Enter or Shift+RMB — cut (the ends are carried to the edges) · Backspace — remove the last anchor · Esc — cancel")
        : !P.start ? tr("Cutting %1: click one of its curves where the cut starts — or on the wall / its top or bottom edge for a cut from edge to edge · Esc — cancel").arg(sf->name)
        : !P.end ? tr("Click on the wall to add anchors · click its other curve to end the cut · Backspace — remove the last anchor · Esc — cancel")
        : built ? tr("Drag the anchors (the ends slide along the wall's curves) · RMB on the cut — insert an anchor · double click — delete one · Enter or Shift+RMB — cut (%1 anchors)").arg(built->mids.size() + 2)
        : tr("%1 — move its anchors").arg(err.isEmpty() ? tr("The cut must run along the wall, from nearer the root to nearer the tip") : err));
  overlayRev_++; requestUpdate();
}

bool Viewport::surfCutDown(QMouseEvent* e) {
  QPointF m = e->position();
  if (!scut_ || !scene_->item(scut_->sf)) {                            // pick the wall
    if (e->button() != Qt::LeftButton) return true;
    if (auto h = pickSurface(m); h && scene_->item(h->itemId) && scene_->item(h->itemId)->type == ItemType::Surface) { startSurfCut(h->itemId); }
    else ui::toast(tr("Click the NURBS wall to cut"));
    return true;
  }
  auto& P = *scut_; auto G = wallGrid(hair_, P.sf); if (!G) { cancelSurfCut(); return true; }
  if (!P.start) {
    if (e->button() != Qt::LeftButton) return true;
    if (!P.edge) if (auto E = edgePick(m, 9)) { P.start = E; drawSurfCut(); emit brushChanged(); emit statusText(surfCutWhere(tr("starts on"), &*E, std::nullopt)); return true; }
    if (!P.edge) if (auto h = pickSurface(m); h && h->itemId != P.sf && scene_->item(h->itemId) && scene_->item(h->itemId)->type == ItemType::Surface) { startSurfCut(h->itemId); return true; }
    // (on the wall itself or its top / bottom edge: an anchor of a cut from edge to edge; a click on the far edge ends it)
    if (auto h = pickWall(m, P.sf)) if (auto q = gridParam(*G, h->point)) {
      const double RL = G->R - 1; SurfCutQ n{q->r, q->t}; const bool atEdge = q->r < 0.04 * RL || q->r > 0.96 * RL;
      if (atEdge) n.r = q->r < 0.5 * RL ? 0 : RL;
      // (a click on the far edge ends a straight cut — one that kept going one way; a "V" (back toward where it began)
      // goes on: its second leg ends on the edge it began from — Enter / Shift+RMB cut it)
      std::vector<SurfCutQ> all = P.pts; all.push_back(n);
      bool up = false, down = false; for (size_t i = 1; i < all.size(); i++) { const double d = all[i].r - all[i - 1].r; if (d > 0.02 * RL) up = true; if (d < -0.02 * RL) down = true; }
      const bool far = P.edge && atEdge && !P.pts.empty() && !(up && down) && std::abs(n.r - P.pts.front().r) > 0.5 * RL;
      P.edge = true; P.pts.push_back(n); P.hoverPt.reset(); emit statusText(surfCutWhere(tr("an anchor on the wall (from edge to edge)"), nullptr, n));
      if (far) { applyEdgeCut(); emit brushChanged(); return true; }
      if (atEdge && P.pts.size() >= 3 && edgeFold(*G, P.pts, std::nullopt) && std::abs(n.r - P.pts.front().r) < 0.1 * RL) { applyEdgeCut(); emit brushChanged(); return true; }   // (the "V"'s second leg brought back to its edge: cut)
      drawSurfCut(); emit brushChanged(); return true; }
    ui::toast(tr("Start the cut on one of the wall's curves, on the wall or on its top / bottom edge")); return true;
  }
  if (!P.end) {
    if (e->button() != Qt::LeftButton) return true;
    if (auto E = edgePick(m, 9)) {
      P.end = E; auto R = cutBuild(*scene_, P, *G, sceneScale(*scene_));
      if (auto* er = std::get_if<QString>(&R)) { P.end.reset(); ui::toast(*er); return true; }
      P.hoverEnd.reset(); P.hoverPt.reset(); drawSurfCut(); emit brushChanged(); emit statusText(surfCutWhere(tr("ends on"), &*P.end, std::nullopt)); return true;
    }
    if (auto h = pickWall(m, P.sf)) { if (auto q = gridParam(*G, h->point)) { P.pts.push_back({q->r, q->t}); emit statusText(surfCutWhere(tr("an anchor on the wall"), nullptr, P.pts.back())); } drawSurfCut(); } else ui::toast(tr("Put the anchors on the wall"));
    return true;
  }
  // the cut's anchor under the cursor: 0 — start, 1…n — the placed ones, n + 1 — end
  Projector Pj = projector(); std::vector<Vec3> dots{P.start->pos}; for (auto& q : P.pts) dots.push_back(gridAt(*G, q.r, q.t)); dots.push_back(P.end->pos);
  int k = -1; double bd = 10; for (size_t i = 0; i < dots.size(); i++) { ScreenPt s = Pj.toScreen(dots[i]); if (s.z > 1) continue; double d = std::hypot(s.x - m.x(), s.y - m.y()); if (d < bd) { bd = d; k = int(i); } }
  if (e->button() == Qt::LeftButton && k >= 0) { scutDrag_ = k; return true; }
  if (e->button() == Qt::RightButton) {                                // RMB on the cut: a new anchor there
    auto R = cutBuild(*scene_, P, *G, sceneScale(*scene_)); auto* b = std::get_if<Build>(&R); if (!b) return true;
    int bi = -1; double bd2 = 8; for (size_t i = 0; i < b->d3.size(); i++) { ScreenPt s = Pj.toScreen(b->d3[i]); if (s.z > 1) continue; double d = std::hypot(s.x - m.x(), s.y - m.y()); if (d < bd2) { bd2 = d; bi = int(i); } }
    if (bi < 0) return true;
    int s = 0; while (s < int(b->idx.size()) - 2 && b->idx[size_t(s + 1)] <= bi) s++;
    auto q = gridParam(*G, b->d3[size_t(bi)]); if (!q) return true;
    int at = b->swapped ? int(P.pts.size()) - s : s; P.pts.insert(P.pts.begin() + at, {q->r, q->t});
    drawSurfCut(); scutDrag_ = b->swapped ? int(P.pts.size()) - s : s + 1;
  }
  return true;
}

void Viewport::surfCutMove(const QPointF& m) {
  if (!scut_) return; auto& P = *scut_; auto G = wallGrid(hair_, P.sf); if (!G) return;
  if (scutDrag_ >= 0) {                                                 // an end — along the wall's curves; one in between — over the wall
    if (scutDrag_ == 0 || scutDrag_ == int(P.pts.size()) + 1) {
      auto& slot = scutDrag_ == 0 ? P.start : P.end; auto E = edgePick(m, 1e9); if (!E) return;
      auto old = slot; slot = E;
      if (P.start && P.end && std::holds_alternative<QString>(cutFrame(*scene_, P))) { slot = old; return; }
      drawSurfCut(); return;
    }
    if (auto h = pickWall(m, P.sf)) if (auto q = gridParam(*G, h->point)) { P.pts[size_t(scutDrag_ - 1)] = {q->r, q->t}; drawSurfCut(); }
    return;
  }
  if (P.edge) { P.hoverPt.reset(); if (auto h = pickWall(m, P.sf)) if (auto q = gridParam(*G, h->point)) P.hoverPt = SurfCutQ{q->r, q->t}; drawSurfCut(); return; }   // (a cut from edge to edge: it follows the cursor)
  if (!P.start || P.end) return;                                        // while placing: the cut follows the cursor
  P.hoverEnd = edgePick(m, 9); P.hoverPt.reset();
  if (!P.hoverEnd) if (auto h = pickWall(m, P.sf)) if (auto q = gridParam(*G, h->point)) P.hoverPt = SurfCutQ{q->r, q->t};
  drawSurfCut();
}

void Viewport::mouseDoubleClickEvent(QMouseEvent* e) {
  if (topoDblClick(e)) return;                               // (the Topology room tells a double click by the press times)
  // the scissors' surface cut: a double click on one of its placed anchors — deleted
  if (tool_ == "cut" && cutMode == "surface" && scut_ && scut_->start && scut_->end && e->button() == Qt::LeftButton) {
    auto G = wallGrid(hair_, scut_->sf); if (!G) return; Projector Pj = projector();
    for (size_t i = 0; i < scut_->pts.size(); i++) { ScreenPt s = Pj.toScreen(gridAt(*G, scut_->pts[i].r, scut_->pts[i].t)); if (s.z < 1 && std::hypot(s.x - e->position().x(), s.y - e->position().y()) < 10) { scut_->pts.erase(scut_->pts.begin() + long(i)); scutDrag_ = -1; drawSurfCut(); return; } }
    return;
  }
  QVulkanWindow::mouseDoubleClickEvent(e);
}

// Enter / the panel's Cut: the cut as it stands — begun on a curve and not ended on the other one: from its last anchor
// carried to the wall's edge, as a cut begun on the wall is; nothing to cut yet — what is missing said
// (a click of the cut — where it went, in the wall's rows and across it: the status line)
QString Viewport::surfCutWhere(const QString& what, const SurfCutEnd* E, std::optional<SurfCutQ> q) const {
  if (!scut_) return {}; auto G = wallGrid(hair_, scut_->sf); if (!G) return {};
  if (E) { const Item* c = scene_->item(E->c); q.reset(); if (auto g = gridParam(*G, E->pos)) q = SurfCutQ{g->r, g->t};
    return tr("Scissors: the cut %1 %2%3%4").arg(what, c ? c->name : QString("?"), E->a >= 0 ? tr(" (its anchor)") : QString(), q ? tr(" — row %1 of %2, across %3").arg(q->r, 0, 'f', 1).arg(G->R - 1).arg(q->t, 0, 'f', 2) : QString()); }
  return q ? tr("Scissors: %1 — row %2 of %3, across %4").arg(what).arg(q->r, 0, 'f', 1).arg(G->R - 1).arg(q->t, 0, 'f', 2) : QString();
}

bool Viewport::surfCutFinish() {
  if (!scut_) return false;
  if (scut_->edge) { applyEdgeCut(); emit brushChanged(); return true; }
  // (begun on the wall's top / bottom edge — a curve's end there, or a point of that edge taken for a curve's: the cut
  // starts there, on the edge, as its own curve — not inserted into the wall's curve and running from its corner along
  // the edge, joined to it; the next anchor on that same edge already — the start there)
  if (scut_->start && (!scut_->pts.empty() || scut_->end)) if (auto G = wallGrid(hair_, scut_->sf)) if (auto q = gridParam(*G, scut_->start->pos)) {
    const double RL = G->R - 1, er = q->r < 0.5 * RL ? 0.0 : RL;
    if (std::abs(q->r - er) < 0.04 * RL) {
      if (!scut_->pts.empty() && std::abs(scut_->pts.front().r - er) < 0.04 * RL) scut_->pts.front().r = er; else scut_->pts.insert(scut_->pts.begin(), SurfCutQ{er, q->t});
      if (scut_->end) if (auto qe = gridParam(*G, scut_->end->pos)) scut_->pts.push_back(SurfCutQ{qe->r, qe->t});   // (its end on the other curve: the last anchor)
      scut_->start.reset(); scut_->end.reset(); scut_->edge = true; applyEdgeCut(); emit brushChanged(); return true; } }
  if (scut_->start && scut_->end) { applySurfCut(); return true; }
  if (scut_->start && !scut_->pts.empty()) if (auto G = wallGrid(hair_, scut_->sf)) {
    const double RL = G->R - 1; const SurfCutQ last = scut_->pts.back();
    if (last.r < 0.04 * RL || last.r > 0.96 * RL) { applyEdgeToCurveCut(); emit brushChanged(); return true; } }
  if (scut_->start && !scut_->pts.empty()) if (auto G = wallGrid(hair_, scut_->sf)) {
    // (its end on the wall's other curve, across from the last anchor — as a click there would put it; the last anchor
    // dropped when it lay by that curve already)
    const int other = 1 - scut_->start->side; const double tO = other ? 1.0 : 0.0; const SurfCutQ last = scut_->pts.back();
    const ScreenPt sp = projector().toScreen(gridAt(*G, last.r, tO));
    if (sp.z < 1) if (auto E = edgePick(QPointF(sp.x, sp.y), 6); E && E->side == other) {
      const auto keep = scut_->pts; if (std::abs(last.t - tO) < 0.25) scut_->pts.pop_back();
      scut_->end = E; if (surfCutReady()) { applySurfCut(); return true; }
      scut_->end.reset(); scut_->pts = keep; }
    // (begun on the wall's top / bottom edge: on to the edge from the last anchor, as a cut begun on the wall)
    if (auto q = gridParam(*G, scut_->start->pos); q && (q->r < 0.04 * (G->R - 1) || q->r > 0.96 * (G->R - 1))) {
      scut_->pts.insert(scut_->pts.begin(), SurfCutQ{q->r, q->t}); scut_->start.reset(); scut_->edge = true; applyEdgeCut(); emit brushChanged(); return true; } }
  ui::toast(!scut_->start ? tr("Start the cut on one of the wall's curves, on the wall or on its top / bottom edge") : tr("Put an anchor on the wall, or click the wall's other curve to end the cut"));
  return true;
}

bool Viewport::surfCutKey(int key) {
  if (!scut_) return false;
  if (key == Qt::Key_Escape) { cancelSurfCut(); emit brushChanged(); return true; }
  if (key == Qt::Key_Return || key == Qt::Key_Enter) { surfCutFinish(); return true; }
  if (key == Qt::Key_Backspace && scut_->edge) { if (!scut_->pts.empty()) scut_->pts.pop_back(); if (scut_->pts.empty()) scut_->edge = false; drawSurfCut(); emit brushChanged(); return true; }
  if (key == Qt::Key_Backspace && scut_->start && !scut_->end) { if (!scut_->pts.empty()) scut_->pts.pop_back(); else scut_->start.reset(); drawSurfCut(); emit brushChanged(); return true; }
  return false;
}

bool Viewport::openSurfCutPie(const QPoint& global) {
  if (!scut_ || !pieHost_) return false;
  auto& P = *scut_; bool ready = surfCutReady(); ui::PieSpec s; s.title = tr("Cutting %1").arg(scene_->item(P.sf)->name);
  ui::PieItem t;
  if (P.edge && !P.pts.empty()) { t.label = tr("✂ Cut from edge to edge (%1 anchor%2)").arg(P.pts.size()).arg(P.pts.size() == 1 ? "" : "s"); t.act = [this] { applyEdgeCut(); emit brushChanged(); }; }
  else if (ready) { auto G = wallGrid(hair_, P.sf); auto R = cutBuild(*scene_, P, *G, sceneScale(*scene_)); t.label = tr("✂ Cut (%1 anchors)").arg(std::get<Build>(R).mids.size() + 2); t.act = [this] { surfCutFinish(); }; }
  else { QString err; if (P.start && P.end) { auto G = wallGrid(hair_, P.sf); auto R = cutBuild(*scene_, P, *G, sceneScale(*scene_)); if (auto* e = std::get_if<QString>(&R)) err = *e; }
         t.label = !P.start ? tr("Start on one of the wall's curves") : !P.end ? tr("End on the wall's other curve") : err.isEmpty() ? tr("The cut must run along the wall — move its ends") : err; t.act = [] {}; }
  s.top.push_back(t);
  if (P.edge && !P.pts.empty()) { ui::PieItem b; b.label = tr("↶ Remove the last anchor"); b.act = [this] { if (!scut_) return; if (!scut_->pts.empty()) scut_->pts.pop_back(); if (scut_->pts.empty()) scut_->edge = false; drawSurfCut(); emit brushChanged(); }; s.bottom.push_back(b); }
  if (P.start && !P.end) { ui::PieItem b; b.label = P.pts.empty() ? tr("↶ Remove the start") : tr("↶ Remove the last anchor"); b.act = [this] { if (!scut_) return; if (!scut_->pts.empty()) scut_->pts.pop_back(); else scut_->start.reset(); drawSurfCut(); emit brushChanged(); }; s.bottom.push_back(b); }
  ui::PieItem c; c.label = tr("✖ Cancel the cut"); c.danger = true; c.act = [this] { cancelSurfCut(); emit brushChanged(); }; s.bottom.push_back(c);
  ui::Pie::open(pieHost_, global, s, true);
  return true;
}

namespace {
// a cut from edge to edge in the wall's coordinates: its anchors ordered from row 0, its ends carried on to the two edges
// (straight on as it goes there; one anchor — straight down / up); never onto the wall's own curves
std::vector<Q> edgeCtl(const Grid& G, const std::vector<Viewport::SurfCutQ>& pts, const std::optional<Viewport::SurfCutQ>& hover) {
  std::vector<Q> qs; for (auto& p : pts) qs.push_back({p.r, p.t}); if (hover) qs.push_back({hover->r, hover->t});
  if (qs.empty()) return {};
  const double RL = G.R - 1, ML = G.M - 1; auto tIn = [&](double t) { return std::clamp(t, 0.04 * ML, 0.96 * ML); };
  if (qs.size() >= 2 && qs.front().r > qs.back().r) std::reverse(qs.begin(), qs.end());
  for (auto& q : qs) q.t = tIn(q.t);
  auto carry = [&](const Q& a, const Q& b, double toR) { const double dr = b.r - a.r; if (std::abs(dr) < 1e-9) return Q{toR, b.t}; return Q{toR, tIn(b.t + (b.t - a.t) * (toR - b.r) / dr)}; };
  std::vector<Q> out;
  if (qs.front().r > 1e-6) out.push_back(qs.size() >= 2 ? carry(qs[1], qs[0], 0) : Q{0, qs[0].t});
  for (auto& q : qs) if (out.empty() || q.r > out.back().r + 1e-6) out.push_back(q);
  if (out.back().r < RL - 1e-6) out.push_back(out.size() >= 2 ? carry(out[out.size() - 2], out.back(), RL) : Q{RL, out.back().t});
  return out;
}
// a "V" placed (its two ends toward one edge, the place between them nearest the other one — the apex): a "Y" — the stem
// from the other edge straight to the apex, the two legs from it on to their edge (their ends carried there); the
// triangle between the legs is taken out. leg1 — the one nearer curve A
std::optional<Fold> edgeFold(const Grid& G, const std::vector<Viewport::SurfCutQ>& pts, const std::optional<Viewport::SurfCutQ>& hover) {
  std::vector<Q> qs; for (auto& p : pts) qs.push_back({p.r, p.t}); if (hover) qs.push_back({hover->r, hover->t});
  if (qs.size() < 3) return std::nullopt;
  const double RL = G.R - 1, ML = G.M - 1; auto tIn = [&](double t) { return std::clamp(t, 0.04 * ML, 0.96 * ML); };
  for (auto& q : qs) q.t = tIn(q.t);
  size_t lo = 0, hi = 0; for (size_t i = 1; i < qs.size(); i++) { if (qs[i].r < qs[lo].r) lo = i; if (qs[i].r > qs[hi].r) hi = i; }
  const double gap = 0.1 * RL; size_t ap = 0; double E = 0, O = 0;
  if (lo > 0 && lo + 1 < qs.size() && qs.front().r > qs[lo].r + gap && qs.back().r > qs[lo].r + gap) { ap = lo; E = RL; O = 0; }   // (opening toward the last row)
  else if (hi > 0 && hi + 1 < qs.size() && qs.front().r < qs[hi].r - gap && qs.back().r < qs[hi].r - gap) { ap = hi; E = 0; O = RL; }   // (toward row 0)
  else return std::nullopt;
  auto carry = [&](const Q& a, const Q& b, double toR) { const double dr = b.r - a.r; if (std::abs(dr) < 1e-9) return Q{toR, b.t}; return Q{toR, tIn(b.t + (b.t - a.t) * (toR - b.r) / dr)}; };
  auto leg = [&](bool back) { std::vector<Q> L{qs[ap]};
    if (back) { for (size_t i = ap + 1; i < qs.size(); i++) if (std::abs(qs[i].r - E) < std::abs(L.back().r - E) - 1e-6) L.push_back(qs[i]); }
    else { for (size_t i = ap; i-- > 0;) if (std::abs(qs[i].r - E) < std::abs(L.back().r - E) - 1e-6) L.push_back(qs[i]); }
    if (std::abs(L.back().r - E) > 1e-6) L.push_back(carry(L[L.size() - 2], L.back(), E));
    return L; };
  Fold F; F.stem = {Q{O, qs[ap].t}, qs[ap]}; F.leg1 = leg(false); F.leg2 = leg(true);
  if (F.leg1.back().t > F.leg2.back().t) std::swap(F.leg1, F.leg2);
  return F;
}
}  // namespace

// a cut from the wall's top / bottom edge to a place on one of its curves (begun there, its last anchor on the edge —
// or the other way round): a new curve from the edge through the anchors, its end that curve's anchor (put in keeping
// the curve's shape) — a Y there; the wall split in two beside it
QString Viewport::applyEdgeToCurveCut() {
  if (!scut_ || !scut_->start || scut_->pts.empty()) return "!";
  const int sf = scut_->sf; auto G = wallGrid(hair_, sf); if (!G) return "!";
  auto qs0 = gridParam(*G, scut_->start->pos); if (!qs0) return "!";
  const double RL = G->R - 1, ML = G->M - 1;
  std::vector<Q> ctl{Q{qs0->r, qs0->t}}; for (auto& p : scut_->pts) ctl.push_back(Q{p.r, std::clamp(p.t, 0.04 * ML, 0.96 * ML)});
  ctl.back().r = ctl.back().r < 0.5 * RL ? 0.0 : RL;                           // (its edge end right on the edge)
  const bool curveFirst = true; (void)curveFirst;
  std::vector<Q> ord = ctl; if (ord.front().r > ord.back().r) std::reverse(ord.begin(), ord.end());   // (along the rows, as the wall's path goes)
  std::vector<Vec3> path; for (auto& q : wallPath(*G, ord).dense) path.push_back(gridAt(*G, q.r, q.t));
  if (path.size() < 2) return "!";
  const Item *A = scene_->item(scut_->A), *B = scene_->item(scut_->B); if (!A || !B || A->anchors.empty()) return "!";
  if (scene_->anchor(A->anchors.front())->pos.distanceTo(G->at(0, 0)) > scene_->anchor(A->anchors.front())->pos.distanceTo(G->at(G->R - 1, 0))) std::reverse(path.begin(), path.end());   // (root → tip, as A runs)
  const bool endIsCurve = path.back().distanceTo(scut_->start->pos) < path.front().distanceTo(scut_->start->pos);   // (which end of it is on the curve)
  const int K = std::max({3, int(scut_->pts.size()) + 2, int(std::lround(std::max(A->anchors.size(), B->anchors.size()) * std::abs(ctl.front().r - ctl.back().r) / std::max(1.0, RL))) + 1});
  std::vector<double> L{0}; for (size_t i = 1; i < path.size(); i++) L.push_back(L.back() + path[i].distanceTo(path[i - 1]));
  std::vector<Vec3> an; for (int k = 0; k < K; k++) { const double d = L.back() * k / (K - 1); size_t i = 0; while (i + 2 < L.size() && L[i + 1] < d) i++;
    const double g = L[i + 1] - L[i]; an.push_back(path[i].lerp(path[i + 1], g > 0 ? std::clamp((d - L[i]) / g, 0.0, 1.0) : 0.0)); }
  SurfCutEnd E = *scut_->start; const int Aid = scut_->A, Bid = scut_->B; cancelSurfCut();
  QString msg;
  runEdit([&](Scene& sc, Selection& sel, const CurveCache& cc) -> QString {
    Network net(sc, cc, sel.items, sel.anchors);
    if (E.a < 0) { E.a = net.insertKeepShape(E.c, E.seg, E.u); sc.reindex(); }   // (the curve's place: an anchor of it now)
    const Item* Ai = sc.item(Aid); const int Apar = Ai->parent; const QString Aname = Ai->name;
    Item& c = net.makeCurve(Apar); const int cid = c.id; c.name = Aname + " cut";
    std::vector<int> ids; for (size_t k = 0; k < an.size(); k++) {
      const bool onCurve = endIsCurve ? k + 1 == an.size() : k == 0;
      if (onCurve) { ids.push_back(E.a); continue; }
      Anchor& n = sc.addAnchor(an[k]); n.smooth = 1; ids.push_back(n.id); }
    sc.item(cid)->anchors = ids;
    { auto& arr = Apar >= 0 ? sc.item(Apar)->children : sc.roots; arr.erase(std::remove(arr.begin(), arr.end(), cid), arr.end()); auto at = std::find(arr.begin(), arr.end(), Aid); arr.insert(at == arr.end() ? arr.end() : at + 1, cid); }
    const Item* w = sc.item(sf); const QJsonObject p = w->raw.value("p").toObject(); const int par = w->parent; const bool vis = w->visible;
    for (auto [x, y] : {std::pair{Aid, cid}, std::pair{cid, Bid}}) { const int n = edit::nextNumber(sc, ItemType::Surface); Item& nw = sc.addItem(ItemType::Surface, par, QString("Surface %1").arg(n)); nw.a = x; nw.b = y; nw.visible = vis; nw.raw = QJsonObject{{"p", p}}; }
    net.deleteItem(sf);   // (after the new walls: the old one's service curves they use stay)
    sc.reindex();
    sel.anchors.clear(); sel.items = {cid};
    return msg = tr("Cut: %1 from the edge to %2 — the wall split in two").arg(sc.item(cid)->name, sc.item(E.c) ? sc.item(E.c)->name : QString());
  }, true);
  return msg;
}

// the cut from edge to edge: a new curve along it (as many anchors as the wall's curves have, evenly), the wall split in
// two beside it; a wall growing out of a crown — the cut's root onto the crown's lower connector
QString Viewport::applyEdgeCut() {
  if (!scut_ || !scut_->edge || scut_->pts.empty()) return "!";
  const int sf = scut_->sf; auto G = wallGrid(hair_, sf); if (!G) return "!";
  if (auto F = edgeFold(*G, scut_->pts, std::nullopt)) return applyFoldCut(&*F);
  const auto ctl = edgeCtl(*G, scut_->pts, std::nullopt); if (ctl.size() < 2) return "!";
  std::vector<Vec3> path; for (auto& q : wallPath(*G, ctl).dense) path.push_back(gridAt(*G, q.r, q.t));
  const Item *A = scene_->item(scut_->A), *B = scene_->item(scut_->B); if (!A || !B || A->anchors.empty()) return "!";
  if (scene_->anchor(A->anchors.front())->pos.distanceTo(G->at(0, 0)) > scene_->anchor(A->anchors.front())->pos.distanceTo(G->at(G->R - 1, 0))) std::reverse(path.begin(), path.end());   // (root → tip, as A runs)
  const int K = std::max({3, int(A->anchors.size()), int(B->anchors.size()), int(scut_->pts.size()) + 2});
  std::vector<double> L{0}; for (size_t i = 1; i < path.size(); i++) L.push_back(L.back() + path[i].distanceTo(path[i - 1]));
  std::vector<Vec3> an; for (int k = 0; k < K; k++) { const double d = L.back() * k / (K - 1); size_t i = 0; while (i + 2 < L.size() && L[i + 1] < d) i++;
    const double g = L[i + 1] - L[i]; an.push_back(path[i].lerp(path[i + 1], g > 0 ? std::clamp((d - L[i]) / g, 0.0, 1.0) : 0.0)); }
  const int Aid = scut_->A, Bid = scut_->B; cancelSurfCut();
  QString msg;
  runEdit([&](Scene& sc, Selection& sel, const CurveCache& cc) -> QString {
    Network net(sc, cc, sel.items, sel.anchors);
    const Item* Ai = sc.item(Aid); const int Apar = Ai->parent; const QString Aname = Ai->name;
    Item& c = net.makeCurve(Apar); const int cid = c.id; c.name = Aname + " cut";
    std::vector<int> ids; for (const Vec3& p : an) { Anchor& n = sc.addAnchor(p); n.smooth = 1; ids.push_back(n.id); }
    sc.item(cid)->anchors = ids;
    { auto& arr = Apar >= 0 ? sc.item(Apar)->children : sc.roots; arr.erase(std::remove(arr.begin(), arr.end(), cid), arr.end()); auto at = std::find(arr.begin(), arr.end(), Aid); arr.insert(at == arr.end() ? arr.end() : at + 1, cid); }
    const Item* w = sc.item(sf); const QJsonObject p = w->raw.value("p").toObject(); const int par = w->parent; const bool vis = w->visible;
    for (auto [x, y] : {std::pair{Aid, cid}, std::pair{cid, Bid}}) { const int n = edit::nextNumber(sc, ItemType::Surface); Item& nw = sc.addItem(ItemType::Surface, par, QString("Surface %1").arg(n)); nw.a = x; nw.b = y; nw.visible = vis; nw.raw = QJsonObject{{"p", p}}; }
    net.deleteItem(sf);   // (after the new walls: a service curve of the old one's that they use stays — deleted with it before, the wall on it was never built)
    sc.reindex();
    // (a wall growing out of a crown — both its curves' roots on a lower connector: the cut's root goes onto it too)
    QString onCrown;
    for (int kid : crown::crowns(sc)) { const Item* low = sc.item(sc.item(kid)->raw.value("low").toInt(-1)); if (!low) continue;
      const Item *a = sc.item(Aid), *b = sc.item(Bid); if (!a || !b) continue;
      auto rooted = [&](const Item* x) { for (int e : {x->anchors.front(), x->anchors.back()}) if (crown::onConnector(sc, e, *low)) return true; return false; };
      if (rooted(a) && rooted(b)) { crown::connectToConnector(sc, cc, ids.front(), low->id); onCrown = tr(", its root on the crown's connector"); break; } }
    sc.reindex();
    sel.anchors.clear(); sel.items = {cid};
    return msg = tr("Cut: %1 from edge to edge — the wall split in two%2").arg(sc.item(cid)->name, onCrown);
  }, true);
  emit brushChanged(); return msg;
}


// the "V" cut as a "Y": two curves sharing the stem (its anchors), each going on along its leg; the wall becomes (A, the
// first) and (the second, B) — the triangle between the legs gone. A wall growing out of a crown: the root onto its connector
QString Viewport::applyFoldCut(const void* fold) {
  const Fold& F = *static_cast<const Fold*>(fold);
  const int sf = scut_->sf; auto G = wallGrid(hair_, sf); if (!G) return "!";
  const Item *A = scene_->item(scut_->A), *B = scene_->item(scut_->B); if (!A || !B || A->anchors.empty()) return "!";
  auto line = [&](const std::vector<Q>& ctl) { std::vector<Vec3> p; for (auto& q : wallPath(*G, ctl).dense) p.push_back(gridAt(*G, q.r, q.t)); return p; };
  auto len = [](const std::vector<Vec3>& p) { double l = 0; for (size_t i = 1; i < p.size(); i++) l += p[i].distanceTo(p[i - 1]); return l; };
  auto even = [](const std::vector<Vec3>& p, int k) { std::vector<double> L{0}; for (size_t i = 1; i < p.size(); i++) L.push_back(L.back() + p[i].distanceTo(p[i - 1]));
    std::vector<Vec3> o; for (int j = 0; j < k; j++) { const double d = L.back() * j / (k - 1); size_t i = 0; while (i + 2 < L.size() && L[i + 1] < d) i++; const double g = L[i + 1] - L[i]; o.push_back(p[i].lerp(p[i + 1], g > 0 ? std::clamp((d - L[i]) / g, 0.0, 1.0) : 0.0)); } return o; };
  const auto ps = line(F.stem), p1 = line(F.leg1), p2 = line(F.leg2);
  const int K = std::max({4, int(A->anchors.size()), int(B->anchors.size())}); const double ls = len(ps), ll = std::max(len(p1), len(p2));
  const int ks = std::max(2, int(std::lround(K * ls / std::max(1e-9, ls + ll)))), kl = std::max(2, K - ks + 1);
  const auto as = even(ps, ks), a1 = even(p1, kl), a2 = even(p2, kl);
  // (the root: at the stem's edge or at the legs' — the curves run root → tip as A does)
  const bool rootRow0 = scene_->anchor(A->anchors.front())->pos.distanceTo(G->at(0, 0)) <= scene_->anchor(A->anchors.front())->pos.distanceTo(G->at(G->R - 1, 0));
  const bool stemAtRoot = (F.stem.front().r < 0.5) == rootRow0;
  const int Aid = scut_->A, Bid = scut_->B; cancelSurfCut();
  QString msg;
  runEdit([&](Scene& sc, Selection& sel, const CurveCache& cc) -> QString {
    Network net(sc, cc, sel.items, sel.anchors);
    const Item* Ai = sc.item(Aid); const int Apar = Ai->parent; const QString Aname = Ai->name;
    auto mk = [&](const Vec3& p) { Anchor& n = sc.addAnchor(p); n.smooth = 1; return n.id; };
    std::vector<int> st; for (const Vec3& p : as) st.push_back(mk(p));          // (the stem: shared — the apex its last)
    std::vector<int> l1, l2; for (size_t i = 1; i < a1.size(); i++) l1.push_back(mk(a1[i])); for (size_t i = 1; i < a2.size(); i++) l2.push_back(mk(a2[i]));
    std::vector<int> x = st, y = st; x.insert(x.end(), l1.begin(), l1.end()); y.insert(y.end(), l2.begin(), l2.end());
    if (!stemAtRoot) { std::reverse(x.begin(), x.end()); std::reverse(y.begin(), y.end()); }
    std::vector<int> cs;
    for (auto [an, nm] : {std::pair{x, Aname + " cut"}, std::pair{y, Aname + " cut b"}}) { Item& c = net.makeCurve(Apar); c.name = nm; c.anchors = an; cs.push_back(c.id); }
    { auto& arr = Apar >= 0 ? sc.item(Apar)->children : sc.roots; for (int c : cs) arr.erase(std::remove(arr.begin(), arr.end(), c), arr.end()); auto at = std::find(arr.begin(), arr.end(), Aid); arr.insert(at == arr.end() ? arr.end() : at + 1, cs.begin(), cs.end()); }
    const Item* w = sc.item(sf); const QJsonObject p = w->raw.value("p").toObject(); const int par = w->parent; const bool vis = w->visible;
    for (auto [a, b] : {std::pair{Aid, cs[0]}, std::pair{cs[1], Bid}}) { const int n = edit::nextNumber(sc, ItemType::Surface); Item& nw = sc.addItem(ItemType::Surface, par, QString("Surface %1").arg(n)); nw.a = a; nw.b = b; nw.visible = vis; nw.raw = QJsonObject{{"p", p}}; }
    net.deleteItem(sf);   // (after the new walls: a service curve of the old one's that they use stays — deleted with it before, the wall on it was never built)
    sc.reindex();
    QString onCrown;
    for (int kid : crown::crowns(sc)) { const Item* low = sc.item(sc.item(kid)->raw.value("low").toInt(-1)); if (!low) continue;
      const Item *a = sc.item(Aid), *b = sc.item(Bid); if (!a || !b) continue;
      auto rooted = [&](const Item* c) { for (int e : {c->anchors.front(), c->anchors.back()}) if (crown::onConnector(sc, e, *low)) return true; return false; };
      if (rooted(a) && rooted(b)) {   // (the root: the shared stem's end — once; or each leg's end)
        if (stemAtRoot) crown::connectToConnector(sc, cc, st.front(), low->id); else { crown::connectToConnector(sc, cc, l1.back(), low->id); crown::connectToConnector(sc, cc, l2.back(), low->id); }
        onCrown = tr(", rooted on the crown's connector"); break; } }
    sc.reindex();
    sel.anchors.clear(); sel.items = cs;
    return msg = tr("Y cut: %1 and %2 share the stem, the triangle between their legs taken out — the wall split in two%3").arg(sc.item(cs[0])->name, sc.item(cs[1])->name, onCrown);
  }, true);
  emit brushChanged(); return msg;
}

void Viewport::surfCutSetPts(int sf, const std::vector<std::pair<double, double>>& rt) {
  startSurfCut(sf); if (!scut_) return; auto G = wallGrid(hair_, sf); if (!G) return;
  scut_->edge = true; scut_->pts.clear(); for (auto [r, t] : rt) scut_->pts.push_back({r * (G->R - 1), t * (G->M - 1)}); drawSurfCut();
}
// (the harness: the cut's start put on a wall's curve at row r, across t — as a click there would)
QString Viewport::surfCutStartAt(double r, double t) {
  if (!scut_) return "no cut"; auto G = wallGrid(hair_, scut_->sf); if (!G) return "no grid";
  const ScreenPt sp = projector().toScreen(gridAt(*G, r, t)); auto E = edgePick(QPointF(sp.x, sp.y), 30); if (!E) return "no curve there";
  scut_->start = E; drawSurfCut(); return surfCutWhere(tr("starts on"), &*E, std::nullopt);
}
QString Viewport::surfCutInfo() const {
  if (!scut_) return "scut: none"; auto G = wallGrid(hair_, scut_->sf);
  QString o = QString("scut wall %1 A %2 B %3 edge %4 R %5 M %6 pts").arg(scut_->sf).arg(scut_->A).arg(scut_->B).arg(scut_->edge).arg(G ? G->R : 0).arg(G ? G->M : 0);
  for (auto& q : scut_->pts) o += QString(" (%1,%2)").arg(q.r, 0, 'f', 2).arg(q.t, 0, 'f', 2);
  for (const auto* E : {&scut_->start, &scut_->end}) if (*E) o += QString(" %1 c%2 a%3 side%4 @%5,%6,%7").arg(E == &scut_->start ? "start" : "end").arg((*E)->c).arg((*E)->a).arg((*E)->side).arg((*E)->pos.x, 0, 'f', 3).arg((*E)->pos.y, 0, 'f', 3).arg((*E)->pos.z, 0, 'f', 3);
  if (G) o += QString(" fold %1").arg(edgeFold(*G, scut_->pts, std::nullopt).has_value());
  if (G) { Projector Pj = projector(); for (auto [r, t] : {std::pair{0.0, 0.5}, std::pair{double(G->R - 1), 0.5}, std::pair{0.5 * (G->R - 1), 0.0}, std::pair{0.5 * (G->R - 1), double(G->M - 1)}}) { const ScreenPt sp = Pj.toScreen(gridAt(*G, r, t)); o += QString(" r%1t%2@%3,%4").arg(r, 0, 'f', 0).arg(t, 0, 'f', 1).arg(int(sp.x)).arg(int(sp.y)); } }
  return o;
}

// the cut itself: a new curve along the cut and the wall split in two
void Viewport::applySurfCut() {
  if (!scut_ || !scut_->start || !scut_->end) return;
  auto G = wallGrid(hair_, scut_->sf); if (!G) return;
  { auto chk = cutBuild(*scene_, *scut_, *G, sceneScale(*scene_)); if (auto* e = std::get_if<QString>(&chk)) { ui::toast(*e); return; } }
  SurfCutState P = *scut_; Grid Gc = *G; std::vector<Vec3> gridPos = *G->pos; Gc.pos = &gridPos;   // (the wall's grid as it is now — the cut is on it)
  cancelSurfCut();
  runEdit([&](Scene& sc, Selection& sel, const CurveCache& cc) -> QString {
    Network net(sc, cc, sel.items, sel.anchors);
    // the ends not put in yet go into their curves now (keeping the shape)
    std::vector<SurfCutEnd*> pend; for (auto* E : {&*P.start, &*P.end}) if (E->a < 0) pend.push_back(E);
    std::stable_sort(pend.begin(), pend.end(), [](auto* x, auto* y) { return x->c == y->c ? (x->seg != y->seg ? x->seg > y->seg : x->u > y->u) : false; });
    for (auto* E : pend) { E->a = net.insertKeepShape(E->c, E->seg, E->u); E->pos = sc.anchor(E->a)->pos; E->vh.clear(); }
    sc.reindex();
    auto R = cutBuild(sc, P, Gc, sceneScale(sc)); if (auto* e = std::get_if<QString>(&R)) return "!" + *e;
    Build B = std::get<Build>(R); Ctx X{sc, P, Gc, B.mids}; cutCurveHandles(X, B, true);
    int a0 = B.F.pre.back(), a1 = B.F.post.front();
    // the curves running through the fork anchors keep their handles there (frozen), so the stretches along A and B stay
    for (int a : {a0, a1}) { double sm = sc.anchor(a)->smooth; if (!(sm > 0)) continue;
      std::vector<std::pair<int, Handles>> fr;
      for (const Item* h : sc.curves()) { int i = idxOf(h->anchors, a); if (i <= 0 || i >= int(h->anchors.size()) - 1 || h->ring >= 0) continue; fr.push_back({h->id, curveHandles(sc, *h, i)}); }
      for (auto& [hid, H] : fr) { Item* h = sc.item(hid); HandleOverride o; o.in = (H.hin - sc.anchor(a)->pos) / sm; o.out = (H.hout - sc.anchor(a)->pos) / sm; h->ovr[a] = o; }
    }
    const Item* A = sc.item(P.A); int Apar = A->parent; QString Aname = A->name;
    Item& c = net.makeCurve(Apar); int cid = c.id; c.name = Aname + " cut";
    std::vector<int> anchors;
    for (int id : B.anchors) { if (id <= -1000) { Anchor& n = sc.addAnchor(B.mids[size_t(-1000 - id)]); n.smooth = 1; anchors.push_back(n.id); } else anchors.push_back(id); }
    Item* cc2 = sc.item(cid); cc2->anchors = anchors;
    for (size_t k = 0; k < B.anchors.size(); k++) { auto f = B.h.find(B.anchors[k]); if (f == B.h.end()) continue; int a = anchors[k]; double sm = sc.anchor(a)->smooth; if (!(sm > 0)) continue;
      HandleOverride o; if (f->second.first) o.in = *f->second.first / sm; if (f->second.second) o.out = *f->second.second / sm; cc2->ovr[a] = o; }
    { auto& arr = Apar >= 0 ? sc.item(Apar)->children : sc.roots; arr.erase(std::remove(arr.begin(), arr.end(), cid), arr.end()); arr.insert(std::find(arr.begin(), arr.end(), P.A) + 1, cid); }
    const Item* sf = sc.item(P.sf); QJsonObject p = sf->raw.value("p").toObject(); int par = sf->parent; bool vis = sf->visible;
    for (auto [x, y] : {std::pair{P.A, cid}, std::pair{cid, P.B}}) { int n = edit::nextNumber(sc, ItemType::Surface); Item& w = sc.addItem(ItemType::Surface, par, QString("Surface %1").arg(n)); w.a = x; w.b = y; w.visible = vis; w.raw = QJsonObject{{"p", p}}; }
    net.deleteItem(P.sf);   // (after the new walls: a service curve of the old one's that they use stays — deleted with it before, the wall on it was never built)
    sc.reindex();
    sel.anchors.clear(); sel.items = {cid};
    return tr("Cut: %1 (%2 anchors) along the cut, the wall is split in two").arg(sc.item(cid)->name).arg(anchors.size());
  }, true);
  emit brushChanged();
}

}  // namespace hm
