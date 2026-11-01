// The outliner (renderOutliner): on the left the scene — the Head row, meshes, guides, mesh modifiers, the Crowns
// section, the Cocoons section (⏻ — every shell modifier off at once; the cocoons as a tree of their own: trunk → its
// branches, their shell modifiers under them); on its right the Curves column — the curve groups, an open group's
// curves | service curves | walls side by side in columns, its scheme above them. A row: click — select (Shift / Ctrl —
// add / take off), double-click — rename (a group: select everything inside), ▸ — fold (Shift — the whole branch), the
// eye (Shift + drag over the eyes — the same for every row passed), ⏻, the colour, the guides' mode. Drag the selection
// onto a row — before / into / after it (onto empty space — out of every group); a cocoon onto another one — its branch;
// a drag from empty space or an unselected row — box selection.
#pragma once
#include <QColor>
#include <QPainterPath>
#include <QPixmap>
#include <QWidget>
#include <climits>
#include <functional>
#include <map>
#include <set>
#include <vector>

class QLineEdit;
class QScrollArea;

namespace hm {

class Viewport;
class Scene;
struct Item;
class SchemeView;

// one tree of the outliner, drawn by itself: rows (an item, a section's head, a hint), a pair's columns side by side,
// the boxes round an open group's content; the cells' handlers get the clicks
class OutlinerView : public QWidget {
  Q_OBJECT
public:
  enum Flag { Sel = 1, ChildSel = 2, Rel = 4, Hid = 8, LockSel = 16, Section = 32, Hint = 64 };
  using Click = std::function<void(Qt::KeyboardModifiers)>;
  struct Cell {
    QString text; int w = 16; QColor color; double op = 0.7; QString tip;
    QPixmap pix; QColor swatch; bool swatchOn = false, swatchCell = false, eye = false, tagBox = false;
    QColor tagFg, tagBorder, tagBg;
    QString prefix; QColor prefixColor;      // (the name: a dim part before it — a cocoon's lock)
    Click click, dbl, rclick;
    QRect r;
  };
  struct Row {
    int id = -1, flags = 0, pad = 4, gap = 4, padR = 6, h = 27;
    QRect r;
    std::vector<Cell> left, right;            // (right: from the row's right edge inwards)
    Cell name;
    QString tip;
    Click click, dbl;
    int drag = 0, drop = 0;                   // 1 — items (the selection), 2 — a cocoon
    bool group = false, empty = false;
    QColor nameColor;
  };
  explicit OutlinerView(QWidget* parent = nullptr);
  void begin();
  Row& add(int x0, int x1, int h = 27);       // at the current line (a pair: several, side by side)
  void next(int h) { y_ += h; }
  int y() const { return y_; }
  int boxOpen(int x);                          // an open group's content box from here on
  void boxClose(int i);
  void addWidget(QWidget* w, int x0, int x1);  // (a scheme: a widget of its own in the list)
  void finish();
  const Row* row(int id) const;
  void editName(int id, const QString& text, std::function<void(const QString&)> done);
  void flash(int id);

  std::function<void(Qt::KeyboardModifiers)> emptyClick;
  std::function<void(const QPoint& global)> pie;           // (the pie's button over it — Shift+RMB unless rebound)
  std::function<void(const std::set<int>& ids, bool add)> boxSelect;
  std::function<bool(int id)> selected;
  std::function<void(int id)> eyeStart, eyeOver;
  std::function<void()> eyeEnd;
  std::function<void(int kind, const std::vector<int>& ids, int target, const QString& zone)> dropped;
  std::function<std::vector<int>(int id)> dragIds;
  std::function<void()> resized;
  // 🔗 the cocoons' links: a line in the gutter on the left from a cocoon's row to the one it takes loops from (arrow
  // there); Alt + click on it — broken; Alt + drag from a cocoon's row onto another — a new one
  void setLinks(const std::vector<std::pair<int, int>>& links) { links_ = links; }
  std::function<void(int from, int to, Qt::KeyboardModifiers mods)> linkClicked;
  std::function<void(int from, int to)> linkMade;
  std::function<QString(int from, int to)> linkTip;

protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void contextMenuEvent(QContextMenuEvent* e) override;
  void leaveEvent(QEvent*) override;
  bool event(QEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dragMoveEvent(QDragMoveEvent* e) override;
  void dragLeaveEvent(QDragLeaveEvent*) override;
  void dropEvent(QDropEvent* e) override;

private:
  struct Box { int x, y0, y1; };
  void place(Row& r);
  int rowAt(const QPoint& p) const;
  const Cell* cellAt(const Row& r, const QPoint& p) const;
  std::vector<Row> rows_;
  std::vector<Box> boxes_;
  std::vector<QWidget*> widgets_;
  int y_ = 4, hover_ = -1, lastW_ = 0;
  // the press: a click, a drag of the selection, a box, the eyes
  struct Press { bool on = false, moved = false, eye = false, box = false, dbl = false; QPoint at; int row = -1; Qt::KeyboardModifiers mods; } pr_;
  QRect box_; std::set<int> boxHit_;
  int dropRow_ = -1; QString dropZone_;
  std::vector<std::pair<int, int>> links_;
  struct LinkDrag { bool on = false; int from = -1; QPoint at; } ld_;
  int hoverLink_ = -1;
  int linkAt(const QPoint& p) const;
  QPainterPath linkPath(size_t i, double* yb = nullptr) const;
  int flashId_ = -1;
  QLineEdit* edit_ = nullptr;
};

class Outliner : public QWidget {
  Q_OBJECT
public:
  Outliner(Viewport* vp, Scene* sc, QWidget* parent = nullptr);
  void rebuild();
  void reveal();                               // F over the outliner: the active item's groups open, its row shown
  void renameSelected();                       // F2
  void parentSelection();                      // P: into the last selected (a group — or a cocoon: branches)
  void unparentSelection();                    // Shift+P
  bool pointerOver() const;
  std::function<void(const QPoint& global)> pieRequested;   // (the outliner's own pie: groups, merging)
  void toggleCocoons();                        // ◍ in the outliner's head
  bool cocoonsFolded() const { return cocoonsFolded_; }
  int paneWidth() const;
  OutlinerView* view(bool curves) const { return curves ? curvesV_ : sceneV_; }
  // (checks) where a row is: its view and its centre (dx from its left — a cell)
  OutlinerView* rowAt(int id, QPoint* at, int dx = INT_MIN) const;   // (dx < 0: from its right edge)
  void moveItems(std::vector<int> ids, int target, const QString& zone);
  void groupSelection();                       // Alt+G: the selected into a new group in their place (both rooms)
  std::vector<int> followers(const std::vector<int>& ids) const;   // (the service curves and walls of the curves moved — they go along)
  bool reparentCocoon(int id, int pid, bool quiet);
  bool linkCocoon(int from, int to);
  bool unlinkCocoon(int from, int to);
signals:
  void widthChanged(int w);
  void propsFlash();                           // (a wall's ⚙: the Settings window flashes)
protected:
  void resizeEvent(QResizeEvent* e) override;
private:
  enum class Side { Scene, Curves };
  void walk(const std::vector<int>& ids, int depth, bool inGroup, OutlinerView* v, Side side, int width);
  void itemRow(OutlinerView::Row& r, const Item& it, int depth);
  void headRow(OutlinerView* v, int width);
  void sections(OutlinerView* v, int width);
  void cocoonRows(OutlinerView* v, int width);
  void edit(const std::function<void(Scene&)>& fn, bool topology = false);
  QString sideOf(const Item* it) const;
  bool shows(const Item& it, Side side);
  std::vector<int> topSelected() const;
  void setWidths(int olw, int ccw, bool save);
  void startRename(int id);

  Viewport* vp_;
  Scene* sc_;
  QScrollArea *sceneScroll_, *curvesScroll_;
  OutlinerView *sceneV_, *curvesV_;
  QWidget* ccol_;
  int olw_ = 235, ccw_ = 340;
  bool crownsFolded_ = false, braidsFolded_ = false, cocoonsFolded_ = false, building_ = false, eyeState_ = true;
  int eyeN_ = 0;
  std::set<int> hl_, rel_;
  std::map<int, std::pair<bool, bool>> kinds_;   // (a group: holds scene things / curve things)
  std::vector<SchemeView*> schemes_;
  int swatchPending_ = -1;
  int texPending_ = -1;                                       // (▦: a second click within the wait — a double click: the texture window)
  void lookCells(std::vector<OutlinerView::Cell>& right, const std::vector<int>& ms, int rowKey);
};

}  // namespace hm
