// Importing models (the HTML version's "+" in the outliner — importHead / importMesh): ☺ Head (the reference model the
// hair is drawn on: an imported mesh marked as the head, the camera framed on it, an empty scene's loop step from its
// height) or ◆ Mesh → Imported (an extra mesh; "Polygroups as separate meshes" — every OBJ group / glTF mesh / FBX node a
// mesh of its own, in a group named after the file). The texture images and an OBJ's .mtl picked with the model become
// its look (kept in the project). Files: OBJ / GLB / GLTF / FBX.
#pragma once
#include <QString>
#include <QStringList>

class QWidget;

namespace hm {

class Viewport;
class Scene;

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
                    uvs = true, textures = true, creases = true, hard = true, curves = false, polypaint = true; };
void topoImportMenu(QWidget* anchor, Viewport* vp, Scene* sc);
void topoImportFiles(const QStringList& files, const TopoImport& o, Viewport* vp, Scene* sc);
// the hair room's import with the import window's choices (head / polygroups as separate meshes / what is taken)
void importFilesWith(const QStringList& files, const TopoImport& o, Viewport* vp, Scene* sc);

}  // namespace hm
