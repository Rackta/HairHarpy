// 🖼 Reference ▾ (the HTML version's refMenu): a new screen-sized canvas for a view, the project's canvases (✏ Draw, 👁, ✖ on
// each row; click — its settings: Draw / Insert image, opacity, in front / behind, only in its view, the canvas it shows,
// make separate), the anchors (show, ✖ all) and the reference board (show / hide, unlock). The canvas is moved, turned and
// scaled in the canvas mode's own tools, not here.
#pragma once
#include <QPoint>
#include <QString>
#include <functional>

class QWidget;

namespace hm {

class Viewport;
class Scene;
class RefBoard;
struct RefPlane;

void openRefMenu(QWidget* parent, Viewport* vp, Scene* sc, RefBoard* board, const QPoint& global);
// (✏ Draw: the reference canvas opened for a plane — set by the main window)
void setRefCanvasOpener(std::function<void(int, bool)> open, std::function<void(const QString&)> newSheet);   // (open: a plane's canvas, true — with the file window for images)
void closeRefMenu();
bool refMenuOpen();
bool refMenuShot(const QString& png);       // (a check: the open menu to a picture)
// an image file → a data URL (the long side at most 2048 px; PNG when it has see-through parts, else JPEG) and w / h
bool refImageFromFile(const QString& file, QString* dataUrl, int* w, int* h);
// (the project changed by the menu outside the history — the anchors: the title's unsaved mark; set by the main window)
void setRefMenuChanged(std::function<void()> changed);
// a plane switched to another canvas: the plane takes that canvas's proportions (its height kept) — the menu's list and the canvas mode's
void refPlaneSetCanvas(RefPlane& p, int canvas, Scene& sc);

}  // namespace hm
