// Edges of a polygon mesh (imported / converted — its own polygons, not the triangles it is drawn with) and what is
// made from them: the edge loop through an edge (up to a pole), the shortest path between two edges, the selected edges
// → curves, and "Mesh → curves & surfaces" — every connected part of the mesh taken for a lock: curves along its edges
// (the creases), walls between them. Ported from the HTML version (edgeTopo, edgeLoop, edgePath, edgeChains,
// fitPolyline, loopFamilies, lockTree, lockFromQuads, lockFromSlices, buildLockCurves, meshToCurves).
#pragma once
#include "scene/Display.h"
#include "scene/Scene.h"
#include <QString>
#include <memory>
#include <unordered_map>
#include <vector>

namespace hm::meshc {

struct Topo {
  int n = 0, ne = 0, nf = 0;
  std::vector<int> ea, eb;                                  // edge ends
  std::unordered_map<long long, int> E;                     // (min, max) → edge
  std::vector<std::vector<int>> eFaces, vE, fEdges;
  long long key(int a, int b) const { return a < b ? (long long)a * n + b : (long long)b * n + a; }
  int edge(int a, int b) const { auto it = E.find(key(a, b)); return it == E.end() ? -1 : it->second; }
  int other(int e, int v) const { return ea[size_t(e)] == v ? eb[size_t(e)] : ea[size_t(e)]; }
};
std::shared_ptr<const Topo> topoOf(const PolyMesh& m);     // (cached per mesh)

struct Loop { std::vector<int> edges, verts; bool closed = false; };
int loopNext(const Topo& T, int e, int v);
Loop edgeLoop(const Topo& T, int e0);
std::vector<int> edgePath(const Topo& T, const std::vector<Vec3>& W, int from, int to);   // (empty: none) — `to` first
struct Chain { std::vector<int> verts; bool closed = false; };
std::vector<Chain> edgeChains(const Topo& T, const std::vector<int>& sel);                 // sel in selection order

// the new curves' group: the selected group (not a service one), else "<mesh> paths" (made once, kept on the mesh)
QString edgesToCurves(Scene& sc, Selection& sel, int meshId, const std::vector<int>& edges);
// a file's curves (polylines) as this program's curves: each fitted with anchors (sharp corners kept), in a new group
std::vector<int> polylinesToCurves(Scene& sc, const std::vector<std::vector<Vec3>>& lines, const QString& groupName);
struct M2C { int maxC = 8; double angle = 30; int count = 6; QString root = "top"; };
QString meshToCurves(Scene& sc, Selection& sel, int meshId, const M2C& opt);

}  // namespace hm::meshc
