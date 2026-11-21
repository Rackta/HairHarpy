// Lock lines: the curves of a lock found on its mesh — where the mesh is marked. Three ways of marking (the modes):
//   Polypaint — the contrast lines painted on the vertices (ZBrush polypaint);
//   Hard edges — the edges the mesh keeps hard;
//   Creases — the edges creased for subdivision.
// A painted line is some vertices wide: it is thinned to the line through its middle (ordered thinning on the mesh: the vertices
// nearest the line's border go first, a vertex goes only when the line stays whole), then the skeleton's branches are traced between
// its ends and junctions, short spurs cut off, ends that lie close joined. The lock's ends are found on the mesh (the two points
// farthest apart along its surface); the root is the end the lines start apart at, the tip — where they come together. Every line is
// given root → tip. Not the mesh's quads' business: any polygons do.
#pragma once
#include "scene/Scene.h"
#include <QString>
#include <vector>

namespace hm::locklines {

enum class Mode { Polypaint, HardEdges, Creases };

struct Opt {
  Mode mode = Mode::Polypaint;
  double contrast = 0.5;      // polypaint: how far a vertex's brightness must lie from the paint's background (0…1)
  double spur = 0.02;         // the shortest branch kept that ends free next to a junction (the share of the lock's length)
  double stitch = 0.008;     // ends / junctions nearer than this are one — a gap in a line (the share of the lock's length)
  double minLine = 0.2;      // lines shorter than this are dropped (the share of the lock's length)
  bool swapEnds = false;      // root ↔ tip, when the guess is wrong
  int smooth = 12;            // smoothing passes of each line
};

struct Line { std::vector<Vec3> pts; int a = -1, b = -1; };   // root → tip; a / b — the nodes at its ends (shared by the lines meeting there)
struct Result {
  std::vector<Line> lines;
  std::vector<Vec3> nodes;
  std::vector<double> nodeDr; // each node's distance from the root along the lock
  Vec3 root, tip; bool hasEnds = false;
  double length = 0;          // the lock's length along its surface
  int inkVertices = 0;
  QString error;
  QString info;
};

Result trace(const PolyMesh& m, const Opt& o);

}  // namespace hm::locklines
