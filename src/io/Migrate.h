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
MigrateResult migrateData(Scene& sc, const QJsonObject& P, const QString& name, bool wrap, const std::optional<HeadFit>& fit);

}  // namespace hm
