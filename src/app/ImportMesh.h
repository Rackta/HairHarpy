// Importing models (the HTML version's "+" in the outliner — importHead / importMesh): ☺ Head (the reference model the
// hair is drawn on: an imported mesh marked as the head, the camera framed on it, an empty scene's loop step from its
// height) or ◆ Mesh → Imported (an extra mesh; "Polygroups as separate meshes" — every OBJ group / glTF mesh / FBX node a
// mesh of its own, in a group named after the file). The texture images and an OBJ's .mtl picked with the model become
// its look (kept in the project). Files: OBJ / GLB / GLTF / FBX.
#pragma once
#include "core/Math.h"
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>
#include <set>
#include <vector>

class QWidget;

namespace hm {

class Viewport;
class Scene;

// content of another scale coming into a scene that has something in it (20.11): over 4 times bigger or smaller than what
// the scene has — asked whether to fit it, with the fitted scale offered and editable (nullopt — kept as it is)
std::optional<double> askFitScale(QWidget* parent, const QString& what, double times, double optimal);
std::optional<double> offerFitScale(Viewport* vp, const Box3& sceneBox, const Box3& content, const QString& what, std::optional<double> optimal = std::nullopt);
Box3 sceneContentBox(const Scene& sc, const std::set<int>& skip = {});   // (what is shown: meshes, curves' anchors)
double boxBig(const Box3& b);
void scaleMeshes(Scene& s, const std::vector<int>& ids, double k, const Vec3& from, const Vec3& to);   // (through each one's Transform: p → (p − from)·k + to)
extern std::function<std::optional<double>(double)> fitScaleHook;   // (checks: the answer instead of the window — given the offered scale)
extern QString fitScaleAsked;                                       // (checks: what the last question was)

// the import menu under the "+" button (Head / Mesh → Imported / Polygroups as separate meshes)
void importMenu(QWidget* anchor, Viewport* vp, Scene* sc);
// the model in `files` (with its images and .mtl) imported: as the head or into Imported
void importFiles(const QStringList& files, bool asHead, Viewport* vp, Scene* sc);
// the Topology room's import (＋): as the head (joins it when there is one), the file's polygroups (as polygroups of one
// mesh or as separate meshes), separate parts split into polygroups; a model over a million triangles — decimated
// (per polygroup, the seams kept, identical parts the same way) or as is
// (and what is taken from the file: its UVs, textures, creases, hard edges — when the file has none, worked out of its
// normals —, its curves — made this program's curves)
struct TopoImport { bool head = false, groups = true, objects = false, parts = false,
                    uvs = true, textures = true, creases = true, hard = true, curves = false, polypaint = true;
                    bool quiet = false; };   // (quiet — files dropped on the window: no questions — a heavy model as it is, a file of curves into the Hair room)
// files dropped on the window: each model imported with what it has (its polygroups, UVs, textures, creases, hard edges,
// polypaint, curves) into the room shown — no window, no question
void importAuto(const QStringList& files, Viewport* vp, Scene* sc);
void topoImportMenu(QWidget* anchor, Viewport* vp, Scene* sc);
void topoImportFiles(const QStringList& files, const TopoImport& o, Viewport* vp, Scene* sc);
// a mesh another program sent (ZBrush: ZBrushLink.h) into the open scene: a mesh `from` already sent under this name has its
// geometry replaced in place (its modifiers, look, place in the outliner kept), else a new one in Imported; it is selected
// → the message ('' — not read)
QString receiveMesh(const QString& file, const QString& name, const QString& from, Viewport* vp, Scene* sc);
// the hair room's import with the import window's choices (head / polygroups as separate meshes / what is taken)
void importFilesWith(const QStringList& files, const TopoImport& o, Viewport* vp, Scene* sc);

}  // namespace hm
