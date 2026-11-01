// Console check of the core without the GPU: loads a project, samples every curve, prints what was read.
//   hm_selftest <project.hairmesh>
#include "io/ProjectIO.h"
#include "scene/CurveMath.h"
#include "scene/Walls.h"
#include "scene/Cocoons.h"
#include "scene/HairState.h"
#include "scene/Network.h"
#include "scene/Convert.h"
#include "scene/AutoFix.h"
#include "scene/Edit.h"
#include <map>
#include <algorithm>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <cstdio>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc < 2) { std::printf("usage: hm_selftest <project.hairmesh>\n"); return 2; }
  hm::Scene sc;
  QElapsedTimer t; t.start();
  hm::LoadResult r = hm::loadProject(QString::fromLocal8Bit(argv[1]), sc);
  if (!r.ok) { std::printf("load failed: %s\n", qPrintable(r.error)); return 1; }
  std::printf("loaded in %lld ms: %zu items, %zu anchors, %zu roots\n", t.elapsed(), sc.items.size(), sc.anchors.size(), sc.roots.size());
  for (const QString& w : r.warnings) std::printf("  warning: %s\n", qPrintable(w));
  int curves = 0, samples = 0, meshes = 0; long long faces = 0;
  t.restart();
  for (const hm::Item* c : sc.curves()) { curves++; samples += int(hm::sampleCurve(sc, *c).size()); }
  std::printf("curves %d, samples %d (%lld ms)\n", curves, samples, t.elapsed());
  for (auto& [id, it] : sc.items) if (it.poly) { meshes++; faces += it.poly->faceCount(); std::printf("  mesh %d '%s': %zu verts, %d faces, %zu groups\n", id, qPrintable(it.name), it.poly->pos.size(), it.poly->faceCount(), it.poly->groupNames.size()); }
  hm::Box3 b = sc.bounds();
  std::printf("meshes %d (%lld faces); bounds (%.3f %.3f %.3f) - (%.3f %.3f %.3f)\n", meshes, faces, b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z);
  // the first curve's samples, for comparing with the HTML version (window.__hair)
  auto cs = sc.curves();
  if (!cs.empty()) {
    auto S = hm::sampleCurve(sc, *cs.front());
    std::printf("curve %d '%s' first/mid/last sample: (%.6f %.6f %.6f) (%.6f %.6f %.6f) (%.6f %.6f %.6f)\n", cs.front()->id, qPrintable(cs.front()->name),
                S.front().p.x, S.front().p.y, S.front().p.z, S[S.size() / 2].p.x, S[S.size() / 2].p.y, S[S.size() / 2].p.z, S.back().p.x, S.back().p.y, S.back().p.z);
  }
  // --dump <file>: every wall (id, vertex count, first / middle / last vertex) and every curve (id, sample count, middle
  // sample) — the same lines the HTML version gives in the browser, for comparing the two
  if (argc >= 4 && QString(argv[2]) == "--dump") {
    t.restart();
    // --move <anchor> dx dy dz (after the dump file): the anchor moved as a drag does (frozen handles around it
    // released, the curves touching it, then only what depends on them rebuilt) — the dump is of the result
    hm::HairState hs; hs.load(sc);
    std::printf("hair state %lld ms\n", t.elapsed());
    std::map<std::string, std::string> netSig; std::vector<int> sel, asel;
    { hm::Network net(sc, hs.curves, sel, asel); auto u = net.loadUpkeep(netSig);
      std::printf("load upkeep: cut %d, merged %d, joined %d, renamed %d\n", u.cut, u.merged, u.joined, int(u.renamed));
      if (u.changed()) hs.load(sc); }
    // --op weld A B | join A B | insertkeep CURVE SEG U | insertseg CURVE SEG X Y Z — an edit, then commit's upkeep
    // (NEXTID env: the first id for new items — the HTML version's UID depends on the page's session)
    if (qEnvironmentVariableIsSet("NEXTID")) sc.nextId = qEnvironmentVariableIntValue("NEXTID");
    if (argc >= 7 && QString(argv[4]) == "--op") {
      QString op = argv[5]; hm::Network net(sc, hs.curves, sel, asel); t.restart();
      if (op == "weld") net.weld(std::atoi(argv[6]), std::atoi(argv[7]));
      else if (op == "dragweld") {                           // A dragged onto B (a commit), then welded — as with the mouse
        int a = std::atoi(argv[6]), b = std::atoi(argv[7]);
        auto rel = hm::releaseFrozenAround(sc, {a}); sc.anchor(a)->pos = sc.anchor(b)->pos;
        std::vector<int> touched{a}; for (auto& r : rel) touched.push_back(r.anchor);
        hs.apply(sc, hm::curvesTouching(sc, touched), false);
        hm::Network(sc, hs.curves, sel, asel).upkeep(netSig);
        hm::Network(sc, hs.curves, sel, asel).weld(a, b);
      }
      else if (op == "join") net.joinCurves(std::atoi(argv[6]), std::atoi(argv[7]));
      else if (op == "delkeep" || op == "del") { hm::Selection s; hm::edit::deleteAnchors(sc, hs.curves, s, {std::atoi(argv[6])}, op == "delkeep"); }
      else if (op == "recalc") hm::edit::recalcHandles(sc, hm::edit::anchorsOf(sc, {std::atoi(argv[6])}));
      else if (op == "wallspaths") { auto pc = hm::edit::pathCover(sc, hs.curves, {std::atoi(argv[6]), std::atoi(argv[7])}, true); QString m; if (pc) hm::edit::wallsAlongPaths(sc, hs.curves, *pc, &m); std::printf("%s\n", qPrintable(m)); }
      else if (op == "cocoon") { hm::Selection s; for (int i = 6; i < argc; i++) s.items.push_back(std::atoi(argv[i])); QString m; int k = hm::edit::makeLiveCocoon(sc, hs.curves, hs.cocoonSystem(), s, &m); std::printf("cocoon %d: %s\n", k, m.toUtf8().constData()); }
      else if (op == "average") std::printf("average curve %d\n", hm::edit::averageCurve(sc, hs.curves, std::atoi(argv[6]), std::atoi(argv[7])));
      else if (op == "fill") { int w = -1; QString m = hm::edit::fillHole(sc, hs.curves, {std::atoi(argv[6]), std::atoi(argv[7]), argc > 8 ? std::atoi(argv[8]) : -1}, &w); std::printf("fill: %s\n", qPrintable(m)); }
      else if (op == "insertkeep") { int a = net.insertKeepShape(std::atoi(argv[6]), std::atoi(argv[7]), std::atof(argv[8])); std::printf("new anchor %d\n", a); }
      else if (op == "insertseg") { int a = sc.addAnchor(hm::Vec3(std::atof(argv[8]), std::atof(argv[9]), std::atof(argv[10]))).id; net.insertOnSegment(std::atoi(argv[6]), std::atoi(argv[7]), a); std::printf("new anchor %d\n", a); }
      auto u = net.upkeep(netSig);
      std::printf("%s: upkeep cut %d, merged %d, joined %d, renamed %d (%lld ms)\n", argv[5], u.cut, u.merged, u.joined, int(u.renamed), t.elapsed());
      hs.load(sc);
    }
    if (argc >= 9 && QString(argv[4]) == "--move") {
      int a = std::atoi(argv[5]); hm::Vec3 d(std::atof(argv[6]), std::atof(argv[7]), std::atof(argv[8]));
      auto rel = hm::releaseFrozenAround(sc, {a});
      sc.anchor(a)->pos += d;
      std::vector<int> touched{a}; for (auto& r : rel) touched.push_back(r.anchor);
      t.restart();
      auto ch = hs.apply(sc, hm::curvesTouching(sc, touched), false);
      std::printf("moved %d: %zu handles released, %zu curves, %zu walls, %zu cocoons rebuilt (%lld ms)\n", a, rel.size(), ch.curves.size(), ch.walls.size(), ch.cocoons.size(), t.elapsed());
    }
    // CONVERT=<cocoon ids, comma-separated | all>: Convert to geometry — every mesh made (name, its points and faces in
    // full) into <dump>.mesh.txt, the same lines the HTML version's convertCocoons gives in the browser
    std::vector<int> selK;                                   // (the selection: Save writes its lock groups — convChecked)
    // LOCKWELD=1: Weld locks on (S.lockWeld); CHECK=1: Check connections first — its places into <dump>.check.txt
    if (qEnvironmentVariableIsSet("LOCKWELD")) sc.fileRaw.insert("lockWeld", qEnvironmentVariableIntValue("LOCKWELD") != 0);
    auto dumpPlaces = [&](FILE* m, const hm::LockWeldResult& lw) {
      std::fprintf(m, "merged %d doubled %d inside %d filled %d open %d\n", lw.merged, lw.doubled, lw.inside, lw.filled, lw.open);
      for (auto* L : {&lw.problems, &lw.accepted}) for (auto& p : *L) {
        std::fprintf(m, "%s %.3f %.3f %.3f %s |", p.ok ? "ok" : "bad", p.c.x, p.c.y, p.c.z, hm::weldIssueText(sc, hs, p).toUtf8().constData());
        for (auto& e : p.edges) std::fprintf(m, " %s", e.kind.toUtf8().constData()); std::fprintf(m, "\n");
      }
    };
    if (qEnvironmentVariableIsSet("CHECK")) {
      std::vector<int> s; QString v = qEnvironmentVariable("CHECK"); if (v != "1") for (const QString& x : v.split(',')) s.push_back(x.toInt());
      t.restart(); auto lw = hm::checkConnections(sc, hs, s);
      std::printf("check connections (%lld ms): %zu places\n", t.elapsed(), lw ? lw->problems.size() : 0);
      FILE* m = std::fopen((std::string(argv[3]) + ".check.txt").c_str(), "wb"); if (lw) dumpPlaces(m, *lw);
      std::fprintf(m, "weldOk %s\n", QJsonDocument(sc.fileRaw.value("weldOk").toArray()).toJson(QJsonDocument::Compact).constData()); std::fclose(m);
      // AUTOFIX=1: the report's Auto-fix on the places found — what it tried and kept into <dump>.autofix.txt; the
      // fixes stay on (SAVE writes that state)
      if (lw && qEnvironmentVariableIsSet("AUTOFIX")) {
        t.restart(); hm::AutoFixResult af = hm::weldAutoFix(sc, hs, lw->problems);
        std::printf("auto-fix (%lld ms): %zu fixes\n", t.elapsed(), af.fixes.size());
        FILE* m3 = std::fopen((std::string(argv[3]) + ".autofix.txt").c_str(), "wb");
        for (auto& l : af.trace) std::fprintf(m3, "%s\n", l.toUtf8().constData());
        for (auto& f : af.fixes) std::fprintf(m3, "fix %.3f %.3f %.3f [%d,%d] %s %s\n", f.c.x, f.c.y, f.c.z, f.ks[0], f.ks[1], f.op.kind.toUtf8().constData(), f.op.txt.toUtf8().constData());
        auto lw3 = hm::checkConnections(sc, hs, s); if (lw3) dumpPlaces(m3, *lw3);
        std::fclose(m3);
      }
      // IGNORE=<i>: the i-th place marked correct (its pin's Ignore), then checked again → <dump>.check2.txt
      if (lw && qEnvironmentVariableIsSet("IGNORE")) {
        int i = qEnvironmentVariableIntValue("IGNORE"); std::vector<hm::WeldProblem> all = lw->problems; all.insert(all.end(), lw->accepted.begin(), lw->accepted.end());
        hm::weldSpotSet(sc, hs, all.at(size_t(i)), !all[size_t(i)].ok);
        auto lw2 = hm::checkConnections(sc, hs, s);
        FILE* m2 = std::fopen((std::string(argv[3]) + ".check2.txt").c_str(), "wb"); if (lw2) dumpPlaces(m2, *lw2);
        std::fprintf(m2, "weldOk %s\n", QJsonDocument(sc.fileRaw.value("weldOk").toArray()).toJson(QJsonDocument::Compact).constData()); std::fclose(m2);
      }
    }
    if (qEnvironmentVariableIsSet("CONVERT")) {
      QString v = qEnvironmentVariable("CONVERT");
      if (v == "all") { std::vector<const hm::Item*> o; for (auto& [id, it] : sc.items) if (it.type == hm::ItemType::Cocoon) o.push_back(&it); std::sort(o.begin(), o.end(), [](auto* a, auto* b) { return a->order < b->order; }); for (auto* k : o) selK.push_back(k->id); }
      else for (const QString& s : v.split(',')) selK.push_back(s.toInt());
      t.restart();
      hm::ConvertResult cr = hm::convertCocoons(sc, hs, selK);
      std::printf("convert (%lld ms): %s\n", t.elapsed(), qPrintable(cr.message));
      if (cr.lockWeld) { FILE* c = std::fopen((std::string(argv[3]) + ".convcheck.txt").c_str(), "wb"); dumpPlaces(c, *cr.lockWeld); std::fclose(c); }
      FILE* m = std::fopen((std::string(argv[3]) + ".mesh.txt").c_str(), "wb");
      for (int id : cr.meshes) {
        const hm::Item* it = sc.item(id); const auto& P = it->poly->pos;
        std::fprintf(m, "mesh|%s|%d|%zu|%d\n", it->name.toUtf8().constData(), it->raw.value("fromCocoon").toInt(), P.size(), it->poly->faceCount());
        for (auto& p : P) std::fprintf(m, "%.4f %.4f %.4f\n", p.x, p.y, p.z);
        for (int f = 0; f < it->poly->faceCount(); f++) { for (int q = it->poly->faceStart[f]; q < it->poly->faceStart[f + 1]; q++) std::fprintf(m, q > it->poly->faceStart[f] ? " %d" : "%d", it->poly->faceVerts[q]); std::fprintf(m, "\n"); }
      }
      std::fclose(m);
    }
    // MERGETEST=1: Auto-fix's merge candidates at every child cocoon's curve ends (as if a place were there) into
    // <dump>.merge.txt; MERGEDO=<i> — the i-th of them done (weldFixDo), the problems left there after it
    if (qEnvironmentVariableIsSet("MERGETEST")) {
      FILE* mf = std::fopen((std::string(argv[3]) + ".merge.txt").c_str(), "wb");
      std::vector<std::pair<hm::FixOp, hm::Vec3>> all;
      std::vector<const hm::Item*> ko; for (auto& [id, it] : sc.items) if (it.type == hm::ItemType::Cocoon) ko.push_back(&it);
      std::sort(ko.begin(), ko.end(), [](auto* a, auto* b) { return a->order < b->order; });
      for (const hm::Item* C : ko) std::fprintf(mf, "# step %d %.4f group %d\n", C->id, hs.cocoonSystem()->loopStep(C->id), hs.cocoonSystem()->lockGroup(C->id));
      for (const hm::Item* C : ko) {
        const hm::Item* P = C->parent >= 0 ? sc.item(C->parent) : nullptr; if (!P || P->type != hm::ItemType::Cocoon) continue;
        std::vector<int> ends; for (const hm::Item* c : sc.curves()) for (int a : {c->anchors.front(), c->anchors.back()}) if (std::find(ends.begin(), ends.end(), a) == ends.end()) ends.push_back(a);
        for (int R : ends) { hm::WeldProblem p; p.c = sc.anchor(R)->pos; p.locks = {P->id, C->id};
          for (auto& op : hm::weldFixOps(sc, hs, p)) if (op.kind == "merge") { all.push_back({op, p.c}); std::fprintf(mf, "%d %d %d %d %d %s\n", C->id, R, op.B, op.R, op.X, op.txt.toUtf8().constData()); } }
      }
      if (qEnvironmentVariableIsSet("MERGEDO")) {
        auto& [op, c] = all.at(size_t(qEnvironmentVariableIntValue("MERGEDO")));
        bool ok = hm::weldFixDo(sc, hs, op);
        std::fprintf(mf, "done %d left %d\n", int(ok), hm::weldFixLeft(sc, hs, c, {op.P, op.C}));
      }
      std::fclose(mf);
    }
    // SAVE=<path>: the project written as the program saves it (after the op)
    if (qEnvironmentVariableIsSet("SAVE")) {
      hm::SaveExtras x; x.cocoons = hs.cocoonSystem(); x.convChecked = hm::convChecked(sc, hs, selK); QString err;
      bool ok = hm::saveProject(qEnvironmentVariable("SAVE"), sc, x, &err);
      std::printf("saved: %s %s\n", ok ? "ok" : "FAILED", qPrintable(err));
    }
    auto& cc = hs.curves;
    std::vector<hm::WallMesh> walls; for (auto& [id, w] : hs.walls) walls.push_back(w);
    std::printf("walls %zu\n", walls.size());
    FILE* f = std::fopen(argv[3], "wb");                     // (no \r: compared line by line with the browser's dump)
    std::sort(walls.begin(), walls.end(), [](auto& a, auto& b) { return a.surfaceId < b.surfaceId; });
    for (auto& w : walls) {
      size_t n = w.pos.size(), m = n / 2;
      std::fprintf(f, "%d %zu %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", w.surfaceId, n, w.pos[0].x, w.pos[0].y, w.pos[0].z,
                   w.pos[m].x, w.pos[m].y, w.pos[m].z, w.pos[n - 1].x, w.pos[n - 1].y, w.pos[n - 1].z);
    }
    std::fprintf(f, "#curves\n");
    std::vector<int> ids; for (auto& [id, s] : cc) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) { auto& s = cc.at(id); size_t m = s.p.size() / 2; std::fprintf(f, "%d %zu %.3f %.3f %.3f\n", id, s.p.size(), s.p[m].x, s.p[m].y, s.p[m].z); }
    // cocoons: id, ring count, sum / first / last ring place, segments on the lock, the sum of their coordinates
    std::vector<hm::CocoonResult> ks; for (auto& [id, k] : hs.cocoons()) ks.push_back(k);
    std::printf("cocoons %zu\n", ks.size());
    std::fprintf(f, "#cocoons\n");
    for (auto& k : ks) {
      double ss = 0; for (auto& r : k.rings) ss += r.s;
      double cs = 0; for (auto& p : k.lockSegs) cs += p.x + p.y + p.z;
      std::fprintf(f, "%d %zu %.3f %.3f %.3f %zu %.1f\n", k.id, k.rings.size(), ss, k.rings.empty() ? 0 : k.rings.front().s,
                   k.rings.empty() ? 0 : k.rings.back().s, k.lockSegs.size() / 2, cs);
    }
    // DETAIL=<cocoon id>: its shape and rings in full
    if (qEnvironmentVariableIsSet("DETAIL")) {
      int id = qEnvironmentVariableIntValue("DETAIL"); auto it = hs.cocoons().find(id);
      if (it != hs.cocoons().end()) {
        auto& k = it->second; double sa = 0, sb = 0; for (int i = 0; i < k.geo.N; i++) { sa += k.geo.a[i]; sb += k.geo.b[i]; }
        std::fprintf(f, "#detail %d N %d c0 %.3f %.3f %.3f cN %.3f %.3f %.3f sa %.3f sb %.3f\n", id, k.geo.N, k.geo.c[0].x, k.geo.c[0].y, k.geo.c[0].z,
                     k.geo.c.back().x, k.geo.c.back().y, k.geo.c.back().z, sa, sb);
        for (auto& r : k.rings) std::fprintf(f, "ring %.3f %s%s%s%s\n", r.s, r.par ? "par " : "", r.run.c_str(), r.pp ? " pp" : "", r.snap ? " snap" : "");
      }
    }
    // the network: every curve (id|name|parent|anchors|frozen handles|seg) and wall (id|name|parent|a|b)
    std::fprintf(f, "#net\n");
    for (auto& [id, it] : sc.items) {
      if (it.type == hm::ItemType::Curve) {
        std::string an; for (size_t i = 0; i < it.anchors.size(); i++) { if (i) an += ','; an += std::to_string(it.anchors[i]); }
        std::fprintf(f, "C|%d|%s|%d|%s|%zu|%d\n", id, qPrintable(it.name), it.parent, an.c_str(), it.ovr.size(), int(it.seg));
      } else if (it.type == hm::ItemType::Surface) std::fprintf(f, "S|%d|%s|%d|%d|%d\n", id, qPrintable(it.name), it.parent, it.a, it.b);
    }
    std::fclose(f);
  }
  return 0;
}
