// The Paint room's left column (in place of the outliner): the manager window on top and the tools with the brush's settings under it.
// The manager window: on its left the modes — the texture in 2D (its UV set picked under the button; opened over the whole viewport by
// its own button) and the model's preview (PaintPreview: nothing painted there — parts chosen and hidden, the selection tools under the
// button); on its right the room's own lists — Objects (the meshes: an eye and a lock each; the selected ones are the only ones
// painted, none selected — all) and Visibility sets (Default, a set per UV set, the user's own). The eye hides in this room only; the
// lock keeps the brush off. In every list: drag over the eyes / locks — the same for every row passed; Alt + click — only that one
// shown / only that one paintable. The room's visibility and polygon selection have their own undo history (VisHistory): Ctrl+Z with
// the pointer over the manager window (not over the texture painted in it) takes back the hiding, not the stroke.
#pragma once
#include <QElapsedTimer>
#include <QPointer>
#include <QRect>
#include <QWidget>
#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QSplitter;
class QVBoxLayout;

class QStackedWidget; class QListWidget;
namespace hm {

class PaintCanvas;
class PaintSession;
class Scene;
class Viewport;
class VisList;

class PaintLeft : public QWidget {
public:
  void freezeSelection(bool rest);                       // (the select tool: the selected polygons frozen — or everything but them)
  PaintLeft(PaintSession* s, Viewport* vp, Scene* sc, QWidget* parent = nullptr);
  PaintCanvas* canvas() const { return canvas_; }
  std::function<QRect()> viewportRect;        // (the screen: where the texture goes when it is opened over the whole viewport)
  void setFull(bool on);                      // the texture over the whole viewport / back in the column
  bool full() const { return full_; }
  void refresh();                             // (the lists made anew)
  void setTool(const QString& t);
  class PaintTools* tools() const { return tools_; }
  void setProj(class PaintProj* p) { proj_ = p; syncOpts(); }
  // the drawing modes' menu (as 3DCoat's E panel): under its button in the tool's settings (atPointer — under the pointer: checks).
  // It is there only while the pointer is over it — gone the moment the pointer leaves; a click picks the mode. hold: it stays (checks)
  void modesMenu(bool atPointer, bool hold = false);
  QWidget* modesWidget() const { return modeFly_; }
  // the manager window: its mode (0 — the texture, 1 — the model's preview), its undo history, the keys over its preview
  void setMgrMode(int m);
  int mgrMode() const { return mgrMode_; }
  class PaintPreview* preview() const { return preview_; }
  bool overManager() const;                   // (the pointer over the manager window, not over the texture painted in it: Ctrl+Z there — the visibility's)
  bool visUndo(bool redo);
  void historyReset();                        // (another project opened)
  bool previewKey(const QString& op);         // (H and its kin over the preview — true: done there)
  bool previewFrame();                        // (F over the preview)
  void setMgrKind(const QString& k);          // (the preview's selection tool)
  // the manager window as a panel of its own over the viewport's bottom right (the author, 16.11: "stretch it so the square is big and
  // move it here") — moved by its title, sized by its top-left corner, kept where it was put; or back in the left column
  void setMgrFloat(bool on);
  bool mgrFloat() const { return mgrFloat_; }
  QWidget* floatWindow() const { return floatWin_; }
  void setFloatSize(int w, int h) { fW_ = w; fH_ = h; placeFloat(); }   // (checks)
  void placeFloat();
  // the parts as the main window's own docks (every panel of the room moved, tabbed, floated by the user — the author, 17.11:
  // "every time restart and wait just to move a window?"): the tools' buttons, the brush list, the settings, the manager
  QWidget* toolsPart() const { return toolsW_; }
  QWidget* brushListPart() const { return brushListW_; }
  QWidget* settingsPart() const { return settingsW_; }
  QWidget* managerPart() const { return mgr_; }
  void setDocked();                           // (the parts live in docks: the column itself stays hidden, its own floating panel is not used)
  QString sheet() const { return sheet_; }    // (the column's style — the docks take it, their parts look the same)
protected:
  void showEvent(QShowEvent* e) override;
  void hideEvent(QHideEvent* e) override;
  bool eventFilter(QObject* o, QEvent* e) override;
public:
private:
  class VisHistory* hist_ = nullptr; class PaintPreview* preview_ = nullptr; QWidget* mgr_ = nullptr; QStackedWidget* stack_ = nullptr;
  QWidget* floatWin_ = nullptr; QVBoxLayout* floatLay_ = nullptr; QSplitter* split_ = nullptr; QPushButton* floatBtn_ = nullptr;
  QWidget *toolsW_ = nullptr, *brushListW_ = nullptr, *settingsW_ = nullptr; QStackedWidget* toolList_ = nullptr; QListWidget *fillList_ = nullptr, *selList_ = nullptr; QWidget *brushPanel_ = nullptr, *noList_ = nullptr;
  void syncToolList();   // (the column of the tool's presets and the brush's settings: as the tool in hand needs them)
  bool docked_ = false; QString sheet_;
  bool mgrFloat_ = false; int fRight_ = 14, fBottom_ = 14, fW_ = 640, fH_ = 520;   // (the panel's place: its distance from the viewport's bottom-right corner, its size)
  void saveFloat() const;
  QPushButton *mode2d_ = nullptr, *modeModel_ = nullptr, *setBtn_ = nullptr, *undoBtn_ = nullptr, *redoBtn_ = nullptr; QWidget* selBox_ = nullptr; QLabel* setLabel_ = nullptr;
  std::vector<std::pair<QString, QPushButton*>> kindBtns_; int mgrMode_ = 0;
  void syncMgr();
  QPointer<QWidget> modeFly_, modeBtn_; QElapsedTimer modeFlyOff_;   // (modeFlyOff_: since a mode was picked — the button under the pointer does not open the menu again at once)
  PaintSession* s_; Viewport* vp_; Scene* sc_;
  VisList *objs_ = nullptr, *sets_ = nullptr;
  QLabel* objNote_ = nullptr;
  QPushButton* noPaintBtn_ = nullptr;
  PaintCanvas* canvas_ = nullptr; QWidget *canvasHost_ = nullptr, *fullWin_ = nullptr; QVBoxLayout *hostLay_ = nullptr, *fullLay_ = nullptr;
  QPushButton* fullBtn_ = nullptr;
  std::vector<std::pair<QString, QPushButton*>> toolBtns_;
  class PaintProj* proj_ = nullptr; class PaintTools* tools_ = nullptr; QWidget* opts_ = nullptr; QVBoxLayout* optLay_ = nullptr; QPushButton* freezeBtn_ = nullptr;
  void syncOpts();                            // (the tool's own settings)
  bool full_ = false, pending_ = false, regionFree_ = false; QString keptPick_;   // (keptPick_: the kept freeze mask chosen in the list; regionFree_: the layer's commands work on what the mask leaves free)
public:
  void syncTools() { syncOpts(); }            // (checks: the tool's settings made anew)
private:
  std::vector<int> actOn(int id) const;       // (a row's action: on every selected row when the row is among them)
  void fillSets();
  void later();
};

// the colour palette (the right column): the picker in its views, the two colours, the recent ones
QWidget* makePaintPalettePanel(PaintSession* s);

}  // namespace hm
