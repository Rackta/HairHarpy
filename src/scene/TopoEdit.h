// The Topology room's modelling tools' editable mesh (no UI): a mesh as a list of polygons, each with its corners' UVs,
// its polygroup and material — so the tools (PolyPen, Loop, Knife, Weld, Plane Cut, Bevel / Extrude, Inset, Bridge)
// cut edges, split polygons, weld vertices, add and remove polygons and get back a PolyMesh with the UVs lerped where
// edges were cut, the hard edges / creases / seams carried onto the pieces of the edges they were on.
#pragma once
#include "scene/Scene.h"
#include <array>
#include <map>
#include <set>
#include <algorithm>
#include <vector>

namespace hm::tedit {

using EK = std::pair<int, int>;
// a corner's UVs: the one shown ([0], [1]) and each UV Set's own ([2 + 2k], [3 + 2k]) — copied, cut, turned along with it
constexpr int kUvSets = 15;
using UVx = std::array<float, 2 * (kUvSets + 1)>;
inline EK ek(int a, int b) { return {std::min(a, b), std::max(a, b)}; }

struct MeshEd {
  std::vector<Vec3> pos;
  std::vector<std::array<float, 3>> vcol;                // (polypaint, parallel to pos — empty: none; a new vertex −1: from its neighbours when written)
  struct F { std::vector<int> v; std::vector<UVx> uv; std::vector<std::array<float, 3>> n; int g = 0, mat = -1; uint32_t sets = 0; int live = -1; };   // (sets: the UV Sets it is in; live: the one it shows)
  std::vector<F> f;
  bool hasUV = false, hasMat = false, hasN = false;     // (hasN: the file's own normals per corner — carried along; a new corner's (0,0,0): made from the shape)
  int nSets = 0;                                         // (the UV Sets carried: their names in the PolyMesh)
  std::set<EK> hard, seam; std::map<EK, float> crease;

  static MeshEd from(const PolyMesh& m);
  // back to a PolyMesh: src's names, materials, maps; the vertices no polygon uses dropped (vmap: old → new, −1 gone);
  // the hard edges / creases / seams kept where the edge still is
  PolyMesh to(const PolyMesh& src, std::vector<int>* vmap = nullptr) const;

  int addVert(const Vec3& p) { pos.push_back(p); if (!vcol.empty()) vcol.push_back({-1, -1, -1}); return int(pos.size()) - 1; }
  int corner(int fi, int v) const;                       // vertex v's corner in polygon fi (−1)
  std::vector<int> facesOfEdge(int a, int b) const;      // the polygons with a and b side by side
  std::vector<int> facesOfVert(int v) const;
  bool edgeExists(int a, int b) const { return !facesOfEdge(a, b).empty(); }
  bool rimEdge(int a, int b) const { return facesOfEdge(a, b).size() == 1; }
  // the vertices vs (ts: their places from a, ascending) put on the edge a–b in every polygon along it (UVs lerped);
  // its hard / crease / seam marks carried onto the pieces
  void insertOnEdge(int a, int b, const std::vector<int>& vs, const std::vector<double>& ts);
  int splitEdge(int a, int b, double t);                 // → the new vertex
  // polygon fi cut along a new edge between its corners at va and vb → the new polygon's index (fi keeps the va…vb
  // part); −1: they are side by side / not both in it
  int splitFace(int fi, int va, int vb);
  // every vertex v → to[v] (−1 or v itself: stays); a polygon's repeated corners dropped, polygons under three gone
  void weld(const std::vector<int>& to);
  void removeFaces(const std::vector<char>& del);
  // a new polygon: its UVs from the same vertices' corners in the other polygons (else 0, 0); turned to agree with a
  // neighbour across a shared edge → its index
  int addFace(const std::vector<int>& vs, int g = -1, int mat = -1);
  void orient(int fi);                                   // turned round if a neighbour runs the shared edge the same way
  Vec3 normal(int fi) const;                             // (not unit)
  Vec3 center(int fi) const;
  Vec3 vertNormal(int v) const;                          // (unit; its polygons' normals summed)
};

// the boundary of a set of polygons: its edges as (a → b) in the polygons' own direction
std::vector<std::pair<int, int>> regionBorder(const MeshEd& E, const std::set<int>& faces);
// the edges as chains of vertices in order (each chain: its vertices; closed — a loop)
struct Chain { std::vector<int> v; bool closed = false; };
std::vector<Chain> edgeChains(const std::vector<EK>& edges);

// the mesh made manifold (generated geometry — the Hair room's conversion): doubled polygons out; an edge of three
// polygons or more — its polygons paired, each with the one it carries on into across the edge (turned against it,
// the nearest to straight on), each pair its own copy of the edge; a vertex its polygons meet at only by touching —
// a copy for each fan of them. → how many changes
int makeManifold(PolyMesh& m);
// a polygon lying over smaller ones made of its own corners (a quad over the two triangles of its own split — a hidden
// double wall: its sides then each of three polygons) taken out; → how many
int dropCovered(MeshEd& E);
// a polygon's corners as a key that is the same for the same polygon only — started at its smallest corner, run the way
// whose next corner is the smaller (turned either way); not the set of its corners: [a b c d] and [a c b d] are two
template <class T> std::vector<T> cycleKey(const std::vector<T>& v) {
  const size_t n = v.size(); if (n < 2) return v; size_t s = size_t(std::min_element(v.begin(), v.end()) - v.begin());
  const bool fwd = !(v[(s + n - 1) % n] < v[(s + 1) % n]); std::vector<T> o; o.reserve(n);
  for (size_t i = 0; i < n; i++) o.push_back(v[fwd ? (s + i) % n : (s + n - i) % n]); return o;
}
}  // namespace hm::tedit
