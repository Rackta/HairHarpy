// ◍ Shell modifiers' panels: the tool's (the tabs Density / Parent influence / Line / NURBS disc, what each adds and its
// brush) and the Settings of a Density, a Parent influence, an Even loops (older) and a guide (line or disc).
#pragma once
#include <QWidget>

class QVBoxLayout;

namespace hm {

class Viewport;
class Scene;

QWidget* buildShellPanel(Viewport* vp, Scene* sc);
void shellModProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id);   // tip / pinf / even / guide

}  // namespace hm
