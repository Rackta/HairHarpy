// Imported meshes' modifiers (item 'xmod' with `mesh`), ported from the HTML version (xformMatrix, meshMatrix,
// newMeshExtra, applyMeshMatrix, meshThick, rebuildImesh, meshGroupVis): the file's geometry stays as it was (Item::base);
// what is shown, picked and snapped (Item::poly) is made from it — the Transform modifier (position, rotation °, scale
// about the mesh's centre), the Thickness modifier (painted weights −1…1 along the normals, held back by its mask `hold`;
// an older project's Mask modifiers too) and the polygroups switched off (raw hidGroups).
#pragma once
#include "scene/Scene.h"
#include <QByteArray>
#include <QString>
#include <functional>
#include <map>
#include <vector>

namespace hm::meshmods {

std::vector<int> modsOf(const Scene& sc, int mesh);                 // its modifiers (file order)
Vec3 baseCenter(const Item& m);                                      // the file geometry's box centre (B.center)
double baseSize(const Item& m);                                      // its box diagonal (B.size)
Affine xformMatrix(const QJsonObject& p, const Vec3& center);        // T(c + t) · R · S · T(−c)
Affine meshMatrix(const Scene& sc, const Item& m);                   // its (switched on) Transform's, else identity
Affine vsymFrame(const Scene& sc, const Item& group);                // a symmetry group's frame: its object's gizmo (under one), else its own point / axes
std::vector<Affine> vsymWorld(const Scene& sc, const Item& group);   // its copies' transforms, world (about that frame)
// Mirror by sides ("axes": "x+", "y-"… — the kept side of each; "weld", "weldDist"): the axes with their sides; a point
// (in the group's frame) on a kept side; one at the seam (within the distance) put onto it
std::vector<std::pair<int, int>> vsymSides(const QJsonObject& vs);
bool vsymKeep(const QJsonObject& vs, const Vec3& local);
Vec3 vsymWeld(const QJsonObject& vs, Vec3 local);
// the shown geometry made again where its modifiers changed (sig: per mesh, what it was made from); weightsShown(mesh) —
// the weights coloured (the modifier selected / its brush on) → the meshes remade
std::vector<int> refresh(Scene& sc, std::map<int, QByteArray>& sig, const std::function<bool(int)>& weightsShown);
int newMeshExtra(Scene& sc, int mesh, const QString& kind, QString* msg);   // xform / thick / mask (−1: none — a heavy mesh)
int meshThick(Scene& sc, int mesh);                                  // its Thickness modifier, made when missing
// the gizmo's change M (world) on top of the Transform it had (X0) → the Transform modifier's settings
void setMeshMatrix(Scene& sc, int mesh, const Affine& W);
// the polygons not shown: their polygroups hidden (hidGroups) or themselves (hidF — H on the selection); empty — none
std::vector<char> hiddenFaces(const Item& m, const PolyMesh& B);
QByteArray hiddenSig(const Item& m);                                 // (what they depend on — for caches)
bool heavy(const Item& m);                                           // over heavyTris() triangles: no wireframe, nothing painted
bool heavy(const PolyMesh& m);
long long heavyTris();                                               // (none now; HM_HEAVY_TRIS sets one)
bool wireless(const Item& m);                                        // over wireTris(): no wireframe, no overlay of every edge / vertex
long long wireTris();
// the weight brush on a mesh: vertices facing the camera inside the brush; W — the weights painted (mask or hold)
using ScreenFn = std::function<bool(const Vec3& p, double& d)>;      // → in front of the camera, d — px from the brush centre
bool paintWeights(Scene& sc, int xmod, bool hold, const QString& op, double target, double lo, double amt, bool absMode,
                  double brushR, const std::function<double(double)>& brushWeight, const ScreenFn& screen, const Vec3& eye, bool ortho, const Vec3& camDir);

}  // namespace hm::meshmods
