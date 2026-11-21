// Head Collision — the Hair room's group of the shapes the drawn curves lie on and keep off (what the head mark did before):
// a root group (raw collGroup), first in the outliner. Its objects are imported meshes shown plain — no textures, no
// material — each a primitive with its settings (raw "prim", a PolySphere at first) or a copy of a mesh of the scene (raw
// collSrc: the mesh it was copied from). The group's switches: collision off (raw collOff), picking in the viewport locked
// (raw selLock, on by default). Only the Hair room has it: the other rooms are scenes of their own and share Meshes only.
// Item::headTag is kept by Scene::reindex: on for the objects inside the group.
#pragma once
#include "scene/Scene.h"
#include <QJsonObject>
#include <QString>

namespace hm::coll {

bool isGroup(const Item& it);
int group(const Scene& sc);                 // the group (−1: none)
bool active(const Scene& sc);               // the group there and its collision on
bool locked(const Scene& sc);               // its objects not picked in the viewport
bool inGroup(const Scene& sc, int id);      // the group or anything inside it
int count(const Scene& sc);                 // its objects
PolyMesh plain(const PolyMesh& m);          // positions and polygons only (one polygroup)
PolyMesh meshCopy(const Scene& sc, const Item& m);   // the mesh where it is shown (its Transform baked in), plain
int ensureGroup(Scene& sc);                 // made when missing; first of the roots
// a new object in the group (extra: fields for its record); → its id
int addObject(Scene& sc, const QString& name, const PolyMesh& m, const QJsonObject& extra);
int addPrimitive(Scene& sc, const QString& type, const Vec3& centre, double unit);
// the preset shapes besides the primitives (a model kept in the program, :/coll/<name>.fbx): "head". An object of one keeps
// raw "preset" {name, c, u, s}: its box's middle at c, its biggest side u · s (as a primitive's: where it is, as big)
const std::vector<QString>& presets();
QString presetLabel(const QString& name);    // "Head"
PolyMesh presetBuild(const QString& name, const Vec3& centre, double unit, double size);
int addPreset(Scene& sc, const QString& name, const Vec3& centre, double unit);
// as a project opens in the Hair room: the group made; the meshes an older project marked as the head (or with its Collision
// switched on) — copies of them in the group, the marks taken off; their collision capsules gone; an empty scene — a
// PolySphere in it. → what changed: the objects added (+1 when the group itself was made)
int prepare(Scene& sc);

}  // namespace hm::coll
