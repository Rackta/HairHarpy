// Pack by shape: each shell rasterised into the texture's grid (its triangles, grown by half the padding), the masks
// kept as runs per row; placed into an occupancy grid of bit rows — the lowest place, then the leftmost, where no run
// meets a taken bit (a clash skips past the bit that clashed). The largest scale that fits: a binary search.
// Symmetric: the grid's column `ac` is the axis — the shells across the plane (their masks made symmetric) stacked on
// it, a pair's leader placed on its side with its mirror image taken too, then the rest anywhere.
#include "scene/Uv.h"
#include <algorithm>
#include <cmath>
#include <bit>
#include <cstdint>
#include <map>

namespace hm::uv {

namespace {
using P = std::array<double, 2>;
struct Run { int r, x0, x1; };                                   // (row r: cells x0…x1−1)
struct Mask { int w = 0, h = 0; std::vector<Run> runs; double area = 0; };

// the occupancy: a bit per cell, rows of 64-bit words
struct Grid {
  int W = 0, H = 0, words = 0; std::vector<uint64_t> bits;
  Grid(int w, int h) : W(w), H(h), words((w + 63) / 64), bits(size_t(words) * size_t(h), 0) {}
  // the first taken cell in [a, b) of row y (−1: none)
  int firstSet(int y, int a, int b) const {
    const uint64_t* row = &bits[size_t(y) * size_t(words)];
    for (int i = a; i < b;) { const int wi = i >> 6, bi = i & 63; uint64_t v = row[wi] >> bi; const int span = std::min(64 - bi, b - i);
      if (span < 64) v &= (uint64_t(1) << span) - 1; if (v) { return i + std::countr_zero(v); } i += span; }
    return -1;
  }
  // the first free cell from a on in row y (b: none before it)
  int firstClear(int y, int a, int b) const {
    const uint64_t* row = &bits[size_t(y) * size_t(words)];
    for (int i = a; i < b;) { const int wi = i >> 6, bi = i & 63; const uint64_t v = ~row[wi] >> bi; const int span = std::min(64 - bi, b - i);
      if (v) { const int z = std::countr_zero(v); if (z < span) return i + z; } i += span; }
    return b;
  }
  void set(int y, int a, int b) { uint64_t* row = &bits[size_t(y) * size_t(words)]; for (int i = a; i < b; i++) row[i >> 6] |= uint64_t(1) << (i & 63); }
  // where the mask's left-bottom may go at (x, y): −1 — it fits; else the next x worth trying
  int clash(const Mask& m, int x, int y, int mirrorAt = -1) const {
    for (const Run& r : m.runs) { const int j = firstSet(y + r.r, x + r.x0, x + r.x1); if (j >= 0) return std::max(firstClear(y + r.r, j, W) - r.x0, x + 1); }   // (past the whole taken stretch)
    if (mirrorAt >= 0) for (const Run& r : m.runs) { const int a = 2 * mirrorAt - (x + r.x1), b = 2 * mirrorAt - (x + r.x0); if (a < 0 || firstSet(y + r.r, a, b) >= 0) return x + 1; }   // (its mirror image's place too)
    return -1;
  }
  void put(const Mask& m, int x, int y, int mirrorAt = -1) {
    for (const Run& r : m.runs) { set(y + r.r, x + r.x0, x + r.x1); if (mirrorAt >= 0) set(y + r.r, 2 * mirrorAt - (x + r.x1), 2 * mirrorAt - (x + r.x0)); }
  }
};

// the distance from p to triangle abc (0 inside)
double triDist(const P& p, const P& a, const P& b, const P& c) {
  auto cr = [](const P& o, const P& u, const P& v) { return (u[0] - o[0]) * (v[1] - o[1]) - (u[1] - o[1]) * (v[0] - o[0]); };
  const double d1 = cr(a, b, p), d2 = cr(b, c, p), d3 = cr(c, a, p);
  if ((d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0)) return 0;
  auto seg = [](const P& p, const P& a, const P& b) { const double dx = b[0] - a[0], dy = b[1] - a[1], L = dx * dx + dy * dy;
    double t = L > 0 ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / L : 0; t = std::clamp(t, 0.0, 1.0); return std::hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy); };
  return std::min({seg(p, a, b), seg(p, b, c), seg(p, c, a)});
}
// the mask of triangles (points in cells, all ≥ 0), every cell whose middle is within `grow` of one; w × h cells
Mask raster(const std::vector<P>& pts, const std::vector<std::array<int, 3>>& tris, int w, int h, double grow, bool symmetric) {
  std::vector<uint8_t> bm(size_t(w) * size_t(h), 0);
  for (auto& t : tris) {
    const P &a = pts[size_t(t[0])], &b = pts[size_t(t[1])], &c = pts[size_t(t[2])];
    const int x0 = std::max(0, int(std::floor(std::min({a[0], b[0], c[0]}) - grow))), x1 = std::min(w - 1, int(std::ceil(std::max({a[0], b[0], c[0]}) + grow)));
    const int y0 = std::max(0, int(std::floor(std::min({a[1], b[1], c[1]}) - grow))), y1 = std::min(h - 1, int(std::ceil(std::max({a[1], b[1], c[1]}) + grow)));
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) { uint8_t& q = bm[size_t(y) * size_t(w) + size_t(x)]; if (!q && triDist({x + 0.5, y + 0.5}, a, b, c) <= grow) q = 1; }
  }
  if (symmetric) for (int y = 0; y < h; y++) for (int x = 0; x < w / 2; x++) { uint8_t& l = bm[size_t(y) * size_t(w) + size_t(x)]; uint8_t& r = bm[size_t(y) * size_t(w) + size_t(w - 1 - x)]; l = r = l | r; }
  Mask m; m.w = w; m.h = h;
  for (int y = 0; y < h; y++) for (int x = 0; x < w;) { if (!bm[size_t(y) * size_t(w) + size_t(x)]) { x++; continue; } int e = x; while (e < w && bm[size_t(y) * size_t(w) + size_t(e)]) e++; m.runs.push_back({y, x, e}); m.area += e - x; x = e; }
  // (the runs: the widest rows first — a clash is found sooner)
  std::stable_sort(m.runs.begin(), m.runs.end(), [](const Run& a, const Run& b) { return a.x1 - a.x0 > b.x1 - b.x0; });
  return m;
}
}  // namespace

PackResult packShapes(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const PackOpts& o) {
  PackResult R; if (!has(m)) return R;
  const Points Pt = points(m, T); const Shells S = shells(m, T, Pt);
  std::vector<char> want(size_t(S.count), faces.empty() ? 1 : 0); for (int f : faces) if (f >= 0 && f < T.nf) want[size_t(S.of[size_t(f)])] = 1;
  // the texture's space: texel-square, its longer side 1
  const double M = std::max(o.w, o.h), Dw = o.w / M, Dh = o.h / M;
  auto toT = [&](const UV& q) { return P{q[0] * Dw, q[1] * Dh}; };
  struct Sh { int s = -1; std::vector<int> corners; std::vector<std::array<int, 3>> tris; std::vector<P> X; double area = 0; char kind = 'f'; int mate = -1; double mid = 0; double x0 = 0, y0 = 0, x1 = 0, y1 = 0; int px = 0, py = 0; bool turn = false, onAxis = false, split = false; Mask mk[2]; bool lock = false, noTurn = false; double mult = 1; int group = 0; Sh* lead = nullptr; };   // (lock / noTurn / mult / group: its own settings; lead: the shell it lies on — a stack, moved as that one)
  std::vector<Sh> sh(size_t(S.count)); std::vector<int> local(m.cornerUV.size(), -1);
  for (int f = 0; f < T.nf; f++) { const int s = S.of[size_t(f)]; if (!want[size_t(s)]) continue; Sh& h = sh[size_t(s)]; h.s = s;
    if (o.attr && size_t(f) < o.attr->size()) { const ShellAttr& a = (*o.attr)[size_t(f)]; h.lock = h.lock || a.lock; h.noTurn = h.noTurn || a.noTurn; if (a.scale > 0 && a.scale != 1) h.mult = a.scale; if (a.group) h.group = a.group; }
    const int a = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
    for (int k = a; k < e; k++) { local[size_t(k)] = int(h.corners.size()); h.corners.push_back(k); h.X.push_back(toT(m.cornerUV[size_t(k)])); }
    for (int k = a + 1; k + 1 < e; k++) h.tris.push_back({local[size_t(a)], local[size_t(k)], local[size_t(k + 1)]});
    double ar = 0; for (int k = a; k < e; k++) { const P p = toT(m.cornerUV[size_t(k)]), q = toT(m.cornerUV[size_t(k + 1 < e ? k + 1 : a)]); ar += p[0] * q[1] - q[0] * p[1]; } h.area += std::abs(ar) / 2; }
  std::vector<Sh*> L; for (auto& h : sh) if (h.s >= 0) { h.x0 = h.y0 = 1e300; h.x1 = h.y1 = -1e300; for (auto& p : h.X) { h.x0 = std::min(h.x0, p[0]); h.y0 = std::min(h.y0, p[1]); h.x1 = std::max(h.x1, p[0]); h.y1 = std::max(h.y1, p[1]); } L.push_back(&h); }
  // (its own size: grown / shrunk about its middle by its scale — packed so; locked ones: where they lie, in the way)
  std::vector<Sh*> locked; { std::vector<Sh*> keep; for (Sh* h : L) { if (h->lock) { locked.push_back(h); continue; }
      if (h->mult != 1) { const double cx = (h->x0 + h->x1) / 2, cy = (h->y0 + h->y1) / 2; for (auto& p : h->X) p = {cx + (p[0] - cx) * h->mult, cy + (p[1] - cy) * h->mult};
        h->x0 = cx + (h->x0 - cx) * h->mult; h->x1 = cx + (h->x1 - cx) * h->mult; h->y0 = cy + (h->y0 - cy) * h->mult; h->y1 = cy + (h->y1 - cy) * h->mult; h->area *= h->mult * h->mult; }
      keep.push_back(h); } L.swap(keep); }
  if (L.empty()) return R;
  // the symmetry: each shell's mirror — itself (across the plane), its pair, or none; and which side of the UV +X lies on
  bool plusRight = true;
  if (o.mc && o.mc->size() == m.cornerUV.size()) {
    std::vector<int> to(size_t(S.count), -2); std::vector<double> sx(size_t(S.count), 0);
    for (size_t k = 0; k < o.mc->size(); k++) { const int s = S.of[size_t(T.cf[k])], mk = (*o.mc)[k]; const int g = mk >= 0 ? S.of[size_t(T.cf[size_t(mk)])] : -1; int& x = to[size_t(s)]; if (x == -2) x = g; else if (x != g) x = -1; sx[size_t(s)] += m.pos[size_t(T.fv[k])].x; }
    double side = 0;
    for (Sh* h : L) { const int g = to[size_t(h->s)];
      if (g == h->s) { h->kind = 'c'; double mid = 0; int n = 0; for (int k : h->corners) { const int p = (*o.mc)[size_t(k)]; if (p >= 0) { mid += (toT(m.cornerUV[size_t(k)])[0] + toT(m.cornerUV[size_t(p)])[0]) / 2; n++; } } h->mid = n ? mid / n : (h->x0 + h->x1) / 2;
        for (int k : h->corners) { const double x = m.pos[size_t(T.fv[size_t(k)])].x; if (std::abs(x) > 0) side += (toT(m.cornerUV[size_t(k)])[0] - h->mid) * x; } }
      else if (g >= 0 && to[size_t(g)] == h->s && want[size_t(g)] && !sh[size_t(g)].lock && sh[size_t(g)].mult == h->mult) {   // (a pair: both free to move, one size)
        h->kind = sx[size_t(h->s)] >= sx[size_t(g)] ? 'l' : 'm'; h->mate = g; } }
    plusRight = side >= 0;
  }
  // (a self-symmetric shell grown by its scale: its middle line moved with it)
  for (Sh* h : L) if (h->kind == 'c' && h->mult != 1) { const double cx = (h->x0 + h->x1) / 2; h->mid = cx + (h->mid - cx) * h->mult; }
  // stacks: alike free shells lying one on another (their boxes 80% over each other) — packed as one, as the first moves
  if (o.stacks) for (size_t i = 0; i < L.size(); i++) { Sh* a = L[i]; if (a->kind != 'f' || a->lead) continue;
    for (size_t j = i + 1; j < L.size(); j++) { Sh* b = L[j]; if (b->kind != 'f' || b->lead || b->corners.size() != a->corners.size() || b->mult != a->mult) continue;
      if (std::abs(a->area - b->area) > 0.03 * std::max(a->area, b->area)) continue;
      const double iw = std::min(a->x1, b->x1) - std::max(a->x0, b->x0), ih = std::min(a->y1, b->y1) - std::max(a->y0, b->y0); if (iw <= 0 || ih <= 0) continue;
      const double ia = iw * ih, ua = (a->x1 - a->x0) * (a->y1 - a->y0) + (b->x1 - b->x0) * (b->y1 - b->y0) - ia; if (ua > 0 && ia / ua > 0.8) { b->lead = a; b->kind = 'S'; } } }
  const bool sym = std::any_of(L.begin(), L.end(), [](const Sh* h) { return h->kind != 'f'; });
  // the grid: cells about a quarter of the padding (a little finer for a small padding), 160…640 along the longer side
  const int N = std::clamp(int(std::lround(M / std::max(1.0, o.pad / 4))), 160, 640);
  const int GW = std::max(8, int(std::lround(N * Dw))), GH = std::max(8, int(std::lround(N * Dh))); const double cell = 1.0 / N;
  int ac = int(std::lround(o.axis * Dw * N)); ac = std::clamp(ac, 1, GW - 1); R.axis = ac * cell / Dw;
  const double grow = o.pad / 2 / (M / N) + 0.7072;            // (half the padding in cells, and half a cell's diagonal: no corner missed)
  const int Rg = int(std::ceil(grow)) + 1;
  std::vector<Sh*> order; for (Sh* h : L) if (h->kind != 'S') order.push_back(h); std::sort(order.begin(), order.end(), [](const Sh* a, const Sh* b) { return a->area > b->area; });
  std::vector<Mask> lockMk; for (Sh* h : locked) { std::vector<P> q(h->X.size()); for (size_t i = 0; i < q.size(); i++) q[i] = {h->X[i][0] * N, h->X[i][1] * N}; lockMk.push_back(raster(q, h->tris, GW, GH, grow, false)); }   // (the locked: where they lie, in the way)
  std::vector<Sh*> order2 = order; std::stable_sort(order2.begin(), order2.end(), [](const Sh* a, const Sh* b) { return a->area * (a->kind == 'l' ? 2 : 1) > b->area * (b->kind == 'l' ? 2 : 1); });   // (a pair takes twice its room)
  auto tryScale = [&](double s) -> bool {
    Grid G(GW, GH); for (const Mask& mk : lockMk) G.put(mk, 0, 0);
    std::map<int, std::array<double, 2>> anchor;                // (a group: where its first shell went — the rest as near it as they fit)
    // (the edges: half the padding off them — the masks' growth does it, the grid's border none)
    auto place = [&](Sh* h, int xmin, int xmax, bool fixedX, int fx) {
      int best = -1, bx = 0, by = 0; const int mir = h->kind == 'l' && !h->split ? ac : -1;
      const auto ga = h->group ? anchor.find(h->group) : anchor.end();
      if (ga != anchor.end() && !fixedX) {                      // (a group's next: the free place nearest its first)
        double bd = 1e300;
        for (int t = 0; t < (h->kind == 'c' || !o.rotate || h->noTurn ? 1 : 2); t++) { const Mask& mk = h->mk[t]; if (mk.w > xmax - xmin || mk.h > GH) continue;
          for (int y = 0; y + mk.h <= GH; y++) for (int x = xmin; x + mk.w <= xmax;) { const int nx = G.clash(mk, x, y, mir); if (nx >= 0) { x = std::max(nx, x + 1); continue; }
            const double d = std::hypot(x + mk.w / 2.0 - ga->second[0], y + mk.h / 2.0 - ga->second[1]); if (d < bd) { bd = d; best = t; bx = x; by = y; } x++; } }
      } else
      for (int t = 0; t < (h->kind == 'c' || !o.rotate || h->noTurn ? 1 : 2); t++) {
        const Mask& mk = h->mk[t]; if (mk.w > xmax - xmin || mk.h > GH) continue;
        for (int y = 0; y + mk.h <= GH && (best < 0 || y <= by); y++) {
          if (fixedX) { if (fx < xmin || fx + mk.w > xmax) break; if (G.clash(mk, fx, y) < 0) { if (best < 0 || y < by) { best = t; bx = fx; by = y; } break; } continue; }
          for (int x = xmin; x + mk.w <= xmax;) { const int nx = G.clash(mk, x, y, mir); if (nx < 0) { if (best < 0 || y < by || (y == by && x < bx)) { best = t; bx = x; by = y; } break; } x = std::max(nx, x + 1); }
          if (best >= 0 && by == y) break;
        }
      }
      if (best < 0) return false;
      h->turn = best == 1; h->px = bx; h->py = by; G.put(h->mk[best], bx, by, mir);
      if (h->group && !anchor.count(h->group)) anchor[h->group] = {bx + h->mk[best].w / 2.0, by + h->mk[best].h / 2.0};
      return true;
    };
    for (Sh* h : order) {                                       // (the masks at this scale: as it lies, and turned)
      h->split = false;                                           // (a pair too wide for its side: both its shells anywhere — set below)
      for (int t = 0; t < (h->kind == 'c' || !o.rotate || h->noTurn ? 1 : 2); t++) {
        std::vector<P> q(h->X.size());
        if (h->kind == 'c') { double hw = 0; for (auto& p : h->X) hw = std::max(hw, std::abs(p[0] - h->mid)); const int half = int(std::ceil(hw * s / cell)) + Rg;
          for (size_t i = 0; i < q.size(); i++) q[i] = {(h->X[i][0] - h->mid) * s / cell + half, (h->X[i][1] - h->y0) * s / cell + Rg};
          h->mk[0] = raster(q, h->tris, 2 * half, int(std::ceil((h->y1 - h->y0) * s / cell)) + 2 * Rg + 1, grow, true); continue; }
        const double w = (h->x1 - h->x0) * s / cell, hh = (h->y1 - h->y0) * s / cell;
        for (size_t i = 0; i < q.size(); i++) { const double x = (h->X[i][0] - h->x0) * s / cell, y = (h->X[i][1] - h->y0) * s / cell; q[i] = t ? P{hh - y + Rg, x + Rg} : P{x + Rg, y + Rg}; }
        h->mk[t] = raster(q, h->tris, int(std::ceil(t ? hh : w)) + 2 * Rg + 1, int(std::ceil(t ? w : hh)) + 2 * Rg + 1, grow, false);
      }
    }
    // the shells across the plane onto the axis first (the biggest first); then everything by size — a pair (its mirror image's
    // room too) on its side, the rest and those with no room on the axis anywhere (upright)
    for (Sh* h : order) if (h->kind == 'c') h->onAxis = place(h, 0, GW, true, ac - h->mk[0].w / 2);
    for (Sh* h : order2) { if (h->kind == 'm' || (h->kind == 'c' && h->onAxis)) continue;
      bool ok;
      if (h->kind == 'l') { ok = place(h, plusRight ? ac : 0, plusRight ? GW : ac, false, 0);
        if (!ok) { h->split = true; ok = place(h, 0, GW, false, 0) && place(&sh[size_t(h->mate)], 0, GW, false, 0); } }   // (no room on its side: both anywhere, still mirror images)
      else ok = place(h, 0, GW, false, 0);
      if (!ok) return false; }
    return true;
  };
  double area = 0; for (Sh* h : L) if (h->kind != 'S') area += h->area; if (!(area > 0)) return R;
  double lockA = 0; for (Sh* h : locked) lockA += h->area;   // (the locked ones' room: not there to fill)
  double hi = std::sqrt(std::max(1e-9, Dw * Dh - lockA) / area), lo = 0;   // (all of the tile filled: never more)
  for (Sh* h : L) { const double e = std::max(h->x1 - h->x0, h->y1 - h->y0); if (e > 0) hi = std::min(hi, std::max(Dw, Dh) / e); }
  double good = 0; lo = hi * 0.25; if (tryScale(lo)) good = lo; else { lo = 0; }
  for (int i = 0; i < 11; i++) { const double mid = (lo + hi) / 2; const bool ok = tryScale(mid); if (std::getenv("UVPACKDBG")) std::fprintf(stderr, "scale %.4f %s (hi %.4f)\n", mid, ok ? "fits" : "no", hi); if (false) std::fprintf(stderr, "%f %s", mid, ok ? "fits" : "no"); if (ok) { good = mid; lo = mid; } else hi = mid; }
  if (!(good > 0) || !tryScale(good)) return R;                // (placed at the scale found: the places kept in the shells)
  // the UV: each shell from its place (turned as it was placed), back from the texture's space
  for (Sh* h : L) {
    if (h->kind == 'S') continue;                                 // (a stack's: as its first, below)
    if (h->kind == 'm' && !sh[size_t(h->mate)].split) continue;   // (a follower: its leader's mirror image — unless the pair was split)
    for (size_t i = 0; i < h->X.size(); i++) {
      double x, y;
      if (h->kind == 'c') { const int half = h->mk[0].w / 2; x = (h->px + half) * cell + (h->X[i][0] - h->mid) * good; y = (h->py + Rg) * cell + (h->X[i][1] - h->y0) * good; }
      else { const double lx = (h->X[i][0] - h->x0) * good, ly = (h->X[i][1] - h->y0) * good, hh = (h->y1 - h->y0) * good;
        x = (h->px + Rg) * cell + (h->turn ? hh - ly : lx); y = (h->py + Rg) * cell + (h->turn ? lx : ly); }
      m.cornerUV[size_t(h->corners[i])] = {float(x / Dw), float(y / Dh)};
    }
    R.shells++; if (h->kind == 'c') { R.centred++; if (!h->onAxis) R.offAxis++; } else if (h->kind == 'l') { R.pairs++; if (h->split) R.split++; } else if (h->kind == 'f') R.free++;
  }
  // the stacks' others: moved, turned as their first was (their points in its frame)
  for (Sh* h : L) if (h->kind == 'S' && h->lead) { const Sh* a = h->lead; if (a->kind != 'f') continue;
    for (size_t i = 0; i < h->X.size(); i++) { const double lx = (h->X[i][0] - a->x0) * good, ly = (h->X[i][1] - a->y0) * good, hh = (a->y1 - a->y0) * good;
      const double x = (a->px + Rg) * cell + (a->turn ? hh - ly : lx), y = (a->py + Rg) * cell + (a->turn ? lx : ly); m.cornerUV[size_t(h->corners[i])] = {float(x / Dw), float(y / Dh)}; }
    R.shells++; R.stacked++; }
  R.locked = int(locked.size());
  for (Sh* h : L) if (h->kind == 'm' && !sh[size_t(h->mate)].split) { for (int k : h->corners) { const int p = (*o.mc)[size_t(k)]; if (p < 0) continue; const UV& q = m.cornerUV[size_t(p)]; m.cornerUV[size_t(k)] = {float(2 * R.axis - q[0]), q[1]}; } R.shells++; }
  R.scale = good; R.ok = true; (void)sym;   // (R.axis: where the axis fell — on a cell's edge)
  double used = 0; for (Sh* h : L) if (h->kind != 'S') used += h->area * good * good; R.fill = used / (Dw * Dh);
  return R;
}

}  // namespace hm::uv
