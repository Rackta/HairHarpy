// The reference canvas MODE (🖼 Reference ▾ → ＋ / ✏ Canvas): a see-through window over the viewport, the only thing that takes the mouse and
// the keys while it is open (the model stays seen, nothing of it can be touched; the wheel and the middle button move the view, the
// canvas locked to the model on screen). A canvas is a smart object of the project: all its layers kept, one canvas shown by any number
// of planes (shared: a change reaches them all) or copied into a separate one. It draws a reference plane's picture with the Paint room's
// brushes and layers — the brush list with ABR tips, the pen's pressure, opacity / flow, layers with blends, masks and groups
// (Photoshop-like). Ctrl held (or the pen's eraser end) — the brush erases. The plane's image shows under the layers to trace
// over; what is drawn shows on the plane in the viewport as it is drawn. Symmetry on (Shift+S): a second, mirrored cursor (across
// X = 0 of the world, seen on this plane) and every dab mirrored with it.
// 📍 Markers (M): points of the world seen on every view — a click puts one (at the model's depth), a drag moves it (along this
// canvas only: its depth stays), Ctrl+click deletes it. On every canvas each marker shows where it falls with a guide line at its
// height, and in every pane of the viewport as a ring (side views: its guide line too) — e.g. a lock sketched in the front view
// with a marker at its tip shows the side view where the tip has to be.
// The drawing is kept with the plane in the project (not in the scene's undo history: the canvas has its own Ctrl+Z).
#pragma once
#include "app/PaintSession.h"
#include "scene/Refs.h"
#include "view/Viewport.h"
#include "view/Camera.h"
#include <QImage>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QTransform>
#include <QWidget>
#include <functional>
#include <map>
#include <optional>

class QPushButton;
class QFrame;
class QScrollArea;
class QSlider;
class QTimer;
class QCheckBox;
class QLabel;
class QPainter;
class QStackedWidget;
class QComboBox;
class QListWidget;
class QLineEdit;
class QVBoxLayout;

namespace hm {

class Viewport;
class Scene;
class PaintSession;
class PaintCanvas;
class SliderRow;
class RefLayerList;
class ColorPicker;
class SwatchGrid;

class RefCanvas : public QWidget {
public:
  RefCanvas(QWidget* parent, QWidget* vpBox, Viewport* vp, Scene* sc);
  void openFor(int planeId, bool insert = false);   // (the mode, for this plane's canvas; insert — the file window for images at once)
  void newSheet(const QString& view);        // (a new canvas the size of the screen on a new plane of this view, the mode opened on it)
  void leave();                              // (the mode closed: the drawing kept, the view as it was)
  bool isOpen() const { return open_; }
  std::function<void()> save;                // (Ctrl+S in the mode)
  int planeId() const { return id_; }
  void flush();                              // the drawing into the scene (its layers packed) — before saving, switching, closing
  void packInto(Scene& out) const;           // (a copy of the scene being saved: the open drawing's layers as they are now)
  void reset();                              // (another project: the open drawing dropped, the window hidden)
  std::function<void()> changed;             // (the project changed: a stroke, a marker)
  std::function<void(bool)> focusLayout;
  // (the session's record — development: ● Rec / ✎ Note on the canvas's bar, as the main window's)
  void setRecHooks(std::function<void()> toggle, std::function<void()> note, std::function<bool()> on);
  void syncRec();     // (the canvas open: the main window's outliner and settings column out of the way — the canvas takes their room; false — back)
  QString testCmd(const QStringList& w);     // (checks from --events: refcanvas open / tool / stroke / mirror / key / shot / info)
protected:
  void closeEvent(QCloseEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  bool eventFilter(QObject* o, QEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;
private:
  Viewport* vp_; Scene* sc_; PaintSession* s_; PaintCanvas* canvas_;
  QWidget* vpBox_; QFrame* top_ = nullptr; QScrollArea* side_ = nullptr; QTimer* follow_ = nullptr; QSlider* op_ = nullptr; QLabel* title_ = nullptr;
  int id_ = -1, cid_ = -1; bool open_ = false, dirty_ = false; QRect pend_; bool pendAll_ = false;   // (id_: the plane; cid_: its canvas, in the session)
  bool hasCam_ = false; Camera savedCam_; int savedPane_ = 0, wasActive_ = -1;
  // the view turned while the canvas is open (the camera rolled round the middle of what is seen; all of it put back on leaving)
  double roll_ = 0, rollRaw_ = 0; bool hasLoc_ = false; M4 savedLoc_ = m4Identity(); QPushButton* rotB_ = nullptr;
  void rollBy(double deg);
  QPointF seenCentre() const;
  void enterView(const std::function<bool(const Camera&)>& fits, const QString& view);
  void followView();
  void syncPlaneUi();
  void insertDialog();
  // a pasted picture, kept whole with its place: moved / scaled again from the original, nothing cut at the canvas's edge (until the layer is painted on)
  struct Placed { QImage src; double cx = 0, cy = 0, sx = 1, sy = 1, a = 0; qint64 key = 0; };
  std::map<int, Placed> placed_;
  bool floatPlaced();
  bool placedValid(int layer) const;
  QJsonObject packAll() const;
  void loadPlaced(const QJsonObject& data);
  QString tool_ = "brush"; int drag_ = -1;
  QCheckBox* show_ = nullptr; QLabel* info_;
  // ---- the tool window: a panel of this window, dragged by its bar (its place kept) — the tools | their modes and the brush sets | their settings ----
  // tools: brush, eraser, selection, fill, shapes, move, select (⬚ Transform), marker (📍 the anchors seen on every view)
  QFrame* tw_ = nullptr; QWidget* twBar_ = nullptr; QPoint twGrab_; bool twDrag_ = false;
  QStackedWidget *modes_ = nullptr, *opts_ = nullptr; std::map<QString, int> modePage_, optPage_;
  std::map<QString, QPushButton*> toolBtns_, modeBtns_;
  QComboBox* setBox_ = nullptr; QListWidget* brushList_ = nullptr;
  void buildTools();
  void placeTools();
  void saveTools() const;
  void syncModes();
  void fillBrushes();
  // ---- the side panel: the reference (opacity, over / behind the model, the canvases of the project, all layers moved in the view), the layers ----
  QPushButton *frontB_ = nullptr, *behindB_ = nullptr; QComboBox* lib_ = nullptr; QLabel* opV_ = nullptr;
  RefLayerList* layerList_ = nullptr; QComboBox* blend_ = nullptr; SliderRow* lop_ = nullptr; QPushButton* lockB_ = nullptr;
  // ---- the colour (over the reference panel): the two colours, the picker with its views (quad / triangle / round / bars / image),
  // the hex, the kept colours and the recent ones (each unfolded or folded; kept between runs) ----
  ColorPicker* picker_ = nullptr; SwatchGrid *savedG_ = nullptr, *recentG_ = nullptr; QLineEdit* hex_ = nullptr; QPushButton* fillCol_ = nullptr;
  void buildColour(QVBoxLayout* v);
  void addRecent(const QColor& c);
  void colourPopup(QWidget* anchor, const QColor& init, std::function<void(const QColor&)> set, std::function<void()> done = {});
  void buildSide(QWidget* col);
  void fillLibrary();
  void syncLayerProps();
  void loadCanvas(int cid);
  void layerAction(const QString& what, QWidget* anchor = nullptr);
  // ---- selection (rect / ellipse / lasso / polygonal lasso / magic wand; new / add (Shift) / subtract (Alt)): a mask over the canvas, its
  // outline the marching ants; the brush, the eraser, the fill and the shapes keep inside it, Delete clears it, Move / Transform lift it.
  // Polygonal lasso: clicks put its corners, a click on the first one / Enter / a double click closes it, Backspace — the last corner back, Esc — dropped.
  // Magic wand: what matches the clicked pixel's colour (tolerance; touching it only or everywhere; this layer or all of them) ----
  QImage selMask_; QPainterPath selPath_; QString selKind_ = "rect"; int selOp_ = 0, selOpNow_ = 0; QPolygonF lasso_; bool selDrag_ = false; QPointF sel0_, sel1_;
  QTimer* ants_ = nullptr; int antsOff_ = 0;
  QPolygonF poly_; bool polyOn_ = false; int polyOp_ = 0;
  int wandTol_ = 32; bool wandContig_ = true, wandAll_ = false;
  void polyClose();
  // the pixels matching the seed's colour (the bucket's and the wand's): 255 where they match, the box round them
  std::vector<uchar> matchAt(QPoint seed, int tol, bool contig, bool all, QRect& box) const;
  bool hasSel() const { return !selMask_.isNull(); }
  QRect selBox_;
  void setSelection(const QPainterPath& path, int op);
  void outlineSel();
  void deselect();
  void selectAll();
  void invertSel();
  bool selInput(int phase, const QPointF& t, Qt::KeyboardModifiers mods);
  void clipToSel(QImage& img, const QImage& before, const QRect& r) const;
  void clearSelected();
  bool floatSel();
  void drawSel(QPainter& p);
  // ---- fill (the bucket): the colour over what matches the clicked pixel ----
  int fillTol_ = 32; bool fillContig_ = true, fillAll_ = false; double fillOp_ = 1;
  void fillAt(const QPointF& t);
  void fillSelection(const QColor& c);      // (Alt+Backspace — the colour, Ctrl+Backspace — the second one: over what is selected, or the whole layer)
  // ---- shapes: a rectangle / an ellipse / a line, filled or outlined (Shift — square, circle, 45°) ----
  QString shapeKind_ = "rect"; bool shapeFilled_ = false, shaping_ = false; double shapeW_ = 6, shapeOp_ = 1; QPointF sh0_, sh1_; Qt::KeyboardModifiers shMods_;
  bool shapeInput(int phase, const QPointF& t, Qt::KeyboardModifiers mods);
  QPainterPath shapePath() const;
  // ---- ✥ Move, all layers: the whole canvas moved in the view (its plane shifted; ⟲ puts it back where it was opened) ----
  QString moveKind_ = "layer"; QTransform mvMap0_; QPointF mvW0_; M4 mvM0_ = m4Identity(); int mvW_ = 1, mvH_ = 1; Vec3 mvDone_, offD_;
  bool moveAllInput(int phase, const QPointF& t);
  void shiftPlane(const Vec3& d);
  // ---- split: two views side by side, each with its own canvas — a lock drawn in both and its places seen, as a drawing in several views.
  // The active view is drawn on (its layers in the list); the other shows its drawing; only a click in it makes it the active one (the
  // pointer moving over it changes nothing); the wheel and the middle button move the view under the pointer. Both views at one scale and
  // height. The windows as they were come back on leaving. ----
  bool splitOn_ = false; std::map<int, int> panePlane_; int navPane_ = -1; bool overOther_ = false;
  QPushButton* splitB_ = nullptr; QComboBox* splitLayout_ = nullptr; std::map<int, QComboBox*> paneBox_;
  bool panelsOn_ = true;                    // (Tab: the tool window and the side panel shown)
  struct Stash { paint::Document doc; std::vector<PaintSession::Undo> undo, redo; int layer = -1; std::map<int, Placed> placed; bool dirty = false; };
  std::map<int, Stash> stash_;
  std::map<int, Vec3> offOf_; std::map<int, std::pair<double, double>> rollOf_;   // (split: each plane's ✥ offset since opened, each pane's turn — the one drawn in has them in offD_ / roll_)
  std::vector<int> madeHere_;               // (planes the split made for its windows: left out of the project on leaving if nothing was drawn on them)              // (the other view's canvas, kept open as it is — its layers, its undo)
  bool hasPanes_ = false; Viewport::PaneState savedPanes_;
  void capturePanes();
  void setSplit(bool on);
  void setActiveSide(int pane);
  void applyPaneView(int pane, const QString& v, const Camera& ref);
  void setPaneViewTo(int pane, const QString& v);
  void placePaneBoxes();
  void stashCurrent();
  void takeUp(int planeId);
  int paneAt(const QPointF& w) const;
  int makeSheet(const QString& view, const Camera& cam);
  void composeNow(bool sync = false);       // (sync: the whole picture composed here and now — before switching canvases; else on a worker)
  // the whole picture recomposed (an opacity / blend / visibility change, …) on a worker thread, the latest change winning: the sliders
  // stay smooth; strokes' small parts meanwhile are still composed here and laid over the worker's picture when it comes
  bool jobRunning_ = false, jobAgain_ = false; int jobGen_ = 0; QRect jobDirty_;
  void startJob();
  void jobDone(const QImage& img, int gen, int cid);
  QJsonObject packDoc(const paint::Document& d, const std::map<int, Placed>& placed) const;
  // (the other floating windows of the main window — the Tool window, panels: hidden while the canvas is open, it is modal; shown again on leaving)
  std::vector<QPointer<QWidget>> hidden_;
  RefPlane* plane() const;
  bool frame(M4& M, Vec3& n, int& W, int& H) const;   // (the plane's matrix in its home pane, its normal, the canvas's size)
  std::optional<QPointF> toTex(const Vec3& world) const;   // (a world point seen on the plane: canvas pixels)
  std::optional<Vec3> toWorld(const QPointF& t) const;     // (a canvas pixel: its point on the plane)
  Vec3 normal() const;
  void setTool(const QString& t);
  void composeSoon();
  void markersChanged();
  void syncInfo();
  // ⬚ Transform (T) / ✥ Move (V): a piece of the active layer cut out and floating — moved, scaled (corners; Shift — keeping its
  // proportions), turned (the round handle; Shift — 15° steps); Enter puts it back, Esc cancels. Move floats the whole layer and
  // puts it back on release. ◧→◨ / ◨→◧: one half of the layer mirrored onto the other across X = 0 of the world
  struct Float { bool on = false; QImage img; double cx = 0, cy = 0, sx = 1, sy = 1, a = 0; int layer = -1; bool pasted = false, keep = false; } fl_, fl0_;   // (pasted: a new layer made for it — Esc removes the layer)
  int xmode_ = 0, xk_ = 0; QPointF xp0_; QRectF selRect_; bool selecting_ = false;
  QTransform flT() const;
  bool xformInput(int phase, const QPointF& t, Qt::KeyboardModifiers mods);
  bool floatRect(const QRect& r);
  void applyFloat();
  void cancelFloat();
  void drawFloat(QPainter& p, const QRectF& r);
  // Ctrl+V: the clipboard's picture (or an image file copied in Explorer) onto a new layer above the active one, floating under ⬚
  // Transform — fitted into the canvas when bigger, Enter puts it down
  void pasteClipboard();
  bool pasteImage(const QImage& img, std::optional<QPointF> at = std::nullopt);
  std::optional<double> mirrorX() const;
  void mirrorCopy(bool leftToRight);
  // ⬚ Transform's own steps: Ctrl+Z / Ctrl+Shift+Z while a piece floats (each drag one step; all undone — the piece put back); flipped
  // across / along, put back as it was (no turn, no flip, its own size); a pasted picture's place kept through the canvas's undo too
  std::vector<Float> flHist_, flRedo_; Float flStart_; std::map<qint64, Placed> placedByKey_;
  void flBegin();
  void flPush();
  bool xformUndo(bool redo);
  bool ensureFloat();
  void flipFloat(bool horizontal);
  void resetFloat();
  void restorePlaced();
  bool focused_ = false;
  QPushButton *recB_ = nullptr, *noteB_ = nullptr; std::function<void()> recToggle_, noteTake_; std::function<bool()> recOn_;
  QString lastStroke_;                       // (the hint line's end: the last stroke drawn with the pen and its pressure, or the mouse)
  // ▦ Grid (Ctrl+'): the views' grids on / off — every window of the split at once
  QPushButton* gridB_ = nullptr; std::map<int, double> gridOp_, axesOp_;
  void toggleGrid();
  void syncGridB();
};

}  // namespace hm
