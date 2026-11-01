// Curve math ported from the HTML version (bez, autoTangent, curveHandles, sampleCurve ...): a curve is a chain of cubic
// Bezier segments through its anchors; the handles come from the neighbours, the curves sharing the anchor (welded
// trunks, Y-junctions, new-logic nodes), Roundness (c.round), Smoothing (c.fair) and Alt-dragged overrides (c.ovr).
#pragma once
#include "scene/Scene.h"
#include <array>
#include <vector>

namespace hm {

using BezSeg = std::array<Vec3, 4>;
struct CurveSample { Vec3 p; int seg = 0; };
struct Handles { Vec3 hin, hout; };

Vec3 bez(const BezSeg& s, double t);
Handles curveHandles(const Scene& sc, const Item& c, int i);
std::vector<BezSeg> curveSegs(const Scene& sc, const Item& c);
std::vector<CurveSample> sampleCurve(const Scene& sc, const Item& c, int per = 24);

}  // namespace hm
