// ⌇ The Braid tool's window and a braid's settings (see BraidPanels.cpp).
#pragma once
#include <QWidget>

class QVBoxLayout;

namespace hm {

class Viewport;
class Scene;

QWidget* buildBraidPanel(Viewport* vp, Scene* sc);
void braidProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id);

}  // namespace hm
