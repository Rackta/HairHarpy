// The main window, laid out as the HTML version: the top bar (File, Save, Autosave, Reload · Export ▾, Bridge ▾, Send ·
// Display ▾), the second bar (the outliner's head · the viewport bar: Gizmo World / X Y Z, Camera ▾, Reference ▾, Solo,
// Lock select, Floor, Axes, the rooms, Hotkeys), the FAQ picture in the top right corner; below — the outliner, the
// viewport, the right column (the tools; Active Tool, Settings, Convert to geometry). Hotkeys: Keymap, caught for the
// whole window (not while typing in a text field).
#pragma once
#include "scene/Convert.h"
#include "scene/Scene.h"
#include <QHash>
#include <QMainWindow>
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

class QLabel;
class QPushButton;
class QFrame;
class QVulkanInstance;

namespace hm {

class Viewport;
class ReportWindow;
class ConvertPanel;
class Outliner;
namespace ui { class DockColumn; class DockPanel; class ToolBox; class HairToolWin; }

class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QVulkanInstance* vk);
  bool openProject(const QString& path);
  bool saveProject(bool saveAs);                         // Ctrl+S / Ctrl+Shift+S
  bool saveProjectTo(const QString& path);
  void forgetChanges() { forget_ = true; }               // (checks run from the command line: quit without asking)
  bool saveViewportShot(const QString& png);
  bool saveWindowShot(const QString& png, bool cursor = false);   // the whole window (the panels, its floating windows too; cursor: the pointer drawn in — an annotation)
  QImage windowShot(bool cursor = false);                          // (the same picture, kept in memory — no PNG written and read back)
  void resetView();
  void setView(const QString& v);
  void selectItems(const std::set<int>& ids);
  // checks: events posted to the viewport and commands — see runEvents in MainWindow.cpp; → a report
  QString runEvents(const QString& script);
  // files (MainWindowFiles.cpp): ＋ New project, a project opened as a copy (an example, an autosave — no file of its own),
  // migration, the start (⟳ Reload's project, else the File window and the recovery offer)
  bool newProject(bool ask = true);
  bool openProjectCopy(const QString& path, const QString& name, bool unsaved);
  bool migrateFile(const QString& path, const QString& name, bool wrap);
  void startup(bool projectGiven);
  void importDropped(const QStringList& files, const QPoint& at = QPoint(-1, -1), bool ask = true);   // (at: where on the screen — pictures go onto the model there; ask: models through the import window — off for the checks)
  void dropPictures(const QStringList& pics, const QPoint& at);

protected:
  void closeEvent(QCloseEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dragMoveEvent(QDragMoveEvent* e) override;
  void dropEvent(QDropEvent* e) override;
  QByteArray exportDragItems(const std::vector<int>& ids);   // (items dragged to another HairHarpy: their file's path)
  void importDragItems(const QString& path);                  // (items dropped from another HairHarpy into this project)
  bool eventFilter(QObject* o, QEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void moveEvent(QMoveEvent* e) override;

private:
  void buildUi(QVulkanInstance* vk);
  QWidget* buildTopBar();
  QWidget* buildBar2();
  void paintImportTexture(bool quiet = false);                // (the Paint room: the model's own textures as a layer of the UV set shown — once per set)
  QWidget* buildDisplayStrip();
  void buildActions();
  void runAction(const QString& id);
  void pickTool(const QString& t);
  void openToolPie();
  void openMainPie(const QPoint& global);
  void openTopoPie(const QPoint& global, bool hold = true);
  void openOutlinerPie(const QPoint& global);
  void openTopoOpsPie(const QPoint& global, bool hold = true);   // (Ctrl+RMB in the Topology room: the edits of the selected)
  void endAssign();                                      // (End over anything of the window: the keys for it)
  void openCustomPie(const QString& id);                 // (a pie of one's own, by its key)
  void openIsolatePie();                                 // H held: every hiding / isolating function on a ring
  void openAlphaRing();                                  // the Paint room, A held: the favourite alphas on a ring (released towards one — it stands in for the brush's tip)
  void faqForHovered();                                  // ~: the FAQ at the help of the button under the pointer
  bool hDown_ = false, hPie_ = false;                    // (H: down — a tap hides, held — the ring)
  QPushButton* unhideBtn_ = nullptr;
  void syncUnhide();
  void rebuildOutliner();
  void syncPanels();                                     // (the selection / the project changed: Settings, Convert)
  void updateTitle();
  void waitSave();                                       // (a background save still writing: waited for)
  bool unsavedOk();                                     // unsaved changes: save / discard / cancel
  QString thumbDataUrl();                                // the File window's preview: the middle of the viewport, 240 px JPEG
  void setViewSetting(const QString& key, const QJsonValue& v);   // 👁 Display (settings.view: the project's, not an undo step)
  QString displayName() const;                           // the project's file name (one not saved yet — projName_)
  void openFiles(const QString& tab = QString());        // 📁 File
  void autosaveNow(bool manual);
  void saveRecovery();
  bool handsBusy() const;                                   // (a button held / a stroke going on: the timed saves wait)
  void clearRecovery();
  void offerRecovery();
  void offerBrokenWalls();
  void reloadApp();
  void updateSendBtn();
  void showDropOverlay();
  bool createPreset(const QString& dir);
  QString presetGroupName() const;
  void afterLoad(const QString& path);                   // (recent list, its preview)
  Scene fileScene() const;                               // (what the file keeps: the Hair room's items, the other rooms under P.rooms)
  void applyRoom(const QString& room);
  void goRoom(const QString& room);                        // (the room switched and the window brought to it — once)
  int roomApplies_ = 0;
  QWidget* uiScaleControl();                             // (👁 Display: the interface size — applied by a restart)                   // (the room switched: its panels, tools, the outliner's one tree)

  QHash<QString, std::function<void()>> actions_;
  int savedChanges_ = 0;
  bool viewDirty_ = false, forget_ = false;              // (Display changed since the save; quit without asking)
  bool dirty() const;
  QImage paintSetPicture(const QString& set, const QRect& r);   // (the map shown, of a UV set — shown or not — as the model wears it; r: a part)
  void paintOtherChanged(const QString& set, const QRect& r);
  void paintImportAll();
  qint64 paintOtherRev_ = 0;
  void paintOthersRefresh();                 // (the Paint room: the other UV sets' paint composed for the model — every set's paint is seen at once)
  QImage paintWireImage() const;             // (the UV polygons of the set painted — the layered PSD's Wireframe layer)
  int changeTotal() const;                    // (every change that makes the project unsaved: the scene's commits and the Paint room's edits)
  std::function<void()> syncDisplay_;
  QString prevTool_ = "gizmo";
  int propsPin_ = -1;                                    // (📌: the item the Settings keep showing)
  QString projName_ = QStringLiteral("hair-project.hairmesh");
  bool startedOnce_ = false;                             // (the empty project of the program's start made: later ones reset the windows)
  bool forcedDirty_ = false;                           // (opened as a copy with unsaved work: an autosave, a recovery copy)
  int asLastChange_ = -1, recChange_ = -1, recGen_ = 0; bool recBusy_ = false, asBusy_ = false, asyncSave_ = false, saveBusy_ = false;   // (recBusy_: the recovery copy being written in the background; recGen_ — bumped when it is cleared)
  qint64 asLast_ = 0; QString asMsg_;
  QPushButton *sendBtn_ = nullptr, *expBtn_ = nullptr, *brgBtn_ = nullptr;
  QWidget* dropOverlay_ = nullptr;
  class RefBoard* board_ = nullptr;
  class RefCanvas* refCanvas_ = nullptr;                // (✏ a reference plane's drawing: the Paint room's brushes and layers; 📍 markers)

  Scene scene_;
  Viewport* viewport_ = nullptr;
  QWidget* vpBox_ = nullptr;                             // (the viewport's container)
  Outliner* outliner_ = nullptr;
  QWidget* olHead_ = nullptr;
  // the Paint room's docks: moved to any edge, tabbed, floated by the user; the arrangement kept per room
  std::vector<class QDockWidget*> paintDocks_; QPushButton* panelsBtn_ = nullptr; QPushButton* bakeBtn_ = nullptr; class QFrame *bakePop_ = nullptr, *bridgePop_ = nullptr;
  void paintPop(class QFrame*& pop, QPushButton* anchor, const std::function<QWidget*()>& make);   // (a Paint panel opened from the top bar, kept while the program runs)
  bool paintDocksOn_ = false, paintDocksMade_ = false;
  void paintDocksDefault(); void paintDocksEnter(); void paintDocksLeave(); void paintDocksSave();
public:
  QDockWidget* paintDock(const QString& name) const;   // (checks)
private:
  ui::DockColumn* dock_ = nullptr;
  ui::ToolBox* tools_ = nullptr; ui::HairToolWin* htools_ = nullptr;   // (the Hair room's tools float in their own window, as the Topology room's)
  ui::DockPanel *dpTool_ = nullptr, *dpProps_ = nullptr, *dpConv_ = nullptr;
  ConvertPanel* convPanel_ = nullptr;
  ReportWindow* report_ = nullptr;
  class InspectorWindow* insp_ = nullptr;
  class HistoryBar* hist_ = nullptr;
  QPushButton* inspBtn_ = nullptr;
  int panelBuilds_ = 0;   // (testing: how many times the panels were made)
  bool pendRefresh_ = false, pendOutliner_ = false, pendAll_ = false;   // (the outliner / panels to be made again, once; pendAll_: not only for a selection click)
  QString lastSelSig_;                                   // (the selection the panels were last made for)
  QPushButton *faq_ = nullptr, *dispBtn_ = nullptr, *gzSpace_ = nullptr, *gzAx_[3] = {}, *soloBtn_ = nullptr, *lockSelBtn_ = nullptr;
#ifdef HM_DEVTOOLS
  class DevSession* dev_ = nullptr; QPushButton *recBtn_ = nullptr, *noteBtn_ = nullptr;   // (development: the session's record, its annotations)
#endif
  QFrame* dispStrip_ = nullptr;
  QHash<QString, QPushButton*> dispToggles_;
  // the Topology room
  class TopoModes* tmodes_ = nullptr;
  class TopoTools* ttools_ = nullptr;
  class UvTools* uvtools_ = nullptr;
  void syncUvTools();
  class TransformTable* ttab_ = nullptr;
  ui::DockPanel* dpTtab_ = nullptr;
  // the Paint room: its session, the right column's panels, the texture canvas window
  class PaintSession* paint_ = nullptr; std::vector<ui::DockPanel*> dpPaint_; QWidget* paintCanvas_ = nullptr; class PaintLeft* paintLeft_ = nullptr; class PaintProj* paintProj_ = nullptr; QWidget *paintTop_ = nullptr, *paintMode_ = nullptr; QPushButton* roomPaint_ = nullptr;
  QFrame* dispTopo_ = nullptr;
  QWidget* cornerBg_ = nullptr; QPushButton *roomHair_ = nullptr, *roomTopo_ = nullptr, *keysBtn_ = nullptr, *srBtn_ = nullptr, *ccBtn_ = nullptr, *wireBtn_ = nullptr;
  qint64 lastQ_ = 0; QString qFrom_;
  bool panelsPending_ = false;
  int paintTubeId_ = -1;   // (checks: the last test tube made — pv thicktube)
};

}  // namespace hm
