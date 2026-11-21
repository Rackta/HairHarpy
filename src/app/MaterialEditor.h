// The material builder's window (the Paint room): the graph of nodes — the room's maps, constants, arithmetic, the light, the view —
// joined into the Output; what it makes is shown on the model (Display → Material).
#pragma once
#include <QString>
#include <QWidget>
#include <functional>

namespace hm {

class PaintSession;

void openMaterialEditor(QWidget* parent, PaintSession* s, std::function<void()> changed, std::function<void()> showOnModel);
void closeMaterialEditor();
bool materialEditorShot(const QString& png);     // (checks)

}  // namespace hm
