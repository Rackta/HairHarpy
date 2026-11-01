// The Topology room's core (no UI), ported from the HTML version: a mesh's polygon topology (ttopo — its edges, each
// corner's edge, vertex → edges / polygons), the selection's walks (edge loop, polygon loop, flood inside a frame,
// shortest path), the edit format's helpers (a subset of the polygons, a polygroup dropped, the packed 'pmesh' / 'hmesh'
// sources), the mesh's pieces and which are copies of which (topologically: pieceIso — the walk round the half-edges,
// the affine map between them), the snap mask's run-length code, the mirror pairs, and the decimation of a heavy model
// (quadric-error edge collapses per polygroup, the seams locked, identical pieces decimated once).
#pragma once
#include "scene/Scene.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace hm::topo {

// ---- topology tables (ttopo) ----
struct TTopo {
  int n = 0, nf = 0, ne = 0;
  std::vector<int> fo, fv;              // the polygons (as the mesh's)
  std::vector<int> ce, cf;              // per corner: its edge (to the next corner), its polygon
  std::vector<int> ea, eb, ef0, ef1;    // per edge: its ends (ea < eb), its polygons (ef1 −1: a rim)
  std::vector<int> voff, vE, vfo, vF;   // vertex → edges, vertex → polygons (CSR)
  std::unordered_map<uint64_t, int> H;  // (lo, hi) → edge
  int edgeOf(int a, int b) const;
  int other(int e, int v) const { return ea[size_t(e)] == v ? eb[size_t(e)] : ea[size_t(e)]; }
  int faceSize(int f) const { return fo[size_t(f) + 1] - fo[size_t(f)]; }
};
// cached per mesh (the base it was made from: a new base — new tables)
std::shared_ptr<const TTopo> ttopo(const std::shared_ptr<PolyMesh>& B);
void shareTopo(const std::shared_ptr<PolyMesh>& to, const std::shared_ptr<PolyMesh>& from);   // (only the positions differ)

std::vector<int> edgeLoop(const TTopo& T, int e0);
std::vector<int> faceLoop(const TTopo& T, int f0, int e0);
// polygons reachable from f0 without crossing the blocked ones; nullopt past cap
std::optional<std::vector<int>> flood(const TTopo& T, int f0, const std::set<int>& blocked, int cap);
// the shortest way from element a to b (key 'f' over the polygon centres, 'e' / 'v' over the vertices)
std::optional<std::vector<int>> path(const TTopo& T, const std::vector<Vec3>& P, char key, int a, int b);
std::vector<int> selVerts(const TTopo& T, char key, const std::set<int>& sel);
std::vector<int> groupFaces(const PolyMesh& B, int gi);

// ---- the edit format: the mesh's polygons (PolyMesh); a subset of them (keep[f]) with its vertices compacted ----
PolyMesh subset(const PolyMesh& d, const std::vector<char>& keep, std::vector<int>* vmap = nullptr);
// the UV Sets: d has them whole; polygon f of d (its corners cs, in d) onto the polygon just added to o (its sets, its live
// one, each set's UVs) — o takes the sets' names the first time
bool hasUvSets(const PolyMesh& d);
void addFaceSets(PolyMesh& o, const PolyMesh& d, int f, const std::vector<int>& cs);
void dropGroup(PolyMesh& d, int gi);                  // (its polygons must be gone already: the later groups renumbered)
QByteArray packPMesh(const PolyMesh& d);
struct Dec { std::vector<Vec3> pos; std::vector<uint32_t> idx; std::vector<uint16_t> tg; std::vector<QString> groups; int twins = 0, pieces = 0; };
QByteArray packHMesh(const Dec& d);
PolyMesh decSubset(const Dec& d, int g, const QString& name);   // (g < 0 — all)
PolyMesh decMesh(const Dec& d);                                    // the decimated result as a mesh (triangles), its groups

// ---- pieces and their copies ----
struct Pieces { int np = 0; std::vector<int> pv, pf, poff, plist, voff, vlist, vloc; };
Pieces meshPieces(const PolyMesh& o);
struct IsoClasses { Pieces P; std::vector<int> cls; std::vector<std::array<double, 9>> M; std::vector<std::array<double, 3>> T; std::vector<std::vector<int>> mapF; };   // (piece p = M·(its class's piece) + T)
std::shared_ptr<const IsoClasses> isoClasses(const std::shared_ptr<PolyMesh>& B);
std::vector<int> similarFaces(const std::shared_ptr<PolyMesh>& B, int gi);   // the same place on topologically identical parts
std::vector<int> identicalFaces(const std::shared_ptr<PolyMesh>& B, int gi, int* parts, int* own);
std::vector<int> symmetricFaces(const std::shared_ptr<PolyMesh>& B, int gi, QString* msg);
std::vector<int> groupBorder(const TTopo& T, const PolyMesh& B, int gi);   // edges with the group on one side only

// ---- snap mask, mirror pairs ----
QString smEnc(const std::vector<char>& a);
std::vector<char> smDec(const QString& s, int n);
struct Partners { std::vector<int> part; std::vector<double> cx; double plane = 0; };
std::shared_ptr<const Partners> smPartners(const std::shared_ptr<PolyMesh>& B);   // each polygon's mirror across X = 0
std::vector<int> symPairs(const PolyMesh& B, const std::vector<Vec3>& base);      // each vertex's mirror (−1 none)

// ---- decimation ----
struct QemResult { std::vector<float> pos; std::vector<uint32_t> idx; std::vector<int> src; };
QemResult qemSimplify(const std::vector<float>& pos, const std::vector<uint32_t>& idx, int target, double aggr, const std::vector<uint8_t>* lock);
Dec decimateByGroups(const PolyMesh& o, double ratio, const std::function<void(double)>& progress);

int triCount(const PolyMesh& m);

// ---- a bounding volume hierarchy over triangles (the snap's nearest point, the brush's circle, the rays of what the
// camera sees) ----
struct Bvh {
  struct Node { Box3 b; int left = -1, right = -1, first = 0, count = 0; };
  std::vector<Node> nodes; std::vector<int> order; std::vector<uint32_t> idx; std::vector<Vec3> P;
  void build(const std::vector<Vec3>& pos, const std::vector<uint32_t>& tris);
  // the nearest point of the triangles ok(t) lets through (null — all) → its distance in *d
  std::optional<Vec3> closest(const Vec3& q, const std::function<bool(int)>& ok, double* d = nullptr, int* tri = nullptr) const;   // (tri: the triangle it lies on)
  std::optional<std::pair<double, int>> ray(const Vec3& o, const Vec3& dir, double tmax) const;   // first hit: t, triangle
  void sphere(const Vec3& c, double r, const std::function<void(int)>& fn) const;                 // the triangles touching it (their box)
  int triCount() const { return int(idx.size() / 3); }
};

// ---- the polygroups' hierarchy (m.gTree — only how the list shows them): a node is a polygroup (g) or a folder (f) ----
struct TgNode { qint64 id = 0; int g = -1; QString f; bool isFolder = false, open = true; std::vector<TgNode> kids; };
qint64 tgNid();
std::vector<TgNode> tgTree(const QJsonValue& raw, int G, const QJsonValue& gOrder);   // cleaned: gone groups' kids stay in their place
QJsonArray tgJson(const std::vector<TgNode>& t);
std::vector<int> tgUnder(const TgNode& n);
TgNode* tgFind(std::vector<TgNode>& list, qint64 id, std::vector<TgNode>** arr = nullptr);
void tgWalk(const std::vector<TgNode>& list, const std::function<void(const TgNode&, int depth)>& fn, int depth = 0);
// the item's own lists (hidGroups, gOrder, gTree) follow the polygroups renumbered (map(old) → new, −1 gone)
void groupsFollow(QJsonObject& raw, const std::function<int(int)>& map);
// the item's selection sets (raw "selSets", by element number) carried from mesh A onto B — its new geometry: a polygon,
// an edge, a vertex still where it was (its corners in the same places) keeps its place in the set; a set left empty
// dropped; one made for other geometry already left as it is
QJsonArray selSetsFollow(const QJsonArray& sets, const std::shared_ptr<PolyMesh>& A, const std::shared_ptr<PolyMesh>& B, bool dropEmpty = true);

}  // namespace hm::topo
