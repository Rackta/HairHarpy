// The import window (the import button, models dropped on the file window): opens in the middle of the screen. At the top
// the path of what is imported — typed or pasted (several: separated by ;), or picked with Browse… (several files, of
// different formats, at once); every model a card of its own that opens into its choices: polygroups, polygroups as
// separate meshes, as the head, UVs, textures, creases, hard edges (worked out of the normals for a file that has none —
// 3ds Max, Cinema 4D), curves (made this program's curves). The texture images / .mtl / .bin picked with the models are
// listed apart — they go with every model.
#pragma once
#include <QStringList>

namespace hm {

class Viewport;
class Scene;

void openImportWindow(Viewport* vp, Scene* sc, const QStringList& files = {}, bool head = false);
void importWindowShot(Viewport* vp, Scene* sc, const QStringList& files, const QString& png);   // (the test harness)
// the import / export windows' look (shared)
const char* ioWindowStyle();

}  // namespace hm
