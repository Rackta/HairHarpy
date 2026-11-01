// The Topology room's panels (the HTML version's #tmodes, #ttoolWin, #dispTopo, #ttab, topoMeshProps): the strip left
// of the outliner (Object / Polygon / Edge / Vertex, then Snap, Symmetry and the snap mask brush), the tools in a
// floating window of their own (drag its bar; double-click — a row / a column; its place kept), 👁 Display in the room
// (Wire, Smooth / Flat / Unlit, No textures, the active geometry's opacity and PDO), the transform table under Settings
// (P / R / S of the active mesh or the selection's centre / size — Ctrl + drag a cell: ±1, Ctrl+Shift: ±0.1; a drag
// across cells selects them, a number typed goes into all), and a mesh's settings there (its polygroups — a tree of
// its own: drag to move, click to pick, double click — select, 👁 ⧉ ✕, RMB — more —, the snap mask, the transform's
// pivot / Bake, how it is shown).
#pragma once
#include <QFrame>
#include <QListWidget>
#include <QJsonObject>
#include <functional>
#include <map>
#include <vector>

class QVBoxLayout;
class QLabel;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;
class QToolButton;
class QComboBox;
class QLineEdit;

class QSplitter;
class QScrollArea;

namespace hm {

class Viewport;
class Scene;

class TopoModes : public QFrame {
  Q_OBJECT
public:
  explicit TopoModes(Viewport* vp, QWidget* parent = nullptr);
  void sync();
private:
  Viewport* vp_;
};

class TopoTools : public QFrame {
  Q_OBJECT
public:
  TopoTools(QWidget* window, Viewport* vp, std::function<void(const QString&)> pick);
  void place(QWidget* vpBox);                           // (its kept place, else the viewport's top left)
  void sync();
protected:
  bool eventFilter(QObject* o, QEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
private:
  void save() const;
  Viewport* vp_;
  QWidget *bar_ = nullptr, *row_ = nullptr, *bar2_ = nullptr, *opts_ = nullptr;
  QSplitter* split_ = nullptr;                    // (the tools | the options: the line between them dragged)
  std::function<void(const QString&)> pick_;
  QString optSig_;
  void buildOptions();                                  // (the Tool Options of the tool in use)
  void buildModelOptions(QVBoxLayout* L);
  void runAct(const QString& a);                         // (the toolbar's one-click edits)
  void vsymMenu();                                       // (the virtual symmetry: a new group's kind, or the selected group's Apply / kind / removal)               // (the modelling tools' options)
  bool vert_ = false; QPoint grab_; bool drag_ = false;
};

// ▦ UV: the UV mode's tools in a floating window of their own (its bar dragged; its place kept) — the UV Editor, seams,
// Unfold, Pack, the selection's shells, turns / mirrors / lining up, the checker, the texture under the UV, the stretch
class UvTools : public QFrame {
  Q_OBJECT
public:
  UvTools(QWidget* window, Viewport* vp);
  void place(QWidget* vpBox);
  void sync();
protected:
  bool eventFilter(QObject* o, QEvent* e) override;
private:
  void save() const;
  Viewport* vp_;
  QWidget* bar_ = nullptr; QLabel* status_ = nullptr; QScrollArea* scroll_ = nullptr; QWidget* body_ = nullptr;
  QPoint grab_; bool drag_ = false;
  QDoubleSpinBox* box_[4] = {};                   // (the transform: U, V, width, height of the selection's box)
  QToolButton *wpBtn_ = nullptr; QWidget* stretchScale_ = nullptr; QToolButton* ppBtn_ = nullptr;   // (Warp, Puppet: lit while on)
  QToolButton *rbBtn_ = nullptr, *pbBtn_ = nullptr; QSpinBox* brR_ = nullptr; QDoubleSpinBox* brK_ = nullptr;   // (the Relax / Pin brush: lit while on; its size, strength)
  QPushButton* symBtn_ = nullptr; QDoubleSpinBox* symAx_ = nullptr;
  QComboBox *texW_ = nullptr, *texH_ = nullptr; QDoubleSpinBox* texPad_ = nullptr; QPushButton* padShow_ = nullptr; QDoubleSpinBox* tdWant_ = nullptr; QLabel* tdNow_ = nullptr; std::map<QString, QLabel*> checkN_;   // (checkN_: the checks' counts)   // (tdWant_ / tdNow_: the texel density wanted, the selection's)   // (the texture the UV is laid out for: its size, the padding)   // (the UV Symmetry: Y / Alt+Y change it from the keys)
  class SliderRow* dispOp_ = nullptr;                    // (the display's opacity: shown while the checker / stretch / texture show)
  QWidget* sets_ = nullptr; QToolButton *setAdd_ = nullptr, *setExit_ = nullptr, *setIso_ = nullptr, *setDel_ = nullptr;   // (UV Sets: the list, Add to, Exit, Isolate)
  void syncSets();
};

// 👁 Display's strip in the Topology room; view() / setView() — the project's settings.view (the meshes' Wire)
QFrame* makeTopoDisplay(QWidget* window, Viewport* vp, Scene* sc, std::function<QJsonObject()> view, std::function<void(const QString&, const QJsonValue&)> setView);
void syncTopoDisplay(QFrame* f);

class TransformTable : public QFrame {
  Q_OBJECT
public:
  explicit TransformTable(Viewport* vp, QWidget* parent = nullptr);
  void sync(bool force = false);
protected:
  bool eventFilter(QObject* o, QEvent* e) override;
private:
  void select(const std::vector<int>& cells);
  void applyTyped(const QString& s);
  bool pend_ = false, pendForce_ = false;
  Viewport* vp_;
  QLabel *lab_ = nullptr, *hint_ = nullptr;
  QComboBox* unit_ = nullptr; QComboBox* space_ = nullptr;   // (World / Local)
  std::vector<QLineEdit*> cells_;
  std::vector<int> sel_;
  std::map<int, double> typed_;                          // (typed, not applied yet: Tab goes on, Enter applies them all at once)
  void markTyped();
  struct Scrub { int cell = -1; double v0 = 0, x0 = 0; bool moved = false; } scrub_;
  int pickA_ = -1;
  QByteArray sig_;
};

void topoMeshProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id);

}  // namespace hm
