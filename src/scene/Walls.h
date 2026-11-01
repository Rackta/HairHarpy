// NURBS walls (item 'surface': a wall between curves a and b) as the HTML version builds them for the viewport:
// wallShapeRows — the wall's own adaptive rows along both curves (the new logic: the shape only, no loops), then
// surfaceData — a grid of quads between them with Width segments and Bulge (and the twin wall back, p.b2on).
// (Only the new logic: old-logic projects aren't supported.)
#pragma once
#include "scene/Scene.h"
#include <array>
#include <set>
#include <unordered_map>
#include <vector>

namespace hm {

// every curve sampled as rebuildCurve does: 24 points a segment + the cumulative arc length
struct CurveSampling { std::vector<Vec3> p; std::vector<int> seg; std::vector<double> arc; };
using CurveCache = std::unordered_map<int, CurveSampling>;
CurveCache sampleAllCurves(const Scene& sc);

struct WallMesh {
  int surfaceId = 0;
  std::vector<Vec3> pos;
  std::vector<std::array<int, 4>> quads;     // (a quad collapsed at a pointed end keeps its 4 indices)
  std::vector<int> vrow;                     // each vertex's row (an edge within one row runs across the lock)
  // the grids in pos: the wall's own, then its twin's (back from b to a) — R rows of M points from pos[start]
  struct Grid { int start = 0, R = 0, M = 0; bool twin = false; };
  std::vector<Grid> grids;
};

// polyFlatIndex: the triangles of polygons as the HTML version cuts them (each polygon's fan from its own "provoking"
// corner) — anything that intersects the wall triangles (the cocoon rings on the lock) must cut the same diagonals
std::vector<uint32_t> polyFlatIndex(const std::vector<std::vector<int>>& faces, int nv);

double projectStep(const Scene& sc);
double usedStep(const Scene& sc);           // the step most walls' groups use (0: none)         // defaultStep: settings.step, else the step most walls' groups use, else headH / 24
double loopStep(const Scene& sc, const Item& it);   // convOf(it).step

// every visible wall's mesh (one per surface item; a twin wall is appended to its surface's mesh); only — just these
// surfaces; draft — the coarser row search used while anchors are dragged
std::vector<WallMesh> buildWalls(const Scene& sc, const CurveCache& cc, bool includeHidden = false,
                                 const std::set<int>* only = nullptr, bool draft = false);
// one curve sampled as rebuildCurve does
CurveSampling sampleCurveFull(const Scene& sc, const Item& c);

}  // namespace hm
