// ⭳ Export ▾ and 🔗 Bridge ▾ (the HTML version's expBar / brgBar): panels under their buttons. Export — the converted
// meshes (selected + in selected groups, or all) as OBJ / glTF / PLY / STL with a scale. Bridge — Blender (its add-on) or
// Maya (its script) listening on a local port (they build the meshes themselves: the same names updated in place), or a
// folder another program watches (the file written there). ⇪ Send — shown once a bridge is set up (Ctrl+Shift+E).
#pragma once
#include <QString>
#include <functional>

class QPushButton;
class QWidget;

namespace hm {

class Viewport;
class Scene;

void openExportPanel(QPushButton* anchor, Viewport* vp, Scene* sc);   // (the export window, in the middle of the screen)
void exportWindowShot(Viewport* vp, Scene* sc, const QString& png);       // (the test harness)
void openBridgePanel(QPushButton* anchor, Viewport* vp, Scene* sc, const std::function<void()>& changed);
void bridgeSend(QWidget* parent, Viewport* vp, Scene* sc, const std::function<void()>& openSetup);
bool bridgeReady();
QString sendLabel();                                          // ⇪ Blender / ⇪ Maya / ⇪ Folder
QString sendTip();

}  // namespace hm
