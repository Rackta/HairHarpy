// Migration (the HTML version's migrateData): another project's hair — curves, NURBS surfaces, modifiers, crowns, their
// groups — into this scene, ids renumbered; the head, guides, meshes, imported meshes and the service groups stay behind
// (and whatever their sources stayed behind with). `wrap` — into a new group named after the file (a preset comes in as
// its own group). Hair made around another head comes in fitted to this scene's head (headFit: one uniform scale and a
// shift taking the source's head box onto this one — the tops and the middles matched).
#pragma once
#include "scene/Scene.h"
#include <QJsonObject>
#include <QString>
#include <optional>
#include <vector>

namespace hm {

struct HeadFit { double k = 1; Vec3 from, to; };
// the source project's head (an older file's head, or its meshes marked ☺) against this scene's; none — nothing to do
std::optional<HeadFit> headFit(const Scene& sc, const QJsonObject& P, const QString& projectPath);

struct MigrateResult { bool ok = false; QString message; std::vector<int> select; };
// everything: the items dragged from another HairHarpy's outliner (P.roots — what was dragged): imported meshes and their modifiers
// come in too; the service groups are left out but what lies in them comes (18.11: two projects open, an item dragged between them)
MigrateResult migrateData(Scene& sc, const QJsonObject& P, const QString& name, bool wrap, const std::optional<HeadFit>& fit, bool everything = false);
// what another program gets when items are dragged out of the outliner: the project's records cut down to them, their contents,
// the modifiers hanging on them and the anchors their curves use; every imported mesh with its geometry inside (no file of its own)
QJsonObject dragSubset(const QJsonObject& P, const std::vector<int>& roots);

}  // namespace hm
