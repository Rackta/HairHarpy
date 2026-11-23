// The Topology room's Scissors: split ends on a mesh. The selected chain of loops (rings of polygons round the lock, one
// after another) is cut lengthwise into two branches: a seam (two on a tube — on its wide sides, one on a flat strip — down
// its middle) runs from the loop where the selection starts to its other end; the vertices on it are doubled, each half of
// a tube closed by a wall along the seams, and the halves pushed apart — at the selection's open end (the tip) by the whole
// radius, back at the loop where it starts not at all (the branches grow out of the lock smoothly). A chain with the lock
// on both of its ends (a stretch in the middle) opens as an eye: widest in its middle, joined at both ends.
#include <QJsonArray>
#include <QJsonObject>
#include <functional>
#include "scene/MeshMods.h"
#include "scene/Topo.h"
#include "scene/TopoEdit.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace hm {

using tedit::MeshEd; using tedit::ek; using tedit::EK;

#define TOPO_NO(msg) do { if (!preview) { ui::toast(msg); scisFail(msg); } return "!" + QString(msg); } while (0)   // (the preview: silent)

namespace {
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
// the main direction of a set of points (power iteration on their covariance)
Vec3 majorAxis(const std::vector<Vec3>& P, const Vec3& c) {
  double C[3][3] = {};
  for (const Vec3& p : P) { const double d[3] = {p.x - c.x, p.y - c.y, p.z - c.z}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) C[i][j] += d[i] * d[j]; }
  double v[3] = {1, 0.7, 0.3};
  for (int it = 0; it < 60; it++) { double w[3] = {}; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) w[i] += C[i][j] * v[j];
    const double n = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]); if (n < 1e-30) break; for (int i = 0; i < 3; i++) v[i] = w[i] / n; }
  return Vec3(v[0], v[1], v[2]);
}
double smooth01(double t) { t = std::clamp(t, 0.0, 1.0); return t * t * (3 - 2 * t); }
}  // namespace

QString Viewport::tScissors(bool preview) {
  std::function<void(const QString&)> scisFail = [](const QString&) {};
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it || it->type != ItemType::IMesh || !baseOf(*it)) TOPO_NO(tr("Pick a mesh and select the chain of loops to split"));
  std::vector<int> fs = topoMode == "object" ? std::vector<int>{} : tSelFaces(m);
  const PolyMesh& B = *baseOf(*it); auto T = topo::ttopo(baseSp(*it));
  // (a refusal while a session is recorded: the mesh and the selection written into it — once per selection — so the very
  // case can be run again; meshes of up to 20 000 vertices)
  scisFail = [this, &B, m](const QString& why) {
    static int lastVer = -1, lastM = -1; if (!ui::devHook() || B.pos.size() > 20000 || (lastVer == tsel_.ver && lastM == m)) return; lastVer = tsel_.ver; lastM = m;
    QJsonArray P, F, se, sv, sf; for (const Vec3& p : B.pos) { P.append(p.x); P.append(p.y); P.append(p.z); }
    for (int f = 0; f < B.faceCount(); f++) { QJsonArray a; for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) a.append(B.faceVerts[size_t(k)]); F.append(a); }
    auto T2 = topo::ttopo(baseSp(*scene_->item(m)));
    for (int e : tsel_.e) if (e >= 0 && e < T2->ne) se.append(QJsonArray{T2->ea[size_t(e)], T2->eb[size_t(e)]});
    for (int v : tsel_.v) sv.append(v); for (int f : tsel_.f) sf.append(f);
    ui::devLog("topodump", QJsonObject{{"tool", "tscissors"}, {"why", why}, {"mode", topoMode}, {"pos", P}, {"faces", F}, {"selEdges", se}, {"selVerts", sv}, {"selFaces", sf}, {"spread", ttools.scisPx}});
  };
  // a cut line — vertices / edges picked along the lock, from where the split starts: the loops it crosses are the ones
  // split, the line itself the first seam
  std::vector<int> line, line2;   // (line2: the second seam, when the selection is the whole cut — down one side, over the tip, up the other)
  if (topoMode != "object" && tKey() != 'f' && tsel_.id == m) {
    const auto vl = tSelVerts(m); const std::set<int> vs(vl.begin(), vl.end());
    std::map<int, std::vector<int>> la; std::vector<int> pe;
    for (int e = 0; e < T->ne; e++) if (vs.count(T->ea[size_t(e)]) && vs.count(T->eb[size_t(e)])) { la[T->ea[size_t(e)]].push_back(T->eb[size_t(e)]); la[T->eb[size_t(e)]].push_back(T->ea[size_t(e)]); pe.push_back(e); }
    bool chain = vs.size() >= 2 && pe.size() + 1 == vs.size(); int endV = -1, ends = 0;
    for (int v : vs) { const size_t d = la[v].size(); if (d == 0 || d > 2) chain = false; if (d == 1) { ends++; if (endV < 0) endV = v; } }
    if (chain && ends == 2) {
      line = {endV}; for (int prev = -1;;) { const int cur = line.back(); int nx = -1; for (int w : la[cur]) if (w != prev) { nx = w; break; } if (nx < 0) break; prev = cur; line.push_back(nx); }
      if (line.size() == vs.size()) {
        // (an edge at either end that doesn't run along the lock — a loop pick gone on over the tip's cap, round the root —
        // left off: the loop of polygons across an edge along the lock is as long as the others', across such an edge it isn't)
        auto loopOf = [&](int a, int b) -> std::vector<int> { const int e = T->edgeOf(a, b); if (e < 0 || T->ef0[size_t(e)] < 0) return {}; return topo::faceLoop(*T, T->ef0[size_t(e)], e); };
        std::vector<size_t> sz; std::map<size_t, int> cnt; for (size_t k = 0; k + 1 < line.size(); k++) { sz.push_back(loopOf(line[k], line[k + 1]).size()); cnt[sz.back()]++; }
        size_t mode = 0; int mc = 0; for (auto [s, c] : cnt) if (c > mc || (c == mc && s < mode)) { mode = s; mc = c; }
        size_t i0 = 0, i1 = sz.size(); while (i0 < i1 && sz[i0] != mode) i0++; while (i1 > i0 && sz[i1 - 1] != mode) i1--;
        // (the whole cut picked: a line down one side of the lock, edges over its tip — across the cap — and a line back up the
        // other side: two stretches along the lock with a stretch across between them. Both are seams, each from its top to
        // the tip — as long as the shorter one)
        size_t a1 = i0; while (a1 < i1 && sz[a1] == mode) a1++;       // (the first stretch: edges i0 … a1)
        size_t b0 = a1; while (b0 < i1 && sz[b0] != mode) b0++;       // (the stretch across: a1 … b0)
        bool two = b0 > a1 && b0 < i1; for (size_t k = b0; two && k < i1; k++) if (sz[k] != mode) two = false;
        if (two) {
          std::vector<int> A(line.begin() + long(i0), line.begin() + long(a1) + 1), Bv(line.begin() + long(b0), line.begin() + long(i1) + 1);
          std::reverse(Bv.begin(), Bv.end());                           // (both: top → tip)
          const size_t n = std::min(A.size(), Bv.size());
          line.assign(A.end() - long(n), A.end()); line2.assign(Bv.end() - long(n), Bv.end());
        } else if (i1 > i0) line = std::vector<int>(line.begin() + long(i0), line.begin() + long(i1) + 1); else line.clear();
        std::set<int> F; for (const auto* L : {&line, &line2}) for (size_t k = 0; k + 1 < L->size(); k++) for (int f : loopOf((*L)[k], (*L)[k + 1])) F.insert(f);
        if (!line.empty()) fs.assign(F.begin(), F.end());
      }
      else line.clear();
    }
  }
  // (a cut line running to the lock's end: the polygons closing it beyond the last loop — a cap, a pointed tip — are split
  // with it: a piece of the rest no bigger than a loop, with no open edge, that the line's end touches. A cap made of
  // several polygons with no vertex of their own is joined into one polygon below, then cut between the seams' ends)
  std::vector<int> capFaces;
  if (!line.empty()) {
    const std::set<int> inF(fs.begin(), fs.end()); std::set<int> rv; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) rv.insert(T->fv[size_t(k)]);
    size_t loopN = 0; for (int k = 0; k + 1 < int(line.size()); k++) { const int e = T->edgeOf(line[size_t(k)], line[size_t(k) + 1]); if (e >= 0 && T->ef0[size_t(e)] >= 0) loopN = std::max(loopN, topo::faceLoop(*T, T->ef0[size_t(e)], e).size()); }
    std::vector<char> seen(size_t(T->nf), 0); std::vector<int> add;
    for (int f0 = 0; f0 < T->nf; f0++) {
      if (inF.count(f0) || seen[size_t(f0)]) continue;
      std::vector<int> comp{f0}; seen[size_t(f0)] = 1; bool rim = false;
      for (size_t i = 0; i < comp.size(); i++) { const int f = comp[i];
        for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int e = T->ce[size_t(k)], g = T->ef0[size_t(e)] == f ? T->ef1[size_t(e)] : T->ef0[size_t(e)];
          if (g < 0) { rim = true; continue; } if (!inF.count(g) && !seen[size_t(g)]) { seen[size_t(g)] = 1; comp.push_back(g); } } }
      if (rim || comp.size() > std::max<size_t>(loopN, 1)) continue;
      bool touches = false, own = false;
      for (int f : comp) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int v = T->fv[size_t(k)]; if (v == line.front() || v == line.back()) touches = true; if (!rv.count(v)) own = true; }
      if (!touches) continue;
      add.insert(add.end(), comp.begin(), comp.end()); if (!own && comp.size() > 1) capFaces = comp;
    }
    fs.insert(fs.end(), add.begin(), add.end());
  }
  if (fs.empty()) TOPO_NO(tr("Select the chain of loops to split — or the cut line along the lock (vertices / edges from where the split starts)"));
  MeshEd E = MeshEd::from(B);
  std::vector<char> inR(E.f.size(), 0); for (int f : fs) if (f >= 0 && size_t(f) < inR.size()) inR[size_t(f)] = 1;

  // the region's edges; those it shares with the polygons left out — where the branches grow from the lock
  std::set<int> ER; for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) ER.insert(T->ce[size_t(k)]);
  std::vector<EK> joinE; std::map<int, std::vector<int>> adj;
  for (int e : ER) {
    const int a = T->ea[size_t(e)], b = T->eb[size_t(e)], f0 = T->ef0[size_t(e)], f1 = T->ef1[size_t(e)];
    adj[a].push_back(b); adj[b].push_back(a);
    if (f1 >= 0 && (inR[size_t(f0)] != 0) != (inR[size_t(f1)] != 0)) joinE.push_back(ek(a, b));
  }
  const auto rings = tedit::edgeChains(joinE);
  if (rings.empty()) TOPO_NO(tr("The selection is the whole lock — select the loops from where the split starts (to the tip, or a stretch in the middle)"));
  if (rings.size() > 2) TOPO_NO(tr("Select one chain of loops: from where the split starts to the tip (or a stretch in the middle)"));
  const tedit::Chain& R0 = rings[0];
  std::set<int> shared(R0.v.begin(), R0.v.end()), endSet;   // (the rings that stay whole: the branches meet there)
  if (rings.size() == 2) { endSet.insert(rings[1].v.begin(), rings[1].v.end()); shared.insert(rings[1].v.begin(), rings[1].v.end()); }
  const bool eye = rings.size() == 2;

  // each vertex's step from the starting ring (its loop's number)
  std::map<int, int> stage; { std::vector<int> q; for (int v : R0.v) { stage[v] = 0; q.push_back(v); }
    for (size_t i = 0; i < q.size(); i++) for (int w : adj[q[i]]) if (!stage.count(w)) { stage[w] = stage[q[i]] + 1; q.push_back(w); } }

  // where the seams start on the first ring: across the lock's width (its widest direction cut in two)
  std::vector<Vec3> P; for (int v : R0.v) P.push_back(E.pos[size_t(v)]);
  Vec3 c0; for (const Vec3& p : P) c0 += p; c0 = c0 * (1.0 / double(P.size()));
  const Vec3 ax = majorAxis(P, c0);
  double lo = 1e300, hi = -1e300; for (const Vec3& p : P) { const double d = (p - c0).dot(ax); lo = std::min(lo, d); hi = std::max(hi, d); }
  const double width = hi - lo; if (!(width > 0)) TOPO_NO(tr("The first loop has no width"));
  const bool tube = R0.closed; const int M = int(R0.v.size());
  std::vector<int> starts;
  if (tube) {
    if (M < 4) TOPO_NO(tr("Too few vertices round the loop to split it (four at least)"));
    int best = 0; double bv = 1e300;
    for (int i = 0; i < M; i++) { const double s = std::abs((P[size_t(i)] - c0).dot(ax)) + std::abs((P[size_t((i + M / 2) % M)] - c0).dot(ax)); if (s < bv) { bv = s; best = i; } }
    starts = {R0.v[size_t(best)], R0.v[size_t((best + M / 2) % M)]};
  } else {
    if (M < 3) TOPO_NO(tr("The strip is one polygon wide — nothing to split it along"));
    std::vector<double> L{0}; for (int i = 1; i < M; i++) L.push_back(L.back() + (P[size_t(i)] - P[size_t(i - 1)]).length());
    int best = 1; for (int i = 1; i < M - 1; i++) if (std::abs(L[size_t(i)] - L.back() / 2) < std::abs(L[size_t(best)] - L.back() / 2)) best = i;
    starts = {R0.v[size_t(best)]};
  }

  // (a cut line: the first seam starts where it does — on a tube the second one on the opposite side of the loop)
  if (!line.empty()) {
    const std::set<int> r0(R0.v.begin(), R0.v.end());
    if (!r0.count(line.front())) std::reverse(line.begin(), line.end());
    if (!r0.count(line.front())) TOPO_NO(tr("The cut line should start on the loop where the split begins"));
    const int i0 = int(std::find(R0.v.begin(), R0.v.end(), line.front()) - R0.v.begin());
    starts = tube ? std::vector<int>{line.front(), R0.v[size_t((i0 + M / 2) % M)]} : std::vector<int>{line.front()};
    if (tube && !line2.empty()) {                                    // (the second seam picked too: it starts where it does)
      if (!r0.count(line2.front())) std::reverse(line2.begin(), line2.end());
      if (!r0.count(line2.front()) || line2.front() == line.front()) TOPO_NO(tr("The two cut lines should start on the same loop, on its two sides"));
      starts[1] = line2.front();
    }
  }
  // the seams: from each start, loop by loop along the lock (the edge to the next loop's vertex)
  std::vector<std::vector<int>> seams;
  for (int s : starts) {
    std::vector<int> path{s};
    if (!line.empty() && s == line.front()) {                       // (the cut line itself, when it runs loop by loop — then on, as a seam does, to a pointed tip)
      bool ok = true; for (size_t i = 0; i < line.size(); i++) { auto f = stage.find(line[i]); if (f == stage.end() || f->second != int(i)) ok = false; }
      if (ok) path = line; }
    if (!line2.empty() && s == line2.front()) {
      bool ok = true; for (size_t i = 0; i < line2.size(); i++) { auto f = stage.find(line2[i]); if (f == stage.end() || f->second != int(i)) ok = false; }
      if (ok) path = line2; }
    for (;;) {
      const int cur = path.back(); if (path.size() > 1 && endSet.count(cur)) break;
      int nx = -1; double bestCos = -1e300;   // (the first step scores by −length: any value)
      const Vec3 prevDir = path.size() > 1 ? (E.pos[size_t(cur)] - E.pos[size_t(path[path.size() - 2])]).normalized() : Vec3();
      for (int w : adj[cur]) if (stage.count(w) && stage[w] == stage[cur] + 1) {
        const Vec3 d = (E.pos[size_t(w)] - E.pos[size_t(cur)]); const double cs = path.size() > 1 ? d.normalized().dot(prevDir) : -d.length();
        if (cs > bestCos) { bestCos = cs; nx = w; } }
      if (nx < 0) break; path.push_back(nx);
    }
    if (path.size() < 2) TOPO_NO(tr("The selection doesn't run along the lock from its first loop"));
    if (eye && !endSet.count(path.back())) TOPO_NO(tr("The seam didn't reach the chain's other end — select whole loops"));
    seams.push_back(path);
  }
  if (tube) { std::set<int> a(seams[0].begin(), seams[0].end());
    for (size_t i = 0; i + 1 < seams[1].size(); i++) if (a.count(seams[1][i])) TOPO_NO(tr("The two seams met before the tip — the loops are too uneven to split")); }

  // the cut: the seams' edges; at a capped tip the cap polygon cut between the seams' ends
  std::set<EK> cut; for (auto& s : seams) for (size_t i = 0; i + 1 < s.size(); i++) cut.insert(ek(s[i], s[i + 1]));
  int capF[2] = {-1, -1};   // (the cap's two halves, when the tip had a cap polygon)
  if (tube && capFaces.size() > 1) {                                // (a cap of several polygons: one polygon round its rim first)
    std::set<int> cf(capFaces.begin(), capFaces.end()); std::map<int, int> nextOf; std::set<std::pair<int, int>> dir; int g = E.f[size_t(capFaces[0])].g;
    for (int f : capFaces) { const auto& V = E.f[size_t(f)].v; for (size_t k = 0; k < V.size(); k++) dir.insert({V[k], V[(k + 1) % V.size()]}); }
    for (auto [a, b] : dir) if (!dir.count({b, a})) nextOf[a] = b;
    std::vector<int> rimV; if (!nextOf.empty()) { int v = nextOf.begin()->first; for (size_t guard = 0; guard <= nextOf.size(); guard++) { rimV.push_back(v); auto f = nextOf.find(v); if (f == nextOf.end()) { rimV.clear(); break; } v = f->second; if (v == rimV[0]) break; } }
    if (rimV.size() >= 3 && rimV.size() == nextOf.size()) {
      std::vector<char> del(E.f.size(), 0); for (int f : capFaces) del[size_t(f)] = 1;
      std::vector<char> keepR; for (size_t f = 0; f < E.f.size(); f++) if (!del[f]) keepR.push_back(inR[f]);
      E.removeFaces(del); inR = keepR; E.addFace(rimV, g); inR.push_back(1);
    }
  }
  if (tube) {
    const int e1 = seams[0].back(), e2 = seams[1].back();
    if (e1 != e2 && !shared.count(e1) && !shared.count(e2)) for (size_t f = 0; f < E.f.size(); f++) {
      if (!inR[f] || E.corner(int(f), e1) < 0 || E.corner(int(f), e2) < 0) continue;
      const int nf = E.splitFace(int(f), e1, e2); if (nf >= 0) { inR.push_back(1); capF[0] = int(f); capF[1] = nf; }
      cut.insert(ek(e1, e2)); break; }
  }

  // the two halves: the region's polygons joined across every edge but the cut
  std::map<EK, std::vector<int>> ef; for (size_t f = 0; f < E.f.size(); f++) if (inR[f]) { const auto& V = E.f[f].v; for (size_t k = 0; k < V.size(); k++) ef[ek(V[k], V[(k + 1) % V.size()])].push_back(int(f)); }
  std::vector<int> side(E.f.size(), -1); int comps = 0;
  for (size_t f0 = 0; f0 < E.f.size(); f0++) { if (!inR[f0] || side[f0] >= 0) continue;
    std::vector<int> q{int(f0)}; side[f0] = comps;
    for (size_t i = 0; i < q.size(); i++) { const auto& V = E.f[size_t(q[i])].v;
      for (size_t k = 0; k < V.size(); k++) { const EK e = ek(V[k], V[(k + 1) % V.size()]); if (cut.count(e)) continue;
        for (int g : ef[e]) if (side[size_t(g)] < 0) { side[size_t(g)] = comps; q.push_back(g); } } }
    comps++; }
  if (comps != 2) TOPO_NO(tr("The loops didn't come apart into two halves (%1 pieces) — select whole loops of one lock").arg(comps));

  // the seams' vertices doubled for the second half (the rings where the branches meet stay whole)
  std::map<int, int> dup;
  for (auto& s : seams) for (int v : s) if (!shared.count(v) && !dup.count(v)) dup[v] = E.addVert(E.pos[size_t(v)]);
  for (size_t f = 0; f < E.f.size(); f++) if (side[f] == 1) for (int& v : E.f[f].v) if (dup.count(v)) v = dup[v];
  auto on = [&](int v, int sd) { return sd == 1 && dup.count(v) ? dup[v] : v; };

  // a tube's halves closed: a wall between the two seams, loop by loop. A cap half with an odd number of corners can't be
  // all quads — then the walls get a middle line (a vertex in the middle of each wall's chord) and the cap half one more
  // corner on its chord: the cap is laid as quads from that middle point
  int walls = 0; std::set<int> midV;
  if (tube) {
    int g = 0; for (int f : fs) if (size_t(f) < E.f.size()) { g = E.f[size_t(f)].g; break; }   // (a cap joined: the indices moved — any polygon of the lock will do)
    const size_t n = std::min(seams[0].size(), seams[1].size());
    // (a pointed tip — the seams meet in its apex: each branch's tip laid as quads round it, as the cap)
    const int apex = seams[0].back() == seams[1].back() && !shared.count(seams[0].back()) ? seams[0].back() : -1;
    size_t half = 0; { std::set<int> h; for (size_t f = 0; f < E.f.size(); f++) if (side[f] == 0) for (int v : E.f[f].v) if (std::find(R0.v.begin(), R0.v.end(), v) != R0.v.end()) h.insert(v); half = h.size(); }
    const bool mid = (capF[0] >= 0 || apex >= 0) && seams[0].size() == seams[1].size() && half % 2 == 1;
    std::vector<int> mv[2]; mv[0].assign(n, -1); mv[1].assign(n, -1);
    if (mid) for (size_t k = 0; k < n; k++) {
      const int a = seams[0][k], b = seams[1][k];
      if (a == b) { mv[0][k] = on(a, 0); mv[1][k] = on(a, 1); continue; }
      const bool sh = shared.count(a) && shared.count(b);   // (where the branches meet: one middle point for both)
      for (int sd = 0; sd < 2; sd++) {
        if (sd == 1 && sh) { mv[1][k] = mv[0][k]; continue; }
        const int v = E.addVert((E.pos[size_t(on(a, sd))] + E.pos[size_t(on(b, sd))]) * 0.5); mv[size_t(sd)][k] = v; midV.insert(v); stage[v] = stage[a]; }
    }
    if (mid) for (int sd = 0; sd < 2; sd++) E.insertOnEdge(on(seams[0][n - 1], sd), on(seams[1][n - 1], sd), {mv[size_t(sd)][n - 1]}, {0.5});
    auto wall = [&](std::vector<int> q, int sd) { std::vector<int> w; for (int v : q) if (std::find(w.begin(), w.end(), v) == w.end()) w.push_back(v);
      if (w.size() < 3) return; E.addFace(w, g); side.push_back(sd); walls++; };
    for (int sd = 0; sd < 2; sd++) for (size_t k = 0; k + 1 < n; k++) {
      const int a0 = on(seams[0][k], sd), a1 = on(seams[0][k + 1], sd), b0 = on(seams[1][k], sd), b1 = on(seams[1][k + 1], sd);
      if (!mid) { wall({a0, a1, b1, b0}, sd); continue; }
      const int m0 = mv[size_t(sd)][k], m1 = mv[size_t(sd)][k + 1];
      wall({a0, a1, m1, m0}, sd); wall({m0, m1, b1, b0}, sd);
    }
    // the pointed tip: each branch's triangles round its apex taken out, the ring round it closed by quads (apex, r0, r1, r2)…
    if (apex >= 0) for (int sd = 0; sd < 2; sd++) {
      const int A = on(apex, sd); std::vector<char> del(E.f.size(), 0); std::vector<EK> ring; int g2 = g;
      for (size_t f = 0; f < E.f.size(); f++) if (side[f] == sd && E.corner(int(f), A) >= 0) { del[f] = 1; g2 = E.f[f].g;
        const auto& V = E.f[f].v; for (size_t k = 0; k < V.size(); k++) { const int a = V[k], b = V[(k + 1) % V.size()]; if (a != A && b != A) ring.push_back(ek(a, b)); } }
      const auto ch = tedit::edgeChains(ring); if (ch.size() != 1 || !ch[0].closed || ch[0].v.size() < 4) continue;   // (not a clean fan: left as it is)
      std::vector<int> keepSide; for (size_t f = 0; f < E.f.size(); f++) if (!del[f]) keepSide.push_back(side[f]);
      E.removeFaces(del); side = keepSide; if (capF[0] >= 0) capF[0] = capF[1] = -1;
      const auto& r = ch[0].v; const size_t L = r.size();
      for (size_t i = 0; i + 2 <= L - (L % 2); i += 2) { E.addFace({A, r[i], r[i + 1], r[(i + 2) % L]}, g2); side.push_back(sd); }
      if (L % 2) { E.addFace({A, r[L - 1], r[0]}, g2); side.push_back(sd); }   // (an odd ring: one triangle can't be helped)
    }
    // the cap halves as quads: cut off one quad at a time, fanning from the chord's middle (or its first corner)
    for (int sd = 0; sd < 2; sd++) { int fi = capF[sd]; if (fi < 0) continue;
      const int from = mid ? mv[size_t(sd)][n - 1] : on(seams[0][n - 1], sd);
      for (int guard = 0; guard < 64 && E.f[size_t(fi)].v.size() > 4; guard++) {
        const auto& V = E.f[size_t(fi)].v; const int c = E.corner(fi, from); if (c < 0) break;
        const int nf = E.splitFace(fi, from, V[size_t((c + 3) % int(V.size()))]); if (nf < 0) break;
        side.push_back(sd); fi = nf; }
    }
  }

  // pushed apart: per loop, from one half's middle to the other's
  int K = 0; for (auto& [v, k] : stage) K = std::max(K, k);
  std::vector<double> L(size_t(K) + 1, -1); { const auto& s = seams[0]; double acc = 0; for (size_t i = 0; i < s.size(); i++) { if (i) acc += (E.pos[size_t(s[i])] - E.pos[size_t(s[i - 1])]).length(); L[size_t(stage[s[i]])] = acc; } }
  { double last = 0; for (double& x : L) { if (x < 0) x = last; last = x; } }
  const double Ltot = std::max(1e-12, L.back());
  std::vector<Vec3> sum[2]; std::vector<int> cnt[2];
  for (int sd = 0; sd < 2; sd++) { sum[sd].assign(size_t(K) + 1, Vec3()); cnt[sd].assign(size_t(K) + 1, 0); }
  std::set<int> moved[2];
  for (size_t f = 0; f < side.size() && f < E.f.size(); f++) if (side[f] >= 0) for (int v : E.f[f].v) moved[side[f]].insert(v);
  auto orig = [&](int v) { for (auto& [a, b] : dup) if (b == v) return a; return v; };
  for (int sd = 0; sd < 2; sd++) for (int v : moved[sd]) { const int o = orig(v); if (dup.count(o) || shared.count(o) || midV.count(o) || !stage.count(o)) continue; sum[sd][size_t(stage[o])] += E.pos[size_t(v)]; cnt[sd][size_t(stage[o])]++; }
  std::vector<Vec3> dir(size_t(K) + 1); std::vector<char> has(size_t(K) + 1, 0);
  for (int k = 0; k <= K; k++) if (cnt[0][size_t(k)] && cnt[1][size_t(k)]) { const Vec3 d = sum[0][size_t(k)] * (1.0 / cnt[0][size_t(k)]) - sum[1][size_t(k)] * (1.0 / cnt[1][size_t(k)]); if (d.lengthSq() > 0) { dir[size_t(k)] = d.normalized(); has[size_t(k)] = 1; } }
  { int first = -1; for (int k = 0; k <= K; k++) if (has[size_t(k)]) { first = k; break; }
    if (first < 0) TOPO_NO(tr("Couldn't tell the two halves apart"));
    for (int k = 0; k < first; k++) dir[size_t(k)] = dir[size_t(first)];
    for (int k = first + 1; k <= K; k++) if (!has[size_t(k)]) dir[size_t(k)] = dir[size_t(k - 1)]; }
  // the spread as in the Hair room's Split ends: each branch moves scisPx screen pixels from where it was, at its widest
  // place (the tip — the middle of a stretch opened as an eye), measured where that place is seen now
  const Affine MW = meshmods::meshMatrix(*scene_, *it);
  Vec3 wideL; { int n = 0; for (auto& s : seams) { const int v = eye ? s[s.size() / 2] : s.back(); wideL += E.pos[size_t(v)]; n++; } wideL = wideL * (1.0 / std::max(1, n)); }
  const double unit = std::max(1e-12, (MW.apply(wideL + ax) - MW.apply(wideL)).length());   // (one unit of the mesh along the width — in the world)
  const double gap = 2 * ttools.scisPx * worldPerPixelAt(MW.apply(wideL)) / unit;
  // (Carve: the branches don't move apart — each half is pressed toward its own outer side, the wedge between them cut out:
  // the lock's outline stays, the tips get thinner across. Per loop: its middle, how far each half reaches out from it)
  const bool carve = ttools.scisMode == "carve";
  std::vector<Vec3> mid(size_t(K) + 1); std::vector<double> reach[2]; reach[0].assign(size_t(K) + 1, 0); reach[1].assign(size_t(K) + 1, 0);
  if (carve) {
    for (int k = 0; k <= K; k++) { const size_t q = size_t(k);
      mid[q] = cnt[0][q] && cnt[1][q] ? (sum[0][q] * (1.0 / cnt[0][q]) + sum[1][q] * (1.0 / cnt[1][q])) * 0.5 : cnt[0][q] ? sum[0][q] * (1.0 / cnt[0][q]) : cnt[1][q] ? sum[1][q] * (1.0 / cnt[1][q]) : Vec3(); }
    for (int sd = 0; sd < 2; sd++) for (int v : moved[sd]) { const int o = orig(v); if (shared.count(o) || !stage.count(o)) continue; const size_t q = size_t(stage[o]);
      reach[sd][q] = std::max(reach[sd][q], (sd == 0 ? 1.0 : -1.0) * (E.pos[size_t(v)] - mid[q]).dot(dir[q])); }
  }
  for (int sd = 0; sd < 2; sd++) for (int v : moved[sd]) {
    const int o = orig(v); if (shared.count(o) || !stage.count(o)) continue;
    const int k = stage[o]; const double t = L[size_t(k)] / Ltot;
    // (the profile of the parting along the cut: 0 where it starts, 1 at the tip — an S curve, a straight line, an arc bulging
    // out (parting fast, then running level) or in (slow, then fast); a stretch in the middle: the same there and back)
    const QString& pf = ttools.scisProfile; const double u = eye ? 1 - std::abs(2 * t - 1) : t;
    const double fct = pf == "line" ? u : pf == "arcout" ? std::sin(u * 1.5707963267948966) : pf == "arcin" ? 1 - std::cos(u * 1.5707963267948966) : smooth01(u);
    if (carve) {                                                     // (pressed toward its outer side: the seam by the spread, the outline not at all)
      const double sg = sd == 0 ? 1.0 : -1.0, R = reach[sd][size_t(k)]; if (!(R > 1e-12)) continue;
      const double x = sg * (E.pos[size_t(v)] - mid[size_t(k)]).dot(dir[size_t(k)]), s = std::min(0.5 * gap * fct / R, 0.9);
      E.pos[size_t(v)] += dir[size_t(k)] * (sg * s * std::max(0.0, R - x));
      continue;
    }
    E.pos[size_t(v)] += dir[size_t(k)] * ((sd == 0 ? 0.5 : -0.5) * gap * fct);
  }

  if (preview) {                                                     // (the preview: both branches' edges as they would be — in the world)
    scisViz_.clear(); std::set<EK> seen;
    for (size_t f = 0; f < side.size() && f < E.f.size(); f++) if (side[f] >= 0) { const auto& V = E.f[f].v;
      for (size_t k = 0; k < V.size(); k++) { const int a = V[k], b = V[(k + 1) % V.size()]; if (!seen.insert(ek(a, b)).second) continue;
        scisViz_.push_back({MW.apply(E.pos[size_t(a)]), MW.apply(E.pos[size_t(b)])}); } }
    return QString();
  }
  auto R = std::make_shared<PolyMesh>(E.to(B));
  topoWrite(m, R);
  std::vector<int> picked; for (size_t f = 0; f < side.size() && f < E.f.size(); f++) if (side[f] >= 0) picked.push_back(int(f));
  const int ver = tsel_.ver + 1; tsel_ = {}; tsel_.id = m; tsel_.B = R.get(); tsel_.ver = ver;
  if (topoMode != "poly") { topoMode = "poly"; updateGizmo(); } tsel_.f.insert(picked.begin(), picked.end());   // (both branches stay picked)
  thov_ = {}; tt_.hov = {}; ttTouch(); updateGizmo(); tt_.lastAfter = R; tt_.lastM = m;
  const QString msg = tr("Split into two branches: %1 loop%2, spread %3 px").arg(K).arg(K == 1 ? "" : "s").arg(int(std::lround(ttools.scisPx)))
                      + (carve ? tr(" — cut out inside, the outline kept") : QString())
                      + (eye ? tr(" — joined at both ends") : QString()) + (walls ? tr(" · %1 wall polygons close the halves").arg(walls) : QString());
  scene_->revision++; commit(true); emit topoState(); ui::toast(msg); return msg;
}

// the preview of the Scissors: made again when the selection, the mesh, the spread or the zoom change
void Viewport::scisPreview() {
  if (room() != "topo" || tool_ != "tscissors") { if (!scisViz_.empty()) { scisViz_.clear(); overlayRev_++; } scisKeyM_ = -2; return; }
  const int m = topoMesh(); const bool any = m >= 0 && topoMode != "object" && tsel_.id == m && (!tsel_.f.empty() || !tsel_.e.empty() || !tsel_.v.empty());
  const std::vector<int> key{m, tsel_.ver, int(tsel_.f.size()), int(tsel_.e.size()), int(tsel_.v.size()), int(tKey())};   // (the selection as it is now)
  const double w = worldPerPixelAt(cam_.pivot);
  if (key == scisKeyF_ && ttools.scisPx == scisKeyPx_ && ttools.scisProfile + ttools.scisMode == scisKeyProf_ && std::abs(w - scisKeyW_) <= 1e-3 * std::abs(scisKeyW_) && scene_->revision == scisKeyRev_) return;
  scisKeyM_ = m; scisKeyF_ = key; scisKeyPx_ = ttools.scisPx; scisKeyProf_ = ttools.scisProfile + ttools.scisMode; scisKeyW_ = w; scisKeyRev_ = scene_->revision;
  scisViz_.clear(); if (any) tScissors(true);
  overlayRev_++; requestUpdate();
}

}  // namespace hm
