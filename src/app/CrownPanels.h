// ◠ The Crown tool's window (Create, new crowns symmetric) and a crown's settings (rings, whorl, select its parts,
// + Crown curve, symmetry) — the HTML version's crown panel / crownProps.
#pragma once
#include <QWidget>

class QVBoxLayout;

namespace hm {

class Viewport;
class Scene;

QWidget* buildCrownPanel(Viewport* vp, Scene* sc);
void crownProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id);

}  // namespace hm
