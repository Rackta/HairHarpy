// The UV Editor's frame and warp (Viewport.h): both work on the UV of the selection as it was when they started (uv0) —
// the frame as one move, size and turn of its box; the warp as a grid of anchors over the box whose moves bend the UV
// smoothly between them (each point moved by the anchors' moves near it, Catmull–Rom weighted — through every anchor,
// no kinks). A drag is shown live, its release one undo step; another selection, an undo, an edit elsewhere — they end.
#include "ui/Pie.h"
#include "scene/Uv.h"
#include "view/Viewport.h"
#include <QElapsedTimer>
#include <QGuiApplication>
#include <cmath>
#include <map>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
// Catmull–Rom weights of the four anchors about t (0…1 between the middle two)
void crw(double t, double w[4]) { const double t2 = t * t, t3 = t2 * t; w[0] = (-t3 + 2 * t2 - t) / 2; w[1] = (3 * t3 - 5 * t2 + 2) / 2; w[2] = (-3 * t3 + 4 * t2 + t) / 2; w[3] = (t3 - t2) / 2; }
}  // namespace

QString Viewport::uvTfStart(char kind) {
  if (uvTf_.on && uvTf_.kind == kind) { uvTfStop(); ui::hud(kind == 'w' ? tr("Warp off") : kind == 'p' ? tr("Puppet off") : tr("Frame off"), 800); return {}; }
  const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it || !uv::has(*baseOf(*it))) { ui::toast(tr("Select a mesh with UV")); return "!"; }
  auto ks = uvMoving(); if (ks.empty()) { ui::toast(tr("Select what to transform in the UV Editor (polygons, edges or UV points — Shells: whole ones)")); return "!"; }
  UvTf t; t.on = true; t.kind = kind; t.m = m; t.ks = ks; t.uv0 = baseOf(*it)->cornerUV; t.base = baseOf(*it);
  t.box = uv::box(*baseOf(*it), ks);
  const double w = std::max(1e-6, t.box[2] - t.box[0]), h = std::max(1e-6, t.box[3] - t.box[1]);
  if (t.box[2] - t.box[0] < 1e-6) { t.box[0] -= 0.01; t.box[2] += 0.01; } if (t.box[3] - t.box[1] < 1e-6) { t.box[1] -= 0.01; t.box[3] += 0.01; }
  t.cu = (t.box[0] + t.box[2]) / 2; t.cv = (t.box[1] + t.box[3]) / 2; t.hw = (t.box[2] - t.box[0]) / 2; t.hh = (t.box[3] - t.box[1]) / 2; t.ang = 0;
  // (the warp's grid: as many cells along the box's longer side, fewer across — about square cells)
  const int N = std::clamp(uvo.warpN, 1, 12); const int lo = std::min(2, N); if (w >= h) { t.nx = N; t.ny = std::clamp(int(std::lround(N * h / w)), lo, N); } else { t.ny = N; t.nx = std::clamp(int(std::lround(N * w / h)), lo, N); }
  for (int j = 0; j <= t.ny; j++) for (int i = 0; i <= t.nx; i++) t.ctl.push_back({t.box[0] + (t.box[2] - t.box[0]) * i / t.nx, t.box[1] + (t.box[3] - t.box[1]) * j / t.ny});
  if (kind == 'p') {                                             // (Puppet: the selection's UV points (corners in one place — one point), its triangles)
    std::map<std::pair<float, float>, int> at; std::map<int, int> ofK;
    for (int k : t.ks) { const auto& u = t.uv0[size_t(k)]; auto r = at.emplace(std::pair<float, float>{u[0], u[1]}, int(t.prest.size())); if (r.second) t.prest.push_back({u[0], u[1]}); t.pof.push_back(r.first->second); ofK[k] = r.first->second; }
    const PolyMesh& B = *baseOf(*it);
    for (int f = 0; f < B.faceCount(); f++) { const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; bool all = true; for (int k = s; k < e && all; k++) all = ofK.count(k) > 0; if (!all) continue;
      for (int k = s + 1; k + 1 < e; k++) t.ptri.push_back({ofK[s], ofK[k], ofK[k + 1]}); }
    t.pcur = t.prest;
  }
  uvTf_ = t; uvTfRev_++; uvChanged();
  if (kind == 'p') { ui::hud(tr("Puppet: click the UV — a pin; drag a pin — the UV follows, stiff round the pins; Ctrl + click a pin — it goes"), 2500); return {}; }
  ui::hud(kind == 'w' ? tr("Warp: drag the anchors — the UV bends with them") : tr("Frame: drag inside — move; corners / sides — scale (Shift: evenly); just outside a corner — turn"), 2000);
  return {};
}
void Viewport::uvTfStop() { if (!uvTf_.on) return; uvTf_ = {}; uvTfRev_++; uvChanged(); }

// the selection's UV as the frame / the warp has it now
std::vector<std::array<float, 2>> Viewport::uvTfResult() const {
  const UvTf& t = uvTf_; std::vector<std::array<float, 2>> U = t.uv0;
  const double W = t.box[2] - t.box[0], H = t.box[3] - t.box[1];
  if (t.kind == 'p') { for (size_t i = 0; i < t.ks.size(); i++) { const auto& q = t.pcur[size_t(t.pof[i])]; U[size_t(t.ks[i])] = {float(q[0]), float(q[1])}; } return U; }
  if (t.kind == 'f') {
    const double hw0 = W / 2, hh0 = H / 2, c0u = (t.box[0] + t.box[2]) / 2, c0v = (t.box[1] + t.box[3]) / 2, cs = std::cos(t.ang), sn = std::sin(t.ang);
    for (int k : t.ks) { const double x = (t.uv0[size_t(k)][0] - c0u) / hw0 * t.hw, y = (t.uv0[size_t(k)][1] - c0v) / hh0 * t.hh;
      U[size_t(k)] = {float(t.cu + x * cs - y * sn), float(t.cv + x * sn + y * cs)}; }
  } else {
    auto anc = [&](int i, int j) { i = std::clamp(i, 0, t.nx); j = std::clamp(j, 0, t.ny); const auto& c = t.ctl[size_t(j * (t.nx + 1) + i)];
      return std::array<double, 2>{c[0] - (t.box[0] + W * i / t.nx), c[1] - (t.box[1] + H * j / t.ny)}; };   // (the anchor's move from its rest)
    for (int k : t.ks) { const double s = (t.uv0[size_t(k)][0] - t.box[0]) / W * t.nx, r = (t.uv0[size_t(k)][1] - t.box[1]) / H * t.ny;
      const int i = std::clamp(int(std::floor(s)), 0, t.nx - 1), j = std::clamp(int(std::floor(r)), 0, t.ny - 1); double wu[4], wv[4]; crw(s - i, wu); crw(r - j, wv);
      double dx = 0, dy = 0; for (int b = 0; b < 4; b++) for (int a = 0; a < 4; a++) { const auto d = anc(i - 1 + a, j - 1 + b); dx += wu[a] * wv[b] * d[0]; dy += wu[a] * wv[b] * d[1]; }
      U[size_t(k)] = {float(t.uv0[size_t(k)][0] + dx), float(t.uv0[size_t(k)][1] + dy)}; }
  }
  return U;
}

namespace {
// the frame's handles in UV: 0–3 the corners, 4–7 the sides' middles (−x, +x, −y, +y in its own axes)
template <class Tf> std::array<double, 2> frameAt(const Tf& t, double x, double y) { const double cs = std::cos(t.ang), sn = std::sin(t.ang), a = x * t.hw, b = y * t.hh; return {t.cu + a * cs - b * sn, t.cv + a * sn + b * cs}; }
const double HX[8] = {-1, 1, 1, -1, -1, 1, 0, 0}, HY[8] = {-1, -1, 1, 1, 0, 0, -1, 1};
}  // namespace

bool Viewport::uvTfPress(const QPointF& mp) {
  UvTf& t = uvTf_; if (!t.on) return false;
  const Item* it = scene_->item(t.m); if (!it || topoMesh() != t.m) { uvTfStop(); return false; }   // (another mesh: it ends)
  uvTfResync();                                                // (the UV changed under it — an undo: made again from it, the pins kept)
  auto px = [&](const std::array<double, 2>& u) { return uvToPx(u[0], u[1]); };
  t.drag = -1; t.p0 = mp; t.ctl0 = t.ctl; t.cu0 = t.cu; t.cv0 = t.cv; t.hw0 = t.hw; t.hh0 = t.hh; t.ang0 = t.ang;
  if (t.kind == 'p') {                                          // (Puppet: a pin dragged; Ctrl — a pin taken off; elsewhere on the UV — a new pin, dragged)
    double bd = 10; int hit = -1; for (size_t i = 0; i < t.pins.size(); i++) { QPointF q = px(t.pcur[size_t(t.pins[i])]); const double d = std::min(std::hypot(q.x() - mp.x(), q.y() - mp.y()), std::hypot(q.x() + 9 - mp.x(), q.y() - 20 - mp.y()));   // (its tip or its head)
      if (d < bd) { bd = d; hit = int(i); } }
    if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) { if (hit >= 0) { t.pins.erase(t.pins.begin() + hit); uvTfRev_++; uvChanged(); } return hit >= 0; }
    if (hit < 0) { double bp = 14; int pt = -1; for (size_t i = 0; i < t.pcur.size(); i++) { QPointF q = px(t.pcur[i]); const double d = std::hypot(q.x() - mp.x(), q.y() - mp.y()); if (d < bp) { bp = d; pt = int(i); } }
      if (pt < 0) return false; t.pins.push_back(pt); hit = int(t.pins.size()) - 1; uvTfRev_++; uvChanged(); }
    t.drag = hit; t.pcur0 = t.pcur; t.pin0.clear(); for (int p : t.pins) t.pin0.push_back(t.pcur[size_t(p)]); return true; }
  if (t.kind == 'w') { double bd = 10; for (size_t i = 0; i < t.ctl.size(); i++) { QPointF q = px(t.ctl[i]); const double d = std::hypot(q.x() - mp.x(), q.y() - mp.y()); if (d < bd) { bd = d; t.drag = int(i); } }
    return t.drag >= 0; }
  for (int h = 0; h < 8; h++) { QPointF q = px(frameAt(t, HX[h], HY[h])); if (std::hypot(q.x() - mp.x(), q.y() - mp.y()) < 9) { t.drag = h; return true; } }
  for (int h = 0; h < 4; h++) { QPointF q = px(frameAt(t, HX[h], HY[h])); const double d = std::hypot(q.x() - mp.x(), q.y() - mp.y()); if (d < 30) { t.drag = 8; return true; } }   // (just outside a corner: turn)
  // inside the frame: move
  auto at = pxToUv(mp); const double cs = std::cos(-t.ang), sn = std::sin(-t.ang), x = (at[0] - t.cu) * cs - (at[1] - t.cv) * sn, y = (at[0] - t.cu) * sn + (at[1] - t.cv) * cs;
  if (std::abs(x) <= std::abs(t.hw) && std::abs(y) <= std::abs(t.hh)) { t.drag = 9; return true; }
  return false;
}

void Viewport::uvTfMove(const QPointF& mp, Qt::KeyboardModifiers mods) {
  UvTf& t = uvTf_; if (!t.on || t.drag < 0) return;
  auto a0 = pxToUv(t.p0), a1 = pxToUv(mp); const bool shift = mods & Qt::ShiftModifier;
  if (t.kind == 'p') {                                          // (the pin where the pointer is; the rest fitted round the pins — each triangle as stiff as it can be)
    if (size_t(t.drag) >= t.pins.size() || t.pin0.size() != t.pins.size() || t.pcur.size() != t.prest.size()) return;   // (no drag begun on these pins: nothing to follow)
    std::vector<std::array<double, 2>> tgt = t.pin0; tgt[size_t(t.drag)] = {t.pin0[size_t(t.drag)][0] + a1[0] - a0[0], t.pin0[size_t(t.drag)][1] + a1[1] - a0[1]};
    if (t.pins.size() == 1) { const double dx = tgt[0][0] - t.pin0[0][0], dy = tgt[0][1] - t.pin0[0][1]; for (size_t i = 0; i < t.pcur.size(); i++) t.pcur[i] = {t.pcur0[i][0] + dx, t.pcur0[i][1] + dy}; }
    else {
      // (as rigid as possible: each triangle's best turn of its rest shape (local), then the whole shell fitted to those
      // turns at once — the linear system over its edges solved exactly (a banded Cholesky, made once per set of pins),
      // the pins held (global); a few rounds: it bends round the pins, it doesn't stretch or lag behind)
      const size_t n = t.pcur.size(); std::vector<char> pinned(n, 0); for (size_t i = 0; i < t.pins.size(); i++) { pinned[size_t(t.pins[i])] = 1; t.pcur[size_t(t.pins[i])] = tgt[i]; }
      // (the edges of the triangles, each counted per triangle, weighted by the cotangent of the angle across it — the true
      // ARAP weights: a fan of thin triangles round a pin doesn't spike; kept positive so the system stays solvable)
      std::vector<std::array<double, 3>> tw(t.ptri.size());
      for (size_t k = 0; k < t.ptri.size(); k++) for (int a = 0; a < 3; a++) { const auto& P0 = t.prest[size_t(t.ptri[k][size_t(a)])]; const auto& P1 = t.prest[size_t(t.ptri[k][size_t((a + 1) % 3)])]; const auto& Q = t.prest[size_t(t.ptri[k][size_t((a + 2) % 3)])];
        const double ax = P0[0] - Q[0], ay = P0[1] - Q[1], bx = P1[0] - Q[0], by = P1[1] - Q[1], cr = std::abs(ax * by - ay * bx), dt = ax * bx + ay * by;
        tw[k][size_t(a)] = std::clamp(cr > 1e-18 ? 0.5 * dt / cr : 0.0, 0.02, 20.0); }
      std::vector<std::vector<int>> adj(n); std::vector<std::vector<double>> adjW(n);
      for (size_t k = 0; k < t.ptri.size(); k++) for (int a = 0; a < 3; a++) { const int i = t.ptri[k][size_t(a)], j = t.ptri[k][size_t((a + 1) % 3)]; const double w = tw[k][size_t(a)];
        adj[size_t(i)].push_back(j); adjW[size_t(i)].push_back(w); adj[size_t(j)].push_back(i); adjW[size_t(j)].push_back(w); }
      std::vector<int> pk = t.pins; std::sort(pk.begin(), pk.end());
      if (t.chPins != pk || t.chN != n) {                       // (the factor: the free points ordered to a narrow band (reverse Cuthill-McKee))
        t.chPins = pk; t.chN = n; t.chPos.assign(n, -1); t.chOrd.clear();
        std::vector<char> seen(n, 0);
        for (size_t s0 = 0; s0 < n; s0++) { if (pinned[s0] || seen[s0]) continue;
          size_t best = s0; { std::vector<int> q{int(s0)}; std::vector<char> v2(n, 0); v2[s0] = 1; for (size_t h = 0; h < q.size(); h++) for (int j : adj[size_t(q[h])]) if (!pinned[size_t(j)] && !v2[size_t(j)]) { v2[size_t(j)] = 1; q.push_back(j); } best = size_t(q.back()); }   // (from a far point of its piece)
          std::vector<int> q{int(best)}; seen[best] = 1;
          for (size_t h = 0; h < q.size(); h++) { std::vector<int> nb; for (int j : adj[size_t(q[h])]) if (!pinned[size_t(j)] && !seen[size_t(j)]) { seen[size_t(j)] = 1; nb.push_back(j); }
            std::sort(nb.begin(), nb.end(), [&](int a, int b) { return adj[size_t(a)].size() < adj[size_t(b)].size(); }); q.insert(q.end(), nb.begin(), nb.end()); }
          bool held = false; for (int v : q) { for (int j : adj[size_t(v)]) if (pinned[size_t(j)]) { held = true; break; } if (held) break; }
          if (held) t.chOrd.insert(t.chOrd.end(), q.begin(), q.end()); }   // (a piece with no pin: left where it is — not in the system, which it would make singular)
        std::reverse(t.chOrd.begin(), t.chOrd.end());
        for (size_t k = 0; k < t.chOrd.size(); k++) t.chPos[size_t(t.chOrd[k])] = int(k);
        const int m = int(t.chOrd.size()); int bw = 0;
        for (int k = 0; k < m; k++) for (int j : adj[size_t(t.chOrd[size_t(k)])]) if (t.chPos[size_t(j)] >= 0) bw = std::max(bw, k - t.chPos[size_t(j)]);
        t.chBand = bw; const size_t W = size_t(bw) + 1; t.chL.assign(size_t(m) * W, 0.0);
        auto L = [&](int i, int j) -> double& { return t.chL[size_t(i) * W + size_t(i - j)]; };   // (row i, column j ≤ i, within the band)
        for (int k = 0; k < m; k++) { const int v = t.chOrd[size_t(k)];
          for (size_t e = 0; e < adj[size_t(v)].size(); e++) { const int j = adj[size_t(v)][e]; const double w = adjW[size_t(v)][e]; L(k, k) += w; const int pj = t.chPos[size_t(j)]; if (pj >= 0 && pj < k) L(k, pj) -= w; } }
        for (int i = 0; i < m; i++) {                          // (in place: the band's Cholesky)
          for (int j = std::max(0, i - bw); j <= i; j++) { double s = L(i, j);
            for (int k = std::max(0, i - bw); k < j; k++) if (j - k <= bw) s -= L(i, k) * L(j, k);
            if (i == j) L(i, i) = std::sqrt(std::max(s, 1e-12)); else L(i, j) = s / L(j, j); } }
      }
      const int m = int(t.chOrd.size()); const int bw = t.chBand; const size_t W = size_t(bw) + 1;
      auto Lc = [&](int i, int j) { return t.chL[size_t(i) * W + size_t(i - j)]; };
      std::vector<std::array<double, 2>> rot(t.ptri.size());
      auto rounds = [&] { for (int round = 0; round < 12 && m > 0; round++) {
        for (size_t k = 0; k < t.ptri.size(); k++) { const auto& tr3 = t.ptri[k]; double sc = 0, ss = 0;   // (local: the turn that best fits the triangle's rest edges to its edges now)
          for (int a = 0; a < 3; a++) { const int i = tr3[size_t(a)], j = tr3[size_t((a + 1) % 3)];
            const double rx = t.prest[size_t(i)][0] - t.prest[size_t(j)][0], ry = t.prest[size_t(i)][1] - t.prest[size_t(j)][1];
            const double ux = t.pcur[size_t(i)][0] - t.pcur[size_t(j)][0], uy = t.pcur[size_t(i)][1] - t.pcur[size_t(j)][1]; const double w = tw[k][size_t(a)]; sc += w * (rx * ux + ry * uy); ss += w * (rx * uy - ry * ux); }
          const double th = std::atan2(ss, sc); rot[k] = {std::cos(th), std::sin(th)}; }
        for (int dim = 0; dim < 2; dim++) {                     // (global: L x = b for u, then v — forward and back through the factor)
          std::vector<double> b(n, 0);
          for (size_t k = 0; k < t.ptri.size(); k++) { const auto& tr3 = t.ptri[k]; const double c = rot[k][0], s = rot[k][1];
            for (int a = 0; a < 3; a++) { const int i = tr3[size_t(a)], j = tr3[size_t((a + 1) % 3)];
              const double rx = t.prest[size_t(i)][0] - t.prest[size_t(j)][0], ry = t.prest[size_t(i)][1] - t.prest[size_t(j)][1];
              const double e = tw[k][size_t(a)] * (dim == 0 ? c * rx - s * ry : s * rx + c * ry); b[size_t(i)] += e; b[size_t(j)] -= e; } }
          std::vector<double> y(size_t(m), 0);
          for (int k = 0; k < m; k++) { const int v = t.chOrd[size_t(k)]; double r = b[size_t(v)]; for (size_t e = 0; e < adj[size_t(v)].size(); e++) { const int j = adj[size_t(v)][e]; if (pinned[size_t(j)]) r += adjW[size_t(v)][e] * t.pcur[size_t(j)][size_t(dim)]; } y[size_t(k)] = r; }
          for (int i = 0; i < m; i++) { double s = y[size_t(i)]; for (int k = std::max(0, i - bw); k < i; k++) s -= Lc(i, k) * y[size_t(k)]; y[size_t(i)] = s / Lc(i, i); }
          for (int i = m - 1; i >= 0; i--) { double s = y[size_t(i)]; for (int k = i + 1; k <= std::min(m - 1, i + bw); k++) s -= Lc(k, i) * y[size_t(k)]; y[size_t(i)] = s / Lc(i, i); }
          for (int k = 0; k < m; k++) t.pcur[size_t(t.chOrd[size_t(k)])][size_t(dim)] = y[size_t(k)];
        }
      } };
      // (from two starts — where it was a moment ago, and the whole shell turned to fit its pins — the one bent least kept)
      auto energy = [&] { double E = 0; for (size_t k = 0; k < t.ptri.size(); k++) { const auto& tr3 = t.ptri[k]; double sc = 0, ss = 0;
          for (int a = 0; a < 3; a++) { const int i = tr3[size_t(a)], j = tr3[size_t((a + 1) % 3)]; const double rx = t.prest[size_t(i)][0] - t.prest[size_t(j)][0], ry = t.prest[size_t(i)][1] - t.prest[size_t(j)][1], ux = t.pcur[size_t(i)][0] - t.pcur[size_t(j)][0], uy = t.pcur[size_t(i)][1] - t.pcur[size_t(j)][1]; sc += rx * ux + ry * uy; ss += rx * uy - ry * ux; }
          const double th = std::atan2(ss, sc), c = std::cos(th), s = std::sin(th);
          for (int a = 0; a < 3; a++) { const int i = tr3[size_t(a)], j = tr3[size_t((a + 1) % 3)]; const double rx = t.prest[size_t(i)][0] - t.prest[size_t(j)][0], ry = t.prest[size_t(i)][1] - t.prest[size_t(j)][1];
            const double ex = t.pcur[size_t(i)][0] - t.pcur[size_t(j)][0] - (c * rx - s * ry), ey = t.pcur[size_t(i)][1] - t.pcur[size_t(j)][1] - (s * rx + c * ry); E += tw[k][size_t(a)] * (ex * ex + ey * ey); } } return E; };
      const auto warm = t.pcur;
      { double ru = 0, rv = 0, cu = 0, cv = 0; const double np = double(t.pins.size());
        for (size_t i = 0; i < t.pins.size(); i++) { ru += t.prest[size_t(t.pins[i])][0] / np; rv += t.prest[size_t(t.pins[i])][1] / np; cu += tgt[i][0] / np; cv += tgt[i][1] / np; }
        double sc = 0, ss = 0; for (size_t i = 0; i < t.pins.size(); i++) { const double rx = t.prest[size_t(t.pins[i])][0] - ru, ry = t.prest[size_t(t.pins[i])][1] - rv, ux = tgt[i][0] - cu, uy = tgt[i][1] - cv; sc += rx * ux + ry * uy; ss += rx * uy - ry * ux; }
        const double th = std::atan2(ss, sc), c = std::cos(th), s = std::sin(th);
        for (size_t i = 0; i < n; i++) if (!pinned[i]) { const double rx = t.prest[i][0] - ru, ry = t.prest[i][1] - rv; t.pcur[i] = {cu + c * rx - s * ry, cv + s * rx + c * ry}; } }
      rounds(); const auto fromFit = t.pcur; const double eFit = energy();
      t.pcur = warm; rounds(); if (eFit < energy()) t.pcur = fromFit;
      for (size_t i = 0; i < n; i++) if (!pinned[i] && t.chPos[i] < 0) t.pcur[i] = warm[i];   // (the pieces with no pin: where they were)
    }
    uvTfRev_++; uvLive(t.m, uvTfResult()); return; }
  if (t.kind == 'w') { t.ctl[size_t(t.drag)] = {t.ctl0[size_t(t.drag)][0] + a1[0] - a0[0], t.ctl0[size_t(t.drag)][1] + a1[1] - a0[1]}; }
  else if (t.drag == 9) { t.cu = t.cu0 + a1[0] - a0[0]; t.cv = t.cv0 + a1[1] - a0[1]; }
  else if (t.drag == 8) { const double b0 = std::atan2(a0[1] - t.cv0, a0[0] - t.cu0), b1 = std::atan2(a1[1] - t.cv0, a1[0] - t.cu0); double d = b1 - b0;
    if (shift) d = std::round(d / (M_PI / 12)) * (M_PI / 12); t.ang = t.ang0 + d; ui::hud(tr("Turn %1°").arg(t.ang * 180 / M_PI, 0, 'f', 1)); }
  else {                                                     // scale: the opposite side held (its own axes)
    const double cs = std::cos(-t.ang0), sn = std::sin(-t.ang0); auto loc = [&](const std::array<double, 2>& u) { const double x = u[0] - t.cu0, y = u[1] - t.cv0; return std::array<double, 2>{x * cs - y * sn, x * sn + y * cs}; };
    const auto p = loc(a1); const double hx = HX[t.drag], hy = HY[t.drag];
    double nx0 = -t.hw0, nx1 = t.hw0, ny0 = -t.hh0, ny1 = t.hh0;   // (the box in its own axes)
    if (hx > 0) nx1 = p[0]; else if (hx < 0) nx0 = p[0];
    if (hy > 0) ny1 = p[1]; else if (hy < 0) ny0 = p[1];
    double nw = (nx1 - nx0) / 2, nh = (ny1 - ny0) / 2;
    if (shift && hx != 0 && hy != 0) { const double k = std::max(std::abs(nw) / std::max(1e-9, t.hw0), std::abs(nh) / std::max(1e-9, t.hh0)); nw = (nw < 0 ? -1 : 1) * t.hw0 * k; nh = (nh < 0 ? -1 : 1) * t.hh0 * k;
      if (hx > 0) nx1 = nx0 + 2 * nw; else nx0 = nx1 - 2 * nw; if (hy > 0) ny1 = ny0 + 2 * nh; else ny0 = ny1 - 2 * nh; }
    const double mx = (nx0 + nx1) / 2, my = (ny0 + ny1) / 2, c = std::cos(t.ang0), s = std::sin(t.ang0);
    t.cu = t.cu0 + mx * c - my * s; t.cv = t.cv0 + mx * s + my * c; t.hw = nw; t.hh = nh;
    ui::hud(tr("Scale U %1 · V %2").arg(t.hw / std::max(1e-9, (t.box[2] - t.box[0]) / 2), 0, 'f', 3).arg(t.hh / std::max(1e-9, (t.box[3] - t.box[1]) / 2), 0, 'f', 3));
  }
  uvTfRev_++; uvLive(t.m, uvTfResult());
}

void Viewport::uvTfRelease() {
  UvTf& t = uvTf_; if (!t.on || t.drag < 0) return; t.drag = -1; ui::hud({});
  if (Item* it = scene_->item(t.m)) { auto B = baseSp(*it); B->cornerUV = uvTfResult(); uvSymApply(B->cornerUV); topoWrite(t.m, B); t.base = baseOf(*scene_->item(t.m)); uvStamp_++; uvRev_++; commit(false); emit topoState(); }
  uvTfRev_++; requestUpdate();
}

// the frame (its box, handles, the turn marks) / the warp (its grid bent as the UV is, its anchors)
void Viewport::uvTfDraw(UvDraw& D) {
  const UvTf& t = uvTf_; if (!t.on) return;
  const QColor C(0xff, 0xc8, 0x3c);
  UvDraw::Lines L; L.c = C; L.w = 1.6f; UvDraw::Pts P; P.size = 9;
  auto f2 = [](const std::array<double, 2>& a) { return std::array<float, 2>{float(a[0]), float(a[1])}; };
  if (t.kind == 'p') {                                          // (Puppet: its triangles as they are bent, the pins)
    UvDraw::Lines M; M.c = QColor(0xc8, 0xc8, 0xd8, 150); M.w = 1;
    for (const auto& tr3 : t.ptri) for (int i = 0; i < 3; i++) { M.p.push_back(f2(t.pcur[size_t(tr3[size_t(i)])])); M.p.push_back(f2(t.pcur[size_t(tr3[size_t((i + 1) % 3)])])); }
    D.lines.push_back(std::move(M)); return; }                  // (the pins: drawn as pins over the editor — Viewport::overlays)
  if (t.kind == 'f') {
    for (int h = 0; h < 4; h++) { L.p.push_back(f2(frameAt(t, HX[h], HY[h]))); L.p.push_back(f2(frameAt(t, HX[(h + 1) % 4], HY[(h + 1) % 4]))); }
    for (int h = 0; h < 8; h++) { P.p.push_back(f2(frameAt(t, HX[h], HY[h]))); P.c.push_back(h < 4 ? C : QColor(0xff, 0xe8, 0xa0)); }
    P.p.push_back(f2(frameAt(t, 0, 0))); P.c.push_back(QColor(0xff, 0xff, 0xff));
  } else {
    // (each grid line drawn bent: the warp of its points along it)
    const double W = t.box[2] - t.box[0], H = t.box[3] - t.box[1];
    auto warp = [&](double u, double v) { double s = (u - t.box[0]) / W * t.nx, r = (v - t.box[1]) / H * t.ny;
      const int i = std::clamp(int(std::floor(s)), 0, t.nx - 1), j = std::clamp(int(std::floor(r)), 0, t.ny - 1); double wu[4], wv[4]; crw(s - i, wu); crw(r - j, wv); double dx = 0, dy = 0;
      for (int b = 0; b < 4; b++) for (int a = 0; a < 4; a++) { const int ii = std::clamp(i - 1 + a, 0, t.nx), jj = std::clamp(j - 1 + b, 0, t.ny); const auto& c = t.ctl[size_t(jj * (t.nx + 1) + ii)];
        dx += wu[a] * wv[b] * (c[0] - (t.box[0] + W * ii / t.nx)); dy += wu[a] * wv[b] * (c[1] - (t.box[1] + H * jj / t.ny)); }
      return std::array<float, 2>{float(u + dx), float(v + dy)}; };
    const int S = 12;
    for (int j = 0; j <= t.ny; j++) for (int q = 0; q < t.nx * S; q++) { const double v = t.box[1] + H * j / t.ny; L.p.push_back(warp(t.box[0] + W * q / (t.nx * S), v)); L.p.push_back(warp(t.box[0] + W * (q + 1) / (t.nx * S), v)); }
    for (int i = 0; i <= t.nx; i++) for (int q = 0; q < t.ny * S; q++) { const double u = t.box[0] + W * i / t.nx; L.p.push_back(warp(u, t.box[1] + H * q / (t.ny * S))); L.p.push_back(warp(u, t.box[1] + H * (q + 1) / (t.ny * S))); }
    for (auto& c : t.ctl) { P.p.push_back(f2(c)); P.c.push_back(C); }
  }
  D.lines.push_back(std::move(L)); D.pts.push_back(std::move(P));
}

QString Viewport::uvTfTestDrag(int h, double dx, double dy, bool shift) {
  UvTf& t = uvTf_; if (!t.on) return "off";
  std::array<double, 2> u; if (t.kind == 'w') { if (size_t(h) >= t.ctl.size()) return "no anchor"; u = t.ctl[size_t(h)]; }
  else u = h == 9 ? std::array<double, 2>{t.cu, t.cv} : h == 8 ? frameAt(t, 1.25, 1.25) : frameAt(t, HX[h], HY[h]);
  if (t.kind == 'p') { if (size_t(h) >= t.pins.size()) return "no pin"; t.pcur0 = t.pcur; t.pin0.clear(); for (int p : t.pins) t.pin0.push_back(t.pcur[size_t(p)]); u = t.pcur[size_t(t.pins[size_t(h)])]; }   // (a pin: its drag begun as a press does)
  const QPointF p = uvToPx(u[0], u[1]); t.drag = t.kind == 'w' ? h : h; t.p0 = p; t.ctl0 = t.ctl; t.cu0 = t.cu; t.cv0 = t.cv; t.hw0 = t.hw; t.hh0 = t.hh; t.ang0 = t.ang;
  uvTfMove(p + QPointF(dx, dy), shift ? Qt::ShiftModifier : Qt::NoModifier); uvTfRelease(); return "dragged";
}

}  // namespace hm

namespace hm {
QString Viewport::uvPuppetTest(int steps, bool bend, bool arc) {
  UvTf& t = uvTf_; if (!t.on || t.kind != 'p' || t.prest.size() < 3) return "no puppet";
  int a = 0, b = 0; for (size_t i = 0; i < t.prest.size(); i++) { if (t.prest[i][0] < t.prest[size_t(a)][0]) a = int(i); if (t.prest[i][0] > t.prest[size_t(b)][0]) b = int(i); }
  const auto A = t.prest[size_t(a)], Bp = t.prest[size_t(b)];
  int mid = a; { double bd = 1e300; const double cu = (A[0] + Bp[0]) / 2, cv = (A[1] + Bp[1]) / 2; for (size_t i = 0; i < t.prest.size(); i++) { const double d = std::hypot(t.prest[i][0] - cu, t.prest[i][1] - cv); if (d < bd) { bd = d; mid = int(i); } } }
  t.pins = arc ? std::vector<int>{a, mid, b} : std::vector<int>{a, b}; t.pcur = t.prest; t.pcur0 = t.pcur;
  QElapsedTimer tm; tm.start();
  if (arc) for (int s = 1; s <= steps; s++) {                  // (an arc: the far end swung round, the middle and the near end held)
    const auto Mp = t.prest[size_t(mid)]; const double th = M_PI / 3 * s / steps, c = std::cos(th), sn = std::sin(th), dx = Bp[0] - Mp[0], dy = Bp[1] - Mp[1];   // (B turned 60° round the middle pin)
    t.pin0 = {A, Mp, {Mp[0] + c * dx - sn * dy, Mp[1] + sn * dx + c * dy}}; t.drag = 2; t.p0 = uvToPx(t.pin0[2][0], t.pin0[2][1]); uvTfMove(t.p0, Qt::NoModifier); }
  else for (int s = 1; s <= steps; s++) {                            // (B turned round A, a step at a time — as a drag)
    const double th = bend ? 0 : M_PI / 2 * s / steps, c = std::cos(th), sn = std::sin(th), k = bend ? 1 - 0.3 * s / steps : 1, dx = (Bp[0] - A[0]) * k - (bend ? (Bp[1] - A[1]) * 0.25 * s / steps : 0), dy = (Bp[1] - A[1]) * k + (bend ? (Bp[0] - A[0]) * 0.25 * s / steps : 0);
    t.pin0 = {A, {A[0] + c * dx - sn * dy, A[1] + sn * dx + c * dy}}; t.drag = 1; t.p0 = uvToPx(t.pin0[1][0], t.pin0[1][1]);
    uvTfMove(t.p0, Qt::NoModifier); }
  double err = 0; int ne = 0, flip = 0;
  for (const auto& tr3 : t.ptri) { for (int k = 0; k < 3; k++) { const int i = tr3[size_t(k)], j = tr3[size_t((k + 1) % 3)];
      const double r = std::hypot(t.prest[size_t(i)][0] - t.prest[size_t(j)][0], t.prest[size_t(i)][1] - t.prest[size_t(j)][1]), c = std::hypot(t.pcur[size_t(i)][0] - t.pcur[size_t(j)][0], t.pcur[size_t(i)][1] - t.pcur[size_t(j)][1]);
      if (r > 1e-12) { err += std::abs(c / r - 1); ne++; } }
    auto ar = [&](const std::vector<std::array<double, 2>>& P) { const auto &p0 = P[size_t(tr3[0])], &p1 = P[size_t(tr3[1])], &p2 = P[size_t(tr3[2])]; return (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0]); };
    if (ar(t.prest) * ar(t.pcur) < 0) flip++; }
  t.drag = -1;
  return QString("points %1 · triangles %2 · stretch %3% · folded %4 · %5 ms a step").arg(t.prest.size()).arg(t.ptri.size()).arg(ne ? err / ne * 100 : 0, 0, 'f', 2).arg(flip).arg(tm.elapsed() / std::max(1, steps));
}
}  // namespace hm

namespace hm {
QString Viewport::uvPuppetSynth(int fan) {
  const UvTf keep = uvTf_; UvTf& t = uvTf_; t = {}; t.on = true; t.kind = 'p'; t.m = -1;
  const int NX = 40, NY = 6; const double L = 0.8, H = 0.12;
  for (int j = 0; j <= NY; j++) for (int i = 0; i <= NX; i++) t.prest.push_back({0.1 + L * i / NX, 0.4 + H * j / NY});
  auto id = [&](int i, int j) { return j * (NX + 1) + i; };
  const int cm = int(t.prest.size()); if (fan) t.prest.push_back({0.1 + L / 2 + 0.5 * L / NX, 0.4 + H / 2 + 0.5 * H / NY});   // (an extra point in the middle cell: a fan round it)
  for (int j = 0; j < NY; j++) for (int i = 0; i < NX; i++) {
    const int a = id(i, j), b = id(i + 1, j), c = id(i + 1, j + 1), d = id(i, j + 1);
    if (fan && i == NX / 2 && j == NY / 2) { t.ptri.push_back({a, b, cm}); t.ptri.push_back({b, c, cm}); t.ptri.push_back({c, d, cm}); t.ptri.push_back({d, a, cm}); }
    else { t.ptri.push_back({a, b, c}); t.ptri.push_back({a, c, d}); } }
  t.pcur = t.prest; const int A = id(0, NY / 2), M = fan ? cm : id(NX / 2, NY / 2), B = id(NX, NY / 2);
  t.pins = {A, M, B}; const auto Mp = t.prest[size_t(M)], Bp = t.prest[size_t(B)];
  const int steps = 12;
  for (int s = 1; s <= steps; s++) { const double th = M_PI / 3 * s / steps, c = std::cos(th), sn = std::sin(th), dx = Bp[0] - Mp[0], dy = Bp[1] - Mp[1];
    t.pcur0 = t.pcur; t.pin0 = {t.prest[size_t(A)], Mp, {Mp[0] + c * dx - sn * dy, Mp[1] + sn * dx + c * dy}}; t.drag = 2; t.p0 = uvToPx(t.pin0[2][0], t.pin0[2][1]); uvTfMove(t.p0, Qt::NoModifier); }
  double err = 0, worst = 0; int ne = 0, flip = 0; double endTurn = 0;
  for (const auto& tr3 : t.ptri) { for (int k = 0; k < 3; k++) { const int i = tr3[size_t(k)], j = tr3[size_t((k + 1) % 3)];
      const double r = std::hypot(t.prest[size_t(i)][0] - t.prest[size_t(j)][0], t.prest[size_t(i)][1] - t.prest[size_t(j)][1]), c = std::hypot(t.pcur[size_t(i)][0] - t.pcur[size_t(j)][0], t.pcur[size_t(i)][1] - t.pcur[size_t(j)][1]);
      if (r > 1e-12) { err += std::abs(c / r - 1); worst = std::max(worst, std::abs(c / r - 1)); ne++; } }
    auto ar = [&](const std::vector<std::array<double, 2>>& P) { const auto &p0 = P[size_t(tr3[0])], &p1 = P[size_t(tr3[1])], &p2 = P[size_t(tr3[2])]; return (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0]); };
    if (ar(t.prest) * ar(t.pcur) < 0) flip++; }
  { const int e0 = id(NX, 0), e1 = id(NX, NY); const double a0 = std::atan2(t.prest[size_t(e1)][1] - t.prest[size_t(e0)][1], t.prest[size_t(e1)][0] - t.prest[size_t(e0)][0]), a1 = std::atan2(t.pcur[size_t(e1)][1] - t.pcur[size_t(e0)][1], t.pcur[size_t(e1)][0] - t.pcur[size_t(e0)][0]); endTurn = (a1 - a0) * 180 / M_PI; }
  uvTf_ = keep;
  return QString("stretch %1% · worst edge %2% · folded %3 · far end turned %4° (60 asked)").arg(err / ne * 100, 0, 'f', 2).arg(worst * 100, 0, 'f', 1).arg(flip).arg(endTurn, 0, 'f', 1);
}
}  // namespace hm

namespace hm {
// the UV symmetry: each moving corner's mirror (the same corner of the mirrored polygon — the mesh symmetric across its
// X) put at its place mirrored across U = symAxis; a mirror that moves itself (both sides selected) left as it goes
void Viewport::uvSymApply(std::vector<std::array<float, 2>>& U) const {
  if (!tset.sym || room() != "topo") return;
  const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return;
  const PolyMesh* B = baseOf(*it); if (!B || U.size() != B->faceVerts.size()) return;
  if (uvSymMap_.B != B || uvSymMap_.n != B->faceVerts.size()) {
    uvSymMap_ = {}; uvSymMap_.B = B; uvSymMap_.n = B->faceVerts.size(); uvSymMap_.part.assign(B->faceVerts.size(), -1);
    const auto pv = topo::symPairs(*B, B->pos);
    std::map<std::vector<int>, int> byVerts;
    for (int f = 0; f < B->faceCount(); f++) { std::vector<int> vs(B->faceVerts.begin() + B->faceStart[size_t(f)], B->faceVerts.begin() + B->faceStart[size_t(f) + 1]); std::sort(vs.begin(), vs.end()); byVerts[vs] = f; }
    for (int f = 0; f < B->faceCount(); f++) { const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1]; std::vector<int> mv; bool ok = true;
      for (int k = s; k < e && ok; k++) { const int v = B->faceVerts[size_t(k)]; const int w = size_t(v) < pv.size() ? pv[size_t(v)] : -1; ok = w >= 0; mv.push_back(w); }
      if (!ok) continue; std::vector<int> sv = mv; std::sort(sv.begin(), sv.end()); auto g = byVerts.find(sv); if (g == byVerts.end() || g->second == f) continue;
      const int f2 = g->second, s2 = B->faceStart[size_t(f2)], e2 = B->faceStart[size_t(f2) + 1];
      for (int k = s; k < e; k++) { const int w = mv[size_t(k - s)]; for (int k2 = s2; k2 < e2; k2++) if (B->faceVerts[size_t(k2)] == w) { uvSymMap_.part[size_t(k)] = k2; break; } } }
  }
  const auto ks = uvMoving(); if (ks.empty()) return;
  std::vector<char> moving(U.size(), 0); for (int k : ks) if (size_t(k) < moving.size()) moving[size_t(k)] = 1;
  const float ax = float(uvo.symAxis);
  for (int k : ks) { const int p = size_t(k) < uvSymMap_.part.size() ? uvSymMap_.part[size_t(k)] : -1; if (p < 0 || moving[size_t(p)]) continue;
    U[size_t(p)] = {2 * ax - U[size_t(k)][0], U[size_t(k)][1]}; }
}
}  // namespace hm

namespace hm {
bool Viewport::uvSymAxisFromSel() {
  const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; const auto ks = uvMoving();
  if (!it || ks.empty()) { ui::toast(tr("Select the shell lying across the axis (or a pair of mirror shells) first")); return false; }
  const auto bx = uv::box(*baseOf(*it), ks); uvo.symAxis = (bx[0] + bx[2]) / 2; saveUvPrefs(); uvChanged();
  ui::hud(tr("UV Symmetry axis: U = %1").arg(uvo.symAxis, 0, 'f', 3), 900); return true;
}
}  // namespace hm

namespace hm {
QString Viewport::uvSymCheck() const {
  const int m = topoMesh(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "no mesh";
  const PolyMesh* B = baseOf(*it); auto U = B->cornerUV; auto V = U; uvSymApply(V);   // (the map made; nothing moved: V as it should be)
  const auto ks = uvMoving(); int pairs = 0; double worst = 0;
  for (int k : ks) { const int p = size_t(k) < uvSymMap_.part.size() ? uvSymMap_.part[size_t(k)] : -1; if (p < 0) continue; pairs++;
    worst = std::max(worst, std::hypot(double(U[size_t(p)][0]) - (2 * uvo.symAxis - U[size_t(k)][0]), double(U[size_t(p)][1]) - U[size_t(k)][1])); }
  return QString("moving corners %1 · with mirrors %2 · worst off %3").arg(ks.size()).arg(pairs).arg(worst, 0, 'g', 3);
}
}  // namespace hm

namespace hm {
// the Puppet's pins as pins: the tip on its point, the needle leaning up and right, a round red head ringed in white
// (filled with rings) — the size of the screen's, whatever the zoom; the dragged one orange
void Viewport::uvPinOverlay(std::vector<Overlay>& out) const {
  const UvTf& t = uvTf_; if (!t.on || t.kind != 'p' || cam_.view != "uv" || room() != "topo") return;
  auto circle = [](double cx, double cy, double r) { std::vector<std::pair<double, double>> P; const int n = std::max(10, int(r * 4)); for (int i = 0; i < n; i++) { const double a = 2 * M_PI * i / n; P.push_back({cx + r * std::cos(a), cy + r * std::sin(a)}); } return P; };
  for (size_t i = 0; i < t.pins.size(); i++) {
    const int p = t.pins[i]; if (size_t(p) >= t.pcur.size()) continue;
    const QPointF s = uvToPx(t.pcur[size_t(p)][0], t.pcur[size_t(p)][1]); const double hx = s.x() + 9, hy = s.y() - 20;   // (the head)
    const bool on = int(i) == t.drag; const QColor head = on ? QColor(0xff, 0x9a, 0x2e) : QColor(0xf0, 0x3c, 0x3c), dark = on ? QColor(0xb8, 0x5a, 0x10) : QColor(0x9a, 0x1e, 0x1e);
    for (double d : {-1.4, 1.4}) out.push_back({{{s.x() + d, s.y()}, {hx + d, hy}}, QColor(0x18, 0x18, 0x1c), false});   // (the needle: dark-edged, light — seen over any wire)
    for (double d : {-0.5, 0.0, 0.5}) out.push_back({{{s.x() + d, s.y()}, {hx + d, hy}}, QColor(0xe8, 0xe8, 0xf0), false});
    out.push_back({{{s.x() - 2, s.y()}, {s.x() + 2, s.y()}}, QColor(0xff, 0xff, 0xff), false});                          // (where it holds)
    for (double r = 0.8; r <= 7.0; r += 0.7) out.push_back({circle(hx, hy, r), r > 5.8 ? dark : head, true});               // (the head, filled)
    out.push_back({circle(hx - 1.8, hy - 1.8, 1.4), QColor(0xff, 0xd0, 0xd0), true});                                  // (its shine)
    out.push_back({circle(hx, hy, 7.8), QColor(0xff, 0xff, 0xff), true}); out.push_back({circle(hx, hy, 8.6), QColor(0x18, 0x18, 0x1c), true});                                               // (ringed in white)
  }
}
}  // namespace hm

namespace hm {
int Viewport::overlayRevision() const {
  const bool giz = uvGizmoOn();                                // (the gizmo: it follows the view, the selection, the tool)
  if (!giz && (!uvTf_.on || uvTf_.kind != 'p' || uvTf_.pins.empty() || cam_.view != "uv")) return overlayRev_;
  const QPointF a = uvToPx(0, 0), b = uvToPx(1, 1);           // (where the tile is on the screen now)
  QByteArray k = QByteArray::number(a.x(), 'f', 2) + "," + QByteArray::number(a.y(), 'f', 2) + "," + QByteArray::number(b.x(), 'f', 2) + "," + QByteArray::number(b.y(), 'f', 2);
  if (giz) { const auto p = uvGizmoPivot(); k += "|" + tool_.toLatin1() + QByteArray::number(tsel_.ver) + "," + QByteArray::number(p[0], 'f', 5) + "," + QByteArray::number(p[1], 'f', 5); }
  return overlayRev_ ^ int(qHash(k));
}
}  // namespace hm

namespace hm {
void Viewport::uvTfResync() {
  UvTf& t = uvTf_; if (!t.on || t.drag >= 0) return;
  const Item* it = scene_->item(t.m); if (!it) { uvTfStop(); return; }
  if (baseOf(*it) == t.base) return;
  const char kind = t.kind; const auto pins = t.pins; const size_t n = t.prest.size();
  t.on = false; uvTfStart(kind); ui::hud({});
  if (kind == 'p' && uvTf_.on && uvTf_.prest.size() == n) { for (int p : pins) if (size_t(p) < n) uvTf_.pins.push_back(p); }
  uvTfRev_++; overlayRev_++;
}
}  // namespace hm

namespace hm {
QString Viewport::uvTfInfo() {
  uvTfResync(); const UvTf& t = uvTf_; const Item* it = scene_->item(t.m);
  double off = 0; if (it && t.on && t.kind == 'p') { const auto& U = baseOf(*it)->cornerUV; for (size_t i = 0; i < t.ks.size() && i < t.pof.size(); i++) { const auto& q = t.pcur[size_t(t.pof[i])]; off = std::max(off, std::hypot(q[0] - U[size_t(t.ks[i])][0], q[1] - U[size_t(t.ks[i])][1])); } }
  return QString("on %1 · kind %2 · pins %3 · mesh in step %4 · puppet off the UV by %5").arg(t.on).arg(QChar(t.kind ? t.kind : '-')).arg(t.pins.size()).arg(it && baseOf(*it) == t.base).arg(off, 0, 'g', 3);
}
}  // namespace hm
