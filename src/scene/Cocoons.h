// ◍ Cocoons (item 'cocoon'), ported from the HTML version: a see-through shell of a lock's shape (k.shape — per station
// along it: c the spine point, u the oval's long axis, a / b its radii) with rings where the loops will be. A live
// cocoon (k.live — the strands as root / tip anchor pairs) is reshaped from its strands' curves as they are now
// (cocoonLivePolys → cocoonShape). Rings (cocoonRings): the loop step and Deviation of its lock's group — closer on bends
// and toward the tip; inside its parent / linked cocoons their rings' places; Density (painted), Parent influence,
// guide lines and scene discs. ringMode 'lock': the rings projected onto the lock's walls (cocoonLockRingSegs).
#pragma once
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <QJsonArray>
#include <QJsonObject>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hm {

struct CocoonGeo {
  std::vector<Vec3> c, u, v, t;
  std::vector<double> a, b, D;
  int N = 0;
};

struct CocoonRing {
  double s = 0;                          // the place along it (arc length of the spine)
  Vec3 p, n;                             // the ring's plane on the cocoon
  std::optional<Vec3> pp, pn;            // the plane it is drawn with on the lock (a source's loop)
  bool par = false, fold = false, tail = false;
  std::optional<int> ri, prio;
  std::string run;                       // a source's stretch ('prio:i'); empty — none
  struct Snap { Vec3 pp, pn; double a; };
  std::optional<Snap> snap;
  std::optional<double> lim, gap;
};

struct CocoonResult {
  int id = 0;
  CocoonGeo geo;
  std::vector<CocoonRing> rings;
  std::vector<Vec3> lockSegs;            // ringMode 'lock': pairs of points — the rings on the lock's walls
  bool lockMode = false;
};

// what Convert to geometry reads of a cocoon (ringMeshOf): its spine, loop step, rings with the tail rings (by their
// place along it), the lock's walls and its tip — the point all its strands end at, only where they meet in one
struct LockInput {
  int id = 0;
  CocoonGeo G;
  double h = 0;
  std::vector<CocoonRing> rings;
  struct Wall { int id, a, b; };                // cocoonLockWalls: the surface and its two curves
  std::vector<Wall> walls;
  std::vector<Vec3> tipP;
};

// every cocoon (live ones reshaped first); walls: buildWalls(…, includeHidden = true) — the rings cut every wall
// a cocoon's station at arc length s (cocoonF), a point of its shell (cocoonPt); CMASK_N — a mask's weights along it
double cocoonStation(const CocoonGeo& G, double s);
Vec3 cocoonPoint(const CocoonGeo& G, int i, double th);
constexpr int CMASK_N = 64;

std::vector<CocoonResult> buildCocoons(const Scene& sc, const CurveCache& cc, const std::vector<WallMesh>& walls);

// The cocoons kept between edits (their reshaped shapes, rings, strands): reads the scene, the curve samples and the
// walls by reference — they must outlive it and be updated in place
class CocoonSystem {
public:
  CocoonSystem(const Scene& sc, const CurveCache& cc, const std::map<int, WallMesh>& walls);
  ~CocoonSystem();
  void rebuildAll();
  std::set<int> update(const std::set<int>& changedCurves);   // → the cocoons rebuilt
  void rebuild(const std::set<int>& ids);                       // these and the ones taking loops from them
  const std::map<int, CocoonResult>& results() const;
  // what a project file keeps of a cocoon as it is now: its shape { c, u, a, b } and, a live one, its strands' ends
  bool liveState(int id, QJsonObject& shape, QJsonArray& ends) const;
  // Convert to geometry
  bool lockInput(int id, LockInput& out);
  std::vector<int> deps(int id);                                // the cocoons it takes loops from: its parent first, its links
  bool dependsOn(int a, int b);                                 // a takes loops from b, any steps away
  int lockGroup(int id);                                        // its lock's group (-1: none)
  double loopStep(int id);                                      // cocoonConv(k).step || defaultStep()
  CocoonGeo geo(int id);                                        // cocoonGeo (empty: no shape)
  std::vector<int> strandCurves(int id);                        // a live cocoon's strands' curves (cocoonLivePolys().cids)
  // a would-be live cocoon (its strands' ends, its lock's curves): has it a shape (the strands run side by side), and the
  // cocoon it grows out of (cocoonHost: its root inside one it runs along the farthest; -1 — a trunk of its own)
  bool trialShape(std::vector<std::vector<int>>& ends, const std::vector<int>& lock, int* host, QJsonObject* shape = nullptr);
  // loops forced in where the lock folds (cocoonFoldPass; k._foldS): the cocoon's rings and those of every cocoon taking
  // loops from it are made again with them; {} — none
  void setFolds(int id, const std::vector<double>& s);
  const std::vector<double>& folds(int id);
  std::vector<Vec3> guideLine(int guide);                       // how a line guide is shown (a ring round its cocoon; {} — not)
  struct ZoneLine { Vec3 a, b; double wa, wb; };
  std::vector<ZoneLine> zone(int guide, double rad, double blend);   // a guide's influence zone (rad — in the scene's units)
private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

}  // namespace hm
