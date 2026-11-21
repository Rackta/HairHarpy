// Console check of the cocoons' loops: hm_ringtest <project.hairmesh> [cocoon name part] — per cocoon: the loop step,
// the number of rings, the largest gap between two rings (in loop steps) and where it lies; the lock-mode segments
// count. A gap far over the step is a stretch drawn without loops.
#include "io/ProjectIO.h"
#include "scene/HairState.h"
#include "scene/Cocoons.h"
#include "scene/Edit.h"
#include "scene/Display.h"
#include <QCoreApplication>
#include <algorithm>
#include <functional>
#include <set>
#include <cstdio>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc < 2) { std::printf("usage: hm_ringtest <project.hairmesh> [name part]\n"); return 2; }
  const QString want = argc > 2 && argv[2][0] != '-' ? QString::fromLocal8Bit(argv[2]) : QString();
  const bool detail = qEnvironmentVariableIsSet("RING_DETAIL");
  hm::Scene sc; hm::LoadResult r = hm::loadProject(QString::fromLocal8Bit(argv[1]), sc);
  if (!r.ok) { std::printf("load failed: %s\n", qPrintable(r.error)); return 1; }
  hm::HairState hs; hs.load(sc);
  // --make G1,G2,… [--link A>B]: cocoons made of those lock groups in that order, as the Cocoon button does (each on the
  // cocoons made before it), then a link: cocoon named A takes the loops of cocoon named B
  for (int ai = 2; ai + 1 < argc; ai++) {
    const QString o = QString::fromLocal8Bit(argv[ai]), v = QString::fromLocal8Bit(argv[ai + 1]);
    if (o == "--make") for (const QString& spec : v.split(',')) {   // (Group or Group@Parent cocoon: made as that cocoon's branch)
      const QString gname = spec.section('@', 0, 0), pname = spec.section('@', 1);
      int gid = -1, pid = -1; for (auto& [id, it] : sc.items) { if (it.type == hm::ItemType::Group && it.name == gname) gid = id; if (it.type == hm::ItemType::Cocoon && !pname.isEmpty() && it.name == pname) pid = id; }
      if (gid < 0) { std::printf("no group %s\n", qPrintable(gname)); continue; }
      hm::Selection sel; sel.items = {gid}; if (pid >= 0) sel.items.push_back(pid); QString msg;
      const int k = hm::edit::makeLiveCocoon(sc, hs.curves, hs.cocoonSystem(), sel, &msg);
      std::printf("make %s -> %d: %s\n", qPrintable(gname), k, qPrintable(msg)); sc.reindex(); hs.load(sc);
    }
    if (o == "--guide") {   // (Cocoon name@share along it@radius: a guide line across it, tilted 30° — as the Line tool makes one)
      const QStringList q = v.split('@'); int kid = -1; for (auto& [id, it] : sc.items) if (it.type == hm::ItemType::Cocoon && it.name == q.value(0)) kid = id;
      if (kid >= 0) { hm::Item& g = sc.addItem(hm::ItemType::Guide, kid, "Guide T");
        g.raw = QJsonObject{{"kind", "line"}, {"hits", QJsonArray()}, {"on", QJsonObject{{"s", q.value(1).toDouble()}, {"d", QJsonArray{0.5, 0.0, 0.866}}}}, {"p", QJsonObject{{"radius", q.value(2).toDouble()}, {"blend", 0.64}, {"density", 1.5}}}};
        sc.item(kid)->children.push_back(g.id); sc.reindex(); hs.load(sc); std::printf("guide on %s at %s\n", qPrintable(q.value(0)), qPrintable(q.value(1))); } }
    if (o == "--strands") { int gid = -1; for (auto& [id, it] : sc.items) if (it.type == hm::ItemType::Group && it.name == v) gid = id;
      hm::Selection gs; gs.items = {gid}; const auto lock = hm::edit::lockCurves(sc, gs); std::vector<std::vector<int>> all; hm::edit::lockEnds(sc, hs.curves, lock, nullptr, &all);
      for (auto& s : all) { std::printf("  strand root %d tip %d:", s[0], s[1]); for (size_t q = 2; q < s.size(); q++) std::printf(" %d", s[q]); std::printf("\n"); }
      for (int c : lock) { const hm::Item* x = sc.item(c); std::printf("  curve %-10s seg %d junc %d anchors", qPrintable(x->name), x->seg, x->junc); for (int a : x->anchors) std::printf(" %d", a); std::printf("\n"); } }
    if (o == "--split") { int gid = -1; for (auto& [id, it] : sc.items) if (it.type == hm::ItemType::Group && it.name == v) gid = id;
      hm::Selection sel; QString m = gid >= 0 ? hm::edit::splitLockBranches(sc, hs.curves, gid, sel) : QString("no group");
      std::printf("split %s: %s\n", qPrintable(v), qPrintable(m)); hs.load(sc); }
    if (o == "--link") { const QStringList ab = v.split('>'); int a = -1, b = -1;
      for (auto& [id, it] : sc.items) if (it.type == hm::ItemType::Cocoon) { if (it.name == ab.value(0)) a = id; if (it.name == ab.value(1)) b = id; }
      if (a >= 0 && b >= 0) { hm::Item* x = sc.item(a); QJsonArray l = x->raw.value("links").toArray(); l.append(b); x->raw.insert("links", l); hs.load(sc); std::printf("linked %d > %d\n", a, b); }
      else std::printf("link: cocoon not found\n"); }
  }
  hm::CocoonSystem* cs = hs.cocoonSystem(); if (!cs) { std::printf("no cocoon system\n"); return 1; }
  int bad = 0;
  for (const auto& [id, res] : cs->results()) {
    const hm::Item* it = sc.item(id); if (!it) continue;
    if (!want.isEmpty() && !it->name.contains(want, Qt::CaseInsensitive)) continue;
    const double h = cs->loopStep(id), L = res.geo.N ? res.geo.D[size_t(res.geo.N - 1)] : 0;
    std::vector<double> s; for (const auto& R : res.rings) s.push_back(R.s);
    std::sort(s.begin(), s.end());
    double worst = 0, at = 0;
    for (size_t i = 1; i < s.size(); i++) if (s[i] - s[i - 1] > worst) { worst = s[i] - s[i - 1]; at = s[i - 1]; }
    int par = 0, snap = 0; for (const auto& R : res.rings) { par += R.par; snap += R.snap.has_value(); }
    // neighbouring loops crossing inside the cocoon: each loop's plane (the drawn one: pp / pn when it has them) met at 12
    // points round the cocoon's oval — the later loop must stay after the earlier one everywhere; and the steepest tilt
    int cross = 0; double tiltMax = 0; std::vector<const hm::CocoonRing*> rs; for (const auto& R : res.rings) rs.push_back(&R);
    std::sort(rs.begin(), rs.end(), [](auto* a, auto* b) { return a->s < b->s; });
    const hm::CocoonGeo& G = res.geo;
    auto along = [&](const hm::CocoonRing& R, int i, const hm::Vec3& Lt) { const hm::Vec3 N = R.pn ? *R.pn : R.n; const double tn = G.t[size_t(i)].dot(N); if (std::abs(tn) < 1e-6) return 1e9; return R.s - Lt.dot(N) / tn; };
    for (auto* R : rs) if (G.N > 1) { const int i = std::clamp(int(std::lround(hm::cocoonStation(G, R->s))), 0, G.N - 1); const hm::Vec3 N = R->pn ? *R->pn : R->n; tiltMax = std::max(tiltMax, std::acos(std::clamp(std::abs(G.t[size_t(i)].dot(N.normalized())), 0.0, 1.0)) * 57.2958); }
    for (size_t q = 1; q < rs.size() && G.N > 1; q++) { const int i = std::clamp(int(std::lround(hm::cocoonStation(G, (rs[q - 1]->s + rs[q]->s) / 2))), 0, G.N - 1); bool x = false;
      for (int j = 0; j < 12 && !x; j++) { const hm::Vec3 Lt = hm::cocoonPoint(G, i, j * 3.14159265 / 6) - G.c[size_t(i)]; if (along(*rs[q], i, Lt) < along(*rs[q - 1], i, Lt)) x = true; }
      cross += x; }
    std::printf("    crossing neighbour loops %d, steepest loop %.0f deg off square; under: %s\n", cross, tiltMax, sc.item(it->parent) ? qPrintable(sc.item(it->parent)->name) : "-");
    const bool gap = h > 0 && worst > 2.2 * h; bad += gap;
    std::printf("%-34s step %7.2f len %8.1f rings %3zu (par %3d snap %2d) lockSegs %4zu  worst gap %5.2f steps at s=%.1f (%.0f%%)%s\n",
                qPrintable(it->name), h, L, s.size(), par, snap, res.lockSegs.size() / 2, h > 0 ? worst / h : 0, at, L > 0 ? 100 * at / L : 0, gap ? "  <-- GAP" : "");
    { hm::LockInput li; if (cs->lockInput(id, li)) { const int g = cs->lockGroup(id); std::set<int> mine; for (auto& w : li.walls) mine.insert(w.id);
        std::vector<QString> out; std::function<void(int)> walk = [&](int gi) { const hm::Item* G = sc.item(gi); if (!G) return; for (int c : G->children) { const hm::Item* x = sc.item(c); if (!x) continue; if (x->type == hm::ItemType::Group) walk(c); else if (x->type == hm::ItemType::Surface && !mine.count(c)) out.push_back(x->name); } };
        if (g >= 0) walk(g);
        std::printf("    walls with loops %zu, lock walls left without: %zu", li.walls.size(), out.size()); for (auto& n : out) std::printf(" %s", qPrintable(n)); std::printf("\n"); } }
    if (qEnvironmentVariable("RING_LIST") == it->name) for (int d : cs->deps(id)) {   // (how far it lies from each source: the overlap test of cocoonRings, by station)
      const hm::CocoonGeo PG = cs->geo(d), &CG = res.geo; std::printf("    vs %s:", qPrintable(sc.item(d)->name));
      for (int i = 0; i < CG.N; i += std::max(1, CG.N / 30)) { int j = 0; double bd = 1e300; for (int q = 0; q < PG.N; q++) { const double dd = (PG.c[size_t(q)] - CG.c[size_t(i)]).length(); if (dd < bd) { bd = dd; j = q; } }
        const hm::Vec3 dv = CG.c[size_t(i)] - PG.c[size_t(j)]; const double x = dv.dot(PG.u[size_t(j)]), y = dv.dot(PG.v[size_t(j)]), L = std::hypot(x, y);
        const double rp = L < 1e-9 ? 0 : 1 / std::hypot(x / L / std::max(PG.a[size_t(j)], 1e-6), y / L / std::max(PG.b[size_t(j)], 1e-6));
        const double cx = -dv.dot(CG.u[size_t(i)]), cy = -dv.dot(CG.v[size_t(i)]); double Lc = std::hypot(cx, cy); if (Lc == 0) Lc = 1;
        const double rc = 1 / std::hypot(cx / Lc / std::max(CG.a[size_t(i)], 1e-6), cy / Lc / std::max(CG.b[size_t(i)], 1e-6));
        std::printf(" %.0f:%.1f", CG.D[size_t(i)], (L - (rp + rc + h)) / h); }
      std::printf("\n"); }
    if (qEnvironmentVariable("RING_LIST") == it->name && res.geo.N) std::printf("    root %.3f %.3f %.3f\n", res.geo.c[0].x, res.geo.c[0].y, res.geo.c[0].z);
    if (qEnvironmentVariable("RING_LIST") == it->name) { auto dd = cs->deps(id); for (const auto& R : res.rings) std::printf("    s %7.1f n %.2f %.2f %.2f par %d prio %d (%s) run %s\n", R.s, (R.pn ? *R.pn : R.n).x, (R.pn ? *R.pn : R.n).y, (R.pn ? *R.pn : R.n).z, R.par, R.prio.value_or(-1), R.prio && size_t(*R.prio) < dd.size() && sc.item(dd[size_t(*R.prio)]) ? qPrintable(sc.item(dd[size_t(*R.prio)])->name) : "own", R.run.c_str()); }
    if (gap && detail) for (const auto& R : res.rings) std::printf("    s %8.2f par %d fold %d ri %d prio %d run %s snap %d\n", R.s, R.par, R.fold, R.ri.value_or(-1), R.prio.value_or(-1), R.run.c_str(), R.snap.has_value());
  }
  std::printf("%d cocoon(s) with a gap over 2.2 loop steps\n", bad);
  return 0;
}
