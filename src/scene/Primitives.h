// Primitives (the "+" menu): a mesh made from a few settings — Cone 3D, Cylinder 3D, PolyCube, PolyCylinder, PolyPlane,
// PolySphere, Ring 3D, Sphere 3D. The item keeps them (raw "prim" {type, p: the settings, c: its middle, u: the size of 1})
// and is made again as they change, till Convert to object makes it a plain polygonal mesh.
#pragma once
#include "scene/Scene.h"
#include <QJsonObject>
#include <QString>
#include <vector>

namespace hm::prim {

struct Param { const char* key; const char* label; double lo, hi, step, def; bool integer; };

const std::vector<QString>& types();                 // in the menu's order
QString label(const QString& type);                  // "PolyCube", "Sphere 3D" …
const std::vector<Param>& params(const QString& type);
QJsonObject defaults(const QString& type);
PolyMesh build(const QString& type, const QJsonObject& p, const Vec3& centre, double unit);

}  // namespace hm::prim
