// Convert to geometry (the HTML version's convertCocoons): a mesh for each selected cocoon's lock, made from its rings
// (ringMeshOf). The lock's walls are grids: rows along it, columns across (column 0 — curve a, the last — curve b), each
// column a line from the root to the tip. The mesh's rows: the root, every ring that crosses the walls' columns — one
// contour round the lock on each (or one strip, a lock of one surface), the contours zipped ring to ring (forks: a
// contour into two and back) — then the tip. Points at one place become one, Auto weld points (S.weld) join what is
// closer than the weld distance, the normals face out from the cocoon's axis. Where the lock folds between two rings far
// apart a loop is forced in between (cocoonFoldPass) and the mesh made again. The meshes go into the Meshes group, the
// locks' groups are hidden, the cocoons' rings switched off.
// Weld locks (S.lockWeld) on: the converted locks that touch made one mesh (weldLocks).
#pragma once
#include <optional>
#include "scene/HairState.h"
#include "scene/Scene.h"
#include <QString>
#include <QStringList>
#include <set>
#include <vector>

namespace hm {

struct WeldSettings { bool on = true; double dist = 0; };      // S.weld (dist 0: a quarter of the cocoon's loop step)
WeldSettings weldSettings(const Scene& sc);
int meshesGroup(Scene& sc);
int meshHierarchy(Scene& sc);                          // (Meshes always there, Imported inside it)                                     // the Meshes group (made when missing)
double sceneScale(const Scene& sc);                            // S.scale: the head's box, its largest side

struct LockMesh {
  std::vector<Vec3> pos;
  std::vector<std::vector<int>> faces;
  int dropped = 0, flips = 0, welded = 0, rows = 0;
  struct Fold { int a, b; };                                   // two rings (indices into ringList) the lock folds between
  std::vector<Fold> folds;
  std::vector<CocoonRing> ringList;
  QString err;
};

LockMesh ringMeshOf(const LockInput& in, const std::map<int, WallMesh>& wallOf, const WeldSettings& weld, double scale);

// the loops forced in where the locks fold (sources first) → the cocoons given loops; cocoonFoldClear — they go again
std::set<int> cocoonFoldPass(HairState& hs, const std::vector<int>& ks, const WeldSettings& weld, double scale);
void cocoonFoldClear(HairState& hs, const std::set<int>& hit);

// ---- Weld locks (S.lockWeld): the converted locks that touch become one surface ----
// (1) points of two related locks (a cocoon and its child, or two linked ones) closer than 0.1 of the loop step → one,
// the source's point staying; (1b) a wall the two share a little off — reported ('off'), welded only where approved
// (S.weldSnap); (2) faces of two locks on the same points (the wall they share) — both go; (3) faces inside another lock
// go; (4) the holes left (up to 16 edges, not a lock's own open root) closed. Locks joined by a welded point are one mesh.
// The places where two locks don't meet (an edge left open, edges of 3+ faces, a child apart from its parent) are the
// problems Check connections shows; S.weldOk — the places marked correct ("Ignore": nothing welded there; 'off' — only
// not reported).
struct WeldProblem {
  Vec3 c;                                                      // the place (the middle of its edges)
  struct Edge { Vec3 a, b; QString kind; };                    // kind: open | many | off | apart
  std::vector<Edge> edges;
  std::vector<int> locks;                                      // the cocoons there
  bool ok = false;                                             // a place marked correct (green)
  int fix = -1;                                                // an Auto-fix under review here (its index): blue / orange
};
struct LockWeldResult {
  struct Group { std::vector<int> fam; std::vector<Vec3> pos; std::vector<std::vector<int>> faces; };
  std::vector<Group> groups;
  int merged = 0, doubled = 0, inside = 0, filled = 0, open = 0;
  std::vector<WeldProblem> problems, accepted;
};
// the locks (sources first): each cocoon's id and its ringMeshOf mesh; S.weldOk's places without a kind are classified
// (and written back into the project)
struct LockIn { int id; const LockMesh* m; };
LockWeldResult weldLocks(Scene& sc, HairState& hs, const std::vector<LockIn>& list);

// Check connections: the selected cocoons' locks (none selected — all) made and welded as Convert would (nothing is
// made) → the places where two locks don't meet; nullopt — no lock meshes
std::optional<LockWeldResult> checkConnections(Scene& sc, HairState& hs, const std::vector<int>& sel);
// "Ignore" (on) / "Weld again" (off) on a place: S.weldOk
void weldSpotSet(Scene& sc, HairState& hs, const WeldProblem& sp, bool on);
QString weldIssueText(const Scene& sc, HairState& hs, const WeldProblem& p);

struct ConvertResult {
  std::vector<int> meshes;                                     // the mesh items made
  int welded = 0, flips = 0, dropped = 0, folds = 0;
  QStringList bad, folded;
  QString message;                                             // the toast
  std::optional<LockWeldResult> lockWeld;                      // Weld locks on: its result (the problems to show)
};
// sel: the selected items (the cocoons among them are converted, in this order)
ConvertResult convertCocoons(Scene& sc, HairState& hs, const std::vector<int>& sel, const Vec3& center = {});   // (center: the braids' frames)
// S.convChecked: the lock groups of the selected cocoons (saved in the project's settings)
std::vector<int> convChecked(const Scene& sc, HairState& hs, const std::vector<int>& sel);

}  // namespace hm
