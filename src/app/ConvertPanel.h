// The "Convert to geometry" window's content: the selected cocoons (as their tree — a click selects only it), Loop step
// (double-click — type it) with Set reference, Deviation, Auto weld points (Off / On, the distance: double-click — type,
// auto), Weld locks (Off / On) with Check connections, and Convert. The loop settings go to the selected cocoons' lock
// groups (none — the project's); the weld settings are the project's (undoable).
#pragma once
#include <QWidget>
#include <vector>

class QLabel;
class QPushButton;
class QScrollArea;
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
  QWidget* footer() const { return footer_; }        // (the Convert button's row — the dock keeps it in sight)
private:
  void rebuildList(const std::vector<int>& ks, const std::vector<int>& bs);
  void syncRest();
  Viewport* vp_;
  Scene* sc_;
  QWidget* list_;
  QScrollArea* listScroll_ = nullptr;
  QWidget* footer_ = nullptr;
  QVBoxLayout* listLay_;
  QLabel *stepVal_, *devVal_, *weldDist_;
  QSlider* dev_;
  QPushButton *weldOff_, *weldOn_, *lockOff_, *lockOn_, *check_, *refBtn_, *pickBtn_ = nullptr;
  QWidget* weldDistRow_;
  bool syncing_ = false;
  QString listSig_;   // (what the list shows now: made again only when it changes — not on every frame of an extrude)
};

}  // namespace hm
