// PatchTool: curves drawn on the references (LMB — anchors; RMB — an anchor picked / added on a curve, dragged), each
// making so many edges, laid evenly / adaptively / as the opposite side. Where curves cross — a corner; an end on the
// built mesh's open rim — joined to it, the rim between such ends taken into the contour. Every closed contour is shown
// filled with a grid (four sides — a grid, unequal sides — rows of changing length, other counts — split round a middle
// point into four-sided pieces); Enter makes it real.
#include "scene/MeshMods.h"
#include "scene/TopoEdit.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <deque>
#include <QFile>
#include <QDateTime>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <queue>
#include <numeric>
#include <set>

namespace hm {
using tedit::MeshEd;
void symAddFaces(MeshEd& E, int nf0);   // (ViewportTopoTools.cpp: Symmetry — the new polygons mirrored)

namespace {
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
const PolyMesh* baseOf(const Item& m) { return baseSp(m).get(); }
// the open rim of what is shown: an edge with one polygon in sight (the polygroups hidden left out — the visible part's
// edge, where the curves meet it, as the rim)
template <class T> std::vector<char> shownRim(const T& Tp, const Item& m) {
  const auto hid = meshmods::hiddenFaces(m, *baseOf(m)); auto vis = [&](int f) { return f >= 0 && (size_t(f) >= hid.size() || !hid[size_t(f)]); };
  std::vector<char> r(size_t(Tp.ne), 0); for (int e = 0; e < Tp.ne; e++) r[size_t(e)] = vis(Tp.ef0[size_t(e)]) != vis(Tp.ef1[size_t(e)]); return r; }
// a smooth curve through the points (Catmull-Rom), `per` samples a span
std::vector<Vec3> spline(const std::vector<Vec3>& a, int per) {
  const int n = int(a.size()); if (n < 2) return a;
  auto at = [&](int i) { return a[size_t(std::clamp(i, 0, n - 1))]; };
  std::vector<Vec3> out;
  for (int i = 0; i < n - 1; i++) { const Vec3 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
    for (int s = 0; s < per; s++) { const double t = double(s) / per, t2 = t * t, t3 = t2 * t;
      out.push_back((p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2 + (p1 * 3 - p0 - p2 * 3 + p3) * t3) * 0.5); } }
  out.push_back(a.back()); return out;
}
std::vector<double> cumOf(const std::vector<Vec3>& L) { std::vector<double> c(L.size(), 0.0); for (size_t i = 1; i < L.size(); i++) c[i] = c[i - 1] + L[i].distanceTo(L[i - 1]); return c; }
Vec3 atLen(const std::vector<Vec3>& L, const std::vector<double>& cum, double d) {
  if (L.empty()) return {}; if (L.size() == 1) return L[0];
  d = std::clamp(d, 0.0, cum.back()); size_t i = size_t(std::upper_bound(cum.begin(), cum.end(), d) - cum.begin()); i = std::clamp<size_t>(i, 1, L.size() - 1);
  const double seg = cum[i] - cum[i - 1]; return L[i - 1] + (L[i] - L[i - 1]) * (seg > 0 ? (d - cum[i - 1]) / seg : 0.0);
}
Vec3 atFrac(const std::vector<Vec3>& L, double f) { const auto c = cumOf(L); return atLen(L, c, f * (c.empty() ? 0 : c.back())); }
// n + 1 points along a polyline: evenly by length, or ('a') denser where it bends
// (aw: how much denser at the bends — 0 even along it, 1 Adaptive)
std::vector<Vec3> resampleW(const std::vector<Vec3>& L, int n, double aw) {
  n = std::max(1, n); if (L.size() < 2) return std::vector<Vec3>(size_t(n) + 1, L.empty() ? Vec3() : L[0]);
  std::vector<double> w(L.size(), 0.0);
  for (size_t i = 1; i < L.size(); i++) { double k = 1;
    if (aw > 0 && i + 1 < L.size()) { const Vec3 a = (L[i] - L[i - 1]).normalized(), b = (L[i + 1] - L[i]).normalized(); k = 1 + aw * 6 * std::acos(std::clamp(a.dot(b), -1.0, 1.0)) / M_PI * 8; }
    w[i] = w[i - 1] + L[i].distanceTo(L[i - 1]) * k; }
  std::vector<Vec3> out{L.front()};
  for (int s = 1; s < n; s++) { const double d = w.back() * s / n; size_t i = size_t(std::upper_bound(w.begin(), w.end(), d) - w.begin()); i = std::clamp<size_t>(i, 1, L.size() - 1);
    const double seg = w[i] - w[i - 1]; out.push_back(L[i - 1] + (L[i] - L[i - 1]) * (seg > 0 ? (d - w[i - 1]) / seg : 0.0)); }
  out.push_back(L.back()); return out;
}
std::vector<Vec3> resample(const std::vector<Vec3>& L, int n, char mode) { return resampleW(L, n, mode == 'a' ? 1.0 : 0.0); }
// the closest points of two segments → the distance, their places along them
double segSeg(const Vec3& p0, const Vec3& p1, const Vec3& q0, const Vec3& q1, double& s, double& t) {
  const Vec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0; const double a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(r);
  if (a < 1e-30 && e < 1e-30) { s = t = 0; return r.length(); }
  if (a < 1e-30) { s = 0; t = std::clamp(f / e, 0.0, 1.0); }
  else { const double c = d1.dot(r); if (e < 1e-30) { t = 0; s = std::clamp(-c / a, 0.0, 1.0); }
    else { const double b = d1.dot(d2), den = a * e - b * b; s = den > 1e-30 ? std::clamp((b * f - c * e) / den, 0.0, 1.0) : 0.0; t = (b * s + f) / e;
      if (t < 0) { t = 0; s = std::clamp(-c / a, 0.0, 1.0); } else if (t > 1) { t = 1; s = std::clamp((b - c) / a, 0.0, 1.0); } } }
  return ((p0 + d1 * s) - (q0 + d2 * t)).length();
}
bool inPoly2(const std::vector<std::array<double, 2>>& P, double x, double y) {
  bool in = false; for (size_t i = 0, j = P.size() - 1; i < P.size(); j = i++) if ((P[i][1] > y) != (P[j][1] > y) && x < (P[j][0] - P[i][0]) * (y - P[i][1]) / (P[j][1] - P[i][1]) + P[i][0]) in = !in;
  return in;
}
}  // namespace

// ---------- where things are ----------
int Viewport::patchTarget() const {
  const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || it->type != ItemType::IMesh) return -1;
  const PolyMesh* B = baseOf(*it); if (!B || B->faceCount() > 150000 || it->raw.value("tghost").toBool() || it->raw.value("tref").toBool()) return -1;   // (a reference is not built on)
  return m;
}
Vec3 Viewport::patchOnRefs(const Vec3& p) const {
  const int tm = patchTarget(); double bd = 1e300; Vec3 best = p;
  for (auto& [id, x] : scene_->items) {
    if (id == tm || x.type != ItemType::IMesh || !x.poly || !scene_->isVisible(x)) continue;
    auto h = heavy_.find(id);
    if (h != heavy_.end() && h->second.bvh && h->second.src.lock() == baseSp(x)) {   // (a high-poly one: its own hierarchy, in its file's space)
      const Affine M = meshmods::meshMatrix(*scene_, x), Mi = M.inverse(); double d = 0;
      if (auto q = h->second.bvh->closest(Mi.apply(p), nullptr, &d)) { const Vec3 w = M.apply(*q); const double dw = w.distanceTo(p); if (dw < bd) { bd = dw; best = w; } }
      continue; }
    if ((long long)x.poly->faceVerts.size() - 2LL * x.poly->faceCount() > 400000) continue;
    if (auto b = bvhOf(id, false)) { double d = 0; if (auto q = b->closest(p, nullptr, &d)) if (d < bd) { bd = d; best = *q; } }
  }
  return best;
}
std::optional<Vec3> Viewport::patchSnap(const QPointF& mp, int skip) const {
  Projector P = projector(); double bd = 12; std::optional<Vec3> best;
  for (size_t c = 0; c < pt_.c.size(); c++) { if (int(c) == skip) continue;   // (another curve's anchor)
    for (const Vec3& a : pt_.c[c].a) { const ScreenPt s = P.toScreen(a); const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (s.z < 1 && d < bd) { bd = d; best = a; } } }
  if (skip >= 0 && size_t(skip) < pt_.c.size() && pt_.c[size_t(skip)].a.size() >= 3) {   // (its own first anchor: a loop of it)
    const Vec3& a = pt_.c[size_t(skip)].a.front(); const ScreenPt s = P.toScreen(a); const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (s.z < 1 && d < bd) { bd = d; best = a; } }
  if (best) return best;
  if (const int tm = patchTarget(); tm >= 0) {                  // (the built mesh's rim vertex)
    const Item* it = scene_->item(tm); auto T = topo::ttopo(baseSp(*it)); const auto W = ttWorld(tm); bd = 12;
    const auto rim = shownRim(*T, *it); for (int e = 0; e < T->ne; e++) if (rim[size_t(e)]) for (int v : {T->ea[size_t(e)], T->eb[size_t(e)]}) {
      const ScreenPt s = P.toScreen(W[size_t(v)]); const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (s.z < 1 && d < bd) { bd = d; best = W[size_t(v)]; } }
    if (best) return best; }
  bd = 8;                                                       // (on another curve — a crossing there)
  for (size_t c = 0; c < pt_.sampled.size(); c++) { if (int(c) == skip) continue; const auto& L = pt_.sampled[c];
    for (size_t i = 0; i + 1 < L.size(); i++) { const ScreenPt a = P.toScreen(L[i]), b = P.toScreen(L[i + 1]); if (a.z >= 1 || b.z >= 1) continue;
      const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? std::clamp(((mp.x() - a.x) * dx + (mp.y() - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
      const double d = std::hypot(a.x + dx * t - mp.x(), a.y + dy * t - mp.y()); if (d < bd) { bd = d; best = L[i] + (L[i + 1] - L[i]) * t; } } }
  if (best) return best;
  if (auto h = pickSurface(mp)) return h->point;               // (the surface under the pointer)
  return std::nullopt;
}

// a curve through its anchors laid on what the view sees under it (the references overlap — the body under the clothes:
// the one in sight); where the view sees something far off it (a lock in front) — the nearest surface
std::vector<Vec3> Viewport::patchSample(const std::vector<Vec3>& a, const std::vector<Vec3>& prevA, const std::vector<Vec3>& prevL) const {
  auto L = spline(a, 16); if (L.size() < 2) return L;
  // (a span between two anchors depends on those and their neighbours only: one whose four are as they were when the
  // curve was last laid keeps its points as laid then — the view since turned away, a hand now in front of it, changes
  // nothing of it; only the spans by an anchor added or moved are laid anew)
  const int n = int(a.size()), pn = int(prevA.size()); std::vector<char> kept(L.size(), 0);
  if (pn >= 2 && int(prevL.size()) == (pn - 1) * 16 + 1) {
    using K = std::array<double, 12>;
    auto keyOf = [](const std::vector<Vec3>& A, int i) { const int m = int(A.size()); K k; for (int j = 0; j < 4; j++) { const Vec3& p = A[size_t(std::clamp(i - 1 + j, 0, m - 1))]; k[size_t(j * 3)] = p.x; k[size_t(j * 3 + 1)] = p.y; k[size_t(j * 3 + 2)] = p.z; } return k; };
    std::map<K, int> was; for (int i = 0; i + 1 < pn; i++) was[keyOf(prevA, i)] = i;
    for (int i = 0; i + 1 < n; i++) { auto f = was.find(keyOf(a, i)); if (f == was.end()) continue;
      for (int s = 0; s < 16; s++) { L[size_t(i * 16 + s)] = prevL[size_t(f->second * 16 + s)]; kept[size_t(i * 16 + s)] = 1; } }
  }
  Projector P = projector(); double chord = 0; for (size_t i = 1; i < a.size(); i++) chord += a[i].distanceTo(a[i - 1]);
  const double far = std::max(1e-6, chord * 0.6);   // (a span across a rounded surface sags inside it — the view's hit is still the one)
  for (size_t i = 1; i + 1 < L.size(); i++) { if (kept[i]) continue; const ScreenPt s = P.toScreen(L[i]); std::optional<RayHit> h;
    if (s.z < 1 && s.x >= 0 && s.y >= 0 && s.x < width() && s.y < height()) h = pickSurface(QPointF(s.x, s.y));
    const Vec3 q = patchOnRefs(L[i]);
    // (the view's hit only when it is about as near as the nearest surface: one well past it is something in front — a
    // brim, a fold over the line — that the curve would jump onto)
    const size_t k = std::min(i / 16, a.size() - 2); const double step = a[k].distanceTo(a[k + 1]) / 16;
    L[i] = h && h->point.distanceTo(L[i]) < far && h->point.distanceTo(L[i]) <= std::max(3 * q.distanceTo(L[i]), step * 2) ? h->point : q; }
  return L;
}

// ---------- the curves into contours, the contours into grids ----------
void Viewport::patchBuild() {
  PatchSt& S = pt_;
  const int tm = patchTarget(); const Item* ti = tm >= 0 ? scene_->item(tm) : nullptr;
  QByteArray key = QByteArray::number(tm) + "|" + QByteArray::number(quintptr(ti ? baseOf(*ti) : nullptr)) + "|" + QByteArray::number(geomStamp_.count(tm) ? geomStamp_.at(tm) : 0) + (S.zoneAlt ? "z" : "-") + (S.toDense ? "d" : "-") + (S.triFan ? "f" : "-") + (S.rimShort ? "r" : "-");
  key += QByteArray(1, S.kind) + QByteArray::number(S.stripW, 'g', 8) + "x" + QByteArray::number(S.stripAcross); for (auto& [k, r] : S.loftRows) key += "," + QByteArray::number(k.first) + "-" + QByteArray::number(k.second) + ":" + QByteArray::number(r);
  for (auto& C : S.c) { if (C.nearLay) key += QByteArray::number(qHash(QByteArray::number(cam_.pos.x) + QByteArray::number(cam_.pos.y) + QByteArray::number(cam_.pos.z) + QByteArray::number(cam_.dir().x)));   // (not laid yet: built again as the view moves)
    key += C.vids.empty() ? "" : "v";
    key += "|" + QByteArray::number(C.div) + C.mode; for (auto& p : C.a) key += QByteArray::number(p.x, 'g', 10) + "," + QByteArray::number(p.y, 'g', 10) + "," + QByteArray::number(p.z, 'g', 10) + ";"; }
  if (key == S.key) return;
  S.key = key;
  // (built a few ways till every curve closes a zone: the ends onto the rim within a third of its edge — any screen size —
  // or only within the curves' spacing; a curve's end a few pixels off a bare corner of the rim kept apart, or put on it —
  // else it cuts across the rim there; none closing all: the one losing fewest)
  int lost = 0;
  auto once = [&](bool jog, bool wide) {
  lost = 0; S.P.clear(); S.vOf.clear(); S.F.clear(); S.sampled.assign(S.c.size(), {}); S.zone.clear(); S.target = tm; S.patches = 0; S.note.clear(); S.zoneChoice = false;
  // the curves on the surface
  double spacing = 0; int ns = 0;
  for (size_t i = 0; i < S.c.size(); i++) if (S.c[i].a.size() >= 2) {
    auto& C = S.c[i]; QByteArray ak; for (auto& p : C.a) ak += QByteArray::number(p.x, 'g', 12) + "," + QByteArray::number(p.y, 'g', 12) + "," + QByteArray::number(p.z, 'g', 12) + ";";
    if (!C.vids.empty()) { C.L = C.a; C.lk = ak; C.la = C.a; C.nearLay = false; }   // (an edge curve: on the mesh's edges exactly — not laid by the view)
    else     if (ak != C.lk || C.nearLay) {                                // (one from an older file, no laid points: laid by the view once it is all in sight — till then its anchors' spline)
      bool seen = true; if (C.nearLay) { Projector Pv = projector(); for (const Vec3& p : C.a) { const ScreenPt s = Pv.toScreen(p); if (s.z >= 1 || s.x < 0 || s.y < 0 || s.x >= width() || s.y >= height()) seen = false; } }
      if (seen) { C.L = patchSample(C.a, C.nearLay ? std::vector<Vec3>{} : C.la, C.nearLay ? std::vector<Vec3>{} : C.L); C.la = C.a; C.lk = ak; C.nearLay = false; } else { C.L = spline(C.a, 16); C.lk.clear(); C.la.clear(); } }
    const auto& L = C.L;
    if (C.edgeLen > 0 && L.size() >= 2) { C.div = std::clamp(int(std::lround(cumOf(L).back() / C.edgeLen)), 1, 200); if (S.draw != int(i)) C.edgeLen = 0; }   // (drawn after a picked curve: as many edges as its length takes of that one's — fixed once it ends)
    for (size_t k = 1; k < L.size(); k++) { spacing += L[k].distanceTo(L[k - 1]); ns++; } S.sampled[i] = L; }
  patchStore();                                                 // (the curves changed: kept with the project — their laid points too)
  if (!ns) return;
  if (S.kind != 'p') return;                                    // (Loft / Strip: built from the curves alone — no zones looked for)
  spacing /= ns; const double tol = std::max(1e-9, spacing * 1.5), tolE = tol * 1.2;   // (the anchors snap onto curves and the rim exactly: the ends found there within little more than the crossings' tolerance)
  Box3 span; for (auto& L : S.sampled) for (auto& p : L) span.expand(p); const double far = std::max(spacing * 12, (span.max - span.min).length() * 0.5);
  Projector Pj = projector();   // (a point inside a patch: onto what the view sees there, near it — else the nearest surface)
  bool tubeOn = false;          // (a tube's: round the body, half of it out of sight — the nearest surface always)
  auto stick = [&](const Vec3& p) { const Vec3 q = patchOnRefs(p); if (tubeOn) return q; const ScreenPt s = Pj.toScreen(p);
    // (the view's hit only when about as near as the nearest surface — not a brim or a fold in front of the patch)
    if (s.z < 1 && s.x >= 0 && s.y >= 0 && s.x < width() && s.y < height()) if (auto h = pickSurface(QPointF(s.x, s.y)); h && h->point.distanceTo(p) < far && h->point.distanceTo(p) <= std::max(3 * q.distanceTo(p), spacing * 2)) return h->point;
    return q; };
  // tubes: two rings (closed curves) — the nearest pairs of them, as a chain (shoulder–elbow, elbow–wrist) — a cylinder
  // between them; a curve from one to the other its seam (where the rings' rows start) and its height (its edges). They
  // and their seams are no part of the contours on the screen (a ring round a body overlaps itself in any view)
  struct Tube { int r1, r2, seam = -1; };
  std::vector<Tube> tubes; std::vector<std::vector<Vec3>> tubeKeep;   // (tubeKeep: the curves taken out of the contours — put back at the end)
  auto ringClosed = [&](size_t i) { const auto& C = S.c[i]; return C.vids.empty() && C.a.size() >= 4 && C.a.front().distanceTo(C.a.back()) < 1e-9 && S.sampled[i].size() >= 4; };
  auto nearOnL = [](const std::vector<Vec3>& B, const Vec3& q, double& at) { const auto cb = cumOf(B); double bd = 1e300;
    for (size_t b = 0; b + 1 < B.size(); b++) { const Vec3 d = B[b + 1] - B[b]; const double l2 = d.dot(d), t = l2 > 0 ? std::clamp((q - B[b]).dot(d) / l2, 0.0, 1.0) : 0.0;
      const double dd = (B[b] + d * t).distanceTo(q); if (dd < bd) { bd = dd; at = cb[b] + (cb[b + 1] - cb[b]) * t; } } return bd; };
  auto mid = [&](size_t i) { Vec3 c; const auto& L = S.sampled[i]; for (size_t k = 0; k + 1 < L.size(); k++) c += L[k]; return c * (1.0 / double(L.size() - 1)); };
  { std::vector<size_t> rings; for (size_t i = 0; i < S.c.size(); i++) if (ringClosed(i)) rings.push_back(i);
    if (rings.size() >= 2) {
      std::vector<char> in(rings.size(), 0); in[0] = 1;              // (the pairs: the shortest links joining them all — Prim's)
      for (size_t step = 1; step < rings.size(); step++) { double bd = 1e300; size_t ba = 0, bb = 0;
        for (size_t x = 0; x < rings.size(); x++) if (in[x]) for (size_t y = 0; y < rings.size(); y++) if (!in[y]) { const double d = mid(rings[x]).distanceTo(mid(rings[y])); if (d < bd) { bd = d; ba = x; bb = y; } }
        in[bb] = 1; tubes.push_back({int(rings[ba]), int(rings[bb])}); }
      for (auto& t : tubes) for (size_t i = 0; i < S.c.size(); i++) { if (ringClosed(i) || S.sampled[i].size() < 2) continue; const auto& L = S.sampled[i]; double at = 0;
        const bool f1 = nearOnL(S.sampled[size_t(t.r1)], L.front(), at) < tolE, b2 = nearOnL(S.sampled[size_t(t.r2)], L.back(), at) < tolE;
        const bool f2 = nearOnL(S.sampled[size_t(t.r2)], L.front(), at) < tolE, b1 = nearOnL(S.sampled[size_t(t.r1)], L.back(), at) < tolE;
        if ((f1 && b2) || (f2 && b1)) { t.seam = int(i); break; } }
      tubeKeep = S.sampled; for (auto& t : tubes) { S.sampled[size_t(t.r1)].clear(); S.sampled[size_t(t.r2)].clear(); if (t.seam >= 0) S.sampled[size_t(t.seam)].clear(); } } }
  // the nodes (merged as they meet); a node on the built mesh's rim keeps its vertex
  struct Node { Vec3 p; int vid = -1; }; std::vector<Node> nodes; std::vector<int> up;
  std::function<int(int)> root = [&](int x) { while (up[size_t(x)] != x) x = up[size_t(x)] = up[size_t(up[size_t(x)])]; return x; };
  auto unite = [&](int a, int b) { a = root(a); b = root(b); if (a == b) return; if (nodes[size_t(b)].vid >= 0) std::swap(a, b); up[size_t(b)] = a; };
  auto nodeAt = [&](const Vec3& p, int vid) {
    for (size_t k = 0; k < nodes.size(); k++) { if (vid >= 0 && nodes[k].vid == vid) return root(int(k)); if (vid < 0 && nodes[k].p.distanceTo(p) < tol) return root(int(k)); }
    nodes.push_back({p, vid}); up.push_back(int(nodes.size()) - 1); return int(nodes.size()) - 1; };
  // the built mesh's open rim: its loops
  std::vector<Vec3> W; std::vector<std::vector<int>> loops; std::vector<Vec3> meshMid;
  std::vector<std::vector<int>> adj;                               // (every edge of the mesh: the shortest way between two rim points)
  if (ti) {
    auto T = topo::ttopo(baseSp(*ti)); W = ttWorld(tm);
    if (S.rimShort) { adj.assign(size_t(T->n), {}); for (int e = 0; e < T->ne; e++) { adj[size_t(T->ea[size_t(e)])].push_back(T->eb[size_t(e)]); adj[size_t(T->eb[size_t(e)])].push_back(T->ea[size_t(e)]); } }
    std::vector<std::vector<int>> nb(size_t(T->n)); const auto rim = shownRim(*T, *ti); for (int e = 0; e < T->ne; e++) if (rim[size_t(e)]) { nb[size_t(T->ea[size_t(e)])].push_back(T->eb[size_t(e)]); nb[size_t(T->eb[size_t(e)])].push_back(T->ea[size_t(e)]); }
    std::set<std::pair<int, int>> used;
    for (int v0 = 0; v0 < T->n; v0++) for (int first : nb[size_t(v0)]) { if (used.count({std::min(v0, first), std::max(v0, first)})) continue;
      std::vector<int> L{v0}; int prev = v0, cur = first; used.insert({std::min(v0, first), std::max(v0, first)});
      for (int guard = 0; cur != v0 && guard < T->n + 2; guard++) { L.push_back(cur); int nx = -1;
        for (int w : nb[size_t(cur)]) if (w != prev && !used.count({std::min(cur, w), std::max(cur, w)})) { nx = w; break; }
        if (nx < 0) break; used.insert({std::min(cur, nx), std::max(cur, nx)}); prev = cur; cur = nx; }
      if (cur == v0 && L.size() >= 3) loops.push_back(L); }
    // (its polygons turned to the camera — the far side of it, seen through, builds nothing on this side)
    const PolyMesh& B = *baseOf(*ti); const auto hid = meshmods::hiddenFaces(*ti, B); for (int f = 0; f < B.faceCount(); f++) { if (size_t(f) < hid.size() && hid[size_t(f)]) continue; Vec3 c, n; const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1];
      for (int k = s; k < e; k++) { const Vec3& a = W[size_t(B.faceVerts[size_t(k)])]; const Vec3& b = W[size_t(B.faceVerts[size_t(k + 1 < e ? k + 1 : s)])]; c += a; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
      c = c * (1.0 / (e - s)); if (n.dot(cam_.ortho() ? cam_.dir() : c - cam_.pos) < 0) meshMid.push_back(c); }
  }
  std::set<int> rimV; for (auto& L : loops) for (int v : L) rimV.insert(v);
  std::map<int, int> loopOf; for (size_t li = 0; li < loops.size(); li++) for (int v : loops[li]) loopOf.emplace(v, int(li));
  double rimEdge = 0; { int k = 0; for (auto& L : loops) for (size_t i = 0; i < L.size(); i++) { rimEdge += W[size_t(L[i])].distanceTo(W[size_t(L[(i + 1) % L.size()])]); k++; } if (k) rimEdge /= k; }
  // the curves cut where they meet: their ends, their crossings, an end on another curve, an end on the rim
  const size_t nc = S.c.size(); std::vector<std::vector<double>> cums(nc); std::vector<std::vector<std::pair<double, int>>> cuts(nc);
  for (size_t i = 0; i < nc; i++) { const auto& L = S.sampled[i]; if (L.size() < 2) continue; cums[i] = cumOf(L);
    // (each end's rim vertices near enough — in space, else on screen within 12 px as the anchors snap; the nearest taken,
    // but two ends near two rims — a curve down a narrow gap between two parts — both onto one rim when they can be:
    // a zone closes only along one rim)
    std::array<std::vector<std::pair<double, int>>, 2> cand;
    for (int end = 0; end < 2; end++) { const Vec3 q = end ? L.back() : L.front(); const double bd = wide ? std::max(tolE, rimEdge * 0.3) : tolE;   // (wide: within a third of the rim's edge — on it, whatever the screen's size)
      for (int v : rimV) { const double d = W[size_t(v)].distanceTo(q); if (d < bd) cand[size_t(end)].push_back({d / bd, v}); }
      if (cand[size_t(end)].empty()) { const ScreenPt sq = Pj.toScreen(q);
        if (sq.z < 1) for (int v : rimV) { const ScreenPt s = Pj.toScreen(W[size_t(v)]); if (s.z >= 1) continue; const double dp = std::hypot(s.x - sq.x, s.y - sq.y);
          if (dp < 12 && W[size_t(v)].distanceTo(q) < spacing * 20) cand[size_t(end)].push_back({1 + dp / 12, v}); } }
      std::sort(cand[size_t(end)].begin(), cand[size_t(end)].end()); }
    int vids[2] = {cand[0].empty() ? -1 : cand[0][0].second, cand[1].empty() ? -1 : cand[1][0].second};
    if (vids[0] >= 0 && vids[1] >= 0 && loopOf.count(vids[0]) && loopOf.count(vids[1]) && loopOf[vids[0]] != loopOf[vids[1]]) {
      double bs = 1e300; for (auto& [sa, va] : cand[0]) for (auto& [sb, vb] : cand[1]) if (loopOf.count(va) && loopOf.count(vb) && loopOf[va] == loopOf[vb] && sa + sb < bs) { bs = sa + sb; vids[0] = va; vids[1] = vb; } }
    for (int end = 0; end < 2; end++) { const Vec3 q = end ? L.back() : L.front(); const int vid = vids[end];
      cuts[i].push_back({end ? cums[i].back() : 0.0, nodeAt(vid >= 0 ? W[size_t(vid)] : q, vid)}); }
    // (both ends on the rim — a strip along it: Match, the rim's vertices across from the curve's points; one end or
    // none — a curve going off on its own: Equal)
    if (S.c[i].modeAuto) { int onRim = 0; for (auto& k : cuts[i]) onRim += nodes[size_t(k.second)].vid >= 0; S.c[i].mw = onRim >= 2 ? 1 : 0; S.c[i].aw = 0; S.c[i].mode = onRim >= 2 ? 'm' : 'e'; } }
  for (size_t i = 0; i < nc; i++) for (size_t j = i + 1; j < nc; j++) { const auto &A = S.sampled[i], &B = S.sampled[j]; if (A.size() < 2 || B.size() < 2) continue;
    // (where they come within the tolerance: each run of such places one crossing — at their nearest, not where they
    // first came near: two curves meeting at a slant run close for a while, and the corner would land short of the crossing)
    struct Hit { double sa, sb, d; }; std::vector<Hit> hits;
    for (size_t a = 0; a + 1 < A.size(); a++) for (size_t b = 0; b + 1 < B.size(); b++) { double s, t; double d = segSeg(A[a], A[a + 1], B[b], B[b + 1], s, t); if (d >= tol) continue;
      const double sa = cums[i][a] + (cums[i][a + 1] - cums[i][a]) * s, sb = cums[j][b] + (cums[j][b + 1] - cums[j][b]) * t;
      { const double toEnd = std::min({sa, cums[i].back() - sa, sb, cums[j].back() - sb}); if (toEnd < tolE) d = -1 + 0.5 * toEnd / tolE; }   // (near an end of either: the end itself — two curves leaving one point together meet there)
      Hit* same = nullptr; for (auto& h : hits) if (std::abs(h.sa - sa) < tolE * 2 && std::abs(h.sb - sb) < tolE * 2) same = &h;
      if (!same) hits.push_back({sa, sb, d}); else if (d < same->d) *same = {sa, sb, d}; }
    for (auto& h : hits) { const int nd = nodeAt((atLen(A, cums[i], h.sa) + atLen(B, cums[j], h.sb)) * 0.5, -1); cuts[i].push_back({h.sa, nd}); cuts[j].push_back({h.sb, nd}); } }
  for (size_t i = 0; i < nc; i++) for (int end = 0; end < 2; end++) { const auto& L = S.sampled[i]; if (L.size() < 2) continue; const Vec3 q = end ? L.back() : L.front();   // (an end on another curve's body)
    for (size_t j = 0; j < nc; j++) { if (j == i || S.sampled[j].size() < 2) continue; const auto& B = S.sampled[j]; double bd = tolE, bs = -1;
      for (size_t b = 0; b + 1 < B.size(); b++) { const Vec3 d = B[b + 1] - B[b]; const double l2 = d.dot(d), t = l2 > 0 ? std::clamp((q - B[b]).dot(d) / l2, 0.0, 1.0) : 0.0;
        const double dist = (B[b] + d * t).distanceTo(q); if (dist < bd) { bd = dist; bs = cums[j][b] + (cums[j][b + 1] - cums[j][b]) * t; } }
      if (bs < 0) {                                              // (not quite on it in space, but on it on screen — within 10 px, near enough: a corner there)
        const ScreenPt sq = Pj.toScreen(q); double bp = 10;
        if (sq.z < 1) for (size_t b = 0; b + 1 < B.size(); b++) { const ScreenPt a = Pj.toScreen(B[b]), c = Pj.toScreen(B[b + 1]); if (a.z >= 1 || c.z >= 1) continue;
          const double dx = c.x - a.x, dy = c.y - a.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? std::clamp(((sq.x - a.x) * dx + (sq.y - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
          const double dp = std::hypot(a.x + dx * t - sq.x, a.y + dy * t - sq.y); if (dp < bp && (B[b] + (B[b + 1] - B[b]) * t).distanceTo(q) < spacing * 20) { bp = dp; bs = cums[j][b] + (cums[j][b + 1] - cums[j][b]) * t; } } }
      if (bs >= 0) cuts[j].push_back({bs, nodeAt(q, -1)}); } }
  // the edges: the curves' pieces between their cuts, the rim's pieces between where curves meet it
  struct Edge { int a, b; std::vector<Vec3> L; std::vector<int> vids; int curve = -1; double len = 0; bool alive = true; };
  std::vector<Edge> E;
  for (size_t i = 0; i < nc; i++) { auto& K = cuts[i]; if (K.size() < 2) continue; for (auto& k : K) k.second = root(k.second);
    std::sort(K.begin(), K.end());
    std::vector<std::pair<double, int>> U; for (auto& k : K) { if (!U.empty() && (U.back().second == k.second || k.first - U.back().first < tol * 0.5)) { unite(U.back().second, k.second); continue; } U.push_back(k); }
    for (size_t k = 0; k + 1 < U.size(); k++) { const double s0 = U[k].first, s1 = U[k + 1].first; if (s1 - s0 < tol * 0.5) continue;
      Edge e; e.curve = int(i); e.a = U[k].second; e.b = U[k + 1].second; e.L.push_back(atLen(S.sampled[i], cums[i], s0));
      for (size_t q = 0; q < S.sampled[i].size(); q++) if (cums[i][q] > s0 && cums[i][q] < s1) e.L.push_back(S.sampled[i][q]);
      e.L.push_back(atLen(S.sampled[i], cums[i], s1)); e.len = s1 - s0;
      if (!S.c[i].vids.empty()) for (const Vec3& p : e.L) { int vv = -1; for (size_t q = 0; q < S.c[i].a.size(); q++) if (S.c[i].a[q].distanceTo(p) < 1e-9) { vv = S.c[i].vids[q]; break; } e.vids.push_back(vv); }   // (an edge curve: the mesh's vertices it runs on)
      E.push_back(std::move(e)); } }
  std::set<int> endNode; for (auto& K : cuts) for (auto& k : K) endNode.insert(root(k.second));   // (the nodes curves reach — the rim's own corners are none of them)
  for (auto& L : loops) {
    const size_t n = L.size(); std::vector<int> at;
    for (size_t k = 0; k < n; k++) for (size_t q = 0; q < nodes.size(); q++) if (nodes[q].vid == L[k]) { at.push_back(int(k)); break; }
    if (at.size() < 2) continue;
    // (a sharp turn of the rim between them: a corner there too — not when the shortest edges are taken instead of it)
    std::set<int> atS(at.begin(), at.end());
    if (!S.rimShort) for (size_t k = 0; k < n; k++) { const Vec3 a = (W[size_t(L[k])] - W[size_t(L[(k + n - 1) % n])]).normalized(), b = (W[size_t(L[(k + 1) % n])] - W[size_t(L[k])]).normalized();
      if (std::acos(std::clamp(a.dot(b), -1.0, 1.0)) > 55 * M_PI / 180 && !atS.count(int(k))) { atS.insert(int(k)); nodeAt(W[size_t(L[k])], L[k]); } }
    at.assign(atS.begin(), atS.end());
    // (the shortest way along the mesh's edges between two rim points — across a tongue sticking out of the rim by the
    // edges at its root; Dijkstra by the edges' lengths)
    auto shortest = [&](int s, int t) { std::vector<double> d(adj.size(), 1e300); std::vector<int> pr(adj.size(), -1); using QE = std::pair<double, int>;
      std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq; d[size_t(s)] = 0; pq.push({0, s});
      while (!pq.empty()) { auto [dd, v] = pq.top(); pq.pop(); if (dd > d[size_t(v)]) continue; if (v == t) break;
        for (int w : adj[size_t(v)]) { const double nd = dd + W[size_t(v)].distanceTo(W[size_t(w)]); if (nd < d[size_t(w)]) { d[size_t(w)] = nd; pr[size_t(w)] = v; pq.push({nd, w}); } } }
      std::vector<int> p; if (d[size_t(t)] >= 1e300) return p; for (int v = t; v >= 0; v = pr[size_t(v)]) p.push_back(v); std::reverse(p.begin(), p.end()); return p; };
    for (size_t q = 0; q < at.size(); q++) { const int k0 = at[q], k1 = at[(q + 1) % at.size()]; Edge e;
      for (int k = k0;; k = (k + 1) % int(n)) { e.L.push_back(W[size_t(L[size_t(k)])]); e.vids.push_back(L[size_t(k)]); if (k == k1 && e.vids.size() > 1) break; if (e.vids.size() > n + 1) break; }
      if (S.rimShort && !adj.empty() && L[size_t(k0)] != L[size_t(k1)]) { auto p = shortest(L[size_t(k0)], L[size_t(k1)]);
        if (p.size() >= 2 && p.size() < e.vids.size()) { e.vids = p; e.L.clear(); for (int v : p) e.L.push_back(W[size_t(v)]); } }
      e.a = nodeAt(W[size_t(L[size_t(k0)])], L[size_t(k0)]); e.b = nodeAt(W[size_t(L[size_t(k1)])], L[size_t(k1)]);
      { double px = 0; ScreenPt s0 = Pj.toScreen(e.L.front()); bool seen = s0.z < 1;   // (a rim piece of a few pixels — a jog at a corner, a curve's end just off it: its ends one corner, else
        for (size_t k = 1; k < e.L.size(); k++) { const ScreenPt s1 = Pj.toScreen(e.L[k]); seen = seen && s1.z < 1; px += std::hypot(s1.x - s0.x, s1.y - s0.y); s0 = s1; }   //  the curve crosses the rim there and the zone it closes is lost)
        const ScreenPt sa = Pj.toScreen(e.L.front()), sb = Pj.toScreen(e.L.back());
        const bool oneEnd = endNode.count(root(e.a)) != endNode.count(root(e.b));   // (a curve's end beside a bare corner — never two curves' ends made one)
        if (jog && seen && oneEnd && std::hypot(sa.x - sb.x, sa.y - sb.y) < 16 && px < 60 && e.vids.size() <= 4 && L.size() > 8) { unite(e.a, e.b); continue; } }
      for (size_t k = 1; k < e.L.size(); k++) e.len += e.L[k].distanceTo(e.L[k - 1]); E.push_back(std::move(e)); } }
  for (auto& e : E) { e.a = root(e.a); e.b = root(e.b); if (e.a == e.b) e.alive = false; }
  // (the rim's piece an edge curve runs along, the same vertices: the curve stands for it)
  for (auto& c : E) if (c.curve >= 0 && !c.vids.empty() && c.alive) { auto r = c.vids; std::reverse(r.begin(), r.end());
    for (auto& e : E) if (e.curve < 0 && e.alive && (e.vids == c.vids || e.vids == r)) e.alive = false; }
  // (a piece hanging free — one of its ends reaching nothing more: no contour goes through it)
  for (bool again = true; again;) { again = false; std::map<int, int> deg; for (auto& e : E) if (e.alive) { deg[e.a]++; deg[e.b]++; }
    for (auto& e : E) if (e.alive && (deg[e.a] < 2 || deg[e.b] < 2)) { e.alive = false; again = true; } }
  // the contours as the view sees them: on the screen (x right, y up) — a long, nearly straight strip along a rim kept
  // its width, as no plane of the curves' own spread would
  int np = 0; for (auto& e : E) if (e.alive) np += int(e.L.size());
  if (!np && tubes.empty()) { S.note = tr("Draw curves that close a contour (or meet the mesh's open rim)"); return; }
  auto to2 = [&](const Vec3& p) { const ScreenPt s = Pj.toScreen(p); return std::array<double, 2>{s.x, -s.y}; };
  // the faces of the drawing: round each node its pieces by their direction; a face — each piece turned into the next
  // one clockwise of where it came back from
  const int nh = int(E.size()) * 2; auto hStart = [&](int h) { return (h & 1) ? E[size_t(h >> 1)].b : E[size_t(h >> 1)].a; };
  auto hEnd = [&](int h) { return (h & 1) ? E[size_t(h >> 1)].a : E[size_t(h >> 1)].b; };
  auto hPts = [&](int h) { auto L = E[size_t(h >> 1)].L; if (h & 1) std::reverse(L.begin(), L.end()); return L; };
  auto hDir = [&](int h, bool atEnd) { const auto L = hPts(h); const size_t n = L.size(); const auto a = to2(atEnd ? L[n - 1 - std::min<size_t>(3, n - 1)] : L[0]), b = to2(atEnd ? L[n - 1] : L[std::min<size_t>(3, n - 1)]);
    return std::array<double, 2>{b[0] - a[0], b[1] - a[1]}; };
  std::map<int, std::vector<int>> outs; for (int h = 0; h < nh; h++) if (E[size_t(h >> 1)].alive) outs[hStart(h)].push_back(h);
  for (auto& [v, L] : outs) std::sort(L.begin(), L.end(), [&](int a, int b) { const auto da = hDir(a, false), db = hDir(b, false); return std::atan2(da[1], da[0]) < std::atan2(db[1], db[0]); });
  auto next = [&](int h) { const int v = hEnd(h), tw = h ^ 1; const auto& L = outs[v]; const auto it = std::find(L.begin(), L.end(), tw); if (it == L.end()) return -1;
    const size_t i = size_t(it - L.begin()); return L[(i + L.size() - 1) % L.size()]; };
  struct Face { std::vector<int> hs; double area = 0; bool rim = false, ok = true, bare = false; std::vector<std::array<double, 2>> poly; };
  std::vector<Face> faces; std::vector<char> seen(size_t(nh), 0);
  for (int h0 = 0; h0 < nh; h0++) { if (seen[size_t(h0)] || !E[size_t(h0 >> 1)].alive) continue; Face f; int h = h0;
    for (int guard = 0; h >= 0 && !seen[size_t(h)] && guard < nh + 1; guard++) { seen[size_t(h)] = 1; f.hs.push_back(h); h = next(h); }
    if (h != h0 || f.hs.size() < 2) continue;
    for (int hh : f.hs) { auto L = hPts(hh); for (size_t k = 0; k + 1 < L.size(); k++) f.poly.push_back(to2(L[k])); if (E[size_t(hh >> 1)].curve < 0 || !E[size_t(hh >> 1)].vids.empty()) f.rim = true; if (E[size_t(hh >> 1)].curve < 0) f.bare = true; }   // (an edge curve: the rim; bare — the rim itself in it)
    for (size_t k = 0, j = f.poly.size() - 1; k < f.poly.size(); j = k++) f.area += (f.poly[j][0] * f.poly[k][1] - f.poly[k][0] * f.poly[j][1]) * 0.5;
    if (f.area <= 0) continue;                                  // (the outside of the drawing)
    bool anyCurve = false; for (int hh : f.hs) if (E[size_t(hh >> 1)].curve >= 0 && E[size_t(hh >> 1)].vids.empty()) anyCurve = true;   // (an edge curve is the rim too) if (!anyCurve) continue;   // (the mesh itself, a hole of it)
    if (!meshMid.empty()) {                                     // (over the built mesh's polygons: that side is built already)
      Box3 bb; for (int hh : f.hs) for (auto& p : E[size_t(hh >> 1)].L) bb.expand(p); const Vec3 pad = (bb.max - bb.min) * 0.05; bb.expand(bb.min - pad); bb.expand(bb.max + pad);
      for (const Vec3& c : meshMid) { if (c.x < bb.min.x || c.y < bb.min.y || c.z < bb.min.z || c.x > bb.max.x || c.y > bb.max.y || c.z > bb.max.z) continue; const auto q = to2(c); if (inPoly2(f.poly, q[0], q[1])) { f.ok = false; break; } } }
    if (f.ok) faces.push_back(std::move(f)); }
  { std::set<int> inFace; for (auto& f : faces) for (int hh : f.hs) inFace.insert(hh >> 1);
    for (size_t e = 0; e < E.size(); e++) if (E[e].alive && E[e].curve >= 0 && E[e].vids.empty() && !inFace.count(int(e))) lost++; }
  // two zones a piece of curve could close with the rim: the smaller filled (Switch zone — the other). A zone holding a
  // curve no other zone has is needed — that curve was drawn for it (a horseshoe's outer side, a corner's short piece):
  // kept, the other one left; both needed — both kept
  // (every curve closed by some zone, as few and as small as can be: the needed ones first, then the rest from the
  // smallest — one kept only while it closes a curve none kept closes yet; strips one above another along a rim, each
  // between two curves, all kept so. Switch zone — the rest from the largest)
  { std::map<int, std::vector<size_t>> all; for (size_t i = 0; i < faces.size(); i++) for (int hh : faces[i].hs) if (E[size_t(hh >> 1)].curve >= 0) all[hh >> 1].push_back(i);
    std::vector<char> need(faces.size(), 0); for (auto& [e, fs] : all) if (fs.size() == 1) need[fs[0]] = 1;
    std::set<int> covered; auto cover = [&](size_t i) { for (int hh : faces[i].hs) if (E[size_t(hh >> 1)].curve >= 0) covered.insert(hh >> 1); };
    std::vector<size_t> rest;
    for (size_t i = 0; i < faces.size(); i++) { if (!faces[i].rim || need[i]) cover(i); else rest.push_back(i); }
    // (one closed by an edge curve before one closed by the bare rim — as it was traced for; then by size)
    std::stable_sort(rest.begin(), rest.end(), [&](size_t a, size_t b) { if (faces[a].bare != faces[b].bare) return !faces[a].bare; return S.zoneAlt ? faces[a].area > faces[b].area : faces[a].area < faces[b].area; });
    for (size_t i : rest) { bool adds = false; for (int hh : faces[i].hs) if (E[size_t(hh >> 1)].curve >= 0 && !covered.count(hh >> 1)) adds = true;
      if (adds) cover(i); else { faces[i].ok = false; S.zoneChoice = true; } } }
  // the points of each piece: a curve's divisions shared among its pieces by their length, laid as its mode says
  std::map<int, std::vector<double>> share; std::map<int, int> divOf;
  { std::map<int, std::vector<int>> pieces; for (size_t e = 0; e < E.size(); e++) if (E[e].alive && E[e].curve >= 0) pieces[E[e].curve].push_back(int(e));
    for (auto& [c, es] : pieces) { double T = 0; for (int e : es) T += E[size_t(e)].len; const int D = std::max(int(es.size()), S.c[size_t(c)].div); std::vector<std::pair<double, int>> rem; int sum = 0;
      for (int e : es) { const double x = D * E[size_t(e)].len / std::max(1e-12, T); const int d = std::max(1, int(std::floor(x))); divOf[e] = d; sum += d; rem.push_back({x - d, e}); }
      std::sort(rem.rbegin(), rem.rend()); for (size_t k = 0; sum < D && k < rem.size(); k++, sum++) divOf[rem[k].second]++; } }
  std::map<int, std::vector<Vec3>> ptsOf; std::set<int> locked;
  auto edgePts = [&](int e) -> const std::vector<Vec3>& {
    auto f = ptsOf.find(e); if (f != ptsOf.end()) return f->second;
    if (E[size_t(e)].curve < 0 || !E[size_t(e)].vids.empty()) return ptsOf[e] = E[size_t(e)].L;   // (the rim, an edge curve: their vertices)
    return ptsOf[e] = resampleW(E[size_t(e)].L, divOf.count(e) ? divOf[e] : 1, S.c[size_t(E[size_t(e)].curve)].aw); };
  // the preview's points: the rim's are its vertices; the same point of two patches — one point
  std::map<int, int> byVid; std::map<std::array<double, 3>, int> byPos;
  auto addPt = [&](const Vec3& p, int vid) {
    if (vid >= 0) { auto f = byVid.find(vid); if (f != byVid.end()) return f->second; S.P.push_back(W[size_t(vid)]); S.vOf.push_back(vid); return byVid[vid] = int(S.P.size()) - 1; }
    const std::array<double, 3> k{p.x, p.y, p.z}; auto f = byPos.find(k); if (f != byPos.end()) return f->second;
    S.P.push_back(p); S.vOf.push_back(-1); return byPos[k] = int(S.P.size()) - 1; };
  auto put = [&](std::vector<int> f) { std::vector<int> g; for (int x : f) if (g.empty() || g.back() != x) g.push_back(x); while (g.size() > 1 && g.front() == g.back()) g.pop_back();
    std::set<int> u(g.begin(), g.end()); if (g.size() >= 3 && u.size() == g.size()) S.F.push_back(g); };
  struct Side { std::vector<Vec3> p; std::vector<int> vid; };
  auto rev = [](Side s) { std::reverse(s.p.begin(), s.p.end()); std::reverse(s.vid.begin(), s.vid.end()); return s; };
  // a four-sided patch: rows from one side to its opposite, each row as long as the two sides' counts between them say
  // (Detail to the denser side: the change in the first row by the denser one); rows next to each other joined
  auto fill4 = [&](std::array<Side, 4> s, bool fan = false) {   // (fan: s[2] a single point — the rows converge to it)
    auto n = [](const Side& x) { return int(x.p.size()) - 1; };
    if (!fan && std::abs(n(s[1]) - n(s[3])) > std::abs(n(s[0]) - n(s[2]))) s = {s[1], s[2], s[3], s[0]};   // (the unequal pair: bottom and top)
    const Side bot = s[0], rgt = s[1], top = rev(s[2]), lft = rev(s[3]);
    const int nb = n(bot), nt = n(top), nl = n(lft), nr = n(rgt), m = std::max(1, std::max(nl, nr));
    if (nb < 1 || nt < 1) return;
    auto idx = [&](const Side& x, int i) { return addPt(x.p[size_t(i)], x.vid[size_t(i)]); };
    auto fracs = [](const std::vector<Vec3>& P) { auto c = cumOf(P); std::vector<double> f(P.size(), 0.0); for (size_t i = 0; i < P.size(); i++) f[i] = c.back() > 0 ? c[i] / c.back() : double(i) / std::max<size_t>(1, P.size() - 1); return f; };
    const auto fb = fracs(bot.p), ft = fracs(top.p), fl = fracs(lft.p), fr = fracs(rgt.p);
    const Vec3 c0 = bot.p.front(), c1 = bot.p.back(), c3 = top.p.front(), c2 = top.p.back();
    std::vector<std::vector<int>> R(size_t(m) + 1);
    for (int i = 0; i <= nb; i++) R[0].push_back(idx(bot, i));
    for (int i = 0; i <= nt; i++) R[size_t(m)].push_back(idx(top, i));
    for (int j = 1; j < m; j++) {
      const int li = int(std::lround(double(j) * nl / m)), ri = int(std::lround(double(j) * nr / m)); const double t = (fl[size_t(li)] + fr[size_t(ri)]) / 2;
      int k = S.toDense && !fan ? (nb > nt ? nt : nb) : int(std::lround(nb + (nt - nb) * t)); k = std::max(1, k);
      std::vector<int> row{idx(lft, li)};
      const Vec3 Lv = lft.p[size_t(li)], Rv = rgt.p[size_t(ri)];
      for (int i = 1; i < k; i++) { double sv = double(i) / k;
        if (k == nb) sv = (1 - t) * fb[size_t(i)] + t * sv; if (k == nt) sv = t * ft[size_t(i)] + (1 - t) * sv;
        const Vec3 P = atFrac(bot.p, sv) * (1 - t) + atFrac(top.p, sv) * t + Lv * (1 - sv) + Rv * sv - (c0 * ((1 - sv) * (1 - t)) + c1 * (sv * (1 - t)) + c3 * ((1 - sv) * t) + c2 * (sv * t));
        row.push_back(addPt(stick(P), -1)); }
      row.push_back(idx(rgt, ri)); R[size_t(j)] = row; }
    // rows next to each other: quads, a triangle only where the longer row has a vertex more (spread evenly along it)
    for (int j = 0; j < m; j++) { const auto& A0 = R[size_t(j)]; const auto& B0 = R[size_t(j) + 1]; const bool flip = A0.size() < B0.size();
      const auto& A = flip ? B0 : A0; const auto& B = flip ? A0 : B0; const int na = int(A.size()) - 1, nbb = int(B.size()) - 1;
      auto fr = [&](const std::vector<int>& row) { std::vector<double> f(row.size(), 0.0); for (size_t i = 1; i < row.size(); i++) f[i] = f[i - 1] + S.P[size_t(row[i])].distanceTo(S.P[size_t(row[i - 1])]);
        for (auto& x : f) x = f.back() > 0 ? x / f.back() : 0; return f; };
      const auto fa = fr(A), fbr = fr(B); std::vector<int> G(size_t(na) + 1, 0);   // (each vertex of the longer row: the other row's vertex nearest along it — a step of one at most)
      for (int i = 1; i <= na; i++) { const int lo = std::max(G[size_t(i) - 1], nbb - (na - i)), hi = std::min(G[size_t(i) - 1] + 1, nbb); int b = lo;
        for (int c = lo; c <= hi; c++) if (std::abs(fbr[size_t(c)] - fa[size_t(i)]) < std::abs(fbr[size_t(b)] - fa[size_t(i)])) b = c; G[size_t(i)] = b; }
      G[size_t(na)] = nbb;
      auto g = [&](int i) { return G[size_t(i)]; };
      for (int i = 0; i < na; i++) { const int b0 = g(i), b1 = g(i + 1);
        std::vector<int> f = b1 == b0 ? std::vector<int>{A[size_t(i)], A[size_t(i) + 1], B[size_t(b0)]} : std::vector<int>{A[size_t(i)], A[size_t(i) + 1], B[size_t(b1)], B[size_t(b0)]};
        put(f); } }
  };
  // each face: its corners (where the contour turns, more than 40°), its sides between them
  for (Face& f : faces) { if (!f.ok) continue;
    const size_t nH = f.hs.size(); std::vector<char> corner(nH, 0);   // (corner[i]: at the start of piece i)
    std::vector<double> turnAt(nH, 0.0);
    for (size_t i = 0; i < nH; i++) { const auto a = hDir(f.hs[(i + nH - 1) % nH], true), b = hDir(f.hs[i], false);
      const double ang = std::abs(std::atan2(a[0] * b[1] - a[1] * b[0], a[0] * b[0] + a[1] * b[1])); corner[i] = ang > 40 * M_PI / 180; }
    for (size_t i = 0; i < nH; i++) { const auto a = hDir(f.hs[(i + nH - 1) % nH], true), b = hDir(f.hs[i], false); turnAt[i] = std::abs(std::atan2(a[0] * b[1] - a[1] * b[0], a[0] * b[0] + a[1] * b[1])); }
    { // (four pieces, a short one among them — a strip's narrow end: every junction a corner, the strip a grid with its four sides; more
      // corners than four with such a piece: the four turning most — else the short side makes the whole an n-gon)
      auto pts = [&](size_t i) { return E[size_t(f.hs[i] >> 1)].curve < 0 || !E[size_t(f.hs[i] >> 1)].vids.empty() ? E[size_t(f.hs[i] >> 1)].vids.size() : size_t(divOf.count(f.hs[i] >> 1) ? divOf[f.hs[i] >> 1] + 1 : 2); };
      bool shortOne = false; for (size_t i = 0; i < nH; i++) if (pts(i) < 3) shortOne = true;
      const int nc0 = int(std::count(corner.begin(), corner.end(), 1));
      if (shortOne && nH == 4) std::fill(corner.begin(), corner.end(), 1);
      else if (shortOne && nc0 > 4) { std::vector<size_t> o(nH); std::iota(o.begin(), o.end(), 0); std::sort(o.begin(), o.end(), [&](size_t x, size_t y) { return turnAt[x] > turnAt[y]; });
        std::fill(corner.begin(), corner.end(), 0); for (size_t k = 0; k < 4; k++) corner[o[k]] = 1; } }
    int nCorner = int(std::count(corner.begin(), corner.end(), 1)); if (nCorner < 3 && nH >= 3) { std::fill(corner.begin(), corner.end(), 1); nCorner = int(nH); }
    if (nCorner < 3) { S.note = tr("A contour needs three corners or more — let curves cross"); continue; }
    size_t c0 = 0; while (!corner[c0]) c0++;
    std::vector<Side> sides; Side cur;
    for (size_t q = 0; q < nH; q++) { const size_t i = (c0 + q) % nH; const int h = f.hs[i], e = h >> 1;
      if (corner[i] && !cur.p.empty()) { sides.push_back(cur); cur = {}; }
      auto P = edgePts(e); std::vector<int> V = E[size_t(e)].curve < 0 || !E[size_t(e)].vids.empty() ? E[size_t(e)].vids : std::vector<int>(P.size(), -1);
      if (h & 1) { std::reverse(P.begin(), P.end()); std::reverse(V.begin(), V.end()); }
      V.front() = nodes[size_t(hStart(h))].vid; V.back() = nodes[size_t(hEnd(h))].vid; if (V.front() >= 0) P.front() = W[size_t(V.front())]; if (V.back() >= 0) P.back() = W[size_t(V.back())];
      for (size_t k = cur.p.empty() ? 0 : 1; k < P.size(); k++) { cur.p.push_back(P[k]); cur.vid.push_back(V[k]); } }
    if (!cur.p.empty()) sides.push_back(cur);
    for (int hh : f.hs) if (E[size_t(hh >> 1)].curve < 0) S.zone.push_back(E[size_t(hh >> 1)].L);
    // (three sides, one corner sharp — under 30°, a curve leaving the rim at a slant: that corner a side of no length, the
    // patch a strip with a triangle at its tip, not three grids meeting in the middle)
    std::vector<int> srcSide(sides.size()); std::iota(srcSide.begin(), srcSide.end(), 0);
    if (sides.size() == 3 && !S.triFan) { double ba = 1e9; size_t bk = 0;
      for (size_t k = 0; k < 3; k++) { const Side &A = sides[k], &Bs = sides[(k + 1) % 3]; if (A.p.size() < 2 || Bs.p.size() < 2) continue; const Vec3 c = A.p.back();
        const Vec3 u = (A.p[A.p.size() - 2] - c).normalized(), w = (Bs.p[1] - c).normalized(); const double ang = std::acos(std::clamp(u.dot(w), -1.0, 1.0)); if (ang < ba) { ba = ang; bk = k; } }
      if (ba < 30 * M_PI / 180) { Side pt; pt.p = {sides[bk].p.back(), sides[bk].p.back()}; pt.vid = {sides[bk].vid.back(), sides[bk].vid.back()};
        sides.insert(sides.begin() + long(bk) + 1, pt); srcSide.insert(srcSide.begin() + long(bk) + 1, -1); } }
    // Match: a side of Match curve pieces laid as the opposite side — each point across from the opposite side's, where
    // that one falls on the curve (the rungs straight across, not leaning as length shares would lay them); the ends, the
    // order along it and the points where its pieces meet kept (the pieces shared with the patches round)
    if (sides.size() == 4) for (size_t i = 0; i < 4; i++) {
      if (srcSide[i] < 0) continue;                                 // (the sharp corner's side of no length)
      std::vector<int> hsI; { int sideIdx = -1; for (size_t q = 0; q < nH; q++) { const size_t k = (c0 + q) % nH; if (corner[k]) sideIdx++; if (sideIdx == srcSide[i]) hsI.push_back(f.hs[k]); } }
      auto isM = [&](int e) { return E[size_t(e)].curve >= 0 && E[size_t(e)].vids.empty() && S.c[size_t(E[size_t(e)].curve)].mode == 'm' && !locked.count(e); };
      bool anyM = false, ok = !hsI.empty(); for (int h : hsI) { if (isM(h >> 1)) anyM = true; else ok = false; }
      if (!anyM) continue;
      if (!ok) {   // (a Match curve going on into the rim, no corner between: its pieces take as many edges as the opposite side has, less the rim's own)
        const int nOpp = int(sides[(i + 2) % 4].p.size()) - 1; int fixedN = 0; double mLen = 0; std::vector<int> ms;
        for (int h : hsI) { const int e = h >> 1; if (isM(e)) { ms.push_back(e); mLen += E[size_t(e)].len; } else fixedN += int(edgePts(e).size()) - 1; }
        int cur = 0; for (int e : ms) cur += divOf.count(e) ? divOf[e] : 1;
        const int want = std::max(int(ms.size()), nOpp - fixedN); int given = 0;
        if (want > std::max(2 * cur, cur + 6)) continue;   // (far more than drawn — the opposite side a long rim round something else: left as drawn)
        for (size_t q = 0; q < ms.size(); q++) { const int d = q + 1 < ms.size() ? std::max(1, int(std::lround(want * E[size_t(ms[q])].len / std::max(1e-12, mLen)))) : std::max(1, want - given); given += d;
          divOf[ms[q]] = d; ptsOf.erase(ms[q]); locked.insert(ms[q]); }
        Side ns;
        for (int h : hsI) { const int e = h >> 1; auto P = edgePts(e); std::vector<int> V = E[size_t(e)].curve < 0 || !E[size_t(e)].vids.empty() ? E[size_t(e)].vids : std::vector<int>(P.size(), -1);
          if (h & 1) { std::reverse(P.begin(), P.end()); std::reverse(V.begin(), V.end()); }
          V.front() = nodes[size_t(hStart(h))].vid; V.back() = nodes[size_t(hEnd(h))].vid; if (V.front() >= 0) P.front() = W[size_t(V.front())]; if (V.back() >= 0) P.back() = W[size_t(V.back())];
          for (size_t k = ns.p.empty() ? 0 : 1; k < P.size(); k++) { ns.p.push_back(P[k]); ns.vid.push_back(V[k]); } }
        if (ns.p.size() >= 2) sides[i] = ns;
        continue; }
      std::vector<Vec3> L; std::vector<double> cutAt;
      for (int h : hsI) { const auto Lh = hPts(h); if (!L.empty()) cutAt.push_back(cumOf(L).back()); for (size_t k = L.empty() ? 0 : 1; k < Lh.size(); k++) L.push_back(Lh[k]); }   // (cutAt: where along it the pieces meet)
      const auto lc = cumOf(L);
      const Side& opp = sides[(i + 2) % 4]; const size_t n = opp.p.size(); if (n < hsI.size() + 1) continue;
      auto onL = [&](const Vec3& q) { double bd = 1e300, bs = 0; for (size_t j = 0; j + 1 < L.size(); j++) { const Vec3 d = L[j + 1] - L[j]; const double l2 = d.dot(d), t = l2 > 0 ? std::clamp((q - L[j]).dot(d) / l2, 0.0, 1.0) : 0.0;
        const double dd = (L[j] + d * t).distanceTo(q); if (dd < bd) { bd = dd; bs = lc[j] + (lc[j + 1] - lc[j]) * t; } } return bs; };
      std::vector<double> sh(n, 0); sh.back() = lc.back();
      { const double step = lc.back() / double(n - 1), gap = step * 0.25;   // (as for the rim's row: across only near the even place, never piled up)
        for (size_t k = 1; k + 1 < n; k++) { const double even = step * double(k), sp = onL(opp.p[n - 1 - k]);
          const double mwc = S.c[size_t(E[size_t(hsI[0] >> 1)].curve)].mw, sm = std::abs(sp - even) < step * 1.5 ? sp : even, s = even + (sm - even) * std::clamp(mwc, 0.0, 1.0);   // (Equal ↔ Match mixed)
          sh[k] = std::clamp(s, sh[k - 1] + gap, std::max(sh[k - 1] + gap, lc.back() - double(n - 1 - k) * gap)); } }
      std::vector<size_t> at;                                       // (each point where pieces meet: onto the nearest point, in order)
      { size_t from = 1; for (double c : cutAt) { size_t bk = from; double bd = 1e300; for (size_t k = from; k + 1 < n; k++) if (std::abs(sh[k] - c) < bd) { bd = std::abs(sh[k] - c); bk = k; }
          const size_t left = cutAt.size() - at.size() - 1; if (bk + left >= n - 1) bk = n - 2 - left; sh[bk] = c; at.push_back(bk); from = bk + 1; } }
      for (size_t k = 1; k + 1 < n; k++) sh[k] = std::clamp(sh[k], sh[k - 1], lc.back());
      std::vector<Vec3> np2; for (double s : sh) np2.push_back(atLen(L, lc, s));
      size_t k0 = 0; for (size_t q = 0; q < hsI.size(); q++) { const size_t k1 = q < at.size() ? at[q] : n - 1; std::vector<Vec3> part(np2.begin() + long(k0), np2.begin() + long(k1) + 1);
        if (hsI[q] & 1) std::reverse(part.begin(), part.end()); ptsOf[hsI[q] >> 1] = part; locked.insert(hsI[q] >> 1); k0 = k1; }
      Side ns; ns.p = np2; ns.vid.assign(ns.p.size(), -1); ns.vid.front() = sides[i].vid.front(); ns.vid.back() = sides[i].vid.back(); sides[i] = ns; }
    S.patches++;
    if (sides.size() == 4) { fill4({sides[0], sides[1], sides[2], sides[3]}); continue; }
    if (sides.size() == 3 && S.triFan) {   // (a triangle as a grid converging to the corner across the side of the most edges)
      size_t a = 0; for (size_t i = 1; i < 3; i++) if (sides[i].p.size() > sides[a].p.size()) a = i;
      const Side& b = sides[(a + 1) % 3]; Side apex; apex.p = {b.p.back(), b.p.back()}; apex.vid = {b.vid.back(), b.vid.back()};
      fill4({sides[a], b, apex, sides[(a + 2) % 3]}, true); continue; }
    // other counts: each side halved, the halves joined to a middle point — a four-sided piece round each corner
    bool small = false; for (auto& sd : sides) if (sd.p.size() < 3) small = true;
    if (small) { std::vector<int> g; for (auto& sd : sides) for (size_t k = 0; k + 1 < sd.p.size(); k++) g.push_back(addPt(sd.p[k], sd.vid[k])); put(g); continue; }
    const size_t K = sides.size(); Vec3 C; int cn = 0; for (auto& sd : sides) for (auto& p : sd.p) { C += p; cn++; } C = stick(C * (1.0 / cn));
    std::vector<int> mid(K); for (size_t i = 0; i < K; i++) mid[i] = (int(sides[i].p.size()) - 1) / 2;
    std::vector<Side> spoke(K);
    for (size_t i = 0; i < K; i++) { const Side& prev = sides[(i + K - 1) % K]; const Side& nxt = sides[(i + 1) % K];
      const int second = int(prev.p.size()) - 1 - mid[(i + K - 1) % K], first = mid[(i + 1) % K]; const int r = std::max(1, int(std::lround((second + first) / 2.0)));
      const Vec3 M = sides[i].p[size_t(mid[i])]; spoke[i].p.push_back(M); spoke[i].vid.push_back(sides[i].vid[size_t(mid[i])]);
      for (int k = 1; k < r; k++) { spoke[i].p.push_back(stick(M + (C - M) * (double(k) / r))); spoke[i].vid.push_back(-1); }
      spoke[i].p.push_back(C); spoke[i].vid.push_back(-1); }
    for (size_t i = 0; i < K; i++) { const size_t pi = (i + K - 1) % K; Side a, b;
      for (size_t k = size_t(mid[pi]); k < sides[pi].p.size(); k++) { a.p.push_back(sides[pi].p[k]); a.vid.push_back(sides[pi].vid[k]); }
      for (size_t k = 0; k <= size_t(mid[i]); k++) { b.p.push_back(sides[i].p[k]); b.vid.push_back(sides[i].vid[k]); }
      fill4({a, b, spoke[i], rev(spoke[pi])}); }
  }
  // a curve from the rim back to the rim, no contour found on screen (it runs close along the rim — a strip seen nearly
  // edge-on, as a hem's underside): the contour taken from the mesh itself — the curve and the shorter arc of the rim
  // between its ends (Switch zone: the longer); a row of polygons between them (Match: the curve's points across the
  // rim's vertices — quads)
  // A curve round the body from the rim back to it (a tube's ring under a cuff): its ends close, the curve long — the
  // long arc of the rim taken (the short one is where it left from and came back, a seam); that arc parted at its sharp
  // turns near its ends (a strip's sides hanging down to the curve's ends) — a grid between it and the curve. Found in
  // space, not on the screen: a ring round a body overlaps itself in any view
  if (S.F.empty()) for (size_t e = 0; e < E.size(); e++) {
    const Edge& ce = E[e]; if (!ce.alive || ce.curve < 0 || !ce.vids.empty()) continue;   // (an edge curve: the rim itself — nothing to fill to it)
    const int va = nodes[size_t(root(ce.a))].vid, vb = nodes[size_t(root(ce.b))].vid; if (va < 0 || vb < 0 || va == vb) continue;
    // (an edge curve between the same two vertices: the strip goes to it — its vertices as the rim's arc, both ways the same)
    std::vector<std::vector<int>> eLoops;
    for (auto& x : E) if (x.alive && x.curve >= 0 && x.vids.size() >= 2 && std::find(x.vids.begin(), x.vids.end(), -1) == x.vids.end()
                          && ((x.vids.front() == va && x.vids.back() == vb) || (x.vids.front() == vb && x.vids.back() == va))) {
      auto f = x.vids; for (int k = int(x.vids.size()) - 2; k >= 1; k--) f.push_back(x.vids[size_t(k)]); eLoops.push_back(f); break; }
    for (const auto& L : eLoops.empty() ? loops : eLoops) {
      const auto ia = std::find(L.begin(), L.end(), va), ib = std::find(L.begin(), L.end(), vb); if (ia == L.end() || ib == L.end()) continue;
      const int n = int(L.size()), a = int(ia - L.begin()), b = int(ib - L.begin());
      std::vector<int> fw, bw; for (int k = a;; k = (k + 1) % n) { fw.push_back(L[size_t(k)]); if (k == b) break; } for (int k = a;; k = (k + n - 1) % n) { bw.push_back(L[size_t(k)]); if (k == b) break; }
      auto len = [&](const std::vector<int>& vs) { double s = 0; for (size_t i = 1; i < vs.size(); i++) s += W[size_t(vs[i])].distanceTo(W[size_t(vs[i - 1])]); return s; };
      const bool fwShort = len(fw) <= len(bw); const double shortLen = std::min(len(fw), len(bw)), curveLen = cumOf(ce.L).back();
      const bool around = curveLen > 3 * shortLen;                   // (round the body: the long arc by default)
      const bool takeShort = around ? S.zoneAlt : !S.zoneAlt;
      const std::vector<int>& arc = fwShort == takeShort ? fw : bw; S.zoneChoice = true;
      // (its sharp turns near its ends: the sides of a strip hanging from the rim down to the curve's ends)
      int c1 = -1, c2 = -1;
      if (around) for (size_t k = 1; k + 1 < arc.size(); k++) {
        const Vec3 u = (W[size_t(arc[k])] - W[size_t(arc[k - 1])]).normalized(), w = (W[size_t(arc[k + 1])] - W[size_t(arc[k])]).normalized();
        if (std::acos(std::clamp(u.dot(w), -1.0, 1.0)) <= 55 * M_PI / 180) continue;
        if (k * 3 < arc.size() && c1 < 0) c1 = int(k); else if (k * 3 >= arc.size() * 2) c2 = int(k); }
      const bool split = c1 > 0 && c2 > c1;
      Side bot, sl, sr; for (size_t k = split ? size_t(c1) : 0; k < (split ? size_t(c2) + 1 : arc.size()); k++) { bot.p.push_back(W[size_t(arc[k])]); bot.vid.push_back(arc[k]); }
      if (split) { for (int k = 0; k <= c1; k++) { sl.p.push_back(W[size_t(arc[size_t(k)])]); sl.vid.push_back(arc[size_t(k)]); }
                   for (size_t k = size_t(c2); k < arc.size(); k++) { sr.p.push_back(W[size_t(arc[k])]); sr.vid.push_back(arc[k]); } }
      const std::vector<Vec3>& cp = ce.L;                          // (the piece runs from its node a — va — to b — vb)
      Side top; const char md = S.c[size_t(ce.curve)].mode;
      if (md == 'm') {                                               // (Match: as many points as the arc)
        const auto ac = cumOf(bot.p), cc = cumOf(cp); double prev = 0;   // (each across from its rim vertex — where that one falls on the curve, in order)
        const size_t nb = bot.p.size(); const double step = cc.back() / double(std::max<size_t>(1, nb - 1)), gap = step * 0.25;
        for (size_t i = 0; i < nb; i++) { const double even = (ac.back() > 0 ? ac[i] / ac.back() : 0) * cc.back(); double s = even;
          if (i > 0 && i + 1 < nb) { double bd = 1e300, sp = even; for (size_t j = 0; j + 1 < cp.size(); j++) { const Vec3 d = cp[j + 1] - cp[j]; const double l2 = d.dot(d), t = l2 > 0 ? std::clamp((bot.p[i] - cp[j]).dot(d) / l2, 0.0, 1.0) : 0.0;
              const double dd = (cp[j] + d * t).distanceTo(bot.p[i]); if (dd < bd) { bd = dd; sp = cc[j] + (cc[j + 1] - cc[j]) * t; } }
            // (straight across only near where an even share puts it — a curve running onto the rim and off it again would
            // pile the points up at one end; and never closer than a quarter step to the one before, room left for the rest)
            if (std::abs(sp - even) < step * 1.5) s = even + (sp - even) * std::clamp(S.c[size_t(ce.curve)].mw, 0.0, 1.0);   // (Equal ↔ Match mixed)
            s = std::clamp(s, prev + gap, std::max(prev + gap, cc.back() - double(nb - 1 - i) * gap)); }
          prev = s; top.p.push_back(atLen(cp, cc, s)); } }
      else top.p = resampleW(cp, std::max(1, S.c[size_t(ce.curve)].div), S.c[size_t(ce.curve)].aw);
      top.vid.assign(top.p.size(), -1); top.vid.front() = va; top.vid.back() = vb; top.p.front() = W[size_t(va)]; top.p.back() = W[size_t(vb)];
      Side right, left; right.p = {W[size_t(vb)]}; right.vid = {vb}; left.p = {W[size_t(va)]}; left.vid = {va};
      if (split) { right = sr; left = sl; }                         // (the strip's sides: from the rim's turn down to the curve's ends)
      S.zone.push_back(bot.p); S.patches++;
      fill4({bot, right, rev(top), left});
      break; }
  }
  // (the patches facing the camera)
  for (auto& f : S.F) { Vec3 nrm, c; for (size_t k = 0; k < f.size(); k++) { const Vec3& a = S.P[size_t(f[k])]; const Vec3& b = S.P[size_t(f[(k + 1) % f.size()])]; nrm += a.cross(b); c += a; } c = c * (1.0 / f.size());
    if (nrm.dot(cam_.ortho() ? cam_.dir() : c - cam_.pos) > 0) std::reverse(f.begin(), f.end()); }
  // the tubes: ring to ring, each row round as many edges as the rings have (they differ — rows of changing length), up the
  // seam as many as it has (none — about as tall as a ring's edges are long)
  for (const Tube& t : tubes) {
    const auto& R1 = tubeKeep[size_t(t.r1)]; const auto& R2 = tubeKeep[size_t(t.r2)]; const auto c1 = cumOf(R1), c2 = cumOf(R2);
    const int n1 = std::max(3, S.c[size_t(t.r1)].div), n2 = std::max(3, S.c[size_t(t.r2)].div);
    auto ringPts = [&](const std::vector<Vec3>& R, const std::vector<double>& cr, double start, int dir, int n) { const double T = cr.back(); std::vector<Vec3> p;
      for (int k = 0; k <= n; k++) { double s = std::fmod(start + dir * T * k / n, T); if (s < 0) s += T; p.push_back(atLen(R, cr, s)); }
      p.back() = p.front(); return p; };
    double st1 = 0, st2 = 0; int dir2 = 1; std::vector<Vec3> seam;
    auto twist = [&](double s1, double s2, int d) { double sum = 0; const auto a = ringPts(R1, c1, s1, 1, 24), b = ringPts(R2, c2, s2, d, 24); for (size_t k = 0; k < a.size(); k++) sum += a[k].distanceTo(b[k]); return sum; };
    if (t.seam >= 0) {                                             // (the seam: its ends where the rings start; its points the tube's rows)
      std::vector<Vec3> L = tubeKeep[size_t(t.seam)]; double at = 0; if (nearOnL(R1, L.front(), at) >= tolE) std::reverse(L.begin(), L.end());
      nearOnL(R1, L.front(), st1); nearOnL(R2, L.back(), st2); dir2 = twist(st1, st2, 1) <= twist(st1, st2, -1) ? 1 : -1;
      seam = resampleW(L, std::max(1, S.c[size_t(t.seam)].div), S.c[size_t(t.seam)].aw); }
    else {                                                         // (none: the second ring turned and started where the first one's rows run straightest)
      double best = 1e300; for (int k = 0; k < 48; k++) for (int d : {1, -1}) { const double s2 = c2.back() * k / 48, tw = twist(0, s2, d); if (tw < best) { best = tw; st2 = s2; dir2 = d; } }
      const Vec3 a = atLen(R1, c1, st1), b = atLen(R2, c2, st2); const double edge = (c1.back() / n1 + c2.back() / n2) / 2;
      const int h = std::max(1, int(std::lround(a.distanceTo(b) / std::max(1e-12, edge)))); seam.push_back(a);
      for (int k = 1; k < h; k++) seam.push_back(patchOnRefs(a + (b - a) * (double(k) / h))); seam.push_back(b); }
    Side bot, top, sd; bot.p = ringPts(R1, c1, st1, 1, n1); top.p = ringPts(R2, c2, st2, dir2, n2); seam.front() = bot.p.front(); seam.back() = top.p.front(); sd.p = seam;
    bot.vid.assign(bot.p.size(), -1); top.vid.assign(top.p.size(), -1); sd.vid.assign(sd.p.size(), -1);
    const size_t f0 = S.F.size(); tubeOn = true; fill4({bot, sd, rev(top), rev(sd)}); tubeOn = false; S.patches++;
    // (its polygons facing out: away from the axis between the rings' middles)
    const Vec3 m1 = mid(size_t(t.r1)), m2 = mid(size_t(t.r2)); Vec3 ax = m2 - m1; const double al = ax.lengthSq();
    for (size_t i = f0; i < S.F.size(); i++) { auto& f = S.F[i]; Vec3 nrm, c; for (size_t k = 0; k < f.size(); k++) { const Vec3& p = S.P[size_t(f[k])]; nrm += p.cross(S.P[size_t(f[(k + 1) % f.size()])]); c += p; } c = c * (1.0 / f.size());
      const Vec3 on = m1 + ax * (al > 0 ? std::clamp((c - m1).dot(ax) / al, 0.0, 1.0) : 0.0); if (nrm.dot(c - on) < 0) std::reverse(f.begin(), f.end()); }
    S.zone.push_back(seam); }
  if (!tubeKeep.empty()) S.sampled = tubeKeep;                    // (the rings and seams drawn as curves again)
  if (S.F.empty() && S.note.isEmpty()) S.note = tr("No closed contour yet — curves crossing each other (or meeting the mesh's open rim) close one");
  };
  { PatchSt best; int bl = -1;
    for (int a = 0; a < 4; a++) { once(a >= 2, a % 2 == 0); if (lost == 0) { bl = -1; break; } if (bl < 0 || lost < bl) { best = S; bl = lost; } }
    if (bl >= 0) S = best; }
  if (S.kind != 'p') patchLoftStrip();
  patchLift();
}

// ---------- over the surface ----------
// a flat polygon across a bulge — a narrow strip over a fold, one quad across it, no point inside — dips under the reference there and
// shows torn (the surface through it); its points lifted along the polygons' outward normal as far as the surface lies
// ahead of it (its middle and its sides' middles looked at), two passes, as the Topology Ghost does. The built mesh's own
// points (reused on its rim) stay where they are
void Viewport::patchLift() {
  auto& S = pt_; if (S.F.empty() || S.P.empty()) return;
  struct Ref { std::shared_ptr<const topo::Bvh> b; Affine M, Mi; bool space = false; };
  std::vector<Ref> refs; const int tm = patchTarget();
  for (auto& [id, x] : scene_->items) {
    if (id == tm || x.type != ItemType::IMesh || !x.poly || !scene_->isVisible(x)) continue;
    auto h = heavy_.find(id);
    if (h != heavy_.end() && h->second.bvh && h->second.src.lock() == baseSp(x)) { const Affine M = meshmods::meshMatrix(*scene_, x); refs.push_back({h->second.bvh, M, M.inverse(), true}); continue; }   // (a high-poly one: in its file's space)
    if ((long long)x.poly->faceVerts.size() - 2LL * x.poly->faceCount() > 400000) continue;
    if (auto b = bvhOf(id, false)) refs.push_back({b, Affine(), Affine(), false});
  }
  if (refs.empty()) return;
  double L = 0; int ne = 0;
  for (auto& f : S.F) for (size_t k = 0; k < f.size(); k++) { L += S.P[size_t(f[k])].distanceTo(S.P[size_t(f[(k + 1) % f.size()])]); ne++; }
  if (!ne) return; L /= ne; const double reach = L * 0.5, margin = L * 0.01;
  // (each point's normal: its polygons' — they face the camera, so outward is towards it)
  auto fnormal = [&](const std::vector<int>& f) { Vec3 n; for (size_t k = 0; k < f.size(); k++) { const Vec3& a = S.P[size_t(f[k])]; const Vec3& b = S.P[size_t(f[(k + 1) % f.size()])]; n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)); } return n; };
  std::vector<Vec3> N(S.P.size());
  for (auto& f : S.F) { const Vec3 n = fnormal(f); for (int v : f) N[size_t(v)] += n; }
  for (auto& n : N) n = n.lengthSq() > 0 ? n.normalized() : Vec3();
  auto ahead = [&](const Vec3& p, const Vec3& d) {                // (how far along d the surface's outside lies ahead of p)
    double h = 0;
    for (auto& r : refs) { const Vec3 o = r.space ? r.Mi.apply(p) : p, e = r.space ? (r.Mi.apply(p + d) - o) : d; const double s = e.length(); if (!(s > 0)) continue;
      if (auto hit = r.b->ray(o, e * (1.0 / s), reach * s); hit && hit->first > 1e-9) { const size_t t = size_t(hit->second);
        const Vec3 tn = (r.b->P[r.b->idx[t * 3 + 1]] - r.b->P[r.b->idx[t * 3]]).cross(r.b->P[r.b->idx[t * 3 + 2]] - r.b->P[r.b->idx[t * 3]]);
        if (tn.dot(e) > 0) h = std::max(h, hit->first / s); } }
    return h; };
  // (only the polygons with nothing inside the patch to hold them: every point on a curve — a row from one side to the
  // other. A wide patch's inner points lie on the surface already, its polygons follow it: left as they are)
  // (a point on the border: on one of the curves — the strips side by side share theirs)
  std::vector<char> rim(S.P.size(), 0); const double on = L * 0.02;
  for (size_t v = 0; v < S.P.size(); v++) for (const auto& C : S.sampled) { for (size_t i = 0; i + 1 < C.size() && !rim[v]; i++) { const Vec3 d = C[i + 1] - C[i]; const double l2 = d.dot(d), t = l2 > 0 ? std::clamp((S.P[v] - C[i]).dot(d) / l2, 0.0, 1.0) : 0.0;
      if ((C[i] + d * t).distanceTo(S.P[v]) < on) rim[v] = 1; } if (rim[v]) break; }
  for (int pass = 0; pass < 2; pass++) {
    std::vector<double> lift(S.P.size(), 0.0); bool any = false;
    for (auto& f : S.F) { bool across = true; for (int v : f) across = across && rim[size_t(v)]; if (!across) continue;
      Vec3 fn = fnormal(f); if (fn.lengthSq() == 0) continue; fn = fn.normalized();
      Vec3 c; for (int v : f) c += S.P[size_t(v)]; c = c * (1.0 / f.size());
      std::vector<Vec3> smp{c}; for (size_t k = 0; k < f.size(); k++) smp.push_back((S.P[size_t(f[k])] + S.P[size_t(f[(k + 1) % f.size()])]) * 0.5);
      double h = 0; for (const Vec3& p : smp) h = std::max(h, ahead(p, fn));
      if (h > 0) { any = true; for (int v : f) lift[size_t(v)] = std::max(lift[size_t(v)], h + margin); } }
    if (!any) break;
    for (size_t v = 0; v < S.P.size(); v++) if (lift[v] > 0 && !(v < S.vOf.size() && S.vOf[v] >= 0)) S.P[v] += N[v] * lift[v];
  }
}

// ---------- events ----------
bool Viewport::patchPress(QMouseEvent* e) {
  const QPointF mp = e->position();
  if (pt_.ordering && pt_.kind == 'l') {                         // (Loft's order being set: a click picks the curve under it)
    Projector P = projector(); patchBuild(); int bc = -1; double bd = 10;
    for (size_t c = 0; c < pt_.sampled.size(); c++) { const auto& L = pt_.sampled[c]; for (size_t i = 0; i + 1 < L.size(); i++) { const ScreenPt a = P.toScreen(L[i]), b = P.toScreen(L[i + 1]); if (a.z >= 1 || b.z >= 1) continue;
        const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? std::clamp(((mp.x() - a.x) * dx + (mp.y() - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
        const double d = std::hypot(a.x + dx * t - mp.x(), a.y + dy * t - mp.y()); if (d < bd) { bd = d; bc = int(c); } } }
    if (bc < 0) { ui::hud(tr("Loft order: click on a curve"), 900); return true; }
    patchOrderClick(bc); return true; }
  if (e->modifiers() & Qt::ShiftModifier) return patchTraceStart(mp);   // (Shift + drag: an edge curve)
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const bool dbl = now - pt_.lastT < 350 && std::hypot(mp.x() - pt_.lastP.x(), mp.y() - pt_.lastP.y()) < 6; pt_.lastT = dbl ? 0 : now; pt_.lastP = mp;
  if (dbl && pt_.draw >= 0) { patchPush(); const int c = pt_.draw; pt_.draw = -1; if (pt_.c[size_t(c)].a.size() < 2) { pt_.c.erase(pt_.c.begin() + c); pt_.selC = pt_.selA = -1; } else { patchJoinAsk(c); patchSplitBy(pt_.selC >= 0 ? pt_.selC : c); } patchChanged(); return true; }
  const auto p = patchSnap(mp, pt_.draw); if (!p) { ui::hud(tr("PatchTool: click on a surface"), 900); return true; }
  patchPush();                                                 // (an anchor put: a step)
  if (pt_.draw < 0) { PatchCurve C; C.a = {*p};
    if (pt_.selC >= 0 && size_t(pt_.selC) < pt_.c.size()) { const PatchCurve& O = pt_.c[size_t(pt_.selC)]; C.mode = O.mode;   // (the picked curve's mode; its edges' length — not their count: a long curve after a short one would get its one edge)
      const auto& L = O.L.size() >= 2 ? O.L : O.a; const double len = cumOf(L).back(); if (len > 0) C.edgeLen = len / std::max(1, O.div); else C.div = O.div; }
    pt_.c.push_back(C); pt_.draw = int(pt_.c.size()) - 1; pt_.selC = pt_.draw; pt_.selA = 0;
    ui::hud(tr("PatchTool: click — the next anchor; double click / Enter — the curve ends; RMB — pick / add / drag an anchor"), 2500); }
  else { auto& C = pt_.c[size_t(pt_.draw)]; if (C.a.empty() || C.a.back().distanceTo(*p) > 1e-9) C.a.push_back(*p); pt_.selC = pt_.draw; pt_.selA = int(C.a.size()) - 1;
    if (C.a.size() >= 3 && C.a.back().distanceTo(C.a.front()) < 1e-9) { pt_.draw = -1; ui::hud(tr("The curve closed on itself"), 900); } }
  patchChanged(); return true;
}
bool Viewport::patchRmb(QMouseEvent* e) {
  const QPointF mp = e->position(); const bool ctrl = e->modifiers() & Qt::ControlModifier; Projector P = projector();
  int bc = -1, ba = -1; double bd = 10;
  for (size_t c = 0; c < pt_.c.size(); c++) for (size_t a = 0; a < pt_.c[c].a.size(); a++) { const ScreenPt s = P.toScreen(pt_.c[c].a[a]); const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (s.z < 1 && d < bd) { bd = d; bc = int(c); ba = int(a); } }
  if (bc >= 0) pt_.c[size_t(bc)].vids.clear();                 // (an edge curve's anchor picked: a curve of its own now, free of the edges)
  pt_.dragPushed = false;
  if (bc >= 0 && ctrl) {                                        // (Ctrl + RMB on an anchor: it goes)
    patchPush();
    auto& C = pt_.c[size_t(bc)]; C.a.erase(C.a.begin() + ba); if (C.a.size() < 2) { pt_.c.erase(pt_.c.begin() + bc); pt_.selC = pt_.selA = -1; if (pt_.draw == bc) pt_.draw = -1; else if (pt_.draw > bc) pt_.draw--; } else { pt_.selC = bc; pt_.selA = std::min(ba, int(C.a.size()) - 1); }
    patchChanged(); return true; }
  if (bc < 0) {                                                 // (on a curve: a new anchor there)
    patchBuild(); double bd2 = 8; size_t bi = 0;
    for (size_t c = 0; c < pt_.sampled.size(); c++) { const auto& L = pt_.sampled[c]; for (size_t i = 0; i + 1 < L.size(); i++) { const ScreenPt a = P.toScreen(L[i]), b = P.toScreen(L[i + 1]); if (a.z >= 1 || b.z >= 1) continue;
        const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? std::clamp(((mp.x() - a.x) * dx + (mp.y() - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
        const double d = std::hypot(a.x + dx * t - mp.x(), a.y + dy * t - mp.y()); if (d < bd2) { bd2 = d; bc = int(c); bi = i; } } }
    if (bc >= 0) { patchPush(); pt_.dragPushed = true; auto& C = pt_.c[size_t(bc)]; C.vids.clear(); const size_t span = std::min(C.a.size() - 2, bi / 16); const auto& L = pt_.sampled[size_t(bc)];
      C.a.insert(C.a.begin() + long(span) + 1, L[bi]); ba = int(span) + 1; }
  }
  if (bc < 0) { if (pt_.draw >= 0) { patchPush(); const int c = pt_.draw; pt_.draw = -1; if (pt_.c[size_t(c)].a.size() < 2) pt_.c.erase(pt_.c.begin() + c); else { patchJoinAsk(c); patchSplitBy(pt_.selC >= 0 ? pt_.selC : c); } }
    pt_.selC = pt_.selA = -1; patchChanged(); return true; }
  pt_.selC = bc; pt_.selA = ba; pt_.dragA = true; pt_.moved = false; pt_.press = mp;
  tt_.drag = 30; td_ = {}; td_.type = 20; drag_ = Drag::Topo; patchChanged(); return true;
}
bool Viewport::patchMove(const QPointF& mp) {
  if (drag_ == Drag::Topo && tt_.drag == 31) { patchTraceMove(mp); return true; }
  if (drag_ == Drag::Topo && tt_.drag == 32) { overlayRev_++; requestUpdate(); return true; }   // (the ring's stroke follows the pointer)
  if (drag_ == Drag::Topo && tt_.drag == 30 && pt_.dragA && pt_.selC >= 0 && size_t(pt_.selC) < pt_.c.size()) {
    if (!pt_.moved && std::hypot(mp.x() - pt_.press.x(), mp.y() - pt_.press.y()) < 3) return true;
    if (!pt_.moved && !pt_.dragPushed) { patchPush(); pt_.dragPushed = true; }   // (an anchor dragged: a step — at its first move)
    pt_.moved = true;
    if (auto p = patchSnap(mp, pt_.selC)) { pt_.c[size_t(pt_.selC)].a[size_t(pt_.selA)] = *p; pt_.key.clear(); ttTouch(); }
    return true; }
  if (pt_.draw >= 0) { overlayRev_++; requestUpdate(); }        // (the next span follows the pointer)
  return true;
}
bool Viewport::patchRelease(const QPointF& mp) {
  if (tt_.drag == 31) { patchTraceEnd(); return true; }
  if (tt_.drag == 32) { tt_.drag = 0; pt_.ring = false; patchRingEnd(pt_.ringA, mp); overlayRev_++; requestUpdate(); return true; }
  if (tt_.drag == 30) { tt_.drag = 0; const bool end = pt_.selC >= 0 && size_t(pt_.selC) < pt_.c.size() && (pt_.selA == 0 || pt_.selA == int(pt_.c[size_t(pt_.selC)].a.size()) - 1);
    pt_.dragA = false; if (pt_.moved && end) { const int c = pt_.selC; patchJoinAsk(c); patchSplitBy(pt_.selC >= 0 ? pt_.selC : c); } patchChanged(); }
  return true;
}
bool Viewport::patchKey(int key) {
  const bool edits = ((key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) && pt_.draw >= 0) || (key == Qt::Key_Backspace && pt_.draw >= 0) || (key == Qt::Key_Delete && pt_.selC >= 0 && size_t(pt_.selC) < pt_.c.size() && pt_.selA >= 0 && pt_.c[size_t(pt_.selC)].a.size() > 2);   // (a whole curve deleted: its own step there)
  if (edits) patchPush();                                      // (a curve ended / an anchor or a curve gone: a step)
  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    if (pt_.draw >= 0) { const int c = pt_.draw; pt_.draw = -1; if (pt_.c[size_t(c)].a.size() < 2) pt_.c.erase(pt_.c.begin() + c); else { patchJoinAsk(c); patchSplitBy(pt_.selC >= 0 ? pt_.selC : c); } patchChanged(); return true; }
    patchCommit(); return true; }
  if (key == Qt::Key_Escape) {
    if (pt_.ordering) { pt_.ordering = false; patchChanged(); return true; }
    if (pt_.draw >= 0) { const int c = pt_.draw; pt_.draw = -1; if (pt_.c[size_t(c)].a.size() < 2) { pt_.c.erase(pt_.c.begin() + c); pt_.selC = pt_.selA = -1; } patchChanged(); return true; }
    if (pt_.selC >= 0) { pt_.selC = pt_.selA = -1; patchChanged(); return true; }
    return false; }
  if (key == Qt::Key_Backspace && pt_.draw >= 0) { auto& C = pt_.c[size_t(pt_.draw)]; if (!C.a.empty()) C.a.pop_back(); if (C.a.empty()) { pt_.c.erase(pt_.c.begin() + pt_.draw); pt_.draw = -1; pt_.selC = pt_.selA = -1; } patchChanged(); return true; }
  if (key == Qt::Key_Delete && pt_.selC >= 0 && size_t(pt_.selC) < pt_.c.size()) {
    auto& C = pt_.c[size_t(pt_.selC)]; if (pt_.selA >= 0 && size_t(pt_.selA) < C.a.size() && C.a.size() > 2) { C.a.erase(C.a.begin() + pt_.selA); pt_.selA = std::min(pt_.selA, int(C.a.size()) - 1); } else patchDeleteCurve();
    patchChanged(); return true; }
  return false;
}
// an end of curve c lying on another curve's end: one curve of the two — or kept apart (a corner there)
void Viewport::patchJoinAsk(int c) {
  if (c < 0 || size_t(c) >= pt_.c.size() || pt_.c[size_t(c)].a.size() < 2) return;
  const auto& A = pt_.c[size_t(c)].a; const double eps = 1e-9;
  for (size_t o = 0; o < pt_.c.size(); o++) { if (int(o) == c || pt_.c[o].a.size() < 2 || !pt_.c[o].vids.empty() || !pt_.c[size_t(c)].vids.empty()) continue;   // (an edge curve stays one of its own: a corner there)
    const auto& B = pt_.c[o].a;
    int ea = -1, eb = -1; for (int i : {0, 1}) for (int j : {0, 1}) if ((i ? A.back() : A.front()).distanceTo(j ? B.back() : B.front()) < eps) { ea = i; eb = j; }
    if (ea < 0) continue;
    QMessageBox box(QMessageBox::Question, tr("PatchTool"), tr("This curve's end lies on another curve's end. Join them into one curve?"), QMessageBox::NoButton);
    auto* join = box.addButton(tr("Join"), QMessageBox::AcceptRole); box.addButton(tr("Keep apart (a corner)"), QMessageBox::RejectRole); box.exec();
    if (box.clickedButton() != join) return;
    std::vector<Vec3> a = A, b = B; if (ea == 0) std::reverse(a.begin(), a.end()); if (eb == 1) std::reverse(b.begin(), b.end());
    a.insert(a.end(), b.begin() + 1, b.end());
    PatchCurve J = pt_.c[size_t(c)]; J.a = a; J.div = pt_.c[size_t(c)].div + pt_.c[o].div;
    const int lo = std::min(c, int(o)), hi = std::max(c, int(o)); pt_.c.erase(pt_.c.begin() + hi); pt_.c[size_t(lo)] = J; pt_.selC = lo; pt_.selA = -1; pt_.draw = -1;
    ui::hud(tr("Joined: one curve of %1 edges").arg(J.div), 1200); return; }
}
void Viewport::patchOrderClick(int c) {
  auto& O = pt_.order; const auto f = std::find(O.begin(), O.end(), c);
  if (f != O.end()) O.erase(f, O.end()); else O.push_back(c);
  pt_.selC = c; pt_.selA = -1; patchStore(); patchChanged();
  ui::hud(O.empty() ? tr("Loft order: none — automatic") : tr("Loft order: %1 curve%2").arg(O.size()).arg(O.size() == 1 ? "" : "s"), 900);
}
void Viewport::patchDeleteCurve() {
  if (pt_.selC >= 0 && size_t(pt_.selC) < pt_.c.size()) patchPush();
  if (pt_.selC < 0 || size_t(pt_.selC) >= pt_.c.size()) return; pt_.c.erase(pt_.c.begin() + pt_.selC);
  { std::vector<int> o; for (int x : pt_.order) if (x != pt_.selC) o.push_back(x > pt_.selC ? x - 1 : x); pt_.order = o; }   // (Loft's order: the curves after it one back)
  if (pt_.draw == pt_.selC) pt_.draw = -1; else if (pt_.draw > pt_.selC) pt_.draw--; pt_.selC = pt_.selA = -1; patchChanged();
}
void Viewport::patchDeleteAll() { if (!pt_.c.empty()) patchPush(); pt_.c.clear(); pt_.order.clear(); pt_.draw = pt_.selC = pt_.selA = -1; patchChanged(); ui::hud(tr("Every curve deleted"), 900); }

// ---------- made real ----------
QString Viewport::patchCommit() {
  patchBuild(); if (pt_.F.empty()) { ui::toast(pt_.note.isEmpty() ? tr("Nothing to make yet") : pt_.note); return "!"; }
  const int m = patchTarget(); const QString msg = tr("Patch: %1 polygon%2").arg(pt_.F.size()).arg(pt_.F.size() == 1 ? "" : "s");
  if (m >= 0) {
    Item* it = scene_->item(m); const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); const Affine Mi = meshmods::meshMatrix(*scene_, *it).inverse();
    std::vector<int> vi(pt_.P.size(), -1); for (size_t i = 0; i < pt_.P.size(); i++) vi[i] = pt_.vOf[i] >= 0 ? pt_.vOf[i] : E.addVert(Mi.apply(pt_.P[i]));
    std::vector<int> made; for (auto& f : pt_.F) { std::vector<int> vs; for (int x : f) vs.push_back(vi[size_t(x)]); made.push_back(E.addFace(vs)); }
    tt_.symNew = true; const auto used = std::make_shared<const std::vector<PatchCurve>>(pt_.c); const QString r = ttDone(m, E, B, msg, 'f', made);
    if (histIdx_ >= 0 && size_t(histIdx_) < history_.size()) history_[size_t(histIdx_)].patchUsed = used;   // (Ctrl+Z on this step: its curves back)
    pt_.c.clear(); pt_.undo.clear(); pt_.redo.clear(); pt_.draw = pt_.selC = pt_.selA = -1; patchChanged(); return r;   // (the curves done with: the next patch from the rim)
  }
  PolyMesh d; d.pos = pt_.P; for (auto& f : pt_.F) d.addFace(f.data(), int(f.size())); d.faceGroup.assign(size_t(d.faceCount()), 0); d.groupNames = {"Polygroup"};
  if (tset.sym && room() == "topo") { MeshEd E = MeshEd::from(d); symAddFaces(E, 0); d = E.to(d); }
  std::set<QString> taken; for (auto& [id, x] : scene_->items) taken.insert(x.name);
  QString name = "RetopoMesh"; for (int i = 2; taken.count(name); i++) name = QString("RetopoMesh %1").arg(i);
  const auto used = std::make_shared<const std::vector<PatchCurve>>(pt_.c);
  const int id = topoNewMesh(-1, name, d); if (id < 0) return "!";
  sel_ = {}; sel_.items = {id}; tt_.m = id; onSelChange(); scene_->revision++; emit sceneEdited(); commit(true); if (histIdx_ >= 0 && size_t(histIdx_) < history_.size()) history_[size_t(histIdx_)].patchUsed = used; pt_.c.clear(); pt_.undo.clear(); pt_.redo.clear(); pt_.draw = pt_.selC = pt_.selA = -1; patchChanged();
  ui::toast(tr("%1: a new mesh — %2").arg(name, msg)); return msg;
}

// ---------- drawn ----------
void Viewport::patchDraw(TopoDraw& D) {
  patchBuild();
  auto up = [&](const Vec3& p, double px) { const Vec3 to = cam_.ortho() ? cam_.dir() * -1.0 : (cam_.pos - p).normalized(); return p + to * (worldPerPixelAt(p) * px); };   // (over the surface they lie on: no flicker through it)
  auto line = [&](const std::vector<Vec3>& L, const QColor& c, float w) { TopoDraw::Lines l; l.c = c; l.w = w; l.xray = 0.45f; for (size_t i = 0; i + 1 < L.size(); i++) { l.p.push_back(up(L[i], 3)); l.p.push_back(up(L[i + 1], 3)); } if (!l.p.empty()) D.lines.push_back(std::move(l)); };
  // the preview: its polygons faint, their edges cyan — seen faintly through what hides them too (a coarse patch's polygons
  // cut through a rounded surface: they would be all but hidden)
  if (!pt_.F.empty() && pt_.show) { TopoDraw::Tris t; t.c = QColor(0x7f, 0xe8, 0xe0); t.op = float(pt_.fillOp); TopoDraw::Lines w; w.c = QColor(0x7f, 0xe8, 0xe0); w.w = 1.5f; w.xray = 0.45f;
    for (auto& f : pt_.F) { for (size_t k = 1; k + 1 < f.size(); k++) { t.p.push_back(pt_.P[size_t(f[0])]); t.p.push_back(pt_.P[size_t(f[k])]); t.p.push_back(pt_.P[size_t(f[k + 1])]); }
      for (size_t k = 0; k < f.size(); k++) { w.p.push_back(up(pt_.P[size_t(f[k])], 1.5)); w.p.push_back(up(pt_.P[size_t(f[(k + 1) % f.size()])], 1.5)); } }
    D.tris.push_back(std::move(t)); D.lines.push_back(std::move(w)); }
  for (auto& z : pt_.zone) line(z, QColor(0xf0, 0x7a, 0xe8), 3.f);   // (the built mesh's rim taken into the contour: pink)
  for (size_t c = 0; c < pt_.sampled.size(); c++) line(pt_.sampled[c], int(c) == pt_.selC ? QColor(0xe0, 0x90, 0xff) : QColor(0xa8, 0x4c, 0xd0), int(c) == pt_.selC ? 3.2f : 2.6f);   // (the curves: purple)
  TopoDraw::Pts p; p.size = 8; p.xray = true;
  for (size_t c = 0; c < pt_.c.size(); c++) for (size_t a = 0; a < pt_.c[c].a.size(); a++) { p.p.push_back(pt_.c[c].a[a]); p.c.push_back(int(c) == pt_.selC && int(a) == pt_.selA ? QColor(0xff, 0xd8, 0x3a) : int(c) == pt_.selC ? QColor(0xff, 0xff, 0xff) : QColor(0xd8, 0xb8, 0xf0)); }
  if (!p.p.empty()) D.pts.push_back(std::move(p));
}
void Viewport::patchOverlay(std::vector<Overlay>& out) const {
  if (pt_.ring) { out.push_back({{{pt_.ringA.x(), pt_.ringA.y()}, {mouse_.x(), mouse_.y()}}, QColor(0x3a, 0xf0, 0xe8), false}); return; }   // (the ring's stroke)
  if (pt_.draw < 0 || size_t(pt_.draw) >= pt_.c.size() || pt_.c[size_t(pt_.draw)].a.empty()) return;
  const ScreenPt s = projector().toScreen(pt_.c[size_t(pt_.draw)].a.back()); if (s.z >= 1) return;
  out.push_back({{{s.x, s.y}, {mouse_.x(), mouse_.y()}}, QColor(0xc0, 0x70, 0xe8), false});   // (the next span, to the pointer)
}
std::vector<Viewport::ViewLabel> Viewport::viewLabels() const {
  std::vector<ViewLabel> out; if (tool_ != "tpatch" || room() != "topo" || cam_.view == "uv") return out;
  Projector P = projector();
  for (size_t c = 0; c < pt_.c.size() && c < pt_.sampled.size(); c++) { const auto& L = pt_.sampled[c]; if (L.size() < 2) continue;
    const size_t mi = L.size() / 2; const ScreenPt a = P.toScreen(L[mi]), b = P.toScreen(L[std::min(mi + 1, L.size() - 1)]); if (a.z >= 1) continue;
    double nx = -(b.y - a.y), ny = b.x - a.x; const double l = std::hypot(nx, ny); if (l > 0) { nx /= l; ny /= l; } else { nx = 0; ny = -1; }
    const char md = pt_.c[c].mode; const QString tag = md == 'a' ? "AD" : md == 'm' ? "MT" : "EQ";
    const auto oi = std::find(pt_.order.begin(), pt_.order.end(), int(c)); const QString num = pt_.kind == 'l' && oi != pt_.order.end() ? QString("#%1 · ").arg(oi - pt_.order.begin() + 1) : QString();
    out.push_back({QPointF(a.x + nx * 16, a.y + ny * 16), num + QString("%1 %2").arg(pt_.c[c].div).arg(tag), int(c) == pt_.selC ? QColor(0xf0, 0xb0, 0xff) : QColor(0xc8, 0x78, 0xe8)}); }
  return out;
}

}  // namespace hm

namespace hm {
// ---------- its own steps: Ctrl+Z / Ctrl+Y while it is in use (the curves are no part of the scene's undo) ----------
void Viewport::patchPush() {
  pt_.undo.push_back({pt_.c, pt_.draw, pt_.selC, pt_.selA}); pt_.redo.clear();
  if (pt_.undo.size() > 300) pt_.undo.erase(pt_.undo.begin());
}
bool Viewport::patchUndoRedo(bool redo) {
  auto& from = redo ? pt_.redo : pt_.undo; auto& to = redo ? pt_.undo : pt_.redo;
  if (from.empty()) return false;
  to.push_back({pt_.c, pt_.draw, pt_.selC, pt_.selA}); const PatchSt::Step s = from.back(); from.pop_back();
  pt_.c = s.c; pt_.draw = s.draw; pt_.selC = s.selC; pt_.selA = s.selA; pt_.dragA = false;
  patchChanged(); ui::hud(redo ? tr("PatchTool: redone") : tr("PatchTool: undone"), 700); return true;
}
}  // namespace hm

namespace hm {
// ---------- kept with the project: fileRaw's patchCurves (each curve's anchors — world x y z in a row — its edges, its mode) ----------
void Viewport::patchLoad() {
  const bool show = pt_.show; const double op = pt_.fillOp; pt_ = {}; pt_.show = show; pt_.fillOp = op;   // (the preview's look: the user's, not the project's)
  for (const QJsonValue& v : scene_->fileRaw.value("patchLoftOrder").toArray()) pt_.order.push_back(v.toInt());
  for (const QJsonValue& v : scene_->fileRaw.value("patchCurves").toArray()) {
    const QJsonObject o = v.toObject(); PatchCurve C; const QJsonArray a = o.value("a").toArray();
    for (int i = 0; i + 2 < int(a.size()); i += 3) C.a.push_back(Vec3(a[i].toDouble(), a[i + 1].toDouble(), a[i + 2].toDouble()));
    C.div = std::max(1, o.value("div").toInt(6)); const QString md = o.value("mode").toString("e"); C.mode = md.isEmpty() ? 'e' : md.at(0).toLatin1(); C.modeAuto = o.value("modeAuto").toBool(false);   // (an older file: its mode as it was)
    C.mw = o.contains("mw") ? o.value("mw").toDouble() : C.mode == 'm' ? 1 : 0; C.aw = o.contains("aw") ? o.value("aw").toDouble() : C.mode == 'a' ? 1 : 0;
    // (the points it was laid on, as saved: not laid again from whatever the view is at the opening)
    const QJsonArray l = o.value("l").toArray(); for (int i = 0; i + 2 < int(l.size()); i += 3) C.L.push_back(Vec3(l[i].toDouble(), l[i + 1].toDouble(), l[i + 2].toDouble()));
    if (C.L.size() >= 2) C.la = C.a;
    if (C.L.size() >= 2) for (auto& p : C.a) C.lk += QByteArray::number(p.x, 'g', 12) + "," + QByteArray::number(p.y, 'g', 12) + "," + QByteArray::number(p.z, 'g', 12) + ";";
    C.nearLay = C.L.size() < 2;
    for (const QJsonValue& x : o.value("v").toArray()) C.vids.push_back(x.toInt()); if (C.vids.size() != C.a.size()) C.vids.clear(); else { C.L = C.a; C.nearLay = false; }
    if (C.a.size() >= 2) pt_.c.push_back(C); }
  pt_.key.clear();
}
void Viewport::patchStore() {
  QJsonArray all;
  for (const PatchCurve& C : pt_.c) { if (C.a.size() < 2) continue; QJsonArray a; for (const Vec3& p : C.a) { a.append(p.x); a.append(p.y); a.append(p.z); }
    QJsonArray l; if (!C.nearLay) for (const Vec3& p : C.L) { l.append(p.x); l.append(p.y); l.append(p.z); }   // (not laid yet: none — laid when it is opened)
    QJsonObject o{{"a", a}, {"l", l}, {"div", C.div}, {"mode", QString(QChar(C.mode))}, {"modeAuto", C.modeAuto}, {"mw", C.mw}, {"aw", C.aw}};
    if (!C.vids.empty()) { QJsonArray v; for (int x : C.vids) v.append(x); o.insert("v", v); }   // (an edge curve: its vertices)
    all.append(o); }
  if (all.isEmpty()) scene_->fileRaw.remove("patchCurves"); else scene_->fileRaw.insert("patchCurves", all);
  QJsonArray o; for (int x : pt_.order) if (x >= 0 && size_t(x) < pt_.c.size()) o.append(x);
  if (o.isEmpty()) scene_->fileRaw.remove("patchLoftOrder"); else scene_->fileRaw.insert("patchLoftOrder", o);
}
}  // namespace hm

namespace hm {
// the wheel over a curve (within a few pixels of it): its edges, one a notch — the notches close together one step
bool Viewport::patchWheel(int steps) {
  Projector P = projector(); patchBuild(); int bc = -1; double bd = 9;
  for (size_t c = 0; c < pt_.sampled.size() && c < pt_.c.size(); c++) { const auto& L = pt_.sampled[c];
    for (size_t i = 0; i + 1 < L.size(); i++) { const ScreenPt a = P.toScreen(L[i]), b = P.toScreen(L[i + 1]); if (a.z >= 1 || b.z >= 1) continue;
      const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? std::clamp(((mouse_.x() - a.x) * dx + (mouse_.y() - a.y) * dy) / l2, 0.0, 1.0) : 0.0;
      const double d = std::hypot(a.x + dx * t - mouse_.x(), a.y + dy * t - mouse_.y()); if (d < bd) { bd = d; bc = int(c); } } }
  if (bc < 0) return patchSpanWheel(steps);                     // (Loft: over a span — its rows)
  const qint64 now = QDateTime::currentMSecsSinceEpoch(); if (now - pt_.wheelT > 700) patchPush(); pt_.wheelT = now;
  auto& C = pt_.c[size_t(bc)]; C.div = std::clamp(C.div + (steps > 0 ? 1 : -1), 1, 200); pt_.selC = bc; pt_.selA = -1;
  ui::hud(tr("Curve: %1 edges").arg(C.div), 800); patchChanged(); return true;
}
}  // namespace hm

namespace hm {
// ---------- Shift + drag: an edge curve along the built mesh's open edges ----------
// the rim vertex nearest the pointer on the screen (within 14 px, in front), -1 — none
int Viewport::patchRimVertexAt(const QPointF& mp, bool ownRim) const {   // (ownRim: only the rim the trace began on — another rim beside it, across a narrow gap, not taken)
  Projector P = projector(); double bd = 14; int best = -1;
  for (size_t v = 0; v < pt_.trNb.size(); v++) { if (pt_.trNb[v].empty() || (ownRim && v < pt_.trOn.size() && !pt_.trOn[v])) continue; const ScreenPt s = P.toScreen(pt_.trW[v]); const double d = std::hypot(s.x - mp.x(), s.y - mp.y()); if (s.z < 1 && d < bd) { bd = d; best = int(v); } }
  return best;
}
bool Viewport::patchTraceStart(const QPointF& mp) {
  const int tm = patchTarget(); const Item* it = tm >= 0 ? scene_->item(tm) : nullptr;
  // (not on an open edge: a stroke across a part of the model — a ring round it where the stroke crosses it)
  auto ringStart = [&] { if (pt_.draw >= 0) { const int c = pt_.draw; pt_.draw = -1; if (pt_.c[size_t(c)].a.size() < 2) pt_.c.erase(pt_.c.begin() + c); }
    pt_.ring = true; pt_.ringA = mp; tt_.drag = 32; td_ = {}; td_.type = 20; drag_ = Drag::Topo;
    ui::hud(tr("Ring: drag across the part of the model — let go, a closed curve round it there"), 1500); return true; };
  if (!it) return ringStart();
  auto T = topo::ttopo(baseSp(*it)); pt_.trW = ttWorld(tm); pt_.trNb.assign(size_t(T->n), {});
  const auto rim = shownRim(*T, *it); for (int e = 0; e < T->ne; e++) if (rim[size_t(e)]) { pt_.trNb[size_t(T->ea[size_t(e)])].push_back(T->eb[size_t(e)]); pt_.trNb[size_t(T->eb[size_t(e)])].push_back(T->ea[size_t(e)]); }
  const int v = patchRimVertexAt(mp); if (v < 0) return ringStart();
  pt_.trOn.assign(pt_.trNb.size(), 0); { std::vector<int> st{v}; pt_.trOn[size_t(v)] = 1; while (!st.empty()) { const int x = st.back(); st.pop_back(); for (int y : pt_.trNb[size_t(x)]) if (!pt_.trOn[size_t(y)]) { pt_.trOn[size_t(y)] = 1; st.push_back(y); } } }
  if (pt_.draw >= 0) { const int c = pt_.draw; pt_.draw = -1; if (pt_.c[size_t(c)].a.size() < 2) pt_.c.erase(pt_.c.begin() + c); }   // (a curve being drawn: ended)
  patchPush(); PatchCurve C; C.a = {pt_.trW[size_t(v)]}; C.vids = {v}; C.mode = 'e'; C.modeAuto = false; C.div = 1;
  pt_.c.push_back(C); pt_.trace = int(pt_.c.size()) - 1; pt_.selC = pt_.trace; pt_.selA = -1;
  tt_.drag = 31; td_ = {}; td_.type = 20; drag_ = Drag::Topo; patchChanged();
  ui::hud(tr("Edge curve: drag along the open edges — let go to end it"), 1500); return true;
}
void Viewport::patchTraceMove(const QPointF& mp) {
  if (pt_.trace < 0 || size_t(pt_.trace) >= pt_.c.size()) return;
  auto& C = pt_.c[size_t(pt_.trace)]; const int v = patchRimVertexAt(mp, true); if (v < 0 || v == C.vids.back()) return;
  // (back over its own vertices: cut back to there)
  if (auto f = std::find(C.vids.begin(), C.vids.end(), v); f != C.vids.end()) { const size_t k = size_t(f - C.vids.begin()) + 1; C.vids.resize(k); C.a.resize(k); }
  else {   // (the way to it along the rim: the fewest edges — the vertices skipped by a quick move filled in)
    std::vector<int> pr(pt_.trNb.size(), -2); std::deque<int> q{C.vids.back()}; pr[size_t(C.vids.back())] = -1;
    while (!q.empty() && pr[size_t(v)] == -2) { const int x = q.front(); q.pop_front(); for (int y : pt_.trNb[size_t(x)]) if (pr[size_t(y)] == -2) { pr[size_t(y)] = x; q.push_back(y); } }
    if (pr[size_t(v)] == -2) return;                             // (another rim: not joined)
    std::vector<int> way; for (int x = v; x != C.vids.back(); x = pr[size_t(x)]) way.push_back(x); std::reverse(way.begin(), way.end());
    for (int x : way) { if (std::find(C.vids.begin(), C.vids.end(), x) != C.vids.end()) break; C.vids.push_back(x); C.a.push_back(pt_.trW[size_t(x)]); } }
  C.div = std::max(1, int(C.a.size()) - 1); pt_.key.clear(); ttTouch();
  ui::hud(tr("Edge curve: %1 edge%2").arg(C.div).arg(C.div == 1 ? "" : "s"), 800);
}
void Viewport::patchTraceEnd() {
  const int c = pt_.trace; pt_.trace = -1; tt_.drag = 0;
  if (c < 0 || size_t(c) >= pt_.c.size()) return;
  if (pt_.c[size_t(c)].a.size() < 2) { pt_.c.erase(pt_.c.begin() + c); pt_.selC = pt_.selA = -1; patchChanged(); return; }
  ui::hud(tr("Edge curve: %1 edges, along the mesh's open edges").arg(pt_.c[size_t(c)].div), 1200); patchChanged();
}
}  // namespace hm

namespace hm {
namespace {
QByteArray anchorKey(const std::vector<Vec3>& a) { QByteArray k; for (auto& p : a) k += QByteArray::number(p.x, 'g', 12) + "," + QByteArray::number(p.y, 'g', 12) + "," + QByteArray::number(p.z, 'g', 12) + ";"; return k; }
// a curve laid already: its anchors at the lengths `at` along the line L (cum — its lengths), its points sixteen a span
// between them — as it was laid, not laid again by the view
void layAlong(Viewport::PatchCurve& C, const std::vector<Vec3>& L, const std::vector<double>& cum, const std::vector<double>& at) {
  C.a.clear(); C.L.clear(); for (double s : at) C.a.push_back(atLen(L, cum, s));
  for (size_t k = 0; k + 1 < at.size(); k++) for (int s = 0; s < 16; s++) C.L.push_back(atLen(L, cum, at[k] + (at[k + 1] - at[k]) * s / 16.0));
  C.L.push_back(C.a.back()); C.la = C.a; C.lk = anchorKey(C.a); C.nearLay = false;
}
}  // namespace

// ---------- a curve dividing a zone: the curves it crosses (or its ends lie on) cut there, each piece a curve of its own ----------
void Viewport::patchSplitBy(int c) {
  if (c < 0 || size_t(c) >= pt_.c.size() || pt_.kind != 'p') return;   // (Loft / Strip: the curves are its rows — never cut)
  patchBuild(); if (size_t(c) >= pt_.sampled.size() || pt_.sampled[size_t(c)].size() < 2) return;
  const auto Lc = pt_.sampled[size_t(c)];
  double spacing = 0; int ns = 0; for (auto& L : pt_.sampled) for (size_t k = 1; k < L.size(); k++) { spacing += L[k].distanceTo(L[k - 1]); ns++; }
  if (!ns) return; const double tol = std::max(1e-9, spacing / ns * 1.5);
  auto closed = [&](const PatchCurve& C) { return C.a.size() >= 3 && C.a.front().distanceTo(C.a.back()) < 1e-9; };
  auto nearOn = [&](const std::vector<Vec3>& B, const std::vector<double>& cb, const Vec3& q, double& at) { double bd = 1e300;
    for (size_t b = 0; b + 1 < B.size(); b++) { const Vec3 d = B[b + 1] - B[b]; const double l2 = d.dot(d), t = l2 > 0 ? std::clamp((q - B[b]).dot(d) / l2, 0.0, 1.0) : 0.0;
      const double dd = (B[b] + d * t).distanceTo(q); if (dd < bd) { bd = dd; at = cb[b] + (cb[b + 1] - cb[b]) * t; } } return bd; };
  // (where it touches each other curve: its crossings, its ends on that one's body)
  // (near the ends of either curve it is a corner there, not a cut: a crossing within a few tolerances of an end left out;
  // the hits of one crossing — a shallow one touches several segments — one)
  std::map<int, std::vector<double>> cutAt; bool held[2] = {false, false}; int touches = 0;
  const auto lcum = cumOf(Lc); const double LT = lcum.back();
  for (size_t o = 0; o < pt_.c.size() && o < pt_.sampled.size(); o++) { const auto& B = pt_.sampled[o]; if (int(o) == c || B.size() < 2) continue; const auto cb = cumOf(B); const double T = cb.back();
    const bool cuttable = !closed(pt_.c[o]); auto inside = [&](double s) { return s > tol * 2 && s < T - tol * 2; };
    for (int end = 0; end < 2; end++) { double at = 0; if (nearOn(B, cb, end ? Lc.back() : Lc.front(), at) < tol) { held[end] = true; if (cuttable && inside(at)) cutAt[int(o)].push_back(at); } }
    std::vector<double> hits;
    for (size_t a = 0; a + 1 < Lc.size(); a++) for (size_t b = 0; b + 1 < B.size(); b++) { double s, t; if (segSeg(Lc[a], Lc[a + 1], B[b], B[b + 1], s, t) >= tol) continue;
      const double sc = lcum[a] + (lcum[a + 1] - lcum[a]) * s; if (sc < tol * 3 || sc > LT - tol * 3) continue;
      hits.push_back(cb[b] + (cb[b + 1] - cb[b]) * t); }
    std::sort(hits.begin(), hits.end());
    for (size_t i = 0; i < hits.size();) { size_t j = i; double sum = 0; while (j < hits.size() && hits[j] - hits[i] < tol * 4) sum += hits[j++];
      const double at = sum / double(j - i); touches++; if (cuttable && inside(at)) cutAt[int(o)].push_back(at); i = j; } }
  // (its ends on the built mesh's open rim count as held too)
  if (const int tm = patchTarget(); tm >= 0 && !(held[0] && held[1])) { const Item* it = scene_->item(tm); auto T = topo::ttopo(baseSp(*it)); const auto W = ttWorld(tm); const auto rim = shownRim(*T, *it);
    for (int end = 0; end < 2; end++) { const Vec3 q = end ? Lc.back() : Lc.front();
      for (int e = 0; e < T->ne && !held[end]; e++) if (rim[size_t(e)]) for (int v : {T->ea[size_t(e)], T->eb[size_t(e)]}) if (W[size_t(v)].distanceTo(q) < tol) held[end] = true; } }
  touches += int(held[0]) + int(held[1]);
  if (touches < 2) return;                                      // (one zone not divided by it: nothing cut)
  int split = 0;
  for (auto it = cutAt.rbegin(); it != cutAt.rend(); ++it) {   // (from the last: the earlier indices stay as they are)
    const int o = it->first; const PatchCurve O = pt_.c[size_t(o)]; const auto B = pt_.sampled[size_t(o)]; const auto cb = cumOf(B); const double T = cb.back();
    std::vector<double> at = it->second; std::sort(at.begin(), at.end());
    std::vector<double> cuts; for (double s : at) if (cuts.empty() || s - cuts.back() > tol * 2) cuts.push_back(s);
    if (cuts.empty()) continue;
    // (each anchor's place along it)
    std::vector<double> aAt; for (size_t i = 0; i < O.a.size(); i++) { double s = 0; nearOn(B, cb, O.a[i], s); aAt.push_back(i == 0 ? 0.0 : i + 1 == O.a.size() ? T : s); }
    std::vector<double> bounds{0.0}; for (double s : cuts) bounds.push_back(s); bounds.push_back(T);
    std::vector<PatchCurve> parts;
    for (size_t k = 0; k + 1 < bounds.size(); k++) { const double s0 = bounds[k], s1 = bounds[k + 1]; PatchCurve P = O;
      if (!O.vids.empty()) {                                    // (an edge curve: cut at the mesh's vertex nearest the crossing)
        auto idxAt = [&](double s) { size_t bi = 0; for (size_t i = 0; i < aAt.size(); i++) if (std::abs(aAt[i] - s) < std::abs(aAt[bi] - s)) bi = i; return bi; };
        const size_t i0 = idxAt(s0), i1 = idxAt(s1); if (i1 <= i0) continue;
        P.a.assign(O.a.begin() + long(i0), O.a.begin() + long(i1) + 1); P.vids.assign(O.vids.begin() + long(i0), O.vids.begin() + long(i1) + 1); P.L = P.a; P.div = int(P.a.size()) - 1; parts.push_back(P); continue; }
      std::vector<double> pa{s0}; for (double s : aAt) if (s > s0 + tol * 0.5 && s < s1 - tol * 0.5) pa.push_back(s); pa.push_back(s1);
      layAlong(P, B, cb, pa); P.div = std::max(1, int(std::lround(O.div * (s1 - s0) / std::max(1e-12, T)))); parts.push_back(P); }
    if (parts.size() < 2) continue;
    if (O.vids.empty()) { int sum = 0; for (auto& P : parts) sum += P.div; if (sum != O.div && O.div >= int(parts.size())) parts.back().div = std::max(1, parts.back().div + O.div - sum); }   // (the edges it had, shared among its pieces)
    pt_.c.erase(pt_.c.begin() + o); pt_.c.insert(pt_.c.begin() + o, parts.begin(), parts.end()); split++;
    if (pt_.selC > o) pt_.selC += int(parts.size()) - 1; if (pt_.draw > o) pt_.draw += int(parts.size()) - 1; }
  if (split) { pt_.key.clear(); ui::hud(tr("The zone divided: %1 curve%2 it crosses cut in two — each piece's edges set on its own (the wheel over it)").arg(split).arg(split == 1 ? "" : "s"), 1800); }
}

// ---------- Shift + drag off the open edges: a ring round the part of the model the stroke crosses ----------
void Viewport::patchRingEnd(const QPointF& a, const QPointF& b) {
  if (std::hypot(b.x() - a.x(), b.y() - a.y()) < 8) { ui::hud(tr("Ring: drag across a part of the model — an arm, a leg — a closed curve round it there"), 1500); return; }
  // (the part it crosses: what the view sees along the stroke, nearest its middle — not the mesh being built)
  const int tm = patchTarget(); std::optional<RayHit> hit; double bm = 1e9;
  for (int i = 0; i <= 32; i++) { const double t = i / 32.0; const QPointF q = a + (b - a) * t; auto h = pickSurface(q); if (!h || h->itemId == tm) continue;
    const Item* x = scene_->item(h->itemId); if (!x || x->type != ItemType::IMesh || !x->poly) continue;
    if (std::abs(t - 0.5) < bm) { bm = std::abs(t - 0.5); hit = h; } }
  if (!hit) { ui::hud(tr("Ring: the stroke crosses no part of the model"), 1500); return; }
  const Item& X = *scene_->item(hit->itemId);
  // (its points in the world: a high-poly one in its file's space with its matrix)
  std::shared_ptr<PolyMesh> src = X.poly; Affine M; bool useM = false;
  if (auto h = heavy_.find(hit->itemId); h != heavy_.end() && h->second.src.lock() == baseSp(X)) { src = baseSp(X); M = meshmods::meshMatrix(*scene_, X); useM = true; }
  const PolyMesh& G = *src;
  auto P = [&](int v) { return useM ? M.apply(G.pos[size_t(v)]) : G.pos[size_t(v)]; };
  // (the plane: through the two rays at the stroke's ends)
  const Ray ra = ray(a), rb = ray(b); const double dep = std::max(1e-6, hit->point.distanceTo(ra.o));
  Vec3 n = ((rb.o + rb.d * dep) - (ra.o + ra.d * dep)).cross(ra.d); if (n.lengthSq() < 1e-24) return; n = n.normalized(); const Vec3 o = ra.o;
  std::vector<double> sd(G.pos.size()); for (size_t v = 0; v < G.pos.size(); v++) { double d = n.dot(P(int(v)) - o); if (d == 0) d = 1e-12; sd[v] = d; }
  // (the plane's cut through every polygon it crosses: a segment; the segments joined where they share an edge's point)
  const Box3 bb = G.bounds(); const double q = std::max(1e-9, bb.size().length() * 1e-7);
  std::map<std::array<long long, 3>, int> nodeOf; std::vector<Vec3> np; std::vector<std::vector<int>> adj; std::vector<std::array<int, 2>> seg;
  auto node = [&](const Vec3& p) { const std::array<long long, 3> k{std::llround(p.x / q), std::llround(p.y / q), std::llround(p.z / q)}; auto f = nodeOf.find(k); if (f != nodeOf.end()) return f->second;
    np.push_back(p); adj.emplace_back(); return nodeOf[k] = int(np.size()) - 1; };
  for (int f = 0; f < G.faceCount(); f++) { const int s = G.faceStart[size_t(f)], e = G.faceStart[size_t(f) + 1];
    std::vector<Vec3> cut; for (int k = s; k < e; k++) { const int u = G.faceVerts[size_t(k)], w = G.faceVerts[size_t(k + 1 < e ? k + 1 : s)];
      if ((sd[size_t(u)] > 0) == (sd[size_t(w)] > 0)) continue;
      const int lo = std::min(u, w), hi = std::max(u, w); const double t = sd[size_t(lo)] / (sd[size_t(lo)] - sd[size_t(hi)]);   // (the same edge's point from either polygon: alike)
      cut.push_back(P(lo) + (P(hi) - P(lo)) * t); }
    if (cut.size() != 2) continue; const int x0 = node(cut[0]), x1 = node(cut[1]); if (x0 == x1) continue;
    adj[size_t(x0)].push_back(int(seg.size())); adj[size_t(x1)].push_back(int(seg.size())); seg.push_back({x0, x1}); }
  if (seg.empty()) { ui::hud(tr("Ring: no cut through that part"), 1500); return; }
  // (the piece of it by the point the view hit: walked round from there)
  int s0 = 0; double bd = 1e300; for (size_t i = 0; i < seg.size(); i++) { double s, t; const double d = segSeg(np[size_t(seg[i][0])], np[size_t(seg[i][1])], hit->point, hit->point, s, t); if (d < bd) { bd = d; s0 = int(i); } }
  std::vector<char> used(seg.size(), 0);
  auto walk = [&](int from, int sIdx) { std::vector<int> path{from}; int cur = from, s = sIdx;
    while (s >= 0 && !used[size_t(s)]) { used[size_t(s)] = 1; const int nx = seg[size_t(s)][0] == cur ? seg[size_t(s)][1] : seg[size_t(s)][0]; path.push_back(nx); cur = nx; s = -1;
      for (int t : adj[size_t(cur)]) if (!used[size_t(t)]) { s = t; break; } }
    return path; };
  auto fw = walk(seg[size_t(s0)][0], s0); const bool isClosed = fw.size() > 3 && fw.back() == fw.front();
  if (!isClosed) { int s = -1; for (int t : adj[size_t(seg[size_t(s0)][0])]) if (!used[size_t(t)]) { s = t; break; }
    if (s >= 0) { auto bw = walk(seg[size_t(s0)][0], s); std::reverse(bw.begin(), bw.end()); bw.pop_back(); bw.insert(bw.end(), fw.begin(), fw.end()); fw = bw; } }
  std::vector<Vec3> L; for (int x : fw) L.push_back(np[size_t(x)]); if (isClosed) L.back() = L.front();
  if (L.size() < 4) { ui::hud(tr("Ring: the cut there is too small"), 1500); return; }
  // (a curve of twelve spans, laid on the cut exactly — closed: its last anchor its first)
  const auto cum = cumOf(L); const double T = cum.back(); const int spans = 12; std::vector<double> at; for (int k = 0; k <= spans; k++) at.push_back(T * k / spans);
  PatchCurve C; layAlong(C, L, cum, at); if (isClosed) { C.a.back() = C.a.front(); C.L.back() = C.a.front(); C.la = C.a; C.lk = anchorKey(C.a); }
  C.div = 16; C.mode = 'e'; C.modeAuto = false;
  patchPush(); pt_.c.push_back(C); pt_.selC = int(pt_.c.size()) - 1; pt_.selA = -1; patchChanged();
  ui::hud(isClosed ? tr("Ring: %1 edges round it — a second ring makes a tube between them; a curve from ring to ring — its seam and height").arg(C.div)
                   : tr("Ring: the cut there is open (the model has a hole in it) — a curve of %1 edges along it").arg(C.div), 2500);
}

// Loft and Strip (PatchTool's other kinds): built from the curves themselves, not the zones they close — the preview's
// points and polygons as a patch's, so Create patch, Symmetry and the steps back work the same
void Viewport::patchLoftStrip() {
  PatchSt& S = pt_; S.P.clear(); S.vOf.clear(); S.F.clear(); S.zone.clear(); S.spanOf.clear(); S.patches = 0; S.zoneChoice = false; S.note.clear();
  std::vector<int> cs; for (size_t i = 0; i < S.c.size(); i++) if (S.c[i].L.size() >= 2) cs.push_back(int(i));
  auto edgesOf = [&](int i) { const auto& C = S.c[size_t(i)]; return std::max(1, C.vids.empty() ? C.div : int(C.vids.size()) - 1); };
  Projector Pj = projector(); double edge = 0; int ne = 0;
  for (int i : cs) { edge += cumOf(S.c[size_t(i)].L).back() / edgesOf(i); ne++; }
  edge = ne ? edge / ne : 1; const double far = edge * 6;
  // (a point onto what the view sees there, near it — else the nearest surface; the surface's normal there too)
  auto hitAt = [&](const Vec3& p) -> std::optional<RayHit> { const ScreenPt s = Pj.toScreen(p);
    if (s.z < 1 && s.x >= 0 && s.y >= 0 && s.x < width() && s.y < height()) if (auto h = pickSurface(QPointF(s.x, s.y)); h && h->point.distanceTo(p) < far) return h;
    return std::nullopt; };
  auto stick = [&](const Vec3& p) { return patchOnRefs(p); };     // (the nearest surface, not the view's: the same from any side — a turn of the view changes nothing of it)
  std::map<int, int> byVid;
  auto add = [&](const Vec3& p, int vid) { if (vid >= 0) { auto f = byVid.find(vid); if (f != byVid.end()) return f->second; }
    S.P.push_back(p); S.vOf.push_back(vid); if (vid >= 0) byVid[vid] = int(S.P.size()) - 1; return int(S.P.size()) - 1; };
  // (the polygons wound alike along the rows, then all of a piece turned at once to face the view — by most of their area,
  // not each its own: one seen edge-on at the sides would flip and the patch's sides come out wound the other way)
  auto facing = [](std::vector<int> f) { return f; };
  auto orient = [&](size_t from) { double vote = 0;
    for (size_t i = from; i < S.F.size(); i++) { const auto& f = S.F[i]; Vec3 nrm, c; for (size_t k = 0; k < f.size(); k++) { nrm += S.P[size_t(f[k])].cross(S.P[size_t(f[(k + 1) % f.size()])]); c += S.P[size_t(f[k])]; } c = c * (1.0 / f.size());
      vote += nrm.dot((cam_.ortho() ? cam_.dir() : (c - cam_.pos).normalized())); }
    if (vote > 0) for (size_t i = from; i < S.F.size(); i++) std::reverse(S.F[i].begin(), S.F[i].end()); };
  if (S.kind == 's') {                                             // Strip: each curve the middle of a band
    if (cs.empty()) { S.note = tr("Strip: draw a curve — the middle of the band"); return; }
    const int A = std::clamp(S.stripAcross, 1, 32); const double half = std::max(1e-6, S.stripW) / 2;
    for (int i : cs) { const auto& C = S.c[size_t(i)]; const auto pts = resampleW(C.L, edgesOf(i), C.aw); const size_t n = pts.size(); const size_t f0 = S.F.size();
      std::vector<std::vector<int>> col(n); Vec3 prevSide;
      for (size_t k = 0; k < n; k++) { const Vec3 t = (pts[std::min(k + 1, n - 1)] - pts[k > 0 ? k - 1 : 0]).normalized();
        const Vec3 toCam = cam_.ortho() ? cam_.dir() * -1.0 : (cam_.pos - pts[k]).normalized();   // (the surface's normal turned to the view — a triangle wound either way)
        Vec3 nr = toCam; if (auto h = hitAt(pts[k])) { nr = h->normal; if (nr.dot(toCam) < 0) nr = nr * -1.0; }
        Vec3 side = t.cross(nr); if (side.lengthSq() < 1e-18) side = t.cross(toCam); side = side.normalized();
        if (k > 0 && side.dot(prevSide) < 0) side = side * -1.0; prevSide = side;   // (the band never turned over along the curve)
        for (int j = 0; j <= A; j++) col[k].push_back(add(stick(pts[k] + side * (-half + 2 * half * j / A)), -1)); }
      for (size_t k = 0; k + 1 < n; k++) for (int j = 0; j < A; j++) S.F.push_back(facing({col[k][size_t(j)], col[k + 1][size_t(j)], col[k + 1][size_t(j) + 1], col[k][size_t(j) + 1]}));
      orient(f0); S.patches++; }
    return; }
  // Loft: the curves in turn — from the one farthest out, each next the nearest; each with its own edges (an edge curve —
  // its vertices), turned the way the one before runs; rows between two, on the surface — two curves of different
  // edges meet in the middle row: quads where the points pair up, a triangle for each edge more on one side
  if (cs.size() < 2) { S.note = tr("Loft: draw two curves — the surface goes between them (a curve between two — a section)"); return; }
  auto midOf = [&](int i) { Vec3 c; for (auto& p : S.c[size_t(i)].L) c += p; return c * (1.0 / double(S.c[size_t(i)].L.size())); };
  Vec3 all; for (int i : cs) all += midOf(i); all = all * (1.0 / double(cs.size()));
  std::vector<int> order;
  { std::vector<char> used(cs.size(), 0); size_t s0 = 0; double bd = -1;
    for (int x : S.order) { const auto f = std::find(cs.begin(), cs.end(), x); if (f != cs.end() && !used[size_t(f - cs.begin())]) { used[size_t(f - cs.begin())] = 1; order.push_back(x); } }   // (the order clicked — the rest after it by nearness)
    if (order.empty()) {
    for (size_t q = 0; q < cs.size(); q++) { const double d = midOf(cs[q]).distanceTo(all); if (d > bd) { bd = d; s0 = q; } }
    order.push_back(cs[s0]); used[s0] = 1; }
    while (order.size() < cs.size()) { size_t b = 0; double bdd = 1e300;
      for (size_t q = 0; q < cs.size(); q++) if (!used[q]) { const double d = midOf(cs[q]).distanceTo(midOf(order.back())); if (d < bdd) { bdd = d; b = q; } }
      order.push_back(cs[b]); used[b] = 1; } }
  std::vector<std::vector<int>> rowsAt; std::vector<Vec3> prev;     // (each curve's points in the preview)
  // (a Y fork: a curve whose end lies on another one's middle — carried on along that one, the way it goes there, to that
  // one's end: the rows then run the long one's whole length. Its edges as long as its own; the curve itself as it is)
  auto forked = [&](int i, int& edges) { std::vector<Vec3> L = S.c[size_t(i)].L; const double own = cumOf(L).back(); const double tol = edge * 0.3;
    for (int end = 0; end < 2; end++) { const Vec3 q = end ? L.back() : L.front(); const Vec3 dir = end ? L.back() - L[L.size() - 2] : L.front() - L[1];
      for (int o : cs) { if (o == i) continue; const auto& B = S.c[size_t(o)].L; const auto cb = cumOf(B); const double T = cb.back(); double at = 0, bd = 1e300; size_t seg = 0;
        for (size_t b = 0; b + 1 < B.size(); b++) { const Vec3 d = B[b + 1] - B[b]; const double l2 = d.dot(d), t = l2 > 0 ? std::clamp((q - B[b]).dot(d) / l2, 0.0, 1.0) : 0.0; const double dd = (B[b] + d * t).distanceTo(q); if (dd < bd) { bd = dd; at = cb[b] + (cb[b + 1] - cb[b]) * t; seg = b; } }
        if (bd > tol || at < tol * 3 || at > T - tol * 3) continue;                 // (not on it, or at its end — a plain joint)
        const bool fwd = dir.dot(B[seg + 1] - B[seg]) >= 0; std::vector<Vec3> ext;  // (on along it the way the curve comes in)
        if (fwd) { for (size_t b = seg + 1; b < B.size(); b++) ext.push_back(B[b]); } else { for (size_t b = seg + 1; b-- > 0;) ext.push_back(B[b]); }
        if (end) L.insert(L.end(), ext.begin(), ext.end()); else { std::reverse(ext.begin(), ext.end()); L.insert(L.begin(), ext.begin(), ext.end()); }
        break; } }
    edges = std::max(1, int(std::lround(edgesOf(i) * cumOf(L).back() / std::max(1e-12, own)))); return L; };
  for (int i : order) { const auto& C = S.c[size_t(i)]; std::vector<Vec3> p; std::vector<int> v;
    if (!C.vids.empty() && C.L.size() == C.vids.size()) { p = C.L; v = C.vids; } else { int ne = 0; const auto L = forked(i, ne); p = resampleW(L, ne, C.aw); v.assign(p.size(), -1); }
    if (!prev.empty() && p.front().distanceTo(prev.front()) + p.back().distanceTo(prev.back()) > p.front().distanceTo(prev.back()) + p.back().distanceTo(prev.front())) { std::reverse(p.begin(), p.end()); std::reverse(v.begin(), v.end()); }
    std::vector<int> r; for (size_t k = 0; k < p.size(); k++) r.push_back(add(p[k], v[k])); rowsAt.push_back(r); prev = p; }
  auto posOf = [&](const std::vector<int>& r) { std::vector<Vec3> p; for (int x : r) p.push_back(S.P[size_t(x)]); return p; };
  auto parOf = [](const std::vector<Vec3>& p) { std::vector<double> u(p.size(), 0); for (size_t k = 1; k < p.size(); k++) u[k] = u[k - 1] + p[k].distanceTo(p[k - 1]);
    const double L = u.empty() ? 0 : u.back(); for (size_t k = 0; k < u.size(); k++) u[k] = L > 1e-12 ? u[k] / L : (u.size() > 1 ? double(k) / double(u.size() - 1) : 0); return u; };
  auto atU = [](const std::vector<Vec3>& p, const std::vector<double>& u, double t) { if (p.size() < 2) return p.empty() ? Vec3{} : p[0];
    size_t k = 1; while (k + 1 < p.size() && u[k] < t) k++; const double d = u[k] - u[k - 1]; const double w = d > 1e-12 ? std::clamp((t - u[k - 1]) / d, 0.0, 1.0) : 0.0; return p[k - 1] + (p[k] - p[k - 1]) * w; };
  for (size_t q = 0; q + 1 < order.size(); q++) {
    const std::pair<int, int> key{std::min(order[q], order[q + 1]), std::max(order[q], order[q + 1])};
    const auto &A0 = rowsAt[q], &B0 = rowsAt[q + 1]; const auto pa = posOf(A0), pb = posOf(B0); const auto ua = parOf(pa), ub = parOf(pb);
    double gap = 0; for (int k = 0; k <= 8; k++) gap += atU(pa, ua, k / 8.0).distanceTo(atU(pb, ub, k / 8.0)); gap /= 9;
    auto f = S.loftRows.find(key); const int R = f != S.loftRows.end() ? f->second : std::clamp(int(std::lround(gap / std::max(1e-12, edge))), 1, 64);
    std::vector<std::vector<int>> G{A0};
    for (int r = 1; r < R; r++) { const bool sideA = 2 * r < R; const auto& us = sideA ? ua : ub; std::vector<int> row;   // (the first half as A's, the rest as B's)
      for (double u : us) { const Vec3 a = atU(pa, ua, u), b = atU(pb, ub, u), p = a + (b - a) * (double(r) / R); Vec3 q = stick(p);
        if (q.distanceTo(p) < 1e-12) if (auto h = hitAt(p)) q = h->point;   // (no reference it can measure to — a high-poly one: what the view sees there, held to the same reach below)
        row.push_back(add(q.distanceTo(p) <= a.distanceTo(b) * 0.35 ? q : p, -1)); }   // (onto the surface — not onto a fold's other layer, farther than the curves are apart there)
      G.push_back(row); }
    G.push_back(B0);
    for (size_t r = 0; r + 1 < G.size(); r++) { const auto &X = G[r], &Y = G[r + 1];
      if (X.size() == Y.size()) { for (size_t k = 0; k + 1 < X.size(); k++) { S.F.push_back(facing({X[k], X[k + 1], Y[k + 1], Y[k]})); S.spanOf.push_back(key); } continue; }
      // (rows of different counts zipped: each step the pairing that keeps the two sides' places nearest)
      const auto px = parOf(posOf(X)), py = parOf(posOf(Y)); size_t i = 0, j = 0; const size_t nx = X.size() - 1, ny = Y.size() - 1;
      while (i < nx || j < ny) {
        const double cq = i < nx && j < ny ? std::abs(px[i + 1] - py[j + 1]) : 1e300, cx = i < nx ? std::abs(px[i + 1] - py[j]) : 1e300, cy = j < ny ? std::abs(px[i] - py[j + 1]) : 1e300;
        if (cq <= cx && cq <= cy) { S.F.push_back(facing({X[i], X[i + 1], Y[j + 1], Y[j]})); i++; j++; }
        else if (cx <= cy) { S.F.push_back(facing({X[i], X[i + 1], Y[j]})); i++; }
        else { S.F.push_back(facing({X[i], Y[j + 1], Y[j]})); j++; }
        S.spanOf.push_back(key); } }
    S.loftRows.emplace(key, R);                                    // (its rows kept — the wheel goes on from them)
    S.patches++; }
  orient(0);
}
// Loft: the wheel over a span's polygons — its rows, one a notch
bool Viewport::patchSpanWheel(int steps) {
  if (pt_.kind != 'l' || pt_.spanOf.size() != pt_.F.size()) return false;
  Projector P = projector();
  for (size_t i = 0; i < pt_.F.size(); i++) { std::vector<std::array<double, 2>> poly; bool ok = true;
    for (int x : pt_.F[i]) { const ScreenPt s = P.toScreen(pt_.P[size_t(x)]); if (s.z >= 1) ok = false; poly.push_back({s.x, s.y}); }
    if (!ok || !inPoly2(poly, mouse_.x(), mouse_.y())) continue;
    const auto key = pt_.spanOf[i]; int& R = pt_.loftRows[key]; if (R < 1) R = 1;
    const qint64 now = QDateTime::currentMSecsSinceEpoch(); if (now - pt_.wheelT > 700) patchPush(); pt_.wheelT = now;
    R = std::clamp(R + (steps > 0 ? 1 : -1), 1, 64);
    ui::hud(tr("Loft: %1 row%2 between these curves").arg(R).arg(R == 1 ? "" : "s"), 800); patchChanged(); return true; }
  return false;
}
// Update: the zones found anew — Switch zone off, every curve's mode back to the automatic one (Match with both ends on the rim)
void Viewport::patchRedetect() {
  pt_.zoneAlt = false; for (auto& C : pt_.c) if (C.vids.empty()) C.modeAuto = true;
  pt_.key.clear(); patchChanged(); patchBuild();
  ui::hud(pt_.F.empty() ? (pt_.note.isEmpty() ? tr("Update: nothing closes a zone yet") : pt_.note) : tr("Update: %1 polygons").arg(pt_.F.size()), 1200);
}
}  // namespace hm
