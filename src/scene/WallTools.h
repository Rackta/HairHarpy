// Wall tools of the Settings window (the HTML version's segmentsToCurves, wallKinks): ✂ Width segments → curves — every
// inner column line of a wall (with its bulge) becomes a curve, the wall replaced by a chain of flat one-segment walls;
// ⚠ dents — an anchor of a side jutting in toward the other side (the wall dents there; shown on the selected walls).
#pragma once
#include "scene/Display.h"
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <QString>
#include <map>
#include <vector>

namespace hm::walltools {

QString segmentsToCurves(Scene& sc, const CurveCache& cc, const std::map<int, WallMesh>& walls, Selection& sel, const std::vector<int>& list);

struct Kink { int a, w, c, i; double turn, ex, score; };   // the anchor, its wall, the curve it is on (index i)
std::vector<Kink> wallKinks(const Scene& sc, const CurveCache& cc, int w);

}  // namespace hm::walltools
