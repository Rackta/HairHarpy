// Walls fitted to a mesh (Lock lines: the walls between the lock's curves follow its high-poly): each wall's Width
// segments, Bulge and the bulge's profile along it (p.bprof) worked out so the wall lies on the mesh's surface.
#pragma once
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <vector>

namespace hm::wallfit {

// → how many walls got a bulge; maxBulge — the Bulge's limit (either way)
int fitToMesh(Scene& sc, const CurveCache& cc, const std::vector<int>& walls, const PolyMesh& m, double maxBulge = 3);

}  // namespace hm::wallfit
