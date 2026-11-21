// The Paint room's bars: the top one (Symmetry, the display mode, the map painted) and the second one (the brush's quick
// settings, the map's own).
#pragma once
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

namespace hm {

class PaintSession;
class Viewport;

QStringList paintDisplayModes();                 // std flat normal (the keys; the hotkeys 1 2 3 in this order)
QString paintDisplayName(const QString& key);
QWidget* makePaintTopBar(PaintSession* s, Viewport* vp, std::function<void()> openMaterial);
QWidget* makePaintModeBar(PaintSession* s, Viewport* vp, std::function<void(bool)> setProjection);

}  // namespace hm
