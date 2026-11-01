// The Lock Extrude tool's geometry (the Hair room): a flat lock — a chain of curves joined by walls — extruded into a
// volume: a copy of its curves offset along the walls' normal (away from the head or toward it), the sides closed by a
// wall each. The segments are curves in the middle of the copied side, standing out further than it (one — a pentagon
// across, more — a rounded back). The tips are the curves' own (welded), the roots their own. The thickness narrows
// toward the root and toward the tip as asked.
#pragma once
#include "scene/Scene.h"
#include "scene/Walls.h"
#include <array>
#include <vector>

namespace hm::lockex {

struct Params {
  double d = 0;          // the thickness: × the lock's mean width (+ away from the head, − toward it)
  int segs = 1;          // the back's segments: curves in its middle = segs − 1
  double root = 0;       // 0 … 1: how much the thickness narrows at the root (1 — to nothing there)
  double tip = 0.3;      // the share of the length over which the thickness narrows to the tip
};

// one curve offset from a curve of the lock: its anchors that move (all but the welded tip), where they start from
struct Layer { int id = -1; std::vector<int> moving; std::vector<Vec3> base, nrm; std::vector<double> u; double t = 0; };   // (t: its place across the lock, 0 … 1)

struct Built {
  std::vector<int> orig;                                     // (the lock's curves, in a row)
  std::vector<Layer> copy;                                   // (orig[i] → copy[i], offset fully)
  std::vector<Layer> mids;                                   // (the back's curves in between the copies)
  std::vector<int> backWalls, sideWalls;                     // (along the back — the copies and the mids in a row; the two sides)
  double width = 1;                                          // (the lock's mean width)
  int segs = 1;                                              // (the side structure made for)
};

// the flat locks in the selection: chains of curves joined by walls — the selected walls, the walls between selected
// curves, every wall of a selected group (rings — volume locks — and forks left out)
std::vector<std::vector<int>> strips(const Scene& sc, const std::vector<int>& items);
// the strip of the wall / curve id: its whole chain
std::vector<int> stripOf(const Scene& sc, int id);
// the copy made: curves, walls; the back with segs segments (into the first curve's parent)
Built create(Scene& sc, const CurveCache& cc, const std::vector<int>& strip, const Vec3& headCenter, int segs);
// the back made again for segs segments (→ true: curves came / went)
bool setSegs(Scene& sc, const CurveCache& cc, Built& B, int segs);
// the copies' and the side curves' anchors placed for P
void apply(Scene& sc, const Built& B, const Params& P);

}  // namespace hm::lockex
