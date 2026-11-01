// Aa Smart Rename (Shift+F2, "Aa" in the outliner's head): tidy names, prefixes, find & replace, sorting by connections.
// Scope: the selected groups with everything inside (and the selected items), or the whole scene; a type filter.
// Clean up & renumber: "copy" / "part" tails go, and items of one type with the same base name are numbered 1, 2, 3 …
// in outliner order (in each group, or through the whole scope); a single item with its own name keeps it; new-logic
// groups — the network names (C_ / S_). Sort: inside every group in scope — sub-groups first, then the curves and walls
// by their network names (an older group: every lock as curve, its wall, next curve …), then the rest.
#pragma once
#include <QDialog>

class QVBoxLayout;

namespace hm {

class Viewport;
class Scene;

// (checks) the renumbering plan of the whole scene "id old → new" lines; a group sorted → its children
QString smartRenamePlanDump(Viewport* vp, Scene& sc);
QString smartRenameSortDump(Viewport* vp, Scene& sc, int gid);

class SmartRename : public QDialog {
  Q_OBJECT
public:
  SmartRename(Viewport* vp, Scene* sc, QWidget* parent = nullptr);
  void refill();                                   // (the dialog drawn anew: after every change)
private:
  Viewport* vp_;
  Scene* sc_;
  QWidget* body_ = nullptr;
  QVBoxLayout* lay_ = nullptr;
};

}  // namespace hm
