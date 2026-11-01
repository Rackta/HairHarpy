#include "scene/LockExtrude.h"
#include "scene/Edit.h"
#include "scene/Network.h"
#include <QJsonObject>
#include <optional>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace hm::lockex {

namespace {
const CurveSampling* smp(const CurveCache& cc, int id) { auto it = cc.find(id); return it == cc.end() || it->second.p.size() < 2 ? nullptr : &it->second; }
double ease(double s) { s = std::clamp(s, 0.0, 1.0); return s * s * (3 - 2 * s); }
// the point at a share u of the curve's length (from its first anchor)
Vec3 at(const CurveSampling& S, double u) {
  const double d = std::clamp(u, 0.0, 1.0) * S.arc.back(); size_t i = 0; while (i + 2 < S.arc.size() && S.arc[i + 1] < d) i++;
  const double g = S.arc[i + 1] - S.arc[i]; return S.p[i].lerp(S.p[i + 1], g > 0 ? std::clamp((d - S.arc[i]) / g, 0.0, 1.0) : 0.0);
}
// the share of the length where the curve passes nearest p
double uOf(const CurveSampling& S, const Vec3& p) {
  double bd = 1e300, bu = 0; const double L = std::max(1e-12, S.arc.back());
  for (size_t i = 0; i + 1 < S.p.size(); i++) { const Vec3 d = S.p[i + 1] - S.p[i]; const double l2 = d.dot(d), t = l2 > 0 ? std::clamp((p - S.p[i]).dot(d) / l2, 0.0, 1.0) : 0.0;
    const double dd = (S.p[i] + d * t).distanceTo(p); if (dd < bd) { bd = dd; bu = (S.arc[i] + (S.arc[i + 1] - S.arc[i]) * t) / L; } }
  return bu;
}
// the walls among the items: selected walls, walls between two selected curves, every wall inside a selected group
std::set<int> wallsOf(const Scene& sc, const std::vector<int>& items) {
  std::set<int> sel(items.begin(), items.end()), out;
  for (auto& [id, w] : sc.items) {
    if (w.type != ItemType::Surface || w.a < 0 || w.b < 0) continue;
    bool take = sel.count(id) || (sel.count(w.a) && sel.count(w.b));
    if (!take) for (int g : items) if (const Item* gi = sc.item(g); gi && gi->type == ItemType::Group && sc.isDescendant(id, g)) take = true;
    if (take) out.insert(id);
  }
  return out;
}
// the chains of curves the walls join (only simple paths: a ring or a fork — not a flat lock)
std::vector<std::vector<int>> chains(const Scene& sc, const std::set<int>& walls) {
  std::map<int, std::vector<int>> nb;
  for (int w : walls) { const Item* x = sc.item(w); nb[x->a].push_back(x->b); nb[x->b].push_back(x->a); }
  std::set<int> seen; std::vector<std::vector<int>> out;
  for (auto& [c0, _] : nb) {
    if (seen.count(c0)) continue;
    std::vector<int> comp, st{c0}; seen.insert(c0);
    while (!st.empty()) { int c = st.back(); st.pop_back(); comp.push_back(c); for (int x : nb[c]) if (!seen.count(x)) { seen.insert(x); st.push_back(x); } }
    int ends = 0, start = -1; bool ok = true;
    for (int c : comp) { const size_t k = nb[c].size(); if (k > 2) ok = false; if (k == 1) { ends++; if (start < 0) start = c; } }
    if (!ok || ends != 2) continue;                                   // (a ring — a volume lock already — or a fork)
    std::vector<int> ch{start}; int prev = -1, cur = start;
    while (true) { int nx = -1; for (int x : nb[cur]) if (x != prev) { nx = x; break; } if (nx < 0) break; prev = cur; cur = nx; ch.push_back(cur); }
    if (ch.size() >= 2) out.push_back(ch);
  }
  return out;
}
}  // namespace

std::vector<std::vector<int>> strips(const Scene& sc, const std::vector<int>& items) { return chains(sc, wallsOf(sc, items)); }

std::vector<int> stripOf(const Scene& sc, int id) {
  const Item* it = sc.item(id); if (!it) return {};
  int c = it->type == ItemType::Surface ? it->a : it->type == ItemType::Curve ? id : -1; if (c < 0) return {};
  // (every wall joined to it through walls)
  std::set<int> walls, curves{c}; std::vector<int> st{c};
  while (!st.empty()) { int x = st.back(); st.pop_back();
    for (auto& [wid, w] : sc.items) if (w.type == ItemType::Surface && (w.a == x || w.b == x) && w.a >= 0 && w.b >= 0) { walls.insert(wid); for (int y : {w.a, w.b}) if (curves.insert(y).second) st.push_back(y); } }
  auto ch = chains(sc, walls); return ch.empty() ? std::vector<int>{} : ch[0];
}

namespace {
// the lock's curves read the same way (the first one's root → tip) and the outward normal along each, for the layers
struct Frame {
  std::vector<const CurveSampling*> S; std::vector<bool> rev; double sgn = 1; size_t n = 0;
  Vec3 P(size_t i, double u) const { return at(*S[i], rev[i] ? 1 - u : u); }
  Vec3 N(size_t i, double u) const {
    const double du = 0.02; Vec3 t = P(i, std::min(1.0, u + du)) - P(i, std::max(0.0, u - du));
    Vec3 x = (i + 1 < n ? P(i + 1, u) : P(i, u)) - (i > 0 ? P(i - 1, u) : P(i, u));
    Vec3 nn = t.cross(x); return nn.lengthSq() > 1e-18 ? nn.normalized() * sgn : Vec3(); }
};
std::optional<Frame> frameOf(const CurveCache& cc, const std::vector<int>& strip, const Vec3& head) {
  Frame F; F.n = strip.size(); if (F.n < 2) return std::nullopt;
  for (int c : strip) { const CurveSampling* s = smp(cc, c); if (!s) return std::nullopt; F.S.push_back(s); }
  F.rev.assign(F.n, false);
  for (size_t i = 1; i < F.n; i++) { const Vec3 a0 = F.S[0]->p.front(), a1 = F.S[0]->p.back(), b0 = F.S[i]->p.front(), b1 = F.S[i]->p.back();
    F.rev[i] = a0.distanceTo(b0) + a1.distanceTo(b1) > a0.distanceTo(b1) + a1.distanceTo(b0); }
  // (u = 0 — the roots: the first curve's first anchor, its diamond — a curve runs root → tip)
  double vote = 0; for (size_t i = 0; i < F.n; i++) for (int q = 1; q < 10; q++) { const double u = q / 10.0; vote += F.N(i, u).dot(F.P(i, u) - head); }
  F.sgn = vote < 0 ? -1 : 1;
  return F;
}
// a curve laid as a layer of the lock's curve i: its anchors where i's are (they move with the thickness), the tip end
// shared with i (welded)
Layer makeLayer(Scene& sc, Network& net, const Frame& F, size_t i, int curve, int parent, const QString& name) {
  Layer L; const Item* o = sc.item(curve); const std::vector<int> an = o->anchors; if (an.size() < 2) return L;
  std::vector<int> cp;
  for (size_t k = 0; k < an.size(); k++) {
    const Vec3 p = sc.anchor(an[k])->pos; double u = uOf(*F.S[i], p); if (F.rev[i]) u = 1 - u;
    if (u > 0.98) { cp.push_back(an[k]); continue; }                  // (the tip: the curve's own)
    Anchor& a = sc.addAnchor(p); a.smooth = sc.anchor(an[k])->smooth; cp.push_back(a.id);
    L.moving.push_back(a.id); L.base.push_back(p); L.nrm.push_back(F.N(i, u)); L.u.push_back(u);
  }
  Item& c = net.makeCurve(parent); c.name = name; c.anchors = cp; L.id = c.id;
  return L;
}
void dropItem(Scene& sc, const CurveCache& cc, int id) { std::vector<int> si, sa; Network net(sc, cc, si, sa); net.deleteItem(id); }
// the back: the copies and, between them, segs − 1 curves in the middle at even shares of the width (each laid between
// the two lock curves round it), walls along them in a row — the copies' walls taking the lock's walls' settings
void buildBack(Scene& sc, const CurveCache& cc, Built& B, const Frame& F, int segs) {
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  const int parent = sc.item(B.orig[0])->parent; const size_t n = F.n;
  std::vector<double> T(n, 0); for (size_t i = 1; i < n; i++) T[i] = T[i - 1] + F.P(i, 0.5).distanceTo(F.P(i - 1, 0.5));
  for (size_t i = 0; i < n; i++) { T[i] /= std::max(1e-12, T[n - 1]); B.copy[i].t = T[i]; }
  for (int k = 1; k < segs; k++) {
    const double t = double(k) / segs; size_t g = 0; while (g + 2 < n && T[g + 1] < t) g++;
    { bool taken = false; for (size_t i = 0; i < n; i++) if (std::abs(T[i] - t) < 0.25 / segs) taken = true; if (taken) continue; }   // (a copy there already: none on top of it)
    const double w = std::clamp((t - T[g]) / std::max(1e-12, T[g + 1] - T[g]), 0.0, 1.0);
    const Item* o = sc.item(B.orig[g]); const Item* o2 = sc.item(B.orig[g + 1]); Layer L; L.t = t; std::vector<int> cp;
    for (size_t q = 0; q < o->anchors.size(); q++) {
      const int a0 = o->anchors[q]; double u = uOf(*F.S[g], sc.anchor(a0)->pos); if (F.rev[g]) u = 1 - u;
      const Vec3 p = F.P(g, u).lerp(F.P(g + 1, u), w);
      if (u > 0.98) {                                                 // (the tip: the lock's own when its curves meet there)
        int tipB = -1; for (int e : {o2->anchors.front(), o2->anchors.back()}) if (e == a0) tipB = e;
        cp.push_back(tipB >= 0 ? a0 : sc.addAnchor(p).id); continue; }
      Anchor& a = sc.addAnchor(p); a.smooth = sc.anchor(a0)->smooth; cp.push_back(a.id);
      Vec3 nn = F.N(g, u) * (1 - w) + F.N(g + 1, u) * w; nn = nn.lengthSq() > 1e-18 ? nn.normalized() : F.N(g, u);
      L.moving.push_back(a.id); L.base.push_back(p); L.nrm.push_back(nn); L.u.push_back(u);
    }
    Item& c = net.makeCurve(parent); c.name = o->name + QString(" mid%1").arg(k); c.anchors = cp; L.id = c.id; B.mids.push_back(L);
  }
  std::vector<const Layer*> row; for (auto& L : B.copy) row.push_back(&L); for (auto& L : B.mids) row.push_back(&L);
  std::stable_sort(row.begin(), row.end(), [](const Layer* a, const Layer* b) { return a->t < b->t; });
  for (size_t k = 0; k + 1 < row.size(); k++) {
    QJsonObject p;                                                    // (two copies side by side: their lock wall's settings)
    for (size_t i = 0; i + 1 < n; i++) if (row[k]->id == B.copy[i].id && row[k + 1]->id == B.copy[i + 1].id)
      for (auto& [id, w] : sc.items) if (w.type == ItemType::Surface && ((w.a == B.orig[i] && w.b == B.orig[i + 1]) || (w.b == B.orig[i] && w.a == B.orig[i + 1]))) { p = w.raw.value("p").toObject(); break; }
    const int w = edit::makeSurface(sc, row[k]->id, row[k + 1]->id); if (w >= 0) { if (!p.isEmpty()) sc.item(w)->raw.insert("p", p); B.backWalls.push_back(w); } }
  B.segs = segs;
}
}  // namespace

Built create(Scene& sc, const CurveCache& cc, const std::vector<int>& strip, const Vec3& headCenter, int segs) {
  Built B; auto F = frameOf(cc, strip, headCenter); if (!F) return B;
  { double w = 0; int k = 0; for (int q = 0; q <= 10; q++) { w += F->P(0, q / 10.0).distanceTo(F->P(F->n - 1, q / 10.0)); k++; } B.width = std::max(1e-6, w / k); }
  const int parent = sc.item(strip[0])->parent;
  std::vector<int> si, sa; Network net(sc, cc, si, sa);
  B.orig = strip;
  for (size_t i = 0; i < F->n; i++) { Layer L = makeLayer(sc, net, *F, i, strip[i], parent, sc.item(strip[i])->name + " ex"); if (L.id < 0) return Built{}; B.copy.push_back(L); }
  for (size_t i : {size_t(0), F->n - 1}) { const int w = edit::makeSurface(sc, strip[i], B.copy[i].id); if (w >= 0) B.sideWalls.push_back(w); }   // (the sides: a wall each)
  buildBack(sc, cc, B, *F, std::max(1, segs));
  sc.reindex();
  return B;
}

bool setSegs(Scene& sc, const CurveCache& cc, Built& B, int segs) {
  segs = std::max(1, segs); if (segs == B.segs) return false;
  // (the frame from the lock's curves as they are — they haven't moved)
  Vec3 head; { const Layer& L = B.copy[0]; if (!L.moving.empty()) head = L.base[0] - L.nrm[0] * (B.width * 10); }
  auto F = frameOf(cc, B.orig, head); if (!F) return false;
  for (int w : B.backWalls) dropItem(sc, cc, w); for (auto& L : B.mids) dropItem(sc, cc, L.id);
  B.backWalls.clear(); B.mids.clear();
  { std::vector<int> si, sa; Network(sc, cc, si, sa).pruneAnchors(); }
  if (!B.copy[0].nrm.empty() && F->N(0, B.copy[0].u[0]).dot(B.copy[0].nrm[0]) < 0) F->sgn = -F->sgn;   // (the outward side kept as the copies have it)
  buildBack(sc, cc, B, *F, segs);
  sc.reindex();
  return true;
}

void apply(Scene& sc, const Built& B, const Params& P) {
  const double tl = std::clamp(P.tip, 0.02, 1.0), pinch = std::clamp(P.root, 0.0, 1.0);
  auto thick = [&](double u) { return (1 - pinch * (1 - ease(u / 0.35))) * ease((1 - u) / tl); };   // (narrower at the root as asked, to nothing at the tip)
  // (segments: the back stands out in its middle — half the thickness more there; one middle curve — a pentagon across)
  auto place = [&](const Layer& L) { const double share = 1 + (B.segs > 1 ? 0.5 * std::sin(L.t * 3.141592653589793) : 0.0);   // (one segment: a flat back)
    for (size_t k = 0; k < L.moving.size(); k++) if (Anchor* a = sc.anchor(L.moving[k])) a->pos = L.base[k] + L.nrm[k] * (P.d * B.width * thick(L.u[k]) * share); };
  for (auto& L : B.copy) place(L);
  for (auto& L : B.mids) place(L);
}

}  // namespace hm::lockex
