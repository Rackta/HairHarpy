// 🖼 Reference ▾ (the HTML version's refMenu): the reference board (show / hide, unlock) and the reference planes — ＋ a
// plane for a view, the list (click — work with it: the gizmo moves, turns, scales it; 👁, ✖), the active plane's
// settings: image, ⊕ Centre, ↺ Reset, 90° turns, ⇋ Mirror, ⧉ Copy to another view (the opposite one mirrored), opacity,
// only in its view, 📌 lock to the model (the Orthographic plane) and ↩ Locked view, over the model, the gizmo tools,
// the pivot (unlock to move it onto a spot of the picture, back to the centre).
#pragma once
#include <QPoint>
#include <QString>
#include <functional>

class QWidget;

namespace hm {

class Viewport;
class Scene;
class RefBoard;

void openRefMenu(QWidget* parent, Viewport* vp, Scene* sc, RefBoard* board, const QPoint& global);
void closeRefMenu();
bool refMenuOpen();
// an image file → a data URL (the long side at most 2048 px; PNG when it has see-through parts, else JPEG) and w / h
bool refImageFromFile(const QString& file, QString* dataUrl, int* w, int* h);
// the icons next to the active plane's pivot in the viewport (while a gizmo tool is in use): 🔒 / 🔓 the pivot, ⊕ centre
void attachRefIcons(QWidget* window, QWidget* vpBox, Viewport* vp, Scene* sc, const std::function<void(const QString&)>& pickTool);

}  // namespace hm
