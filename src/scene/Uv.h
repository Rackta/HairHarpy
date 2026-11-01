// The Topology room's UV (no UI): a mesh's UV as its corners carry them (PolyMesh::cornerUV) — the UV points (the corners
// at one vertex with the same UV: they move together), the shells (the polygons joined where their UV goes on unbroken),
// the seams (PolyMesh::seams: where Unfold cuts), Unfold (least squares conformal maps of each shell cut along its seams, then SLIM — UvSolve.h;
// turned to its smallest box, sized to its 3D area), Pack (the shells' boxes into the 0…1 tile at one scale), the turns
// and mirrors, the polygons turned over in UV, and Preserve UVs (moved vertices take the UV of where they now lie on the
// surface as it was).
#pragma once
#include "scene/Scene.h"
#include "scene/Topo.h"
#include <QString>
#include <array>
#include <functional>
#include <optional>
#include <set>
#include <vector>

namespace hm::uv {

using UV = std::array<float, 2>;
bool has(const PolyMesh& m);                                // a UV per corner
void ensure(PolyMesh& m);                                   // (none yet: every corner at 0, 0)
int cornerAt(const topo::TTopo& T, int f, int v);           // the corner of polygon f at vertex v (−1)

// the UV points: per corner its point; per point its corners (CSR) and vertex
struct Points { std::vector<int> of, off, list, vert; int count() const { return int(vert.size()); } };
Points points(const PolyMesh& m, const topo::TTopo& T);
// a mesh edge's UV unbroken across it (both its ends one point on each side); a rim — false
bool continuous(const PolyMesh& m, const topo::TTopo& T, const Points& P, int e);
// the shells: per polygon its shell; joined across unbroken edges (no UV: across every edge — the mesh's parts)
struct Shells { std::vector<int> of; int count = 0; };
Shells shells(const PolyMesh& m, const topo::TTopo& T, const Points& P);
std::vector<int> borderEdges(const PolyMesh& m, const topo::TTopo& T, const Points& P);   // inner edges where the UV breaks

// seams: the mesh's (vertex pairs) as edge flags, and back
std::vector<char> seamFlags(const PolyMesh& m, const topo::TTopo& T);
void setSeams(PolyMesh& m, const topo::TTopo& T, const std::vector<char>& flags);
std::vector<int> seamsByAngle(const PolyMesh& m, const topo::TTopo& T, double degrees);   // inner edges sharper than that
std::vector<int> autoSeams(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces);   // a closed shell's seams: its handles opened, a path between its far ends — a disk

// Unfold: the shells (cut along the seams) holding any of `faces` (empty — all); each must have a border (a closed one is
// left and counted). keepPlace: each goes where its old UV was, at the density of the rest; else all are packed after
// pins: mesh edges (vertex pairs, smaller first) whose UV points stay where they are — a shell holding two or more such
// points is laid round them (at the density they give, not turned, not moved, not packed with a turn)
// sym: the mesh's symmetry across X = 0 — a shell lying across the plane (its mirror is itself, the seams symmetric) is
// unfolded symmetric: its middle line straight up, its sides mirror images; pairs: a shell whose mirror is another shell
// — either of them asked, both done: the one on +X unfolded, the other its mirror image across U = axis
using Pins = std::set<std::pair<int, int>>;
struct SymUnfold { std::vector<int> mc; bool pairs = false; double axis = 0.5; };   // (mc: mirrorCorners)
struct UnfoldResult { int done = 0, closed = 0, holes = 0, pinned = 0, symmetric = 0, mirrored = 0, asym = 0, featured = 0; std::vector<int> closedFaces, asymFaces; };   // (asym: across the plane, but its seams not symmetric — unfolded as it is)
struct PackOpts;
// Face features (UvFace.cpp — lips, eyelids): two chains of mesh vertices between the same two corners, a — the upper,
// b — the lower; their points face each other in UV: open — each pair on one vertical (U the same), the gap kept;
// closed — each pair one point (the hole zipped: lids and lips laid as if shut). straight: the corners level (V the same)
struct Feature { std::vector<int> a, b; bool closed = false, straight = true; };
// the holes of the shells (cut along the seams) holding any of `faces` (empty — all): each inner border split at its two
// points farthest apart (the corners); the chain higher in 3D the upper
std::vector<Feature> detectFeatures(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces);
// from edges: one closed loop (the upper chain and the lower between the same corners), split at its corners
std::optional<Feature> featureFromEdges(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& edges);
std::optional<Feature> mirrorFeature(const Feature& f, const std::vector<int>& vpair);   // (by topo::symPairs)
// the ties a shell's local vertices get from the features (vloc: mesh vertex → local, −1 none / more than one):
// (lower, upper, both u and v) pairs and the corner pairs to level
struct FeatureTies { std::vector<std::array<int, 3>> pairs; std::vector<std::array<int, 2>> level; std::array<int, 2> frame{-1, -1}; };
FeatureTies featureTies(const std::vector<Feature>& fs, const std::vector<int>& vloc, const std::vector<Vec3>& pos, const std::vector<char>* inner = nullptr);   // (inner: the locals on a hole's rim — a feature only there: the shell whose outline it is left alone)
// the local vertices on the shell's holes' rims (every border but the longest)
std::vector<char> innerRims(int n, const std::vector<std::array<int, 3>>& tris, const std::vector<Vec3>& X);
UnfoldResult unfold(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, bool keepPlace, bool optimize = false, const Pins* pins = nullptr, const SymUnfold* sym = nullptr, const PackOpts* po = nullptr, const std::vector<Feature>* feats = nullptr);   // (po: the texture — its proportion, the packing after)   // (optimize: then SLIM — the stretch evened out, no triangle turned over; off — LSCM alone, quick: the preview)
// the symmetry: each corner's mirror corner — the corner at the vertex's mirror (topo::symPairs) in the mirrored polygon
// (a polygon lying across the plane: in itself); −1 none
std::vector<int> mirrorCorners(const PolyMesh& m, const topo::TTopo& T);
std::vector<int> mirrorEdges(const PolyMesh& m, const topo::TTopo& T);   // each edge's mirror edge (−1)
// Symmetrize: the UV of the corners `from` put on their mirror corners, mirrored across U = axis — in a shell lying across
// the plane across its own middle (where its pairs meet now); corners on the plane put on that middle → the corners set
int symmetrize(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& mc, const std::vector<int>& from, double axis, int* skipped = nullptr);   // (skipped: the shells whose mirror isn't one shell cut the same way — left)
// Optimize (RizomUV's): the UV as it lies relaxed towards each polygon's own shape — no seams cut, nothing moved away or
// packed; pinned points held; the selection's shells (none — all); how many done
int optimize(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const Pins* pins = nullptr, int iters = 60, const PackOpts* po = nullptr, const std::vector<Feature>* feats = nullptr);   // (po: the texture — worked on texel-square in it)
// Pack by shape (UvPack.cpp): the shells holding any of `faces` (empty — all) into the 0…1 tile at one scale, as their
// outlines lie (a raster of the texture, not their boxes): the biggest first, each where it goes lowest, then leftmost,
// a quarter turn tried (rotate). The texture: w × h pixels — the UV kept texel-square in it; pad: the gap between the
// shells in its pixels (half of it off the tile's edges). mc (the mirror corners — Symmetry): a shell across the plane
// centred on U = axis, upright, never turned; a pair's +X shell on its side of the axis, the other its mirror image; the
// rest wherever they fit
// a shell's own settings (per polygon — all of a shell's alike): locked — never moved (Pack: in the way); noTurn — Pack
// doesn't turn it; scale — its size against the rest (texel density × scale); group — Pack lays a group's shells together
struct ShellAttr { bool lock = false, noTurn = false; double scale = 1; int group = 0; };
struct PackOpts { int w = 2048, h = 2048; double pad = 8; bool rotate = true; const std::vector<int>* mc = nullptr; double axis = 0.5; const std::vector<ShellAttr>* attr = nullptr; bool stacks = true; };   // (stacks: alike shells one on another packed as one)
struct PackResult { double fill = 0, scale = 0, axis = 0.5; int shells = 0, centred = 0, offAxis = 0, pairs = 0, split = 0, free = 0, locked = 0, stacked = 0; bool ok = false; };   // (offAxis: across the plane, but no room on the axis — upright elsewhere; split: a pair too wide for its side — both anywhere)
PackResult packShapes(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const PackOpts& o);
// ---- lining up (UvAlign.cpp; po: the texture — worked on texel-square in it) ----
// Grid: the quads of the shells holding any of `faces` laid as a grid — across each quad strip level (V the same), along
// it upright (U the same), as LSCM then SLIM find the lengths; each where it lay, its size kept → the shells done
int rectify(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const PackOpts* po = nullptr);
// Straighten: a chain of edges — its points on the line from its first end to its last, spaced as the 3D lengths → points
int straightenChain(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& edges, const PackOpts* po = nullptr);
// Orient: the shell of the edge turned so the edge lies level or upright (the nearer) → the angle turned (degrees)
double orientByEdge(PolyMesh& m, const topo::TTopo& T, int edge, const PackOpts* po = nullptr);
// Upright: the shells holding any of `faces` turned so the 3D up runs up their V → the shells turned
int orientUpright(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const PackOpts* po = nullptr);
// Stack: the shells holding any of `faces` laid over the shell of `refFace` (scaled to its size, turned / mirrored to fit
// best) → the shells stacked; Unstack: shells lying over each other set apart along U, the padding between → moved
int stackOnto(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, int refFace, const PackOpts* po = nullptr);
int unstack(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, double gap);
// Snap together: along the edges (a seam between two shells) the smaller shell turned and moved onto the bigger, then
// sewn there (the seam off those edges) → the edges sewn
int snapTogether(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& edges, const PackOpts* po = nullptr);

// ---- checks (UvCheck.cpp): what is wrong with the UV of `faces` (one tile — a UV Mat's) for its texture ----
// flipped — turned over; overlap — lying over another polygon (two alike shells laid one on another — a stack — not);
// outside — past the 0…1 tile; padding — nearer another shell than the padding (pixels), or nearer the tile's edge than
// half of it; density — a shell's texel density off the wanted one (none: the shells' median) by more than tdTol;
// asym — a mirror pair's shells not mirror images of each other (mc given)
struct CheckOpts { int w = 2048, h = 2048; double pad = 8, td = 0, tdTol = 0.1; const std::vector<int>* mc = nullptr; };
struct CheckReport { std::vector<int> flipped, overlap, outside, padding, density, asym; int stacks = 0; double tdRef = 0; };
CheckReport check(const PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, const CheckOpts& o);
// each polygon's angle distortion: its triangles' worst ratio of the stretch's two directions (1 — angles kept)
std::vector<float> angleStretch(const PolyMesh& m, const topo::TTopo& T, double asp = 1);

// Pack (boxes — the old way, kept for comparison): the shells holding any of `faces` (empty — all) into 0…1 at one scale, each box turned to lie flat (rotate);
// margin / padding in parts of the tile → the share of the tile they fill
double pack(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, double padding, bool rotate);
// Straighten (hair strands): each shell holding any of `faces` (empty — all) unbent along its length — its middle line (from
// one far end to the other, through the UV) loses `amount` of its bends (1 — straight), every point keeping its place
// across and along it, so the width and the shape stay; turned upright (along V) when `upright`, in its old middle →
// the shells done
int straighten(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces, double amount, bool upright);
// Rectangle (hair strands): each shell holding any of `faces` (empty — all) laid out as a strip — along its middle line
// (V) and across it, the width made one all along (U); a fork's arm sticks out → the shells done
int rectangle(PolyMesh& m, const topo::TTopo& T, const std::vector<int>& faces);

// the UV points of these corners and those welded to them (a corner moved — its point moves)
std::vector<int> pointCorners(const Points& P, const std::vector<int>& corners);
std::vector<int> faceCorners(const topo::TTopo& T, const std::vector<int>& faces);
void transform(PolyMesh& m, const std::vector<int>& corners, const std::function<UV(const UV&)>& fn);
std::array<double, 4> box(const PolyMesh& m, const std::vector<int>& corners);   // u0, v0, u1, v1
// the polygons turned over (their UV wound against the most of their shell's area)
std::vector<int> flipped(const PolyMesh& m, const topo::TTopo& T, const Shells& S);
double faceArea(const PolyMesh& m, int f);                   // signed, in UV
double faceArea3(const PolyMesh& m, int f);                  // in 3D
// how much of the 0…1 tile the UV covers (the polygons' areas, overlaps counted twice)
double coverage(const PolyMesh& m);

// Preserve UVs: the vertices moved (orig — before, with its UV; P — now): each UV point at them takes the UV of the place
// on the surface as it was nearest to where the vertex is now (its polygons round it and their neighbours, in its shell)
std::vector<UV> preserve(const PolyMesh& orig, const topo::TTopo& T, const std::vector<Vec3>& P, const std::vector<int>& moved);

}  // namespace hm::uv
