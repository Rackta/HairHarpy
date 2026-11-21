// Lock lines on a real mesh: hm_lltest <mesh.obj|fbx> <mode 0|1|2> <out lines.txt> [contrast] [out curves.txt]
#include "io/MeshImport.h"
#include "scene/CurveMath.h"
#include "scene/LockLines.h"
#include "scene/AnchorOps.h"
#include "scene/Edit.h"
#include "scene/MeshCurves.h"
#include "scene/WallFit.h"
#include "scene/Walls.h"
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <cstdio>
#include <fstream>

using namespace hm;

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc < 4) { std::puts("usage: hm_lltest <mesh> <mode 0 polypaint|1 hard|2 creases> <out> [contrast]"); return 2; }
  QElapsedTimer t; t.start();
  QFile f(argv[1]); if (!f.open(QIODevice::ReadOnly)) { std::puts("cannot open"); return 1; }
  QString err; auto m = importMesh(QFileInfo(argv[1]).suffix().toLower(), f.readAll(), &err);
  if (!m) { std::printf("import failed: %s\n", qPrintable(err)); return 1; }
  std::printf("imported: %zu vertices, %d polygons, polypaint %zu, hard %zu, creases %zu (%lld ms)\n", m->pos.size(), m->faceCount(), m->vcol.size(), m->hardEdges.size(), m->creases.size(), t.elapsed());
  locklines::Opt o; o.mode = locklines::Mode(std::atoi(argv[2])); if (argc > 4) o.contrast = std::atof(argv[4]);
  t.restart(); const auto r = locklines::trace(*m, o);
  if (!r.error.isEmpty()) { std::printf("error: %s\n", qPrintable(r.error)); return 1; }
  std::printf("%s | ink vertices %d | length %.3f | root (%.2f %.2f %.2f) tip (%.2f %.2f %.2f) | %lld ms\n", qPrintable(r.info), r.inkVertices, r.length, r.root.x, r.root.y, r.root.z, r.tip.x, r.tip.y, r.tip.z, t.elapsed());
  std::ofstream out(argv[3]);
  for (size_t i = 0; i < r.lines.size(); i++) { const auto& L = r.lines[i]; double len = 0; for (size_t k = 1; k < L.pts.size(); k++) len += L.pts[k].distanceTo(L.pts[k - 1]);
    std::printf("  line %zu: %zu pts, length %.3f, nodes %d → %d\n", i, L.pts.size(), len, L.a, L.b);
    out << L.pts.size(); for (auto& p : L.pts) out << ' ' << p.x << ' ' << p.y << ' ' << p.z; out << '\n'; }
  std::vector<int> deg(r.nodes.size(), 0); for (auto& L : r.lines) { deg[size_t(L.a)]++; deg[size_t(L.b)]++; }
  // [curves out]: the curves as the tool makes them (shared anchors at the nodes), each sampled — a line of x y z per sample
  if (argc > 5) {
    Scene sc; std::vector<std::vector<Vec3>> pl; std::vector<std::array<int, 2>> ends;
    for (const auto& L : r.lines) { pl.push_back(L.pts); ends.push_back({L.a, L.b}); }
    const auto made = meshc::nodeLinesToCurves(sc, pl, ends, r.nodes, "lines");
    for (int id : made) sc.item(id)->seg = true; sc.reindex();                                         // (as the program's network makes them: one tangent at the nodes)
    std::ofstream co(argv[5]); size_t na = 0;
    for (int id : made) { const Item* c = sc.item(id); na += c->anchors.size(); const auto sm = sampleCurve(sc, *c, 24);
      co << sm.size(); for (const auto& s : sm) co << ' ' << s.p.x << ' ' << s.p.y << ' ' << s.p.z; co << '\n'; }
    std::printf("curves: %zu, anchors %zu (shared ones counted per curve), anchors in the scene %zu\n", made.size(), na, sc.anchors.size());
    // [6th argument: walls] as the program does after: optimize, walls along the paths, fitted to the mesh — how far the walls lie from it
    if (argc > 6) {
      // how smooth the curves are: the turning (the sum of the angles between samples, a wave adds to it) and the bends
      // the other way (the curvature's sign flips seen from the surface: a wave) — before and after the optimizing
      auto smooth = [&](const char* what, const std::vector<int>& ids) {
        double turnAll = 0; int flipsAll = 0; double devMax = 0;
        for (int id : ids) { const Item* c = sc.item(id); if (!c) continue; const auto sm = sampleCurve(sc, *c, 24); if (sm.size() < 4) continue;
          double turn = 0; int flips = 0; double prevSide = 0;
          for (size_t k = 1; k + 1 < sm.size(); k++) { Vec3 a = sm[k].p - sm[k - 1].p, b = sm[k + 1].p - sm[k].p; if (a.length() < 1e-9 || b.length() < 1e-9) continue;
            const double ang = std::acos(std::clamp(a.normalized().dot(b.normalized()), -1.0, 1.0)); turn += ang;
            if (k >= 2 && ang > 0.004) { Vec3 a0 = sm[k - 1].p - sm[k - 2].p; const Vec3 n0 = a0.cross(a), n1 = a.cross(b); const double side = n0.dot(n1) >= 0 ? 1 : -1; if (prevSide != 0 && side != prevSide) flips++; prevSide = side; } }
          turnAll += turn; flipsAll += flips; (void)devMax; }
        std::printf("smoothness %s: curves %zu, turning %.1f deg in all, bends the other way %d\n", what, ids.size(), turnAll * 57.2958, flipsAll); };
      std::vector<std::vector<Vec3>> before; for (int id : made) { std::vector<Vec3> v; for (auto& q : sampleCurve(sc, *sc.item(id), 24)) v.push_back(q.p); before.push_back(v); }
      smooth("before the optimizing", made);
      std::vector<int> cs = made; aops::optimizeAnchors(sc, sampleAllCurves(sc), cs, nullptr, argc > 7 ? std::atof(argv[7]) : 1.0);
      smooth("after the optimizing", cs);
      { double mx = 0; for (size_t i = 0; i < made.size(); i++) { const Item* c = sc.item(made[i]); if (!c) continue; for (auto& q : sampleCurve(sc, *c, 24)) { double d = 1e300; for (size_t k = 0; k + 1 < before[i].size(); k++) { Vec3 ab = before[i][k + 1] - before[i][k]; double t = ab.lengthSq() > 0 ? std::clamp((q.p - before[i][k]).dot(ab) / ab.lengthSq(), 0.0, 1.0) : 0; d = std::min(d, (q.p - (before[i][k] + ab * t)).length()); } mx = std::max(mx, d); } }
        std::printf("optimizing moved the curves at most %.4f (%.3f%% of the lock's length)\n", mx, 100 * mx / r.length); }
      std::size_t na2 = 0; for (int id : cs) if (sc.item(id)) na2 += sc.item(id)->anchors.size();
      std::vector<int> walls; QString wm; { const CurveCache cc = sampleAllCurves(sc); if (auto pc = edit::pathCover(sc, cc, cs, true); pc && pc->size() >= 2) walls = edit::wallsAlongPaths(sc, cc, *pc, &wm, true); }
      std::printf("optimized: anchors %zu -> %zu; %s\n", na, na2, qPrintable(wm));
      const Box3 bb = m->bounds(); const double diag = bb.size().length(), cs0 = diag / 160;
      auto key = [&](const Vec3& p, int dx, int dy, int dz) { return ((long long)(std::floor((p.x - bb.min.x) / cs0) + dx) * 100003LL + (long long)(std::floor((p.y - bb.min.y) / cs0) + dy)) * 100003LL + (long long)(std::floor((p.z - bb.min.z) / cs0) + dz); };
      std::unordered_map<long long, std::vector<int>> grid; for (size_t i = 0; i < m->pos.size(); i++) grid[key(m->pos[i], 0, 0, 0)].push_back(int(i));
      auto dist = [&](const Vec3& p) { double bd = 1e300; for (int r = 0; r <= 6 && bd > 1e299; r++) for (int x = -r; x <= r; x++) for (int y = -r; y <= r; y++) for (int z = -r; z <= r; z++) { auto it = grid.find(key(p, x, y, z)); if (it != grid.end()) for (int v : it->second) bd = std::min(bd, (m->pos[size_t(v)] - p).lengthSq()); } return std::sqrt(bd); };
      auto report = [&](const char* what) { const CurveCache cc = sampleAllCurves(sc); const std::set<int> only(walls.begin(), walls.end()); double sum = 0, mx = 0; size_t n = 0;
        for (const auto& W : buildWalls(sc, cc, true, &only)) for (const auto& g : W.grids) for (int k = 0; k < g.R; k++) for (int j = 1; j + 1 < g.M; j++) { const double d = dist(W.pos[size_t(g.start + k * g.M + j)]); sum += d; mx = std::max(mx, d); n++; }
        std::printf("walls %s: inner points %zu, from the mesh: mean %.3f%%, max %.3f%% of the size\n", what, n, n ? 100 * sum / double(n) / diag : 0.0, 100 * mx / diag); };
      for (int w : walls) { QJsonObject p = sc.item(w)->raw.value("p").toObject(); p.insert("segs", 4); p.insert("bulge", 0.0); sc.item(w)->raw.insert("p", p); }
      report("flat (4 segments)");
      const int nb = wallfit::fitToMesh(sc, sampleAllCurves(sc), walls, *m, 3);
      report("fitted"); std::printf("  bulged %d of %zu\n", nb, walls.size());
      for (int w : walls) { QJsonObject p = sc.item(w)->raw.value("p").toObject(); std::printf("  wall %d: segs %d bulge %.3f\n", w, p.value("segs").toInt(), p.value("bulge").toDouble()); }
    }
  }
  for (size_t i = 0; i < r.nodes.size(); i++) std::printf("  node %zu: degree %d at (%.2f %.2f %.2f), from the root %.0f%%\n", i, deg[i], r.nodes[i].x, r.nodes[i].y, r.nodes[i].z, i < r.nodeDr.size() ? 100 * r.nodeDr[i] / std::max(r.length, 1e-9) : -1.0);
  return 0;
}
