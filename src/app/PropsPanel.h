// The Settings window's content (renderProps): the selected anchors' smoothness, or the settings of the last selected item
// (the last selected curve while curves and walls are selected together; the pinned item while 📌 is on) — group, cocoon,
// curve, wall, meshes; the other kinds come with their tools. Sliders change the scene live and commit on release.
#pragma once
#include <QWidget>

namespace hm {

class Viewport;
class Scene;

// pinned: the item 📌 keeps (-1: the selection)
QWidget* buildPropsPanel(Viewport* vp, Scene* sc, int pinned);

}  // namespace hm
