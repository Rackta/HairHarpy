// The Hair room's built geometry, kept between edits: every curve's samples, every wall's mesh, the cocoons. An edit
// marks curves changed (markAnchorsDirty) and only what depends on them is rebuilt, as the HTML version's frame does:
// the walls on those curves (in draft while anchors are dragged), the live cocoons whose strands run along them and the
// cocoons taking loops from those.
#pragma once
#include "scene/Cocoons.h"
#include "scene/Walls.h"
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace hm {

struct HairChanges { std::set<int> curves, walls, cocoons; bool any() const { return !curves.empty() || !walls.empty() || !cocoons.empty(); } };

class HairState {
public:
  CurveCache curves;
  std::map<int, WallMesh> walls;                     // surface id → mesh (hidden walls too: the cocoons' rings cut them)

  void load(const Scene& sc);
  // curves changed: their samples, the walls on them (draft: the coarse row search of a drag), the cocoons
  HairChanges apply(const Scene& sc, const std::set<int>& changedCurves, bool draft);
  // a drag over: the walls built in draft built right, and the cocoons it touched rebuilt on them
  HairChanges finishDraft(const Scene& sc);
  // walls' settings changed (segments, bulge, twin …): those walls, and the cocoons on their locks
  HairChanges rebuildWalls(const Scene& sc, const std::set<int>& walls);
  const std::map<int, CocoonResult>& cocoons() const;
  const CocoonSystem* cocoonSystem() const { return cocoons_.get(); }
  CocoonSystem* cocoonSystem() { return cocoons_.get(); }

private:
  std::unique_ptr<CocoonSystem> cocoons_;
  std::set<int> draftWalls_, draftCocoons_;
};

// markAnchorsDirty: the curves holding the anchors, those sharing anchors with them (a Y junction's tangents depend on
// the neighbours — not through an edge curve), and the edge curves running along any of them
std::set<int> curvesTouching(const Scene& sc, const std::vector<int>& anchors);

// releaseFrozenAround: as a move starts, the frozen handles (c.ovr) of the moved anchors whose neighbours stay, and of
// the two anchors on from them each side, become automatic again → what was removed (for undo)
struct RemovedOverride { int curve, anchor; HandleOverride h; };
std::vector<RemovedOverride> releaseFrozenAround(Scene& sc, const std::vector<int>& moved);

int groupLogic(const Scene& sc, const Item& it);

}  // namespace hm
