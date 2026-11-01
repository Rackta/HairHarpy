// Edits of the Hair room's curves and items, ported from the HTML version (Tools / State): deleting anchors (keep shape —
// the neighbours' handles fitted to the old shape), recalculating handles, merging anchors at one place, unwelding an
// anchor from the selected curves, deleting items, a new curve's look. Each works on the scene; the caller rebuilds the
// geometry and commits.
#pragma once
#include "scene/Display.h"
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <QString>
#include <optional>
#include <vector>

namespace hm { class CocoonSystem; }

namespace hm::edit {

std::vector<int> curvesWith(const Scene& sc, int anchor);           // curves().filter(c => c.anchors.includes(a))
bool isRingAnchor(const Scene& sc, int anchor);                     // a disc guide's ring point
// least squares handle lengths along d1 / d2 for a Bezier from P0 to P3 through pts (fitHandles)
std::pair<double, double> fitHandles(const std::vector<Vec3>& pts, const Vec3& P0, const Vec3& P3, const Vec3& d1, const Vec3& d2);

void removeAnchorFromCurve(Scene& sc, Item& c, int a, bool keep);
// → a message for the toast (empty: done); the selection: the edited curves
QString deleteAnchors(Scene& sc, const CurveCache& samples, Selection& sel, std::vector<int> list, bool keep, int drawing = -1);
void recalcHandles(Scene& sc, const std::vector<int>& anchors);     // (recalcHandles / recalcSilent without the commit)
std::vector<int> anchorsOf(const Scene& sc, const std::vector<int>& curves);
int mergeCoincident(Scene& sc, const CurveCache& samples, Selection& sel, const std::vector<int>& curves, double tol);
// Ctrl + click on a welded anchor of a selected curve: that curve gets its own anchor there → the curves unwelded (none —
// not such an anchor)
std::vector<int> unweldAnchor(Scene& sc, int a, const std::vector<int>& selItems, std::vector<int>* made = nullptr);
void deleteItems(Scene& sc, const CurveCache& samples, Selection& sel);   // the selected items (deleteSelected)

// ---- walls ----
bool hasSurface(const Scene& sc, int c1, int c2);
int makeSurface(Scene& sc, int c1, int c2);                         // (-1: one is there already)
// walls between consecutive curves (closed — last → first too); not exact: two trunks — between the facing branches.
// → the walls made; msg — the toast
std::vector<int> createSurfaces(Scene& sc, const CurveCache& samples, std::vector<int> list, bool closed, bool exact, QString* msg);
bool twoTrunks(const Scene& sc, const CurveCache& samples, const std::vector<int>& list);   // resolveTrunks changes it
// new logic: the paths through the forks
struct Step { int c, from; };
using Path = std::vector<Step>;
std::vector<Path> pathsFor(Scene& sc, const CurveCache& samples, const std::vector<int>& sel, bool all = false);
std::optional<std::vector<Path>> pathCover(Scene& sc, const CurveCache& samples, const std::vector<int>& sel, bool loose = false);
std::vector<int> wallsAlongPaths(Scene& sc, const CurveCache& samples, const std::vector<Path>& paths, QString* msg);
std::vector<Vec3> pathPts(const Scene& sc, const CurveCache& samples, const Path& p);
QString fillCycle(Scene& sc, const CurveCache& samples, const std::vector<int>& P, const std::vector<int>& V, int parent, int* wall);
QString fillHole(Scene& sc, const CurveCache& samples, const std::vector<int>& curves, int* wall);   // (msg)
int averageCurve(Scene& sc, const CurveCache& samples, int c1, int c2);
bool isNewLogic(const Scene& sc, const Item& it);
bool segNetCurve(const Item& c);

// ---- cocoons ----
// the lock the selection names: the selected curves, the selected walls' curves, a selected group's curves (an edge
// curve — the curves it runs along)
std::vector<int> lockCurves(const Scene& sc, const Selection& sel);
// its strands: [root, tip, …the anchors between] — the paths through the forks lying on its curves; a lock splitting into
// branches — its main branch only (dropped: how many strands of the others were left out)
std::vector<std::vector<int>> lockEnds(Scene& sc, const CurveCache& samples, const std::vector<int>& cs, int* dropped = nullptr);
// a group's loop settings (groupConv): its own conv, the step and the missing keys filled from the project's — kept in
// the group (g null: the project's alone)
QJsonObject groupConv(Scene& sc, Item* g);
// the counters of the default names ("Curve N") after renaming: each the highest number in use + 1 (nextNumber)
void refreshCounters(Scene& sc);
// nextNumber(type): the highest "<Type> N" in use + 1, kept in the project's counters (a new item's default number)
int nextNumber(Scene& sc, ItemType t);
// Even out anchors (c.asm 0…1): Taubin smoothing of the anchors, always from the positions before (c.asrc) — the slider
// can go back; ends, anchors shared with other curves and attached ones stay → the anchors moved
std::vector<int> evenAnchors(Scene& sc, const std::vector<int>& curves, double v, double scale);
// ⛓ Detach from neighbouring groups: the group's curves get anchors of their own where they share them with curves of
// other groups (and ends attached to another group's connector are let go) → the message ("!…" — nothing done)
QString detachGroup(Scene& sc, int gid, Selection& sel);
// ↻ Match project loop step: the group and its sub-groups take the project's loop step
void matchProjectStep(Scene& sc, int gid);
// ◍ the Cocoon button: a cocoon of the selected lock that keeps its shape → its id (-1: none, msg says why)
int makeLiveCocoon(Scene& sc, const CurveCache& samples, CocoonSystem* cs, Selection& sel, QString* msg);

// a new curve the user makes: Roundness 60 %, Smoothing 50 %
Item& makeUserCurve(Scene& sc, int parent);
int targetGroup(const Scene& sc, const Selection& sel);             // where new curves go (-1: the root)

}  // namespace hm::edit
