// 🎥 Camera ▾ (the HTML version's camMenu): the active view — reset, perspective / orthographic, ⧉ copy / ⎘ paste the
// camera's position (HMC-CAM text), the views (click — use, drag onto a split window), the split screen (on, layouts 2 ·
// 2+1 · 1+2 · 4, the layout in miniature: a view dropped on a window shows it there, windows dragged onto each other swap,
// a click — the active one). Tab: split on + the menu at the pointer; Tab twice — the active window alone.
// Also the view overlay over the viewport: each window's label (its view, the isolation) and the view axes in the corner
// (X / Y / Z as the camera sees them; a click — the view from that axis).
#pragma once
#include <QPoint>
#include <QString>

class QWidget;

namespace hm {

class Viewport;

void openCameraMenu(QWidget* parent, Viewport* vp, const QPoint& global);
void closeCameraMenu();
bool cameraMenuOpen(); bool cameraMenuShot(const QString& png);
QString camPosText(const Viewport* vp);
bool applyCamPos(Viewport* vp, const QString& text);

// the labels and the view axes over the viewport (top-level windows following its container)
class ViewOverlay;
ViewOverlay* makeViewOverlay(QWidget* container, Viewport* vp);

}  // namespace hm
