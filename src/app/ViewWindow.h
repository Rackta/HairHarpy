// The view window: one or two views of the scene in a window of their own — moved to another screen, sized freely. They are
// windows of the viewport like the split screen's (their own cameras; in the Paint room — their own visibility sets, the camera
// following the main view's), drawn by the viewport's renderer into a picture this window shows; the mouse over it works as over
// the viewport. Tab over it — its own menu: how many views and how they lie, each view's projection, visibility set and camera
// sync, "Move into the main viewport" (the views become the split screen's windows, this window closes).
#pragma once
#include <QPoint>
#include <QPointF>
#include <QString>

class QWidget;

namespace hm {

class Viewport;

void openViewWindow(QWidget* mainWindow, Viewport* vp);   // opens it with one view; open already — a second view added
void closeViewWindow();
bool viewWindowOpen();
bool viewWindowUnderPointer();
void toggleViewWindowMenu(const QPoint& global);          // (Tab over it)
bool viewWindowShot(const QString& png, bool menu);       // (checks)
bool viewWindowMouse(int type, const QPointF& at, int button, int held, int mods);   // (checks: a mouse event given to its picture)

}  // namespace hm
