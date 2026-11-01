// The "Convert to geometry" window's content: the selected cocoons (as their tree — a click selects only it), Loop step
// (double-click — type it) with Set reference, Deviation, Auto weld points (Off / On, the distance: double-click — type,
// auto), Weld locks (Off / On) with Check connections, and Convert. The loop settings go to the selected cocoons' lock
// groups (none — the project's); the weld settings are the project's (undoable).
#pragma once
#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QVBoxLayout;

namespace hm {

class Viewport;
class Scene;

class ConvertPanel : public QWidget {
  Q_OBJECT
public:
  ConvertPanel(Viewport* vp, Scene* sc, QWidget* parent = nullptr);
  void sync();                                       // (selection / project changed)
private:
  Viewport* vp_;
  Scene* sc_;
  QWidget* list_;
  QVBoxLayout* listLay_;
  QLabel *stepVal_, *devVal_, *weldDist_;
  QSlider* dev_;
  QPushButton *weldOff_, *weldOn_, *lockOff_, *lockOn_, *check_, *refBtn_, *pickBtn_ = nullptr;
  QWidget* weldDistRow_;
  bool syncing_ = false;
};

}  // namespace hm
