// Mesh files → PolyMesh, as the HTML version reads them into an imported mesh's base (imeshBaseParsed / imeshBaseFrom):
// positions (Float32) welded, the file's own polygons, polygroups, material slots and a UV per corner. OBJ is parsed here,
// FBX goes through ufbx, glTF / GLB through a small reader of their JSON and buffers; the HTML version's packed meshes
// ('hmesh' — decimated, 'pmesh' — edited in the Topology room) are read too. Coordinates stay as in the file.
#pragma once
#include "scene/Scene.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <memory>

namespace hm {

std::shared_ptr<PolyMesh> parseObj(const QByteArray& text, QString* error = nullptr);
std::shared_ptr<PolyMesh> parseObj(const QByteArray& text, QString* error, bool flat);
std::shared_ptr<PolyMesh> parseFbx(const QByteArray& data, QString* error = nullptr);
std::shared_ptr<PolyMesh> parseGltf(const QByteArray& data, bool glb, QString* error = nullptr);
std::shared_ptr<PolyMesh> parseHMesh(const QByteArray& data, QString* error = nullptr);
std::shared_ptr<PolyMesh> parsePMesh(const QByteArray& data, QString* error = nullptr);
// by extension (obj, fbx, glb, gltf, hmesh, pmesh); flat — the polygroups ignored (one group); null + error otherwise
std::shared_ptr<PolyMesh> importMesh(const QString& ext, const QByteArray& data, QString* error = nullptr);
std::shared_ptr<PolyMesh> importMesh(const QString& ext, const QByteArray& data, QString* error, bool flat);
void uvSetsFromMats(PolyMesh& m);   // materials whose UV overlap in the tile — a UV Set each (a UVW tag per material in Cinema 4D / 3DCoat)
// "Split separate parts into polygroups" (src.split): every connected part a polygroup of its own — named after the group
// it was in ("Head 1", "Head 2"… when a group had several parts), "Part N" in a mesh of one group
void splitPartsIntoGroups(PolyMesh& m);
// the texture sets of a mesh (its base's `sets`): one per material slot its polygons use, in slot order — 'Default' for
// polygons with none (or a file with no materials at all)
struct TexSet { QString name; int mat = -1; QString color; bool alpha = false; };   // color — the file's ('#rrggbb', empty — none); alpha — see-through in the file
std::vector<TexSet> texSets(const PolyMesh& m);
// the import's choices kept in the mesh's source (applied whenever it is read): noUV — the UVs dropped; noCrease — the
// creases; noTex — the textures the file carries; noHard — the hard edges; hardN — the hard edges worked out of the file's normals where it has none (3ds Max,
// Cinema 4D: smoothing groups / split normals instead of edges)
void applySourceOptions(PolyMesh& m, const QJsonObject& src);
void hardFromNormals(PolyMesh& m);                     // (edges where the polygons' corner normals part)
// the curves in a file (OBJ 'l' polylines; FBX line / NURBS curves, in world space) — each a polyline
std::vector<std::vector<Vec3>> importCurves(const QString& ext, const QByteArray& data);

}  // namespace hm
