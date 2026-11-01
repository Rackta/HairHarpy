// Export of the converted meshes (the HTML version's exportText): OBJ and PLY keep the quads, glTF (one .gltf, the buffer
// inside as a data: URI — positions, smooth normals, uint32 triangles; a node per mesh) and STL are triangles. Numbers as
// the HTML version writes them (6 decimals, trailing zeros dropped).
#pragma once
#include "scene/Scene.h"
#include <QByteArray>
#include <QString>
#include <vector>

namespace hm {

struct ExportMesh { QString name; const PolyMesh* mesh = nullptr; };
// fmt: obj / gltf / ply / stl / fbx (fbxGroups — the normals type: on, 3ds Max smoothing groups; off, Maya normals:
// per corner, split at the hard edges — FBX smoothing per edge, OBJ vn)
QByteArray exportText(const QString& fmt, const std::vector<ExportMesh>& list, double scale = 1, bool fbxGroups = false);
// binary FBX 7.4 (FbxExport.cpp): polygons, edges, normals (custom / split at hard edges), smoothing, creases, UVs, materials
QByteArray exportFbx(const std::vector<ExportMesh>& list, double scale, bool smoothGroups);
QString exportExt(const QString& fmt);
QString exportLabel(const QString& fmt);                      // "OBJ — quads"…
// 'sel' — the selected meshes and those in the selected groups; 'all' — every visible mesh (the Topology room: its meshes
// — imported ones too, as shown, in world space)
std::vector<ExportMesh> exportMeshes(const Scene& sc, const std::vector<int>& sel, const QString& what);
// the live bridge's objects: [{ name, pos (flat), faces }]
QByteArray bridgePayload(const std::vector<ExportMesh>& list, double scale, bool replace);

}  // namespace hm
