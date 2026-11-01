// Console check of the UV solvers: hm_uvtest <project.hairmesh> [min faces]. Every mesh with a UV: its seams made where
// its UV breaks, then its shells unfolded anew (Unfold, Unfold + optimize) and its own UV optimized — each measured:
// the triangles turned over, the stretch (the symmetric Dirichlet energy, area-weighted over the shells: 4 — none), time
#include "io/ProjectIO.h"
#include "scene/Topo.h"
#include "scene/Uv.h"
#include "scene/UvSolve.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <cmath>
#include <cstdio>
#include <unordered_map>

using namespace hm;

namespace {
struct Score { int flips = 0, shells = 0, bad = 0; double sd = 0; };
// per shell (as the UV joins it): its UV points, triangles, the energy; the shells weighted by their 3D area
Score score(const PolyMesh& m, const topo::TTopo& T) {
  Score S; if (!uv::has(m)) return S;
  const uv::Points P = uv::points(m, T); const uv::Shells Sh = uv::shells(m, T, P);
  std::vector<std::vector<int>> by(size_t(Sh.count)); for (int f = 0; f < T.nf; f++) by[size_t(Sh.of[size_t(f)])].push_back(f);
  double wsum = 0;
  for (auto& fs : by) {
    std::unordered_map<int, int> loc; std::vector<Vec3> X; std::vector<uv::solve::P2> U; std::vector<uv::solve::Tri> tris;
    auto L = [&](int k) { const int p = P.of[size_t(k)]; auto r = loc.emplace(p, int(X.size())); if (r.second) { X.push_back(m.pos[size_t(T.fv[size_t(k)])]); U.push_back({m.cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1]}); } return r.first->second; };
    double a3 = 0;
    for (int f : fs) { const int s = T.fo[size_t(f)], e = T.fo[size_t(f) + 1]; std::vector<Vec3> ring; std::vector<int> li;
      for (int k = s; k < e; k++) { ring.push_back(m.pos[size_t(T.fv[size_t(k)])]); li.push_back(L(k)); }
      for (auto& t : uv::solve::triangulate(ring)) tris.push_back({li[size_t(t[0])], li[size_t(t[1])], li[size_t(t[2])]});
      a3 += uv::faceArea3(m, f); }
    const int fl = uv::solve::flips(U, X, tris); const double sd = uv::solve::distortion(U, X, tris);
    S.shells++; S.flips += fl; if (fl) S.bad++;
    if (std::isfinite(sd) && a3 > 0) { S.sd += sd * a3; wsum += a3; }
  }
  if (wsum > 0) S.sd /= wsum;
  return S;
}
void line(const char* what, const Score& s, qint64 ms) { std::printf("    %-22s flips %6d (in %4d of %4d shells)  stretch %8.4f  %6lld ms\n", what, s.flips, s.bad, s.shells, s.sd, ms); }
// the shells of `faces` rasterised at w × h texels, each grown by `grow` texels: the texels two shells share; and the
// texels outside the tile
std::pair<long, long> clashes(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, int w, int h, double grow) {
  const uv::Points P = uv::points(m, T); const uv::Shells S = uv::shells(m, T, P);
  std::vector<int> owner(size_t(w) * size_t(h), -1); long both = 0, out = 0; std::set<long> bothSet;
  for (int f : faces) { const int s = S.of[size_t(f)]; const int a = T.fo[size_t(f)], e = T.fo[size_t(f) + 1];
    for (int k = a + 1; k + 1 < e; k++) {
      std::array<std::array<double, 2>, 3> t; int i = 0; for (int c : {a, k, k + 1}) t[size_t(i++)] = {m.cornerUV[size_t(c)][0] * w, m.cornerUV[size_t(c)][1] * h};
      const int x0 = int(std::floor(std::min({t[0][0], t[1][0], t[2][0]}) - grow)), x1 = int(std::ceil(std::max({t[0][0], t[1][0], t[2][0]}) + grow));
      const int y0 = int(std::floor(std::min({t[0][1], t[1][1], t[2][1]}) - grow)), y1 = int(std::ceil(std::max({t[0][1], t[1][1], t[2][1]}) + grow));
      for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const std::array<double, 2> p{x + 0.5, y + 0.5};
        auto cr = [](const std::array<double, 2>& o, const std::array<double, 2>& u, const std::array<double, 2>& v) { return (u[0] - o[0]) * (v[1] - o[1]) - (u[1] - o[1]) * (v[0] - o[0]); };
        const double d1 = cr(t[0], t[1], p), d2 = cr(t[1], t[2], p), d3 = cr(t[2], t[0], p); bool in = (d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0);
        if (!in && grow > 0) { auto seg = [&](const std::array<double, 2>& u, const std::array<double, 2>& v) { const double dx = v[0] - u[0], dy = v[1] - u[1], L = dx * dx + dy * dy; double q = L > 0 ? ((p[0] - u[0]) * dx + (p[1] - u[1]) * dy) / L : 0; q = std::clamp(q, 0.0, 1.0); return std::hypot(p[0] - u[0] - q * dx, p[1] - u[1] - q * dy); };
          in = std::min({seg(t[0], t[1]), seg(t[1], t[2]), seg(t[2], t[0])}) <= grow; }
        if (!in) continue;
        if (x < 0 || y < 0 || x >= w || y >= h) { out++; continue; }
        int& o = owner[size_t(y) * size_t(w) + size_t(x)]; if (o < 0) o = s; else if (o != s && bothSet.insert(long(y) * w + x).second) both++; } } }
  return {both, out};
}
void packLine(const char* what, const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& fs, int w, int h, double pad, double fill, qint64 ms) {
  const auto ov = clashes(m, T, fs, w / 4, h / 4, 0), pd = clashes(m, T, fs, w, h, pad / 2 - 0.5);   // (overlaps at a quarter of the size; the padding at full)
  std::printf("    %-24s fill %5.1f%%  overlap texels %6ld  padding broken texels %6ld (out of the tile %ld)  %6lld ms\n", what, fill * 100, ov.first, pd.first, pd.second, ms);
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc < 2) { std::printf("usage: hm_uvtest <project.hairmesh> [min faces]\n"); return 2; }
  const int minFaces = argc > 2 ? std::atoi(argv[2]) : 0;
  Scene sc; LoadResult r = loadProject(QString::fromLocal8Bit(argv[1]), sc);
  if (!r.ok) { std::printf("load failed: %s\n", qPrintable(r.error)); return 1; }
  for (auto& [id, it] : sc.items) {
    if (!it.poly || !uv::has(*it.poly) || it.poly->faceCount() < minFaces) continue;
    std::printf("mesh %d '%s': %zu verts, %d faces\n", id, qPrintable(it.name), it.poly->pos.size(), it.poly->faceCount());
    auto base = std::make_shared<PolyMesh>(*it.poly); auto T = topo::ttopo(base);
    { const uv::Points P = uv::points(*base, *T); auto be = uv::borderEdges(*base, *T, P); std::vector<char> fl(size_t(T->ne), 0); for (int e : be) fl[size_t(e)] = 1; uv::setSeams(*base, *T, fl); }
    QElapsedTimer t;
    if (argc > 3 && QString(argv[3]) == "attr") {              // (Pack with each shell's settings: stacks, lock, no turn, scale, group)
      auto matFaces = [&](int mat) { std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (base->faceMat.empty() || base->faceMat[size_t(f)] == mat) fs.push_back(f); return fs; };
      auto shellsOf = [&](const PolyMesh& m, const std::vector<int>& fs) { const uv::Points P = uv::points(m, *T); const uv::Shells S = uv::shells(m, *T, P); std::map<int, std::vector<int>> by; for (int f : fs) by[S.of[size_t(f)]].push_back(f); std::vector<std::vector<int>> o; for (auto& [s, v] : by) o.push_back(v); return o; };
      auto chk = [&](const PolyMesh& m, const std::vector<int>& fs) { uv::CheckOpts o; o.pad = 8; return uv::check(m, *T, fs, o); };
      auto tdOf = [&](const PolyMesh& m, const std::vector<int>& sfs) { double au = 0, a3 = 0; for (int f : sfs) { au += std::abs(uv::faceArea(m, f)); a3 += uv::faceArea3(m, f); } return std::sqrt(au / a3); };
      { const auto fs = matFaces(3); PolyMesh m = *base; const auto c0 = chk(m, fs); uv::PackOpts o; auto r = uv::packShapes(m, *T, fs, o); const auto c1 = chk(m, fs);
        std::printf("  eyes, own UV: stacks %d, overlap %zu → packed (stacks kept %d): stacks %d, overlap %zu, padding %zu, fill %.1f%%\n", c0.stacks, c0.overlap.size(), r.stacked, c1.stacks, c1.overlap.size(), c1.padding.size(), r.fill * 100); }
      const auto fs = matFaces(0); PolyMesh u = *base; { uv::SymUnfold su; su.mc = uv::mirrorCorners(*base, *T); su.pairs = true; uv::PackOpts po; uv::unfold(u, *T, fs, false, true, nullptr, &su, &po); }
      auto sh = shellsOf(u, fs); std::sort(sh.begin(), sh.end(), [](auto& a, auto& b) { return a.size() > b.size(); }); const auto& face = sh[0];
      { PolyMesh m = u; std::vector<uv::ShellAttr> at(size_t(T->nf)); for (int f : face) at[size_t(f)].lock = true;
        const auto before = uv::box(m, uv::faceCorners(*T, face)); uv::PackOpts o; o.attr = &at; auto r = uv::packShapes(m, *T, fs, o); const auto after = uv::box(m, uv::faceCorners(*T, face)); const auto c = chk(m, fs);
        std::printf("  head, the face locked: moved %.2e; packed %d (locked %d), overlap %zu, padding %zu, fill %.1f%%\n", std::abs(after[0] - before[0]) + std::abs(after[1] - before[1]), r.shells, r.locked, c.overlap.size(), c.padding.size(), r.fill * 100); }
      { PolyMesh m = u; std::vector<uv::ShellAttr> at(size_t(T->nf)); for (auto& a : at) a.noTurn = true; uv::PackOpts o; o.attr = &at; uv::packShapes(m, *T, fs, o);
        int turned = 0; for (auto& s : sh) { const int f = s[0], k0 = T->fo[size_t(f)], k1 = k0 + 1; const double a0 = std::atan2(u.cornerUV[size_t(k1)][1] - u.cornerUV[size_t(k0)][1], u.cornerUV[size_t(k1)][0] - u.cornerUV[size_t(k0)][0]), a1 = std::atan2(m.cornerUV[size_t(k1)][1] - m.cornerUV[size_t(k0)][1], m.cornerUV[size_t(k1)][0] - m.cornerUV[size_t(k0)][0]);
          if (std::abs(std::remainder(a1 - a0, 2 * M_PI)) > 1e-3) turned++; }
        std::printf("  head, no turn on all: %d of %zu shells turned\n", turned, sh.size()); }
      { PolyMesh m = u; std::vector<uv::ShellAttr> at(size_t(T->nf)); for (int f : face) at[size_t(f)].scale = 2; uv::PackOpts o; o.attr = &at; uv::packShapes(m, *T, fs, o);
        std::printf("  head, the face ×2: its texel density to the next shell's %.3f (2 wanted; before %.3f)\n", tdOf(m, face) / tdOf(m, sh[1]), tdOf(u, face) / tdOf(u, sh[1])); }
      { auto spread = [&](const PolyMesh& m, const std::vector<std::vector<int>>& g) { double d = 0; std::vector<std::array<double, 2>> c; for (auto& s : g) { const auto b = uv::box(m, uv::faceCorners(*T, s)); c.push_back({(b[0] + b[2]) / 2, (b[1] + b[3]) / 2}); }
          for (size_t i = 0; i < c.size(); i++) for (size_t j = i + 1; j < c.size(); j++) d += std::hypot(c[i][0] - c[j][0], c[i][1] - c[j][1]); return d / std::max<size_t>(1, c.size() * (c.size() - 1) / 2); };
        std::vector<std::vector<int>> grp; for (size_t i = sh.size() - 1; grp.size() < 6 && i > 0; i--) grp.push_back(sh[i]);   // (the six smallest)
        PolyMesh a = u, b = u; uv::PackOpts o; uv::packShapes(a, *T, fs, o); std::vector<uv::ShellAttr> at(size_t(T->nf)); for (auto& s : grp) for (int f : s) at[size_t(f)].group = 1; o.attr = &at; uv::packShapes(b, *T, fs, o);
        std::printf("  head, six small shells grouped: their mean distance apart %.3f (ungrouped %.3f)\n", spread(b, grp), spread(a, grp)); }
      continue;
    }
    if (argc > 3 && QString(argv[3]) == "auto") {              // (Auto seams: each UV Mat's seams cleared — its closed pieces opened by them, unfolded)
      std::map<int, std::vector<int>> byMat; for (int f = 0; f < T->nf; f++) byMat[base->faceMat.empty() ? -1 : base->faceMat[size_t(f)]].push_back(f);
      for (auto& [mat, fs] : byMat) {
        PolyMesh m = *base; m.seams.clear(); uv::PackOpts po;
        t.start(); auto r0 = uv::unfold(m, *T, fs, false, true, nullptr, nullptr, &po); const qint64 m0 = t.elapsed();
        std::printf("  mat %d (%zu polygons): no seams — %d unfolded, %d closed (%lld ms)\n", mat, fs.size(), r0.done, r0.closed, m0);
        if (!r0.closed) continue;
        t.start(); const auto es = uv::autoSeams(m, *T, r0.closedFaces); const qint64 m1 = t.elapsed(); std::vector<char> fl(size_t(T->ne), 0); for (int e : es) fl[size_t(e)] = 1; uv::setSeams(m, *T, fl);
        t.start(); auto r1 = uv::unfold(m, *T, r0.closedFaces, false, true, nullptr, nullptr, &po); const qint64 m2 = t.elapsed();
        int flips = 0; for (int f : r0.closedFaces) if (uv::faceArea(m, f) < 0) flips++;
        const auto a = uv::angleStretch(m, *T); double sa = 0; int bad = 0; for (int f : r0.closedFaces) { sa += a[size_t(f)]; if (a[size_t(f)] > 1.5f) bad++; }
        uv::CheckOpts co; const auto ck = uv::check(m, *T, r0.closedFaces, co);
        std::printf("    auto seams: %zu edges (%lld ms) → %d unfolded, %d still closed, %d turned over, overlap %zu, angle stretch mean %.3f, %d over 1.5 of %zu (%lld ms)\n",
                    es.size(), m1, r1.done, r1.closed, flips, ck.overlap.size(), sa / std::max<size_t>(1, r0.closedFaces.size()), bad, r0.closedFaces.size(), m2);
      }
      continue;
    }
    if (argc > 3 && QString(argv[3]) == "check") {             // (the checks: own UV; symmetric unfold + pack; the old box pack on a 2:1)
      const auto mc = uv::mirrorCorners(*base, *T); std::map<int, std::vector<int>> byMat; for (int f = 0; f < T->nf; f++) byMat[base->faceMat.empty() ? -1 : base->faceMat[size_t(f)]].push_back(f);
      auto show = [&](const char* what, const PolyMesh& m, const std::vector<int>& fs, int W, int H) {
        uv::CheckOpts o; o.w = W; o.h = H; o.pad = 8; o.mc = &mc; t.start(); const auto r = uv::check(m, *T, fs, o); const qint64 ms = t.elapsed();
        std::printf("    %-26s flipped %4zu  overlap %5zu (stacks %d)  outside %5zu  padding %5zu  density %5zu (ref %.2f)  asym %5zu   %lld ms\n", what, r.flipped.size(), r.overlap.size(), r.stacks, r.outside.size(), r.padding.size(), r.density.size(), r.tdRef, r.asym.size(), ms); };
      for (auto& [mat, fs] : byMat) {
        std::printf("  mat %d (%zu polygons)\n", mat, fs.size());
        show("own UV", *base, fs, 2048, 2048);
        PolyMesh m = *base; { uv::SymUnfold su; su.mc = mc; su.pairs = true; uv::PackOpts po; uv::unfold(m, *T, fs, false, true, nullptr, &su, &po); } show("sym unfold + pack 2048", m, fs, 2048, 2048);
        PolyMesh b = m; uv::pack(b, *T, fs, 8.0 / 2048, true); show("old box pack, 2048x1024", b, fs, 2048, 1024);
        PolyMesh c = m; { const uv::Points P = uv::points(c, *T); const uv::Shells S = uv::shells(c, *T, P); int s0 = S.of[size_t(fs[0])];   // (one shell dragged over the middle: overlaps)
          std::vector<int> ks; for (int f : fs) if (S.of[size_t(f)] == s0) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) ks.push_back(k);
          const auto bx = uv::box(c, ks); uv::transform(c, ks, [&](const uv::UV& q) { return uv::UV{float(q[0] - (bx[0] + bx[2]) / 2 + 0.5), float(q[1] - (bx[1] + bx[3]) / 2 + 0.5)}; }); }
        show("one shell dragged to middle", c, fs, 2048, 2048);
      }
      { PolyMesh m = *base; t.start(); const auto a = uv::angleStretch(m, *T); const qint64 ms = t.elapsed(); int bad = 0; for (float x : a) if (x > 1.5f) bad++; std::printf("  angle stretch: %d of %zu polygons over 1.5 (%lld ms)\n", bad, a.size(), ms); }
      continue;
    }
    if (argc > 3 && QString(argv[3]) == "align") {             // (Grid on a UV Mat's shells; Stack of mirror pairs)
      std::vector<int> fs; const int mat = argc > 4 ? std::atoi(argv[4]) : 2; for (int f = 0; f < T->nf; f++) if (base->faceMat.empty() || base->faceMat[size_t(f)] == mat) fs.push_back(f);
      PolyMesh u0 = *base; { uv::SymUnfold su; su.mc = uv::mirrorCorners(*base, *T); su.pairs = true; uv::unfold(u0, *T, fs, true, true, nullptr, &su); }
      line("unfolded (sym)", score(u0, *T), 0);
      { PolyMesh m = u0; t.start(); const int n = uv::rectify(m, *T, fs); const qint64 ms = t.elapsed(); line("Grid", score(m, *T), ms);
        // (how level / upright the quads' sides lie: per quad side the smaller of |du|, |dv| over its length — 0 for a grid)
        double worst = 0, sum = 0; int cnt = 0, quads = 0; for (int f : fs) { if (T->faceSize(f) != 4) continue; quads++;
          for (int j = 0; j < 4; j++) { const auto& a = m.cornerUV[size_t(T->fo[size_t(f)] + j)]; const auto& b = m.cornerUV[size_t(T->fo[size_t(f)] + (j + 1) % 4)];
            const double du = std::abs(b[0] - a[0]), dv = std::abs(b[1] - a[1]), L = std::hypot(du, dv); if (L <= 0) continue; const double off = std::min(du, dv) / L; worst = std::max(worst, off); sum += off; cnt++; } }
        int offQuads = 0; for (int f : fs) { if (T->faceSize(f) != 4) continue; double o = 0; for (int j = 0; j < 4; j++) { const auto& a = m.cornerUV[size_t(T->fo[size_t(f)] + j)]; const auto& b = m.cornerUV[size_t(T->fo[size_t(f)] + (j + 1) % 4)]; const double du = std::abs(b[0] - a[0]), dv = std::abs(b[1] - a[1]), L = std::hypot(du, dv); if (L > 0) o = std::max(o, std::min(du, dv) / L); } if (o > 1e-3) offQuads++; }
        std::printf("    (%d shells; quads %d: sides off level/upright mean %.2e, worst %.2e; quads not on the grid %d)\n", n, quads, cnt ? sum / cnt : 0, worst, offQuads);
        // (the shells all of quads: how many came out an exact grid — every side level or upright within 0.1%)
        const uv::Points P = uv::points(m, *T); const uv::Shells S = uv::shells(m, *T, P); std::map<int, std::pair<bool, double>> st;
        for (int f : fs) { auto& s = st.emplace(S.of[size_t(f)], std::make_pair(true, 0.0)).first->second; if (T->faceSize(f) != 4) { s.first = false; continue; }
          for (int j = 0; j < 4; j++) { const auto& a = m.cornerUV[size_t(T->fo[size_t(f)] + j)]; const auto& b = m.cornerUV[size_t(T->fo[size_t(f)] + (j + 1) % 4)]; const double du = std::abs(b[0] - a[0]), dv = std::abs(b[1] - a[1]), L = std::hypot(du, dv); if (L > 0) s.second = std::max(s.second, std::min(du, dv) / L); } }
        int allQ = 0, exact = 0, near = 0; for (auto& [s, v] : st) if (v.first) { allQ++; if (v.second < 1e-3) exact++; else if (v.second < 0.05) near++; }
        std::printf("    (shells all of quads: %d — an exact grid %d, within 5%% %d)\n", allQ, exact, near); }
      { PolyMesh m = u0; const auto mc = uv::mirrorCorners(m, *T); const uv::Points P = uv::points(m, *T); const uv::Shells S = uv::shells(m, *T, P);
        // (the mirror pairs: the −X shell stacked onto its +X twin — how far each corner is from its twin's)
        std::map<int, int> to; for (int f : fs) { const int k = T->fo[size_t(f)]; if (mc[size_t(k)] >= 0) to[S.of[size_t(f)]] = S.of[size_t(T->cf[size_t(mc[size_t(k)])])]; }
        int pairs = 0; double worst = 0, size = 0; t.start();
        for (auto& [a, b] : to) { if (a == b || a > b) continue; int fa = -1, fb = -1; for (int f : fs) { if (fa < 0 && S.of[size_t(f)] == a) fa = f; if (fb < 0 && S.of[size_t(f)] == b) fb = f; }
          if (fa < 0 || fb < 0) continue; uv::stackOnto(m, *T, {fa}, fb); pairs++; }
        const qint64 ms = t.elapsed();
        for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int p = mc[size_t(k)]; if (p < 0) continue; const int sa = S.of[size_t(f)], sb = S.of[size_t(T->cf[size_t(p)])]; if (sa == sb) continue;
          worst = std::max(worst, double(std::hypot(m.cornerUV[size_t(k)][0] - m.cornerUV[size_t(p)][0], m.cornerUV[size_t(k)][1] - m.cornerUV[size_t(p)][1]))); }
        const auto bb = uv::box(m, uv::faceCorners(*T, fs)); size = std::max(bb[2] - bb[0], bb[3] - bb[1]);
        std::printf("    Stack mirror pairs: %d stacked in %lld ms; corners off their twins at most %.2e (of the layout's size %.3f)\n", pairs, ms, worst, size); }
      continue;
    }
    if (argc > 3 && QString(argv[3]) == "face") {               // (the face: its holes found, unfolded symmetric — no features, open, closed)
      const auto mc = uv::mirrorCorners(*base, *T); std::vector<int> fs; for (int f = 0; f < T->nf; f++) if (base->faceMat.empty() || base->faceMat[size_t(f)] == (argc > 4 ? std::atoi(argv[4]) : 0)) fs.push_back(f);
      auto feats = uv::detectFeatures(*base, *T, fs);
      std::printf("  %zu polygons, %zu features (holes):\n", fs.size(), feats.size());
      for (auto& f : feats) std::printf("    corners %d %d: upper %zu, lower %zu vertices; middle at x %.2f y %.2f\n", f.a.front(), f.a.back(), f.a.size(), f.b.size(),
        (base->pos[size_t(f.a.front())].x + base->pos[size_t(f.a.back())].x) / 2, (base->pos[size_t(f.a.front())].y + base->pos[size_t(f.a.back())].y) / 2);
      for (int mode = 0; mode < 3; mode++) {
        PolyMesh m = *base; uv::SymUnfold su; su.mc = mc; su.pairs = true; auto fe = feats; for (auto& f : fe) f.closed = mode == 2;
        t.start(); const auto u = uv::unfold(m, *T, fs, true, true, nullptr, &su, nullptr, mode ? &fe : nullptr); const qint64 ms = t.elapsed();
        // (each pair's points: how far off one vertical (open) / one point (closed); the corners off level) — over the corners of the faces
        double du = 0, dd = 0, lv = 0; int n = 0;
        const uv::Points Pq = uv::points(m, *T); const uv::Shells Sq = uv::shells(m, *T, Pq); int inShell = -1;
        auto uvOf = [&](int v) { for (int f : fs) if (Sq.of[size_t(f)] == inShell) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) if (T->fv[size_t(k)] == v) return m.cornerUV[size_t(k)]; return uv::UV{0, 0}; };
        for (auto& f : fe) { std::map<int, int> cnt; std::set<int> fv(f.a.begin(), f.a.end());   // (the shell the feature lies in: the most of its points)
          for (int x : fs) for (int k = T->fo[size_t(x)]; k < T->fo[size_t(x) + 1]; k++) if (fv.count(T->fv[size_t(k)])) cnt[Sq.of[size_t(x)]]++;
          inShell = -1; int bc = 0; for (auto& [s, c] : cnt) if (c > bc) { bc = c; inShell = s; }
          const auto ft = uv::featureTies({f}, [&] { std::vector<int> id(m.pos.size()); std::iota(id.begin(), id.end(), 0); return id; }(), m.pos);
          double fu = 0, fd = 0;
          for (auto& p : ft.pairs) { const auto a = uvOf(p[1]), b = uvOf(p[0]); fu = std::max(fu, double(std::abs(a[0] - b[0]))); fd = std::max(fd, double(std::hypot(a[0] - b[0], a[1] - b[1]))); n++; }
          du = std::max(du, fu); dd = std::max(dd, fd); lv = std::max(lv, double(std::abs(uvOf(f.a.front())[1] - uvOf(f.a.back())[1])));
          std::printf("        feature %d-%d: %zu pairs, off one vertical %.1e, apart %.1e, corners off level %.1e\n", f.a.front(), f.a.back(), ft.pairs.size(), fu, fd, double(std::abs(uvOf(f.a.front())[1] - uvOf(f.a.back())[1]))); }
        line(mode == 0 ? "symmetric, no features" : mode == 1 ? "features open" : "features closed", score(m, *T), ms);
        { // (the triangles of these polygons: turned over for real, against flat — shut lips' own have no area)
          std::vector<double> so(size_t(Sq.count), 0); auto ar = [&](int a, int b, int c) { const auto &p = m.cornerUV[size_t(a)], &q = m.cornerUV[size_t(b)], &r = m.cornerUV[size_t(c)]; return (double(q[0]) - p[0]) * (double(r[1]) - p[1]) - (double(r[0]) - p[0]) * (double(q[1]) - p[1]); };
          for (int f : fs) for (int k = T->fo[size_t(f)] + 1; k + 1 < T->fo[size_t(f) + 1]; k++) so[size_t(Sq.of[size_t(f)])] += ar(T->fo[size_t(f)], k, k + 1);
          int inv = 0, flat = 0; for (int f : fs) for (int k = T->fo[size_t(f)] + 1; k + 1 < T->fo[size_t(f) + 1]; k++) { const double a = ar(T->fo[size_t(f)], k, k + 1) * (so[size_t(Sq.of[size_t(f)])] < 0 ? -1 : 1); if (a < -1e-12) inv++; else if (a < 1e-12) flat++; }
          std::printf("      (fan triangles of these polygons: turned over %d, flat %d)\n", inv, flat); }
        std::printf("      (symmetric %d, featured %d; pairs %d: off one vertical %.1e, apart %.1e; corners off level %.1e)\n", u.symmetric, u.featured, n, du, dd, lv);
        if (argc > 5) { QString p = QString::fromLocal8Bit(argv[5]) + QString("_%1.obj").arg(mode); FILE* o = std::fopen(qPrintable(p), "w");   // (the UV as an OBJ to look at)
          if (o) { for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) std::fprintf(o, "v %f %f 0\n", m.cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1]);
            int c = 1; for (int f : fs) { std::fprintf(o, "f"); for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) std::fprintf(o, " %d", c++); std::fprintf(o, "\n"); } std::fclose(o); } }
      }
      continue;
    }
    if (argc > 3 && QString(argv[3]) == "pack") {              // (Pack: each UV Mat on its own tile — the old boxes, by shape, symmetric)
      const int W = argc > 4 ? std::atoi(argv[4]) : 2048, H = argc > 5 ? std::atoi(argv[5]) : 2048; const double pad = argc > 6 ? std::atof(argv[6]) : 8;
      const auto mc = uv::mirrorCorners(*base, *T); std::map<int, std::vector<int>> byMat; for (int f = 0; f < T->nf; f++) byMat[base->faceMat.empty() ? -1 : base->faceMat[size_t(f)]].push_back(f);
      std::printf("  texture %d x %d, padding %.0f px; %zu UV Mats\n", W, H, pad, byMat.size());
      for (auto& [mat, fs] : byMat) {
        std::printf("  mat %d '%s': %zu polygons\n", mat, mat >= 0 && size_t(mat) < base->matNames.size() ? qPrintable(base->matNames[size_t(mat)]) : "-", fs.size());
        { PolyMesh m = *base; t.start(); const double f = uv::pack(m, *T, fs, pad / std::max(W, H), true); const qint64 ms = t.elapsed();
          double a = 0; for (int x : fs) a += std::abs(uv::faceArea(m, x)); (void)f; packLine("boxes (old)", m, *T, fs, W, H, pad, a, ms); }
        { PolyMesh m = *base; uv::PackOpts o; o.w = W; o.h = H; o.pad = pad; t.start(); auto pr = uv::packShapes(m, *T, fs, o); const qint64 ms = t.elapsed(); packLine(pr.ok ? "by shape" : "by shape FAILED", m, *T, fs, W, H, pad, pr.fill, ms); }
        PolyMesh u0 = *base; { uv::SymUnfold su; su.mc = mc; su.pairs = true; su.axis = 0.5; uv::unfold(u0, *T, fs, true, true, nullptr, &su); }   // (the same symmetric unfold for both)
        { PolyMesh m = u0; uv::PackOpts o; o.w = W; o.h = H; o.pad = pad; t.start(); auto pr = uv::packShapes(m, *T, fs, o); const qint64 ms = t.elapsed(); packLine(pr.ok ? "unfold, by shape" : "unfold, by shape FAILED", m, *T, fs, W, H, pad, pr.fill, ms); }
        { PolyMesh m = u0; uv::PackOpts o; o.w = W; o.h = H; o.pad = pad; o.mc = &mc; t.start(); auto pr = uv::packShapes(m, *T, fs, o); const qint64 ms = t.elapsed();
          packLine(pr.ok ? "unfold, symmetric" : "unfold, symmetric FAILED", m, *T, fs, W, H, pad, pr.fill, ms);
          // (the mirror, over the shells whose every corner's mirror lies in one shell: a pair — about U = axis, one across the plane — on the axis)
          const uv::Points Pp = uv::points(m, *T); const uv::Shells Sh = uv::shells(m, *T, Pp); std::vector<int> to(size_t(Sh.count), -2); std::set<int> in(fs.begin(), fs.end());
          for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int p = mc[size_t(k)]; const int g = p >= 0 && in.count(T->cf[size_t(p)]) ? Sh.of[size_t(T->cf[size_t(p)])] : -1; int& x = to[size_t(Sh.of[size_t(f)])]; if (x == -2) x = g; else if (x != g) x = -1; }
          double worst = 0, cw = 0; int n = 0, cn = 0; std::vector<double> mid(size_t(Sh.count), 0); std::vector<int> mn(size_t(Sh.count), 0);
          for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int s = Sh.of[size_t(f)], p = mc[size_t(k)]; if (to[size_t(s)] == s && p >= 0) { mid[size_t(s)] += (m.cornerUV[size_t(k)][0] + m.cornerUV[size_t(p)][0]) / 2.0; mn[size_t(s)]++; } }
          int onAxis = 0; for (int s = 0; s < Sh.count; s++) if (mn[size_t(s)] && std::abs(mid[size_t(s)] / mn[size_t(s)] - pr.axis) < 1e-5) onAxis++;
          for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int s = Sh.of[size_t(f)], g = to[size_t(s)], p = mc[size_t(k)]; if (g < 0 || p < 0) continue;
            if (g == s) { cw = std::max(cw, std::abs((m.cornerUV[size_t(k)][0] + m.cornerUV[size_t(p)][0]) / 2.0 - mid[size_t(s)] / mn[size_t(s)])); cn++; }   // (each about its own middle line)
            else if (to[size_t(g)] == s) { worst = std::max(worst, double(std::hypot(m.cornerUV[size_t(p)][0] - (2 * pr.axis - m.cornerUV[size_t(k)][0]), m.cornerUV[size_t(p)][1] - m.cornerUV[size_t(k)][1]))); n++; } }
          std::printf("      (centred %d (off the axis %d; on it found %d), pairs %d (split %d), free %d; pairs off the mirror %.1e over %d corners; across the plane off its own middle %.1e over %d)\n", pr.centred, pr.offAxis, onAxis, pr.pairs, pr.split, pr.free, worst, n, cw, cn); }
      }
      continue;
    }
    line("own UV", score(*base, *T), 0);
    { PolyMesh m = *base; t.start(); auto u = uv::unfold(m, *T, {}, false, false, nullptr); const qint64 ms = t.elapsed(); line("Unfold", score(m, *T), ms); if (u.closed) std::printf("    (closed shells left: %d)\n", u.closed); }
    { PolyMesh m = *base; t.start(); uv::unfold(m, *T, {}, false, true, nullptr); const qint64 ms = t.elapsed(); line("Unfold + optimize", score(m, *T), ms); }
    { PolyMesh m = *base; t.start(); uv::optimize(m, *T, {}, nullptr); const qint64 ms = t.elapsed(); line("Optimize own UV", score(m, *T), ms); }
    // pins: every 40th inner edge; their ends must not move (Unfold + optimize, Optimize)
    uv::Pins pins; for (int e = 0; e < T->ne; e += 40) if (T->ef1[size_t(e)] >= 0) pins.insert({T->ea[size_t(e)], T->eb[size_t(e)]});
    auto moved = [&](const PolyMesh& m) { double d = 0; for (int f = 0; f < T->nf; f++) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) {
        const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)]; const int a = T->fv[size_t(k)], b = T->fv[size_t(nk)];
        if (pins.count({std::min(a, b), std::max(a, b)})) d = std::max(d, double(std::hypot(m.cornerUV[size_t(k)][0] - base->cornerUV[size_t(k)][0], m.cornerUV[size_t(k)][1] - base->cornerUV[size_t(k)][1]))); }
      return d; };
    { PolyMesh m = *base; t.start(); std::vector<int> fs(size_t(T->nf)); for (int f = 0; f < T->nf; f++) fs[size_t(f)] = f; uv::unfold(m, *T, fs, true, true, &pins); const qint64 ms = t.elapsed(); line("Unfold pinned", score(m, *T), ms); std::printf("    (pinned points moved at most %.2e)\n", moved(m)); }
    { PolyMesh m = *base; t.start(); uv::optimize(m, *T, {}, &pins); const qint64 ms = t.elapsed(); line("Optimize pinned", score(m, *T), ms); std::printf("    (pinned points moved at most %.2e)\n", moved(m)); }
    // symmetry: Unfold symmetric (pairs on, in place — no packing) and Symmetrize (from +X): how far each corner is from its
    // mirror's place (a shell across the middle — about its own middle; a pair — about U = 0.5), in parts of the UV's size
    const auto mc = uv::mirrorCorners(*base, *T); int withMirror = 0; for (int k : mc) if (k >= 0) withMirror++;
    std::printf("    corners with a mirror: %d of %zu\n", withMirror, mc.size());
    if (!withMirror) continue;
    auto symErr = [&](const PolyMesh& m, double& selfErr, double& pairErr, const std::vector<int>& skip = {}) {   // (skip: polygons whose shells are left out — not unfolded)
      std::vector<char> skipF(size_t(T->nf), 0); for (int f : skip) skipF[size_t(f)] = 1;
      const uv::Points P = uv::points(m, *T); const uv::Shells S = uv::shells(m, *T, P);
      std::vector<double> mid(size_t(S.count), 0); std::vector<int> cnt(size_t(S.count), 0);
      for (size_t k = 0; k < mc.size(); k++) { const int p = mc[k]; if (p < 0) continue; const int s = S.of[size_t(T->cf[k])]; if (S.of[size_t(T->cf[size_t(p)])] == s) { mid[size_t(s)] += (m.cornerUV[k][0] + m.cornerUV[size_t(p)][0]) / 2.0; cnt[size_t(s)]++; } }
      std::vector<int> allK(mc.size()); for (size_t k = 0; k < mc.size(); k++) allK[k] = int(k);
      const auto b = uv::box(m, allK); const double size = std::max({b[2] - b[0], b[3] - b[1], 1e-12});
      // (only shells whose every corner's mirror lies in one shell — itself, or its pair)
      std::vector<int> to(size_t(S.count), -2);
      for (size_t k = 0; k < mc.size(); k++) { const int s = S.of[size_t(T->cf[k])]; const int g = mc[k] >= 0 && !skipF[size_t(T->cf[k])] && !skipF[size_t(T->cf[size_t(mc[k])])] ? S.of[size_t(T->cf[size_t(mc[k])])] : -1; int& x = to[size_t(s)]; if (x == -2) x = g; else if (x != g) x = -1; }
      selfErr = pairErr = 0;
      for (size_t k = 0; k < mc.size(); k++) { const int p = mc[k]; if (p < 0) continue; const int s = S.of[size_t(T->cf[k])]; if (to[size_t(s)] < 0) continue; const bool self = S.of[size_t(T->cf[size_t(p)])] == s;
        const double a = self ? mid[size_t(s)] / cnt[size_t(s)] : 0.5;
        const double e = std::hypot(m.cornerUV[size_t(p)][0] - (2 * a - m.cornerUV[k][0]), m.cornerUV[size_t(p)][1] - m.cornerUV[k][1]) / size;
        if (e > (self ? selfErr : pairErr) && std::getenv("UVDBG") && e > 1e-3) std::printf("      worst so far: k %zu (x %.5f, v %d) p %d (x %.5f, v %d) self %d err %.3f uvk %.4f %.4f uvp %.4f %.4f a %.4f\n", k, m.pos[size_t(T->fv[k])].x, T->fv[k], p, m.pos[size_t(T->fv[size_t(p)])].x, T->fv[size_t(p)], int(self), e, m.cornerUV[k][0], m.cornerUV[k][1], m.cornerUV[size_t(p)][0], m.cornerUV[size_t(p)][1], a);
        (self ? selfErr : pairErr) = std::max(self ? selfErr : pairErr, e); }
    };
    std::vector<int> fs(size_t(T->nf)); for (int f = 0; f < T->nf; f++) fs[size_t(f)] = f;
    { PolyMesh m = *base; uv::SymUnfold su; su.mc = mc; su.pairs = true; su.axis = 0.5;
      t.start(); auto u = uv::unfold(m, *T, fs, true, true, nullptr, &su); const qint64 ms = t.elapsed(); line("Unfold symmetric", score(m, *T), ms);
      std::vector<int> skip = u.closedFaces; skip.insert(skip.end(), u.asymFaces.begin(), u.asymFaces.end()); double se, pe; symErr(m, se, pe, skip); std::printf("    (symmetric %d, mirrored %d, seams not symmetric %d; off the mirror: across the middle %.2e, pairs %.2e)\n", u.symmetric, u.mirrored, u.asym, se, pe);
      // the −X side spoilt (each UV point shifted its own way), then Symmetrize from +X — symmetric again?
      PolyMesh w = m; const auto b = uv::box(w, std::vector<int>(fs.size(), 0)); (void)b;
      { const uv::Points P = uv::points(w, *T); for (int p = 0; p < P.count(); p++) { if (!(w.pos[size_t(P.vert[size_t(p)])].x < -1e-3)) continue;
          const float du = 0.003f * float(std::sin(p * 1.7)), dv = 0.003f * float(std::cos(p * 2.3)); for (int i = P.off[size_t(p)]; i < P.off[size_t(p) + 1]; i++) { auto& q = w.cornerUV[size_t(P.list[size_t(i)])]; q[0] += du; q[1] += dv; } } }
      symErr(w, se, pe, skip); std::printf("    (the −X side spoilt: off the mirror %.2e / %.2e)\n", se, pe); line("  spoilt", score(w, *T), 0);
      const double eps = 1e-4 * w.bounds().size().length(); std::vector<char> sk(size_t(T->nf), 0); for (int f : skip) sk[size_t(f)] = 1;   // (the shells it unfolded only)
      std::vector<int> from; for (size_t k = 0; k < mc.size(); k++) if (w.pos[size_t(T->fv[k])].x > -eps && !sk[size_t(T->cf[k])] && (mc[k] < 0 || !sk[size_t(T->cf[size_t(mc[k])])])) from.push_back(int(k));
      const int n = uv::symmetrize(w, *T, mc, from, 0.5); symErr(w, se, pe, skip);
      line("Symmetrize from +X", score(w, *T), 0); std::printf("    (%d corners set; off the mirror: across the middle %.2e, pairs %.2e)\n", n, se, pe);
      { double worst = 0; size_t wk = 0; for (size_t k = 0; k < mc.size(); k++) { if (sk[size_t(T->cf[k])]) continue; const double d = std::hypot(w.cornerUV[k][0] - m.cornerUV[k][0], w.cornerUV[k][1] - m.cornerUV[k][1]); if (d > worst) { worst = d; wk = k; } }        std::vector<char> inFrom(mc.size(), 0); for (int k : from) inFrom[size_t(k)] = 1; const int p = mc[wk];        std::printf("    (back to the symmetric unfold: worst %.2e at corner %zu: x %.5f, in from %d, mirror %d (x %.5f, in from %d, same vertex %d))\n", worst, wk, w.pos[size_t(T->fv[wk])].x, int(inFrom[wk]), p, p >= 0 ? w.pos[size_t(T->fv[size_t(p)])].x : 0.0, p >= 0 ? int(inFrom[size_t(p)]) : -1, p >= 0 ? int(T->fv[wk] == T->fv[size_t(p)]) : -1); } }
  }
  return 0;
}
