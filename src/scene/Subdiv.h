// The Subdivision modifier of a Topology modifier group (raw "subdiv"): Catmull–Clark subdivision of a polygon mesh —
// any polygons (every one becomes quads), open borders smoothed or kept, edge creases (PolyMesh::creases) kept sharp, the
// UVs (cornerUV and the UV Sets) subdivided with the surface — as a surface of their own, the island borders its rim
// (smooth: the rim rounded like the mesh's; keep borders: the rim stays where it is, the inside is smoothed).
//
// Reconstruct: the other way round — a mesh that is a subdivision (of a mesh of triangles and quads, never n-gons) is taken
// back one level down; what the lower mesh's subdivision lacks of it is kept as the detail of level 1 (a Detail: per point
// of that subdivision, an offset in the point's own frame — normal, tangent, bitangent — so it follows the cage when that
// is edited). Done again, the lower detail moves on to level 2, and so on; run() puts the details back.
#pragma once
#include "scene/Scene.h"
#include <QJsonArray>
#include <QJsonValue>
#include <array>
#include <memory>

namespace hm::subdiv {

struct Opts {
  int level = 1;               // times the surface is subdivided (each level — four times the quads)
  bool smoothOpen = true;      // the open borders (edges of one polygon) rounded; off — the border vertices stay where they are
  bool uvSmooth = true;        // the UV rims rounded; off — the rims stay (the UV islands keep their outline)
};
// the shape a level has beyond the plain subdivision of the one below: offsets (along the normal, the tangent, the
// bitangent) at every point of the subdivided surface, in its own order; sig — the topology of the base it belongs to
struct Detail { uint64_t sig = 0; std::vector<std::array<float, 3>> d; };
uint64_t topoSig(const PolyMesh& m);                                   // (what the details belong to: the polygons of the base mesh)
QJsonArray packDetails(const std::vector<Detail>& v);                  // → a mesh's raw "subdDetail" (level 1 first)
std::vector<Detail> unpackDetails(const QJsonValue& v);

Opts optsOf(const QJsonObject& raw);                                   // a group's raw "subdiv" → settings
// the details (level 1 first) are put back as the levels are made — only when they are of this very base (its sig)
std::shared_ptr<PolyMesh> run(const PolyMesh& in, const Opts& o, int* levelDone = nullptr, long long maxFaces = 3000000, const std::vector<Detail>* details = nullptr);
long long facesAfter(const PolyMesh& in, int level);                   // the polygons it would have (every corner of every polygon — one quad per level)

// The Subdivide tool (for good, not a modifier): the polygons picked (`picked`, one flag per polygon; all set — the whole mesh)
// split into quads, `levels` times — simple (the new points on the edges and in the polygons) or smooth (Catmull–Clark's places
// for the points inside the region; its border stays where it is, so the rest of the mesh is not touched). The polygons next to a
// region get the new points of the shared edges (so they are n-gons there; no crack). UVs, polygroups, materials, normals,
// polypaint, seams, creases carried. `made`: the polygons of the result that came from the picked ones
std::shared_ptr<PolyMesh> subdivideRegion(const PolyMesh& in, const std::vector<char>& picked, bool smooth, int levels, std::vector<int>* made = nullptr, int* levelDone = nullptr, long long maxFaces = 3000000);

struct Reconstruction {
  std::shared_ptr<PolyMesh> cage;        // the mesh one level down (null — none: why says it)
  std::vector<Detail> details;           // for it, level 1 first: the new one, then the older ones carried over
  QString why;                           // "empty" | "notquads" | "nonmanifold" | "ngon" | "nolabel" — why it cannot go lower
};
// `old`: the details the mesh has already (of this very mesh — else dropped)
Reconstruction reconstruct(const PolyMesh& high, const std::vector<Detail>& old, const Opts& o);

}  // namespace hm::subdiv
