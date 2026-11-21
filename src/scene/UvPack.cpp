// Pack by shape: each shell rasterised into the texture's grid (its triangles, grown by half the padding), the masks
// kept as runs per row; placed into an occupancy grid of bit rows — the lowest place, then the leftmost, where no run
// meets a taken bit (a clash skips past the bit that clashed). The largest scale that fits: a binary search.
// Symmetric: the grid's column `ac` is the axis — the shells across the plane (their masks made symmetric) stacked on
// it, a pair's leader placed on its side with its mirror image taken too, then the rest anywhere.
#include <random>
#include <atomic>
#include <thread>
#include <deque>
#include <set>
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
    if (mirrorAt >= 0) for (const Run& r : m.runs) { const int a = 2 * mirrorAt - (x + r.x1), b = 2 * mirrorAt - (x + r.x0); if (a < 0 || b > W || firstSet(y + r.r, a, b) >= 0) return x + 1; }   // (its mirror image's place too — inside the grid on both sides: an axis right of the middle put it past the row)
    return -1;
  }
  void put(const Mask& m, int x, int y, int mirrorAt = -1) {
    for (const Run& r : m.runs) { set(y + r.r, x + r.x0, x + r.x1); if (mirrorAt >= 0) set(y + r.r, 2 * mirrorAt - (x + r.x1), 2 * mirrorAt - (x + r.x0)); }
  }
};

// the mask of triangles (points in cells, all ≥ 0), every cell whose middle is within `grow` of one; w × h cells
Mask raster(const std::vector<P>& pts, const std::vector<std::array<int, 3>>& tris, int w, int h, double grow, bool symmetric, const std::vector<uint8_t>* rim = nullptr) {   // (rim: per triangle, which of its three sides (bits 0…2) may be the shell's outline — none given: all)
  std::vector<uint8_t> bm(size_t(w) * size_t(h), 0);
  // (the inside: each row's middle line through a triangle — its cells; then the rim: the cells within `grow` of a side on the shell's
  //  outline (by the mesh: a UV border, a fold — never by two sides lying on each other: a shell folded in half had no rim at all, 2026-11-12). The same cells as testing every cell round every triangle against it — a cell near a shared side but in no triangle is
  //  nearer still to the rim — at a fraction of the work: on the Final pack's fine grid the masks were most of its 20 s, 2026-11-12)
  struct Side { P a, b; }; std::vector<Side> sides; sides.reserve(tris.size()); size_t ti = 0;
  for (auto& t : tris) {
    const P* q[3] = {&pts[size_t(t[0])], &pts[size_t(t[1])], &pts[size_t(t[2])]};
    const double lo = std::min({(*q[0])[1], (*q[1])[1], (*q[2])[1]}), hi = std::max({(*q[0])[1], (*q[1])[1], (*q[2])[1]});
    const int y0 = std::max(0, int(std::floor(lo - 0.5))), y1 = std::min(h - 1, int(std::ceil(hi - 0.5)));
    for (int y = y0; y <= y1; y++) { const double yc = y + 0.5; double a = 1e300, b = -1e300;
      for (int i = 0; i < 3; i++) { const P &u = *q[i], &v = *q[(i + 1) % 3]; if ((u[1] <= yc) == (v[1] <= yc)) continue; const double x = u[0] + (v[0] - u[0]) * (yc - u[1]) / (v[1] - u[1]); a = std::min(a, x); b = std::max(b, x); }
      if (a > b) continue; const int x0 = std::max(0, int(std::ceil(a - 0.5))), x1 = std::min(w - 1, int(std::floor(b - 0.5)));
      if (x0 <= x1) std::fill(bm.begin() + (size_t(y) * size_t(w) + size_t(x0)), bm.begin() + (size_t(y) * size_t(w) + size_t(x1) + 1), uint8_t(1)); }
    const uint8_t rb = rim && ti < rim->size() ? (*rim)[ti] : uint8_t(7); ti++;
    for (int i = 0; i < 3; i++) if (rb >> i & 1) sides.push_back(Side{*q[i], *q[(i + 1) % 3]});
  }
  for (size_t i = 0; i < sides.size(); i++) {
    { const P &u = sides[i].a, &v = sides[i].b; const double dx = v[0] - u[0], dy = v[1] - u[1], L = dx * dx + dy * dy;
      const int y0 = std::max(0, int(std::floor(std::min(u[1], v[1]) - grow - 0.5))), y1 = std::min(h - 1, int(std::ceil(std::max(u[1], v[1]) + grow - 0.5)));
      for (int y = y0; y <= y1; y++) { const double yc = y + 0.5; double xa, xb;                 // (the part of the side within `grow` of this row: only the cells beside it tried)
        if (std::abs(dy) < 1e-12) { xa = std::min(u[0], v[0]); xb = std::max(u[0], v[0]); }
        else { double t0 = (yc - grow - u[1]) / dy, t1 = (yc + grow - u[1]) / dy; if (t0 > t1) std::swap(t0, t1); t0 = std::max(0.0, t0); t1 = std::min(1.0, t1); if (t0 > t1) continue;
          xa = u[0] + dx * t0; xb = u[0] + dx * t1; if (xa > xb) std::swap(xa, xb); }
        const int x0 = std::max(0, int(std::floor(xa - grow - 0.5))), x1 = std::min(w - 1, int(std::ceil(xb + grow - 0.5)));
        for (int x = x0; x <= x1; x++) { uint8_t& c = bm[size_t(y) * size_t(w) + size_t(x)]; if (c) continue; const double px = x + 0.5 - u[0], py = yc - u[1];
          const double t = L > 0 ? std::clamp((px * dx + py * dy) / L, 0.0, 1.0) : 0.0; if (std::hypot(px - t * dx, py - t * dy) <= grow) c = 1; } } }
  }
  if (symmetric) for (int y = 0; y < h; y++) for (int x = 0; x < w / 2; x++) { uint8_t& l = bm[size_t(y) * size_t(w) + size_t(x)]; uint8_t& r = bm[size_t(y) * size_t(w) + size_t(w - 1 - x)]; l = r = l | r; }
  Mask m; m.w = w; m.h = h;
  for (int y = 0; y < h; y++) for (int x = 0; x < w;) { if (!bm[size_t(y) * size_t(w) + size_t(x)]) { x++; continue; } int e = x; while (e < w && bm[size_t(y) * size_t(w) + size_t(e)]) e++; m.runs.push_back({y, x, e}); m.area += e - x; x = e; }
  // (the runs: the widest rows first — a clash is found sooner)
  std::stable_sort(m.runs.begin(), m.runs.end(), [](const Run& a, const Run& b) { return a.x1 - a.x0 > b.x1 - b.x0; });
  return m;
}
}  // namespace

// Margin: the pack done in the tile's inner frame (the margin off each edge) — the UV taken into the frame's own 0…1, packed there by the frame's
// size in texels (the padding stays in px), and brought back; what the pack did not move is left exactly as it was. Before, Pack left only half the
// padding off the tile's edges, whatever the Margin: shells lay over the dashed frame (2026-11-12)
static PackResult packInTile(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const PackOpts& o);
PackResult packShapes(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const PackOpts& o) {
  const double mg = std::clamp(o.margin, 0.0, 0.45); if (!(mg > 0) || !has(m)) return packInTile(m, T, faces, o);
  const double k = 1 - 2 * mg; const std::vector<UV> was = m.cornerUV;
  for (auto& q : m.cornerUV) q = {float((q[0] - mg) / k), float((q[1] - mg) / k)};
  const std::vector<UV> in = m.cornerUV;
  PackOpts o2 = o; o2.margin = 0; o2.w = std::max(16, int(std::lround(o.w * k))); o2.h = std::max(16, int(std::lround(o.h * k))); o2.axis = (o.axis - mg) / k; o2.keepDensity = o.keepDensity / (k * k);
  PackResult R = packInTile(m, T, faces, o2);
  for (size_t i = 0; i < m.cornerUV.size(); i++) { const UV q = m.cornerUV[i]; m.cornerUV[i] = !R.ok || (q[0] == in[i][0] && q[1] == in[i][1]) ? was[i] : UV{float(mg + q[0] * k), float(mg + q[1] * k)}; }
  R.axis = mg + R.axis * k; R.fill *= k * k;                   // (how full the whole tile is)
  return R;
}
static PackResult packInTile(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const PackOpts& o) {
  PackResult R; if (!has(m)) return R;
  // (the Final pack with pack groups: the groups' inner paddings come from how much each block is scaled down in the tile — known only after a
  //  pack. That first pack is done the quick way, the Final one once, with the paddings known: before, the whole Final pack ran twice, 2026-11-12)
  if (o.final && o.attr && !o.gpad && !o.gpadOut && std::any_of(o.attr->begin(), o.attr->end(), [](const ShellAttr& a) { return a.group != 0; })) {
    std::map<int, double> need; const std::vector<UV> uv0 = m.cornerUV; PackOpts q = o; q.final = false; q.passes = 1; q.gpadOut = &need; packShapes(m, T, faces, q); m.cornerUV = uv0;
    if (!need.empty()) { PackOpts o3 = o; o3.gpad = &need; const PackResult R2 = packShapes(m, T, faces, o3); if (R2.ok) return R2; m.cornerUV = uv0; } }
  const std::vector<UV> uvStart = o.attr && !o.gpad ? m.cornerUV : std::vector<UV>();   // (for the second pass of the groups, below)
  std::map<int, std::pair<double, const void*>> gInfo;           // (each group's inner scale and its block — the block's final size known at the end)
  const Points Pt = points(m, T); const Shells S = shells(m, T, Pt);
  std::vector<char> want(size_t(S.count), faces.empty() ? 1 : 0); for (int f : faces) if (f >= 0 && f < T.nf) want[size_t(S.of[size_t(f)])] = 1;
  // the texture's space: texel-square, its longer side 1
  const double M = std::max(o.w, o.h), Dw = o.w / M, Dh = o.h / M;
  auto toT = [&](const UV& q) { return P{q[0] * Dw, q[1] * Dh}; };
  struct Sh { int s = -1; std::vector<int> corners; std::vector<std::array<int, 3>> tris; std::vector<P> X; double area = 0, a3 = 0; char kind = 'f'; int mate = -1; double mid = 0; double x0 = 0, y0 = 0, x1 = 0, y1 = 0; int px = 0, py = 0; int turn = 0; bool onAxis = false, split = false; Mask mk[7]; bool symTag = false; double dw[7] = {}, dh[7] = {}, ox[7] = {}, oy[7] = {}; bool lock = false, noTurn = false, level = false; int rotMode = 0; double mult = 1; int group = 0; Sh* lead = nullptr; bool unit = false, ovl = false; int stack = 0; std::vector<int> subs; std::vector<uint8_t> trim; const std::vector<uint8_t>* parts() const { return trim.size() == tris.size() ? &trim : nullptr; } };   /* (trim: per triangle, its sides on the shell's outline) */   // (stack: its Overlap stack; subs: a unit's shells)   // (lock / noTurn / mult / group: its own settings; lead: the shell it lies on — a stack, moved as that one)
  std::vector<Sh> sh(size_t(S.count)); std::vector<int> local(m.cornerUV.size(), -1);
  std::vector<char> cont(size_t(T.ne), 0); for (int ed = 0; ed < T.ne; ed++) cont[size_t(ed)] = continuous(m, T, Pt, ed) ? 1 : 0;
  std::vector<signed char> fsg(size_t(T.nf), 2); auto fsign = [&](int g) { signed char& v = fsg[size_t(g)]; if (v == 2) { const double ar = faceArea(m, g); v = ar > 0 ? 1 : ar < 0 ? -1 : 0; } return int(v); };
  for (int f = 0; f < T.nf; f++) { const int s = S.of[size_t(f)]; if (!want[size_t(s)]) continue; Sh& h = sh[size_t(s)]; h.s = s;
    if (o.attr && size_t(f) < o.attr->size()) { const ShellAttr& a = (*o.attr)[size_t(f)]; h.lock = h.lock || a.lock; h.noTurn = h.noTurn || a.noTurn; h.level = h.level || a.level; h.symTag = h.symTag || a.sym; if (a.turn) h.rotMode = a.turn; if (a.scale > 0 && a.scale != 1) h.mult = a.scale; if (a.group) h.group = a.group; if (a.ovl) h.ovl = true; if (a.stack) h.stack = a.stack; }
    h.a3 += faceArea3(m, f);
    const int a = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
    for (int k = a; k < e; k++) { local[size_t(k)] = int(h.corners.size()); h.corners.push_back(k); h.X.push_back(toT(m.cornerUV[size_t(k)])); }
    // (a triangle's sides on the shell's outline: a polygon's edge that is a UV border, or one the UV folds over — its two polygons turned opposite ways,
    //  both on one side of it; a fan's inner side where its two triangles are turned opposite ways — a polygon folded on itself)
    auto sgn = [&](int i, int j, int l) { const UV &p = m.cornerUV[size_t(i)], &q = m.cornerUV[size_t(j)], &r = m.cornerUV[size_t(l)]; const double c = (double(q[0]) - p[0]) * (double(r[1]) - p[1]) - (double(r[0]) - p[0]) * (double(q[1]) - p[1]); return c > 0 ? 1 : c < 0 ? -1 : 0; };
    auto edgeRim = [&](int k, int ts) { const int ed = T.ce[size_t(k)]; if (!cont[size_t(ed)]) return true; const int g = T.ef0[size_t(ed)] == f ? T.ef1[size_t(ed)] : T.ef0[size_t(ed)]; return g < 0 || ts == 0 || fsign(g) != ts; };
    for (int k = a + 1; k + 1 < e; k++) { h.tris.push_back({local[size_t(a)], local[size_t(k)], local[size_t(k + 1)]}); const int ts = sgn(a, k, k + 1); uint8_t rb = 0;
      if (k == a + 1 ? edgeRim(a, ts) : (ts == 0 || sgn(a, k - 1, k) != ts)) rb |= 1;
      if (edgeRim(k, ts)) rb |= 2;
      if (k + 2 == e ? edgeRim(e - 1, ts) : (ts == 0 || sgn(a, k + 1, k + 2) != ts)) rb |= 4;
      h.trim.push_back(rb); }
    double ar = 0; for (int k = a; k < e; k++) { const P p = toT(m.cornerUV[size_t(k)]), q = toT(m.cornerUV[size_t(k + 1 < e ? k + 1 : a)]); ar += p[0] * q[1] - q[0] * p[1]; } h.area += std::abs(ar) / 2; }
  std::vector<Sh*> L; for (auto& h : sh) if (h.s >= 0) { h.x0 = h.y0 = 1e300; h.x1 = h.y1 = -1e300; for (auto& p : h.X) { h.x0 = std::min(h.x0, p[0]); h.y0 = std::min(h.y0, p[1]); h.x1 = std::max(h.x1, p[0]); h.y1 = std::max(h.y1, p[1]); } L.push_back(&h); }
  // Groups as units (as RizomUV): the shells of a group are packed together first, into a block of their own (the same Pack, inside); the block then goes into the tile as one rigid shell,
  // so the shells of a group lie side by side. With Symmetry on a group that holds each other's mirrors (or itself across the plane) is packed symmetrically inside its block — the block
  // takes any place, the symmetry is its own; groups that don't close under the mirror go with the rest (the whole symmetry about the tile's axis)
  std::deque<Sh> units;
  auto boxOf = [](Sh* cu) { cu->x0 = cu->y0 = 1e300; cu->x1 = cu->y1 = -1e300; for (auto& p : cu->X) { cu->x0 = std::min(cu->x0, p[0]); cu->y0 = std::min(cu->y0, p[1]); cu->x1 = std::max(cu->x1, p[0]); cu->y1 = std::max(cu->y1, p[1]); } };
  // Overlap stacks first: the shells of a stack lie on each other as they are — one rigid unit, the room of its biggest shell. It keeps its pack
  // group (a group may hold stacks and shells side by side: the stack goes into the group's block as one piece)
  if (o.attr) {
    std::map<int, std::vector<Sh*>> stm; for (Sh* h : L) if (h->stack && !h->lock) stm[h->stack].push_back(h);
    for (auto& [sid, mem] : stm) { if (mem.size() < 2) continue;
      Sh c; c.s = mem[0]->s; c.unit = true; c.kind = 'f'; c.ovl = true;
      for (Sh* h : mem) { if (h->noTurn) c.noTurn = true; if (h->level) c.level = true; if (!c.group && h->group) c.group = h->group; c.subs.push_back(h->s); c.area = std::max(c.area, h->area); c.a3 = std::max(c.a3, h->a3); if (h->mult != 1) c.mult = h->mult;
        const int off = int(c.corners.size()); for (size_t i = 0; i < h->corners.size(); i++) { c.corners.push_back(h->corners[i]); c.X.push_back(h->X[i]); } for (const auto& t : h->tris) { c.tris.push_back({t[0] + off, t[1] + off, t[2] + off}); } c.trim.insert(c.trim.end(), h->trim.begin(), h->trim.end()); }
      units.push_back(std::move(c)); Sh* cu = &units.back(); boxOf(cu);
      std::set<int> memS(cu->subs.begin(), cu->subs.end()); std::vector<Sh*> keep; for (Sh* h : L) if (!memS.count(h->s) || h->unit) keep.push_back(h); keep.push_back(cu); L.swap(keep); }
  }
  if (o.attr) {
    std::map<int, std::vector<Sh*>> gm; for (Sh* h : L) if (h->group && !h->lock) gm[h->group].push_back(h);
    std::vector<ShellAttr> attr2 = *o.attr; for (auto& a : attr2) a.group = 0;   // (inside a group's block: its stacks still stacks)
    const std::vector<UV> backup = m.cornerUV; const bool haveMc = o.mc && o.mc->size() == m.cornerUV.size(); bool anyUnit = false;
    for (auto& [gid, mem] : gm) { if (mem.size() < 2) continue;
      std::set<int> memS; for (Sh* h : mem) { memS.insert(h->s); for (int x : h->subs) memS.insert(x); } bool symIn = false; bool ovlG = false;
      for (Sh* h : mem) if (h->ovl && !h->unit) ovlG = true;   /* (an older Overlap group — saved as a group with "ovl": its shells stay as they lie on each other — no pack inside) */
      if (haveMc && !ovlG) { bool closed = true;                 // (closed under the mirror: every shell's mirror — the corners that have one — is itself or another of the group; the corners with none are let go)
        for (Sh* h : mem) { int to = -2; bool amb = false; for (int k : h->corners) { const int p = (*o.mc)[size_t(k)]; if (p < 0) continue; const int g2 = S.of[size_t(T.cf[size_t(p)])]; if (to == -2) to = g2; else if (to != g2) { amb = true; break; } }
          if (amb || to == -2 || !memS.count(to)) { closed = false; break; } }
        symIn = closed && (o.symAll || std::any_of(mem.begin(), mem.end(), [](const Sh* h) { return h->symTag; })); }                                        // (not closed: the group is still one block, packed freely inside)
      std::set<int> fset; for (Sh* h : mem) for (int k : h->corners) fset.insert(T.cf[size_t(k)]); const std::vector<int> fl(fset.begin(), fset.end());
      PackOpts o2 = o; o2.symAll = true; o2.final = false; o2.passes = 1; /* (a block is laid out once, quickly: it is scaled down into the tile after — the Final pack's fine grid on two small shells grown to a whole tile took 5 s, 2026-11-12) */ o2.attr = &attr2; o2.mc = symIn ? o.mc : nullptr; o2.axis = 0.5; o2.stacks = false; o2.gpad = nullptr;
      if (o.gpad) if (auto gp = o.gpad->find(gid); gp != o.gpad->end()) o2.pad = gp->second;   // (the second pass: the padding inside the block that comes out right once it is scaled)
      PackResult r2; if (ovlG) { r2.ok = true; r2.scale = 1; } else r2 = packShapes(m, T, fl, o2);
      if (!r2.ok || !(r2.scale > 0)) { for (size_t k = 0; k < backup.size(); k++) m.cornerUV[k] = backup[k]; continue; }
      Sh c; c.s = mem[0]->s; c.unit = true; c.kind = 'f'; c.group = 0; c.noTurn = symIn; for (Sh* h : mem) { if (h->noTurn) c.noTurn = true; if (h->level) c.level = true; c.a3 += h->a3; }
      for (Sh* h : mem) { const int off = int(c.corners.size()); for (int k : h->corners) { c.corners.push_back(k); const P p = toT(m.cornerUV[size_t(k)]); c.X.push_back({p[0] / r2.scale, p[1] / r2.scale}); }
        for (const auto& t : h->tris) c.tris.push_back({t[0] + off, t[1] + off, t[2] + off}); c.trim.insert(c.trim.end(), h->trim.begin(), h->trim.end()); }
      for (const auto& t : c.tris) { const P &a = c.X[size_t(t[0])], &b = c.X[size_t(t[1])], &d = c.X[size_t(t[2])]; c.area += std::abs((b[0] - a[0]) * (d[1] - a[1]) - (d[0] - a[0]) * (b[1] - a[1])) / 2; }
      { double own = 0, tri = 0; for (Sh* h : mem) { own += h->area; for (const auto& t : h->tris) { const P &a = h->X[size_t(t[0])], &b = h->X[size_t(t[1])], &d = h->X[size_t(t[2])]; tri += std::abs((b[0] - a[0]) * (d[1] - a[1]) - (d[0] - a[0]) * (b[1] - a[1])) / 2; } }
        if (tri > 0 && own > 0 && own < tri) c.area *= own / tri; }   // (stacks in it: their shells on each other counted once)
      if (ovlG) { double ma = 0, m3 = 0; for (Sh* h : mem) { ma = std::max(ma, h->area); m3 = std::max(m3, h->a3); } c.area = ma; c.a3 = m3; }   // (alike shells on each other: the room of one)
      units.push_back(std::move(c)); Sh* cu = &units.back(); cu->x0 = cu->y0 = 1e300; cu->x1 = cu->y1 = -1e300; for (auto& p : cu->X) { cu->x0 = std::min(cu->x0, p[0]); cu->y0 = std::min(cu->y0, p[1]); cu->x1 = std::max(cu->x1, p[0]); cu->y1 = std::max(cu->y1, p[1]); }
      if (!ovlG) gInfo[gid] = {r2.scale, cu};
      std::vector<Sh*> keep; for (Sh* h : L) if (!memS.count(h->s) || h->lock) keep.push_back(h); keep.push_back(cu); L.swap(keep); anyUnit = true; }
    (void)anyUnit;
  }
  // Equal texel density: each free shell sized so its UV area to its 3D area is the one the whole set has (the sizes of the shells against each other
  // by the model's, not by how they were unfolded); its own factor (Shell density ×) still on top
  if (o.equalize) { double sau = 0, sa3 = 0; for (Sh* h : L) if (!h->lock && h->a3 > 0 && h->area > 0) { sau += h->area; sa3 += h->a3; }
    if (sau > 0 && sa3 > 0) { const double k2 = sau / sa3; for (Sh* h : L) if (!h->lock && h->a3 > 0 && h->area > 0) h->mult *= std::sqrt(k2 * h->a3 / h->area); } }
  // (its own size: grown / shrunk about its middle by its scale — packed so; locked ones: where they lie, in the way)
  std::vector<Sh*> locked; { std::vector<Sh*> keep; for (Sh* h : L) { if (h->lock) { locked.push_back(h); continue; }
      if (h->mult != 1) { const double cx = (h->x0 + h->x1) / 2, cy = (h->y0 + h->y1) / 2; for (auto& p : h->X) p = {cx + (p[0] - cx) * h->mult, cy + (p[1] - cy) * h->mult};
        h->x0 = cx + (h->x0 - cx) * h->mult; h->x1 = cx + (h->x1 - cx) * h->mult; h->y0 = cy + (h->y0 - cy) * h->mult; h->y1 = cy + (h->y1 - cy) * h->mult; h->area *= h->mult * h->mult; }
      keep.push_back(h); } L.swap(keep); }
  if (L.empty()) return R;
  // the turns, as the shell's own points in its box (unit size, before the scale): 0 as it lies, 1 by 90°, 2 by 180°, 3…6 by 45° / 135° / 225° / 315° — the box of the
  // turned shell (dw × dh) and where it starts (ox, oy); a long strand turned slantwise takes the tile's diagonal, not its side
  for (Sh* h : L) { const double W = h->x1 - h->x0, H = h->y1 - h->y0;
    h->dw[0] = h->dw[2] = W; h->dh[0] = h->dh[2] = H; h->dw[1] = H; h->dh[1] = W;
    for (int t = 3; t < 7; t++) { const double a = (2 * (t - 3) + 1) * M_PI / 4, c = std::cos(a), s = std::sin(a); double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
      for (const P& p : h->X) { const double lx = p[0] - h->x0, ly = p[1] - h->y0, x = lx * c - ly * s, y = lx * s + ly * c; x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
      h->ox[t] = x0; h->oy[t] = y0; h->dw[t] = x1 - x0; h->dh[t] = y1 - y0; } }
  auto turned = [](const Sh* h, int t, double lx, double ly) -> P {   // (lx, ly: from the box's corner, unit size)
    const double W = h->x1 - h->x0, H = h->y1 - h->y0;
    if (t == 1) return {H - ly, lx}; if (t == 2) return {W - lx, H - ly};
    if (t >= 3) { const double a = (2 * (t - 3) + 1) * M_PI / 4; return {lx * std::cos(a) - ly * std::sin(a) - h->ox[t], lx * std::sin(a) + ly * std::cos(a) - h->oy[t]}; }
    return {lx, ly}; };
  // the symmetry: each shell's mirror — itself (across the plane), its pair, or none; and which side of the UV +X lies on
  bool plusRight = true;
  if (o.mc && o.mc->size() == m.cornerUV.size()) {
    std::vector<int> to(size_t(S.count), -2); std::vector<double> sx(size_t(S.count), 0);
    for (size_t k = 0; k < o.mc->size(); k++) { const int s = S.of[size_t(T.cf[k])], mk = (*o.mc)[k]; const int g = mk >= 0 ? S.of[size_t(T.cf[size_t(mk)])] : -1; int& x = to[size_t(s)]; if (x == -2) x = g; else if (x != g) x = -1; sx[size_t(s)] += m.pos[size_t(T.fv[k])].x; }
    double side = 0;
    // (the tag "Pack symmetrically": with any shell tagged, only the tagged ones and their mirror shells are packed symmetrically — the rest freely)
    const bool anyTag = !o.symAll || std::any_of(L.begin(), L.end(), [](const Sh* x) { return x->symTag; });   /* (Symmetry off: only the tagged — a tag on a shell not packed now (another UV set's) made the whole pack symmetric, 2026-11-12) */
    auto tagged = [&](const Sh* h) { if (!anyTag || h->symTag) return true; const int g2 = to[size_t(h->s)]; return g2 >= 0 && sh[size_t(g2)].s >= 0 && sh[size_t(g2)].symTag; };
    // (a pair only with a mate packed on its own: one taken into a group's block or an Overlap stack lies with that unit — no mirror place for it)
    std::set<int> alone; for (const Sh* x : L) if (!x->unit) alone.insert(x->s);
    for (Sh* h : L) { if (h->unit) continue; const int g = to[size_t(h->s)]; if (!tagged(h)) continue;
      if (g == h->s) { h->kind = 'c'; double mid = 0; int n = 0; for (int k : h->corners) { const int p = (*o.mc)[size_t(k)]; if (p >= 0) { mid += (toT(m.cornerUV[size_t(k)])[0] + toT(m.cornerUV[size_t(p)])[0]) / 2; n++; } } h->mid = n ? mid / n : (h->x0 + h->x1) / 2;
        for (int k : h->corners) { const double x = m.pos[size_t(T.fv[size_t(k)])].x; if (std::abs(x) > 0) side += (toT(m.cornerUV[size_t(k)])[0] - h->mid) * x; } }
      else if (g >= 0 && to[size_t(g)] == h->s && want[size_t(g)] && alone.count(g) && !sh[size_t(g)].lock && std::abs(sh[size_t(g)].mult - h->mult) <= 0.02 * h->mult) {   // (a pair: both free to move, one size)
        h->kind = sx[size_t(h->s)] >= sx[size_t(g)] ? 'l' : 'm'; h->mate = g; } }
    plusRight = side >= 0;
  }
  // (a self-symmetric shell grown by its scale: its middle line moved with it)
  for (Sh* h : L) if (h->kind == 'c' && h->mult != 1) { const double cx = (h->x0 + h->x1) / 2; h->mid = cx + (h->mid - cx) * h->mult; }
  // stacks: alike free shells lying one on another (their boxes 80% over each other) — packed as one, as the first moves
  if (o.stacks) for (size_t i = 0; i < L.size(); i++) { Sh* a = L[i]; if (a->kind != 'f' || a->lead) continue;
    for (size_t j = i + 1; j < L.size(); j++) { Sh* b = L[j]; if (b->kind != 'f' || b->lead || b->corners.size() != a->corners.size() || std::abs(b->mult - a->mult) > 0.02 * a->mult) continue;
      if (std::abs(a->area - b->area) > 0.03 * std::max(a->area, b->area)) continue;
      const double iw = std::min(a->x1, b->x1) - std::max(a->x0, b->x0), ih = std::min(a->y1, b->y1) - std::max(a->y0, b->y0); if (iw <= 0 || ih <= 0) continue;
      const double ia = iw * ih, ua = (a->x1 - a->x0) * (a->y1 - a->y0) + (b->x1 - b->x0) * (b->y1 - b->y0) - ia; if (ua > 0 && ia / ua > 0.8) { b->lead = a; b->kind = 'S'; } } }
  const bool sym = std::any_of(L.begin(), L.end(), [](const Sh* h) { return h->kind != 'f'; });
  // the grid: cells about a quarter of the padding (a little finer for a small padding), 160…640 along the longer side
  // (the Final pack ends on a finer grid — as fine as the texture, 2048 at most: the coarse grid's rounding adds to the padding)
  int N = 0, GW = 0, GH = 0, ac = 0, Rg = 0; double cell = 0, grow = 0; std::vector<Mask> lockMk;
  auto setGrid = [&](int n) { N = n; GW = std::max(8, int(std::lround(N * Dw))); GH = std::max(8, int(std::lround(N * Dh))); cell = 1.0 / N;
    ac = std::clamp(int(std::lround(o.axis * Dw * N)), 1, GW - 1); R.axis = ac * cell / Dw;
    grow = o.pad / 2 / (M / N) + 0.7072;                         // (half the padding in cells, and half a cell's diagonal: no corner missed)
    Rg = int(std::ceil(grow)) + 1;
    lockMk.clear(); for (Sh* h : locked) { std::vector<P> q(h->X.size()); for (size_t i = 0; i < q.size(); i++) q[i] = {h->X[i][0] * N, h->X[i][1] * N}; lockMk.push_back(raster(q, h->tris, GW, GH, grow, false, h->parts())); } };   // (the locked: where they lie, in the way)
  const int N0 = std::clamp(int(std::lround(M / std::max(1.0, o.pad / 4))), 160, 640); setGrid(N0);
  std::vector<Sh*> order; for (Sh* h : L) if (h->kind != 'S') order.push_back(h); std::sort(order.begin(), order.end(), [](const Sh* a, const Sh* b) { return a->area > b->area; });
  int rule = 0; bool upright = false;                            // (the Final pack's ways: rule 1 — the free place nearest the tile's corner (x + y) instead of the lowest; upright — no slanted turns)
  std::vector<Sh*> order2 = order; std::stable_sort(order2.begin(), order2.end(), [](const Sh* a, const Sh* b) { return a->area * (a->kind == 'l' ? 2 : 1) > b->area * (b->kind == 'l' ? 2 : 1); });   // (a pair takes twice its room)
  // (a group: its shells one after another, straight after its biggest — else the others take the places round the first before its own come to be placed, and the group lies apart)
  { std::vector<Sh*> o2; std::set<int> done; for (Sh* h : order2) { if (h->group) { if (done.count(h->group)) continue; done.insert(h->group); o2.push_back(h); for (Sh* g : order2) if (g != h && g->group == h->group) o2.push_back(g); } else o2.push_back(h); } order2.swap(o2); }
  // the turns a shell may be packed in: 0 as it lies, 1 by 90°, 2 by 180° — none for a self-symmetric one, with Turn off, or a shell that must keep its way; its own mode: 90 (0 or 90°) / 180 (0 or 180°)
  auto turnsRaw = [&](const Sh* h) -> std::vector<int> { if (h->rotMode == 4 && h->kind != 'c') return {0, 1, 2, 3, 4, 5, 6}; if (h->kind == 'c' || h->noTurn || h->rotMode == 1) return {0}; if (h->rotMode == 3) return {0, 2}; if (h->rotMode == 2) return {0, 1};
    if (o.freeTurn) return h->kind == 'f' ? std::vector<int>{0, 1, 3, 4, 5, 6} : std::vector<int>{0, 1};   // (Free turn: 45° steps, slanted too)
    if (!o.rotate) return {0}; return {0, 1}; };   // (Rotate shells: by 90° only — slanted only with Free turn, 2026-11-11)
  auto turnsAll = [&](const Sh* h) -> std::vector<int> { auto v = turnsRaw(h); if (h->level) v.erase(std::remove_if(v.begin(), v.end(), [](int t) { return t >= 3; }), v.end()); return v; };   // (holding h / v constraints: never slanted)
  auto turns = [&](const Sh* h) -> std::vector<int> { auto v = turnsAll(h); if (upright) v.erase(std::remove_if(v.begin(), v.end(), [](int t) { return t >= 3; }), v.end()); return v; };
  auto tryScale = [&](double s) -> bool {
    Grid G(GW, GH); for (const Mask& mk : lockMk) G.put(mk, 0, 0);
    std::map<int, std::array<double, 2>> anchor;                // (a group: where its first shell went — the rest as near it as they fit)
    // (the edges: half the padding off them — the masks' growth does it, the grid's border none)
    auto place = [&](Sh* h, int xmin, int xmax, bool fixedX, int fx) {
      int best = -1, bx = 0, by = 0; const int mir = h->kind == 'l' && !h->split ? ac : -1;
      const auto ga = h->group ? anchor.find(h->group) : anchor.end();
      if (ga != anchor.end() && !fixedX) {                      // (a group's next: the free place nearest its first)
        double bd = 1e300;
        for (int t : turns(h)) { const Mask& mk = h->mk[t]; if (mk.w > xmax - xmin || mk.h > GH) continue;
          for (int y = 0; y + mk.h <= GH; y++) for (int x = xmin; x + mk.w <= xmax;) { const int nx = G.clash(mk, x, y, mir); if (nx >= 0) { x = std::max(nx, x + 1); continue; }
            const double d = std::hypot(x + mk.w / 2.0 - ga->second[0], y + mk.h / 2.0 - ga->second[1]); if (d < bd) { bd = d; best = t; bx = x; by = y; } x++; } }
      } else if (rule == 1 && !fixedX) {                         // (the Final pack's other way: of each row's first free place, the one nearest the tile's corner by x + y)
        double bs = 1e300;
        for (int t : turns(h)) { const Mask& mk = h->mk[t]; if (mk.w > xmax - xmin || mk.h > GH) continue;
          for (int y = 0; y + mk.h <= GH; y++) { if (y + (mk.w + mk.h) / 2.0 >= bs) break;
            for (int x = xmin; x + mk.w <= xmax;) { const int nx = G.clash(mk, x, y, mir); if (nx < 0) { const double sc = x + y + (mk.w + mk.h) / 2.0; if (sc < bs) { bs = sc; best = t; bx = x; by = y; } break; } x = std::max(nx, x + 1); } } }
      } else
      for (int t : turns(h)) {
        const Mask& mk = h->mk[t]; if (mk.w > xmax - xmin || mk.h > GH) continue;
        for (int y = 0; y + mk.h <= GH && (best < 0 || y <= by); y++) {
          if (fixedX) { if (fx < xmin || fx + mk.w > xmax) break; if (G.clash(mk, fx, y) < 0) { if (best < 0 || y < by) { best = t; bx = fx; by = y; } break; } continue; }
          for (int x = xmin; x + mk.w <= xmax;) { const int nx = G.clash(mk, x, y, mir); if (nx < 0) { if (best < 0 || y < by || (y == by && x < bx)) { best = t; bx = x; by = y; } break; } x = std::max(nx, x + 1); }
          if (best >= 0 && by == y) break;
        }
      }
      if (best < 0) return false;
      h->turn = best; h->px = bx; h->py = by; G.put(h->mk[best], bx, by, mir);
      if (h->group && !anchor.count(h->group)) anchor[h->group] = {bx + h->mk[best].w / 2.0, by + h->mk[best].h / 2.0};
      return true;
    };
    auto masksOf = [&](Sh* h) {                                 // (the masks at this scale: as it lies, and turned)
      h->split = false;                                           // (never set otherwise: a pair too wide for its side is never laid apart — the scale goes down till it fits)
      for (int t : turns(h)) {
        std::vector<P> q(h->X.size());
        if (h->kind == 'c') { double hw = 0; for (auto& p : h->X) hw = std::max(hw, std::abs(p[0] - h->mid)); const int half = int(std::ceil(hw * s / cell)) + Rg;
          for (size_t i = 0; i < q.size(); i++) q[i] = {(h->X[i][0] - h->mid) * s / cell + half, (h->X[i][1] - h->y0) * s / cell + Rg};
          h->mk[0] = raster(q, h->tris, 2 * half, int(std::ceil((h->y1 - h->y0) * s / cell)) + 2 * Rg + 1, grow, true, h->parts()); continue; }
        for (size_t i = 0; i < q.size(); i++) { const P r = turned(h, t, h->X[i][0] - h->x0, h->X[i][1] - h->y0); q[i] = {r[0] * s / cell + Rg, r[1] * s / cell + Rg}; }
        h->mk[t] = raster(q, h->tris, int(std::ceil(h->dw[t] * s / cell)) + 2 * Rg + 1, int(std::ceil(h->dh[t] * s / cell)) + 2 * Rg + 1, grow, false, h->parts());
      }
    };
    // (the Final pack: the shells' masks made in threads — on its fine grid they are most of the time)
    const unsigned nth = o.final ? std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), unsigned(std::max<size_t>(1, order.size() / 2))) : 1u;
    if (nth > 1) { std::atomic<size_t> nx{0}; auto work = [&] { for (size_t i; (i = nx++) < order.size();) masksOf(order[i]); }; std::vector<std::thread> th; for (unsigned i = 1; i < nth; i++) th.emplace_back(work); work(); for (auto& x : th) x.join(); }
    else for (Sh* h : order) masksOf(h);
    // the shells across the plane onto the axis first (the biggest first); then everything by size — a pair (its mirror image's
    // room too) on its side, the rest and those with no room on the axis anywhere (upright)
    for (Sh* h : order) if (h->kind == 'c') h->onAxis = place(h, 0, GW, true, ac - h->mk[0].w / 2);
    for (Sh* h : order2) { if (h->kind == 'm' || (h->kind == 'c' && h->onAxis)) continue;
      bool ok;
      if (h->kind == 'l') ok = place(h, plusRight ? ac : 0, plusRight ? GW : ac, false, 0);   // (a pair that has no room on its side: the scale goes down till it has — never laid apart, the pack stays symmetric)
      else ok = place(h, 0, GW, false, 0);
      if (!ok) return false; }
    return true;
  };
  double area = 0; for (Sh* h : L) if (h->kind != 'S') area += h->area; if (!(area > 0)) return R;
  double lockA = 0; for (Sh* h : locked) lockA += h->area;   // (the locked ones' room: not there to fill)
  double hi = std::sqrt(std::max(1e-9, Dw * Dh - lockA) / area), lo = 0;   // (all of the tile filled: never more)
  for (Sh* h : L) { double e = 1e300; for (int t : turns(h)) e = std::min(e, std::max(h->dw[t], h->dh[t])); if (h->kind == 'c') e = std::max(h->x1 - h->x0, h->y1 - h->y0); if (e > 0 && e < 1e299) hi = std::min(hi, std::max(Dw, Dh) / e); }
  // (Pack passes: the first try is the shells by area, biggest first; each more pass tries another order — by length, by width, then jittered by area — and the search of the scale finer; the best scale kept)
  auto regroup = [&](std::vector<Sh*>& v) { std::vector<Sh*> o2; std::set<int> done; for (Sh* h : v) { if (h->group) { if (done.count(h->group)) continue; done.insert(h->group); o2.push_back(h); for (Sh* g : v) if (g != h && g->group == h->group) o2.push_back(g); } else o2.push_back(h); } v.swap(o2); };
  auto search = [&]() { double g = 0, lo2 = hi * 0.25, hi2 = hi; if (tryScale(lo2)) g = lo2; else lo2 = 0;
    const int its = o.passes > 1 ? 14 : 11;
    for (int i = 0; i < its; i++) { const double mid = (lo2 + hi2) / 2; const bool ok = tryScale(mid); if (ok) { g = mid; lo2 = mid; } else hi2 = mid; }
    // (the fit is not monotonic in the scale — a bigger one may fit where a smaller did not: a few steps up from the found one probed)
    if (g > 0) { const int up = o.passes > 1 ? 12 : 6; for (int i = 1; i <= up; i++) { const double s2 = g * (1 + 0.006 * i); if (s2 > hi * 1.0001) break; if (tryScale(s2)) g = s2; } }
    return g; };
  // (a part packed among the rest, locked where it lies: at the texel density asked — the islands not grown to fill the holes; smaller only when they do not fit)
  double sKeep = 0; if (o.keepDensity < 0) { sKeep = 1; hi = std::min(hi, sKeep); }   /* (below 0: at the size they lie) */
  else if (o.keepDensity > 0) { double a = 0, a3 = 0; for (Sh* h : L) if (h->kind != 'S') { a += h->area; a3 += h->a3; } if (a > 0 && a3 > 0) { sKeep = std::sqrt(o.keepDensity * a3 / a); hi = std::min(hi, sKeep); } }
  double good = search(); std::vector<Sh*> bestOrder = order2;
  if (sKeep > 0 && good < sKeep && tryScale(sKeep)) good = sKeep;
  for (int pass = 1; pass < std::clamp(o.passes, 1, 20); pass++) {
    std::vector<Sh*> v = order; std::mt19937 rng(unsigned(pass) * 7919u + 13u); std::uniform_real_distribution<double> J(0.6, 1.4);
    std::vector<std::pair<double, Sh*>> key; for (Sh* h : v) { const double w = h->kind == 'l' ? 2 : 1; double k;
      if (pass == 1) k = std::max(h->dw[0], h->dh[0]) * w; else if (pass == 2) k = h->dw[0] * w; else k = h->area * w * J(rng); key.push_back({k, h}); }
    std::stable_sort(key.begin(), key.end(), [](const auto& a, const auto& b) { return a.first > b.first; }); v.clear(); for (auto& kv : key) v.push_back(kv.second); regroup(v);
    order2 = v; const double g = search(); if (g > good) { good = g; bestOrder = v; } }
  order2 = bestOrder;
  // the Final pack: other ways of placing tried against the best so far — the place nearest the corner (x + y) instead of the lowest; with
  // no slanted turns (a strand laid on the tile's diagonal blocks the rest); each in three orders. One that takes a scale a little bigger is
  // searched up. The shells are never turned more than Quick may (turned less in the upright way). Then the best scale refined on a grid as
  // fine as the texture (2048 cells at most): the coarse grid's rounding no longer eats into the room
  if (o.final && good > 0) {
    bool slanted = false; for (Sh* h : L) for (int t : turnsAll(h)) if (t >= 3) slanted = true;
    int bRule = 0; bool bUp = false;
    for (int ru : {0, 1}) for (int up : {0, 1}) { if (ru == 0 && up == 0) continue; if (up && !slanted) continue;
      for (int ord = 0; ord < 3; ord++) { rule = ru; upright = up;
        std::vector<std::pair<double, Sh*>> key; for (Sh* h : order) { const double w = h->kind == 'l' ? 2 : 1; key.push_back({(ord == 0 ? h->area : ord == 1 ? std::max(h->dw[0], h->dh[0]) : h->dw[0]) * w, h}); }
        std::stable_sort(key.begin(), key.end(), [](const auto& a, const auto& b) { return a.first > b.first; }); std::vector<Sh*> v; for (auto& kv : key) v.push_back(kv.second); regroup(v);
        order2 = v; const double t1 = good * 1.003; if (t1 > hi || !tryScale(t1)) continue;
        double g = t1, lo2 = t1, hi2 = std::min(hi, good * 1.15); for (int i = 0; i < 8; i++) { const double mid = (lo2 + hi2) / 2; if (tryScale(mid)) { g = mid; lo2 = mid; } else hi2 = mid; }
        if (g > good) { good = g; bestOrder = v; bRule = ru; bUp = up; } } }
    rule = bRule; upright = bUp; order2 = bestOrder;
    const int Nf = std::clamp(int(std::lround(M)), N0, 2048);
    if (Nf > N0) { setGrid(Nf);
      if (!tryScale(good)) setGrid(N0);                           // (not there on the fine grid — the coarse one's result stands)
      else { double g = good, lo2 = good, hi2 = std::min(hi, good * 1.08); for (int i = 0; i < 6; i++) { const double mid = (lo2 + hi2) / 2; if (tryScale(mid)) { g = mid; lo2 = mid; } else hi2 = mid; } good = g; } }
  }
  if (!(good > 0) || !tryScale(good)) return R;                // (placed at the scale found: the places kept in the shells)
  // the UV: each shell from its place (turned as it was placed), back from the texture's space
  for (Sh* h : L) {
    if (h->kind == 'S') continue;                                 // (a stack's: as its first, below)
    if (h->kind == 'm' && !sh[size_t(h->mate)].split) continue;   // (a follower: its leader's mirror image — unless the pair was split)
    for (size_t i = 0; i < h->X.size(); i++) {
      double x, y;
      if (h->kind == 'c') { const int half = h->mk[0].w / 2; x = (h->px + half) * cell + (h->X[i][0] - h->mid) * good; y = (h->py + Rg) * cell + (h->X[i][1] - h->y0) * good; }
      else { const P r = turned(h, h->turn, h->X[i][0] - h->x0, h->X[i][1] - h->y0); x = (h->px + Rg) * cell + r[0] * good; y = (h->py + Rg) * cell + r[1] * good; }
      m.cornerUV[size_t(h->corners[i])] = {float(x / Dw), float(y / Dh)};
    }
    R.shells++; if (h->kind == 'c') { R.centred++; if (!h->onAxis) R.offAxis++; } else if (h->kind == 'l') { R.pairs++; if (h->split) R.split++; } else if (h->kind == 'f') R.free++;
  }
  // the stacks' others: moved, turned as their first was (their points in its frame)
  for (Sh* h : L) if (h->kind == 'S' && h->lead) { const Sh* a = h->lead; if (a->kind != 'f') continue;
    for (size_t i = 0; i < h->X.size(); i++) { const P r = turned(a, a->turn, h->X[i][0] - a->x0, h->X[i][1] - a->y0);
      const double x = (a->px + Rg) * cell + r[0] * good, y = (a->py + Rg) * cell + r[1] * good; m.cornerUV[size_t(h->corners[i])] = {float(x / Dw), float(y / Dh)}; }
    R.shells++; R.stacked++; }
  R.locked = int(locked.size());
  for (Sh* h : L) if (h->kind == 'm' && !sh[size_t(h->mate)].split) { for (int k : h->corners) { const int p = (*o.mc)[size_t(k)]; if (p < 0) continue; const UV& q = m.cornerUV[size_t(p)]; m.cornerUV[size_t(k)] = {float(2 * R.axis - q[0]), q[1]}; } R.shells++; }
  R.scale = good; R.ok = true; (void)sym;   // (R.axis: where the axis fell — on a cell's edge)
  double used = 0; for (Sh* h : L) if (h->kind != 'S') used += h->area * good * good; for (Sh* h : locked) used += h->area; R.fill = used / (Dw * Dh);   // (the locked ones in the tile too: how full it is now)
  // a group's block was laid out with the padding at its own scale; the block then came out smaller (the tile's scale, its density) and so did the
  // gaps inside it — shells of a group touched, red. Once more, each group's inner padding grown by how much its block shrank: the gaps come out at
  // the padding (2026-11-11)
  if (!o.gpad && !gInfo.empty() && uvStart.size() == m.cornerUV.size()) {
    std::map<int, double> need; bool redo = false;
    for (auto& [gid, gi] : gInfo) { const Sh* cu = static_cast<const Sh*>(gi.second); const double k = good * cu->mult / gi.first; if (!(k > 0)) continue;   // (the block's final size to its inner one)
      need[gid] = std::min(o.pad / k, o.pad * 50.0); if (k < 0.95) redo = true; }
    if (o.gpadOut) { *o.gpadOut = need; return R; }
    if (redo) { m.cornerUV = uvStart; PackOpts o3 = o; o3.gpad = &need; const PackResult R2 = packShapes(m, T, faces, o3); if (R2.ok) return R2;
      m.cornerUV = uvStart; return packShapes(m, T, faces, [&] { PackOpts o4 = o; static const std::map<int, double> none; o4.gpad = &none; return o4; }()); }   // (it failed: as before)
  }
  return R;
}

}  // namespace hm::uv
