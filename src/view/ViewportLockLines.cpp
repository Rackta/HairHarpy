// The Lock lines tool: the lock's curves generated from its mesh (LockLines.h) — polypaint, hard edges or creases; optionally with a cocoon.
#include "scene/Edit.h"
#include "scene/AnchorOps.h"
#include "scene/MeshCurves.h"
#include "scene/Network.h"
#include "scene/WallFit.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QCursor>

namespace hm {

void Viewport::lockLinesRun(int mesh) {
  const Item* it = scene_->item(mesh);
  if (!it || !it->poly) { ui::toast(tr("Select an imported mesh first")); return; }
  // the waiting window («Let me think…») over the whole generation: the lines, the curves, the walls
  ui::ThinkScope think(tr("Lock lines: finding the lines on %1 (%2 polygons)…").arg(it->name).arg(it->poly->faceCount()), 0); think.now();
  QApplication::setOverrideCursor(Qt::WaitCursor);
  const locklines::Result r = locklines::trace(*it->poly, llOpt);
  QApplication::restoreOverrideCursor();
  if (!r.error.isEmpty()) { ui::toast(r.error); return; }
  if (r.lines.empty()) { ui::toast(tr("No lines found — try another Contrast, or fewer cut / joined parts")); return; }
  std::vector<std::vector<Vec3>> pl; std::vector<std::array<int, 2>> ends; for (const auto& L : r.lines) { pl.push_back(L.pts); ends.push_back({L.a, L.b}); }
  const std::vector<Vec3> nodes = r.nodes; const QString name = it->name + tr(" → lines"); const bool cocoon = llCocoon, bulge = llBulge, ring = llRing;
  const std::shared_ptr<PolyMesh> hp = it->poly;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  think.progress(0.6, tr("Curves from %1 lines…").arg(r.lines.size()));
  ui::ThinkScope* tp = &think;
  runEdit([pl, ends, nodes, name, bulge, ring, hp, tp](Scene& s, Selection& sl, const CurveCache&) {
    auto made = meshc::nodeLinesToCurves(s, pl, ends, nodes, name);
    if (made.empty()) return QString("!") + QObject::tr("Nothing to make curves of");
    const int grp = s.item(made[0])->parent;
    auto inGroup = [&s, grp] { std::vector<int> v; for (int id : s.childrenOf(grp)) if (const Item* c = s.item(id); c && c->type == ItemType::Curve && !c->hasPath()) v.push_back(id); return v; };
    { std::vector<int> si, sa; CurveCache none; Network net(s, none, si, sa); net.segmentNetwork(); s.reindex(); made = inGroup(); }   // (the network curves: one tangent at the nodes)
    tp->progress(0.7, QObject::tr("Optimizing the anchors…"));
    // ⚙ Optimize anchors (the shape kept), then ▦ the walls along the paths through the forks — as the curves' menu makes them
    aops::optimizeAnchors(s, sampleAllCurves(s), made, nullptr, 0.5);   // (half the usual tolerance: the shape kept closer — no waves on the long curves)
    made = inGroup();
    tp->progress(0.8, QObject::tr("Walls between the curves…"));
    std::vector<int> walls; QString wm;
    { const CurveCache cc = sampleAllCurves(s);
      if (auto pc = edit::pathCover(s, cc, made, true); pc && pc->size() >= 2) walls = edit::wallsAlongPaths(s, cc, *pc, &wm, ring); }
    int bulged = 0;
    if (bulge && !walls.empty()) tp->progress(0.88, QObject::tr("Bulging the walls to the mesh…"));
    if (bulge && !walls.empty()) bulged = wallfit::fitToMesh(s, sampleAllCurves(s), walls, *hp, 3);
    sl.anchors.clear(); sl.items = made;
    QString msg = QObject::tr("%1 curves from the lines of %2").arg(made.size()).arg(name);
    if (!walls.empty()) msg += bulge ? QObject::tr(", %1 walls (%2 bulged to the mesh)").arg(walls.size()).arg(bulged) : QObject::tr(", %1 walls").arg(walls.size());
    return msg; }, true);
  QApplication::restoreOverrideCursor();
  if (cocoon) {
    think.progress(0.95, tr("The cocoon…"));
    CocoonSystem* cs = hair_.cocoonSystem();
    runEdit([cs](Scene& sc, Selection& sel, const CurveCache& cc) { QString m; const int k = edit::makeLiveCocoon(sc, cc, cs, sel, &m); return k < 0 ? "!" + m : m; });
  }
}

}  // namespace hm
