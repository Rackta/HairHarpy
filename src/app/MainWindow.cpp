#include "app/SymPanel.h"
#include "app/MainWindow.h"
#include "paint/LayerFx.h"
#include "app/InspectorWindow.h"
#include "app/KeyCastWindow.h"
#include "app/Overlays.h"
#include "app/HistoryBar.h"
#include "app/DevSession.h"
#include "scene/TopoEdit.h"
#include "scene/AnchorOps.h"
#include <numeric>
#include "app/ImportMesh.h"
#include "app/RibPanels.h"
#include "app/ImportWindow.h"
#include "app/ExportPanels.h"
#include "app/ZBrushLink.h"
#include "app/FileWindow.h"
#include "app/CameraMenu.h"
#include "app/ViewWindow.h"
#include "app/ViewFrame.h"
#include "app/Extras.h"
#include <QComboBox>
#include <QToolBar>
#include <QDockWidget>
#include <QCursor>
#include <QTreeWidget>
#include <QAbstractButton>
#include <QListWidget>
#include "app/RefBoard.h"
#include "app/RefPanels.h"
#include "app/TexWindow.h"
#include "app/TopoPanels.h"
#include "scene/Braid.h"
#include "scene/Looks.h"
#include "scene/Refs.h"
#include <QElapsedTimer>
#include <QWheelEvent>
#include <QScrollArea>
#include <QScrollBar>
#include "app/ConvertPanel.h"
#include "app/Outliner.h"
#include "app/PropsPanel.h"
#include "app/Scheme.h"
#include "app/SmartRename.h"
#include "scene/Cut.h"
#include "scene/AnchorOps.h"
#include <QClipboard>
#include "app/ReportWindow.h"
#include "app/ToolPanels.h"
#include "io/ProjectIO.h"
#include "io/Export.h"
#include "scene/Convert.h"
#include "scene/Edit.h"
#include "scene/Primitives.h"
#include "scene/Network.h"
#include "scene/Symmetry.h"
#include "scene/Tubes.h"
#include "scene/WallTools.h"
#include <QJsonDocument>
#include "ui/DockPanel.h"
#include "ui/Keymap.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "ui/ToolBox.h"
#include "view/Viewport.h"
#include "view/SceneGeometry.h"
#include "scene/MeshMods.h"
#include <QTextStream>
#include <QAbstractSpinBox>
#include <QApplication>
#include <cstring>
#include <QBuffer>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDesktopServices>
#include <QDir>
#include <QPointer>
#include <thread>
#include <QElapsedTimer>
#include <QWheelEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QActionGroup>
#include <QMenu>
#include <QToolTip>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QTextEdit>
#include <QTimer>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include "app/PaintCanvas.h"
#include "app/PaintPreview.h"
#include "app/RefCanvas.h"
#include "app/MaterialEditor.h"
#include "app/PaintBar.h"
#include "app/PaintLeft.h"
#include "app/PaintPanels.h"
#include "app/PaintProj.h"
#include "app/PaintTools.h"
#include <QPainterPath>
#include "app/PaintSession.h"

namespace hm {

namespace {
QFrame* vsep() { auto* f = new QFrame; f->setProperty("sep", true); f->setFixedSize(1, 22); return f; }
QPushButton* btn(const QString& t, const QString& tip = {}) { auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); return b; }
const char* VIEW_KEYS[] = {"persp", "ortho", "front", "right", "left", "back", "top"};
const char* VIEW_NAMES[] = {"Perspective", "Orthographic", "Front", "Left", "Right", "Back", "Top"};   // (the side views named for the side of the model seen: the camera on +X ("right") looks at its left side — Left)
}  // namespace

// a panel's notes (its "?"): a popup under the button, a line per note
static void showPanelHelp(QWidget* owner, QWidget* anchor, const QString& title, QString t) {
  t = t.isEmpty() ? QObject::tr("Nothing to tell about this tool.") : t.toHtmlEscaped().replace("\n", "<br>").replace("    ", "&nbsp;&nbsp;&nbsp;&nbsp;");
  auto* f = new QFrame(owner, Qt::Popup); f->setAttribute(Qt::WA_DeleteOnClose); f->setObjectName("pdHelp"); f->setStyleSheet("#pdHelp{background:#262626;border:1px solid #555;border-radius:6px} QLabel{color:#d0d0d4}");
  auto* fv = new QVBoxLayout(f); fv->setContentsMargins(12, 10, 12, 10); auto* hl = new QLabel("<b>" + title.toHtmlEscaped() + "</b><br><br>" + t); hl->setWordWrap(true); hl->setTextFormat(Qt::RichText); hl->setFixedWidth(400); fv->addWidget(hl);
  f->adjustSize(); QPoint p = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height() + 2)) - QPoint(f->width(), 0);
  if (QScreen* sc = anchor->screen()) { const QRect a = sc->availableGeometry(); p.setX(std::clamp(p.x(), a.left(), a.right() - f->width())); p.setY(std::min(p.y(), a.bottom() - f->height())); } f->move(p); f->show();
}

MainWindow::MainWindow(QVulkanInstance* vk) {
  buildUi(vk);
  buildActions();
  updateTitle();
  QSettings st;
  restoreGeometry(st.value("main/geometry").toByteArray());
  qApp->installEventFilter(this);
  setAcceptDrops(true);
  viewport_->onFileDrag = [this] { showDropOverlay(); };
  viewport_->beforeHeavy = [this] { saveRecovery(); };      // (files dragged over the viewport: its window gets them)
}

// ======================= layout =======================
void MainWindow::buildUi(QVulkanInstance* vk) {
  setWindowIcon(ui::icon("app_icon"));
  viewport_ = new Viewport(&scene_);
  viewport_->setVulkanInstance(vk);
  viewport_->setPieHost(this);
  paint_ = new PaintSession(this);
  // (the live gradients: where the surface lies under every texel of a UV set — the bake's own picture of it, made again only when
  // the scene's shapes or UVs changed)
  paint_->doc.positionsOf = [this](const QString& set, int w, int h, const float*& p, const float*& n) {
    const auto& G = viewport_->pvBakeGeom(set, w, h); if (G.w != w || G.h != h || G.p.empty()) return false; p = G.p.data(); n = G.n.data(); return true; };
  paint_->doc.geomRevOf = [this](const QString&) { return viewport_->pvGeomRev(); };
  // (an object shadow's casters: the named meshes' triangles — hidden ones too, the hair hidden to paint under it still casts)
  paint_->castersOf = [this](const QStringList& names) -> std::shared_ptr<const paint::TriMesh> {
    static QStringList kn; static qint64 kr = -1; static std::shared_ptr<const paint::TriMesh> kept;
    const qint64 rev = viewport_->pvGeomRev(); if (kept && kn == names && kr == rev) return kept;
    auto T = std::make_shared<paint::TriMesh>();
    for (const auto& [id, it] : scene_.items) { if (it.type != ItemType::IMesh || !it.poly || !names.contains(it.name)) continue;
      const paint::TriMesh m = paint::triMeshFrom(*it.poly); const int base = int(T->pos.size());
      T->pos.insert(T->pos.end(), m.pos.begin(), m.pos.end()); for (auto t : m.tri) { for (int& k : t) k += base; T->tri.push_back(t); } }
    kn = names; kr = rev; kept = T; return kept; };
  // (the colour spheres' rings follow the active layer: another layer picked, or the spheres made an ordinary layer — gone)
  connect(paint_, &PaintSession::layersChanged, this, [this] { viewport_->paintOverlayChanged(); });
  connect(paint_, &PaintSession::activeChanged, this, [this] { viewport_->paintOverlayChanged(); });
  outliner_ = new Outliner(viewport_, &scene_);
  outliner_->pieRequested = [this](const QPoint& g) { openOutlinerPie(g); };
  outliner_->exportItems = [this](const std::vector<int>& ids) { return exportDragItems(ids); };   // (dragged out to another HairHarpy)

  auto* app = new QWidget; app->setObjectName("app");
  auto* v = new QVBoxLayout(app); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
  // (the two bars at the window's top, across its whole width — over the docks at its sides as well as the middle)
  { auto* bars = new QWidget(this); bars->setObjectName("barsTop"); auto* bv = new QVBoxLayout(bars); bv->setContentsMargins(0, 0, 0, 0); bv->setSpacing(0);
    bv->addWidget(buildTopBar()); bv->addWidget(buildBar2()); setMenuWidget(bars); }
  auto* main = new QHBoxLayout; main->setContentsMargins(0, 0, 0, 0); main->setSpacing(0);

  // the Topology room's mode strip (left of the outliner — only there), the outliner: the scene tree and the Curves column
  tmodes_ = new TopoModes(viewport_); tmodes_->hide(); main->addWidget(tmodes_);
  main->addWidget(outliner_);
  connect(outliner_, &Outliner::widthChanged, this, [this](int w) { if (scene_.room == "paint") return; if (olHead_) olHead_->setFixedWidth(w + (scene_.room == "topo" ? 45 : 0)); });

  // the viewport
  vpBox_ = QWidget::createWindowContainer(viewport_, this);
  vpBox_->setFocusPolicy(Qt::StrongFocus); vpBox_->setMinimumSize(320, 240);
  main->addWidget(vpBox_, 1);

  // the right column: the tools; Active Tool, Settings, Convert to geometry
  dock_ = new ui::DockColumn; dock_->setFixedWidth(280);
  tools_ = new ui::ToolBox; dock_->setTop(tools_);
  dpTool_ = new ui::DockPanel(tr("Active Tool"), dock_, false, false, false);
  dpProps_ = new ui::DockPanel(tr("Settings"), dock_, false, true, true);
  connect(dpProps_, &ui::DockPanel::pinToggled, this, [this](bool on) {
    // (the item shown now: the last selected curve, else the last selected item)
    propsPin_ = -1;
    if (on) { const Selection& s = viewport_->selection();
      for (auto it = s.items.rbegin(); it != s.items.rend(); ++it) if (const Item* x = scene_.item(*it); x && x->type == ItemType::Curve) { propsPin_ = *it; break; }
      if (propsPin_ < 0 && !s.items.empty()) propsPin_ = s.items.back(); }
    syncPanels();
  });
  dpConv_ = new ui::DockPanel(tr("Convert to geometry"), dock_, true, false, false);
  convPanel_ = new ConvertPanel(viewport_, &scene_); dpConv_->setBody(convPanel_);
  // (the Topology room: its transform table right under Settings)
  dpTtab_ = new ui::DockPanel(tr("Transform"), dock_, true, false, false); ttab_ = new TransformTable(viewport_); dpTtab_->setBody(ttab_);
  for (auto* p : {dpTool_, dpProps_, dpTtab_, dpConv_}) dock_->addPanel(p);
  dpTtab_->setWanted(false);
  // (the Paint room: its own left column in the outliner's place — objects, polygroups, visibility sets, the texture, the tools; the right
  // column — the palette, the layers, then the bake / bridge / lighting folded)
  paintLeft_ = new PaintLeft(paint_, viewport_, &scene_); paintLeft_->hide(); main->insertWidget(main->indexOf(outliner_) + 1, paintLeft_);
  paintLeft_->viewportRect = [this] { return QRect(vpBox_->mapToGlobal(QPoint(0, 0)), vpBox_->size()); };
  // (the Paint room's panels: the window's own docks — the user moves them to any edge, tabs them together, floats them; kept per room)
  paintLeft_->setDocked();
  { auto scroll = [](QWidget* w) { auto* sa = new QScrollArea; sa->setWidget(w); sa->setWidgetResizable(true); sa->setFrameShape(QFrame::NoFrame); sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); return sa; };
    auto mk = [&](const char* obj, const QString& title, QWidget* body) { auto* d = new QDockWidget(title, this); d->setObjectName(obj); d->setWidget(body);
      { auto* tb = new QWidget; tb->setObjectName("pdTitle"); tb->setStyleSheet("#pdTitle{background:#2b2b2e;border-bottom:1px solid #3a3a3a} #pdTitle QLabel{color:#d8d8dc} #pdTitle QToolButton{border:none;color:#9a9aa2;padding:0 4px;font-weight:bold} #pdTitle QToolButton:hover{color:#fff;background:#3c3c40;border-radius:3px}");
        auto* th = new QHBoxLayout(tb); th->setContentsMargins(6, 2, 3, 2); th->setSpacing(2); auto* tl = new QLabel(title); tl->setAttribute(Qt::WA_TransparentForMouseEvents); th->addWidget(tl); th->addStretch(1); connect(d, &QDockWidget::windowTitleChanged, tl, &QLabel::setText);
        auto tbtn = [&](const QString& t, const QString& tip) { auto* b = new QToolButton; b->setText(t); b->setToolTip(tip); b->setCursor(Qt::PointingHandCursor); th->addWidget(b); return b; };
        auto* hq = tbtn("?", tr("About this panel")); hq->setVisible(!helpOf(body).isEmpty() || QByteArray(obj) == "pdSettings");
        connect(hq, &QToolButton::clicked, this, [this, hq, body, title] { showPanelHelp(this, hq, title, helpOf(body)); });
        auto* fl = tbtn(QString::fromUtf8("❐"), tr("Float / dock (or drag the title; double click)")); connect(fl, &QToolButton::clicked, d, [d] { d->setFloating(!d->isFloating()); });
        auto* cl = tbtn(QString::fromUtf8("✕"), tr("Close (back through ▦ Panels)")); connect(cl, &QToolButton::clicked, d, [d] { d->close(); });
        d->setTitleBarWidget(tb); }
      d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable); d->setStyleSheet(paintLeft_->sheet()); d->hide();
      connect(d, &QDockWidget::dockLocationChanged, this, [this] { paintDocksSave(); }); connect(d, &QDockWidget::topLevelChanged, this, [this] { paintDocksSave(); });
      connect(d->toggleViewAction(), &QAction::toggled, this, [this] { if (!isMinimized()) paintDocksSave(); }); paintDocks_.push_back(d); };
    mk("pdTools", tr("Tools"), paintLeft_->toolsPart()); mk("pdBrushes", tr("Brushes"), paintLeft_->brushListPart()); mk("pdSettings", tr("Tool settings"), paintLeft_->settingsPart());
    mk("pdManager", tr("Manager"), paintLeft_->managerPart()); mk("pdPalette", tr("Palette"), scroll(makePaintPalettePanel(paint_))); mk("pdLayers", tr("Layers"), scroll(makePaintLayersPanel(paint_, viewport_)));
    mk("pdReference", tr("Reference"), makePaintReferencePanel(paint_));
    { auto* paS = qobject_cast<QScrollArea*>(paintDock("pdPalette")->widget());   // (the palette's panel as tall as what it holds — no empty space left under it, the author, 17.11)
      auto fit = [paS] { QWidget* in = paS->widget(); const int wd = std::max(80, paS->viewport()->width()); const int hh = in->hasHeightForWidth() ? in->heightForWidth(wd) : in->sizeHint().height(); paS->setMaximumHeight(std::max(60, hh + 4)); };
      struct Fit : QObject { std::function<void()> f; using QObject::QObject; bool eventFilter(QObject*, QEvent* e) override { if (e->type() == QEvent::Resize || e->type() == QEvent::LayoutRequest) QTimer::singleShot(0, this, [this] { f(); }); return false; } };
      auto* ft = new Fit(paS); ft->f = fit; paS->installEventFilter(ft); paS->widget()->installEventFilter(ft); fit(); }
    // (Bake and Bridge open from the top bar now; Lighting & Toon is not made for now — it returns with the materials, the author, 17.11)
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);
    // (the program ending: the arrangement kept as it is now — the floating panels' windows close first, and that is not the user hiding them)
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { paintDocksOn_ = false; }); }
  main->addWidget(dock_);
  v->addLayout(main, 1);
  setCentralWidget(app);

  // the FAQ picture in the top right corner, over both bars
  faq_ = new QPushButton(this); faq_->setFixedSize(56, 56); faq_->setIcon(ui::icon("faq")); faq_->setIconSize(QSize(56, 56));
  faq_->setStyleSheet("QPushButton{border:none;background:transparent;padding:0}"); faq_->setCursor(Qt::PointingHandCursor);
  faq_->setToolTip(tr("FAQ: navigation, main hotkeys and help with search — what each part of the program does, how to…"));
  extras::attachFaqButton(faq_);                             // (her hover pictures, petting)
  connect(faq_, &QPushButton::clicked, this, [this] { extras::openFaq(this, scene_.room); });
  // the Maranspector, left of the FAQ: its window over the viewport's top right
  inspBtn_ = makeInspectorButton(this);
  // the rooms and the hotkeys left of the Inspector: pictures as big as its and the FAQ's — the room you are in framed blue
  roomHair_->setIcon(ui::icon("room_hair")); roomTopo_->setIcon(ui::icon("room_topo")); roomPaint_->setIcon(ui::icon("room_paint")); keysBtn_->setIcon(ui::icon("hotkeys")); keysBtn_->setText({});
  for (QPushButton* b : {roomHair_, roomTopo_, roomPaint_, keysBtn_}) { b->setParent(this); b->setFixedSize(56, 56); b->setIconSize(QSize(50, 50)); b->setCursor(Qt::PointingHandCursor); b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet("QPushButton{border:2px solid transparent;border-radius:12px;background:transparent;padding:0} QPushButton:hover{border-color:#5b6b88} QPushButton:checked{border-color:#6f9fe8;background:#25344d}"); b->show(); }
  keysBtn_->setToolTip(tr("Hotkeys: the hotkey manager — every key, your own pie menus; hover a button and press End to give it a key"));
  cornerBg_ = new QWidget(this); cornerBg_->setAttribute(Qt::WA_TransparentForMouseEvents); cornerBg_->setAttribute(Qt::WA_StyledBackground); cornerBg_->show(); cornerBg_->lower();   // (behind the corner's pictures: the bars' lines fade out before them)
  { // (the two rooms one switch: side by side, the outer corners round; the room you are in — blue, the other dark)
    const QString on = "background:qlineargradient(x1:0,y1:1,x2:1,y2:0,stop:0 #2b3a7e,stop:1 #33729a)";
    roomHair_->setStyleSheet("QPushButton{border:none;border-top-left-radius:12px;border-bottom-left-radius:12px;background:#1e1e1e;padding:0} QPushButton:hover:!checked{background:#2c2c2c} QPushButton:checked{" + on + "}");
    roomTopo_->setStyleSheet("QPushButton{border:none;border-radius:0;background:#1e1e1e;padding:0} QPushButton:hover:!checked{background:#2c2c2c} QPushButton:checked{" + on + "}");
    roomPaint_->setStyleSheet("QPushButton{border:none;border-top-right-radius:12px;border-bottom-right-radius:12px;background:#1e1e1e;padding:0} QPushButton:hover:!checked{background:#2c2c2c} QPushButton:checked{" + on + "}");
    for (QPushButton* b : {roomHair_, roomTopo_, roomPaint_}) b->setIconSize(QSize(46, 46)); }
  insp_ = new InspectorWindow(vpBox_, viewport_); insp_->hide();
  hist_ = new HistoryBar(vpBox_, viewport_); hist_->hide();   // (the History slider: the Inspector's ⏱ History)
  insp_->historyShown = [this] { return hist_->isVisible(); };
  insp_->historyStyled = [this] { hist_->applyStyle(); };
  insp_->toggleHistory = [this] { if (hist_->isVisible()) hist_->hide(); else { hist_->sync(); hist_->place(); hist_->show(); hist_->raise(); } };
  ui::stepHook() = [this](const QString& msg) { viewport_->noteStep(msg, true); };   // (a message right after a step names it in the history)
  connect(viewport_, &Viewport::historyOverwritten, this, [this](int n, bool bySlider) {
    ui::toast(tr("%1 later history step%2 overwritten").arg(n).arg(n == 1 ? "" : "s")); if (bySlider) extras::grant("stones"); });   // (the achievement: only a jump back by the History slider)
  connect(inspBtn_, &QPushButton::clicked, this, [this] { if (insp_->isVisible()) { insp_->hide(); return; } insp_->sync(); insp_->place(); insp_->show(); insp_->raise(); });

  report_ = new ReportWindow(vpBox_, viewport_, &scene_); report_->hide();
  connect(report_, &ReportWindow::convertRequested, viewport_, &Viewport::convertSelected);
  connect(viewport_, &Viewport::weldIssues, report_, &ReportWindow::showProblems);

  ui::setOverlayHost(vpBox_, vpBox_); ui::busyPosHook() = [] { return overlays::cfg().busyPos; }; installSliderAssist();   // (every plain slider: double click — its first value, Ctrl + drag — finely)
  makeViewOverlay(vpBox_, viewport_);                         // (each window's label, the view axes in the corner)
  board_ = new RefBoard(vpBox_, &scene_, [this] { viewDirty_ = true; updateTitle(); });   // (the reference board: saved with the project, not an undo step)
  refCanvas_ = new RefCanvas(this, vpBox_, viewport_, &scene_); refCanvas_->hide(); refCanvas_->changed = [this] { if (!viewDirty_) { viewDirty_ = true; updateTitle(); } };
  // (drawing a reference: the outliner of curves and cocoons and the objects' settings are not needed — the canvas takes their room; back as they were on leaving)
#ifdef HM_DEVTOOLS
  if (dev_) refCanvas_->setRecHooks([this] { if (dev_->recording()) dev_->stop(); else dev_->start(); }, [this] { dev_->annotate(); }, [this] { return dev_->recording(); });   // (● Rec / ✎ Note on the canvas's own bar: the main window's are blocked while it is open)
#endif
  refCanvas_->focusLayout = [this, was = std::make_shared<std::array<bool, 4>>()](bool on) {
    QWidget* w[4] = {outliner_, dock_, tmodes_, olHead_};
    for (int i = 0; i < 4; i++) { if (!w[i]) continue; if (on) { (*was)[size_t(i)] = w[i]->isVisible(); w[i]->hide(); } else if ((*was)[size_t(i)]) w[i]->show(); }
  };
  setRefCanvasOpener([this](int id, bool insert) { refCanvas_->openFor(id, insert); }, [this](const QString& view) { refCanvas_->newSheet(view); });
  setRefMenuChanged([this] { if (!viewDirty_) { viewDirty_ = true; updateTitle(); } });   // (🖼 menu: the anchors changed — the project unsaved)
  refCanvas_->save = [this] { if (scene_.path.isEmpty()) ui::toast(tr("The project has no file yet: press Esc and save it")); else runAction("save"); };
  connect(viewport_, &Viewport::statusText, this, [](const QString& s) { if (!s.isEmpty()) ui::toast(s); });
  // (the outliner and the panels made again once, after the events that asked — a step often sends several; a click
  // that left the selection as it was — on the same mesh again — remakes nothing: they show the same)
  auto selSig = [this] { const Selection& s = viewport_->selection(); const auto& t = viewport_->topoSel(); QString k = scene_.room + "|" + viewport_->tool() + "|" + viewport_->topoMode + "|";
    for (int i : s.items) k += QString::number(i) + ","; k += "|"; for (int a : s.anchors) k += QString::number(a) + ",";
    return k + QString("|%1:%2:%3:%4|%5").arg(t.id).arg(t.f.size()).arg(t.e.size()).arg(t.v.size()).arg(scene_.revision); };
  auto refresh = [this, selSig](bool outliner, bool selOnly = false) {
    if (!selOnly) pendAll_ = true;
    pendOutliner_ |= outliner; if (pendRefresh_) return; pendRefresh_ = true;
    QTimer::singleShot(20, this, [this, selSig] { pendRefresh_ = false; const QString sig = selSig(); const bool same = !pendAll_ && sig == lastSelSig_; lastSelSig_ = sig; pendAll_ = false;
      if (same) { pendOutliner_ = false; return; }
      if (pendOutliner_) rebuildOutliner(); pendOutliner_ = false; syncPanels(); });   // (a moment: the step's later signals in the same remaking)
  };
  connect(viewport_, &Viewport::selectionChanged, this, [refresh](const std::vector<int>&) { refresh(true, true); });
  connect(viewport_, &Viewport::sceneEdited, this, [refresh] { refresh(true); });
  connect(viewport_, &Viewport::changed, this, [this, refresh] { updateTitle(); refresh(false); });
  connect(viewport_, &Viewport::pieRequested, this, &MainWindow::openMainPie);
  connect(viewport_, &Viewport::uvConstraintPieRequested, this, [this](const QPoint& g) {       // (Alt+C in the UV Editor: the edges' actions)
    Viewport* vp = viewport_; ui::PieSpec s; s.bare = true;
    auto it = [](const QString& icon, const QString& label, const QString& tip, std::function<void()> act) { ui::PieItem p; p.icon = icon; p.label = label; p.tip = tip; p.act = act; return p; };
    s.ring = {
      it("uv_pin", tr("Pin"), tr("The selected elements pinned where they lie"), [vp] { vp->uvPinKind("pin"); }),
      it("uv_pinoff", tr("Unpin"), tr("The pins of the selection off"), [vp] { vp->uvUnpin(); }),
      it("uv_pinh", tr("Horizontal"), tr("The chain straightened into a horizontal line and kept straight"), [vp] { vp->uvPinStraight(false); }),
      it("uv_unpinh", tr("Free horizontal"), tr("The horizontal constraint off"), [vp] { vp->uvPinKind("unh"); }),
      it("uv_pinv", tr("Vertical"), tr("The chain straightened into a vertical line and kept straight"), [vp] { vp->uvPinStraight(true); }),
      it("uv_unpinv", tr("Free vertical"), tr("The vertical constraint off"), [vp] { vp->uvPinKind("unv"); }),
      it("uv_pineye", tr("Eye / lips"), tr("The eye / lips rim half shut with a small slit"), [vp] { vp->uvFeatureKey(3); }),
      it("uv_unpineye", tr("Free eye / lips"), tr("The eye / lips laid free again"), [vp] { vp->uvFeatureKey(2); }),
      it("uv_pinangled", tr("Line as is"), tr("The selected line held as it lies"), [vp] { vp->uvPinKind("angled"); }),
      it("uv_pinlinesoff", tr("Free all"), tr("Every line constraint off"), [vp] { vp->uvPinKind("unall"); }),
    };
    ui::Pie::open(this, g, s, false); });
  connect(viewport_, &Viewport::uvPieRequested, this, [this](const QPoint& g) {       // (Ctrl+RMB in the UV Editor: what RMB does to the shells)
    Viewport* vp = viewport_; ui::PieSpec s; s.bare = true;
    auto it = [vp](const QString& key, const QString& icon, const QString& label, const QString& tip) { ui::PieItem p; p.icon = icon; p.label = label; p.tip = tip; p.cur = vp->uvRmb == key;
      p.act = [vp, key, label] { vp->uvRmb = key; ui::hud(QObject::tr("RMB in the UV Editor: %1").arg(label), 1500); }; return p; };
    s.ring = {
      it("rotate", "tool_rotate", tr("Free turn"), tr("RMB drag: the selected shells (none — the one under the pointer) turned about their middle")),
      it("rot90", "tool_rotate", tr("Turn 90°"), tr("RMB click: turned 90° (Shift — the other way); a drag — in 90° steps the way the pointer goes round")),
      it("scale", "tool_scale", tr("Scale"), tr("RMB drag from the shell: scaled evenly about its middle")),
      it("flip", "top_mirror", tr("Flip"), tr("RMB swipe across the shell: flipped in U; up or down — flipped in V")),
    };
    ui::Pie::open(this, g, s, false); });
  connect(viewport_, &Viewport::opsPieRequested, this, [this](const QPoint& g) { openTopoOpsPie(g); });
  ui::Pie::faqHook = [this](const QString& what) { extras::openFaqAt(this, scene_.room, what); };   // (~ over a pie item)
  connect(viewport_, &Viewport::brushChanged, this, &MainWindow::syncPanels);
  connect(viewport_, &Viewport::toolChanged, this, [this](const QString& t) { tools_->setCurrent(t); syncPanels(); });
  connect(tools_, &ui::ToolBox::toolPicked, this, &MainWindow::pickTool);
  viewport_->hairSnap = QSettings().value("hair/snap", false).toBool(); tools_->setSnap(viewport_->hairSnap);
  connect(tools_, &ui::ToolBox::snapClicked, this, [this] { viewport_->hairSnap = !viewport_->hairSnap; QSettings().setValue("hair/snap", viewport_->hairSnap); tools_->setSnap(viewport_->hairSnap);
    bool any = false; for (auto& [id, it] : scene_.items) if (it.type == ItemType::IMesh && it.raw.value("tref").toBool()) any = true;
    ui::toast(!viewport_->hairSnap ? tr("Snap off") : any ? tr("Snap on — new and dragged anchors stick to the reference meshes") : tr("Snap on — mark a mesh as the reference first (its settings → Snapping → Reference)")); });
  tools_->setSym(viewport_->tset.sym);   // (Symmetry: shared with the Topology room — Shift+S, its settings by the button)
  connect(tools_, &ui::ToolBox::symClicked, this, [this] { viewport_->setSymmetry(!viewport_->tset.sym); });
  connect(tools_, &ui::ToolBox::symSettings, this, [this](QWidget* a) { openSymPanel(viewport_, a, false); });
  connect(viewport_, &Viewport::topoState, this, [this] { tools_->setSym(viewport_->tset.sym); });
  connect(tools_, &ui::ToolBox::cocoonClicked, this, [this] {
    CocoonSystem* cs = viewport_->hair().cocoonSystem();
    viewport_->runEdit([cs](Scene& sc, Selection& sel, const CurveCache& cc) { QString m; int k = edit::makeLiveCocoon(sc, cc, cs, sel, &m); return k < 0 ? "!" + m : m; });
  });
  connect(tools_, &ui::ToolBox::about, this, [] { QDesktopServices::openUrl(QUrl("https://catarackta.netlify.app")); });
  connect(tools_, &ui::ToolBox::support, this, [] { QDesktopServices::openUrl(QUrl("https://boosty.to/catarackta/donate")); });
  connect(tools_, &ui::ToolBox::selCopy, this, [this] { runAction("copySel"); });
  ttools_ = new TopoTools(this, viewport_, [this](const QString& t) { pickTool(t); }); ttools_->hide();
  htools_ = new ui::HairToolWin(this, tools_->rows()); htools_->hide();
  // (a button of the Topology tool options: kept with its tool — a pie or a key presses it with that tool's options made for it)
  ui::Keymap::get().toolOf = [this](const QWidget* w) { return ttools_ && ttools_->isAncestorOf(w) && scene_.room == "topo" ? viewport_->tool() : QString(); };
  uvtools_ = new UvTools(this, viewport_); uvtools_->hide();
  new ZBrushLink(this, viewport_, &scene_);   // (the bridge from ZBrush: SubTools sent from it come into the open scene — Bridge ▾ → ZBrush)
  connect(viewport_, &Viewport::topoState, this, [this] { syncUvTools(); });
  { ViewFrame::Hooks fh;                                      // (the viewport's frame: the wire, the grids, the axes, the room's display)
    fh.view = [this] { return scene_.fileRaw.value("settings").toObject().value("view").toObject(); };
    fh.setView = [this](const QString& k, const QJsonValue& v) { setViewSetting(k, v); };
    auto popAt = [](QFrame* f, const QPoint& br) { f->adjustSize(); f->move(std::max(0, br.x() - f->width() + 1), br.y() + 6); f->show(); };
    fh.openHairDisplay = [this, popAt](const QPoint& br) { if (dispStrip_->isVisible()) { dispStrip_->hide(); return; } if (syncDisplay_) syncDisplay_(); popAt(dispStrip_, br); };
    fh.strip = [this] { return static_cast<QWidget*>(dispStrip_); }; fh.syncStrip = [this] { if (syncDisplay_) syncDisplay_(); };
    fh.openTopoDisplay = [this, popAt](const QPoint& br) { if (dispTopo_->isVisible()) { dispTopo_->hide(); return; } syncTopoDisplay(dispTopo_); popAt(dispTopo_, br); };
    new ViewFrame(vpBox_, viewport_, fh); }
  connect(viewport_, &Viewport::roomChanged, this, &MainWindow::applyRoom);
  // (Topology: the mode / its selection / the picked polygroups — the panels a moment later, once for many)
  connect(viewport_, &Viewport::topoState, this, [refresh] { refresh(false); });   // (one remaking with the edits' own — not a second after it)
  syncPanels();
}

QWidget* MainWindow::buildTopBar() {
  auto* bar = new QWidget; bar->setObjectName("topbar"); bar->setFixedHeight(30);
  bar->setStyleSheet("#topbar{background:#262626;border-bottom:1px solid #3a3a3a} #topbar QPushButton{padding:3px 8px}");
  auto* h = new QHBoxLayout(bar); h->setContentsMargins(8, 0, 130, 0); h->setSpacing(6);   // (room for the Maranspector and the FAQ)
  auto* file = btn("📁 File", tr("File: recent projects, autosaves, migration and presets (Ctrl+O). Files can also be dropped onto the window"));
  auto* save = btn("💾 Save", tr("Save project (Ctrl+S; Ctrl+Shift+S — save as)"));
  auto* as = btn("⏱ Autosave", tr("Autosave settings: interval, number of copies, folder"));
  auto* rl = btn("⟳ Reload", tr("Reload: restart the program and open the same project again (unsaved changes are kept)"));
  auto* ex = btn(tr("Export"), tr("Export: the meshes (the selected ones, or all) as OBJ, FBX, glTF, PLY or STL — the path, format, scale and the normals type in the export window")); ex->setIcon(ui::icon("export")); ex->setIconSize(QSize(18, 18));
  auto* br = btn("🔗 Bridge ▾", tr("Bridge: send the meshes straight into another program — Maya or Blender (live, over a local port) or a folder another program watches (Unreal, Unity, Godot, ZBrush…)"));
  auto* sd = btn("⇪ Send", tr("Send the meshes over the bridge set up in 🔗 Bridge"));
  dispBtn_ = btn("⚙ Interface ▾", tr("Interface: the size of the buttons, text and panels (the display settings moved to the viewport's frame — move the pointer to its top edge)"));
#ifdef HM_DEVTOOLS
  // (the session's record — what is done, into a file to analyse; while it runs, annotations — the
  // camera, the project's state, a note and a drawing over the window's picture)
  { auto grabWin = [this] { return windowShot(true); };
    dev_ = new DevSession(this, viewport_, &scene_, grabWin, [this] { return displayName(); });
    recBtn_ = btn("● Rec", tr("Record this session — the tools and actions used, the window's stalls, the messages — into a folder in Documents\\HairHarpy Sessions. Nothing is sent anywhere: send the folder to the author yourself if you want to help. Again — stop"));
    noteBtn_ = btn("✎ Note", tr("F9: a note in the session's record — the camera, the project's state, your text and a drawing over a picture of the program's window"));
    recBtn_->setCheckable(true); noteBtn_->setVisible(false); recBtn_->setStyleSheet("color:#ff8080;font-weight:600");
    connect(recBtn_, &QPushButton::clicked, this, [this](bool on) { if (on) dev_->start(); else dev_->stop(); });
    connect(noteBtn_, &QPushButton::clicked, this, [this] { dev_->annotate(); });
    // (the room's own values for the record: a line whenever one of them changes)
    dev_->stateHook = [this] { QJsonObject s{{"view", viewport_->camera().view}, {"selected", int(viewport_->selection().items.size())}, {"changes", double(changeTotal())}, {"sym", viewport_->tset.sym}};
      if (scene_.room == "paint" && paint_) { const paint::Layer* l = paint_->active();
        s.insert("ptool", paint_->tool); s.insert("drawMode", paint_->drawMode); s.insert("layer", l ? l->name : QString()); s.insert("layers", int(paint_->doc.layers().size())); s.insert("uvset", paint_->doc.activeSet());
        s.insert("map", paint_->view); s.insert("disp", viewport_->paintDisp); s.insert("size", std::round(paint_->brush.size)); s.insert("opacity", std::round(paint_->brush.opacity * 100)); s.insert("flow", std::round(paint_->brush.flow * 100));
        s.insert("hardness", std::round(paint_->brush.hardness * 100)); s.insert("color", paint_->color.name()); s.insert("freeze", paint_->freezePaint); s.insert("blend", l ? paint::blendKey(l->blend) : QString());
        s.insert("projection", paintProj_ && paintProj_->on); s.insert("tex", QString("%1x%2").arg(paint_->doc.width()).arg(paint_->doc.height())); }
      if (scene_.room == "topo") { s.insert("uv", viewport_->tset.uv); }
      return s; };
    // (every action of the program with its key: what was never used in a session shows against this list)
    dev_->catalogHook = [] { QJsonArray acts; for (const ui::ActionDef& d : ui::Keymap::get().defs()) acts.append(QJsonObject{{"id", d.id}, {"label", d.label}, {"key", ui::Keymap::get().key(d.id)}, {"cat", d.cat}}); return QJsonObject{{"actions", acts}}; };
    if (dev_->autoStart() && QApplication::applicationName() == "HairHarpy") QTimer::singleShot(1200, this, [this] { if (!dev_->recording()) dev_->start(); });   // (the recorder's settings file: every run recorded)
    dev_->changed = [this] { recBtn_->setChecked(dev_->recording()); recBtn_->setText(dev_->recording() ? "■ Stop" : "● Rec"); recBtn_->setStyleSheet(dev_->recording() ? "color:#fff;background:#b02a2a;font-weight:600" : "color:#ff8080;font-weight:600"); noteBtn_->setVisible(dev_->recording());  if (refCanvas_) refCanvas_->syncRec(); };
  }
#endif
  panelsBtn_ = btn("▦ Panels ▾", tr("The Paint room's panels: show or hide each; drag a panel by its title to any edge, onto another (tabs) or out (its own window); Reset — as they were at first"));
  { auto* m = new QMenu(panelsBtn_); m->setToolTipsVisible(true);
    connect(m, &QMenu::aboutToShow, this, [this, m] { m->clear(); for (QDockWidget* d : paintDocks_) m->addAction(d->toggleViewAction()); m->addSeparator();
      QAction* r = m->addAction(tr("Reset the panels' layout")); connect(r, &QAction::triggered, this, [this] { paintDocksDefault(); paintDocksSave(); ui::toast(tr("The panels as they were at first")); }); });
    connect(panelsBtn_, &QPushButton::clicked, this, [this, m] { m->popup(panelsBtn_->mapToGlobal(QPoint(0, panelsBtn_->height() + 2))); }); panelsBtn_->hide(); }
  for (QWidget* w : std::initializer_list<QWidget*>{file, save, as, rl, vsep(), ex, br, sd, vsep(), dispBtn_, panelsBtn_}) h->addWidget(w);
#ifdef HM_DEVTOOLS
  h->addWidget(vsep()); h->addWidget(recBtn_); h->addWidget(noteBtn_);   // (development: after Display)
#endif
  // (PDO and the display settings: in the viewport's frame)
  auto* gh = btn("Ghost", tr("The ghost's wire shown / hidden (hidden — nothing snaps to it either)")); gh->setCheckable(true); h->addWidget(gh);
  auto syncGh = [this, gh] { gh->setVisible(viewport_->room() == "topo" && viewport_->ghostAny()); QSignalBlocker b(gh); gh->setChecked(viewport_->gho.show); };
  connect(gh, &QPushButton::clicked, this, [this, syncGh] { viewport_->gho.show = !viewport_->gho.show; viewport_->saveGhostPrefs(); viewport_->ghostChanged(); emit viewport_->topoState(); syncGh(); });
  connect(viewport_, &Viewport::topoState, this, syncGh); connect(viewport_, &Viewport::changed, this, syncGh); connect(viewport_, &Viewport::roomChanged, this, syncGh); connect(viewport_, &Viewport::sceneEdited, this, syncGh); syncGh();
  // (the Topology room: the polygons hidden with H shown again — there while some are hidden)
  unhideBtn_ = btn(tr("👁 Unhide all"), tr("Show every polygon hidden with H (and the hidden polygroups)")); unhideBtn_->hide(); h->addWidget(unhideBtn_);
  connect(unhideBtn_, &QPushButton::clicked, this, [this] { viewport_->tUnhideAll(); syncUnhide(); });
  connect(viewport_, &Viewport::changed, this, [this] { syncUnhide(); });
  connect(viewport_, &Viewport::topoState, this, [this] { syncUnhide(); });
  paintTop_ = makePaintTopBar(paint_, viewport_, [this] { openMaterialEditor(this, paint_, [this] { emit paint_->settingsChanged(); }, [this] { viewport_->setPaintDisp("material"); }); }); paintTop_->hide(); h->addWidget(vsep()); h->addWidget(paintTop_);   // (the Paint room: Symmetry, the display mode, the map painted)
  bakeBtn_ = btn(tr("Bake ▾"), tr("Bake: the high-poly's detail into the low-poly's maps (normal, AO, curvature…), all or by the Bake mask")); bakeBtn_->hide(); h->addWidget(bakeBtn_);
  connect(bakeBtn_, &QPushButton::clicked, this, [this] { paintPop(bakePop_, bakeBtn_, [this] { QWidget* w = makePaintBakePanel(paint_, viewport_, &scene_); return w; }); });
  h->addStretch(1);
  connect(file, &QPushButton::clicked, this, [this] { runAction("open"); });
  connect(save, &QPushButton::clicked, this, [this] { saveProject(false); });
  connect(as, &QPushButton::clicked, this, [this] { files::openAutosaveDialog(this, files::baseName(displayName()), asMsg_, [this] { autosaveNow(true); }); });
  connect(rl, &QPushButton::clicked, this, [this] { reloadApp(); });
  expBtn_ = ex; brgBtn_ = br; sendBtn_ = sd;
  connect(ex, &QPushButton::clicked, this, [this, ex] {
    if (viewport_->room() != "paint") { openExportPanel(ex, viewport_, &scene_); return; }
    QMenu m(this); m.addAction(tr("Meshes…"), this, [this, ex] { openExportPanel(ex, viewport_, &scene_); });   // (the Paint room: the texture maps too — moved here from the Layers panel)
    m.addAction(tr("Texture maps…"), this, [this] { exportPaintMaps(paint_, this); }); m.exec(ex->mapToGlobal(QPoint(0, ex->height()))); });
  connect(br, &QPushButton::clicked, this, [this, br] {
    if (viewport_->room() != "paint") { openBridgePanel(br, viewport_, &scene_, [this] { updateSendBtn(); }); return; }
    paintPop(bridgePop_, br, [this, br] { auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(6);   // (the Paint room: the painting editors' bridge, and the meshes' one below)
      QWidget* pb = makePaintBridgePanel(paint_, viewport_, &scene_); v->addWidget(pb);
      auto* mb = new QPushButton(tr("Meshes: Maya / Blender / a folder…")); mb->setToolTip(tr("The meshes' bridge — as in the other rooms")); v->addWidget(mb);
      connect(mb, &QPushButton::clicked, this, [this, br] { if (bridgePop_) bridgePop_->hide(); openBridgePanel(br, viewport_, &scene_, [this] { updateSendBtn(); }); });
      return w; }); });
  connect(sd, &QPushButton::clicked, this, [this] { runAction("bridgeSend"); });
  updateSendBtn();
  dispStrip_ = qobject_cast<QFrame*>(buildDisplayStrip());
  dispTopo_ = makeTopoDisplay(this, viewport_, &scene_, [this] { return scene_.fileRaw.value("settings").toObject().value("view").toObject(); }, [this](const QString& k, const QJsonValue& v) { setViewSetting(k, v); });
  // (the interface size: its own popup under the button, in place of Display)
  { auto* pop = new QFrame(this, Qt::Popup); pop->setObjectName("ifBar"); pop->setStyleSheet("#ifBar{background:#262626;border:1px solid #555;border-radius:6px}");
    auto* ph = new QHBoxLayout(pop); ph->setContentsMargins(10, 6, 10, 6); ph->addWidget(uiScaleControl()); pop->hide();
    connect(dispBtn_, &QPushButton::clicked, this, [this, pop] { if (pop->isVisible()) { pop->hide(); return; } pop->adjustSize(); pop->move(dispBtn_->mapToGlobal(QPoint(0, dispBtn_->height() + 4))); pop->show(); }); }
  return bar;
}

// 👁 Display: X-ray, show / hide and the opacity of anchors, curves, guides, the walls' edges and loops, the NURBS
// surfaces, the modifiers, the meshes' wire, one colour — the project's (settings.view)
QWidget* MainWindow::buildDisplayStrip() {
  auto* f = new QFrame(this, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint); f->setObjectName("dispBar");   // (a panel of the viewport's frame, shown under its buttons by ViewFrame)
  f->setStyleSheet("#dispBar{background:#262626;border:1px solid #555;border-radius:6px}");
  auto* h = new QVBoxLayout(f); h->setContentsMargins(10, 8, 10, 8); h->setSpacing(5);
  auto view = [this] { return scene_.fileRaw.value("settings").toObject().value("view").toObject(); };
  auto slider = [&](const QString& key, double lo, double hi, double step, double def, int w, const QString& tip) {
    auto* s = new QSlider(Qt::Horizontal); s->setRange(int(std::lround(lo / step)), int(std::lround(hi / step))); s->setFixedWidth(w); s->setToolTip(tip);
    s->setProperty("key", key); s->setProperty("step", step); s->setProperty("def", def);
    connect(s, &QSlider::valueChanged, this, [this, s, key, step] { setViewSetting(key, s->value() * step); });
    return s;
  };
  auto* xl = new QLabel(tr("X-ray")); xl->setProperty("mut", true);
  xl->setToolTip(tr("How much curves, anchors and guides show through the head and the hair (100 — fully visible through everything); also how see-through the NURBS surfaces are"));
  auto* xs = slider("xray", 0, 1, 0.01, 0.2, 90, xl->toolTip()); auto* xv = new QLabel("20"); xv->setFixedWidth(26); xv->setProperty("mut", true);
  connect(xs, &QSlider::valueChanged, xv, [xv](int x) { xv->setText(QString::number(x)); });
  { auto* xr = new QHBoxLayout; xr->setSpacing(6); xr->addWidget(xl); xr->addWidget(xs); xr->addWidget(xv); h->addLayout(xr); }
  struct T { const char *key, *label, *tip, *op; double def; };
  const T ts[] = {
    {"showAnchors", "● Anchors", "Show / hide anchors", "anchorOpacity", 1},
    {"showCurves", "∿ Curves", "Show / hide curves", "curveOpacity", 1},
    {"showGuides", "┼ Guides", "Show / hide guide lines", "guideOpacity", 1},
    {"showGrid", "║ Edges", "Show / hide the lines along: the NURBS surfaces' width segments (between their two curves) and their curves' places at the edges, and the crowns' edges from the lower connector up to the parting", nullptr, 1},
    {"showLoops", "☰ Loops", "Show / hide the loops across: the cocoons' loops on their locks and the crowns' loops (ring by ring, and the parting line). The slider — the lines' opacity (Edges too)", "gridOpacity", 1},
    {"showSurfaces", "◧ NURBS", "Show / hide the NURBS surfaces (walls, crown, caps, fill-hole previews). The slider — their opacity: 100 — solid", "surfOpacity", 0.85},
    {"showMods", "⬡ Modifiers", "Show / hide the profile modifiers' tubes. The slider — their opacity (100 — solid, nothing shows through). X-ray still makes them see-through", "modOpacity", 0.45},
  };
  for (const T& t : ts) {
    auto* b = btn(t.label, tr(t.tip)); b->setCheckable(true); dispToggles_[t.key] = b;
    QString key = t.key; connect(b, &QPushButton::toggled, this, [this, key](bool on) { setViewSetting(key, on); });
    auto* g = new QHBoxLayout; g->setSpacing(4); g->addWidget(b);
    if (t.op) g->addWidget(slider(t.op, 0.05, 1, 0.05, t.def, 64, tr("Opacity")));
    h->addLayout(g);
  }
  auto* one = btn("◐ One colour", tr("One colour: the walls, crowns, caps, meshes and imported meshes all in the colour on the right — the groups' colours are ignored (the head stays as it is). Off — the colour is for the imported meshes and for whatever is outside the coloured groups"));
  one->setCheckable(true); dispToggles_["oneColor"] = one;
  connect(one, &QPushButton::toggled, this, [this](bool on) { setViewSetting("oneColor", on); });
  auto* col = btn(""); col->setFixedSize(26, 20); col->setToolTip(tr("The model colour (off: the imported meshes and whatever is outside the coloured groups)"));
  auto paintCol = [this, col] { QString c = scene_.fileRaw.value("settings").toObject().value("view").toObject().value("modelColor").toString("#808080"); col->setStyleSheet(QString("background:%1;border:1px solid #555;border-radius:3px").arg(c)); };
  connect(col, &QPushButton::clicked, this, [this, paintCol] {
    QColor c = QColorDialog::getColor(QColor(scene_.fileRaw.value("settings").toObject().value("view").toObject().value("modelColor").toString("#808080")), this, tr("Model colour"));
    if (c.isValid()) { setViewSetting("modelColor", c.name()); paintCol(); }
  });
  auto* rs = btn("↺", tr("Reset the model colour to the default grey"));
  connect(rs, &QPushButton::clicked, this, [this, paintCol] { setViewSetting("modelColor", "#808080"); paintCol(); });
  { auto* cr = new QHBoxLayout; cr->setSpacing(4); cr->addWidget(one); cr->addWidget(col); cr->addWidget(rs); cr->addStretch(1); h->addSpacing(4); h->addLayout(cr); }
  // (the strip shows the project's values: on opening a project, when it opens)
  syncDisplay_ = [this, f, view, paintCol, xv] {
    QJsonObject vw = view();
    for (auto* s : f->findChildren<QSlider*>()) {
      QSignalBlocker b(s); double st = s->property("step").toDouble(), def = s->property("def").toDouble();
      QString k = s->property("key").toString();
      s->setValue(int(std::lround((vw.contains(k) ? vw.value(k).toDouble(def) : def) / st)));
    }
    for (auto it = dispToggles_.begin(); it != dispToggles_.end(); ++it) { QSignalBlocker b(it.value()); it.value()->setChecked(vw.value(it.key()).toBool(it.key() != "oneColor" && it.key() != "showGrid" && it.key() != "showLoops")); }   // (the walls' lines: off by default)
    xv->setText(QString::number(int(std::lround(vw.value("xray").toDouble(0.2) * 100))));
    paintCol();
  };
  f->hide();
  return f;
}

QWidget* MainWindow::buildBar2() {
  auto* bar = new QWidget; bar->setObjectName("bar2"); bar->setFixedHeight(30);
  bar->setStyleSheet("#bar2{background:#262626;border-bottom:1px solid #3a3a3a} #bar2 QPushButton{padding:2px 8px}");
  auto* h = new QHBoxLayout(bar); h->setContentsMargins(0, 0, 130, 0);   /* (room for the Maranspector and the FAQ) */ h->setSpacing(0);
  // the outliner's head
  auto* ol = new QWidget; ol->setFixedWidth(outliner_->paneWidth()); ol->setObjectName("olHead"); ol->setStyleSheet("#olHead{border-right:1px solid #3a3a3a}"); olHead_ = ol;
  auto* oh = new QHBoxLayout(ol); oh->setContentsMargins(6, 0, 6, 0); oh->setSpacing(4);
  auto* imp = btn("+", tr("Add: import the reference head or a mesh (OBJ / GLB / GLTF / FBX), or make a primitive — a cube, a sphere, a cylinder… (on the window's top)")); imp->setStyleSheet("font-size:16px;font-weight:bold;padding:0 9px");
  auto* wire = btn("W", tr("Wireframe: the head and the imported meshes in the Hair room; in the Topology room — every mesh that isn't a reference")); wire->setCheckable(true);
  auto* sr = btn("Aa", tr("Smart Rename (Shift+F2): tidy names and numbers, prefixes, find & replace, sort by connections")); sr->setStyleSheet("font-size:11px");
  auto* lab = new QLabel(tr("Outliner")); lab->setProperty("mut", true);
  auto* ccb = btn("◍", tr("Fold / unfold the Cocoons section of the outliner")); ccb->setCheckable(true); ccb->setChecked(!outliner_->cocoonsFolded());
  connect(ccb, &QPushButton::clicked, this, [this, ccb] { outliner_->toggleCocoons(); ccb->setChecked(!outliner_->cocoonsFolded()); });
  wire->setParent(ol); wire->hide();   // (W: gone from every room — the wire is in the viewport's frame; kept hidden for the checks' "wirebtn")
  oh->addWidget(imp); oh->addWidget(sr); oh->addWidget(lab); oh->addStretch(1); oh->addWidget(ccb);
  srBtn_ = sr; ccBtn_ = ccb; wireBtn_ = wire;
  connect(imp, &QPushButton::clicked, this, [this] { openImportWindow(viewport_, &scene_); });   // (the primitives: on its top)
  connect(wire, &QPushButton::toggled, this, [this](bool on) { viewport_->showWire = on; viewport_->sceneChanged(false); });
  connect(sr, &QPushButton::clicked, this, [this] { runAction("smartRename"); });
  h->addWidget(ol);
  // the viewport bar
  auto* vp = new QWidget; auto* vh = new QHBoxLayout(vp); vh->setContentsMargins(10, 0, 10, 0); vh->setSpacing(8);
  auto* gl = new QLabel(tr("Gizmo")); gl->setProperty("mut", true); gl->setObjectName("gzLabel"); vh->addWidget(gl);
  gzSpace_ = btn("🌐 World", tr("Gizmo axes: World / Local (Shift+W)")); vh->addWidget(gzSpace_);
  connect(gzSpace_, &QPushButton::clicked, this, [this] { runAction("gizmoSpace"); });
  { auto* gc = btn("⊙", tr("Local axes: the gizmo (pivot) of the selected meshes to the middle of their geometry")); gc->setObjectName("gzCentre"); gc->setVisible(false); vh->addWidget(gc);
    connect(gc, &QPushButton::clicked, this, [this] { int n = 0; for (int id : viewport_->selection().items) { const Item* it = scene_.item(id); const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr;
        if (it && it->type == ItemType::IMesh && B) { viewport_->setMeshPivot(id, B->bounds().center()); n++; } }
      ui::toast(n ? tr("Gizmo to the middle of the geometry") : tr("Select a mesh first")); }); }
  const char* axc[3] = {"#ff7070", "#7fe07f", "#7fa8ff"}; const char* axn[3] = {"X", "Y", "Z"};
  auto* axes = new QHBoxLayout; axes->setSpacing(0);
  for (int i = 0; i < 3; i++) {
    gzAx_[i] = btn(axn[i], tr("%1 axis on / locked (Shift+%1)").arg(axn[i])); gzAx_[i]->setCheckable(true); gzAx_[i]->setChecked(true);
    gzAx_[i]->setStyleSheet(QString("QPushButton{color:%1;padding:2px 8px}").arg(axc[i])); axes->addWidget(gzAx_[i]);
    connect(gzAx_[i], &QPushButton::toggled, this, [this, i](bool on) {
      viewport_->setAxisLock(i, !on); ui::toast(tr("Axis %1 %2").arg("XYZ"[i]).arg(on ? tr("unlocked") : tr("locked")));
      if (gzAx_[0]->isChecked() && !gzAx_[1]->isChecked() && !gzAx_[2]->isChecked()) extras::grant("twitter");   // (only X left)
    });
  }
  vh->addLayout(axes); vh->addWidget(vsep());
  auto* cam = btn("🎥 Camera ▾", tr("Camera: reset, perspective / orthographic, views, split screen"));
  connect(cam, &QPushButton::clicked, this, [this, cam] { if (cameraMenuOpen()) closeCameraMenu(); else openCameraMenu(this, viewport_, cam->mapToGlobal(QPoint(0, cam->height() + 2))); });
  auto* ref = btn("🖼 Reference ▾", tr("References: a reference board window (like PureRef) and reference planes with images in the views"));
  connect(ref, &QPushButton::clicked, this, [this, ref] { if (refMenuOpen()) closeRefMenu(); else openRefMenu(this, viewport_, &scene_, board_, ref->mapToGlobal(QPoint(0, ref->height() + 2))); });
  soloBtn_ = btn("◎ Solo", tr("Solo: only the group you work in is shown (like Ctrl+Shift+H) — select something in another group and it switches to that one. Ctrl+H / Shift+H / Ctrl+Shift+H switch it off. The Topology room: only the selected object — RMB: its folder, or the object with its high-poly"));
  soloBtn_->setCheckable(true); soloBtn_->setChecked(viewport_->solo());
  connect(soloBtn_, &QPushButton::clicked, this, [this](bool on) { viewport_->setSolo(on); });
  // (the Topology room, RMB: what Solo shows — the selected object, its folder, the object with its high-poly)
  soloBtn_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(soloBtn_, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
    if (viewport_->room() != "topo") return;
    QMenu menu(this); auto* grp = new QActionGroup(&menu);
    for (auto [k, t] : {std::pair<const char*, QString>{"mesh", tr("Only the selected object")}, {"folder", tr("The selected object's folder")}, {"high", tr("The object and its high-poly")}}) {
      QAction* a = menu.addAction(t); a->setCheckable(true); a->setChecked(viewport_->soloTopo == k); grp->addAction(a);
      const QString key = k; connect(a, &QAction::triggered, this, [this, key] { viewport_->setSoloTopo(key); }); }
    menu.exec(soloBtn_->mapToGlobal(at)); });
  connect(viewport_, &Viewport::soloChanged, soloBtn_, [this](bool on) { QSignalBlocker b(soloBtn_); soloBtn_->setChecked(on); });
  lockSelBtn_ = btn("⧉ Lock select", tr("Lock selection mode: a click on a curve, a wall or a profile modifier selects every curve of its lock (the curves joined by walls). Shift — add / remove the whole lock"));
  lockSelBtn_->setCheckable(true); lockSelBtn_->setChecked(QSettings().value("lockSel").toBool());
  viewport_->lockSel = lockSelBtn_->isChecked();
  connect(lockSelBtn_, &QPushButton::toggled, this, [this](bool on) { viewport_->lockSel = on; QSettings().setValue("lockSel", on); ui::toast(on ? QObject::tr("Lock select: a click selects every curve of the lock") : QObject::tr("Lock select off")); });
  vh->addWidget(cam); vh->addWidget(ref); vh->addWidget(soloBtn_); vh->addWidget(lockSelBtn_); vh->addWidget(vsep());
  // (Floor, Axes: in the viewport's frame — ViewFrame)
  paintMode_ = makePaintModeBar(paint_, viewport_, [this](bool on) { if (paintProj_) paintProj_->on = on; }); paintMode_->hide(); vh->addWidget(paintMode_);   // (the Paint room: the brush's quick settings, the map's own)
  vh->addStretch(1);
  auto* rh = btn("", tr("Hair room: curves, NURBS walls, cocoons, conversion into hair meshes")); rh->setCheckable(true); rh->setChecked(true);
  auto* rt = btn("", tr("Topology room: retopology — its own outliner, selection modes (object / polygon / edge / vertex), Cursor and gizmo")); rt->setCheckable(true);

  auto* rp = btn("", tr("Paint room: textures — layers, masks, fill and adjustment layers, brushes (ABR), bump / flow painting, baking a high-poly, the bridge to Photoshop and 3D-Coat, lighting and the toon shader")); rp->setCheckable(true);
  roomHair_ = rh; roomTopo_ = rt;
  roomPaint_ = rp;
  for (QPushButton* b : {rh, rt, rp}) b->setToolTip(b->toolTip() + tr("\nRooms: each one a scene of its own — its outliner, tools, undo history; the head and the camera are shared"));
  connect(rh, &QPushButton::clicked, this, [this] { goRoom("hair"); });
  connect(rt, &QPushButton::clicked, this, [this] { goRoom("topo"); });
  connect(rp, &QPushButton::clicked, this, [this] { goRoom("paint"); });
  // (the rooms and the hotkeys: big pictures by the FAQ — placed over the bars in the constructor, see resizeEvent)
  auto* keys = btn("⌨ Hotkeys", tr("Hotkey manager"));
  connect(keys, &QPushButton::clicked, this, [this] {   // (not modal: the window's buttons and tools can be dragged onto it)
    static QPointer<ui::HotkeyDialog> dlg; if (dlg) { dlg->raise(); dlg->activateWindow(); return; }
    dlg = new ui::HotkeyDialog(this); dlg->setAttribute(Qt::WA_DeleteOnClose); connect(dlg, &ui::HotkeyDialog::message, [](const QString& s) { ui::toast(s); }); dlg->show(); });
  keysBtn_ = keys;
  h->addWidget(vp, 1);
  return bar;
}

// ======================= hotkeys, tools, pies =======================
void MainWindow::buildActions() {
  auto& A = actions_;
  A["cursor"] = [this] {
    if (scene_.room == "topo") {                             // (Q twice quickly: the snap mask brush ↔ the Cursor)
      qint64 now = QDateTime::currentMSecsSinceEpoch();
      if (now - lastQ_ < 400) { lastQ_ = 0; QString to = qFrom_ == "tmask" ? "cursor" : "tmask"; viewport_->setTool(to); ui::toast(to == "tmask" ? tr("Snap mask brush") : tr("Cursor")); return; }
      lastQ_ = now; qFrom_ = viewport_->tool();
    }
    if (viewport_->tool() != "cursor") { prevTool_ = viewport_->tool(); viewport_->setTool("cursor"); } else viewport_->setTool(prevTool_.isEmpty() || (scene_.room == "topo" && prevTool_ == "tmask") ? "gizmo" : prevTool_); };
  for (const char* t : {"move", "rotate", "gizmo", "draw", "cut", "brush", "guide", "modifier", "crown", "braid", "lockex", "lockfix", "edge"}) { QString id = t; A[id] = [this, id] { pickTool(id); }; }
  A["gizmoSpace"] = [this] {
    bool local = !viewport_->gizmoLocal(); viewport_->setGizmoLocal(local);
    if (auto* gc = findChild<QPushButton*>("gzCentre")) gc->setVisible(local);
    gzSpace_->setText(local ? "⌖ Local" : "🌐 World"); ui::toast(local ? tr("Gizmo: Local axes") : tr("Gizmo: World axes"));
  };
  for (int i = 0; i < 3; i++) { QString id = QString("axis") + "XYZ"[i]; A[id] = [this, i] { gzAx_[i]->toggle(); }; }
  A["toolPie"] = [this] { openToolPie(); };
  A["mainPie"] = [this] { openMainPie(QCursor::pos()); };
  A["isolatePie"] = [this] { openIsolatePie(); };
  A["topoOpsPie"] = [this] { if (scene_.room == "topo") openTopoOpsPie(QCursor::pos(), false); };
  // Tab: split screen on + the camera menu at the pointer; a double press shows the ACTIVE window (the one under the
  // pointer) alone, with its camera
  A["camMenu"] = [this] {
    if (viewWindowUnderPointer()) { toggleViewWindowMenu(QCursor::pos() - QPoint(30, 20)); return; }   // (Tab over the view window: its own menu)
    static QElapsedTimer last; static int from = -1;
    if (last.isValid() && last.elapsed() < 350) { last.invalidate(); closeCameraMenu();
      int at = from >= 0 ? from : viewport_->activePane();   // (the window under the pointer — the menu over it took the hover)
      if (from < 0) { const QPoint p = viewport_->mapFromGlobal(QCursor::pos()); for (int i : viewport_->shownPanes()) if (viewport_->paneRect(i).contains(p)) at = i; }
      viewport_->backToSingle(at); from = -1; return; }
    last.start();
    from = -1;                                            // (split already: the second press takes the window under the pointer — not one left from before)
    if (!viewport_->split()) { from = viewport_->activePane(); viewport_->setSplit(true); }
    openCameraMenu(this, viewport_, QCursor::pos() - QPoint(30, 20));
  };
  A["frame"] = [this] { if (scene_.room == "paint" && paintLeft_ && paintLeft_->previewFrame()) return; if (outliner_->pointerOver()) outliner_->reveal(); else viewport_->frameSelection(); };
  A["groupSel"] = [this] { outliner_->groupSelection(); };
  A["parent"] = [this] { outliner_->parentSelection(); };
  A["unparent"] = [this] { outliner_->unparentSelection(); };
  A["rename"] = [this] {                                  // (F2 over the polygroup list — or with it focused: its picked row renamed, not the mesh)
    for (QWidget* w : {QApplication::focusWidget(), QApplication::widgetAt(QCursor::pos())}) for (QWidget* x = w; x; x = x->parentWidget())
      if (auto* t = qobject_cast<QTreeWidget*>(x); t && t->objectName() == "tgTree") { QTreeWidgetItem* c = t->currentItem(); if (!c && !t->selectedItems().isEmpty()) c = t->selectedItems().first(); if (c) { t->editItem(c, 0); return; } }
    outliner_->renameSelected(); };
  A["smartRename"] = [this] { SmartRename d(viewport_, &scene_, this); d.exec(); };
  // (the Paint room: the strokes — on the model and on the texture alike — are the room's own steps, taken back first; with none left
  // the viewport's history goes on as ever)
  A["undo"] = [this] { if (scene_.room == "paint" && paintLeft_ && paintLeft_->overManager()) { paintLeft_->visUndo(false); ui::devLog("op", {{"name", "undo"}, {"what", "visibility"}}); return; }   // (over the manager window: the hiding taken back, not the stroke)
    QElapsedTimer t; t.start(); const bool pnt = scene_.room == "paint" && paint_ && paint_->undoStroke(); if (pnt) ui::hud(tr("Undo"), 500); else viewport_->undo();
    ui::devLog("op", {{"name", "undo"}, {"what", pnt ? "paint" : "scene"}, {"ms", double(t.elapsed())}}); };
  A["redo"] = [this] { if (scene_.room == "paint" && paintLeft_ && paintLeft_->overManager()) { paintLeft_->visUndo(true); ui::devLog("op", {{"name", "redo"}, {"what", "visibility"}}); return; }
    QElapsedTimer t; t.start(); const bool pnt = scene_.room == "paint" && paint_ && paint_->redoStroke(); if (pnt) ui::hud(tr("Redo"), 500); else viewport_->redo();
    ui::devLog("op", {{"name", "redo"}, {"what", pnt ? "paint" : "scene"}, {"ms", double(t.elapsed())}}); };
  A["redo2"] = A["redo"];
  A["del"] = [this] {                                     // (the Topology room in a component mode: the picked elements, not the mesh)
    const QWidget* fw = QApplication::focusWidget(); const bool inOutliner = fw && outliner_ && (fw == outliner_ || outliner_->isAncestorOf(fw));   // (the Outliner has the focus: its objects deleted, in any mode)
    if (!inOutliner && scene_.room == "topo" && viewport_->topoMode != "object") { if (viewport_->topoSel().id >= 0) viewport_->tDeleteSel(); return; }
    viewport_->deleteSelected(); };
  A["duplicate"] = [this] { viewport_->runEdit([](Scene& s, Selection& sl, const CurveCache&) { return sym::duplicateSelection(s, sl); }, true); };   // (D: the selected — in the Topology room the meshes; Ctrl+D deselects there)
  A["deselect"] = [this] { if (scene_.room == "paint") { viewport_->pvSelClear(); return; } viewport_->topoDeselect(); };   // (the Paint room: its polygon selection)
  // ([ ] / Shift+[ ]: in the Paint room the painting brush's size / hardness wherever the pointer is — not the Hair room's brush)
  A["brushSmaller"] = [this] { if (scene_.room == "paint" && paint_) { paint_->stepBrush(false, false); return; } viewport_->brushResize(0.85); syncPanels(); };
  A["brushBigger"] = [this] { if (scene_.room == "paint" && paint_) { paint_->stepBrush(true, false); return; } viewport_->brushResize(1.15); syncPanels(); };
  A["brushSofter"] = [this] { if (scene_.room == "paint" && paint_) paint_->stepBrush(false, true); };
  auto turn = [this](float d) { if (scene_.room != "paint" || !paint_) return; float a = paint_->brush.angle + d; while (a > 180) a -= 360; while (a < -180) a += 360; paint_->brush.angle = a; emit paint_->brushChanged(); ui::hud(tr("Alpha angle %1°").arg(int(std::lround(a))), 700); };
  A["brushAngleDown"] = [turn] { turn(-15); };
  A["paintWire"] = [this] { if (scene_.room != "paint") return; viewport_->paintWire = !viewport_->paintWire; viewport_->showWire = viewport_->paintDisp == "wire" || viewport_->paintWire; viewport_->sceneChanged(false); ui::hud(viewport_->paintWire ? tr("Wire on") : tr("Wire off"), 600); };
  A["paintUvFull"] = [this] { if (scene_.room == "paint" && paintLeft_) paintLeft_->setFull(!paintLeft_->full()); };
  A["paintThrough"] = [this] { if (scene_.room != "paint" || !paint_) return; paint_->setThrough(!paint_->drawThrough); ui::hud(paint_->drawThrough ? tr("Through: the walls behind too") : tr("Only what the camera sees"), 700); };
  A["paintSolo"] = [] {};                                         // (held: the event filter shows the active layer alone while the key is down)
  A["paintLocate"] = [this] {                                     // (L: the other view brought to the point under the pointer — the model ↔ the texture)
    if (scene_.room != "paint" || !paintLeft_ || paint_->doc.width() <= 0) return; const QPoint g = QCursor::pos(); PaintCanvas* cv = paintLeft_->canvas();
    if (cv->isVisible() && QRect(cv->mapToGlobal(QPoint(0, 0)), cv->size()).contains(g)) { const QPointF t = cv->texAt(cv->mapFromGlobal(g));
      if (!viewport_->paintLocateAt(QPointF(t.x() / paint_->doc.width(), t.y() / paint_->doc.height()))) ui::toast(tr("No polygon of the model there")); return; }
    const auto uv = QRect(vpBox_->mapToGlobal(QPoint(0, 0)), vpBox_->size()).contains(g) ? viewport_->paintUvAt(viewport_->fromGlobal(g)) : std::nullopt;
    if (!uv) { ui::toast(tr("L — point at the model or the texture")); return; }
    cv->centreOn(QPointF(uv->x() * paint_->doc.width(), uv->y() * paint_->doc.height())); };
  A["brushAngleUp"] = [turn] { turn(15); };
  A["brushHarder"] = [this] { if (scene_.room == "paint" && paint_) paint_->stepBrush(true, true); };
  A["relax"] = [this] { if (scene_.room == "topo") viewport_->tRelaxMesh(); };   // (Ctrl+R: the Topology room's Relax)
  A["symmetry"] = [this] { viewport_->setSymmetry(!viewport_->tset.sym); };   // (Shift+S: Symmetry — both rooms; the Topology room's UV too)
  A["selAll"] = [this] { if (scene_.room == "topo") viewport_->tSelectAll(); };   // (Ctrl+A)
  A["invertSel"] = [this] { if (scene_.room == "topo") viewport_->tInvertSel(); };   // (Ctrl+I)
  for (int i = 0; i < paintDisplayModes().size(); i++) A[QString("paintDisp%1").arg(i + 1)] = [this, i] { if (scene_.room != "paint") return; const QString k = paintDisplayModes()[i]; viewport_->setPaintDisp(k); ui::toast(tr("Display: %1").arg(paintDisplayName(k))); };
  // (the Paint room's H and its kin: on the selected polygons, else on what is under the pointer — which must be over the viewport)
  auto paintKey = [this](const char* op) {
    if (paintLeft_ && paintLeft_->previewKey(op)) return;          // (over the manager's model preview: the part under its pointer)
    const QPoint g = QCursor::pos();
    if (viewport_->pvSel.empty() && !QRect(vpBox_->mapToGlobal(QPoint(0, 0)), vpBox_->size()).contains(g)) { ui::toast(tr("Point at the model: the part under the pointer is hidden")); return; }
    viewport_->setMouseAt(viewport_->fromGlobal(g)); viewport_->pvKey(op); };
  // (one standard in every room — the Paint room's, 19.11: on the selection, else on what is under the pointer; H hide, Shift+H only it,
  // Ctrl+H / Ctrl+Shift+H the whole object, Ctrl+X everything back; a second press does not undo — Ctrl+X / Ctrl+Z do)
  auto visKey = [this, paintKey](const char* op) {
    if (scene_.room == "paint") { paintKey(op); return; }
    const QPoint g = QCursor::pos(); if (QRect(vpBox_->mapToGlobal(QPoint(0, 0)), vpBox_->size()).contains(g)) viewport_->setMouseAt(viewport_->fromGlobal(g));
    viewport_->visKey(op); if (scene_.room == "topo") syncUnhide(); rebuildOutliner(); };
  A["paintShowAll"] = [this, visKey] { if (scene_.room == "paint") viewport_->pvKey("showAll"); else visKey("showAll"); };   // (Ctrl+X)
  // (the Hair room keeps its own isolation — there more is isolated than hidden, 19.11: H hides / shows the selected, Ctrl+H isolates
  // the selection, Shift+H — with the meshes tagged Head, Ctrl+Shift+H — the working group; again — everything back)
  A["hide"] = [this, visKey] { if (scene_.room == "hair") { viewport_->toggleHide(); rebuildOutliner(); return; } visKey("hide"); };              // (H)
  A["isolate"] = [this, visKey] { if (scene_.room == "hair") { viewport_->toggleIsolate(false); return; } visKey("hideObj"); };        // (Ctrl+H)
  A["isolateHead"] = [this, visKey] { if (scene_.room == "hair") { viewport_->toggleIsolate(true); return; } visKey("only"); };       // (Shift+H)
  A["isolateGroup"] = [this, visKey] { if (scene_.room == "hair") { viewport_->toggleIsolateGroup(); return; } visKey("onlyObj"); };   // (Ctrl+Shift+H)
  A["isolateAlt"] = [this] {   // (Alt+H: the Topology room's reference meshes hidden / shown — Shift+H's old job there)
    if (scene_.room != "topo") return;
    std::vector<int> refs; bool anyOn = false;
    for (auto& [id, it] : scene_.items) if (it.type == ItemType::IMesh && it.raw.value("tref").toBool()) { refs.push_back(id); anyOn |= scene_.isVisible(it); }
    if (refs.empty()) { ui::toast(tr("No reference meshes: mark one in the mesh panel's Snapping block")); return; }
    viewport_->runEdit([&](Scene& s, Selection&, const CurveCache&) {
      if (anyOn) { for (int id : refs) s.item(id)->visible = false; }
      else {   // (shown: their hidden groups too — what else is in them stays hidden as it was)
        std::set<int> hidden; for (auto& [id, it] : s.items) if (!s.isVisible(it)) hidden.insert(id);
        for (int id : refs) for (Item* x = s.item(id); x; x = s.item(x->parent)) x->visible = true;
        for (int id : hidden) if (Item* x = s.item(id); x && s.isVisible(*x) && !x->raw.value("tref").toBool() && x->type != ItemType::Group) x->visible = false;
      }
      s.revision++; return QString(); }, false);
    viewport_->sceneChanged(false);
    ui::toast(anyOn ? tr("References hidden") : tr("References shown")); };
  // ⧉ SEL: the selection as text (to paste into a message) — and the curves become ⇶ Snap to path's target
  A["copySel"] = [this] {
    const Selection& s = viewport_->selection(); QStringList L;
    L << QString("HMC-SEL %1 — %2 item(s), %3 anchor(s)").arg(displayName()).arg(s.items.size()).arg(s.anchors.size());
    auto path = [this](const Item* it) { QStringList p; for (const Item* g = scene_.item(it->parent); g; g = scene_.item(g->parent)) p.prepend(g->name); return p.isEmpty() ? QString("root") : p.join(" / "); };
    std::vector<int> snap;
    for (int id : s.items) { const Item* it = scene_.item(id); if (!it) continue;
      QString kind = it->type == ItemType::Curve ? (it->hasPath() ? "service curve EC" : it->junc ? "service curve" : it->seg ? "curve (network piece)" : "curve") : it->typeStr, x;
      if (it->type == ItemType::Curve) { x = QString(", %1 anchors").arg(it->anchors.size()); if (it->hasPath()) { QStringList r; for (auto& p : it->path) if (scene_.item(p[0])) r << scene_.item(p[0])->name; x += ", route: " + r.join(" → "); }
        if (it->ring < 0 && !it->junc && !it->hasPath()) snap.push_back(id); }
      else if (it->type == ItemType::Surface) x = QString(", between %1 | %2").arg(scene_.item(it->a) ? scene_.item(it->a)->name : QString(), scene_.item(it->b) ? scene_.item(it->b)->name : QString());
      else if (it->type == ItemType::Group) x = QString(", %1 inside").arg(it->children.size());
      L << QString("- %1 [%2 #%3] in %4%5").arg(it->name, kind).arg(id).arg(path(it), x); }
    for (int a : s.anchors) { QStringList on; for (const Item* c : scene_.curves()) { int i = int(std::find(c->anchors.begin(), c->anchors.end(), a) - c->anchors.begin()); if (i < int(c->anchors.size())) on << QString("%1:%2/%3").arg(c->name).arg(i).arg(c->anchors.size() - 1); }
      const Vec3& p = scene_.anchor(a)->pos; L << QString("- anchor #%1 on %2 at [%3, %4, %5]").arg(a).arg(on.isEmpty() ? "—" : on.join(", ")).arg(p.x, 0, 'f', 2).arg(p.y, 0, 'f', 2).arg(p.z, 0, 'f', 2); }
    L << camPosText(viewport_);
    if (!snap.empty()) viewport_->snapSrc = snap;
    QApplication::clipboard()->setText(L.join("\n"));
    ui::toast(tr("Selection copied (%1 item(s), %2 anchor(s)) — paste it into the message").arg(s.items.size()).arg(s.anchors.size()));
  };
  A["save"] = [this] { if (saveBusy_) { ui::toast(tr("Still saving…")); return; } asyncSave_ = true; saveProject(false); asyncSave_ = false; };   // (Save: written in the background — the window not stopped)
  A["saveAs"] = [this] { saveProject(true); };
  A["open"] = [this] { openFiles(); };
  A["openDialog"] = [this] {
    if (!unsavedOk()) return;
    QSettings st;
    QWidget* over = QApplication::activeWindow() ? QApplication::activeWindow() : this;   // (over the start screen when it is open)
    QString p = QFileDialog::getOpenFileName(over, tr("Open project"), st.value("lastDir").toString(), tr("HairHarpy project (*.hairmesh *.json)"));
    if (!p.isEmpty()) { st.setValue("lastDir", QFileInfo(p).absolutePath()); forget_ = true; bool ok = openProject(p); forget_ = false; if (ok) files::closeFileWindow(); }
  };
  A["bridgeSend"] = [this] { bridgeSend(this, viewport_, &scene_, [this] { openBridgePanel(brgBtn_, viewport_, &scene_, [this] { updateSendBtn(); }); }); };
}

// End over anything of the window: an action's button — that action's key; any other button / check — a key that
// clicks it (found again by its text and tip); the keys pressed and let go are set
void MainWindow::endAssign() {
  QWidget* w = QApplication::widgetAt(QCursor::pos());
  // (a command in a button's menu, the menu open and the pointer over the item: the key goes to it as to a button — by its caption and tip)
  if (auto* mn = qobject_cast<QMenu*>(w)) { QAction* a = mn->actionAt(mn->mapFromGlobal(QCursor::pos()));
    if (!a || a->isSeparator() || a->menu()) { ui::toast(tr("End: hover a command of the menu — then the keys for it")); return; }
    const QString text = a->text(), tip = a->toolTip(), name = text.trimmed().isEmpty() ? tr("this command") : text.trimmed();
    while (QWidget* p = QApplication::activePopupWidget()) p->close();
    ui::KeyCapture::start(this, name, [name, text, tip](const QString& combo) { const QString clash = ui::Keymap::get().clashWith(QString(), combo); const QString id = ui::Keymap::get().bindButton(name, text, tip, combo);
      ui::toast(tr("“%1”: %2").arg(name, ui::Keymap::label(combo)) + (clash.isEmpty() || clash == id ? QString() : tr(" — “%1” lost it").arg(ui::Keymap::get().labelOf(clash)))); });
    return; }
  for (QWidget* x = w; x; x = x->parentWidget()) {
    if (x->property("hmAction").isValid()) {
      const QString id = x->property("hmAction").toString(), name = ui::Keymap::get().labelOf(id);
      ui::KeyCapture::start(this, name, [id, name](const QString& combo) { const QString clash = ui::Keymap::get().clashWith(id, combo);
        ui::Keymap::get().setKey(id, combo); ui::toast(tr("“%1”: %2").arg(name, ui::Keymap::label(combo)) + (clash.isEmpty() ? QString() : tr(" — “%1” lost it").arg(ui::Keymap::get().labelOf(clash)))); });
      return; }
    if (auto* b = qobject_cast<QAbstractButton*>(x)) {
      QString name = b->text().trimmed(); if (name.isEmpty()) { name = b->toolTip().section('\n', 0, 0); name = name.section(" — ", 0, 0).section(": ", 0, 0).left(60); }
      if (name.isEmpty()) name = tr("this button");
      const QString text = b->text(), tip = b->toolTip();
      ui::KeyCapture::start(this, name, [name, text, tip](const QString& combo) { const QString clash = ui::Keymap::get().clashWith(QString(), combo); const QString id = ui::Keymap::get().bindButton(name, text, tip, combo);
        ui::toast(tr("“%1”: %2").arg(name, ui::Keymap::label(combo)) + (clash.isEmpty() || clash == id ? QString() : tr(" — “%1” lost it").arg(ui::Keymap::get().labelOf(clash)))); });
      return; }
  }
  ui::toast(tr("End: hover a button or a tool first — then the keys for it"));
}

// a pie of one's own: its actions round the ring (held by its key — released towards one)
void MainWindow::openCustomPie(const QString& id) {
  const ui::CustomPie* p = nullptr; for (auto& q : ui::Keymap::get().pies()) if (q.id == id) p = &q; if (!p) return;
  ui::PieSpec s; s.title = p->name;
  for (int k = 0; k < p->items.size() && k < 8; k++) { const QString a = p->items[k]; if (a.isEmpty()) continue; ui::PieItem it; it.label = ui::Keymap::get().labelOf(a); it.act = [this, a] { runAction(a); }; s.ring.push_back(it);
    s.ringAng.push_back(-M_PI / 2 + k * M_PI / 4); }   // (each at its place on the ring: slot 1 at the top, on clockwise — the gaps kept)
  if (s.ring.empty()) { ui::toast(tr("Pie “%1” is empty — fill it in Hotkeys → Pie menus of my own").arg(p->name)); return; }
  ui::Pie::open(this, QCursor::pos(), s, true, p->combo.section('+', -1));
}

// (the Topology room: only its own tools — the Cursor, the gizmo's three, the snap mask brush)
static const QStringList TOPO_TOOLS = {"cursor", "move", "rotate", "gizmo", "tmask", "tbrush", "tpen", "tloop", "tknife", "tchain", "tweld", "tplane", "tbevel", "tinset", "tbridge", "tfill", "tshaper", "tslide", "tnormals", "toptimize", "tmirror", "treveal", "tpatch", "tlobe", "tfixmesh", "tmodgroups", "tsubdiv"};

void MainWindow::runAction(const QString& id) {
#ifdef HM_DEVTOOLS
  if (dev_) dev_->event("action", {{"id", id}});   // (development: the session's record)
#endif
  if (id.startsWith("c:")) {
    if (ui::Keymap::get().triggerCustom(id)) return;
    // (a button of a tool's options, that tool not in use: its options made for a moment, the button pressed, the tool back)
    // (one kept before its tool was: the tools' options looked through once — the tool found is kept with it)
    const ui::CustomBind* cb = ui::Keymap::get().customBind(id); const QString tool = cb ? cb->tool : QString();
    if (scene_.room == "topo" && ttools_) { const QString was = viewport_->tool(), wasPrev = prevTool_, wasMode = viewport_->topoMode; bool ok = false;
      auto inTool = [&](const QString& t) { pickTool(t); ttools_->sync(); return viewport_->tool() == t && (ui::Keymap::get().customButton(id, true) || ui::Keymap::get().customAction(id, true)); };
      if (!tool.isEmpty()) ok = inTool(tool) && ui::Keymap::get().triggerCustom(id, true);
      else for (const QString& t : TOPO_TOOLS) if (t != was && t != "cursor" && t != "move" && t != "rotate" && t != "gizmo" && inTool(t)) { ui::Keymap::get().setCustomTool(id, t); ok = ui::Keymap::get().triggerCustom(id, true); break; }
      pickTool(was); prevTool_ = wasPrev; if (viewport_->topoMode != wasMode) viewport_->setTopoMode(wasMode); ttools_->sync(); if (ok) return; }
    ui::toast(tr("“%1” isn't on screen now").arg(ui::Keymap::get().labelOf(id))); return; }
  if (id.startsWith("pie:")) { openCustomPie(id); return; }
  auto it = actions_.find(id);
  // (the Topology room's edits of the selection on a high-poly mesh: the wait shown)
  static const QStringList HEAVY = {"del", "selAll", "invertSel", "hide", "relax", "isolate", "isolateGroup", "duplicate"};
  static const QStringList STEPS = {"del", "duplicate", "groupSel", "parent", "unparent", "relax", "hide"};   // (the history's name for the step they make)
  const ui::ActionDef* sd = STEPS.contains(id) ? ui::Keymap::get().def(id) : nullptr; if (sd) viewport_->noteStep(tr(sd->label.toUtf8().constData()));
  if (it != actions_.end() && scene_.room == "topo" && HEAVY.contains(id)) { const ui::ActionDef* d = ui::Keymap::get().def(id); auto fn = it.value(); viewport_->heavyWork(d ? d->label : id, [fn] { fn(); }); viewport_->clearStepNote(); return; }
  if (it != actions_.end()) { it.value()(); viewport_->clearStepNote(); return; }
  const ui::ActionDef* d = ui::Keymap::get().def(id);
  ui::toast(tr("\"%1\" comes with the next parts of this rewrite").arg(d ? d->label : id));
}

void MainWindow::pickTool(const QString& t) {
  if (scene_.room == "topo" && !TOPO_TOOLS.contains(t)) return;
  if (scene_.room == "paint") { if (t == "move" && paintLeft_) paintLeft_->setTool(paint_->tool == "eraser" ? "brush" : "eraser");
    if (t == "edge" && paintLeft_) {                            // (V: the colour under the pointer — on the model or over the texture; always the Color map's own texels, never the screen)
      const QPoint g = QCursor::pos(); PaintCanvas* cv = paintLeft_->canvas();
      const bool overCv = cv->isVisible() && QRect(cv->mapToGlobal(QPoint(0, 0)), cv->size()).contains(g), overVp = !overCv && QRect(vpBox_->mapToGlobal(QPoint(0, 0)), vpBox_->size()).contains(g);
      if (paint_->pickFrom == "screen") {                        // (the pixel of the screen: anywhere — but not the empty viewport, not the empty space round the islands)
        if (overCv) { const QPointF t = cv->texAt(cv->mapFromGlobal(g)); const paint::IslandMap* im = paint_->islands ? paint_->islands() : nullptr; if (im && !im->ids.isNull() && im->at(t) == 0) { ui::toast(tr("Nothing to take here: the empty space round the UV islands")); return; } }
        QScreen* sc = QGuiApplication::screenAt(g); if (!sc) sc = QGuiApplication::primaryScreen();
        const QImage px = sc->grabWindow(0, g.x() - sc->geometry().x(), g.y() - sc->geometry().y(), 1, 1).toImage(); if (px.isNull()) { ui::toast(tr("The screen could not be read here")); return; }
        const QColor c = px.pixelColor(0, 0);
        if (overVp && viewport_->meshUnder(viewport_->fromGlobal(g)) < 0) {   // (off the model: a reference picture may be taken from, the viewport's own empty ground may not)
          bool refs = false; for (const auto& rp : scene_.refPlanes) refs |= rp.visible;
          if (!refs || (std::abs(c.red() - 0x2a) <= 3 && std::abs(c.green() - 0x2a) <= 3 && std::abs(c.blue() - 0x2a) <= 3)) { ui::toast(tr("Nothing to take here: the viewport is empty under the pointer")); return; } }
        paint_->color = c; emit paint_->brushChanged(); ui::hud(tr("Colour from the screen: %1").arg(c.name()), 700); return; }
      if (overCv) { cv->pickGlobal(g); return; }
      const auto uv = overVp ? viewport_->paintUvAt(viewport_->fromGlobal(g)) : std::nullopt;
      if (!uv) { ui::toast(tr("V picks the colour under the pointer — point at the model or the texture")); return; }
      if (const auto c = paint_->pickColor(QPointF(uv->x() * paint_->doc.width(), uv->y() * paint_->doc.height()), paint_->brush.size / 2 * viewport_->paintScaleNow())) {   // (under the brush: its ring on the model, in texels)
        paint_->color = *c; emit paint_->brushChanged(); showPickLoupe(paint_, QPointF(uv->x() * paint_->doc.width(), uv->y() * paint_->doc.height()), *c, g); }
      else ui::toast(paint_->pickFrom == "layer" ? tr("The active layer has no paint here to take a colour from") : tr("No paint here to take a colour from")); }
    return; }   // (the Paint room: nothing is moved there — E is the eraser, as over the texture)   // (the Paint room: selecting and moving the meshes; the brush is on the canvas)
  if (scene_.room != "topo" && t == "tmask") return;
  const QString tt = t == "draw" && viewport_->drawMode == "edges" ? QString("edge") : t;   // (the Curve tool in its Edges mode)
  if (tt != "cursor") prevTool_ = tt; viewport_->setTool(tt);
}

// H held: the hiding and isolating functions on a ring (release towards one — or click it)
void MainWindow::openAlphaRing() {
  ui::PieSpec s; s.title = tr("Alpha"); s.bare = true;
  QStringList keys = paint_->alphaFavs; if (paint_->alphaStandIn) keys.prepend("own");
  const QString dir = QDir::temp().filePath("HairHarpyAlphas"); QDir().mkpath(dir);
  for (const QString& k : keys) {
    ui::PieItem it; it.label = paint_->alphaLabel(k); it.tip = k == "own" ? tr("The brush's own tip back") : tr("This alpha stands in for the brush's tip; the brush's size, pressure and colour stay");
    const QString f = dir + "/" + QString::number(qHash(k + QString::number(paint_->brush.tipSoft) + QString::number(paint_->brush.tipFlat) + QString::number(paint_->brush.tipCore))) + ".png";
    if (!QFile::exists(f)) paint_->alphaThumb(k, 64).save(f);
    it.icon = "file:" + f; it.act = [this, k] { paint_->applyAlpha(k); }; s.ring.push_back(it); }
  if (s.ring.empty()) { ui::toast(tr("No favourite alphas: ☆ in the brush panel adds the chosen brush's tip")); return; }
  ui::Pie::open(this, QCursor::pos(), s, true, "KeyA");
}

void MainWindow::openIsolatePie() {
  if (scene_.room == "paint") { hDown_ = false; openMainPie(QCursor::pos()); return; }   // (the Paint room: H held — its own menu of what is under the pointer; the RMB is the brush's)
  ui::PieSpec s; s.title = tr("Isolation");
  auto it = [this](const QString& label, const QString& tip, const QString& act) { ui::PieItem i; i.label = label; i.tip = tip; i.act = [this, act] { runAction(act); }; return i; };
  if (scene_.room == "topo") {   // (the Topology room: the one standard of its keys — on the selection, else on what is under the pointer; a second press does not undo)
    s.ring = {it(tr("Hide"), tr("H tap — the selection hidden: polygons, polygroups or meshes; nothing selected — what is under the pointer"), "hide"),
              it(tr("Only this"), tr("Shift+H — only the selection shown; nothing selected — what is under the pointer"), "isolateHead"),
              it(tr("Hide object"), tr("Ctrl+H — the whole mesh of the selection hidden"), "isolate"),
              it(tr("Only object"), tr("Ctrl+Shift+H — only the whole mesh of the selection shown"), "isolateGroup"),
              it(tr("Hide / show references"), tr("Alt+H — the reference meshes hidden (again — shown)"), "isolateAlt"),
              it(tr("Show all"), tr("Ctrl+X — everything hidden shown again"), "paintShowAll")};
    s.onEnd = [this] { hDown_ = false; };
    ui::Pie::open(this, QCursor::pos(), s, true, "KeyH");
    return; }
  s.ring = {it(tr("Hide / show selected"), tr("H tap — the selected hidden (shown again); in a component mode: the selected polygons"), "hide"),
            it(tr("Isolate selection"), tr("Ctrl+H — only the selection shown (again — all back); picked polygroups first"), "isolate"),
            it(tr("Isolate + head"), tr("Shift+H — the selection and the head shown"), "isolateHead"),
            it(tr("Isolate working group"), tr("Ctrl+Shift+H — the working group alone; in a component mode — only the selected polygons"), "isolateGroup")};
  s.onEnd = [this] { hDown_ = false; };
  ui::Pie::open(this, QCursor::pos(), s, true, "KeyH");
}

// ~: the FAQ at the help of the button (or the toolbar's tool) under the pointer — its name and tip looked up
void MainWindow::faqForHovered() {
  QWidget* w = QApplication::widgetAt(QCursor::pos()); QString tip, name;
  for (QWidget* x = w; x; x = x->parentWidget()) { if (!x->toolTip().isEmpty()) { tip = x->toolTip(); break; } }
  if (auto* b = qobject_cast<QAbstractButton*>(w)) name = b->text();
  QString plain = tip; plain.remove(QRegularExpression("<[^>]*>"));
  extras::openFaqAt(this, scene_.room, (name.isEmpty() ? plain.section(QRegularExpression("\\s[—(:]"), 0, 0) : name) + " | " + plain);
}

// Space: every tool on a ring around the pointer (held — release towards a tool to take it; a short press leaves it
// open for a click)
void MainWindow::openToolPie() {
  if (ui::Pie::current()) { ui::Pie::closeCurrent(); return; }
  ui::PieSpec s;
  if (scene_.room == "topo") {   // (the Topology room: its own tools — never the Hair room's)
    struct T { const char* id; const char* icon; const char* label; };
    static const T TT[] = {{"move", "tool_move", "Move"}, {"rotate", "tool_rotate", "Rotate"}, {"gizmo", "tool_scale", "Scale"}, {"cursor", "tsel_cursor", "Select"},
                           {"tpen", "tt_polypen", "PolyPen"}, {"tloop", "tt_loop", "Loop"}, {"tknife", "tt_knife", "Knife"}, {"tbridge", "tt_bridge", "Bridge"},
                           {"tfill", "tt_fillhole", "Fill Hole"}, {"tweld", "tt_weld", "Weld"}, {"tslide", "tt_slide", "Slide"}, {"tbrush", "tool_brush", "Brush"}};
    for (const T& t : TT) { ui::PieItem it; it.label = tr(t.label); it.icon = t.icon; it.cur = viewport_->tool() == t.id; const QString id = t.id; it.act = [this, id] { pickTool(id); }; s.ring.push_back(it); }
    // (the inner ring: the editing mode — Object / Polygons / Vertices / Edges; released over one it is switched to)
    for (auto [k, lb, ic] : {std::tuple<const char*, const char*, const char*>{"object", "Object", "topo_object"}, {"poly", "Polygons", "topo_polygon"}, {"vert", "Vertices", "topo_vertex"}, {"edge", "Edges", "topo_edge"}}) {
      ui::PieItem it; it.icon = ic; it.tip = tr("The editing mode: %1").arg(tr(lb)); it.cur = viewport_->topoMode == k; const QString key = k; it.act = [this, key] { viewport_->setTopoMode(key); }; s.inner.push_back(it); }   // (the icon alone, no name)
    ui::Pie::open(this, QCursor::pos(), s, true, ui::Keymap::get().key("toolPie").section('+', -1));
    return;
  }
  for (auto& t : ui::ToolBox::ringTools()) {
    ui::PieItem it; it.label = t.label; it.icon = t.icon; it.cur = viewport_->tool() == t.id; it.tip = tools_->tip(t.id);
    QString id = t.id; it.act = [this, id] { pickTool(id); };
    s.ring.push_back(it);
  }
  ui::Pie::open(this, QCursor::pos(), s, true, ui::Keymap::get().key("toolPie").section('+', -1));   // (held by its own key)
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
  if (e->type() == QEvent::MouseButtonDblClick) if (auto* s = qobject_cast<QSlider*>(o); s && s->property("dblReset").isValid()) { s->setValue(s->property("dblReset").toInt()); return true; }
  if (e->type() == QEvent::ApplicationDeactivate) {   // (a held key let go with the window gone: the snaps, R's roll, B's smoothness drag, H, the solo layer — not stuck on)
    viewport_->setSnapHold('x', false); viewport_->setSnapHold('v', false); viewport_->keyRoll = false; viewport_->keyB = false; hDown_ = false;
    if (paint_ && paint_->doc.solo >= 0) { paint_->doc.solo = -1; emit paint_->pixelsChanged({}); } }
  if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease) return QMainWindow::eventFilter(o, e);
  auto* k = static_cast<QKeyEvent*>(e);
  // (the scissors' NURBS cut: Enter cuts while the pointer is over the viewport — even with a number field of a panel
  // still holding the focus from before, which would take the key and nothing be cut)
  if (e->type() == QEvent::KeyPress && viewport_->surfCutting() && (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && !QApplication::activeModalWidget() && !ui::Pie::current()
      && vpBox_->rect().contains(vpBox_->mapFromGlobal(QCursor::pos())) && viewport_->surfCutKey(k->key())) return true;
  // (typing in a text box: its own keys; a dialog open: its own)
  QWidget* fw = QApplication::focusWidget();
  if (fw && (qobject_cast<QLineEdit*>(fw) || qobject_cast<QAbstractSpinBox*>(fw) || qobject_cast<QTextEdit*>(fw) || qobject_cast<QPlainTextEdit*>(fw))) return false;
#ifdef HM_DEVTOOLS
  // (F9 — a note of the session's record: over the reference canvas too, which is modal)
  if (k->key() == Qt::Key_F9 && !k->modifiers() && refCanvas_ && QApplication::activeModalWidget() == refCanvas_) { if (e->type() == QEvent::KeyPress && !k->isAutoRepeat() && dev_) QTimer::singleShot(0, this, [this] { dev_->annotate(); }); return true; }
#endif
  if (QApplication::activeModalWidget()) return false;
  if (ui::Pie::current()) return false;                        // (the pie handles its keys)
  if (QWidget* pp = QApplication::activePopupWidget(); pp && pp->objectName() == "camMenu" && e->type() == QEvent::KeyPress && !k->isAutoRepeat()
      && ui::Keymap::get().actionFor(ui::Keymap::comboOf(k)) == "camMenu") { runAction("camMenu"); return true; }   // (Tab again over the camera menu: its double press)
  // (the floating panels — the ToolBar among them — count as the main window: a click on a tool there must not leave
  // the hotkeys (Ctrl+D…) dead until the viewport is clicked)
  QWidget* aw = QApplication::activeWindow(); if (aw && aw != this && aw->objectName() != "viewWindow" && !aw->inherits("hm::ui::DockPanel") && !((aw->windowType() == Qt::Tool) && aw->parentWidget() && aw->parentWidget()->window() == this)) return false;
#ifdef HM_DEVTOOLS
  if (k->key() == Qt::Key_F9 && !k->modifiers()) { if (e->type() == QEvent::KeyPress && !k->isAutoRepeat() && dev_) QTimer::singleShot(0, this, [this] { dev_->annotate(); }); return true; }   // (F9: an annotation — the pointer where it is now)
#endif
  // (the Paint room: Space / Ctrl+Space over the texture window — its pan and zoom, as Photoshop's; the window gets the key whatever
  // has the focus — not the tool pie, which is the Hair room's)
  // (R too: held + drag — the texture turns, Shift+R — upright; it went to the Hair room's Rotate before — the author, 17.11)
  // (R let go, wherever the pointer is: the model's roll and the texture's turn both end — pressed over one view, released over the other, neither stays held)
  if (scene_.room == "paint" && paintLeft_ && e->type() == QEvent::KeyRelease && ui::Keymap::codeOf(k) == "KeyR" && !k->isAutoRepeat()) {
    viewport_->keyRoll = false; PaintCanvas* cv = paintLeft_->canvas();
    if (cv && o == cv) return false;
    if (cv) { QKeyEvent c(k->type(), k->key(), k->modifiers(), k->nativeScanCode(), k->nativeVirtualKey(), k->nativeModifiers(), k->text(), k->isAutoRepeat()); QCoreApplication::sendEvent(cv, &c); }
    return true; }
  if (scene_.room == "paint" && paintLeft_ && (k->key() == Qt::Key_Space || (ui::Keymap::codeOf(k) == "KeyR" && !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier))))) {
    PaintCanvas* cv = paintLeft_->canvas(); const QPoint g = QCursor::pos();
    if (cv && cv->isVisible() && QRect(cv->mapToGlobal(QPoint(0, 0)), cv->size()).contains(g)) { if (o != cv) { QKeyEvent c(k->type(), k->key(), k->modifiers(), k->nativeScanCode(), k->nativeVirtualKey(), k->nativeModifiers(), k->text(), k->isAutoRepeat()); QCoreApplication::sendEvent(cv, &c); } else return false; return true; } }
  // (Alt+Backspace in the Paint room: as Photoshop — the active layer filled with the colour wherever it is not frozen; the author, 17.11)
  if (scene_.room == "paint" && paintLeft_ && k->key() == Qt::Key_Backspace && (k->modifiers() & Qt::AltModifier) && !(k->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
    if (e->type() == QEvent::KeyPress && !k->isAutoRepeat()) paintLeft_->tools()->fillAll(); return true; }
  if ((k->key() == Qt::Key_Shift || k->key() == Qt::Key_Control) && !k->isAutoRepeat())
    QTimer::singleShot(0, viewport_, [this] { viewport_->modifiersChanged(); });   // (a tool's preview that wants them: shown / gone at once)
  if (k->key() == Qt::Key_End && !k->isAutoRepeat() && !ui::KeyCapture::active()) { if (e->type() == QEvent::KeyPress) endAssign(); return true; }   // (End: a key for what is under the pointer)
  if (ui::Keymap::codeOf(k) == "KeyB" && !k->isAutoRepeat() && !k->modifiers()) {
    viewport_->keyB = e->type() == QEvent::KeyPress;           // (B held: smoothness drag)
    if (scene_.room == "topo") { viewport_->softKey(e->type() == QEvent::KeyPress); return true; }   // (Topology: Soft Selection — a tap, or held: its radius)
  }
  // A in the Paint room: held — the favourite alphas on a ring (the Hair room's A — Curve modifiers — means nothing there)
  if (scene_.room == "paint" && paint_ && ui::Keymap::codeOf(k) == "KeyA" && !k->isAutoRepeat() && !(k->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier))) {
    if (e->type() == QEvent::KeyPress && !ui::Pie::current()) openAlphaRing(); return true; }
  // (E in the Paint room stays the eraser — pickTool("move") there: the brush ↔ the eraser, over the model as over the texture)
  // R in the Paint room over the model: held + drag — the view rolls (as over the texture); Shift+R — upright again. The Hair room's
  // R (Rotate) means nothing in this room
  if (scene_.room == "paint" && paintLeft_ && ui::Keymap::codeOf(k) == "KeyR" && !k->isAutoRepeat() && !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
    if (e->type() == QEvent::KeyRelease) { const bool was = viewport_->keyRoll; viewport_->keyRoll = false; if (was) return true; }
    else if (!paintLeft_->full() && vpBox_->rect().contains(vpBox_->mapFromGlobal(QCursor::pos()))) {
      if (k->modifiers() & Qt::ShiftModifier) { viewport_->rollView(0); ui::hud(tr("View upright"), 600); } else viewport_->keyRoll = true;
      return true; } }
  // H: a tap — hide / show the selected; held — the ring of every hiding and isolating function
  if (ui::Keymap::codeOf(k) == "KeyH" && !k->isAutoRepeat() && !(k->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier))) {
    if (e->type() == QEvent::KeyPress) { hDown_ = true; hPie_ = false; QTimer::singleShot(230, this, [this] { if (hDown_ && !hPie_) { hPie_ = true; openIsolatePie(); } }); return true; }
    const bool was = hPie_; hDown_ = false; hPie_ = false; if (!was) runAction("hide"); return true;
  }
  // X / V held (the Topology room): the snap onto the grid / the vertices while the key is down; a quick tap with no mouse button
  // down still does what the key is bound to (V — the Edges tool)
  if ((ui::Keymap::codeOf(k) == "KeyX" || ui::Keymap::codeOf(k) == "KeyV") && !k->isAutoRepeat() && scene_.room == "topo" && !(k->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier))
      && !(ui::Keymap::codeOf(k) == "KeyV" && viewport_->uvKeys && viewport_->tset.uv)) {   // (the UV hotkeys mode: V is its view in turn)
    static QElapsedTimer snapT; const char c = ui::Keymap::codeOf(k) == "KeyX" ? 'x' : 'v';
    if (e->type() == QEvent::KeyPress) { snapT.start(); viewport_->setSnapHold(c, true); return true; }
    viewport_->setSnapHold(c, false);
    if (snapT.isValid() && snapT.elapsed() < 250 && QApplication::mouseButtons() == Qt::NoButton) { const QString id = ui::Keymap::get().actionFor(c == 'x' ? "KeyX" : "KeyV"); if (!id.isEmpty()) runAction(id); }
    return true;
  }
  // The Paint room's looks on a key: the wire, a display mode — a short press switches it, a held key shows it only while it is down
  // (a glance at the wire and back); the active layer alone — only while its key is held
  if (scene_.room == "paint" && paint_ && !k->isAutoRepeat() && !(k->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier))) {
    const QString id0 = ui::Keymap::get().actionFor(ui::Keymap::codeOf(k));
    if (id0 == "paintSolo") {
      if (e->type() == QEvent::KeyPress) { if (paint_->layer >= 0) { paint_->doc.solo = paint_->layer; emit paint_->pixelsChanged({}); ui::hud(tr("Only the layer %1").arg(paint_->active() ? paint_->active()->name : QString())); } }
      else if (paint_->doc.solo >= 0) { paint_->doc.solo = -1; emit paint_->pixelsChanged({}); ui::hud({}); }
      return true; }
    if (id0 == "paintThrough") {                                // (a tap switches it; held — only while the key is down, then back)
      static QElapsedTimer held;
      if (e->type() == QEvent::KeyPress) { held.start(); runAction(id0); return true; }
      if (held.isValid() && held.elapsed() > 320) { paint_->setThrough(!paint_->drawThrough); ui::hud({}); }
      held.invalidate(); return true; }
    if (id0 == "paintWire" || id0.startsWith("paintDisp")) {
      static QElapsedTimer held; static QString heldId, dispWas;
      if (e->type() == QEvent::KeyPress) { held.start(); heldId = id0; dispWas = viewport_->paintDisp; runAction(id0); return true; }
      if (heldId == id0 && held.isValid() && held.elapsed() > 320) { if (id0 == "paintWire") runAction(id0); else viewport_->setPaintDisp(dispWas); ui::hud({}); }   // (held: back to what it was)
      heldId.clear(); return true; }
  }
  if (e->type() == QEvent::KeyRelease) return false;
  if (scene_.room == "topo" && viewport_->uvKey(ui::Keymap::latinKey(k), k->modifiers())) return true;   // (the UV Editor's hotkeys mode: its keys first)
  if (scene_.room == "topo" && viewport_->topoToolKey(k->key(), k->modifiers())) return true;   // (the modelling tools' Esc / Enter / [ ])
  if ((k->key() == Qt::Key_QuoteLeft || k->key() == Qt::Key_AsciiTilde) && !k->isAutoRepeat()) { faqForHovered(); return true; }   // (~: the help of what is under the pointer)
  if (k->key() == Qt::Key_Escape) { if (dispStrip_->isVisible()) { dispStrip_->hide(); return true; } return viewport_->escape(); }
  if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && viewport_->drawingCurve() >= 0) { viewport_->finishDrawing(); return true; }
  if (viewport_->surfCutting() && (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter || k->key() == Qt::Key_Backspace) && viewport_->surfCutKey(k->key())) return true;
  QString combo = ui::Keymap::comboOf(k), id = ui::Keymap::get().actionFor(combo);
  if (id.isEmpty()) return false;
  const ui::ActionDef* d = ui::Keymap::get().def(id);
  if (k->isAutoRepeat() && !(d && d->repeatable)) return true;
  runAction(id);
  return true;
}

void MainWindow::resizeEvent(QResizeEvent* e) {
  QMainWindow::resizeEvent(e);
  // (the corner's pictures over the bars at the window's top right — the bars are the window's toolbars, across its whole width)
  if (faq_) { faq_->move(width() - 60, 2); faq_->raise(); }
  if (inspBtn_) { inspBtn_->move(width() - 120, 2); inspBtn_->raise(); }
  { const int x = width() - 180; if (keysBtn_) { keysBtn_->move(x, 2); keysBtn_->raise(); }   // (Hotkeys, then the rooms' switch — its two halves touching)
    if (roomPaint_) { roomPaint_->move(x - 62, 2); roomPaint_->raise(); } if (roomTopo_) { roomTopo_->move(x - 118, 2); roomTopo_->raise(); } if (roomHair_) { roomHair_->move(x - 174, 2); roomHair_->raise(); } }
  if (cornerBg_) { const int icons = width() - 180 - 174, F = 160, G = 20, x0 = icons - G;
    // (under the corner's pictures the bars' own colour; the bars' lines (their bottom borders, y 29 and 59) fade out over
    // 160 px before it — only the lines: what stands on the bars there stays as it is)
    cornerBg_->setGeometry(x0, 0, width() - x0, 61); cornerBg_->setStyleSheet("background:#262626"); cornerBg_->raise(); cornerBg_->stackUnder(faq_);
    for (int k = 0; k < 2; k++) { const QString nm = QString("lineFade%1").arg(k); auto* f = findChild<QWidget*>(nm);
      if (!f) { f = new QWidget(this); f->setObjectName(nm); f->setAttribute(Qt::WA_TransparentForMouseEvents); f->setAttribute(Qt::WA_StyledBackground);
        f->setStyleSheet("background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 rgba(38,38,38,0),stop:1 #262626)"); f->show(); }
      f->setGeometry(x0 - F, k == 0 ? 29 : 59, F, 1); f->raise(); f->stackUnder(faq_); } }
  for (QWidget* b : std::initializer_list<QWidget*>{roomHair_, roomTopo_, roomPaint_, keysBtn_, inspBtn_, faq_}) if (b) b->raise();
  if (insp_ && insp_->isVisible()) insp_->place();
  if (hist_ && hist_->isVisible()) hist_->place();
  if (report_ && report_->isVisible()) report_->place();
}
void MainWindow::moveEvent(QMoveEvent* e) { QMainWindow::moveEvent(e); if (hist_ && hist_->isVisible()) hist_->place(); if (report_ && report_->isVisible()) report_->place(); if (insp_ && insp_->isVisible()) insp_->place(); }

void MainWindow::syncPanels() {
  ProfScope prof_("syncPanels"); panelBuilds_++;
  if (convPanel_) convPanel_->sync();
  // Active Tool: the tool's own panel (none — the window hidden); Settings: filled as the items' settings are ported
  // (the Hair room: the options on the right of its tools' window, as the Topology room's — the Active Tool panel not used)
  if (scene_.room == "paint") dpTool_->setWanted(false);
  else if (scene_.room == "hair" && htools_) { htools_->setOptions(buildToolPanel(viewport_, &scene_)); dpTool_->setWanted(false); }
  else if (QWidget* tw = buildToolPanel(viewport_, &scene_)) { dpTool_->setBody(tw); dpTool_->setWanted(true); }
  else dpTool_->setWanted(false);
  // Settings: the selection's (📌 — the pinned item's, while it exists)
  if (propsPin_ >= 0 && !scene_.item(propsPin_)) propsPin_ = -1;
  if (!dpProps_->pinned() || propsPin_ >= 0) dpProps_->setBody(buildPropsPanel(viewport_, &scene_, dpProps_->pinned() ? propsPin_ : -1));
}

void MainWindow::setViewSetting(const QString& key, const QJsonValue& v) {
  QJsonObject S = scene_.fileRaw.value("settings").toObject(), vw = S.value("view").toObject();
  if (vw.value(key) == v) return;
  vw.insert(key, v); S.insert("view", vw); scene_.fileRaw.insert("settings", S);
  viewport_->sceneChanged(false);
  viewDirty_ = true;                                         // (a change of the project, not an undo step)
  updateTitle();
}

// ======================= project =======================
bool MainWindow::openProject(const QString& path) {
  ui::DevOp devT("open", 0, {{"file", QFileInfo(path).fileName()}});
  waitSave();
  QApplication::setOverrideCursor(Qt::WaitCursor);
  QElapsedTimer t; t.start();
  ui::BusyScope bz(tr("Opening"));
  LoadResult r = loadProject(path, scene_);
  QApplication::restoreOverrideCursor();
  if (!r.ok) { QMessageBox::warning(this, tr("Open project"), r.error); return false; }
  forcedDirty_ = false; projName_ = QFileInfo(path).fileName(); propsPin_ = -1;   // (the pinned Settings: an id of the project before means another item here)
  report_->hide(); if (board_) board_->reload();
  viewport_->sceneChanged(true);                             // (the hair — cocoons — rebuilt on the new scene first: the outliner reads them; before, it read the cocoons of the project closed — a crash)
  rebuildOutliner();
  if (paint_) { paint_->fromJson(scene_.fileRaw.value("paint").toObject()); paint_->loadFiles(path); }
  if (paintLeft_) paintLeft_->historyReset();                      // (the visibility's undo history: of this project only)
  { const QString rm = scene_.fileRaw.value("room").toString(); if (rm == "topo" || rm == "paint") { goRoom(rm); } }
  savedChanges_ = changeTotal();
  viewDirty_ = false;
  updateTitle(); syncPanels();
  if (syncDisplay_) syncDisplay_();
  QString msg = tr("%1 opened — %2 ms").arg(QFileInfo(path).fileName()).arg(t.elapsed());
  if (!r.warnings.isEmpty()) msg += "  ·  " + r.warnings.join("; ");
  ui::toast(msg);
  looks::projectTexDir(scene_); afterLoad(path);
  QTimer::singleShot(0, this, [this] { offerBrokenWalls(); });
  return true;
}

// Save: into the project's own file (an older .json one too); Save as… / a project not saved yet: asks where (.hairmesh)
bool MainWindow::saveProject(bool saveAs) {
  QString path = scene_.path;
  
  if (saveAs || path.isEmpty()) {
    QSettings st;
    QString base = path.isEmpty() ? files::baseName(projName_) : QFileInfo(path).completeBaseName();
    QString dir = path.isEmpty() ? st.value("lastDir", files::dataDir("Projects")).toString() : QFileInfo(path).absolutePath();
    path = QFileDialog::getSaveFileName(this, tr("Save project"), QDir(dir).filePath(base + ".hairmesh"), tr("HairHarpy project (*.hairmesh)"));
    if (path.isEmpty()) return false;
    if (!path.endsWith(".hairmesh", Qt::CaseInsensitive)) path += ".hairmesh";
    st.setValue("lastDir", QFileInfo(path).absolutePath());
  }
  return saveProjectTo(path);
}

bool MainWindow::saveProjectTo(const QString& path) {
  ui::DevOp devT("save", 0, {{"file", QFileInfo(path).fileName()}});
  waitSave();                                                // (one save at a time)
  viewport_->beforeFrame();                                 // (an edit still waiting for the next frame: in first)
  { const QString was = scene_.path; scene_.path = path; looks::rememberTexDir(scene_); scene_.path = was; }   // (the texture folder kept with the project)
  // pfFlush: the meshes kept in files of their own in the project folder (<project>_files/Meshes) — after Save as…
  // somewhere else copied into the new project's folder (the ones next to their original stay where they are)
  { QFileInfo pf(path); QDir dir(pf.dir().filePath(pf.completeBaseName() + "_files"));
    for (auto& [id, it] : scene_.items) {
      if (it.type != ItemType::IMesh) continue; QJsonObject src = it.raw.value("src").toObject(); QJsonObject ref = src.value("ref").toObject();
      if (ref.isEmpty() || ref.value("where").toString() != "proj" || !ref.value("ok").toBool(true)) continue;
      QString file = ref.value("file").toString(); if (file.isEmpty() || !QFile::exists(file)) continue;
      if (QFileInfo(file).absoluteFilePath().startsWith(dir.absolutePath() + "/", Qt::CaseInsensitive)) { ref.insert("rel", dir.relativeFilePath(file)); }
      else {
        dir.mkpath("Meshes"); QString stem = QFileInfo(file).completeBaseName(), ext = QFileInfo(file).suffix(), to = dir.filePath("Meshes/" + stem + "." + ext);
        for (int k = 2; QFile::exists(to); k++) to = dir.filePath(QString("Meshes/%1_%2.%3").arg(stem).arg(k).arg(ext));
        if (!QFile::copy(file, to)) { ui::toast(tr("Project folder: could not copy %1").arg(QFileInfo(file).fileName())); continue; }
        ref.insert("file", to); ref.insert("rel", dir.relativeFilePath(to));
      }
      src.insert("ref", ref); it.raw.insert("src", src);
    } }
  SaveExtras x; x.camera = viewport_->cameraJson(); x.thumb = thumbDataUrl(); x.cocoons = viewport_->hair().cocoonSystem();
  x.convChecked = convChecked(scene_, viewport_->hair(), viewport_->selection().items);
  QApplication::setOverrideCursor(Qt::WaitCursor);
  ui::ThinkScope th(tr("Saving %1").arg(QFileInfo(path).fileName()), 0); th.now();   // (a manual save: the waiting window)
  if (asyncSave_) {                                             // (Save: the scene taken now, made into the file and written in the background)
    QApplication::restoreOverrideCursor(); saveBusy_ = true; ui::toast(tr("Saving %1…").arg(QFileInfo(path).fileName())); ui::thinkBegin(tr("Saving %1").arg(QFileInfo(path).fileName()));   // (the waiting window till the file is written)
    auto sc = std::make_shared<Scene>(fileScene()); const int cc = changeTotal(); QPointer<MainWindow> self(this);
    snapCocoons(x, *sc);
    std::thread([self, sc, x, path, cc] {
      QString err; const bool ok = hm::saveProject(path, *sc, x, &err);
      QMetaObject::invokeMethod(qApp, [self, ok, err, path, cc, thumb = x.thumb] {
        ui::thinkEnd(); if (!self) return; self->saveBusy_ = false;
        if (!ok) { QMessageBox::warning(self, tr("Save project"), tr("The project could not be saved:\n%1").arg(err)); return; }
        if (self->paint_) self->paint_->saveFilesAsync(path, true, [self, path](bool ok) { if (self && !ok) { self->forcedDirty_ = true; self->updateTitle(); ui::toast(tr("Some paint layers of %1 could not be written — save again").arg(QFileInfo(path).fileName())); } });   // (the layers that changed: in the background — painting goes on)
        self->scene_.path = path; self->savedChanges_ = cc; self->viewDirty_ = false; self->forcedDirty_ = false; self->projName_ = QFileInfo(path).fileName();
        self->updateTitle(); self->clearRecovery(); files::addRecent(path, thumb); ui::toast(tr("Saved: %1").arg(QFileInfo(path).fileName())); }, Qt::QueuedConnection);
    }).detach();
    return true;
  }
  QString err; bool ok = hm::saveProject(path, fileScene(), x, &err);
  QApplication::restoreOverrideCursor();
  const bool layersOk = !(ok && paint_) || paint_->saveFiles(path);
  if (!ok) { QMessageBox::warning(this, tr("Save project"), tr("The project could not be saved:\n%1").arg(err)); return false; }
  scene_.path = path; savedChanges_ = changeTotal(); viewDirty_ = false; forcedDirty_ = false; projName_ = QFileInfo(path).fileName();
  if (!layersOk) {   // (the project is written, some of its paint layers are not: it stays unsaved, and closing asks again)
    forcedDirty_ = true; updateTitle();
    QMessageBox::warning(this, tr("Save project"), tr("The project is saved, but some paint layers of %1 could not be written (the folder is not writable, the disk is full, or another program holds a layer's file). Save again.").arg(QFileInfo(path).fileName()));
    return false; }
  updateTitle(); clearRecovery();
  files::addRecent(path, x.thumb);
  ui::toast(tr("Saved: %1").arg(QFileInfo(path).fileName()));
  return true;
}

// the Hair room's items where the project always kept them (another room shown: from what Hair keeps, Meshes as now),
// the other rooms under P.rooms
Scene MainWindow::fileScene() const {
  Scene out = scene_;
  viewport_->hairItemsFor(out);
  QJsonObject rooms = viewport_->roomsJson();
  if (rooms.isEmpty()) out.fileRaw.remove("rooms"); else out.fileRaw.insert("rooms", rooms);
  if (scene_.room == "hair") out.fileRaw.remove("room"); else out.fileRaw.insert("room", scene_.room);   // (opened again in the room it was saved in)
  if (paint_ && paint_->doc.width() > 0) out.fileRaw.insert("paint", paint_->toJson());
  if (refCanvas_) refCanvas_->packInto(out);                  // (the reference drawing open now: its layers as they are)
  return out;
}

QWidget* MainWindow::uiScaleControl() {
  auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(4);
  const double now = QSettings().value("ui/scale", 0.9).toDouble();
  auto* l = new QLabel(tr("Interface")); l->setProperty("mut", true);
  const QString tip = tr("The size of the interface (buttons, text, panels). 90% is the standard. A change applies after a restart — Apply (⟳ Reload: the project stays as it is)");
  l->setToolTip(tip);
  auto* s = new QSlider(Qt::Horizontal); s->setRange(12, 30); s->setValue(int(std::lround(now * 20))); s->setFixedWidth(80); s->setToolTip(tip);   // (steps of 5 %)
  s->setProperty("dblReset", 18); s->installEventFilter(this);
  auto* v = new QLabel; v->setFixedWidth(34); v->setProperty("mut", true);
  auto* ap = btn(tr("Apply"), tr("Restart with the new interface size (⟳ Reload: the project stays as it is)")); ap->setEnabled(false);
  auto show = [s, v, ap, now] { v->setText(QString("%1%").arg(s->value() * 5)); ap->setEnabled(std::abs(s->value() / 20.0 - now) > 1e-6); };
  show();
  connect(s, &QSlider::valueChanged, w, [s, show] { QSettings().setValue("ui/scale", s->value() / 20.0); show(); });
  connect(ap, &QPushButton::clicked, this, [this] { reloadApp(); });
  h->addWidget(l); h->addWidget(s); h->addWidget(v); h->addWidget(ap);
  return w;
}

// the UV mode's window: shown while the mode is on in the Topology room
void MainWindow::syncUvTools() {
  if (!uvtools_) return;
  const bool on = viewport_->uvMode();
  if (on && !uvtools_->isVisible()) { uvtools_->place(vpBox_); uvtools_->show(); uvtools_->raise(); uvtools_->sync(); }
  else if (!on && uvtools_->isVisible()) uvtools_->hide();
}

// The model's own textures into the Paint room: the first time a UV set is shown there, what its polygons wear in the other rooms (the
// meshes' colour maps — a file's own or linked ones) becomes the pixels of a "Model texture" layer of that set, above the base fill and
// under the paint — painted over, masked, hidden or exported like any layer. Nothing to take (no colour maps): nothing is made
void MainWindow::paintImportTexture(bool quiet) {
  ui::DevOp devT("paintImportTexture", 15);
  if (!paint_ || paint_->doc.width() <= 0) return; const QString set = paint_->doc.activeSet(); if (paint_->texIn.contains(set)) return;
  const int W = paint_->doc.width(), H = paint_->doc.height(); QImage ground; std::map<QString, QImage> imgs;
  for (auto& [id, it] : scene_.items) {
    if (it.type != ItemType::IMesh || scene_.noPaint(it)) continue; const PolyMesh* B = it.base ? it.base.get() : it.poly.get(); if (!B || B->cornerUV.size() != B->faceVerts.size() || B->cornerUV.empty()) continue;
    int ks = -1; if (!set.isEmpty()) { for (size_t i = 0; i < B->uvSetNames.size(); i++) if (B->uvSetNames[i] == set) ks = int(i); if (ks < 0) continue; }
    const auto sets = looks::setsOf(it); std::map<int, size_t> setOf; for (size_t i = 0; i < sets.size(); i++) setOf.emplace(sets[i].mat, i);
    std::vector<QPainterPath> paths(sets.size()); std::vector<char> any(sets.size(), 0);
    for (int f = 0; f < B->faceCount(); f++) {
      if (ks >= 0 && (!(size_t(f) < B->faceSets.size() && (B->faceSets[size_t(f)] >> ks & 1u)) || (size_t(f) < B->faceLive.size() && B->faceLive[size_t(f)] != ks))) continue;   // (the set's polygons that wear its UV now: the textures are drawn through it)
      const int mat = size_t(f) < B->faceMat.size() ? B->faceMat[size_t(f)] : -1; const auto si = setOf.find(mat); if (si == setOf.end()) continue;
      const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1]; if (e - s < 3) continue;
      double cu = 0, cv = 0; for (int c = s; c < e; c++) { cu += B->cornerUV[size_t(c)][0]; cv += B->cornerUV[size_t(c)][1]; } const double ou = std::floor(cu / (e - s)), ov = std::floor(cv / (e - s));
      QPolygonF poly; for (int c = s; c < e; c++) { const double u = B->cornerUV[size_t(c)][0] - ou, v = B->cornerUV[size_t(c)][1] - ov; poly << QPointF(u * W, (B->flipY ? 1 - v : v) * H); }
      paths[si->second].addPolygon(poly); paths[si->second].closeSubpath(); any[si->second] = 1; }
    for (size_t i = 0; i < sets.size(); i++) { if (!any[i]) continue; const looks::MapSrc src = looks::resolve(it, int(i), 0); if (src.kind == looks::MapSrc::None || src.missing || src.removed) continue;
      auto c = imgs.find(src.key); if (c == imgs.end()) { QImage im = src.kind == looks::MapSrc::Linked ? looks::loadImage(src.path) : looks::decodeImage(src.bytes);
        c = imgs.emplace(src.key, im.isNull() ? QImage() : im.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied)).first; }
      if (c->second.isNull()) continue;
      if (ground.isNull()) { ground = QImage(W, H, QImage::Format_ARGB32_Premultiplied); ground.fill(Qt::transparent); }
      QPainter p(&ground); paths[i].setFillRule(Qt::WindingFill); const QBrush tex(c->second); p.setBrush(tex); p.setPen(QPen(tex, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.drawPath(paths[i]); }   // (the pen: a few texels past the shells' edges — no seam on the model)
  }
  paint_->texIn << set;                                         // (asked once per set — also when there was nothing to take)
  if (ground.isNull()) return;
  const QString name = tr("Model texture"); int id = -1, fill = -1;
  for (const paint::Layer& l : paint_->doc.layers()) { if (l.kind == paint::LayerKind::Paint && l.channel == paint::Channel::Color && l.name == name) id = l.id; if (fill < 0 && l.kind == paint::LayerKind::Fill && l.channel == paint::Channel::Color) fill = l.id; }
  if (id < 0) id = paint_->doc.addPaint(paint::Channel::Color, name, fill);
  paint_->doc.ensurePixels(id); if (paint::Layer* l = paint_->doc.layer(id)) l->image = ground.convertToFormat(QImage::Format_ARGB32);
  if (quiet) return;
  emit paint_->layersChanged(); emit paint_->pixelsChanged({});
  ui::toast(tr("The model's texture brought in as the layer \"%1\" of this UV set").arg(name));
}

// (into a room: the viewport's switch calls applyRoom by its signal — applied here only when it did not, the room being the same)
void MainWindow::goRoom(const QString& room) { const int n = roomApplies_; viewport_->switchRoom(room); if (roomApplies_ == n) applyRoom(scene_.room); }

void MainWindow::applyRoom(const QString& room) {
  roomApplies_++; ui::DevOp roomT("room", 0, {{"room", room}});
  const bool topo = room == "topo", pnt = room == "paint", hairR = !topo && !pnt;
  { QSignalBlocker a(roomHair_), b(roomTopo_), c(roomPaint_); roomHair_->setChecked(hairR); roomTopo_->setChecked(topo); roomPaint_->setChecked(pnt); roomHair_->setText({}); roomTopo_->setText({}); roomPaint_->setText({}); }   // (the active room named, the other — its picture)
  tmodes_->setVisible(topo); tools_->setRoom(room); viewport_->syncCursor();
  for (QWidget* w : std::initializer_list<QWidget*>{srBtn_, ccBtn_, lockSelBtn_}) if (w) w->setVisible(hairR);
  dpConv_->setWanted(hairR); dpTtab_->setWanted(topo); for (auto* p : dpPaint_) p->setWanted(pnt);
  viewport_->showWire = pnt && (viewport_->paintDisp == "wire" || viewport_->paintWire);   // (the room's "Standard + wireframe" display, or W)
  outliner_->setVisible(!pnt); paintLeft_->setVisible(false); dock_->setVisible(!pnt); if (panelsBtn_) panelsBtn_->setVisible(pnt);
  if (pnt) paintDocksEnter(); else paintDocksLeave(); paintTop_->setVisible(pnt); paintMode_->setVisible(pnt); if (bakeBtn_) bakeBtn_->setVisible(pnt);   // (the Paint room's panels: the window's docks, as the user left them)
  dpProps_->setWanted(!pnt); tools_->setVisible(!pnt);        // (the Paint room: the palette and the layers in the right column — no object settings, no transform tools)
  for (QWidget* x : std::initializer_list<QWidget*>{gzSpace_, gzAx_[0], gzAx_[1], gzAx_[2], soloBtn_, findChild<QLabel*>("gzLabel")}) if (x) x->setVisible(!pnt);
  if (pnt) { if (!paintCanvas_) { paintCanvas_ = paintLeft_;
      // (the map on the meshes: the canvas's picture handed to the viewport whenever it changed; the brush on the model goes to the canvas's)
      { PaintCanvas* cv = paintLeft_->canvas();
        auto push = [this, cv] { auto& pv = scene_.paintView; const bool on = scene_.room == "paint" && paint_->onModel && !cv->shown().isNull(); const QString set = paint_->doc.activeSet();
          QImage shown = on ? cv->shown() : QImage(); viewport_->paintTexSize = QSize(paint_->doc.width(), paint_->doc.height()); viewport_->paintRingPx = paint_->brush.size;
          if (on && viewport_->paintDisp == "normal") { const QImage nm = paint_->normalResult(); if (!nm.isNull()) shown = nm.convertToFormat(QImage::Format_ARGB32_Premultiplied); }   // (the normal map as it will be written)
          if (on && viewport_->paintDisp == "material") {          // (the material builder's graph worked out over the texture — at most 1024 across: seen at once)
            if (paint_->material.nodes.empty()) paint_->material = paint::MatGraph::toon();
            const int W = paint_->doc.width(), H = paint_->doc.height(), mw = std::min(W, 1024), mh = std::max(1, int(qint64(H) * mw / std::max(1, W)));
            const auto& G = viewport_->pvBakeGeom(set, mw, mh); paint::MatContext c; c.w = mw; c.h = mh; c.normal = G.n.data(); c.pos = G.p.data();
            c.map = [this](const QString& key) { for (const paint::MapInfo& m : paint::maps()) if (key == m.key) { const QImage im = m.ch == paint::Channel::Height ? paint_->doc.compose(m.ch) : paint_->doc.flatten(m.ch); return im.format() == QImage::Format_Grayscale16 ? im.convertToFormat(QImage::Format_Grayscale8) : im; } return QImage(); };
            const Camera& cam = viewport_->camera(); const Vec3 d = cam.dir(); c.ortho = cam.ortho(); c.eye[0] = float(cam.pos.x); c.eye[1] = float(cam.pos.y); c.eye[2] = float(cam.pos.z); c.look[0] = float(d.x); c.look[1] = float(d.y); c.look[2] = float(d.z);
            const paint::Light* L = nullptr; for (const paint::Light& l : paint_->rig.lights) if (l.on) { L = &l; break; }
            const double az = (L ? L->azimuth : 40) * 3.14159265358979 / 180, el = (L ? L->elevation : 35) * 3.14159265358979 / 180; Vec3 ld(std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el));
            if (!L || L->follow) ld = cam.quat.rotate(ld); c.light[0] = float(ld.x); c.light[1] = float(ld.y); c.light[2] = float(ld.z);
            const QImage mi = paint::matEvaluate(paint_->material, c); if (!mi.isNull()) shown = mi; }
          bool re = on != pv.on || set != pv.set; if (pv.others.erase(set)) re = true;   // (the set shown now is drawn live, not from its kept picture)
          // (a stroke's step: only the part it changed copied into the model's picture and sent to the GPU; anything else — the picture anew)
          const QRect dr = cv->shownDirty(); const bool live = viewport_->paintDisp != "normal" && viewport_->paintDisp != "material";
          if (on && !re && live && !dr.isEmpty() && !pv.flat.isNull() && pv.flat.size() == shown.size() && pv.flat.format() == shown.format() && QRect(QPoint(0, 0), shown.size()).contains(dr)) {
            const int bpp = shown.depth() / 8;
            for (int y = dr.top(); y <= dr.bottom(); y++) std::memcpy(pv.flat.scanLine(y) + qsizetype(dr.left()) * bpp, shown.constScanLine(y) + qsizetype(dr.left()) * bpp, size_t(dr.width()) * bpp);
            if (pv.dirty.w != shown.width() || pv.dirty.h != shown.height()) pv.dirty.reset(shown.width(), shown.height()); pv.dirty.mark(dr); }
          else { pv.flat = on ? shown.copy() : QImage(); pv.full = true; pv.dirty.clear(); }
          pv.on = on; pv.set = set; pv.rev++; viewport_->paintOnModel = on;
          // (painting textures: every mesh with UVs goes to its Texture display by itself — its material switched off, its textures on;
          // the Display buttons show it. A mesh brought in later is switched at the next stroke)
          bool modes = false;
          if (on) for (auto& [id, it] : scene_.items) { if (it.type != ItemType::IMesh || !it.poly || it.poly->cornerUV.size() != it.poly->faceVerts.size() || it.poly->cornerUV.empty()) continue;
            if (it.raw.value("mat").isObject() && !it.raw.value("matOff").toBool()) { it.raw.insert("matOff", true); modes = true; }
            QJsonObject L = it.raw.value("look").toObject(); if (!L.value("tex").toBool()) { L.insert("tex", true); it.raw.insert("look", L); modes = true; } }
          if (modes) { re = true; QTimer::singleShot(0, this, [this] { syncPanels(); }); }
          if (re) viewport_->paintViewChanged(); else viewport_->requestUpdate(); };
        cv->shownChanged = [this, push] { if ((viewport_->paintDisp == "normal" || viewport_->paintDisp == "material") && viewport_->paintDragging()) return; push(); };   // (the normal map: made anew when the stroke ends, not at every dab)
        cv->strokeEnded = [this, push](bool, float, float) { if (viewport_->paintDisp == "normal" || viewport_->paintDisp == "material") push(); };
        connect(paint_, &PaintSession::settingsChanged, this, [this, push] { if (viewport_->paintDisp == "material") push(); });   // (the graph, the lights changed)
        // (the light that follows the camera, the rim: worked out again once the camera has come to rest)
        { auto* tm = new QTimer(this); tm->setInterval(250); auto last = std::make_shared<std::pair<Vec3, Quat>>();
          connect(tm, &QTimer::timeout, this, [this, push, last] { if (scene_.room != "paint" || viewport_->paintDisp != "material" || QApplication::mouseButtons() != Qt::NoButton) return;
            const Camera& cam = viewport_->camera(); if (cam.pos.distanceTo(last->first) < 1e-9 && cam.quat.x == last->second.x && cam.quat.y == last->second.y && cam.quat.z == last->second.z && cam.quat.w == last->second.w) return;
            *last = {cam.pos, cam.quat}; push(); }); tm->start(); }
        // (the other UV sets' paint on the model: the set left — composed as it is; another map shown — all of them anew)
        connect(paint_, &PaintSession::setLeaving, this, [this] { auto& pv = scene_.paintView; const QString cur = paint_->doc.activeSet(); if (scene_.room != "paint" || cur.isEmpty() || paint_->view.startsWith("bake:")) return;
          bool any = false; for (const paint::Layer& l : paint_->doc.layers()) any |= !l.image.isNull(); const QImage f = any ? paintSetPicture(cur, {}) : QImage(); if (f.isNull()) pv.others.erase(cur); else pv.others[cur] = {f, ++paintOtherRev_}; });
        connect(paint_, &PaintSession::setPixels, this, [this](const QString& set, const QRect& r) { paintOtherChanged(set, r); });
        // (the layers changed - one shown or hidden, its blend, its opacity, one added or deleted: the other sets' pictures made anew, once the changes have settled)
        { auto* tm = new QTimer(this); tm->setSingleShot(true); tm->setInterval(200);
          connect(tm, &QTimer::timeout, this, [this, tm] { if (scene_.room != "paint") return; if (QApplication::mouseButtons() != Qt::NoButton) { tm->start(); return; } paintOthersRefresh(); });
          connect(paint_, &PaintSession::layersChanged, tm, [tm] { tm->start(); }); }
        connect(paint_, &PaintSession::viewChanged, this, [this] { static QString last; if (scene_.room == "paint" && last != paint_->view) { last = paint_->view; paintOthersRefresh(); } });
        connect(paint_, &PaintSession::viewChanged, this, [this] { if (scene_.room == "paint") paintImportTexture(); });   // (a UV set shown for the first time: the model's own texture under the paint)
        connect(paint_, &PaintSession::viewChanged, this, push); connect(viewport_, &Viewport::paintVisChanged, this, [this, push] { static QString last; if (last != viewport_->paintDisp) { last = viewport_->paintDisp; push(); } });
        // (Symmetry: the stroke's mirrored point in the texture — between two points of the stroke the dabs' mirrors lie between the two mirrors)
        struct Mir { bool on = false, had = false; QPointF a, b; std::vector<QPointF> a2, b2; }; auto mir = std::make_shared<Mir>();
        viewport_->paintMirror = [this, mir](const std::vector<QPointF>& uv) { mir->on = false; if (uv.empty()) { mir->had = false; return; } const double W = paint_->doc.width(), H = paint_->doc.height();
          std::vector<QPointF> t; for (const QPointF& u : uv) t.push_back(QPointF(u.x() * W, u.y() * H));
          const QPointF c = viewport_->paintTexNow(); const QPointF ct(c.x() * W, c.y() * H); const bool cont = mir->had && mir->b2.size() == t.size();
          mir->a = cont ? mir->b : ct; mir->a2 = cont ? mir->b2 : t; mir->b = ct; mir->b2 = t; mir->had = true; mir->on = true; };
        cv->mirrorsN = [this, mir, cv](const QPointF& t) -> std::vector<QPointF> {
          std::vector<QPointF> out; const double W = paint_->doc.width(), H = paint_->doc.height();
          if (!viewport_->paintDragging()) {                     // (a stroke on the texture itself: its point's place on the model, mirrored there)
            if (!viewport_->tset.sym || W <= 0 || H <= 0) return out;
            if (const auto h = viewport_->paintHitOfUv(QPointF(t.x() / W, t.y() / H))) for (const QPointF& u : viewport_->paintMirrorsUv(*h)) out.push_back(QPointF(u.x() * W, u.y() * H));
            return out; }
          if (!mir->on) return out;
          const QPointF d = mir->b - mir->a; const double L = d.x() * d.x() + d.y() * d.y(); const double k = L > 1e-9 ? std::clamp(((t.x() - mir->a.x()) * d.x() + (t.y() - mir->a.y()) * d.y()) / L, 0.0, 1.0) : 1.0;
          for (size_t i = 0; i < mir->b2.size(); i++) { const QPointF d2 = mir->b2[i] - mir->a2[i];
            if (std::hypot(d2.x(), d2.y()) > 4 * std::sqrt(L) + std::max(24.0, double(paint_->brush.size))) out.push_back(mir->b2[i] + (t - mir->b));   // (the mirror jumped over a UV seam: its dabs by the new place)
            else out.push_back(mir->a2[i] + d2 * k); }
          return out; };
        cv->mirror = [this, cv](const QPointF& t) -> std::optional<QPointF> {   // (the second ring over the texture: where the first mirror's dab would go)
          if (cv->painting() || viewport_->paintDragging() || !viewport_->tset.sym) return std::nullopt; const double W = paint_->doc.width(), H = paint_->doc.height(); if (W <= 0 || H <= 0) return std::nullopt;
          const auto h = viewport_->paintHitOfUv(QPointF(t.x() / W, t.y() / H)); if (!h) return std::nullopt; const auto u = viewport_->paintMirrorsUv(*h); if (u.empty()) return std::nullopt;
          return QPointF(u[0].x() * W, u[0].y() * H); };
        cv->symmetryOn = [this] { return viewport_->tset.sym; };
        cv->mirrorOf = [this](const QPointF& t) -> std::optional<QPointF> {   // (the Stamp's symmetric copy: a texture point's first mirror, painting or not)
          if (!viewport_->tset.sym) return std::nullopt; const double W = paint_->doc.width(), H = paint_->doc.height(); if (W <= 0 || H <= 0) return std::nullopt;
          const auto h = viewport_->paintHitOfUv(QPointF(t.x() / W, t.y() / H)); if (!h) return std::nullopt; const auto u = viewport_->paintMirrorsUv(*h); if (u.empty()) return std::nullopt;
          return QPointF(u[0].x() * W, u[0].y() * H); };
        viewport_->paintStrokeEnd = [mir] { mir->on = false; mir->had = false; };
        // (Ctrl + Shift + click, on the model or on the texture: the topmost layer that has paint there becomes the active one)
        cv->pickLayer = [this](const QPointF& t) { const int id = paint_->doc.layerAt(paint_->modeChannel(), int(std::floor(t.x())), int(std::floor(t.y())));
          if (id < 0) { ui::toast(tr("No layer of this map has paint here")); return; } paint_->setLayer(id); emit paint_->layersChanged(); ui::toast(tr("Layer: %1").arg(paint_->active() ? paint_->active()->name : QString())); };
        viewport_->paintPickLayer = [this, cv](const QPointF& uv) { cv->pickLayer(QPointF(uv.x() * paint_->doc.width(), uv.y() * paint_->doc.height())); };
        // (the brush, the fill, any tool that writes — put down on a polygon of another UV set: that set becomes the one shown and painted.
        // The polygons' selection leaves the set as it is)
        viewport_->paintSetSwitch = [this](const QString& set, bool ask) { if (paint_->tool == "select" || paint_->view.startsWith("bake:")) return false;
          if (paintProj_ && paintProj_->would() && (paint_->drawMode == "stroke" || paint_->drawMode == "polyline")) return false;   // (the projected brush writes every set it lies over: none is shown for it)
          const auto t = viewport_->uvSetProps(set); if (t.w <= 0 || t.h <= 0) return false; if (ask) return true;
          paint_->setSet(set, t.w, t.h); ui::hud(tr("UV set: %1").arg(set), 700); return paint_->doc.activeSet() == set; };
        viewport_->paintInput = [this, cv](int phase, const QPointF& uv, bool inv) { cv->strokeTex(phase, uv, inv, viewport_->paintScaleNow(), viewport_->paintPressureNow(), viewport_->paintPenNow()); };   // (the pen's pressure on the model, as on the texture)
        connect(paint_, &PaintSession::brushChanged, this, [this] { viewport_->paintRingPx = paint_->brush.size; });
        viewport_->paintThrough = paint_->drawThrough;
        connect(paint_, &PaintSession::throughChanged, this, [this] { viewport_->paintThrough = paint_->drawThrough; viewport_->paintOverlayChanged(); });
        // (RMB drag over the model: the brush's size sideways, its opacity up / down — the values as they were at the press)
        { auto at0 = std::make_shared<std::pair<float, float>>();
          viewport_->paintAdjust = [this, at0](int ph, const QPointF& d) {
            if (ph == 0) { *at0 = {paint_->brush.size, paint_->brush.hardness}; return; }
            if (ph == 2) { paint_->rememberTool(); QTimer::singleShot(600, this, [] { ui::hud({}); }); return; }
            paint_->brush.size = std::clamp(at0->first * float(std::pow(1.008, d.x())), 1.f, 2048.f);
            if (viewport_->paintAdjHard()) {                       // (the Photoshop / Substance controls: up and down - the hardness)
              paint_->brush.hardness = std::clamp(at0->second - float(d.y()) * 0.004f, 0.f, 1.f); emit paint_->brushChanged();
              ui::hud(tr("Brush size %1 · hardness %2%").arg(int(std::lround(paint_->brush.size))).arg(int(std::lround(paint_->brush.hardness * 100)))); return; }
            emit paint_->brushChanged();                         // (the size alone: up and down does nothing — a slanted drag never changes the opacity by accident)
            ui::hud(tr("Brush size %1").arg(int(std::lround(paint_->brush.size)))); }; }
        viewport_->paintWheelSize = [this](double n) { paint_->brush.size = std::clamp(paint_->brush.size * float(std::pow(1.12, n)), 1.f, 2048.f); emit paint_->brushChanged();
          ui::hud(tr("Brush size %1").arg(int(std::lround(paint_->brush.size))), 600); };
        viewport_->paintAltPick = [this](const QPointF& m) {   // (Alt + click: as V, at the place clicked)
          if (paint_->pickFrom == "screen") { pickTool("edge"); return; }
          const auto uv = viewport_->paintUvAt(m); if (!uv) return;
          const QPointF t(uv->x() * paint_->doc.width(), uv->y() * paint_->doc.height());
          if (const auto c = paint_->pickColor(t, paint_->brush.size / 2 * viewport_->paintScaleNow())) { paint_->color = *c; emit paint_->brushChanged(); showPickLoupe(paint_, t, *c, viewport_->mapToGlobal(m.toPoint())); } };
        viewport_->paintSteadyPx = [this] { return double(paint_->brush.smoothing); };
        cv->uvWire = [this]() -> const std::vector<QPolygonF>* { if (paint_->doc.width() <= 0) return nullptr; viewport_->paintIslands(paint_->doc.activeSet(), paint_->doc.width(), paint_->doc.height()); return &viewport_->paintUvWire(); };
        viewport_->paintHoverUv = [this, cv](std::optional<QPointF> uv) {   // (the brush on the model → its ring on the texture)
          if (!uv) { cv->showRemote(std::nullopt, 0); return; }
          const float r = paint_->brush.size * (viewport_->paintSizeScreen ? viewport_->paintScaleNow() : 1.f) / 2;
          cv->showRemote(QPointF(uv->x() * paint_->doc.width(), uv->y() * paint_->doc.height()), r); };
        cv->pointerTex = [this](std::optional<QPointF> t) {           // (the pointer over the texture → its place on the model)
          if (!t || paint_->doc.width() <= 0) { viewport_->paintRemoteAt(std::nullopt, 0); return; }
          viewport_->paintRemoteAt(QPointF(t->x() / paint_->doc.width(), t->y() / paint_->doc.height()), std::max(4.0, double(paint_->brush.size) * 0.5 / std::max(1e-3f, viewport_->paintScaleNow()))); };
        paint_->islands = [this]() -> const paint::IslandMap* { if (paint_->doc.width() <= 0) return nullptr; return &viewport_->paintIslands(paint_->doc.activeSet(), paint_->doc.width(), paint_->doc.height()); };
        // (projection painting: the stroke laid on the screen and carried into the texture; the stencil, the strip; the view to Photoshop)
        paintProj_ = new PaintProj(paint_, viewport_, cv, vpBox_); paintProj_->on = QSettings().value("paint/projection", true).toBool(); paintLeft_->setProj(paintProj_);
        viewport_->paintProj = [this](int ph, const QPointF& m, bool inv) { return paintProj_->input(ph, m, inv); };
        { auto* tm = new QTimer(this); tm->setInterval(120); auto last = std::make_shared<QByteArray>();
          connect(tm, &QTimer::timeout, this, [this, last] { if (scene_.room != "paint" || !paintProj_->on || QApplication::mouseButtons() != Qt::NoButton || !isActiveWindow()) return;
            const QByteArray k = viewport_->pvViewKey(); if (k == *last) viewport_->pvView(); *last = k; });   // (the same for two ticks: at rest - made if it is not yet)
          tm->start(); }
        // (a UV set the stroke passes over that is not the one shown: what the brush may not touch there — a locked or hidden polygon,
        // an object not among the selected, the set's own freeze mask — kept as it was. A set's mask: made once per state of the locks)
        { struct OG { QImage mask; qint64 key = -1; QImage nearImg; QString nearKey; }; auto og = std::make_shared<std::map<QString, OG>>();
          paintProj_->clipSet = [this, og](const QString& set, QImage& target, const QImage& before, const QRect& r0) {
            if (before.size() != target.size() || before.format() != target.format() || target.depth() != 32) return; const QRect r = r0 & target.rect(); if (r.isEmpty()) return;
            OG& g = (*og)[set]; const qint64 k = viewport_->pvKeyNow();
            if (g.key != k || (!g.mask.isNull() && g.mask.size() != target.size())) { g.key = k; g.mask = viewport_->pvAllowMask(set, target.width(), target.height()); }
            if (!g.mask.isNull() && g.mask.size() == target.size())
              for (int y = r.top(); y <= r.bottom(); y++) { const uchar* m = g.mask.constScanLine(y); QRgb* t = reinterpret_cast<QRgb*>(target.scanLine(y)); const QRgb* b = reinterpret_cast<const QRgb*>(before.constScanLine(y));
                for (int x = r.left(); x <= r.right(); x++) if (!m[x]) t[x] = b[x]; }
            // (near the scalp only: this set's own nearness — a set far from the scalp takes nothing)
            if (paint_->cavity == 3 && !paint_->freezePaint) {
              const QString nk = QString("%1|%2|%3x%4").arg(viewport_->pvKeyNow()).arg(paint_->scalpReach).arg(target.width()).arg(target.height());
              if (g.nearKey != nk) { g.nearKey = nk; g.nearImg = viewport_->pvScalpSets().contains(set) ? viewport_->pvScalpNear(set, target.width(), target.height(), paint_->scalpReach * viewport_->pvSceneSize()) : QImage(); }   // (a set the scalp is not in: the hair is not there — nothing taken)
              std::vector<uchar> hold(size_t(target.width()), 255);
              for (int y = r.top(); y <= r.bottom(); y++) { const uchar* n = g.nearImg.size() == target.size() ? g.nearImg.constScanLine(y) : nullptr;
                for (int x = r.left(); x <= r.right(); x++) hold[size_t(x)] = uchar(255 - (n ? n[x] : 0));
                paint::keepTexels(target, before, y, r.left(), r.right(), hold.data()); } }
            const auto fz = paint_->freeze.constFind(set);
            if (!paint_->freezePaint && fz != paint_->freeze.constEnd() && fz->size() == target.size())
              for (int y = r.top(); y <= r.bottom(); y++) paint::keepTexels(target, before, y, r.left(), r.right(), fz->constScanLine(y)); }; }
        // (before the work goes out to the editor: an autosave copy in the background, when something changed since the last one — a
        // point to go back to if the pass there does not work out; 19.11, recording Cloth_1, 176:05 / 194:55: Ctrl+S of the scene before
        // every Shift+P, 9–12 s each, the author: 3D-Coat falls, a habit, and a point to go back to)
        auto restorePoint = [this] { if (changeTotal() != asLastChange_) autosaveNow(false); };
        paint_->sendProjection = [this, restorePoint] { restorePoint(); ui::toast(paintProj_->sendView()); };
        // (all the layers as a PSD: with the UV polygons drawn as a hidden Wireframe layer on top — what a selection is made by in the editor)
        paint_->sendLayersHook = [this, restorePoint] { restorePoint(); ui::toast(paint_->sendLayers(paintWireImage())); };
        connect(&paint_->bridge, &paint::Bridge::imageReturned, this, [this](const QString& key, const QImage& im) { if (key == "proj") ui::toast(paintProj_->takeBack(im)); });
        connect(&paint_->bridge, &paint::Bridge::layersReturned, this, [this](const QString& path) { if (QFileInfo(path).fileName() == PaintProj::viewFileName()) ui::toast(paintProj_->takeBackFile(path)); });
        // (what the brush may not touch — a locked or hidden polygon, an object not among the selected: its texels kept as they were.
        // The mask is made when a stroke begins, only if the locks / the selection changed since)
        struct Guard { QImage mask, cav; int cavKind = 0; qint64 key = -1; QString set; }; auto g = std::make_shared<Guard>();   // (cavKind: which of the brush's holds cav is — 1 curvature, 3 the scalp, 4 the shine)
        cv->strokeBegan = [this, g] { const qint64 k = viewport_->pvKeyNow(); const QString set = paint_->doc.activeSet(); const QSize sz(paint_->doc.width(), paint_->doc.height());
          // (the curvature the brush is held by: the bake's, at the texture's size)
          if (paint_->cavity == 4) {                                  // (Shine: the highlight as the model is seen at the stroke's start — the light round the view)
            const Vec3 d = viewport_->camera().dir(); const float V[3] = {float(-d.x), float(-d.y), float(-d.z)};
            const double vaz = std::atan2(-d.x, -d.z) * 180 / M_PI, vel = std::asin(std::clamp(-d.y, -1.0, 1.0)) * 180 / M_PI; float L[3]; paint::fxLight(vaz + paint_->shineAz, std::clamp(vel + paint_->shineEl, -89.0, 89.0), L);
            static std::shared_ptr<const paint::FxFrame> fr; static QString fk; const QString key = QString("%1|%2|%3x%4").arg(set).arg(paint_->doc.geomRevOf ? paint_->doc.geomRevOf(set) : 0).arg(sz.width()).arg(sz.height());
            if (key != fk || !fr) { const float *P = nullptr, *N = nullptr; fr.reset(); fk = key;
              if (paint_->doc.positionsOf && paint_->doc.positionsOf(set, sz.width(), sz.height(), P, N) && P && N) fr = paint::fxFrame(sz.width(), sz.height(), P, N, L); }
            g->cav = fr ? paint::fxShineMap(*fr, L, V, paint_->shineRough) : QImage(); g->cavKind = 4; if (!fr) ui::toast(tr("Shine: the surface under the texture is not known here")); }
          else if (paint_->cavity == 3) {                                  // (near the scalp: the nearness made for this set and reach, kept while they stay)
            static QString nk; const QString key = QString("%1|%2|%3|%4x%5").arg(set).arg(viewport_->pvKeyNow()).arg(paint_->scalpReach).arg(sz.width()).arg(sz.height());
            if (key != nk || g->cav.size() != sz || g->cavKind != 3) { nk = key; g->cavKind = 3; g->cav = viewport_->pvScalpNear(set, sz.width(), sz.height(), paint_->scalpReach * viewport_->pvSceneSize());
              if (!g->cav.isNull() && !set.isEmpty() && !viewport_->pvScalpSets().contains(set)) g->cav.fill(0);   // (a set the scalp is not in: nothing taken)
              if (g->cav.isNull()) ui::toast(tr("Near the scalp: mark the scalp first — select its polygons (Select tool) → the Freeze menu → Mark as the scalp")); } }
          else if (!paint_->cavity) g->cav = {}; else if (g->cav.size() != sz || g->cavKind != 1) { g->cavKind = 1; const QImage c = paint_->baked.value("curvature"); if (c.isNull()) { g->cav = {}; ui::toast(tr("The brush's Hollows / Ridges need the curvature: bake it first (Bake → Curvature)")); } else g->cav = c.scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8); }
          if (k == g->key && set == g->set && (g->mask.isNull() || g->mask.size() == sz)) return;
          g->key = k; g->set = set; g->mask = viewport_->pvAllowMask(set, sz.width(), sz.height()); };
        cv->clip = [this, g](QImage& target, const QImage& before, const QRect& r0) {
          if (before.size() != target.size() || before.format() != target.format()) return;
          const QRect r = r0 & target.rect(); const int bpp = target.depth() / 8; if (bpp < 1) return;
          if (!g->mask.isNull() && g->mask.size() == target.size())
            for (int y = r.top(); y <= r.bottom(); y++) { const uchar* m = g->mask.constScanLine(y); uchar* t = target.scanLine(y); const uchar* b = before.constScanLine(y);
              for (int x = r.left(); x <= r.right(); x++) if (!m[x]) std::memcpy(t + x * bpp, b + x * bpp, size_t(bpp)); }
          // (the freeze mask: a frozen texel as it was, a gray one — the stroke that much weaker; not while the mask itself is painted)
          const QImage* fz = paint_->freezePaint ? nullptr : paint_->freezeOf(false); if (fz && fz->size() != target.size()) fz = nullptr;
          const QImage* cav = !paint_->freezePaint && paint_->cavity && g->cav.size() == target.size() ? &g->cav : nullptr; if (!fz && !cav) return;
          // (how much of each texel is held back: the mask's own row where nothing else holds; colour mixed premultiplied — paint::keepTexels —
          // so an erase under a half-frozen texel leaves the colour half as dense, no darker edge, no single holes)
          const int cmode = paint_->cavity; std::vector<uchar> hold;
          for (int y = r.top(); y <= r.bottom(); y++) { const uchar* m = fz ? fz->constScanLine(y) : nullptr; const uchar* cr = cav ? cav->constScanLine(y) : nullptr;
            if (!cr) { paint::keepTexels(target, before, y, r.left(), r.right(), m); continue; }
            hold.assign(size_t(target.width()), 0);
            for (int x = r.left(); x <= r.right(); x++) { const int open = cmode >= 3 ? int(cr[x]) : std::clamp((cmode == 1 ? 128 - cr[x] : cr[x] - 128) * 6, 0, 255); hold[size_t(x)] = uchar(std::max(m ? int(m[x]) : 0, 255 - open)); }   // (128 — flat; darker — a hollow, lighter — a ridge)
            paint::keepTexels(target, before, y, r.left(), r.right(), hold.data()); } }; } }
    paint_->ensureDoc(paint_->bakeS.size);
    // (a project not painted yet whose model has UV sets: its first set shown, not the single texture nothing of it lies in)
    if (paint_->doc.activeSet().isEmpty() && paint_->undo.empty() && paint_->texIn.isEmpty() && paint_->doc.sets().size() <= 1) { const auto names = viewport_->uvSetNamesAll(); if (!names.empty()) { const auto t = viewport_->uvSetProps(names[0]); paint_->setSet(names[0], t.w, t.h); } }
    paintImportTexture(); paintImportAll(); paintOthersRefresh(); emit paint_->viewChanged(); paintLeft_->refresh(); }
  else { paintLeft_->setFull(false); if (scene_.paintView.on) { scene_.paintView.on = false; scene_.paintView.flat = {}; viewport_->paintOnModel = false; viewport_->paintViewChanged(); } }
  if (topo) { ttools_->place(vpBox_); ttools_->show(); ttools_->raise(); ttools_->sync(); } else ttools_->hide();
  if (hairR) { htools_->place(vpBox_); htools_->show(); htools_->raise(); } else htools_->hide();
  syncUvTools();
  if (olHead_) { olHead_->setVisible(!pnt); olHead_->setFixedWidth(outliner_->paneWidth() + (topo ? 45 : 0)); }
  if (dispTopo_) dispTopo_->hide(); if (dispStrip_) dispStrip_->hide();
  tmodes_->sync(); ttab_->sync(true); syncUnhide(); if (insp_) insp_->sync();
  rebuildOutliner(); syncPanels();
}

QString MainWindow::thumbDataUrl() {
  if (!viewport_->supportsGrab() || !viewport_->isExposed()) return {};
  QImage img = viewport_->grab();
  if (img.isNull()) return {};
  if (viewport_->colorFormat() == VK_FORMAT_B8G8R8A8_SRGB) img = img.rgbSwapped();
  int s = int(std::min(img.width(), img.height()) * 0.9);
  QImage sq = img.copy((img.width() - s) / 2, (img.height() - s) / 2, s, s).scaled(240, 240, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  QByteArray jpg; QBuffer buf(&jpg); buf.open(QIODevice::WriteOnly);
  sq.convertToFormat(QImage::Format_RGB888).save(&buf, "JPEG", 80);
  return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(jpg.toBase64());
}

// A save still being written in the background (Ctrl+S): waited for — before another save, another project or the
// program's end. Opened / closed meanwhile, its end stamped the old file's path and "saved" mark on the new project, or
// never came; a second save wrote the same <name>.saving at the same time
void MainWindow::waitSave() {
  if (!saveBusy_) return;
  QElapsedTimer t; t.start();
  while (saveBusy_ && t.elapsed() < 180000) QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
}

bool MainWindow::unsavedOk() {
  waitSave();
  if (forget_ || !dirty()) return true;
  auto r = QMessageBox::question(this, tr("Unsaved changes"), tr("%1 has unsaved changes. Save them?").arg(displayName()),
                                 QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
  if (r == QMessageBox::Save) return saveProject(false);
  // (Discard: the changes are given up on purpose — their recovery copy too. Kept, it was offered at the next start as work lost
  // in a crash: recording Dress_3, 12:11 Discard → 123:16 "Restore unsaved work?")
  if (r == QMessageBox::Discard) { clearRecovery(); return true; }
  return false;
}

bool MainWindow::saveViewportShot(const QString& png) {
  if (!viewport_->supportsGrab()) { qWarning("the swapchain can't be read back"); return false; }
  QImage img = viewport_->grab();
  // (grab knows B8G8R8A8_UNORM's channel order, not the sRGB variant we render into: red and blue come out swapped)
  if (viewport_->colorFormat() == VK_FORMAT_B8G8R8A8_SRGB) img = img.rgbSwapped();
  return !img.isNull() && img.save(png);
}

// the whole window: the widgets, the viewport's frame put in its place
bool MainWindow::saveWindowShot(const QString& png, bool cursor) { const QImage im = windowShot(cursor); return !im.isNull() && im.save(png); }
QImage MainWindow::windowShot(bool cursor) {
  QPixmap pm = grab();
  QImage vp = viewport_->grab(); if (viewport_->colorFormat() == VK_FORMAT_B8G8R8A8_SRGB) vp = vp.rgbSwapped();
  QPainter g(&pm); QRect r(vpBox_->mapTo(this, QPoint(0, 0)), vpBox_->size());
  if (!vp.isNull()) g.drawImage(r, vp);
  // (the app's own floating windows over it — the Topology tools, panels, the Inspector: drawn where they are)
  const QPoint o = mapToGlobal(QPoint(0, 0));
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (w == this || !w->isVisible() || w->isMinimized() || w->inherits("QMenu") || w->windowType() == Qt::ToolTip || w->windowType() == Qt::Popup) continue;
    const QRect at(w->mapToGlobal(QPoint(0, 0)) - o, w->size()); if (!at.intersects(pm.rect())) continue;
    g.drawPixmap(at.topLeft(), w->grab());
  }
  // (the pointer: an arrow where it is)
  if (cursor) { const QPoint c = QCursor::pos() - o;
    if (pm.rect().contains(c)) { QPolygonF a; for (auto [x, y] : std::initializer_list<std::pair<double, double>>{{0, 0}, {0, 17}, {4.5, 13}, {8, 20}, {10.5, 19}, {7, 12}, {12.5, 12}}) a << QPointF(c.x() + x, c.y() + y);
      g.setRenderHint(QPainter::Antialiasing); g.setPen(QPen(Qt::black, 1.2)); g.setBrush(Qt::white); g.drawPolygon(a); } }
  g.end();
  return pm.toImage();
}

void MainWindow::resetView() { viewport_->camera().reset(); viewport_->requestUpdate(); }
void MainWindow::setView(const QString& v) { viewport_->setViewMode(v); }
void MainWindow::selectItems(const std::set<int>& ids) { viewport_->setSelectedItems(std::vector<int>(ids.begin(), ids.end())); }

// Checks from the command line (--events): "press P [shift|ctrl|alt|right]; move P; release P" (P — "x,y" in logical
// px, "a<anchor>[+dx+dy]" — where that anchor is on screen, "m<a>_<b>" — half way between two anchors), undo, redo,
// weld=<choice>, frame, convert, check, autofix, fix <i> before|after|accept|reject, fixall accept|reject,
// lockweld=0|1, ignore <i>, tool <name>, action <id>, save <path>, winshot <png>
QString MainWindow::runEvents(const QString& script) {
  QStringList log; QElapsedTimer clock_; clock_.start();   // (clock: the ms since the script began — what took long)
  auto pointOf = [&](const QString& spec) -> std::optional<QPointF> {
    if (spec.startsWith("ue")) {                                // ue<edge>_<side 0|1>: the UV editor's edge, on that side's polygon (a little into it) — on the screen
      const QStringList p = spec.mid(2).split('_'); const int m = viewport_->topoMesh(); const Item* it = m >= 0 ? scene_.item(m) : nullptr; if (!it || p.size() != 2) return std::nullopt;
      const PolyMesh& B = *(it->base ? it->base : it->poly); auto T = topo::ttopo(it->base ? it->base : it->poly); const int e = p[0].toInt(); if (e < 0 || e >= T->ne) return std::nullopt;
      const int f = p[1].toInt() == 0 ? T->ef0[size_t(e)] : T->ef1[size_t(e)]; if (f < 0) return std::nullopt;
      const int ka = uv::cornerAt(*T, f, T->ea[size_t(e)]), kb = uv::cornerAt(*T, f, T->eb[size_t(e)]); if (ka < 0 || kb < 0) return std::nullopt;
      double cu = 0, cv = 0; int n = 0; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { cu += B.cornerUV[size_t(k)][0]; cv += B.cornerUV[size_t(k)][1]; n++; } cu /= n; cv /= n;
      const double mu = (B.cornerUV[size_t(ka)][0] + B.cornerUV[size_t(kb)][0]) / 2.0, mv = (B.cornerUV[size_t(ka)][1] + B.cornerUV[size_t(kb)][1]) / 2.0;
      return viewport_->uvPxOf(mu + (cu - mu) * 0.08, mv + (cv - mv) * 0.08); }
    if (spec.startsWith('m')) {
      QStringList p = spec.mid(1).split('_'); if (p.size() != 2) return std::nullopt;
      const Anchor *a = scene_.anchor(p[0].toInt()), *b = scene_.anchor(p[1].toInt()); if (!a || !b) return std::nullopt;
      Projector P(viewport_->camera(), viewport_->width(), viewport_->height()); ScreenPt s = P.toScreen(a->pos), t = P.toScreen(b->pos);
      return QPointF((s.x + t.x) / 2, (s.y + t.y) / 2);
    }
    if (spec.startsWith('a')) {
      QStringList p = spec.mid(1).split('+');
      const Anchor* a = scene_.anchor(p[0].toInt()); if (!a) return std::nullopt;
      ScreenPt s = Projector(viewport_->camera(), viewport_->width(), viewport_->height()).toScreen(a->pos);
      return QPointF(s.x + (p.size() > 1 ? p[1].toDouble() : 0), s.y + (p.size() > 2 ? p[2].toDouble() : 0));
    }
    if (spec.startsWith('g')) {                              // g[+dx+dy]: from the gizmo's centre
      auto g = viewport_->gizmoScreen(); if (!g) return std::nullopt;
      QStringList p = spec.mid(1).split('+', Qt::SkipEmptyParts);
      return QPointF(g->x() + (p.size() > 0 ? p[0].toDouble() : 0), g->y() + (p.size() > 1 ? p[1].toDouble() : 0));
    }
    if (spec == "vc") return QPointF(viewport_->width() / 2.0, viewport_->height() / 2.0);   // (the viewport's middle)
    QStringList xy = spec.split(','); if (xy.size() != 2) return std::nullopt;
    return QPointF(xy[0].toDouble(), xy[1].toDouble());
  };
  Qt::MouseButtons held;
  // (the question about content of another scale: no window in a check — kept as it is unless "fitans" says otherwise)
  if (!fitScaleHook) fitScaleHook = [](double) -> std::optional<double> { return std::nullopt; };
  for (const QString& cmd : script.split(';', Qt::SkipEmptyParts)) {
    QStringList w = cmd.trimmed().split(' ', Qt::SkipEmptyParts);
    if (w.isEmpty()) continue;
    if (w.size() == 2 && w[0] == "fitans") {                    // fitans opt|keep|<k>: the answer to the next "another scale" questions
      const QString a = w[1]; fitScaleHook = [a](double opt) -> std::optional<double> { if (a == "keep") return std::nullopt; if (a == "opt") return opt; return a.toDouble(); };
      log << "fitans " + a; continue; }
    if (w.size() == 2 && w[0] == "fitwin") {                    // fitwin <png>: the "another scale" window as it opens (hair 1900 times bigger), shot, then closed
      auto keepHook = fitScaleHook; fitScaleHook = nullptr;
      QTimer::singleShot(600, this, [png = w[1]] { if (auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget())) { d->grab().save(png); d->reject(); } });
      askFitScale(this, "RightTail_01", 1900, 1.0 / 1900); fitScaleHook = keepHook; log << "fitwin " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "drawmode") { viewport_->drawMode = w[1]; log << "drawmode " + w[1]; continue; }   // drawmode curve|lock|…: the Curve tool's mode
    if (w.size() == 2 && w[0] == "asel") { std::vector<int> a; for (const QString& x : w[1].split(',')) a.push_back(x.toInt()); viewport_->setSelectedAnchors(a); log << "asel " + w[1]; continue; }   // asel <a,b,…>: anchors selected
    if (w.size() == 2 && w[0] == "bend") { QStringList o; double tot = 0;   // bend <ids>: the total turn along each curve's samples (degrees) — waves add to it
      for (const QString& x : w[1].split(',')) { const auto& cc = viewport_->hair().curves; auto f = cc.find(x.toInt()); if (f == cc.end()) continue; const auto& P = f->second.p; double t = 0;
        for (size_t i = 1; i + 1 < P.size(); i++) { const Vec3 a = P[i] - P[i - 1], b = P[i + 1] - P[i]; const double la = a.length(), lb = b.length(); if (la > 0 && lb > 0) t += std::acos(std::clamp(a.dot(b) / (la * lb), -1.0, 1.0)); }
        t *= 180 / M_PI; tot += t; o << QString("%1:%2").arg(x).arg(t, 0, 'f', 0); }
      log << QString("bend total %1 · %2").arg(tot, 0, 'f', 0).arg(o.join(' ')); continue; }
    if (w.size() == 2 && w[0] == "brushrel") { hm::brushFreesHandles =w[1] == "1"; log << "brushrel " + w[1]; continue; }   // (checks: the brush frees frozen handles or not)
    if (w.size() == 3 && w[0] == "floorop") { auto F = viewport_->paneFloor(w[1].toInt()); F.op = w[2].toDouble(); viewport_->setPaneFloor(w[1].toInt(), F); viewport_->beforeFrame(); log << "floorop " + w[1] + " " + w[2]; continue; }   // (the window's grid button)
    if (w.size() == 1 && w[0] == "ovrn") { int n = 0, k = 0; for (auto& [id, it] : scene_.items) if (it.type == ItemType::Curve && !it.ovr.empty()) { n++; k += int(it.ovr.size()); } log << QString("ovrn: %1 curves with frozen handles, %2 handles").arg(n).arg(k); continue; }
    if (w.size() == 3 && w[0] == "ribdetach") { setRibDetached(viewport_, w[1].toInt(), w[2] == "1"); viewport_->beforeFrame(); const Item* k = scene_.item(w[1].toInt()); const Item* p = k ? scene_.item(k->parent) : nullptr;
      log << QString("ribdetach %1 %2: parent %3 '%4' (%5 children) · %6").arg(w[1], w[2]).arg(k ? k->parent : -2).arg(p ? p->name : QString()).arg(p ? p->children.size() : 0).arg(ui::lastToast()); continue; }   // ribdetach <lock> 0|1
    if (w.size() == 3 && w[0] == "ribroot") { setRibRootDir(viewport_, w[1].toInt(), w[2]); viewport_->beforeFrame(); log << "ribroot " + w[1] + " " + w[2]; continue; }   // ribroot <lock> along|flip|free
    if (w.size() == 1 && w[0] == "fitlast") { log << "fit asked: " + (fitScaleAsked.isEmpty() ? QString("nothing") : fitScaleAsked); fitScaleAsked.clear(); continue; }
    if (w.size() == 1 && (w[0] == "undo" || w[0] == "redo")) { runAction(w[0]); viewport_->beforeFrame(); log << w[0]; continue; }
    if (w.size() == 1 && w[0].startsWith("weld=")) { viewport_->weldChoice = w[0].mid(5); continue; }
    if (w.size() == 1 && w[0] == "frame") { viewport_->frameSelection(); continue; }
    if (w.size() == 1 && w[0] == "selcocoons") { std::vector<int> ids; for (auto& [id, it] : scene_.items) if (it.type == ItemType::Cocoon) ids.push_back(id); viewport_->setSelectedItems(ids); log << QString("selcocoons %1").arg(ids.size()); continue; }
    if (w.size() >= 2 && w[0] == "lockfix") {                // lockfix <mode 0 polypaint|1 hard|2 creases> [cocoon 0|1] [contrast]: the Lock lines tool on the last imported mesh
      int mm = -1; for (auto& [id, it] : scene_.items) if (it.type == ItemType::IMesh && it.poly) mm = id;
      viewport_->llOpt.mode = locklines::Mode(std::clamp(w[1].toInt(), 0, 2)); viewport_->llCocoon = w.size() > 2 && w[2] == "1"; if (w.size() > 3) viewport_->llOpt.contrast = w[3].toDouble();
      const int c0 = int(std::count_if(scene_.items.begin(), scene_.items.end(), [](auto& kv) { return kv.second.type == ItemType::Curve; }));
      viewport_->lockLinesRun(mm); viewport_->beforeFrame();
      int nc = 0, nk = 0; for (auto& [id, it] : scene_.items) { if (it.type == ItemType::Curve) nc++; if (it.type == ItemType::Cocoon) nk++; }
      log << QString("lockfix mesh %1: curves %2 → %3, cocoons %4, toast \"%5\"").arg(mm).arg(c0).arg(nc).arg(nk).arg(ui::lastToast()); continue; }
    if (w.size() == 1 && w[0] == "convert") { viewport_->convertSelected(); viewport_->beforeFrame(); log << "convert: " + ui::lastToast(); continue; }
    if (w.size() == 1 && w[0] == "hairq") {   // (the converted meshes: polygons, open edges, edges of 3+ polygons, polygroups)
      for (auto& [id, it] : scene_.items) { if (!it.raw.contains("srcSurfs") || !it.poly) continue; const PolyMesh& M = it.base ? *it.base : *it.poly; std::map<std::pair<int, int>, int> ec;
        for (int f = 0; f < M.faceCount(); f++) { const int s = M.faceStart[size_t(f)], e = M.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) { int a = M.faceVerts[size_t(k)], b = M.faceVerts[size_t(k + 1 < e ? k + 1 : s)]; ec[{std::min(a, b), std::max(a, b)}]++; } }
        int op = 0, nm = 0; for (auto& [k, c] : ec) { if (c == 1) op++; if (c > 2) nm++; }
        log << QString("hairq %1 \"%2\" %3: faces %4 open %5 nonmanifold %6 groups %7").arg(id).arg(it.name).arg(it.typeStr).arg(M.faceCount()).arg(op).arg(nm).arg(M.groupNames.size()); }
      continue; }
    if (w.size() == 1 && w[0] == "check") { auto lw = viewport_->checkConnections(true); log << QString("check: %1 places").arg(lw ? int(lw->problems.size()) : -1);
      if (lw) { for (auto& p : lw->problems) { QStringList ks; for (auto& e : p.edges) ks << e.kind; ks.removeDuplicates(); QStringList ls; for (int l : p.locks) ls << QString::number(l);
          log << QString("  place (%1, %2, %3) locks %4 kinds %5: %6").arg(p.c.x, 0, 'f', 0).arg(p.c.y, 0, 'f', 0).arg(p.c.z, 0, 'f', 0).arg(ls.join(',')).arg(ks.join(',')).arg(weldIssueText(scene_, viewport_->hair(), p)); }
        log << QString("  marked correct: %1, merged %2, double walls %3, inside %4, filled %5, open %6, meshes %7").arg(lw->accepted.size()).arg(lw->merged).arg(lw->doubled).arg(lw->inside).arg(lw->filled).arg(lw->open).arg(lw->groups.size()); }
      continue; }
    if (w.size() == 1 && w[0] == "autofix") { report_->runAutoFix(); log << QString("autofix: %1 fixes").arg(viewport_->fixes().size()); continue; }
    if (w.size() == 3 && w[0] == "fix") {
      int i = w[1].toInt();
      if (w[2] == "before" || w[2] == "after") viewport_->fixShow(i, w[2] == "after"); else viewport_->fixDecide(i, w[2] == "accept");
      log << QString("fix %1 %2: %3 places").arg(i).arg(w[2]).arg(std::count_if(viewport_->weldSpots().begin(), viewport_->weldSpots().end(), [](auto& p) { return !p.ok && p.fix < 0; })); continue;
    }
    if (w.size() == 2 && w[0] == "fixall") { viewport_->fixAll(w[1] == "accept"); log << "fixall " + w[1]; continue; }
    if (w.size() == 1 && w[0].startsWith("lockweld=")) { viewport_->setWeldSetting("lockWeld", w[0].mid(9) == "1"); continue; }
    if (w.size() == 2 && w[0] == "ignore") {
      int i = w[1].toInt(); if (i < 0 || i >= int(viewport_->weldSpots().size())) { log << "no place " + w[1]; continue; }
      WeldProblem sp = viewport_->weldSpots()[size_t(i)]; weldSpotSet(scene_, viewport_->hair(), sp, !sp.ok);
      viewport_->setWeldSetting("weldOk", scene_.fileRaw.value("weldOk"));
      auto lw = viewport_->checkConnections(false); log << QString("ignore %1: %2 places left").arg(i).arg(lw ? int(lw->problems.size()) : -1); continue;
    }
    // ol press|move|release|dbl r<id>[+dx] [shift|ctrl] — on an outliner row (dx: from its left edge; none — its name)
    if (w.size() >= 3 && w[0] == "ol") {
      QString sp = w[2].mid(1); int cut = int(sp.indexOf(QRegularExpression("[+-]"))); QPoint at;
      OutlinerView* v = outliner_->rowAt((cut < 0 ? sp : sp.left(cut)).toInt(), &at, cut < 0 ? INT_MIN : sp.mid(cut).toInt());
      if (!v) { log << "no row " + w[2]; continue; }
      Qt::KeyboardModifiers mods; if (w.contains("shift")) mods |= Qt::ShiftModifier; if (w.contains("ctrl")) mods |= Qt::ControlModifier; if (w.contains("alt")) mods |= Qt::AltModifier;
      QEvent::Type t = w[1] == "press" ? QEvent::MouseButtonPress : w[1] == "release" ? QEvent::MouseButtonRelease : w[1] == "dbl" ? QEvent::MouseButtonDblClick : QEvent::MouseMove;
      QMouseEvent ev(t, QPointF(at), v->mapToGlobal(QPointF(at)), t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
      QCoreApplication::sendEvent(v, &ev); QApplication::processEvents();
      log << QString("ol %1 %2 at %3,%4").arg(w[1], w[2]).arg(at.x()).arg(at.y()); continue;
    }
    if (w.size() >= 2 && w[0] == "shell") { viewport_->shTab = w[1]; viewport_->tipBrush.on = w[1] == "tip" && w.contains("on"); viewport_->pinfBrush.on = w[1] == "pinf" && w.contains("on"); viewport_->shellChanged(); syncPanels(); log << "shell " + w[1]; continue; }
    // cutc <curve> <seg> <u> <mode>[,<curve> <seg> <u>…]: the scissors' cut with the hits given (checks)
    if (w.size() >= 5 && w[0] == "cutc") {
      std::vector<cut::Hit> hits; for (int k = 1; k + 2 <= int(w.size()) - 2; k += 3) hits.push_back({w[k].toInt(), w[k + 1].toInt(), w[k + 2].toDouble()});
      QString mode = w.last();
      viewport_->runEdit([hits, mode](Scene& s, Selection& sl, const CurveCache& cc) { return cut::cutCurves(s, cc, sl, hits, mode); }, true);
      viewport_->beforeFrame(); log << QString("cut %1 hits %2").arg(hits.size()).arg(mode); continue;
    }
    if (w.size() == 2 && w[0] == "cutmode") { viewport_->cutMode = w[1]; if (w[1] == "surface" && viewport_->selection().items.size() == 1) viewport_->startSurfCut(viewport_->selection().items[0]); syncPanels(); log << "cutmode " + w[1]; continue; }
    if (w.size() == 1 && w[0] == "surfapply") { log << (viewport_->surfCutReady() ? "surface cut ready" : "surface cut NOT ready"); viewport_->applySurfCut(); viewport_->beforeFrame(); continue; }
    if (w.size() == 2 && w[0] == "sepx") { viewport_->runEdit([g = w[1].toInt()](Scene& s, Selection& sl, const CurveCache&) { return cut::separateGroupX(s, sl, g); }, true); viewport_->beforeFrame(); log << "sepx " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "opt") { std::vector<int> ids; for (const QString& x : w[1].split(",")) ids.push_back(x.toInt());
      viewport_->runEdit([ids](Scene& s, Selection& sl, const CurveCache& c) { sl.anchors.clear(); return aops::optimizeAnchors(s, c, ids, nullptr); }, true); viewport_->beforeFrame(); log << "opt " + w[1]; continue; }
    if (w.size() >= 2 && w[0] == "modapply") { std::vector<int> ids; for (const QString& x : w[1].split(",")) ids.push_back(x.toInt());
      const auto pp = tube::profileByName(w.size() > 2 ? w[2] : "Quad"); if (!pp) { log << "modapply: no such profile " + w.value(2); continue; } const auto prof = *pp;
      viewport_->runEdit([ids, prof](Scene& s, Selection& sl, const CurveCache& c) { sl.items = ids; sl.anchors.clear(); return tube::applyModifier(s, c, sl, prof); }, true); viewport_->beforeFrame();
      log << "modapply " + w[1]; continue; }
    if (w.size() == 3 && w[0] == "xadd") { int t = w[1].toInt(); QString k = w[2];
      viewport_->runEdit([t, k](Scene& s, Selection& sl, const CurveCache& c) { sl.items = {t}; return tube::addExtra(s, c, sl, k); }, true); viewport_->beforeFrame(); log << "xadd " + w[1] + " " + k; continue; }
    if (w.size() == 3 && w[0] == "viewset") { const QJsonValue v = w[2] == "true" ? QJsonValue(true) : w[2] == "false" ? QJsonValue(false) : QJsonValue(w[2].toDouble()); setViewSetting(w[1], v); viewport_->beforeFrame(); log << "viewset " + w[1] + " " + w[2]; continue; }   // (viewset key true|false|number — a Display setting)
    if (w.size() >= 3 && w[0] == "xset") {           // xset <item> key=value,key=value (numbers into raw p)
      int id = w[1].toInt(); QString kv = w.mid(2).join(" ");
      viewport_->runEdit([id, kv](Scene& s, Selection&, const CurveCache&) { Item* it = s.item(id); QJsonObject p = it->raw.value("p").toObject();
        for (const QString& e : kv.split(",")) { auto pr = e.split("="); if (pr.size() == 2) { bool ok; double v = pr[1].toDouble(&ok); p.insert(pr[0], ok ? QJsonValue(v) : QJsonValue(pr[1] == "true")); } }
        it->raw.insert("p", p); return QString(); }, false); viewport_->liveTubes(); if (const Item* x = scene_.item(id); x && x->type == ItemType::Surface) viewport_->liveWalls({id}); viewport_->beforeFrame(); log << "xset " + w[1]; continue; }
    if (w.size() == 3 && w[0] == "xmasksin") {        // a test mask: sin(i / 10) × k
      int id = w[1].toInt(); double k = w[2].toDouble();
      viewport_->runEdit([id, k](Scene& s, Selection&, const CurveCache&) { QJsonArray a; for (int i = 0; i < tube::MASK_N; i++) a.append(std::sin(i / 10.0) * k); s.item(id)->raw.insert("mask", a); return QString(); }, false);
      viewport_->liveTubes(); viewport_->beforeFrame(); log << "xmasksin " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "modconv") { int t = w[1].toInt(); Vec3 c = viewport_->camera().sceneCenter;
      viewport_->runEdit([t, c](Scene& s, Selection& sl, const CurveCache& cc) { return tube::convertTube(s, cc, sl, t, c); }, true); viewport_->beforeFrame(); log << "modconv " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "tubedump") { viewport_->beforeFrame(); auto it = viewport_->tubes().find(w[1].toInt());
      if (it == viewport_->tubes().end()) { log << "tubedump: none"; continue; }
      QStringList L; L << QString("tube %1: NR %2 n %3").arg(w[1]).arg(it->second.NR).arg(it->second.n);
      for (int i : {0, it->second.NR / 3, it->second.NR - 1}) { QStringList r; for (int j = 0; j < it->second.n; j++) { const Vec3& p = it->second.pos[size_t(i * it->second.n + j)]; r << QString("%1,%2,%3").arg(p.x, 0, 'f', 3).arg(p.y, 0, 'f', 3).arg(p.z, 0, 'f', 3); } L << QString("ring %1: %2").arg(i).arg(r.join(" ")); }
      log << L.join("\n"); continue; }
    if (w.size() >= 2 && (w[0] == "import" || w[0] == "importhead")) { importFiles(w.mid(1).join(' ').split('|'), w[0] == "importhead", viewport_, &scene_); viewport_->beforeFrame(); log << w[0]; continue; }
    if (w.size() >= 3 && w[0] == "exportf") {         // exportf <fmt> <path> — every visible mesh
      auto list = exportMeshes(scene_, {}, "all"); QFile f(w.mid(2).join(' ')); bool ok = f.open(QIODevice::WriteOnly) && f.write(exportText(w[1] == "fbxg" ? QString("fbx") : w[1], list, 1, w[1] == "fbxg")) >= 0;
      log << QString("exportf %1: %2 meshes %3").arg(w[1]).arg(list.size()).arg(ok ? "ok" : "FAILED"); continue; }
    if (w.size() == 1 && w[0] == "fbxcheck") {        // a cube through the FBX writer and back through ufbx
      PolyMesh c; c.pos = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}};
      const int F[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
      for (auto& q : F) c.addFace(q, 4); for (size_t k = 0; k < c.faceVerts.size(); k++) c.cornerUV.push_back({float(k % 4) * 0.25f, float(k / 4) * 0.1f});
      c.hardEdges = {{4, 5}, {5, 6}, {6, 7}, {7, 4}}; c.creases = {{0, 1, 0.5f}};
      for (int pass = 0; pass < 3; pass++) {
        PolyMesh m = c; if (pass == 2) for (size_t k = 0; k < m.faceVerts.size(); k++) { Vec3 n = Vec3(double(k), 1, -double(k % 3)).normalized(); m.cornerNormal.push_back({float(n.x), float(n.y), float(n.z)}); }
        QString err; auto r = parseFbx(exportFbx({{"Cube", &m}}, 1, pass == 1), &err);
        if (!r) { log << "fbxcheck: read failed " + err; continue; }
        double dn = 0; QStringList top; for (int k = 4; k < 8; k++) { auto& n = r->cornerNormal[size_t(k)]; top << QString("%1,%2,%3").arg(n[0], 0, 'f', 2).arg(n[1], 0, 'f', 2).arg(n[2], 0, 'f', 2); }
        if (pass == 2) for (size_t k = 0; k < m.cornerNormal.size() && k < r->cornerNormal.size(); k++) for (int i = 0; i < 3; i++) dn = std::max(dn, double(std::abs(m.cornerNormal[k][size_t(i)] - r->cornerNormal[k][size_t(i)])));
        double du = 0; for (size_t k = 0; k < c.cornerUV.size() && k < r->cornerUV.size(); k++) for (int i = 0; i < 2; i++) du = std::max(du, double(std::abs(c.cornerUV[k][size_t(i)] - r->cornerUV[k][size_t(i)])));
        QStringList cr; for (auto& e : r->creases) cr << QString("%1-%2:%3").arg(e.a).arg(e.b).arg(e.w);
        log << QString("fbxcheck %1: v %2 f %3 c %4 hard %5 creases [%6] uv %7 du %8 top normals %9 dn %10").arg(pass == 0 ? "edges" : pass == 1 ? "groups" : "custom")
                 .arg(r->pos.size()).arg(r->faceCount()).arg(r->faceVerts.size()).arg(r->hardEdges.size()).arg(cr.join(" ")).arg(r->cornerUV.size()).arg(du).arg(top.join(" ")).arg(dn);
      }
      continue; }
    if (w.size() >= 2 && w[0] == "migrate") { bool ok = migrateFile(w.mid(1).join(' '), QString(), true); viewport_->beforeFrame();
      log << QString("migrate %1: items %2 anchors %3").arg(ok ? "ok" : "FAILED").arg(scene_.items.size()).arg(scene_.anchors.size()); continue; }
    if (w.size() >= 2 && w[0] == "split") { viewport_->setSplit(w[1] == "1"); if (w.size() > 2) viewport_->setSplitLayout(w[2]); viewport_->requestUpdate(); log << "split " + w.mid(1).join(' '); continue; }
    if (w.size() == 3 && w[0] == "paneview") { viewport_->setPaneView(w[1].toInt(), w[2]); log << "paneview " + w[1] + " " + w[2]; continue; }
    if (w.size() == 4 && w[0] == "panewire") { QJsonArray a = scene_.fileRaw.value("settings").toObject().value("view").toObject().value("paneWire").toArray(); while (a.size() < 4) a.append(QJsonArray{false, false}); a[w[1].toInt() & 3] = QJsonArray{w[2] == "1", w[3] == "1"}; setViewSetting("paneWire", a); QElapsedTimer t; t.start(); while (t.elapsed() < 200) QApplication::processEvents(); log << "panewire " + w.mid(1).join(QChar(32)); continue; }   // panewire <pane> <only 0|1> <wire 0|1>: a view.s wireframe (checks)
    if (w.size() == 2 && w[0] == "pane") { viewport_->activatePane(w[1].toInt()); log << QString("pane %1: %2").arg(w[1]).arg(viewport_->camera().view); continue; }
    if (w.size() >= 3 && w[0] == "refplane") {        // refplane <view> <image> — a plane with a picture, behind the model
      QString url; int iw = 0, ih = 0; bool ok = refImageFromFile(w.mid(2).join(' '), &url, &iw, &ih);
      RefPlane rp; rp.id = scene_.nextRefId++; rp.view = w[1]; rp.name = refs::viewLabel(w[1]) + " 1"; rp.lock = QJsonValue(QJsonValue::Null);
      refs::place(rp, scene_, viewport_->refHomeCamera(rp));
      if (ok) { double a = double(iw) / ih; rp.img = url; rp.scl.x = rp.scl.y * a; rp.aspect = a; refs::capture(rp, viewport_->refHomeCamera(rp)); }
      scene_.refPlanes.push_back(rp); viewport_->refPlanesEdited(true); viewport_->beforeFrame();
      log << QString("refplane %1: image %2 (%3×%4), pos %5 %6 %7, scale %8 %9").arg(w[1]).arg(ok ? "ok" : "FAILED").arg(iw).arg(ih).arg(rp.pos.x, 0, 'f', 1).arg(rp.pos.y, 0, 'f', 1).arg(rp.pos.z, 0, 'f', 1).arg(rp.scl.x, 0, 'f', 1).arg(rp.scl.y, 0, 'f', 1); continue; }
    if (w.size() == 1 && w[0] == "boardstate") { log << QString("board: open %1, locked %2, items %3").arg(board_->isOpen()).arg(board_->isLocked()).arg(scene_.refBoard.value("items").toArray().size()); continue; }
    if (w.size() >= 3 && w[0] == "board") {         // board <image> <png> — the board shown with an image, grabbed
      board_->showBoard(true); board_->addFiles({w[1]}); QElapsedTimer t; t.start(); while (t.elapsed() < 400) QApplication::processEvents();
      board_->grab().save(w.mid(2).join(' ')); log << QString("board: %1 items").arg(scene_.refBoard.value("items").toArray().size()); continue; }
    if (w.size() >= 2 && w[0] == "faq") {             // faq <png> [query] — the FAQ opened (searched), grabbed
      extras::openFaq(this, "hair"); QApplication::processEvents();
      for (QWidget* tw : QApplication::topLevelWidgets()) if (tw->isVisible() && tw->windowTitle() == tr("FAQ")) {
        if (w.size() > 2 && w[2] == "@ach") { for (QLabel* l : tw->findChildren<QLabel*>()) if (l->text().contains("Achievements") && l->cursor().shape() == Qt::PointingHandCursor) { QMouseEvent ev(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, {}); QApplication::sendEvent(l, &ev); break; } }   // (@ach: the Achievements section)
        else if (w.size() > 2) if (auto* le = tw->findChild<QLineEdit*>()) le->setText(w.mid(2).join(' '));
        QElapsedTimer t; t.start(); while (t.elapsed() < 300) QApplication::processEvents(); tw->grab().save(w[1]); log << "faq " + w.mid(2).join(' '); }
      continue; }
    if (w.size() >= 2 && w[0] == "ach") { log << QString("ach %1: %2").arg(w[1]).arg(extras::grant(w[1]) ? "granted" : "no"); continue; }
    if (w.size() == 2 && w[0] == "where") { viewport_->beforeFrame(); const Item* it = scene_.item(w[1].toInt()); if (it && it->poly) { const auto b = it->poly->bounds(); const Vec3 c = b.center(); log << QString("where %1: shown centre %2 %3 %4, poly==base %5").arg(w[1]).arg(c.x, 0, 'f', 2).arg(c.y, 0, 'f', 2).arg(c.z, 0, 'f', 2).arg(it->poly == it->base); } else log << "where " + w[1] + ": none"; continue; }
    if ((w.size() == 7 || w.size() == 10) && w[0] == "campt") {   // campt px py pz tx ty tz [ux uy uz]: the perspective camera from a recording's HMC-CAM
      Camera& c = viewport_->camera(); c.setView("persp"); c.pos = {w[1].toDouble(), w[2].toDouble(), w[3].toDouble()}; c.pivot = {w[4].toDouble(), w[5].toDouble(), w[6].toDouble()};
      c.up = w.size() == 10 ? Vec3(w[7].toDouble(), w[8].toDouble(), w[9].toDouble()) : Vec3(0, 1, 0); c.lookAt(c.pivot); viewport_->requestUpdate(); log << "campt"; continue; }
    if (w.size() == 1 && w[0] == "camtext") { log << camPosText(viewport_); continue; }
    if (w.size() == 4 && w[0] == "cam") { Camera& c = viewport_->camera(); c.pos.x = c.pivot.x = w[1].toDouble(); c.pos.y = c.pivot.y = w[2].toDouble(); c.zoom = w[3].toDouble();   // cam <x> <y> <zoom>: the flat (front) view centred there
      viewport_->sceneChanged(false); viewport_->beforeFrame(); QApplication::processEvents(); log << "cam: " + camPosText(viewport_); continue; }
    // the Topology room: room <hair|topo>, tmode <object|poly|edge|vert>, tsel <f|e|v> <i,j,…>, tg <op> <mesh> <group> [<group2>], topodump
    // (the Paint room's checks) paintmode <map key>: the map painted · paintset <UV set name | -> [w h]: the UV set · paintdab <x y> [gray 0-255]: a dab of the
    // brush in hand into the active layer at that texel · paintinfo: the layers and their pixels
    if (w.size() == 2 && w[0] == "paintmode" && paint_) { paint_->setView(w[1]); QApplication::processEvents(); log << QString("paintmode %1: layer %2").arg(paint_->view).arg(paint_->layer); continue; }
    if (w.size() >= 2 && w[0] == "paintset" && paint_) { const QString n = w[1] == "-" ? QString() : w[1]; const auto t = viewport_->uvSetProps(n); paint_->setSet(n, w.size() > 3 ? w[2].toInt() : t.w, w.size() > 3 ? w[3].toInt() : t.h); QApplication::processEvents();
      log << QString("paintset '%1': %2x%3").arg(paint_->doc.activeSet()).arg(paint_->doc.width()).arg(paint_->doc.height()); continue; }
    if (w.size() >= 3 && w[0] == "paintdab" && paint_) { paint::Layer* l = paint_->active(); if (!l) { paint_->ensureDoc(); paint_->layer = paint_->doc.addPaint(paint_->modeChannel(), {}, paint_->layer); l = paint_->active(); emit paint_->layersChanged(); }
      paint_->doc.ensurePixels(l->id); paint::Brush b = paint_->brush; b.mode = paint_->effectiveMode(); b.pressureSize = false; const int g = w.size() > 3 ? w[3].toInt() : -1;
      paint::Stroke st(l->image, b, g >= 0 ? QColor(g, g, g) : paint_->color); if (st.valid()) { st.begin(QPointF(w[1].toDouble(), w[2].toDouble())); st.move(QPointF(w[1].toDouble() + 3 * b.size, w[2].toDouble() + b.size)); emit paint_->pixelsChanged(st.takeDirty()); }
      QApplication::processEvents(); log << QString("paintdab into layer %1 (%2): %3").arg(l->id).arg(l->name).arg(st.valid() ? "ok" : "the brush does not fit"); continue; }
    if (w.size() == 3 && (w[0] == "paintpx" || w[0] == "paintpick") && paint_) {   // (checks) paintpx u v: the Color map there · paintpick u v: V's colour taken there
      const QPointF t(w[1].toDouble() * paint_->doc.width(), w[2].toDouble() * paint_->doc.height());
      if (w[0] == "paintpick") { const bool ok = pickInto(paint_, t, QCursor::pos()); log << QString("paintpick %1 %2: %3 %4").arg(w[1], w[2]).arg(ok ? "took" : "nothing").arg(paint_->color.name()); continue; }
      const QImage c = paint_->doc.compose(paint::Channel::Color, QRect(int(t.x()), int(t.y()), 1, 1)).convertToFormat(QImage::Format_ARGB32);
      const QRgb v = c.isNull() ? 0 : c.pixel(0, 0); log << QString("paintpx %1 %2: %3,%4,%5 a%6").arg(w[1], w[2]).arg(qRed(v)).arg(qGreen(v)).arg(qBlue(v)).arg(qAlpha(v)); continue; }
    if (w.size() == 3 && w[0] == "bridgeback" && paint_) {      // (checks) bridgeback <layer id>@<set> <#colour>: as if the editor saved the layer back
      QImage im(256, 256, QImage::Format_ARGB32); im.fill(QColor(w[2])); emit paint_->bridge.imageReturned(w[1] == "@" ? QString::number(paint_->layer) + "@" + paint_->doc.activeSet() : w[1], im); log << "bridgeback " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "paintrot" && paintLeft_) { paintLeft_->canvas()->setRotation(w[1].toDouble()); log << QString("paintrot %1").arg(paintLeft_->canvas()->rotation()); continue; }   // (checks) the texture turned
    if (w.size() == 2 && w[0] == "paintfreeze" && paintLeft_) { paintLeft_->freezeSelection(w[1] == "rest"); log << QString("paintfreeze %1: any %2").arg(w[1]).arg(paint_->freezeAny()); continue; }   // (checks) the selection frozen
    if (w.size() == 2 && w[0] == "paintcolor" && paint_) { paint_->color = QColor(w[1]); emit paint_->brushChanged(); log << "paintcolor " + paint_->color.name(); continue; }   // (checks) the brush's colour
    if (w.size() == 2 && w[0] == "paintlock" && paint_) { paint_->islandLock = w[1] == "1"; log << QString("paintlock %1").arg(paint_->islandLock); continue; }   // (checks) the stroke kept in its island
    if (w.size() == 2 && w[0] == "paintsel" && paintLeft_) {    // (checks) paintsel <face|loop|ring|fill|part|group>: the select tool's kind (and H's)
      paintLeft_->setTool("select"); paintLeft_->tools()->selKind = w[1]; viewport_->pvHKind = (w[1] == "loop" || w[1] == "ring" || w[1] == "fill" || w[1] == "group") ? w[1] : QString("part"); log << "paintsel " + w[1]; continue; }
    if (w.size() == 1 && w[0] == "pvinfo") { int n = 0, h = 0; for (const auto& [m, fs] : viewport_->pvSel) n += int(fs.size()); for (const auto& [m, fs] : viewport_->pvHover_) h += int(fs.size());
      const paint::IslandMap* im = paint_ && paint_->islands ? paint_->islands() : nullptr; int ni = 0; if (im) ni = int(im->angle.size()) - 1;
      QString ang; if (im) for (size_t i = 1; i < im->angle.size(); i++) ang += QString(" %1").arg(im->angle[i], 0, 'f', 0);
      log << QString("pvinfo selected %1 · under the pointer %2 · islands %3 (angles%4)").arg(n).arg(h).arg(ni).arg(ang); continue; }
    if (w.size() >= 1 && w[0] == "paintbrush" && paint_) {      // (checks) paintbrush [tool]: the tool taken, then the brush in hand and the camera
      if (w.size() > 1 && paintLeft_) paintLeft_->setTool(w[1]);
      const Quat& q = viewport_->camera().quat;
      log << QString("paintbrush tool %1 size %2 opacity %3 hardness %4 smooth %5 colour %6 · cam %7 %8 %9 %10").arg(paint_->tool).arg(paint_->brush.size, 0, 'f', 1).arg(paint_->brush.opacity, 0, 'f', 3).arg(paint_->brush.hardness, 0, 'f', 2)
               .arg(paint_->brush.smoothing, 0, 'f', 2).arg(paint_->color.name()).arg(q.x, 0, 'f', 4).arg(q.y, 0, 'f', 4).arg(q.z, 0, 'f', 4).arg(q.w, 0, 'f', 4); continue; }
    if (w.size() == 1 && w[0] == "paintinfo" && paint_) { QString o = QString("set '%1' %2x%3 mode %4 · on the model %5 (view on %6 rev %7 %8x%9, brush %10) ·").arg(paint_->doc.activeSet()).arg(paint_->doc.width()).arg(paint_->doc.height()).arg(paint_->view).arg(paint_->onModel).arg(scene_.paintView.on).arg(scene_.paintView.rev).arg(scene_.paintView.flat.width()).arg(scene_.paintView.flat.height()).arg(viewport_->paintOnModel);
      for (const auto& l : paint_->doc.layers()) o += QString(" [%1 %2 %3 px %4 parked %5]").arg(l.id).arg(l.name).arg(paint::mapInfo(l.channel).key).arg(l.image.isNull() ? 0 : 1).arg(l.other.size()); log << o; continue; }
    if (w.size() == 2 && w[0] == "dragout") { std::vector<int> ids; for (const QString& x : w[1].split(',')) ids.push_back(x.toInt()); log << "dragout " + w[1] + ": " + QString::fromUtf8(exportDragItems(ids)); continue; }   // (checks: the file another HairHarpy would get)
    if (w.size() >= 2 && w[0] == "dragin") { const QString p = cmd.trimmed().mid(7).trimmed(); const int before = int(scene_.items.size()); importDragItems(p); viewport_->beforeFrame(); log << QString("dragin: items %1 → %2").arg(before).arg(scene_.items.size()); continue; }
    if (w.size() == 1 && w[0] == "new") { newProject(false); for (int i = 0; i < 6; i++) { viewport_->beforeFrame(); QApplication::processEvents(); } log << QString("new · room %1 · items %2").arg(scene_.room).arg(scene_.items.size()); continue; }   // (checks: a new project, as at a start with none)
    if (w.size() >= 2 && w[0] == "open") {                    // (checks) open <project>: as the File window opens one — after the window has drawn
      const QString p = cmd.trimmed().mid(5).trimmed(); const bool ok = openProject(p); for (int i = 0; i < 6; i++) { viewport_->beforeFrame(); QApplication::processEvents(); }
      log << QString("open %1: %2 · room %3").arg(p).arg(ok).arg(scene_.room); continue; }
    if (w.size() == 2 && w[0] == "room") { goRoom(w[1]); viewport_->beforeFrame(); log << "room " + scene_.room + QString(": items %1").arg(scene_.items.size()); continue; }
    if (w.size() == 4 && w[0] == "tt") { auto tg = viewport_->ttTarget(); viewport_->ttSet(tg, w[1].toInt(), w[2].toInt(), w[3].toDouble()); viewport_->ttDone(tg); viewport_->beforeFrame();
      auto v = viewport_->ttValues(viewport_->ttTarget()); QStringList L; for (auto& r : v) L << (r ? QString("%1 %2 %3").arg((*r)[0]).arg((*r)[1]).arg((*r)[2]) : QString("-")); log << QString("tt kind %1: %2").arg(tg.kind).arg(L.join(" | ")); continue; }
    if (w.size() == 2 && w[0] == "snap") { viewport_->tset.snap = w[1] == "1"; continue; }
    if (w.size() == 3 && w[0] == "freept") { log << "freept: " + viewport_->testFreePoint(QPointF(w[1].toDouble(), w[2].toDouble())); continue; }   // freept <x> <y> — where a PolyPen click there puts its point
    if (w.size() == 1 && w[0] == "uvprev") { viewport_->beforeFrame(); log << QString("uvprev: %1 line points, tool %2").arg(viewport_->uvPreviewLines().size()).arg(viewport_->tool()); continue; }   // (the hover preview's size)
    if (w.size() >= 2 && w[0] == "uvset") {   // uvset new | add N | pick N | iso 0/1 — then the sets (faces), those of the selection, the picked one
      const int m = viewport_->topoMesh();
      if (w[1] == "new") viewport_->uvSetFromShells(-1); else if (w[1] == "add" && w.size() > 2) viewport_->uvSetFromShells(w[2].toInt());
      else if (w[1] == "pick" && w.size() > 2) viewport_->uvPickSet(m, w[2].toInt()); else if (w[1] == "iso" && w.size() > 2) { viewport_->uvSetIso = w[2] == "1"; viewport_->uvChanged(); }
      viewport_->beforeFrame(); QStringList q; for (auto& x : viewport_->uvSets(m)) q << QString("%1:%2").arg(x.name).arg(x.faces);
      QStringList l; for (int x : viewport_->uvSetsOfSel(m)) l << QString::number(x);
      if (w[1] == "all") { QStringList a; for (const auto& [id, it] : scene_.items) { if (!it.poly) continue; const PolyMesh& B = *(it.base ? it.base : it.poly); auto h = viewport_->uvHidden(it, B); int n = 0; for (int f = 0; f < B.faceCount(); f++) if (h.empty() || !h[size_t(f)]) n++; a << QString("%1=%2/%3").arg(it.name).arg(n).arg(B.faceCount()); }   // (every mesh: its polygons the UV Editor shows)
        log << "uvset all: " + a.join(" "); continue; }
      int vis = 0; if (const Item* it = scene_.item(m)) { const PolyMesh& B = *(it->base ? it->base : it->poly); auto h = viewport_->uvHidden(*it, B); for (int f = 0; f < B.faceCount(); f++) if (h.empty() || !h[size_t(f)]) vis++; }
      log << QString("uvset %1: sets [%2] sel in [%3] picked %4 iso %5 shown %6").arg(w.mid(1).join(" "), q.join(" "), l.join(",")).arg(viewport_->uvPicked.set).arg(viewport_->uvSetIso).arg(vis); continue; }
    if (w.size() == 5 && w[0] == "uvselbox") {   // uvselbox u0 v0 u1 v1: the active mesh's shown polygons whose UV middle is in that box picked (Polygon mode)
      const int m = viewport_->topoMesh(); const Item* it = m >= 0 ? scene_.item(m) : nullptr; std::vector<int> fs;
      if (it) { const PolyMesh& B = *(it->base ? it->base : it->poly); const auto h = viewport_->uvHidden(*it, B); const double u0 = w[1].toDouble(), v0 = w[2].toDouble(), u1 = w[3].toDouble(), v1 = w[4].toDouble();
        for (int f = 0; f < B.faceCount(); f++) { if (!h.empty() && h[size_t(f)]) continue; double cu = 0, cv = 0; const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) { cu += B.cornerUV[size_t(k)][0]; cv += B.cornerUV[size_t(k)][1]; } cu /= (e - s); cv /= (e - s); if (cu >= u0 && cu <= u1 && cv >= v0 && cv <= v1) fs.push_back(f); }
        viewport_->tselApply(m, fs, "set", 'f'); }
      log << QString("uvselbox: %1 polygons on %2").arg(fs.size()).arg(it ? it->name : QString("-")); continue; }
    if (w.size() == 1 && w[0] == "esel") { const auto& S = viewport_->topoSel(); QStringList l;   // (the picked edges, each with the polygons of the sides it is picked on)
      for (int e : S.e) { const auto g = S.eSide.find(e); QStringList fs; if (g != S.eSide.end()) for (int f : g->second) fs << QString::number(f); l << QString("%1[%2]").arg(e).arg(fs.isEmpty() ? "-" : fs.join('/')); }
      log << QString("esel %1: %2").arg(S.e.size()).arg(l.join(' ')); continue; }
    if (w.size() == 2 && w[0] == "tloop") { const int e = w[1].toInt(); log << QString("tloop %1: %2 edges, before seam %3 edges").arg(e).arg(viewport_->testLoop(e, false)).arg(viewport_->testLoop(e, true)); continue; }   // (the 3D edge loop through e, without / with Before seam)
    if (w.size() == 3 && w[0] == "uvloopside") {   // uvloopside <edge> <side 0|1>: the UV edge loop on that side — its edges and their sides
      const int m = viewport_->topoMesh(); const Item* it = m >= 0 ? scene_.item(m) : nullptr; QString r = "no mesh";
      if (it) { auto T = topo::ttopo(it->base ? it->base : it->poly); const int e = w[1].toInt(), f = w[2].toInt() == 0 ? T->ef0[size_t(e)] : T->ef1[size_t(e)]; int ek = -1;
        if (f >= 0) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) if (T->ce[size_t(k)] == e) ek = k;
        const auto L = viewport_->uvLoopOnSide(e, ek); const auto L0 = topo::edgeLoop(*T, e); r = QString("%1 edges (side face %2; the mesh loop %3, corner %4)").arg(L.size()).arg(f).arg(L0.size()).arg(ek); }
      log << "uvloopside " + w[1] + " " + w[2] + ": " + r; continue; }
    if (w.size() == 2 && w[0] == "uvseamsame") {   // (edges cut by a seam with both sides on one shell — a sleeve's seam; up to N)
      const int m = viewport_->topoMesh(); const Item* it = m >= 0 ? scene_.item(m) : nullptr; QStringList l;
      if (it) { const PolyMesh& B = *(it->base ? it->base : it->poly); auto T = topo::ttopo(it->base ? it->base : it->poly); auto Pp = uv::points(B, *T); auto Sh = uv::shells(B, *T, Pp);
        for (int e = 0; e < T->ne && l.size() < w[1].toInt(); e++) { const int a = T->ef0[size_t(e)], b = T->ef1[size_t(e)]; if (a >= 0 && b >= 0 && Sh.of[size_t(a)] == Sh.of[size_t(b)] && !uv::continuous(B, *T, Pp, e)) l << QString("%1(f%2/f%3 shell %4)").arg(e).arg(a).arg(b).arg(Sh.of[size_t(a)]); } }
      log << "uvseamsame: " + l.join(' '); continue; }
    if (w.size() == 2 && w[0] == "uvkeys") { viewport_->uvKeys = w[1] == "1"; log << "uvkeys " + w[1]; continue; }   // (the UV hotkeys mode — not kept in the settings)
    if (w.size() >= 2 && w[0] == "uvk") {   // uvk <key> [alt] [shift]: a key of the UV hotkeys mode; logs the selected edges' UV ends and the pins
      Qt::KeyboardModifiers md; for (int i = 2; i < w.size(); i++) { if (w[i] == "alt") md |= Qt::AltModifier; if (w[i] == "shift") md |= Qt::ShiftModifier; }
      const QChar kc = w[1].isEmpty() ? QChar('T') : w[1][0].toUpper(); const int key = kc == '[' ? Qt::Key_BracketLeft : kc == ']' ? Qt::Key_BracketRight : kc.isLetter() ? Qt::Key_A + (kc.unicode() - 'A') : Qt::Key_T;   // (a letter, [ or ])
      const bool h = viewport_->uvKey(key, md); viewport_->beforeFrame(); QString r = QString("uvk %1: %2").arg(w.mid(1).join(" ")).arg(h);
      const int m = viewport_->topoMesh(); if (const Item* it = scene_.item(m)) { const PolyMesh& B = *(it->base ? it->base : it->poly); r += QString(" pins %1").arg(it->raw.value("uvPins").toArray().size());
        if (B.cornerUV.size() == B.faceVerts.size()) { QStringList q; for (int e : viewport_->topoSel().e) { auto T = topo::ttopo(it->base ? it->base : it->poly); if (e >= T->ne) continue; const int f = T->ef0[size_t(e)];
            for (int v : {T->ea[size_t(e)], T->eb[size_t(e)]}) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) if (T->fv[size_t(k)] == v) q << QString("(%1,%2)").arg(B.cornerUV[size_t(k)][0], 0, 'f', 3).arg(B.cornerUV[size_t(k)][1], 0, 'f', 3); if (q.size() > 16) break; }
          r += " uv " + q.join(" "); } }
      log << r; continue; }
    if (w.size() >= 2 && w[0] == "uv") {               // uv mode|keep 0/1, unfold[all], pack[all], angle N, fromuv, clear, mark, unmark, turn, flipu, shells, frame, stat
      const QString op = w[1]; QString r;
      if (op == "open") viewport_->openUvEditor(); else
      if (op == "mode") viewport_->setUvMode(w.value(2) == "1"); else if (op == "keep") viewport_->setPreserveUv(w.value(2) == "1");
      else if (op == "dropseams") r = viewport_->uvDropSeamsBetween(w.value(2).toInt(), w.value(3).toInt());
      else if (op == "chk") { viewport_->uvo.checker = w.value(2) == "1"; viewport_->uvChanged(); r = "ok"; }
      else if (op == "drawtime") { QStringList ms; for (int i = 0; i < 4; i++) { viewport_->topoChanged(); QElapsedTimer t; t.start(); viewport_->uvDraw(); ms << QString::number(t.nsecsElapsed() / 1e6, 'f', 1); } r = "uvDraw ms: " + ms.join(" "); }
      else if (op == "unfold") r = viewport_->uvUnfold(false); else if (op == "unfoldall") r = viewport_->uvUnfold(true);
      else if (op == "pack") r = viewport_->uvPack(false); else if (op == "packall") r = viewport_->uvPack(true);
      else if (op == "angle") r = viewport_->uvSeamsByAngle(w.value(2).toDouble()); else if (op == "fromuv") r = viewport_->uvSeamsFromBorders();
      else if (op == "clear") r = viewport_->uvClearSeams(); else if (op == "mark") r = viewport_->uvMarkSeams(true); else if (op == "unmark") r = viewport_->uvMarkSeams(false);
      else if (op == "turn") r = viewport_->uvTurn(1); else if (op == "flipu") r = viewport_->uvFlip(true); else if (op == "shells") r = viewport_->uvSelectShells();
      else if (op == "frame") viewport_->frameSelection();
      else if (op == "margin") { if (w.size() > 2) viewport_->uvo.pad = w[2].toDouble() / 100; r = QString("margin %1%").arg(viewport_->uvo.pad * 100); }
      else if (op == "bounds") { const auto t = viewport_->uvTex(viewport_->topoMesh()); double a0 = 1e9, b0 = 1e9, a1 = -1e9, b1 = -1e9;   /* the shown shells' box and how far it stays off the tile's edges, px (the pack leaves half the padding) */
        for (int id : viewport_->selection().items) if (const Item* it = scene_.item(id); it && it->poly) { const PolyMesh& B = *(it->base ? it->base : it->poly); if (!uv::has(B)) continue; auto h = viewport_->uvHidden(*it, B); for (int f = 0; f < B.faceCount(); f++) { if (!h.empty() && h[size_t(f)]) continue; for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) { a0 = std::min(a0, double(B.cornerUV[size_t(k)][0])); a1 = std::max(a1, double(B.cornerUV[size_t(k)][0])); b0 = std::min(b0, double(B.cornerUV[size_t(k)][1])); b1 = std::max(b1, double(B.cornerUV[size_t(k)][1])); } } }
        r = QString("box %1..%2 x %3..%4 · off the edges, px: left %5 right %6 bottom %7 top %8 (half padding %9)").arg(a0, 0, 'f', 4).arg(a1, 0, 'f', 4).arg(b0, 0, 'f', 4).arg(b1, 0, 'f', 4).arg(a0 * t.w, 0, 'f', 1).arg((1 - a1) * t.w, 0, 'f', 1).arg(b0 * t.h, 0, 'f', 1).arg((1 - b1) * t.h, 0, 'f', 1).arg(t.pad / 2); }
      else if (op == "stretchfill") { r = viewport_->uvStretchFill(); QStringList bx; for (int id : viewport_->selection().items) if (const Item* it = scene_.item(id)) { const PolyMesh& B = *(it->base ? it->base : it->poly); auto h = viewport_->uvHidden(*it, B); double a0 = 1e9, b0 = 1e9, a1 = -1e9, b1 = -1e9; for (int f = 0; f < B.faceCount(); f++) { if (!h.empty() && h[size_t(f)]) continue; for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) { a0 = std::min(a0, double(B.cornerUV[size_t(k)][0])); a1 = std::max(a1, double(B.cornerUV[size_t(k)][0])); b0 = std::min(b0, double(B.cornerUV[size_t(k)][1])); b1 = std::max(b1, double(B.cornerUV[size_t(k)][1])); } } bx << QString("%1 %2..%3 x %4..%5").arg(it->name).arg(a0, 0, 'f', 3).arg(a1, 0, 'f', 3).arg(b0, 0, 'f', 3).arg(b1, 0, 'f', 3); } r += " | shown boxes: " + bx.join("; "); }
      else if (op == "status") r = viewport_->uvStatus();
      else if (op == "sew") r = viewport_->uvSew();
      else if (op == "cutoff") r = viewport_->uvSeamKey();   // (C with polygons picked: their area cut off as a shell of its own)
      else if (op == "stretch") { viewport_->uvo.stretch = w.value(2) == "1"; viewport_->uvChanged(); }
      else if (op == "solo") { for (int i = 0; i < 4; i++) if (viewport_->paneCamera(i).view == "uv") { viewport_->backToSingle(i); break; } }
      else if (op == "selfover") { const int m = viewport_->topoMesh(); const Item* it = scene_.item(m); const PolyMesh& B = *(it->base ? it->base : it->poly); auto T = topo::ttopo(it->base ? it->base : it->poly);
        auto Pp = uv::points(B, *T); auto Sh = uv::shells(B, *T, Pp); std::map<int, std::vector<std::array<int, 2>>> rim;   // (each shell's outline: its UV border sides, as corners)
        for (int e = 0; e < T->ne; e++) if (!uv::continuous(B, *T, Pp, e)) for (int fsel : {T->ef0[size_t(e)], T->ef1[size_t(e)]}) { if (fsel < 0) continue; const int ka = uv::cornerAt(*T, fsel, T->ea[size_t(e)]), kb = uv::cornerAt(*T, fsel, T->eb[size_t(e)]); if (ka >= 0 && kb >= 0) rim[Sh.of[size_t(fsel)]].push_back({ka, kb}); }
        int bad = 0; QStringList which; for (auto& [sh, E] : rim) { int c = 0; for (size_t i = 0; i < E.size(); i++) for (size_t j = i + 1; j < E.size(); j++) {
            auto P0 = B.cornerUV[size_t(E[i][0])], P1 = B.cornerUV[size_t(E[i][1])], Q0 = B.cornerUV[size_t(E[j][0])], Q1 = B.cornerUV[size_t(E[j][1])];
            if (P0 == Q0 || P0 == Q1 || P1 == Q0 || P1 == Q1) continue; auto cr = [](std::array<float, 2> a, std::array<float, 2> b, std::array<float, 2> c) { return (double(b[0]) - a[0]) * (double(c[1]) - a[1]) - (double(b[1]) - a[1]) * (double(c[0]) - a[0]); };
            if ((cr(P0, P1, Q0) > 0) != (cr(P0, P1, Q1) > 0) && (cr(Q0, Q1, P0) > 0) != (cr(Q0, Q1, P1) > 0)) c++; }
          if (c) { bad++; which << QString("%1:%2").arg(sh).arg(c); } }
        r = QString("%1 shells cross themselves %2").arg(bad).arg(which.join(QChar(0x20))); }
      else if (op == "gizpx") { const QPointF c = viewport_->uvGizmoPx(); const auto fr = viewport_->uvFramePx(); r = QString("gizmo %1 at %2,%3 frame %4,%5 .. %6,%7").arg(viewport_->uvGizmoOn()).arg(c.x(), 0, 'f', 1).arg(c.y(), 0, 'f', 1).arg(fr[0], 0, 'f', 1).arg(fr[1], 0, 'f', 1).arg(fr[2], 0, 'f', 1).arg(fr[3], 0, 'f', 1); }
      else if (op == "diag") { viewport_->uvo.diag = w.value(2) == "1"; viewport_->uvo.stretchAngle = w.value(3) == "ang"; if (w.value(3) == "ang") viewport_->uvo.stretch = true; viewport_->uvChanged(); r = "ok"; }
      else if (op == "checks") { const auto& c = viewport_->uvCheck(); r = QString("flipped %1 overlap %2 (stacks %3) outside %4 padding %5 density %6 asym %7").arg(c.flipped.size()).arg(c.overlap.size()).arg(c.stacks).arg(c.outside.size()).arg(c.padding.size()).arg(c.density.size()).arg(c.asym.size()); }
      else if (op == "checksel") r = viewport_->uvCheckSelect(w.value(2));
      else if (op == "attr") r = viewport_->uvShellSet(w.value(2), w.value(3).toDouble());
      else if (op == "hover") { viewport_->uvTestHover(w.value(2).toInt()); r = "ok"; }
      else if (op == "hsymtest") { const Vec3 at(w.value(2).toDouble(), w.value(3).toDouble(), w.value(4).toDouble()), d(w.value(5).toDouble(), w.value(6).toDouble(), w.value(7).toDouble()); int id = -1; double bd = 1e300; for (const auto& [aid, a] : scene_.anchors) { const double dd = a.pos.distanceTo(at); if (dd < bd) { bd = dd; id = aid; } }
        const auto H = viewport_->hairSymTwins({id}); Anchor* a = scene_.anchor(id); if (!a) r = "no anchor"; else { a->pos = a->pos + d; viewport_->hairSymApply(H); QStringList l; for (int t : H.twinIds()) { const Anchor* b = scene_.anchor(t); l << QString("%1 at %2,%3,%4").arg(t).arg(b->pos.x, 0, 'f', 3).arg(b->pos.y, 0, 'f', 3).arg(b->pos.z, 0, 'f', 3); } r = QString("anchor %1 now %2,%3,%4 · twins: %5").arg(id).arg(a->pos.x, 0, 'f', 3).arg(a->pos.y, 0, 'f', 3).arg(a->pos.z, 0, 'f', 3).arg(l.join("; ")); } }
      else if (op == "symopt") { auto& S = viewport_->symo; const QString k = w.value(2), v = w.value(3); if (k == "type") S.type = v; else if (k == "space") S.space = v; else if (k == "context") S.context = v; else if (k == "axes") { S.ax[0] = v.contains('x'); S.ax[1] = v.contains('y'); S.ax[2] = v.contains('z'); } else if (k == "radial") S.radial = v.toInt(); else if (k == "tol") S.tol = v.toDouble(); else if (k == "planes") S.planes = v == "1"; viewport_->symChanged(); r = "ok"; }
      else if (op == "symstat") { const int m = viewport_->topoMesh(); const Item* it = scene_.item(m); if (!it) r = "no mesh"; else { const auto& B = *(it->base ? it->base : it->poly); const auto S = viewport_->symSet(m, B.pos); QStringList l; for (size_t k = 0; k < S.pr.size(); k++) { int n = 0, self = 0; for (size_t v = 0; v < S.pr[k].size(); v++) { if (S.pr[k][v] >= 0) n++; if (S.pr[k][v] == int(v)) self++; } l << QString("map%1 det%2: %3 twins (%4 on itself)").arg(k).arg(S.T[k].det(), 0, 'f', 0).arg(n).arg(self); } r = QString("%1 verts, %2 maps, planes %3: ").arg(B.pos.size()).arg(S.T.size()).arg(S.planes.size()) + l.join("; "); } }
      else if (op == "sympanel") { QWidget* a = findChild<QWidget*>("tt_symset"); if (!a) a = this; openSymPanel(viewport_, a, true); QApplication::processEvents(); auto* p = findChild<QWidget*>("sympanel"); r = p && p->grab().save(w.value(2)) ? "shot" : "no panel"; if (p) p->close(); }
      else if (op == "brush") { viewport_->uvBrushR = w.value(8, "40").toDouble(); r = viewport_->uvBrushTest(w.value(2) == "p" ? 'p' : w.value(2) == "s" ? 's' : 'r', w.value(3).toDouble(), w.value(4).toDouble(), w.value(5).toDouble(), w.value(6).toDouble(), w.value(7) == "1"); }
      else if (op == "attrinfo") { const auto a = viewport_->uvShellAttrs(viewport_->topoMesh()); int l = 0, g = 0; for (auto& x : a) { l += x.lock; g += x.group > 0; } const auto& D = viewport_->uvDraw(); QStringList ts; for (auto& t : D.tris) ts << QString("%1@%2").arg(t.c.size()).arg(t.op); r = QString("faces %1 locked %2 grouped %3 tris %4").arg(a.size()).arg(l).arg(g).arg(ts.join(" ")); }
      else if (op == "align") r = viewport_->uvAlign(w.value(2)); else if (op == "dist") r = viewport_->uvDistribute(w.value(2) == "u"); else if (op == "grid") r = viewport_->uvGrid(); else if (op == "line") r = viewport_->uvStraightenLoop();
      else if (op == "byedge") r = viewport_->uvOrientEdge(); else if (op == "upright") r = viewport_->uvOrientUpright(); else if (op == "stack") r = viewport_->uvStack(); else if (op == "unstack") r = viewport_->uvUnstack(); else if (op == "snapt") r = viewport_->uvSnapTogether();
      else if (op == "td") r = QString("td sel %1 px/cm, wanted %2").arg(viewport_->uvDensityOfSel()).arg(viewport_->uvTex(viewport_->topoMesh()).td); else if (op == "tdget") r = viewport_->uvDensityGet(); else if (op == "tdset") r = viewport_->uvDensitySet(); else if (op == "nudge") r = viewport_->uvNudge(w.value(2).toInt(), w.value(3).toInt());
      else if (op == "face") r = viewport_->uvFeatureKey(w.value(2) == "closed" ? 1 : w.value(2) == "off" ? 2 : 0);
      else if (op == "rizomfiles") r = viewport_->uvToRizom(false, false); else if (op == "rizomback") r = viewport_->uvFromRizom(w.value(2));
      else if (op == "tex") { Viewport::UvTex t; t.w = w.value(2).toInt(); t.h = w.value(3).toInt(); t.pad = w.value(4).toDouble(); t.color = w.value(5); t.tex = w.value(6); if (t.color == "-") t.color.clear(); r = viewport_->uvSetTex(viewport_->topoMesh(), t); }
      else if (op == "dropuv") r = viewport_->uvTestStrip(); else if (op == "uvfreepick") { viewport_->uvFreePicked = w.mid(2).join(QChar(0x20)); r = "lit " + viewport_->uvFreePicked; }
      else if (op == "uvprops") { const auto t = viewport_->uvSetProps(w.mid(2).join(QChar(0x20))); r = QString("props %1x%2 pad %3 td %4 color %5 tex %6").arg(t.w).arg(t.h).arg(t.pad).arg(t.td).arg(t.color, t.tex); }
      else if (op == "packmode") { viewport_->uvo.packFinal = w.value(2) == "final"; viewport_->uvo.packEqual = w.value(3) == "equal"; r = QString("packmode final %1 equal %2").arg(viewport_->uvo.packFinal).arg(viewport_->uvo.packEqual); }
      else if (op == "packfree") { viewport_->uvo.packFree = w.value(2) == "1"; r = QString("packfree %1").arg(viewport_->uvo.packFree); }
      else if (op == "tiles") { viewport_->setUvTiles(w.value(2) == "1"); viewport_->beforeFrame(); QStringList ns; for (const QString& x : viewport_->uvTileNames()) ns << x; r = QString("tiles %1: %2").arg(viewport_->uvTiles).arg(ns.join(", ")); }
      else if (op == "tileclick") { const int i = w.value(2).toInt(); const int n = int(viewport_->uvTileNames().size()); const auto o = viewport_->uvTileOrigin(i, n); const bool ok = viewport_->uvTilePick(viewport_->uvPxOf(o[0] + 0.5, o[1] + 0.5)); viewport_->beforeFrame(); r = QString("tileclick %1: %2, tiles %3, picked mesh %4 set %5").arg(i).arg(ok).arg(viewport_->uvTiles).arg(viewport_->uvPicked.id).arg(viewport_->uvPicked.set); }
      else if (op == "showpad") { viewport_->uvShowPad = w.value(2) == "1"; viewport_->uvChanged(); r = "ok"; } else if (op == "mat") { viewport_->uvPickMat(viewport_->topoMesh(), w.value(2).toInt()); r = "mat " + w.value(2); }
      else if (op == "symon") { viewport_->tset.sym = w.value(2) != "0"; if (w.size() > 3) viewport_->uvo.symAxis = w[3].toDouble(); viewport_->uvChanged(); r = "ok"; } else if (op == "symz") r = viewport_->uvSymmetrize(); else if (op == "symunfold") r = viewport_->uvUnfold(w.value(2) == "all", true); else if (op == "symkey") { runAction("symmetry"); r = viewport_->tset.sym ? "sym on" : "sym off"; } else if (op == "symcheck") r = viewport_->uvSymCheck(); else if (op == "tfinfo") r = viewport_->uvTfInfo(); else if (op == "psynth") r = viewport_->uvPuppetSynth(w.value(2).toInt()); else if (op == "ptest") r = viewport_->uvPuppetTest(w.value(2).toInt(), w.value(3) == "bend", w.value(3) == "arc"); else if (op == "tf") r = viewport_->uvTfStart(w.value(2) == "w" ? 'w' : w.value(2) == "p" ? 'p' : 'f'); else if (op == "tfdrag") r = viewport_->uvTfTestDrag(w.value(2).toInt(), w.value(3).toDouble(), w.value(4).toDouble(), w.value(5) == "1");
      else if (op == "rectall") r = viewport_->uvRectangleStrands(true);
      else if (op == "optimize") r = viewport_->uvOptimize();
      else if (op == "strall") { if (w.size() > 2) viewport_->uvo.strAmt = w[2].toDouble(); r = viewport_->uvStraightenStrands(true); } else if (op == "snap") r = viewport_->uvSnapEdge(w.value(2));
      else if (op == "scale") r = viewport_->uvScaleBy(w.value(2).toDouble()) + " box " + [&] { const auto b = viewport_->uvSelBox(); return b ? QString("%1 %2 %3 %4").arg((*b)[0]).arg((*b)[1]).arg((*b)[2]).arg((*b)[3]) : QString(); }();
      else if (op == "selfsym") r = viewport_->testUvSelfSym(w.value(2, "0.02").toDouble(), w.value(3, "0.01").toDouble());
      else if (op == "box") r = viewport_->uvSetBox(w.value(2).toDouble(), w.value(3).toDouble(), w.value(4).toDouble(), w.value(5).toDouble());
      else if (op == "selbox") { auto b = viewport_->uvSelBox(); r = b ? QString("%1 %2 %3 %4").arg((*b)[0]).arg((*b)[1]).arg((*b)[2]).arg((*b)[3]) : QString("-"); }
      else if (op == "winshot") { QApplication::processEvents(); if (uvtools_) { uvtools_->show(); uvtools_->sync(); QApplication::processEvents(); r = uvtools_->grab().save(w.value(2)) ? "shot" : "shot failed"; } }
      else if (op == "vert") { int m = viewport_->topoMesh(); const Item* it = m >= 0 ? scene_.item(m) : nullptr; const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr; int v = w.value(2).toInt();
        if (B && v >= 0 && size_t(v) < B->pos.size()) { r = QString("pos %1 %2 %3 uv").arg(B->pos[size_t(v)].x, 0, 'f', 3).arg(B->pos[size_t(v)].y, 0, 'f', 3).arg(B->pos[size_t(v)].z, 0, 'f', 3);
          if (uv::has(*B)) for (size_t k = 0; k < B->faceVerts.size(); k++) if (B->faceVerts[k] == v) r += QString(" %1,%2").arg(B->cornerUV[k][0], 0, 'f', 4).arg(B->cornerUV[k][1], 0, 'f', 4); } }
      else if (op == "aligncheck") { int m = viewport_->topoMesh(); const Item* it = m >= 0 ? scene_.item(m) : nullptr; const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr;   // (the h / v constraints: how far each edge is off level / upright, to its length)
        if (B && uv::has(*B)) { auto T = topo::ttopo(it->base ? it->base : it->poly); const uv::Pins pins = viewport_->uvPinsOf(m); int n = 0; double worst = 0;
          const uv::Points PP = uv::points(*B, *T); const uv::Shells SS = uv::shells(*B, *T, PP); const int only = w.size() > 2 ? w[2].toInt() : -1;   // (a face given: its shell only)
          for (int f = 0; f < T->nf; f++) if (only < 0 || SS.of[size_t(f)] == SS.of[size_t(only)]) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)]; const int al = uv::edgeAligned(pins, f, T->ce[size_t(k)]); if (!al) continue;
            const auto &p = B->cornerUV[size_t(k)], &q = B->cornerUV[size_t(nk)]; const double du = std::abs(double(p[0]) - q[0]), dv = std::abs(double(p[1]) - q[1]), L = std::hypot(du, dv); if (L <= 0) continue; worst = std::max(worst, (al == 1 ? dv : du) / L); n++; }
          double s1 = 0, s2 = 0; int ne = 0; for (int f = 0; f < T->nf; f++) if (only >= 0 && SS.of[size_t(f)] == SS.of[size_t(only)]) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { const int nk = k + 1 < T->fo[size_t(f) + 1] ? k + 1 : T->fo[size_t(f)];   // (the stretch: the spread of UV / 3D edge lengths)
            const double l3 = B->pos[size_t(T->fv[size_t(k)])].distanceTo(B->pos[size_t(T->fv[size_t(nk)])]); if (l3 <= 0) continue; const auto &p = B->cornerUV[size_t(k)], &q = B->cornerUV[size_t(nk)]; const double x = std::log(std::max(1e-12, std::hypot(double(p[0]) - q[0], double(p[1]) - q[1]) / l3)); s1 += x; s2 += x * x; ne++; }
          const double sd = ne ? std::sqrt(std::max(0.0, s2 / ne - (s1 / ne) * (s1 / ne))) : 0;
          r = QString("aligned edges %1, worst off %2, edge stretch spread %3").arg(n).arg(worst, 0, 'f', 5).arg(sd, 0, 'f', 4); } }
      else if (op == "stat") { int m = viewport_->topoMesh(); const Item* it = m >= 0 ? scene_.item(m) : nullptr; const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr;
        r = viewport_->uvStatus() + QString(" | twoTone %1 shown %2 checker %3 mode %4").arg(viewport_->topoTwoTone(m)).arg(viewport_->uvShown(m)).arg(viewport_->uvo.checker).arg(viewport_->uvMode());
        if (B && uv::has(*B)) { std::vector<int> all(B->faceVerts.size()); std::iota(all.begin(), all.end(), 0); auto b = uv::box(*B, all);
          r += QString(" | box %1,%2 .. %3,%4 | uv0 %5,%6 | seams %7").arg(b[0], 0, 'f', 4).arg(b[1], 0, 'f', 4).arg(b[2], 0, 'f', 4).arg(b[3], 0, 'f', 4).arg(B->cornerUV[0][0], 0, 'f', 4).arg(B->cornerUV[0][1], 0, 'f', 4).arg(B->seams.size()); } }
      viewport_->beforeFrame(); log << "uv " + w.mid(1).join(QChar(0x20)) + (r.isEmpty() ? QString() : ": " + r); continue; }
    if (w.size() == 1 && w[0] == "curvevis") { QStringList L; for (const Item* c : scene_.curves()) if (c->ring < 0 && scene_.isVisible(*c) && c->anchors.size() >= 4 && L.size() < 6) L << QString("%1:%2").arg(c->id).arg(QStringList([&] { QStringList a; for (int x : c->anchors) a << QString::number(x); return a; }()).join("-")); log << "curvevis " + L.join(" "); continue; }
    if (w.size() == 2 && w[0] == "curvedbg") { const Item* c = scene_.item(w[1].toInt()); QStringList ds; if (c) { for (size_t i = 0; i + 1 < c->anchors.size(); i++) { const Anchor* a = scene_.anchor(c->anchors[i]); const Anchor* b = scene_.anchor(c->anchors[i + 1]); ds << (a && b ? QString::number(a->pos.distanceTo(b->pos), 'g', 4) : QString("?")); } }
      log << QString("curvedbg %1: vis %2 ring %3 room %4 step %5 · %6").arg(w[1]).arg(c && scene_.isVisible(*c)).arg(c ? c->ring : -9).arg(viewport_->room()).arg(projectStep(scene_)).arg(ds.join(",")); continue; }
    if (w.size() == 3 && w[0] == "hinsp") { auto& H = viewport_->hinsp; bool on = w[2] == "1"; if (w[1] == "near") H.near = on; else if (w[1] == "twisted") H.twisted = on; else if (w[1] == "degen") H.degen = on; else if (w[1] == "clip") H.clip = on; viewport_->hinspChanged(); viewport_->beforeFrame();
      if (w[1] == "clip") { const auto& C = viewport_->selfClips(); QStringList ps; for (auto [a, b] : C.walls) { const Item* A = scene_.item(a); const Item* B = scene_.item(b); const Item* g = A ? scene_.item(A->parent) : nullptr;
          ps << QString("%1%2 [%3]").arg(A ? A->name : "?").arg(a == b ? QString(" itself") : " × " + (B ? B->name : QString("?"))).arg(g ? g->name : QString()); }
        log << QString("selfclips: %1 crossings, %2 tris · %3").arg(C.n).arg(C.tris.size() / 3).arg(ps.join("; "));
        QStringList dd; for (auto& x : C.dbg) dd << QString("%1/%2@%3deg[%4x%5]").arg(x[0], 0, 'f', 2).arg(x[1], 0, 'f', 2).arg(x[2], 0, 'f', 0).arg(int(x[3])).arg(int(x[4])) + QString("(%1,%2,%3)").arg(x[5], 0, 'f', 0).arg(x[6], 0, 'f', 0).arg(x[7], 0, 'f', 0); log << "rimdist " + dd.join(" "); }
      const auto& D = viewport_->topoDraw(); size_t l = 0, p = 0, t = 0; for (auto& x : D.lines) l += x.p.size() / 2; for (auto& x : D.pts) p += x.p.size(); for (auto& x : D.tris) t += x.p.size() / 3; log << QString("hinsp %1 %2: %3 segments, %4 points, %5 tris").arg(w[1], w[2]).arg(l).arg(p).arg(t); continue; }
    if (w.size() == 2 && w[0] == "camfile") { QFile f(w[1]); bool ok = f.open(QIODevice::ReadOnly) && applyCamPos(viewport_, QString::fromUtf8(f.readAll())); viewport_->beforeFrame(); log << QString("camfile %1").arg(ok); continue; }
    if (w.size() >= 3 && w[0] == "impwin") { importWindowShot(viewport_, &scene_, w.mid(2).join(' ').split('|'), w[1]); log << "impwin " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "expwin") { exportWindowShot(viewport_, &scene_, w[1]); log << "expwin " + w[1]; continue; }
    if (w.size() >= 4 && w[0] == "exporto") {            // exporto <fmt> <smoothing groups 0|1> <path> — every mesh
      auto list = exportMeshes(scene_, {}, "all"); QFile f(w.mid(3).join(' ')); bool ok = !list.empty() && f.open(QIODevice::WriteOnly);
      if (ok) f.write(exportText(w[1], list, 1, w[2] == "1"));
      log << QString("exporto %1 %2: %3 meshes, %4").arg(w[1], w[2]).arg(list.size()).arg(ok); continue; }
    if (w.size() >= 3 && w[0] == "importo") {            // importo uvs=0,hard=1,curves=1 <files|…> — the import window's choices
      TopoImport o; for (const QString& kv : w[1].split(',')) { auto p = kv.split('='); if (p.size() != 2) continue; bool on = p[1] == "1";
        if (p[0] == "groups") o.groups = on; else if (p[0] == "objects") o.objects = on; else if (p[0] == "head") o.head = on; else if (p[0] == "uvs") o.uvs = on;
        else if (p[0] == "textures") o.textures = on; else if (p[0] == "creases") o.creases = on; else if (p[0] == "hard") o.hard = on; else if (p[0] == "curves") o.curves = on; else if (p[0] == "parts") o.parts = on; }
      const QStringList fs = w.mid(2).join(' ').split('|');
      if (scene_.room != "hair") topoImportFiles(fs, o, viewport_, &scene_); else importFilesWith(fs, o, viewport_, &scene_);
      viewport_->beforeFrame(); int curves = 0, meshes = 0; size_t hard = 0, uv = 0, cr = 0;
      for (auto& [id, it] : scene_.items) { if (it.type == ItemType::Curve) curves++; if (it.type == ItemType::IMesh && it.poly) { meshes++; hard += it.poly->hardEdges.size(); uv += it.poly->cornerUV.size(); cr += it.poly->creases.size(); } }
      log << QString("importo %1: %2 meshes, %3 curves, %4 hard edges, %5 uv corners, %6 creases").arg(w[1]).arg(meshes).arg(curves).arg(hard).arg(uv).arg(cr); continue; }
    if (w.size() == 1 && w[0] == "dblmat") {             // two quick clicks on the material's picture (the panel made again between them)
      auto find = [this]() -> QPushButton* { for (auto* b : findChildren<QPushButton*>()) if (b->isVisible() && b->toolTip().startsWith("Custom material")) return b; return nullptr; };
      auto click = [](QPushButton* b) { QPointF c(b->width() / 2.0, b->height() / 2.0);
        QMouseEvent p(QEvent::MouseButtonPress, c, b->mapToGlobal(c.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(b, &p);
        QMouseEvent r(QEvent::MouseButtonRelease, c, b->mapToGlobal(c.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier); QApplication::sendEvent(b, &r); };
      for (auto* f : QApplication::topLevelWidgets()) if (auto* mb = qobject_cast<QMessageBox*>(f); mb && mb->isVisible()) mb->close();   // (a message left from opening the project)
      QPushButton* b1 = find(); if (!b1) { log << "dblmat: no material button"; continue; }
      click(b1); QElapsedTimer t; t.start(); while (t.elapsed() < 60) QApplication::processEvents();
      QPushButton* b2 = find(); const bool rebuilt = b2 != b1; if (b2) click(b2);
      t.restart(); while (t.elapsed() < 200) QApplication::processEvents();
      bool pop = false; QStringList vis; for (auto* f : QApplication::topLevelWidgets()) { if (f->objectName() == "matPop" && f->isVisible()) { pop = true; f->grab().save(QDir::temp().filePath("hm_matwin.png")); vis << QString("@%1,%2 %3x%4").arg(f->x()).arg(f->y()).arg(f->width()).arg(f->height()); } if (f->isVisible()) vis << f->objectName() + ":" + f->metaObject()->className(); }
      log << "visible: " + vis.join(", ");
      log << QString("dblmat: panel rebuilt between clicks %1, settings window open %2").arg(rebuilt).arg(pop); continue;
    }
    if (w.size() == 1 && w[0] == "paneltime") {           // the outliner and the panels made again — each timed
      QElapsedTimer t; t.start(); rebuildOutliner(); const qint64 a = t.elapsed(); syncPanels(); const qint64 b = t.elapsed();
      if (convPanel_) convPanel_->sync(); const qint64 c = t.elapsed(); dpProps_->setBody(buildPropsPanel(viewport_, &scene_, -1)); const qint64 d = t.elapsed();
      if (QWidget* tw = buildToolPanel(viewport_, &scene_)) { delete tw; } const qint64 e = t.elapsed();
      t.restart(); tmodes_->sync(); const qint64 m1 = t.elapsed(); uvtools_->sync(); const qint64 m2 = t.elapsed(); ttab_->sync(true); const qint64 m3 = t.elapsed();
      log << QString("paneltime: outliner %1 ms, panels %2 ms (conv %3, props %4, tool %5); modes %6, uv %7, table %8").arg(a).arg(b - a).arg(c - b).arg(d - c).arg(e - d).arg(m1).arg(m2 - m1).arg(m3 - m2); continue;
    }
    if (w.size() == 2 && w[0] == "savetime") {            // savetime <file>: each step of a save timed (what holds the window: before the thread)
      QElapsedTimer t; t.start(); viewport_->beforeFrame(); const qint64 a = t.elapsed();
      SaveExtras x; x.camera = viewport_->cameraJson(); x.thumb = thumbDataUrl(); const qint64 b = t.elapsed();
      x.cocoons = viewport_->hair().cocoonSystem(); x.convChecked = convChecked(scene_, viewport_->hair(), viewport_->selection().items); const qint64 c = t.elapsed();
      auto sc = std::make_shared<Scene>(fileScene()); const qint64 d = t.elapsed();
      const QByteArray js = projectJson(*sc, x); const qint64 e = t.elapsed(); QFile f(w[1]); f.open(QIODevice::WriteOnly); f.write(js); f.close(); const qint64 g = t.elapsed();
      log << QString("savetime: frame %1, thumb %2, conv %3, scene copy %4 ms (window held %5 ms); json %6, write %7 ms, %8 MB").arg(a).arg(b - a).arg(c - b).arg(d - c).arg(d).arg(e - d).arg(g - e).arg(js.size() / 1048576); continue;
    }
    if (w.size() == 2 && (w[0] == "seltime" || w[0] == "undotime")) {   // seltime <ids|none> / undotime -: the selection (or an undo), the events after it, the frame — each timed
      QElapsedTimer t; t.start();
      if (w[0] == "undotime") viewport_->undo(); else { std::vector<int> ids; if (w[1] != "none") for (const QString& s : w[1].split(',')) ids.push_back(s.toInt()); viewport_->setSelectedItems(ids); }
      const qint64 a = t.elapsed(); QApplication::processEvents(); const qint64 b = t.elapsed();
      const int f0 = viewport_->framesDrawn; viewport_->requestUpdate(); QElapsedTimer to; to.start();
      while (viewport_->framesDrawn == f0 && to.elapsed() < 10000) QApplication::processEvents(QEventLoop::AllEvents, 5);
      log << QString("%1 %2: call %3 ms, events %4 ms, frame %5 ms").arg(w[0], w[1]).arg(a).arg(b - a).arg(t.elapsed() - b); continue;
    }
    if (w.size() == 2 && w[0] == "acttime") {             // acttime <action>: the action, the events after it, the frame — each timed
      QElapsedTimer t; t.start(); runAction(w[1]); const qint64 a = t.elapsed();
      QApplication::processEvents(); const qint64 b = t.elapsed();
      const int f0 = viewport_->framesDrawn; viewport_->requestUpdate(); QElapsedTimer to; to.start();
      while (viewport_->framesDrawn == f0 && to.elapsed() < 10000) QApplication::processEvents(QEventLoop::AllEvents, 5);
      log << QString("acttime %1: action %2 ms, events %3 ms, frame %4 ms").arg(w[1]).arg(a).arg(b - a).arg(t.elapsed() - b); continue;
    }
    if (w.size() == 2 && (w[0] == "kdown" || w[0] == "kup")) { const int key = QKeySequence(w[1])[0].key(); QKeyEvent ev(w[0] == "kdown" ? QEvent::KeyPress : QEvent::KeyRelease, key, Qt::NoModifier, w[1].toLower());
      QWidget* tgt = ui::Pie::current() ? static_cast<QWidget*>(ui::Pie::current()) : this; QCoreApplication::sendEvent(tgt, &ev); QApplication::processEvents(); log << w[0] + " " + w[1] + QString(" · pie %1").arg(ui::Pie::current() != nullptr); continue; }
    if (w.size() == 2 && w[0] == "wirebtn") { wireBtn_->setChecked(w[1] == "1"); viewport_->beforeFrame(); log << "wirebtn " + w[1]; continue; }   // (the outliner's W)
    if (w.size() == 2 && w[0] == "pieshot") { QApplication::processEvents(); bool ok = ui::Pie::current() && ui::Pie::current()->grab().save(w[1]); log << QString("pieshot %1").arg(ok); continue; }
    if (w.size() >= 3 && w[0] == "faqat") { extras::openFaqAt(this, scene_.room, w.mid(2).join(QChar(0x20))); { QElapsedTimer t; t.start(); while (t.elapsed() < 300) QApplication::processEvents(); } QWidget* fw = nullptr; for (QWidget* x : QApplication::topLevelWidgets()) if (x->isVisible() && x->windowTitle() == tr("FAQ")) fw = x; log << QString("faqat %1").arg(fw && fw->grab().save(w[1])); if (fw) fw->close(); continue; }
    if (w.size() >= 2 && w[0] == "keytime") {             // keytime H [ctrl] [shift]: a real key press through the app, until the frame after it
      Qt::KeyboardModifiers km; if (w.contains("ctrl")) km |= Qt::ControlModifier; if (w.contains("shift")) km |= Qt::ShiftModifier;
      const int key = QKeySequence(w[1]).count() ? QKeySequence(w[1])[0].key() : 0;
      QElapsedTimer t; t.start(); const int f0 = viewport_->framesDrawn;
      QKeyEvent* ev = new QKeyEvent(QEvent::KeyPress, key, km, w[1].toLower()); QCoreApplication::postEvent(viewport_, ev);
      QKeyEvent* er = new QKeyEvent(QEvent::KeyRelease, key, km, w[1].toLower()); QCoreApplication::postEvent(viewport_, er);
      viewport_->requestUpdate(); QElapsedTimer to; to.start();
      while ((viewport_->framesDrawn == f0 || t.elapsed() < 5) && to.elapsed() < 10000) QApplication::processEvents(QEventLoop::AllEvents, 5);
      log << QString("keytime %1: %2 ms, active window %3").arg(w.mid(1).join("+")).arg(t.elapsed()).arg(QApplication::activeWindow() ? QApplication::activeWindow()->metaObject()->className() : "none"); continue;
    }
    if (w.size() == 1 && w[0] == "nearlist") {
      std::map<int, std::vector<const Item*>> of; for (const Item* c : scene_.curves()) if (c->ring < 0) for (int a : c->anchors) of[a].push_back(c);
      const double thr = 0.15 * viewport_->weldDistance(); int n = 0;
      for (auto& [a, ca] : of) for (auto& [b, cb] : of) {
        if (b <= a || n >= 12) continue; const Anchor* A = scene_.anchor(a); const Anchor* B = scene_.anchor(b); if (!A || !B) continue;
        double d = A->pos.distanceTo(B->pos); if (d >= thr) continue; bool sh = false; for (auto* x : ca) for (auto* y : cb) if (x == y) sh = true; if (sh) continue;
        n++; QStringList na, nb; for (auto* x : ca) na << x->name + QString("[%1 i%2/%3]").arg(x->id).arg(std::find(x->anchors.begin(), x->anchors.end(), a) - x->anchors.begin()).arg(x->anchors.size());
        for (auto* y : cb) nb << y->name + QString("[%1 i%2/%3]").arg(y->id).arg(std::find(y->anchors.begin(), y->anchors.end(), b) - y->anchors.begin()).arg(y->anchors.size());
        log << QString("near %1 / %2 d=%3: %4 | %5").arg(a).arg(b).arg(d, 0, 'g', 3).arg(na.join(","), nb.join(","));
      }
      log << QString("thr %1").arg(thr); continue;
    }
    if (w.size() == 1 && w[0] == "convsum") { auto S = viewport_->convertSummary(); log << QString("convsum: %1 locks, %2 polys, %3 tris, %4 verts").arg(S.locks).arg(S.polys).arg(S.tris).arg(S.verts); continue; }
    if (w.size() == 2 && w[0] == "inspshot") { if (!insp_->isVisible()) inspBtn_->click(); QApplication::processEvents(); bool ok = insp_->grab().save(w[1]); log << (ok ? "inspector shot " : "inspector shot FAILED ") + w[1]; continue; }
    if (w.size() == 3 && w[0] == "insp") { auto& I = viewport_->insp; bool on = w[2] == "1"; if (w[1] == "ngons") I.ngons = on; else if (w[1] == "open") I.open = on; else if (w[1] == "planar") I.nonPlanar = on; else if (w[1] == "manifold") I.nonManifold = on; else if (w[1] == "interior") I.interior = on; else if (w[1] == "hard") I.hard = on; else if (w[1] == "doubles") I.doubles = on; else if (w[1] == "asym") I.asym = on; viewport_->inspChanged(); viewport_->beforeFrame();
      size_t a = 0, b = 0, c3 = 0, d = 0, e = 0, pc = 0, hd = 0; for (int m : viewport_->topoMeshes()) { const auto& c = viewport_->inspOf(m); a += c.ngons.size(); b += c.open.size(); c3 += c.nonPlanar.size(); d += c.nonMan.size(); e += c.interior.size(); pc += c.interiorPts.size(); hd += c.hard.size(); }
      log << QString("insp %1 %2: ngons %3 open %4 nonplanar %5 nonmanifold %6 interior %7 (%8 pieces) hard %9").arg(w[1], w[2]).arg(a).arg(b).arg(c3).arg(d).arg(e).arg(pc).arg(hd); continue; }
    if (w.size() == 1 && w[0] == "lsmesh") { viewport_->beforeFrame(); for (auto& [id, it] : scene_.items) if (it.type == ItemType::IMesh) { const PolyMesh* B = it.base ? it.base.get() : it.poly.get();
        log << QString("mesh %1 %2 (in " + (it.parent >= 0 && scene_.item(it.parent) ? scene_.item(it.parent)->name : QString("root")) + "): %3 faces, %4 mats, uv %5, polypaint %6, textured %7").arg(id).arg(it.name).arg(B ? B->faceCount() : -1).arg(B ? int(B->matNames.size()) : -1).arg(B && !B->cornerUV.empty()).arg(it.poly ? int(it.poly->vcol.size()) : 0).arg(looks::shown(it)) + QString(" · proxy %1 · mask %2").arg(viewport_->maskProxyTris(id)).arg([&] { auto a = viewport_->smArr(id); return int(std::count(a.begin(), a.end(), 1)); }()) + (viewport_->lodOf(id) ? QString(", lod %1 verts %2 colours").arg(viewport_->lodOf(id)->pos.size()).arg(viewport_->lodOf(id)->vcol.size()) : QString()); } continue; }
    if (w.size() == 2 && w[0] == "propshot") { QApplication::processEvents(); QElapsedTimer t; t.start(); while (t.elapsed() < 300) QApplication::processEvents(); if (dpProps_) for (auto* sa : dpProps_->findChildren<QScrollArea*>()) sa->verticalScrollBar()->setValue(w[1].endsWith("_top.png") ? 0 : sa->verticalScrollBar()->maximum()); QApplication::processEvents(); bool ok = dpProps_ && dpProps_->grab().save(w[1]); log << (ok ? "props shot " : "props shot FAILED ") + w[1]; continue; }
    if (w.size() == 2 && w[0] == "setstep") { viewport_->setConv("step", w[1].toDouble(), true); viewport_->beforeFrame(); log << "step " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "llflags") { viewport_->llRing = w[1].contains('r'); viewport_->llBulge = w[1].contains('b'); log << "llflags " + w[1]; continue; }   // (r — the ring closed, b — bulged)
    if (w.size() == 1 && w[0] == "meshcheck") {   // every converted mesh: polygons folded back on a neighbour (the angle between their normals over 120°)
      for (auto& [id, it] : scene_.items) if (it.type == ItemType::Mesh && it.poly) { const PolyMesh& M = *it.poly; std::map<std::pair<int, int>, std::vector<int>> ef; std::vector<Vec3> fn(size_t(M.faceCount())); std::vector<double> area(size_t(M.faceCount())); double meanA = 0;
        for (int f = 0; f < M.faceCount(); f++) { const int a = M.faceStart[size_t(f)], b = M.faceStart[size_t(f) + 1]; Vec3 n; for (int k = a; k < b; k++) { const Vec3& p = M.pos[size_t(M.faceVerts[size_t(k)])]; const Vec3& q = M.pos[size_t(M.faceVerts[size_t(k + 1 < b ? k + 1 : a)])]; n += Vec3((p.y - q.y) * (p.z + q.z), (p.z - q.z) * (p.x + q.x), (p.x - q.x) * (p.y + q.y)); }
          area[size_t(f)] = n.length() * 0.5; meanA += area[size_t(f)] / M.faceCount(); fn[size_t(f)] = n.length() > 0 ? n.normalized() : n; for (int k = a; k < b; k++) { int u = M.faceVerts[size_t(k)], v = M.faceVerts[size_t(k + 1 < b ? k + 1 : a)]; ef[{std::min(u, v), std::max(u, v)}].push_back(f); } }
        int folds = 0, nonMan = 0, open = 0; for (auto& [e, fs] : ef) { if (fs.size() == 1) open++; else if (fs.size() > 2) nonMan++; else if (fn[size_t(fs[0])].dot(fn[size_t(fs[1])]) < -0.5) { folds++; log << QString("  areas %1 %2 (mean %3)").arg(area[size_t(fs[0])], 0, 'g', 3).arg(area[size_t(fs[1])], 0, 'g', 3).arg(meanA, 0, 'g', 3); const Vec3 c = (M.pos[size_t(e.first)] + M.pos[size_t(e.second)]) * 0.5; log << QString("  fold at (%1 %2 %3)").arg(c.x, 0, 'f', 3).arg(c.y, 0, 'f', 3).arg(c.z, 0, 'f', 3); } }
        log << QString("meshcheck \"%1\": v %2 f %3, folded edges %4, open edges %5, edges of 3+ polygons %6").arg(it.name).arg(M.pos.size()).arg(M.faceCount()).arg(folds).arg(open).arg(nonMan); }
      continue; }
    if (w.size() == 1 && w[0] == "zbon") { if (auto* zl = ZBrushLink::get()) { zl->setReceiving(true); zl->checkNow(); } int n = 0; for (auto& [id, it] : scene_.items) if (it.type == ItemType::IMesh) n++; log << QString("zbon: meshes %1, toast \"%2\"").arg(n).arg(ui::lastToast()); continue; }   // (the inbox taken now)
    if (w.size() == 2 && w[0] == "refmenu") { openRefMenu(this, viewport_, &scene_, board_, mapToGlobal(QPoint(300, 80))); QApplication::processEvents(); log << (refMenuShot(w[1]) ? "ref menu shot " + w[1] : QString("ref menu shot FAILED")); closeRefMenu(); continue; }
    if (w.size() >= 2 && w[0] == "refcanvas") { log << "refcanvas " + refCanvas_->testCmd(w); continue; }
    if (w.size() == 2 && w[0] == "sym") { viewport_->setSymmetry(w[1] == "1"); log << "sym " + w[1]; continue; }
    if (w.size() >= 2 && w[0] == "zbrecv") {   // zbrecv <obj>: a mesh as the ZBrush bridge receives it (a second time — replaced in place)
      const QString f = w.mid(1).join(' '); const QString r = receiveMesh(f, QFileInfo(f).completeBaseName(), "ZBrush", viewport_, &scene_); viewport_->beforeFrame();
      int n = 0; for (auto& [id, it] : scene_.items) if (it.type == ItemType::IMesh) n++;
      log << QString("zbrecv %1: %2, meshes %3, toast \"%4\"").arg(QFileInfo(f).fileName(), r.isEmpty() ? "FAILED" : r).arg(n).arg(ui::lastToast()); continue; }
    if (w.size() == 2 && w[0] == "htshot") { QApplication::processEvents(); bool ok = htools_ && htools_->grab().save(w[1]); log << (ok ? "hair tools shot " : "hair tools shot FAILED ") + w[1] + QString(" %1x%2").arg(htools_->width()).arg(htools_->height()); continue; }
    if (w.size() == 2 && w[0] == "ttshot") { QApplication::processEvents(); bool ok = ttools_ && ttools_->grab().save(w[1]); log << (ok ? "toolbar shot " : "toolbar shot FAILED ") + w[1] + QString(" %1x%2").arg(ttools_->width()).arg(ttools_->height()); continue; }
    if (w.size() == 1 && w[0] == "tiso") { bool ok = viewport_->tIsolateSel(); viewport_->beforeFrame(); log << QString("tiso %1 · hidden %2").arg(ok).arg(viewport_->anyHidden()); continue; }
    if (w.size() == 2 && w[0] == "toppie") { openTopoPie(viewport_->mapToGlobal(QPoint(viewport_->width() / 2, viewport_->height() / 2))); QApplication::processEvents(); if (auto* pp = ui::Pie::current()) pp->grab().save(w[1]); log << "toppie " + w[1]; continue; }
    if (w.size() >= 2 && w[0] == "top") { QString op = w[1], r; viewport_->mirrorWeldAns = w.size() > 2 ? w[2].toInt() : 1;
      if (op == "detach") r = viewport_->tDetachSel(); else if (op == "split") r = viewport_->tSplitSel(); else if (op == "splitpg") r = viewport_->tSplitToGroup(); else if (op == "melt") r = viewport_->tMelt();
      else if (op == "mirror") r = viewport_->tMirrorGeo(); else if (op == "grow") r = viewport_->tGrowSel(false); else if (op == "shrink") r = viewport_->tGrowSel(true); else if (op == "selmirror") r = viewport_->tMirrorSel(w.size() > 2 ? w[2].toInt() : 1);
      else if (op == "hard") r = viewport_->tHardEdges(true); else if (op == "soft") r = viewport_->tHardEdges(false);
      else if (op == "conv") r = viewport_->tConvertSel(w[2]); else if (op == "hide") r = viewport_->tHideSel(); else if (op == "unhide") r = viewport_->tUnhideAll(); else if (op == "seltool") { viewport_->setSelTool(w[2]); r = "ok"; }
      else if (op == "flush") { QApplication::processEvents(); r = "flushed " + viewport_->histInfo(); } else if (op == "selset") r = viewport_->tSelSetCreate(); else if (op == "setapply") { viewport_->tSelSetApply(viewport_->topoMesh(), w[2].toInt(), w.size() > 3 ? w[3] : "set"); r = "ok"; }
      else if (op == "sym") { viewport_->tset.sym = w[2] == "1"; r = "ok"; } else if (op == "softsel") { viewport_->tset.soft = w[2] == "1"; if (w.size() > 3) viewport_->tset.softR = w[3].toDouble(); if (w.size() > 4) viewport_->tset.softMode = w[4]; r = QString("soft verts %1").arg(viewport_->softOf(viewport_->topoMesh()).vs.size()); }
      else if (op == "undo") { QApplication::processEvents(); viewport_->undo(); r = "undone"; } else if (op == "redo") { viewport_->redo(); r = "redone"; } else if (op == "setsel") { const auto& S = viewport_->topoSel(); r = QString("f %1 e %2 v %3").arg(S.f.size()).arg(S.e.size()).arg(S.v.size()); }
      else if (op == "pie") { openTopoPie(viewport_->mapToGlobal(QPoint(viewport_->width() / 2, viewport_->height() / 2))); QApplication::processEvents(); if (auto* pp = ui::Pie::current()) pp->grab().save(w[2]); r = "pie"; }
      else if (op == "ids") { const auto& S0 = viewport_->topoSel(); QStringList q; for (int f : S0.f) { if (q.size() > 12) break; q << QString::number(f); } r = "f[" + q.join(",") + "]";   // (top ids: the picked polygons' numbers — the first one's corners on the screen)
        if (!S0.f.empty()) if (const Item* it = scene_.item(viewport_->topoMesh()); it && it->poly) { const PolyMesh& B = *(it->base ? it->base : it->poly); const int f = *S0.f.begin(); Projector Pr(viewport_->camera(), viewport_->width(), viewport_->height());
          for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) { const ScreenPt sp = Pr.toScreen(it->poly->pos[size_t(B.faceVerts[size_t(k)])]); r += QString(" (%1,%2)").arg(int(sp.x)).arg(int(sp.y)); } } }
      else if (op == "click") { QStringList p = w[2].split(QChar(0x2c)); viewport_->topoClickTool(QPointF(p[0].toDouble(), p[1].toDouble()), w.size() > 3 && w[3] == "shift", w.size() > 3 && w[3] == "ctrl"); r = "clicked"; }
      viewport_->mirrorWeldAns = -1; viewport_->beforeFrame(); const auto& S = viewport_->topoSel();
      log << QString("top %1: %2 · sel f %3 e %4 v %5 · hidden %6").arg(w.mid(1).join(QChar(0x20)), r).arg(S.f.size()).arg(S.e.size()).arg(S.v.size()).arg(viewport_->anyHidden()); continue; }
    // (the modelling tools' tests: the pointer simulated — mouse press|move|release|rpress x,y [ctrl] [shift]; a key;
    // a vertex's place on the screen; the active mesh's counts; the tools' edits called)
    if (w.size() >= 3 && (w[0] == "mouse" || w[0] == "mousef")) {   // (mousef: the frame after it waited for and both timed)
      QElapsedTimer mt; mt.start();
      const QStringList p = w[2].split(QChar(0x2c)); const QPointF at(p[0].toDouble(), p[1].toDouble());
      Qt::KeyboardModifiers md; for (int i = 3; i < w.size(); i++) { if (w[i] == "ctrl") md |= Qt::ControlModifier; if (w[i] == "shift") md |= Qt::ShiftModifier; if (w[i] == "alt") md |= Qt::AltModifier; }
      const bool rmb = w[1] == "rpress" || w[1] == "rrelease"; const Qt::MouseButton b = rmb ? Qt::RightButton : Qt::LeftButton;
      const QEvent::Type ty = w[1] == "press" || w[1] == "rpress" ? QEvent::MouseButtonPress : w[1] == "move" ? QEvent::MouseMove : QEvent::MouseButtonRelease;
      static Qt::MouseButtons mheld; if (ty == QEvent::MouseButtonPress) mheld = b;
      QMouseEvent ev(ty, at, viewport_->mapToGlobal(at.toPoint()), ty == QEvent::MouseMove ? Qt::NoButton : b, ty == QEvent::MouseButtonRelease ? Qt::MouseButtons() : mheld, md);
      if (ty == QEvent::MouseButtonRelease) mheld = {};
      QCoreApplication::sendEvent(viewport_, &ev); viewport_->beforeFrame(); QApplication::processEvents();
      if (auto* pp = ui::Pie::current()) pp->close();
      if (w[0] == "mousef") { const qint64 ev0 = mt.elapsed(); const int f0 = viewport_->framesDrawn; viewport_->requestUpdate(); QElapsedTimer to; to.start();
        while (viewport_->framesDrawn == f0 && to.elapsed() < 5000) QApplication::processEvents(QEventLoop::AllEvents, 2);
        log << QString("mousef %1: event %2 ms, frame %3 ms").arg(w.mid(1).join(QChar(0x20))).arg(ev0).arg(mt.elapsed() - ev0); continue; }
      log << "mouse " + w.mid(1).join(QChar(0x20)); continue; }
    if (w.size() >= 3 && w[0] == "scutpts") { std::vector<std::pair<double, double>> rt; for (int i = 2; i < w.size(); i++) { auto q = w[i].split(QChar(0x2c)); if (q.size() == 2) rt.push_back({q[0].toDouble(), q[1].toDouble()}); }   // scutpts <wall> r,t … (fractions): a cut from edge to edge placed
      viewport_->surfCutSetPts(w[1].toInt(), rt); log << viewport_->surfCutInfo(); continue; }
    if (w.size() == 3 && w[0] == "scutstart") { log << viewport_->surfCutStartAt(w[1].toDouble(), w[2].toDouble()); continue; }   // (scutstart r t — the cut begun on a curve there)
    if (w.size() == 1 && w[0] == "scutinfo") { log << viewport_->surfCutInfo(); continue; }   // (the NURBS cut's state: its wall, mode, anchors in the wall's coordinates)
    if (w.size() == 2 && w[0] == "scutkey") { const int k = w[1] == "enter" ? Qt::Key_Return : w[1] == "back" ? Qt::Key_Backspace : Qt::Key_Escape; log << QString("scutkey %1: %2").arg(w[1]).arg(viewport_->surfCutKey(k)); viewport_->beforeFrame(); continue; }   // (the NURBS cut's keys)
    if (w.size() >= 2 && w[0] == "tkey") { const int k = w[1] == "esc" ? Qt::Key_Escape : w[1] == "enter" ? Qt::Key_Return : w[1] == "[" ? Qt::Key_BracketLeft : Qt::Key_BracketRight;
      log << QString("tkey %1: %2").arg(w[1]).arg(viewport_->topoToolKey(k, {})); continue; }
    if (w.size() == 3 && w[0] == "amove") { const int a = w[1].toInt(); auto q = w[2].split(QChar(0x2c)); if (q.size() == 3 && scene_.anchor(a)) { const Vec3 p(q[0].toDouble(), q[1].toDouble(), q[2].toDouble());
        viewport_->runEdit([a, p](Scene& s, Selection&, const CurveCache&) { s.anchor(a)->pos = p; return QString(); }, true); }
      for (int i = 0; i < 4; i++) { viewport_->beforeFrame(); QApplication::processEvents(); } log << "amove " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "apos") { const Anchor* A = scene_.anchor(w[1].toInt()); log << (A ? QString("apos %1: %2 %3 %4").arg(w[1]).arg(A->pos.x, 0, 'f', 3).arg(A->pos.y, 0, 'f', 3).arg(A->pos.z, 0, 'f', 3) : QString("apos: none")); continue; }
    if (w.size() >= 3 && w[0] == "mkcurve") {   // (a curve through the points x,y,z … — selected)
      std::vector<Vec3> pts; for (int i = 1; i < w.size(); i++) { auto q = w[i].split(QChar(0x2c)); if (q.size() == 3) pts.push_back(Vec3(q[0].toDouble(), q[1].toDouble(), q[2].toDouble())); }
      int made = -1;
      viewport_->runEdit([&](Scene& s, Selection& sl, const CurveCache&) { Item& c = s.addItem(ItemType::Curve, -1, "Curve T"); made = c.id; std::vector<int> as; for (auto& p : pts) as.push_back(s.addAnchor(p).id);
        s.item(made)->anchors = as; s.reindex(); sl.items = {made}; sl.anchors.clear(); return QString(); }, true);
      viewport_->beforeFrame(); log << QString("mkcurve: %1").arg(made); continue; }
    if (w.size() >= 2 && w[0] == "braid") {   // (create / set k v / mesh tubes|shell / info — the selected braid)
      int b = -1; for (int id : viewport_->selection().items) if (const Item* it = scene_.item(id); it && it->type == ItemType::Braid) b = id;
      const Vec3 cen = viewport_->camera().sceneCenter; QString r;
      if (w[1] == "hide" && b >= 0) { scene_.item(b)->visible = false; viewport_->liveTubes(); r = "hidden"; }
      else if (w[1] == "create") viewport_->runEdit([&](Scene& s, Selection& sl, const CurveCache& cc) { r = braid::createBraids(s, cc, sl, cen); return r; }, true);
      else if (w[1] == "set" && w.size() == 4 && b >= 0) { QJsonObject p = scene_.item(b)->raw.value("p").toObject(); bool ok = false; const double x = w[3].toDouble(&ok); p.insert(w[2], ok ? QJsonValue(x) : QJsonValue(w[3])); if (w[3] == "true" || w[3] == "false") p.insert(w[2], w[3] == "true"); scene_.item(b)->raw.insert("p", p); viewport_->liveTubes(); viewport_->runEdit([b, cen](Scene& s, Selection&, const CurveCache& cc) { braid::updateConnectors(s, cc, b, cen); return QString(); }, true); r = "set"; }
      else if (w[1] == "ends" && b >= 0) { viewport_->beforeFrame(); r = braid::endReport(scene_, viewport_->hair().curves, *scene_.item(b), cen); }
      else if (w[1] == "locks" && b >= 0) viewport_->runEdit([&](Scene& s, Selection& sl, const CurveCache& cc) { r = braid::toLocks(s, cc, sl, b, cen); return r; }, true);
      else if (w[1] == "movecon" && w.size() == 6 && b >= 0) {   // (movecon <k> dx dy dz — the k-th connector moved, in widths)
        const QJsonArray cs = scene_.item(b)->raw.value("conns").toArray(); const int k = w[2].toInt(); const double W = braid::paramsOf(*scene_.item(b)).width;
        if (k >= 0 && k < cs.size()) if (const Item* c = scene_.item(cs[k].toInt())) for (int a : c->anchors) scene_.anchor(a)->pos += Vec3(w[3].toDouble(), w[4].toDouble(), w[5].toDouble()) * W;
        viewport_->sceneChanged(false); viewport_->beforeFrame(); r = "moved"; }
      else if (w[1] == "mesh" && b >= 0) { viewport_->runEdit([&](Scene& s, Selection& sl, const CurveCache& cc) { r = braid::makeMesh(s, cc, sl, b, cen); return r; }, true);
        for (int id : viewport_->selection().items) if (const Item* it = scene_.item(id); it && it->type == ItemType::IMesh && it->base) { const PolyMesh& M = *it->base; int t3 = 0, q4 = 0, ng = 0; for (int f = 0; f < M.faceCount(); f++) { const int n = M.faceStart[size_t(f) + 1] - M.faceStart[size_t(f)]; (n == 3 ? t3 : n == 4 ? q4 : ng)++; }
          std::map<std::pair<int, int>, int> ec; for (int f = 0; f < M.faceCount(); f++) { const int s0 = M.faceStart[size_t(f)], e0 = M.faceStart[size_t(f) + 1]; for (int k = s0; k < e0; k++) { int a = M.faceVerts[size_t(k)], c = M.faceVerts[size_t(k + 1 < e0 ? k + 1 : s0)]; ec[{std::min(a, c), std::max(a, c)}]++; } }
          int border = 0, nonman = 0; for (auto& [e, n] : ec) { if (n == 1) border++; if (n > 2) nonman++; }
          r += QString(" · %1 verts, %2 polys (%3 tris %4 quads %5 ngons), groups %6, hard %7, uv corners %8, border edges %9, non-manifold %10").arg(M.pos.size()).arg(M.faceCount()).arg(t3).arg(q4).arg(ng).arg(M.groupNames.size()).arg(M.hardEdges.size()).arg(M.cornerUV.size()).arg(border).arg(nonman); } }
      viewport_->beforeFrame();
      if (b < 0) for (int id : viewport_->selection().items) if (const Item* it = scene_.item(id); it && it->type == ItemType::Braid) b = id;
      int parts = 0; for (auto& [k, t] : viewport_->tubes()) if (t.item == b && b >= 0) parts++;
      QString cinfo; if (b >= 0) for (const QJsonValue& x : scene_.item(b)->raw.value("conns").toArray()) { const Item* k = scene_.item(x.toInt(-1)); cinfo += k ? QString(" [%1: %2 anchors, parent %3]").arg(k->id).arg(k->anchors.size()).arg(k->parent) : QString(" [none]"); }
      r += cinfo;
      log << QString("braid %1: %2 · braid %3, drawn parts %4, conns %5").arg(w.mid(1).join(QChar(0x20))).arg(r).arg(b).arg(parts).arg(b >= 0 ? QString::fromUtf8(QJsonDocument(scene_.item(b)->raw.value("conns").toArray()).toJson(QJsonDocument::Compact)) : QString());
      continue; }
    if (w.size() == 3 && w[0] == "vis") { if (Item* it = scene_.item(w[1].toInt())) { it->visible = w[2] == "1"; viewport_->sceneChanged(false); } viewport_->beforeFrame(); log << "vis " + w[1] + " " + w[2]; continue; }   // (vis <item> 0|1 — shown / hidden)
    if (w.size() >= 2 && w[0] == "lobe") {   // (Lobe Brush: info / new / act k / auto / build / clear / sens x / per n / target id)
      auto* vp = viewport_; const QString op = w[1];
      if (op == "target" && w.size() == 3) vp->lb_.m = w[2].toInt();
      if (op == "new") vp->lobeNew();
      if (op == "act" && w.size() == 3) vp->lobeSetActive(w[2].toInt());
      if (op == "sens" && w.size() == 3) vp->lb_.sens = w[2].toDouble();

      if (op == "clear") vp->lobeClear();
      if (op == "apply") log << "lobe apply: " + vp->lobeApply();
      if (op == "flip" && w.size() == 3) vp->lobeFlip(w[2].toInt());
      if (op == "ghost") log << "lobe ghost: " + vp->lobeGhostInfo();
      if (op == "tpl" && w.size() == 4) log << "lobe tpl: " + vp->testLobeTemplate(w[2].toInt(), w[3].toInt());
      if (op == "crev" && w.size() == 3) log << "lobe crev: " + vp->testLobeCrev(w[2].toDouble());
      const int m = vp->lobeTarget(); QStringList cs;
      if (m >= 0) { const auto c = vp->lobeCounts(m); const auto n = vp->lobeNames(m); for (size_t i = 0; i < n.size(); i++) cs << QString("%1:%2").arg(n[i]).arg(c[i + 1]); }
      log << QString("lobe %1: target %2, active %3, %4 · last fill %5").arg(w.mid(1).join(QChar(0x20))).arg(m).arg(m >= 0 ? vp->lobeActive(m) : 0).arg(cs.join(", ")).arg(vp->lb_.lastFill);
      continue; }
    if (w.size() >= 2 && w[0] == "patch") {   // (PatchTool: info / div N / mode e|a|m / dense 0|1 / zone / sel c a / commit / clear)
      auto& P = viewport_->pt_; const QString op = w[1];
      if (op == "div" && w.size() == 3 && P.selC >= 0) P.c[size_t(P.selC)].div = w[2].toInt();
      if (op == "mode" && w.size() == 3 && P.selC >= 0) { P.c[size_t(P.selC)].mode = w[2].at(0).toLatin1(); P.c[size_t(P.selC)].modeAuto = false; }
      if (op == "dense" && w.size() == 3) P.toDense = w[2] == "1";
      if (op == "zone") P.zoneAlt = !P.zoneAlt; else if (op == "rimshort") P.rimShort = !P.rimShort;
      if (op == "fan") P.triFan = !P.triFan;
      if (op == "sel" && w.size() == 4) { P.selC = w[2].toInt(); P.selA = w[3].toInt(); }
      if (op == "clear") viewport_->patchDeleteAll();
      if (op == "del") viewport_->patchDeleteCurve();   // (patch sel c 0; patch del — that curve gone)
      if (op == "kind" && w.size() == 3) P.kind = w[2].at(0).toLatin1();   // (patch kind p|l|s)
      if (op == "width" && w.size() == 3) P.stripW = w[2].toDouble();
      if (op == "update") viewport_->patchRedetect();
      if (op == "spanwheel" && w.size() == 5) { viewport_->setMouseAt(QPointF(w[2].toDouble(), w[3].toDouble())); log << QString("spanwheel %1").arg(viewport_->patchSpanWheel(w[4].toInt())); }   // (patch spanwheel x y steps)
      auto xy = [&](const QString& s) { const auto p = s.split(QChar(0x2c)); return QPointF(p.value(0).toDouble(), p.value(1).toDouble()); };
      if (op == "ring" && w.size() == 4) viewport_->patchRingEnd(xy(w[2]), xy(w[3]));   // (patch ring x,y x,y — the stroke)
      if (op == "anch" && w.size() == 3) { const int c = w[2].toInt(); QString s; Projector Pr(viewport_->camera(), viewport_->width(), viewport_->height());   // (patch anch c — its anchors on the screen)
        if (c >= 0 && c < int(P.c.size())) for (auto& a : P.c[size_t(c)].a) { const auto q = Pr.toScreen(a); s += QString(" %1,%2/%3").arg(q.x, 0, 'f', 0).arg(q.y, 0, 'f', 0).arg(viewport_->camera().pos.distanceTo(a), 0, 'f', 2); } log << "anch" + s; }
      if (op == "curve" && w.size() >= 4) {                       // (patch curve x,y x,y … — its anchors snapped as clicks would, ended as by a double click)
        Viewport::PatchCurve C; for (int i = 2; i < w.size(); i++) if (auto p = viewport_->patchSnapAt(xy(w[i]))) C.a.push_back(*p);
        if (C.a.size() >= 2) { viewport_->patchPush(); P.c.push_back(C); P.selC = int(P.c.size()) - 1; viewport_->patchChanged(); viewport_->patchSplitBy(P.selC); } }
      if (op == "onrefs" && w.size() == 5) { QElapsedTimer t; t.start(); const Vec3 q = viewport_->patchOnRefsAt(Vec3(w[2].toDouble(), w[3].toDouble(), w[4].toDouble())); const qint64 a = t.elapsed(); viewport_->patchOnRefsAt(q); log << QString("onrefs %1 %2 %3 · %4 ms, again %5 ms").arg(q.x).arg(q.y).arg(q.z).arg(a).arg(t.elapsed() - a); }   // (patch onrefs x y z — the nearest surface, timed)
      if (op == "order" && w.size() == 3) viewport_->patchOrderClick(w[2].toInt());   // (patch order c — Loft's order: c clicked)
      if (op == "show" && w.size() == 3) P.show = w[2] == "1";
      if (op == "fillop" && w.size() == 3) P.fillOp = w[2].toDouble();
      if (op == "addw" && w.size() >= 8) { Viewport::PatchCurve C; for (int i = 2; i + 2 < w.size(); i += 3) C.a.push_back(Vec3(w[i].toDouble(), w[i + 1].toDouble(), w[i + 2].toDouble())); C.div = 6; viewport_->patchPush(); P.c.push_back(C); P.selC = int(P.c.size()) - 1; }   // (patch addw x y z x y z … — a curve through world points)
      if (op == "commit") log << "patch commit: " + viewport_->patchCommit();
      viewport_->patchChanged(); viewport_->patchBuild(); viewport_->beforeFrame();
      int tri = 0, quad = 0, ngon = 0; for (auto& f : P.F) (f.size() == 3 ? tri : f.size() == 4 ? quad : ngon)++;
      QStringList cs; for (auto& c : P.c) cs << QString("%1a/%2%3").arg(c.a.size()).arg(c.div).arg(QChar(c.mode));
      int reuse = 0; for (int v : P.vOf) reuse += v >= 0;
      log << QString("patch %1: curves [%2] draw %3 sel %4/%5 · target %6 · patches %7 · %8 pts (%9 of the mesh) · %10 polys (%11 tris, %12 quads, %13 n-gons) · zone %14 choice %15 · %16")
               .arg(op).arg(cs.join(" ")).arg(P.draw).arg(P.selC).arg(P.selA).arg(P.target).arg(P.patches).arg(P.P.size()).arg(reuse).arg(P.F.size()).arg(tri).arg(quad).arg(ngon).arg(P.zone.size()).arg(P.zoneChoice).arg(P.note);
      continue; }
    if (w.size() == 2 && w[0] == "tproj") { const int m = viewport_->topoMesh(); const Item* it = scene_.item(m); QString out;
      for (const QString& x : w[1].split(QChar(0x2c))) { const int v = x.toInt(); const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr; if (B && v < int(B->pos.size())) { auto s = Projector(viewport_->camera(), viewport_->width(), viewport_->height()).toScreen(B->pos[size_t(v)]); out += QString(" %1:%2,%3").arg(v).arg(s.x, 0, 'f', 1).arg(s.y, 0, 'f', 1); } }
      log << "tproj" + out; continue; }
    if (w.size() == 1 && w[0] == "tcount") { const int m = viewport_->topoMesh(); const Item* it = scene_.item(m); const auto& S = viewport_->topoSel();
      int tri = 0, quad = 0, ngon = 0; if (it && it->base) for (int f = 0; f < it->base->faceCount(); f++) { int n = it->base->faceStart[size_t(f) + 1] - it->base->faceStart[size_t(f)]; (n == 3 ? tri : n == 4 ? quad : ngon)++; }
      log << QString("tcount: %1 verts, %2 polys (%3 tris, %4 quads, %5 n-gons) · mode %6 · sel f %7 e %8 v %9 · curves %10").arg(it && it->base ? int(it->base->pos.size()) : 0).arg(it && it->base ? it->base->faceCount() : 0)
               .arg(tri).arg(quad).arg(ngon).arg(viewport_->topoMode).arg(S.f.size()).arg(S.e.size()).arg(S.v.size()).arg(std::count_if(scene_.items.begin(), scene_.items.end(), [](auto& kv) { return kv.second.type == ItemType::Curve; })); continue; }
    if (w.size() >= 2 && w[0] == "mt") { QString r; const QString op = w[1];
      if (op == "pivotc") { const int m = viewport_->topoMesh(); const Item* it = scene_.item(m); viewport_->setMeshPivot(m, (it->base ? it->base : it->poly)->bounds().center()); log << "mt pivotc"; continue; }
      if (op == "gzalign") { r = viewport_->gzAlign(w.value(2), w.value(3)); viewport_->beforeFrame(); log << "mt gzalign: " + r; continue; }
      if (op == "gzinfo") { viewport_->updateGizmo(); const int m = viewport_->topoMesh(); Quat g = m >= 0 ? viewport_->meshGizmoQ(m) : Quat{}; auto gs = viewport_->gizmoScreen();
        Quat q = viewport_->gizmoAxes(); auto ax = [&](Vec3 e) { Vec3 v = q.rotate(e); return QString("(%1 %2 %3)").arg(v.x, 0, 'f', 3).arg(v.y, 0, 'f', 3).arg(v.z, 0, 'f', 3); };
        log << QString("gzinfo: orient %1 · X %2 Y %3 Z %4 · gq %5 %6 %7 %8 · screen %9").arg(viewport_->tset.gzOrient, ax({1, 0, 0}), ax({0, 1, 0}), ax({0, 0, 1})).arg(g.x).arg(g.y).arg(g.z).arg(g.w).arg(gs ? QString("%1,%2").arg(gs->x()).arg(gs->y()) : QString("-"))
          + (m >= 0 ? [&] { const Vec3 p = meshmods::meshMatrix(scene_, *scene_.item(m)).apply(viewport_->meshPivotLocal(m)); return QString(" · pivot %1 %2 %3").arg(p.x, 0, 'f', 3).arg(p.y, 0, 'f', 3).arg(p.z, 0, 'f', 3); }() : QString()); continue; }
      if (op.startsWith("uv") && op != "uvx") { const int m = viewport_->topoMesh(); const int a = w.size() > 2 ? w[2].toInt() : -2;   // the UV Sets / Mats: mt uvinfo | uvpickset i | uvpickmat k | uvsetnew | uvsetapply i | uvmatnew | uvmatapply k | uvmatdel k | uvmatren k name | uvsetren i name | uvtoset k i | uvpack
        if (op == "uvinfo") { for (auto& s : viewport_->uvSets(m)) { QStringList ns; for (int k : s.mats) ns << QString::number(k); r += s.name + "[" + ns.join(",") + "] "; } r += "| mats:"; for (auto& x : viewport_->uvMats(m)) r += QString(" %1=%2(%3)").arg(x.idx).arg(x.name).arg(x.faces);
          r += QString(" | picked %1 %2 %3").arg(viewport_->uvPicked.id).arg(viewport_->uvPicked.set).arg(viewport_->uvPicked.mat); }
        else if (op == "uvpickset") viewport_->uvPickSet(m, a); else if (op == "uvpickmat") viewport_->uvPickMat(m, a);
        else if (op == "uvfreenew") r = viewport_->uvSetNewFree(); else if (op == "uvput") r = viewport_->uvSetPutPicked(w.mid(3).join(QChar(0x20)), w.value(2) == "1"); else if (op == "uvputm") r = viewport_->uvSetPutPicked(w.mid(3).join(QChar(0x20)), false, Viewport::SetClash(w.value(2).toInt())); else if (op == "uvclash") { const auto c = viewport_->uvSetClash(w.mid(2).join(QChar(0x20))); r = QString("clash %1 in %2").arg(c.n).arg(c.sets.join("+")); } else if (op == "uvnames") { for (const QString& n : viewport_->uvSetNamesAll()) r += n + ";"; r += " free: "; for (const QString& n : viewport_->uvFreeSets()) r += n + ";"; r += " sel: " + QString::number(m >= 0 ? int(viewport_->tSelFaces(m).size()) : -1); if (const Item* sm = scene_.item(m)) if (const PolyMesh* sb = sm->base ? sm->base.get() : sm->poly.get()) r += " seams: " + QString::number(sb->seams.size()); if (m >= 0 && !viewport_->uvNone(m)) { r += " faces:"; for (auto& s : viewport_->uvSets(m)) r += " " + QString::number(s.faces); } }
        else if (op == "uvselfaces") { viewport_->setTopoMode("poly"); std::vector<int> fs; for (int i = 0; i < a; i++) fs.push_back(i); viewport_->tSelectFaces(m, fs); r = "selected " + QString::number(a); } else if (op == "uvnew") r = viewport_->uvSetFromShells(-1);
        else if (op == "uvsetnew") r = viewport_->uvSetNew(m); else if (op == "uvsetapply") r = viewport_->uvSetApply(m, a); else if (op == "uvmatnew") r = viewport_->uvMatNew(m);
        else if (op == "uvmatapply") r = viewport_->uvMatApply(m, a); else if (op == "uvmatdel") r = viewport_->uvMatDelete(m, a); else if (op == "uvtoset") r = viewport_->uvMatToSet(m, a, w.size() > 3 ? w[3].toInt() : 0);
        else if (op == "uvmatren") r = viewport_->uvMatRename(m, a, w.mid(3).join(QChar(0x20))); else if (op == "uvsetren") r = viewport_->uvSetRename(m, a, w.mid(3).join(QChar(0x20)));
        else if (op == "uvpack") r = viewport_->uvPack(true); else if (op == "uvselmat") r = QString::number(viewport_->uvSelectMats(m, {a}));
        else if (op == "uvsetdup") r = viewport_->uvSetDuplicate(m, a); else if (op == "uvsetmerge") r = viewport_->uvSetMerge(m, a, w.value(3).toInt(), w.value(4) == "1");
        else if (op == "uvsetmove") r = viewport_->uvSetMove(m, a, w.value(3).toInt()); else if (op == "uvsetout") r = viewport_->uvSetTakeOut(m, a); else if (op == "uvmatmerge") r = viewport_->uvMatMerge(m, a, w.value(3).toInt());
        else if (op == "uvfirstmat") { auto fs = viewport_->uvMatFaces(m, {a}); r = fs.empty() ? QString("-") : QString::number(fs[0]); }
        else if (op == "texon") { lookChanged(viewport_, {m}, [](QJsonObject& L) { L.insert("tex", true); }); r = "tex on"; }
        else if (op == "uvsample") { const Item* it = scene_.item(m); const PolyMesh* B = it->base ? it->base.get() : it->poly.get(); const int f = a; if (f >= 0 && f < B->faceCount()) { const auto& u = B->cornerUV[size_t(B->faceStart[size_t(f)])]; r = QString("face %1 uv %2 %3 live %4 sets %5").arg(f).arg(u[0]).arg(u[1]).arg(f < int(B->faceLive.size()) ? B->faceLive[size_t(f)] : -9).arg(f < int(B->faceSets.size()) ? B->faceSets[size_t(f)] : 0u); } }
        viewport_->beforeFrame(); log << "mt " + w.mid(1).join(QChar(0x20)) + ": " + r; continue; }
      if (op == "delsel") r = viewport_->tDeleteSel(); else
      if (op == "ghost") r = viewport_->ghostInfo(); else if (op == "revealall") r = viewport_->testRevealAll(); else
      if (op == "extrude") r = viewport_->testExtrude(); else
      if (op == "invert") r = viewport_->tInvertSel(); else
      if (op == "selall") r = viewport_->tSelectAll(); else if (op == "relax") r = viewport_->tRelaxMesh(); else
      if (op == "weld") r = viewport_->weldNow(nullptr); else if (op == "weldat" && w.size() >= 3) { const QPointF p(w[2].section(QChar(0x2c), 0, 0).toDouble(), w[2].section(QChar(0x2c), 1, 1).toDouble()); r = viewport_->weldNow(&p); } else if (op == "bevel") r = viewport_->testBevel(); else if (op == "f2v") { const int mm = viewport_->topoMesh(); auto vs = viewport_->tSelVerts(mm, QChar(0x66).toLatin1()); viewport_->setTopoMode("vert"); viewport_->tselApply(mm, vs, "set", QChar(0x76).toLatin1()); r = QString::number(vs.size()) + " vertices"; } else if (op == "bridge") r = viewport_->bridgeNow(); else if (op == "circles") r = viewport_->insetCircles();
      else if (op == "knife") r = viewport_->knifeCut(); else if (op == "state") r = viewport_->ttState(); else if (op == "ngons") r = viewport_->tSelectNgons(false); else if (op == "fill") r = viewport_->fillApply(); else if (op == "holes") r = viewport_->fillHolesInfo();
      else if (op == "alignN") r = viewport_->tAlignNormals(); else if (op == "reverseN") r = viewport_->tReverseNormals(); else if (op == "relax") r = viewport_->tRelaxMesh();
      else if (op == "collapse") r = viewport_->tCollapseEdges(); else if (op == "snapgeom") r = viewport_->tSnapToGeom(); else if (op == "shellcheck") r = viewport_->testShell(); else if (op == "fixmesh") r = viewport_->tFixMesh(); else if (op == "optimize") r = viewport_->tOptimize(); else if (op == "mirror") { r = viewport_->tMirrorMesh(); r += " [" + viewport_->lastReplaceInfo_ + "]"; } else if (op == "identical") r = viewport_->tApplyIdentical(); else if (op == "symgrid") r = viewport_->testSymGrid(); else if (op == "chain") r = viewport_->testChain();
      else if (op == "marks") { const Item* it = scene_.item(viewport_->topoMesh()); const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr; if (B) { auto mid = [&](int a, int b) { const Vec3 c = (B->pos[size_t(a)] + B->pos[size_t(b)]) * 0.5; return QString("(%1 %2 %3)").arg(c.x, 0, 'f', 1).arg(c.y, 0, 'f', 1).arg(c.z, 0, 'f', 1); };
        r = QString("seams %1:").arg(B->seams.size()); for (auto& e : B->seams) r += mid(e[0], e[1]); r += QString(" hard %1:").arg(B->hardEdges.size()); for (auto& e : B->hardEdges) r += mid(e[0], e[1]); r += QString(" creases %1:").arg(B->creases.size()); for (auto& c : B->creases) r += mid(c.a, c.b); } }
      else if (op == "pgsel" && w.size() >= 3) { const int mm = viewport_->topoMesh(); const Item* it = scene_.item(mm); const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr; std::vector<int> l;   // (mt pgsel g — the polygons of polygroup g)
        if (B) for (int f = 0; f < B->faceCount(); f++) if (size_t(f) < B->faceGroup.size() && B->faceGroup[size_t(f)] == w[2].toInt()) l.push_back(f); viewport_->tselApply(mm, l, "set", 'f'); r = QString::number(l.size()) + " polygons"; }
      else if (op == "rough" && w.size() >= 3) { const int mm = viewport_->topoMesh(); const Item* it = scene_.item(mm); const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr;   // (mt rough g — polygroup g's inner vertices: how far each is off its neighbours' middle, on average; the moved distance from the file)
        if (B) { std::map<int, std::set<int>> nb; std::map<std::pair<int, int>, int> ec; for (int f = 0; f < B->faceCount(); f++) { if (B->faceGroup[size_t(f)] != w[2].toInt()) continue; const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1];
            for (int k = s; k < e; k++) { const int a = B->faceVerts[size_t(k)], b = B->faceVerts[size_t(k + 1 < e ? k + 1 : s)]; nb[a].insert(b); nb[b].insert(a); ec[{std::min(a, b), std::max(a, b)}]++; } }
          std::set<int> rimv; for (auto& [e, c] : ec) if (c == 1) { rimv.insert(e.first); rimv.insert(e.second); }
          double sum = 0, sn = 0, dn = 0; int n = 0; std::vector<double> nd; for (auto& [v, ns] : nb) { if (rimv.count(v)) continue; Vec3 c; for (int u : ns) c += B->pos[size_t(u)]; c = c * (1.0 / ns.size()); sum += c.distanceTo(B->pos[size_t(v)]); n++;
            std::vector<int> nv(ns.begin(), ns.end()); Vec3 nr; for (size_t i = 0; i < nv.size(); i++) for (size_t j = i + 1; j < nv.size(); j++) { Vec3 x = (B->pos[size_t(nv[i])] - B->pos[size_t(v)]).cross(B->pos[size_t(nv[j])] - B->pos[size_t(v)]); if (nr.lengthSq() > 0 && x.dot(nr) < 0) x = x * -1.0; nr += x; }
            if (nr.lengthSq() > 0) { const double d = (c - B->pos[size_t(v)]).dot(nr.normalized()); sn += std::abs(d); nd.push_back(d); } }
          double mean = 0; for (double d : nd) mean += d; mean /= std::max<size_t>(1, nd.size()); for (double d : nd) dn += std::abs(d - mean);   // (dn: the bumps — off the panel's even bend)
          r = QString("inner %1, mean off-middle %2, across %3, bumps %4").arg(n).arg(n ? sum / n : 0, 0, 'g', 4).arg(nd.empty() ? 0 : sn / nd.size(), 0, 'g', 4).arg(nd.empty() ? 0 : dn / nd.size(), 0, 'g', 4);
          { Box3 bx; for (auto& [v, ns] : nb) bx.expand(B->pos[size_t(v)]); const Vec3 s = bx.size(); r += QString(", box %1 x %2 x %3").arg(s.x, 0, 'f', 2).arg(s.y, 0, 'f', 2).arg(s.z, 0, 'f', 2); } } }
      else if (op == "selopen") r = QString::number(viewport_->tSelectOpenEdges()) + " open edges";
      else if (op == "pdo" && w.size() >= 3) { viewport_->tdisp.pdo = w[2].toDouble(); viewport_->tdisp.pdoOn = viewport_->tdisp.pdo > 0; viewport_->topoDisplayChanged(); r = "pdo"; }
      else if (op == "vdump" && w.size() >= 3) { const Item* it = scene_.item(viewport_->topoMesh()); const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr; if (B) {
        // (the polygons round a vertex: each corner's vertex, its normal; the polygon's own normal)
        const int v = w[2].toInt(); for (int f = 0; f < B->faceCount(); f++) { const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1]; bool has = false; for (int k = s; k < e; k++) has |= B->faceVerts[size_t(k)] == v; if (!has) continue;
          Vec3 n; for (int k = s + 1; k < e - 1; k++) n += (B->pos[size_t(B->faceVerts[size_t(k)])] - B->pos[size_t(B->faceVerts[size_t(s)])]).cross(B->pos[size_t(B->faceVerts[size_t(k + 1)])] - B->pos[size_t(B->faceVerts[size_t(s)])]); n = n.normalized();
          r += QString("f%1 n(%2 %3 %4):").arg(f).arg(n.x, 0, 'f', 2).arg(n.y, 0, 'f', 2).arg(n.z, 0, 'f', 2);
          for (int k = s; k < e; k++) { const auto& c = B->cornerNormal.size() == B->faceVerts.size() ? B->cornerNormal[size_t(k)] : std::array<float, 3>{0, 0, 0}; r += QString(" v%1(%2 %3 %4)").arg(B->faceVerts[size_t(k)]).arg(c[0], 0, 'f', 2).arg(c[1], 0, 'f', 2).arg(c[2], 0, 'f', 2); }
          r += "\n"; } } }
      else if (op == "rimdump") { const Item* it = scene_.item(viewport_->topoMesh()); const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr; if (B) {
        // (the open edges' vertices: their faces, the points lying on them, each corner's normal against its face's)
        std::map<std::pair<int, int>, int> ec; const int nf = B->faceCount(); std::vector<Vec3> fn(static_cast<size_t>(nf));
        for (int f = 0; f < nf; f++) { const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) { const int a = B->faceVerts[size_t(k)], b = B->faceVerts[size_t(k + 1 < e ? k + 1 : s)];
          const Vec3& p = B->pos[size_t(a)]; const Vec3& q = B->pos[size_t(b)]; fn[size_t(f)].x += (p.y - q.y) * (p.z + q.z); fn[size_t(f)].y += (p.z - q.z) * (p.x + q.x); fn[size_t(f)].z += (p.x - q.x) * (p.y + q.y); ec[{std::min(a, b), std::max(a, b)}]++; } }
        std::set<int> rv; for (auto& [e, c] : ec) if (c == 1) { rv.insert(e.first); rv.insert(e.second); }
        r = QString("open verts %1 corners %2/%3\n").arg(rv.size()).arg(B->cornerNormal.size()).arg(B->faceVerts.size()); int shown = 0;
        const int skip = w.size() >= 3 ? w[2].toInt() : 0; int idx = 0;
        for (int v0 : rv) { if (idx++ < skip) continue; if (shown++ >= 6) break; QString s = QString("v%1 (%2 %3 %4) near:").arg(v0).arg(B->pos[size_t(v0)].x, 0, 'f', 2).arg(B->pos[size_t(v0)].y, 0, 'f', 2).arg(B->pos[size_t(v0)].z, 0, 'f', 2);
          std::vector<int> grp{v0}; for (int u = 0; u < int(B->pos.size()); u++) if (u != v0 && B->pos[size_t(u)].distanceTo(B->pos[size_t(v0)]) < 0.05) { s += QString(" v%1 d=%2").arg(u).arg(B->pos[size_t(u)].distanceTo(B->pos[size_t(v0)]), 0, 'g', 2); grp.push_back(u); }
          for (int v : grp) for (int f = 0; f < nf; f++) for (int k = B->faceStart[size_t(f)]; k < B->faceStart[size_t(f) + 1]; k++) if (B->faceVerts[size_t(k)] == v) { const Vec3 n = fn[size_t(f)].normalized(); s += QString("\n  v%1").arg(v);
            Vec3 c; if (B->cornerNormal.size() == B->faceVerts.size()) c = Vec3(B->cornerNormal[size_t(k)][0], B->cornerNormal[size_t(k)][1], B->cornerNormal[size_t(k)][2]);
            s += QString("\n   f%1 n(%2 %3 %4) c(%5 %6 %7) dot %8").arg(f).arg(n.x, 0, 'f', 2).arg(n.y, 0, 'f', 2).arg(n.z, 0, 'f', 2).arg(c.x, 0, 'f', 2).arg(c.y, 0, 'f', 2).arg(c.z, 0, 'f', 2).arg(c.dot(n), 0, 'f', 2); }
          r += s + "\n"; } } }
      else if (op == "nsmooth") r = viewport_->tNormalsSmooth(); else if (op == "nback") r = viewport_->tNormalsFromBack(); else if (op == "nset" && w.size() >= 3) r = viewport_->tNormalsSet(w[2]); else if (op == "vsym" && w.size() >= 3) r = viewport_->vsymCreate(w[2]); else if (op == "vsymapply") { int g = -1; for (int id : viewport_->selection().items) if (int x = viewport_->vsymGroupOf(id); x >= 0) g = x; r = viewport_->vsymApply(g); }
          else if (op == "subdivide") { if (w.size() >= 3) viewport_->ttools.subdLevels = std::clamp(w[2].toInt(), 1, 4); if (w.size() >= 4) viewport_->ttools.subdSmooth = w[3] == "1"; r = viewport_->tSubdivide(); }   // (mt subdivide <levels> <smooth 0|1>)
          else if (op == "kcn") r = QString("knife curves %1").arg(viewport_->knifeCurves());
          else if (op == "subdiv") r = viewport_->subdivCreate();
          else if (op == "subdivset" && w.size() >= 4) { int g = -1; for (int id : viewport_->selection().items) if (int x = viewport_->subdivGroupOf(id); x >= 0) g = x;   // (mt subdivset <level|grid|smoothOpen|uv> <value>)
            const QString k = w[2], val = w[3]; viewport_->runEdit([g, k, val](Scene& s, Selection&, const CurveCache&) { Item* it = s.item(g); if (!it) return QString("!no group"); QJsonObject o = it->raw.value("subdiv").toObject();
              if (k == "level") o.insert(k, val.toInt()); else if (k == "uv") o.insert(k, val); else o.insert(k, val == "1"); it->raw.insert("subdiv", o); return QString("subdiv %1=%2").arg(k, val); }, false); r = "subdivset"; }
          else if (op == "subdivrec") { int g = -1; for (int id : viewport_->selection().items) if (int x = viewport_->subdivGroupOf(id); x >= 0) g = x; r = viewport_->subdivReconstruct(g); }
          else if (op == "subdivapply") { int g = -1; for (int id : viewport_->selection().items) if (int x = viewport_->subdivGroupOf(id); x >= 0) g = x; r = viewport_->subdivApply(g); }
      else if (op == "vis" && w.size() >= 4) { if (Item* it = scene_.item(w[2].toInt())) { it->visible = w[3] == "1"; viewport_->sceneChanged(false); } r = "vis"; }
      else if (op == "orbit" && w.size() >= 3) { auto p = w[2].split(QChar(0x2c)); viewport_->camera().orbit(p[0].toDouble(), p[1].toDouble()); viewport_->requestUpdate(); r = "orbit"; }
      else if (op == "look" && w.size() >= 3) { auto p = w[2].split(QChar(0x2c)); const Vec3 c(p[0].toDouble(), p[1].toDouble(), p[2].toDouble()); const double rr = p.size() > 3 ? p[3].toDouble() : 3; Box3 b; b.expand(c - Vec3(rr, rr, rr)); b.expand(c + Vec3(rr, rr, rr)); viewport_->fitToBox(b); r = "look"; }
      else if (op == "fillat" && w.size() >= 3) { auto p = w[2].split(QChar(0x2c)); viewport_->fillAimAt(Vec3(p[0].toDouble(), p[1].toDouble(), p[2].toDouble())); r = viewport_->ttState(); } else if (op == "chain") r = viewport_->chainCreate(); else if (op == "reapply") r = viewport_->modelReapply();
      else if (op == "set" && w.size() >= 4) { auto& T = viewport_->ttools; const QString k = w[2]; const double x = w[3].toDouble();
        if (k == "normMode") T.normMode = w[3]; else if (k == "normNose") T.normNose = x; else if (k == "weldMode") T.weldMode = w[3]; else if (k == "weldPos") T.weldPos = w[3]; else if (k == "bevTop") T.bevTop = x != 0; else if (k == "bevSelSides") T.bevSelSides = x != 0; else if (k == "bevSelTop") T.bevSelTop = x != 0; else if (k == "brushAll") viewport_->tset.brushAll = x != 0; else if (k == "planeMode") T.planeMode = w[3]; else if (k == "loopCuts") T.loopCuts = int(x); else if (k == "loopPct") T.loopPct = x;
        else if (k == "nBrush") T.nBrush = x != 0; else if (k == "normOp") T.normOp = x; else if (k == "normLen") T.normLen = x; else if (k == "normShow") T.normShow = x != 0; else if (k == "nBrushVal") T.nBrushVal = x; else if (k == "normSmooth") T.normSmooth = x; else if (k == "planeCuts") T.planeCuts = int(x); else if (k == "planeSpacing") T.planeSpacing = x; else if (k == "bevOffset") T.bevOffset = x; else if (k == "bevSmart") T.bevSmart = x != 0; else if (k == "bevBevel") T.bevBevel = x; else if (k == "bevSeg") T.bevSeg = int(x);
        else if (k == "insOffset") T.insOffset = x; else if (k == "insCircleN") T.insCircleN = int(x); else if (k == "insCircleR") T.insCircleR = x; else if (k == "brSeg") T.brSeg = int(x); else if (k == "knifeThrough") T.knifeThrough = x != 0;
        else if (k == "chainEqual") T.chainEqual = x != 0; else if (k == "fillMode") T.fillMode = w[3]; else if (k == "soft") viewport_->tset.soft = x != 0; else if (k == "snap") viewport_->tset.snap = x != 0; else if (k == "fillSub") T.fillSub = int(x); else if (k == "mirType") T.mirType = w[3]; else if (k == "mirReplace") T.mirReplace = x != 0; else if (k == "mirAxis") T.mirAxis = int(x); else if (k == "mirLocal") T.mirLocal = x != 0; else if (k == "mirCount") T.mirCount = int(x); else if (k == "optWeld") T.optWeld = x; else if (k == "optMode") T.optMode = w[3]; else if (k == "fillFlow") T.fillFlow = x; else if (k == "insGroups") T.insGroups = x != 0; else if (k == "bevBottom") T.bevBottom = x != 0;
        r = "set"; }
      viewport_->beforeFrame(); log << QString("mt %1: %2").arg(w.mid(1).join(QChar(0x20)), r); continue; }
    if (w.size() == 2 && w[0] == "tconv") { viewport_->tConvertSel(w[1]); log << "tconv " + viewport_->topoMode; continue; }
    if (w.size() == 2 && w[0] == "tmode") { viewport_->setTopoMode(w[1]); log << "tmode " + viewport_->topoMode; continue; }
    if (w.size() == 2 && w[0] == "tselx") {   // (the active mesh's polygons whose middle is on the − / + side of X = 0, world)
      const int m = viewport_->topoMesh(); const Item* it = scene_.item(m); std::vector<int> l; int symV = 0, nV = 0;
      if (it) { const PolyMesh* B = it->base ? it->base.get() : it->poly.get(); const Affine M = meshmods::meshMatrix(scene_, *it);
        for (int f = 0; f < B->faceCount(); f++) { Vec3 c; const int s = B->faceStart[size_t(f)], e = B->faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) c += M.apply(B->pos[size_t(B->faceVerts[size_t(k)])]); c = c * (1.0 / (e - s));
          if (w[1] == "neg" ? c.x < 0 : c.x > 0) l.push_back(f); }
        // (how symmetric: the vertices with a mirror twin across X)
        std::vector<Vec3> W; for (auto& p : B->pos) W.push_back(M.apply(p)); const double eps = B->bounds().size().length() * 1e-4;
        for (auto& p : W) { nV++; for (auto& q : W) if (std::abs(q.x + p.x) < eps && std::abs(q.y - p.y) < eps && std::abs(q.z - p.z) < eps) { symV++; break; } } }
      viewport_->tselApply(m, l, "set", 'f'); log << QString("tselx %1: %2 polygons · %3 of %4 vertices have a mirror twin").arg(w[1]).arg(l.size()).arg(symV).arg(nV); continue; }
    if (w.size() == 2 && w[0] == "ndump") {               // ndump <file>: the active mesh's polygons — "f sel" then a line per corner: world position, normal
      const int m = viewport_->topoMesh(); const Item* it = scene_.item(m); QFile f(w[1]);
      if (it && f.open(QIODevice::WriteOnly | QIODevice::Text)) { const PolyMesh& B = *(it->base ? it->base : it->poly); const Affine M = meshmods::meshMatrix(scene_, *it);
        const auto sel = viewport_->tSelFaces(m); std::set<int> S(sel.begin(), sel.end()); const auto H = hardCornerNormals(B); QTextStream o(&f);
        for (int fc = 0; fc < B.faceCount(); fc++) { o << "f " << int(S.count(fc)) << "\n";
          for (int k = B.faceStart[size_t(fc)]; k < B.faceStart[size_t(fc) + 1]; k++) { const Vec3 p = M.apply(B.pos[size_t(B.faceVerts[size_t(k)])]);
            const Vec3 n = M.normal(B.cornerNormal.size() == B.faceVerts.size() ? Vec3(B.cornerNormal[size_t(k)][0], B.cornerNormal[size_t(k)][1], B.cornerNormal[size_t(k)][2]) : H[size_t(k)]);
            o << p.x << " " << p.y << " " << p.z << " " << n.x << " " << n.y << " " << n.z << "\n"; } } }
      log << "ndump " + w[1]; continue; }
    if (w.size() == 3 && w[0] == "selset") { viewport_->tSelSetApply(w[1].toInt(), w[2].toInt(), "set"); viewport_->beforeFrame(); log << "selset " + w[1] + " " + w[2]; continue; }   // selset <mesh> <index>: a kept selection set made the selection
    if (w.size() == 3 && w[0] == "tsel") { std::vector<int> l; for (const QString& x : w[2].split(QChar(0x2c))) { const auto ab = x.split('-');   // (a-b: a range)
        if (ab.size() == 2) for (int i = ab[0].toInt(); i <= ab[1].toInt(); i++) l.push_back(i); else l.push_back(x.toInt()); } viewport_->tselApply(viewport_->topoMesh(), l, "set", w[1][0].toLatin1()); log << "tsel " + w[1]; continue; }
    if (w.size() >= 4 && w[0] == "tg") { int m = w[2].toInt(), g = w[3].toInt(), g2 = w.size() > 4 ? w[4].toInt() : -1; QString op = w[1], r;
      if (op == "del") r = viewport_->tgDelete(m, g); else if (op == "dup") r = viewport_->tgDuplicate(m, g); else if (op == "merge") r = viewport_->tgMerge(m, g, g2);
      else if (op == "detach") r = viewport_->tgDetach(m, g); else if (op == "border") r = viewport_->tGroupBorder(m, g); else if (op == "ident") r = viewport_->tSelectIdentical(m, g);
      else if (op == "sym") r = viewport_->tSelectSymmetric(m, g); else if (op == "move") r = viewport_->tgMoveSelHere(m, g); else if (op == "bake") r = viewport_->topoBakeXform(m);
      else if (op == "pick") { viewport_->tgPick.id = m; viewport_->tgPick.set.insert(g); r = "picked"; } else if (op == "hide" || op == "show") { viewport_->tgSetVisible(m, {g}, op == "show"); r = op; }
      else if (op == "selgrp") { const Item* it = scene_.item(m); const PolyMesh* B = it->base ? it->base.get() : it->poly.get(); viewport_->setTopoMode("poly"); r = QString::number(viewport_->tSelectFaces(m, topo::groupFaces(*B, g))); }
      else if (op == "occ") r = QString::number(viewport_->maskOccluded(m, g));
      else if (op == "maskin") { const Item* it = scene_.item(m); const PolyMesh* B = it->base ? it->base.get() : it->poly.get(); auto a = viewport_->smArr(m); std::map<int, std::pair<int, int>> c; for (size_t f = 0; f < a.size(); f++) { auto& x = c[B->faceGroup[f]]; x.second++; x.first += a[f]; } for (auto& [gg, x] : c) r += QString("%1:%2/%3 ").arg(gg).arg(x.first).arg(x.second); } else if (op == "mask") r = viewport_->smCreate(m); else if (op == "mirror") r = viewport_->smMirror(m, g);
      viewport_->beforeFrame(); log << QString("tg %1 %2 %3: %4").arg(op).arg(m).arg(g).arg(r); continue; }
    if (w.size() == 1 && w[0] == "topodump") { viewport_->beforeFrame(); const auto& S = viewport_->topoSel();
      log << QString("topo: room %1 mode %2 active %3 sel %4: f %5 e %6 v %7 · tool %8 · snap %9 sym %10").arg(scene_.room, viewport_->topoMode).arg(viewport_->topoMesh()).arg(S.id).arg(S.f.size()).arg(S.e.size()).arg(S.v.size()).arg(viewport_->tool()).arg(viewport_->tset.snap).arg(viewport_->tset.sym);
      for (auto& [id, it] : scene_.items) if (it.type == ItemType::IMesh) { const PolyMesh* B = it.base ? it.base.get() : it.poly.get(); if (!B) continue;
        QStringList gs; for (auto& g : B->groupNames) gs << g;
        log << QString("  imesh %1 \"%2\": v %3 f %4 groups %5 [%6] src %7 smask %8 hid %9").arg(id).arg(it.name).arg(B->pos.size()).arg(B->faceCount()).arg(gs.size()).arg(gs.mid(0, 8).join(",")).arg(it.raw.value("src").toObject().value("ext").toString())
                 .arg(it.raw.value("smask").toString().left(40)).arg(QString(QJsonDocument(it.raw.value("hidGroups").toArray()).toJson(QJsonDocument::Compact)))
                 + QString(" vis %1 poly %2 parent %3").arg(scene_.isVisible(it)).arg(it.poly ? int(it.poly->pos.size()) : -1).arg(it.parent); }
      if (const Item* am = scene_.item(S.id)) { const PolyMesh* B = am->base ? am->base.get() : am->poly.get(); Vec3 sum; for (auto& q : B->pos) sum += q; log << QString("  sel mesh %1 pos sum %2 %3 %4 src %5").arg(S.id).arg(sum.x, 0, 'f', 3).arg(sum.y, 0, 'f', 3).arg(sum.z, 0, 'f', 3).arg(am->raw.value("src").toObject().value("ext").toString()); }
      auto D = viewport_->topoDraw(); log << QString("  draw: %1 line sets, %2 tri sets, %3 point sets").arg(D.lines.size()).arg(D.tris.size()).arg(D.pts.size()); continue; }
    if (w.size() == 1 && w[0] == "newproj") { newProject(false); log << QString("newproj: items %1").arg(scene_.items.size()); continue; }
    if (w.size() >= 2 && w[0] == "preset") { log << QString("preset %1").arg(createPreset(w.mid(1).join(' ')) ? "ok" : "FAILED"); continue; }
    if (w.size() == 3 && w[0] == "winsize") { showNormal(); resize(w[1].toInt(), w[2].toInt()); QApplication::processEvents(); log << QString("winsize %1x%2").arg(width()).arg(height()); continue; }   // (the main window's size, for a shot)
    if (w.size() >= 3 && w[0] == "filewin") {          // filewin <tab> <png>
      openFiles(w[1]); QApplication::processEvents(); QApplication::processEvents();
      for (QWidget* tw : QApplication::topLevelWidgets()) if (tw->isVisible() && tw->windowTitle() == tr("File")) { tw->grab().save(w.mid(2).join(' ')); log << "filewin " + w[1]; }
      files::closeFileWindow(); continue; }
    if (w.size() >= 2 && w[0] == "texdir") { looks::connectTexDir(w.mid(1).join(' ')); log << QString("texdir %1: %2 images").arg(looks::texDir().name).arg(looks::texDir().files.size()); continue; }
    if (w.size() == 2 && w[0] == "hsympanel") {   // hsympanel <png>: the Hair room's Symmetry settings opened from its button — where it lands, a shot
      QPushButton* btn = nullptr; for (auto* b : tools_->findChildren<QPushButton*>()) if (b->toolTip().startsWith("Symmetry settings")) btn = b;
      if (btn) { openSymPanel(viewport_, btn, false); QApplication::processEvents(); auto* pn = findChild<QWidget*>("sympanel");
        const QRect A = btn->screen()->availableGeometry(); log << (pn ? QString("hsympanel at %1,%2 %3x%4 screen %5,%6 %7x%8").arg(pn->x()).arg(pn->y()).arg(pn->width()).arg(pn->height()).arg(A.x()).arg(A.y()).arg(A.width()).arg(A.height()) : QString("hsympanel none"));
        if (pn) { pn->grab().save(w[1]); pn->close(); } } else log << "hsympanel: no button";
      continue; }
    if (w.size() == 2 && w[0] == "ghodist") { viewport_->gho.dist = w[1].toDouble(); viewport_->ghostChanged(); log << "ghodist " + w[1]; continue; }   // ghodist <reach>: the Topology Ghost's reach
    if (w.size() >= 2 && w[0] == "wait") { QElapsedTimer t; t.start(); while (t.elapsed() < w[1].toInt()) QApplication::processEvents(QEventLoop::AllEvents, 50); continue; }
    if (w.size() >= 3 && w[0] == "texwin") {          // texwin <item> <png> — the texture window opened and grabbed
      { auto id = w[1].split(QChar(0x3a)); openTexWindow(this, viewport_, &scene_, id[0].toInt(), id.size() > 1 ? id[1].toInt() : -2); } QApplication::processEvents();   // (<item>:<mat> — on one UV Mat)
      for (QWidget* tw : QApplication::topLevelWidgets()) if (tw->isVisible() && tw->windowTitle().startsWith("▦")) { QApplication::processEvents(); tw->grab().save(w.mid(2).join(' ')); log << "texwin " + tw->windowTitle(); }
      continue; }
    if (w.size() >= 4 && (w[0] == "rawset" || w[0] == "rawsetf")) {   // rawset <item> <key> <json value> | rawsetf <item> <key> <json file>
      int id = w[1].toInt(); QString key = w[2]; QByteArray js = w.mid(3).join(' ').toUtf8();
      if (w[0] == "rawsetf") { QFile f(QString::fromUtf8(js)); js = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("null"); }
      QJsonValue v = QJsonDocument::fromJson("[" + js + "]").array().at(0);
      viewport_->runEdit([id, key, v](Scene& s, Selection&, const CurveCache&) { if (Item* it = s.item(id)) it->raw.insert(key, v); return QString(); }, false);
      viewport_->beforeFrame(); log << "rawset " + w[1] + " " + key; continue; }
    if (w.size() == 1 && w[0] == "imeshdump") {
      auto imeshList = [](const Scene& s) { std::vector<const Item*> v; for (auto& [id, it] : s.items) if (it.type == ItemType::IMesh) v.push_back(&it); std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->order < b->order; }); return v; };
      for (const Item* m : imeshList(scene_)) { const PolyMesh* B = m->base ? m->base.get() : m->poly.get(); if (!B) { log << QString("imesh %1 %2: no mesh").arg(m->id).arg(m->name); continue; }
        { double cw = 0; for (auto& c : B->creases) cw += c.w;
          log << QString("  normals %1 hard %2 creases %3 (sum %4) n0 %5,%6,%7").arg(B->cornerNormal.size()).arg(B->hardEdges.size()).arg(B->creases.size()).arg(cw, 0, 'f', 3)
                   .arg(B->cornerNormal.empty() ? 0.f : B->cornerNormal[0][0], 0, 'f', 3).arg(B->cornerNormal.empty() ? 0.f : B->cornerNormal[0][1], 0, 'f', 3).arg(B->cornerNormal.empty() ? 0.f : B->cornerNormal[0][2], 0, 'f', 3); }
        const Item* p = scene_.item(m->parent);
        log << QString("imesh %1 \"%2\" in \"%3\"%4: v %5 f %6 corners %7 groups [%8] mats [%9] uv %10 look %11").arg(m->id).arg(m->name).arg(p ? p->name : "-").arg(m->headTag ? " head" : "")
                 .arg(B->pos.size()).arg(B->faceCount()).arg(B->faceVerts.size()).arg(QStringList(B->groupNames.begin(), B->groupNames.end()).join(",")).arg(QStringList(B->matNames.begin(), B->matNames.end()).join(","))
                 .arg(B->cornerUV.size()).arg(QString::fromUtf8(QJsonDocument(m->raw.value("look").toObject()).toJson(QJsonDocument::Compact)).left(300));
        log << QString("  p0 %1 %2 %3 | %4 %5 %6").arg(B->pos[0].x, 0, 'g', 9).arg(B->pos[0].y, 0, 'g', 9).arg(B->pos[0].z, 0, 'g', 9).arg(B->pos[1].x, 0, 'g', 9).arg(B->pos[1].y, 0, 'g', 9).arg(B->pos[1].z, 0, 'g', 9); }
      Box3 b; for (const Item* m : imeshList(scene_)) if (m->poly) b.unite(m->poly->bounds());
      log << QString("step %1 camScale %2").arg(viewport_->convValue("step").toDouble()).arg(viewport_->camera().sceneScale); continue; }
    if (w.size() == 1 && w[0] == "headshow") { for (auto& [id, it] : scene_.items) if (it.headTag) { it.visible = true; for (Item* p = scene_.item(it.parent); p; p = scene_.item(p->parent)) p->visible = true; }
      viewport_->sceneChanged(false); log << "headshow"; continue; }
    if (w.size() == 2 && w[0] == "seg2c") { std::vector<int> ids; for (const QString& x : w[1].split(",")) ids.push_back(x.toInt()); const auto* W = &viewport_->hair().walls;
      viewport_->runEdit([ids, W](Scene& s, Selection& sl, const CurveCache& c) { return walltools::segmentsToCurves(s, c, *W, sl, ids); }, true); viewport_->beforeFrame(); log << "seg2c " + w[1]; continue; }
    if (w.size() == 1 && w[0] == "kinks") { viewport_->beforeFrame(); QStringList L;
      for (auto& [id, it] : scene_.items) if (it.type == ItemType::Surface) for (auto& k : walltools::wallKinks(scene_, viewport_->hair().curves, id)) L << QString("%1:%2:%3").arg(id).arg(k.a).arg(k.turn, 0, 'f', 1);
      log << "kinks " + L.join(" "); continue; }
    if (w.size() == 2 && w[0] == "zone") { viewport_->showZone(w[1].toInt()); log << QString("zone %1: %2 lines").arg(w[1]).arg(viewport_->zoneLines().size()); continue; }
    if (w.size() == 1 && w[0] == "refcube") { viewport_->toggleReference(); log << QString("refcube: %1").arg(viewport_->refCube ? QString::number(viewport_->refCube->height) : QString("off")); continue; }
    if (w.size() == 2 && w[0] == "ribdump") { viewport_->beforeFrame(); const Item* c = scene_.item(w[1].toInt());
      auto sh = c ? rib::ribShape(scene_, viewport_->hair().curves, *c, viewport_->camera().dir(), viewport_->camera().sceneScale) : std::nullopt;
      if (!sh) { log << "ribdump: none"; continue; }
      auto f = [](const Vec3& v) { return QString("%1,%2,%3").arg(v.x, 0, 'f', 4).arg(v.y, 0, 'f', 4).arg(v.z, 0, 'f', 4); };
      QString s = QString("rib %1: n %2 f0 %3 %4 f30 %5 %6 %7 f60 %8 %9").arg(w[1]).arg(sh->F.size()).arg(f(sh->F[0].A)).arg(sh->F[0].w, 0, 'f', 4)
                    .arg(f(sh->F[30].p), f(sh->F[30].A)).arg(sh->F[30].w, 0, 'f', 4).arg(f(sh->F[60].A)).arg(sh->F[60].w, 0, 'f', 4);
      if (sh->extruded) s += QString(" xr %1 %2 %3 %4").arg(sh->rings.size()).arg(f(sh->rings[20][2]), f(sh->rings[20][3]), f(sh->rings[60][2]));
      log << s; continue; }
    if (w.size() == 2 && w[0] == "ribconv") { viewport_->ribConvert(w[1].toInt()); viewport_->beforeFrame(); log << "ribconv " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "m2c") { viewport_->meshToCurves(w[1].toInt()); viewport_->beforeFrame();
      int nc = 0, nw = 0; std::vector<int> sl = viewport_->selection().items; const Item* g = sl.empty() ? nullptr : scene_.item(sl.back());
      if (g) for (auto& [id, it] : scene_.items) if (it.parent == g->id) (it.type == ItemType::Curve ? nc : nw)++;
      log << QString("m2c %1: group %2 \"%3\": %4 curves, %5 walls").arg(w[1]).arg(g ? g->id : -1).arg(g ? g->name : QString()).arg(nc).arg(nw); continue; }
    if (w.size() == 3 && w[0] == "attach") { int a = w[1].toInt(), c = w[2].toInt();
      viewport_->runEdit([a, c](Scene& s, Selection&, const CurveCache& cc) { return crown::connectToConnector(s, cc, a, c); }, true); viewport_->beforeFrame(); log << "attach " + w[1]; continue; }
    if (w.size() >= 1 && w[0] == "spiraladd") {   // spiraladd [loops]: a Spiral on the selected cocoon, painted over its whole length
      const auto made = viewport_->addShellMods("spiral", QJsonObject{{"loops", w.value(1, "8").toDouble()}, {"phase", 0}}, "Spiral"); QJsonArray a; for (int i = 0; i < CMASK_N; i++) a.append(1.0);
      viewport_->runEdit([made, a](Scene& s, Selection&, const CurveCache&) { for (int id : made) s.item(id)->raw.insert("cmask", a); return QString(); }, true); viewport_->touchShellMods(made); viewport_->beforeFrame(); log << QString("spiraladd %1").arg(made.size()); continue; }
    if (w.size() == 1 && w[0] == "cocoon") { CocoonSystem* cs = viewport_->hair().cocoonSystem();
      QString msg; viewport_->runEdit([cs, &msg](Scene& sc, Selection& sel, const CurveCache& cc) { int k = edit::makeLiveCocoon(sc, cc, cs, sel, &msg); return k < 0 ? "!" + msg : msg; }); viewport_->beforeFrame();
      log << "cocoon: " + QString::number(viewport_->selection().items.empty() ? -1 : viewport_->selection().items.back()) + " " + msg; continue; }
    if (w.size() == 1 && w[0] == "meshdump") { for (auto& [id, it] : scene_.items) if (it.type == ItemType::Mesh && it.poly) log << QString("mesh %1 \"%2\": verts %3 faces %4 crowns %5").arg(id).arg(it.name).arg(it.poly->pos.size()).arg(it.poly->faceCount()).arg(QString(QJsonDocument(it.raw.value("srcCrowns").toArray()).toJson(QJsonDocument::Compact))); continue; }
    if (w.size() == 3 && w[0] == "crownsharp") {   // crownsharp <i> <0|1>: the last crown's lower connector — a sharp corner at its i-th anchor
      auto ks = crown::crowns(scene_); const Item* k = ks.empty() ? nullptr : scene_.item(ks.back()); const Item* lo = k ? scene_.item(k->raw.value("low").toInt(-1)) : nullptr;
      const int i = w[1].toInt(); if (lo && i >= 0 && i < int(lo->anchors.size())) { const int a = lo->anchors[size_t(i)]; viewport_->runEdit([a, on = w[2] == "1"](Scene& s, Selection&, const CurveCache&) { crown::setSharpCorner(s, a, on); return QString(); }, true); viewport_->beforeFrame(); log << QString("crownsharp %1 anchor %2").arg(i).arg(a); }
      continue; }
    if (w.size() >= 2 && w[0] == "clicktip") { int nth = 0; QStringList ww = w; if (ww.size() > 2 && ww[1].startsWith("#")) { nth = ww[1].mid(1).toInt(); ww.removeAt(1); } const QString pre = ww.mid(1).join(' '); QPushButton* hitB = nullptr;   // clicktip <tooltip start>: the first shown button with that tip clicked
      for (QPushButton* b : findChildren<QPushButton*>()) if (b->isVisible() && b->toolTip().startsWith(pre) && nth-- <= 0) { hitB = b; break; }
      if (hitB) { hitB->click(); QApplication::processEvents(); } log << QString("clicktip %1").arg(hitB ? "clicked" : "none"); continue; }
    if (w.size() == 1 && w[0] == "lasttoast") { log << "lasttoast " + ui::lastToast(); continue; }
    if (w.size() == 1 && w[0] == "popups") { QStringList ps; for (QWidget* t : QApplication::topLevelWidgets()) if (t->isVisible() && t != this) ps << QString("%1 %2 %3x%4 at %5,%6").arg(t->metaObject()->className(), t->objectName()).arg(t->width()).arg(t->height()).arg(t->x()).arg(t->y()); for (QWidget* t : QApplication::allWidgets()) if (t->objectName() == "sympanel") ps << QString("sympanel visible %1 %2x%3").arg(t->isVisible()).arg(t->width()).arg(t->height()); log << "popups: " + ps.join("; "); continue; }
    if (w.size() == 1 && w[0] == "symerr") { const double cx = viewport_->camera().sceneCenter.x; double worst = 0; int wa = -1;   // symerr: each anchor's distance to the nearest anchor at its X-mirror — the worst
      for (const auto& [id, a] : scene_.anchors) { Vec3 m = a.pos; m.x = 2 * cx - m.x; double bd = 1e300; for (const auto& [id2, b] : scene_.anchors) bd = std::min(bd, b.pos.distanceTo(m)); if (bd > worst) { worst = bd; wa = id; } }
      log << QString("symerr %1 at anchor %2 (of %3)").arg(worst, 0, 'f', 3).arg(wa).arg(scene_.anchors.size()); continue; }
    if (w.size() == 1 && w[0] == "toast") { log << "toast: " + ui::lastToast(); continue; }
    if (w.size() == 1 && w[0] == "asmooth") { QStringList o; for (int a : viewport_->selection().anchors) if (const Anchor* A = scene_.anchor(a)) o << QString("%1:%2").arg(a).arg(A->smooth, 0, 'f', 3); log << "asmooth " + o.join(' ') + QString(" keyB %1").arg(viewport_->keyB); continue; }
    if (w.size() == 1 && w[0] == "clock") { log << QString("clock %1 ms").arg(clock_.elapsed()); continue; }
    if (w.size() >= 1 && w[0] == "crowncreate") { viewport_->crownSym = !(w.size() > 1 && w[1] == "nosym"); viewport_->crownBangs = w.size() > 2 && w[2] == "bangs"; const int n0 = scene_.nextId; viewport_->createCrownAt(std::nullopt); viewport_->beforeFrame();
      // (the collision's middle across, and the new crown's anchors' middle across: a symmetric crown — the same)
      Box3 cb; for (auto& [id, it] : scene_.items) if (it.headTag && it.poly) for (const Vec3& p : it.poly->pos) cb.expand(p);
      double sx = 0; int na = 0; for (auto& [id, it] : scene_.items) if (id >= n0 && it.type == ItemType::Curve) for (int a : it.anchors) if (const Anchor* A = scene_.anchor(a)) { sx += A->pos.x; na++; }
      log << QString("crowncreate: collision middle x %1, crown middle x %2 (%3 anchors), scene middle x %4").arg(cb.empty() ? 0 : cb.center().x, 0, 'f', 2).arg(na ? sx / na : 0, 0, 'f', 2).arg(na).arg(viewport_->camera().sceneCenter.x, 0, 'f', 2); continue; }
    if (w.size() == 2 && w[0] == "refheight") { viewport_->setRefHeight(w[1].toDouble()); viewport_->beforeFrame(); int rings = 0; for (auto& [id, r] : viewport_->hair().cocoons()) rings += int(r.rings.size());
      log << QString("refheight %1: project step %2, rings %3").arg(w[1]).arg(projectStep(scene_)).arg(rings); continue; }
    if (w.size() == 2 && w[0] == "sellast") { std::vector<int> cs; for (const Item* c : scene_.curves()) if (c->conn.isEmpty()) cs.push_back(c->id); std::sort(cs.begin(), cs.end());   // sellast n: the n curves made last selected
      std::vector<int> ids(cs.end() - std::min<long long>(w[1].toInt(), (long long)cs.size()), cs.end()); viewport_->setSelectedItems(ids); QStringList q; for (int x : ids) { const Item* c = scene_.item(x); q << QString("%1[%2..%3]").arg(x).arg(c->anchors.front()).arg(c->anchors.back()); } log << "sellast " + q.join(" "); continue; }
    if (w.size() == 1 && w[0] == "mkwall") {   // mkwall: walls along the selected curves (as Shift+RMB → wall)
      viewport_->runEdit([](Scene& s2, Selection& sl, const CurveCache& c2) { QString m; std::vector<int> cs; for (int x : sl.items) if (s2.item(x) && s2.item(x)->type == ItemType::Curve) cs.push_back(x);
        auto made = edit::createSurfaces(s2, c2, cs, false, false, &m); return QString("%1 walls").arg(made.size()); }, true); viewport_->beforeFrame(); log << "mkwall"; continue; }
    if (w.size() >= 1 && w[0] == "crowncont") { auto ks = crown::crowns(scene_); if (!ks.empty()) { if (w.size() > 1) viewport_->runEdit([k = ks.back(), x = w[1].toDouble()](Scene& s2, Selection&, const CurveCache&) { Item* it = s2.item(k); QJsonObject p = it->raw.value("p").toObject(); p.insert("contLen", x); it->raw.insert("p", p); return QString(); }, false);
        viewport_->crownContinue(ks.back()); } viewport_->beforeFrame(); log << "crowncont"; continue; }   // crowncont [len]: the last crown's continuation
    if (w.size() >= 2 && w[0] == "prim") {   // prim <type> | prim set <key> <v> | prim convert — on the selected primitive; logs its mesh
      const int sel = viewport_->selection().items.empty() ? -1 : viewport_->selection().items.back();
      if (w[1] == "set" && w.size() == 4) viewport_->primSet(sel, w[2], w[3].toDouble(), true); else if (w[1] == "convert") viewport_->primConvert(sel); else viewport_->createPrimitive(w[1]);
      viewport_->beforeFrame(); const int id = viewport_->selection().items.empty() ? -1 : viewport_->selection().items.back(); const Item* it = scene_.item(id);
      const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr; int tri = 0, quad = 0, ng = 0; if (B) for (int f = 0; f < B->faceCount(); f++) { const int n = B->faceStart[size_t(f) + 1] - B->faceStart[size_t(f)]; (n == 3 ? tri : n == 4 ? quad : ng)++; }
      // (closed: every edge shared by two polygons — the plane has a rim)
      int open = 0; if (B) { std::map<std::pair<int, int>, int> ec; for (int f = 0; f < B->faceCount(); f++) for (int k = B->faceStart[size_t(f)]; k < B->faceStart[size_t(f) + 1]; k++) { int a = B->faceVerts[size_t(k)], b = B->faceVerts[size_t(k + 1 < B->faceStart[size_t(f) + 1] ? k + 1 : B->faceStart[size_t(f)])]; ec[{std::min(a, b), std::max(a, b)}]++; } for (auto& [e, c] : ec) if (c != 2) open++; }
      log << QString("prim %1: %2 \"%3\" verts %4 faces %5 (tri %6 quad %7 ngon %8) open edges %9 prim %10").arg(w.mid(1).join(" ")).arg(id).arg(it ? it->name : QString()).arg(B ? int(B->pos.size()) : 0).arg(B ? B->faceCount() : 0).arg(tri).arg(quad).arg(ng).arg(open).arg(it && it->raw.contains("prim")); continue; }
    if (w.size() == 1 && w[0] == "crowndump") {
      viewport_->beforeFrame();
      for (auto& [id, k] : viewport_->crownMeshes()) {
        Vec3 c; for (auto& p : k.pos) c += p; c = c / double(std::max<size_t>(1, k.pos.size()));
        int tri = 0, quad = 0; for (auto& f : k.faces) (f.size() == 3 ? tri : quad)++;
        log << QString("crown %1: loops %2 rings %3 part %4 verts %5 faces %6 (tris %7) centroid %8,%9,%10").arg(id).arg(k.loops).arg(k.rings).arg(k.part).arg(k.pos.size()).arg(k.faces.size()).arg(tri)
                 .arg(c.x, 0, 'f', 3).arg(c.y, 0, 'f', 3).arg(c.z, 0, 'f', 3);
        if (const Item* ki = scene_.item(id)) if (const Item* lo = scene_.item(ki->raw.value("low").toInt(-1))) { QStringList q; for (int a : lo->anchors) q << QString::number(a); log << QString("crown %1 low %2: %3").arg(id).arg(lo->id).arg(q.join(",")); }   // (its lower connector's anchors)
      }
      continue; }
    if (w.size() >= 2 && w[0] == "symon") { int g = w[1].toInt(); QString a = w.size() > 2 ? w[2] : "x";
      viewport_->runEdit([g, a](Scene& s, Selection&, const CurveCache&) { s.item(g)->raw.insert("sym", QJsonObject{{"on", true}, {"axis", a}}); return QString(); }, true); viewport_->beforeFrame(); log << "symon " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "symapply") { int g = w[1].toInt(); Vec3 c = viewport_->camera().sceneCenter;
      viewport_->runEdit([g, c](Scene& s, Selection& sl, const CurveCache&) { return sym::applySymmetry(s, g, c, sl); }, true); viewport_->beforeFrame();
      log << QString("symapply %1: %2 items, center %3").arg(w[1]).arg(scene_.items.size()).arg(c.x); continue; }
    if (w.size() == 1 && w[0] == "srplan") { log << smartRenamePlanDump(viewport_, scene_); continue; }
    if (w.size() == 2 && w[0] == "srsort") { log << "srsort " + w[1] + ": " + smartRenameSortDump(viewport_, scene_, w[1].toInt()); continue; }
    if (w.size() == 2 && w[0] == "scheme") { log << "scheme " + w[1] + "\n" + schemeDump(scene_, viewport_->hair().curves, w[1].toInt()); continue; }
    if (w.size() == 2 && w[0] == "expand") { for (const QString& s : w[1].split(',')) if (Item* g = scene_.item(s.toInt())) g->expanded = true; rebuildOutliner(); log << "expand " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "junctionpie") {          // junctionpie <png>: an anchor shared by welded curves selected, the Shift+RMB pie, the way to «Rotate junction»'s items walked
      int a = -1; for (auto& [id, an] : scene_.anchors) if (!aops::junctionChoices(scene_, id).empty()) { a = id; break; }
      if (a < 0) { log << "junctionpie: no junction anchor"; continue; }
      viewport_->setSelectedAnchors({a}); openMainPie(viewport_->mapToGlobal(QPoint(viewport_->width() / 2, viewport_->height() / 2))); QApplication::processEvents();
      QString r = "no pie"; if (auto* pp = ui::Pie::current()) { r = pp->subWalk(); pp->grab().save(w[1]); } ui::Pie::closeCurrent();
      log << QString("junctionpie anchor %1: %2").arg(a).arg(r); continue; }
    if (w.size() == 2 && w[0] == "mainpie") { openMainPie(viewport_->mapToGlobal(QPoint(viewport_->width() / 2, viewport_->height() / 2))); QApplication::processEvents(); if (auto* pp = ui::Pie::current()) pp->grab().save(w[1]); ui::Pie::closeCurrent(); log << "mainpie " + w[1]; continue; }   // (the Shift+RMB pie, grabbed)
    if (w.size() == 2 && w[0] == "olpie") { openOutlinerPie(outliner_->mapToGlobal(outliner_->rect().center())); QApplication::processEvents(); if (auto* pp = ui::Pie::current()) pp->grab().save(w[1]); ui::Pie::closeCurrent(); log << "olpie " + w[1]; continue; }   // (the outliner's pie, grabbed)
    if (w.size() >= 2 && w[0] == "restoresets") {   // restoresets <ref.hairmesh> — meshes with no UV sets (made by a Merge that dropped them): each polygon given the sets of its twin in ref (same place)
      Scene ref; const LoadResult lr = loadProject(w.mid(1).join(' '), ref); if (!lr.ok) { log << "restoresets: cannot load " + lr.error; continue; }
      struct RF { std::vector<QString> in; QString live; std::map<QString, std::vector<std::pair<Vec3, std::array<float, 2>>>> uvOf; };   // (its sets, the one shown, each set's corner UVs by place)
      auto keyOf = [](const PolyMesh& B, int f) { Vec3 c; const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; for (int k = s; k < e; k++) c = c + B.pos[size_t(B.faceVerts[size_t(k)])]; c = c / double(e - s);
        return QString("%1|%2|%3|%4").arg(qint64(std::llround(c.x * 1e4))).arg(qint64(std::llround(c.y * 1e4))).arg(qint64(std::llround(c.z * 1e4))).arg(e - s); };
      std::unordered_map<QString, RF> byKey;
      for (auto& [id, it] : ref.items) { if (!it.poly) continue; const PolyMesh& B = *(it.base ? it.base : it.poly); if (!topo::hasUvSets(B)) continue;
        for (int f = 0; f < B.faceCount(); f++) { RF r; for (size_t k = 0; k < B.uvSetNames.size(); k++) if (B.faceSets[size_t(f)] >> k & 1u) r.in.push_back(B.uvSetNames[k]);
          const int l = B.faceLive[size_t(f)]; r.live = l >= 0 && size_t(l) < B.uvSetNames.size() ? B.uvSetNames[size_t(l)] : (r.in.empty() ? QString() : r.in[0]);
          for (size_t k = 0; k < B.uvSetNames.size(); k++) { auto& v = r.uvOf[B.uvSetNames[k]]; for (int q = B.faceStart[size_t(f)]; q < B.faceStart[size_t(f) + 1]; q++) v.push_back({B.pos[size_t(B.faceVerts[size_t(q)])], B.setUV[k][size_t(q)]}); }
          byKey[keyOf(B, f)] = std::move(r); } }
      QStringList rep;
      for (auto& [id, it] : scene_.items) { if (!it.poly || it.type != ItemType::IMesh) continue; const PolyMesh& B0 = *(it.base ? it.base : it.poly); if (topo::hasUvSets(B0) || B0.cornerUV.size() != B0.faceVerts.size()) continue;
        std::vector<const RF*> got(size_t(B0.faceCount()), nullptr); int hit = 0; std::map<QString, int> votes;
        for (int f = 0; f < B0.faceCount(); f++) if (auto g = byKey.find(keyOf(B0, f)); g != byKey.end() && !g->second.in.empty()) { got[size_t(f)] = &g->second; hit++; votes[g->second.live]++; }
        if (!hit) continue;
        QString most; int mv = 0; for (auto& [n, v] : votes) if (v > mv) { mv = v; most = n; }
        std::vector<QString> names; for (const RF* r : got) if (r) for (const QString& n : r->in) if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        if (names.size() > 15) continue;
        auto nb = std::make_shared<PolyMesh>(B0); PolyMesh& d = *nb; d.uvSetNames = names; d.faceSets.assign(size_t(d.faceCount()), 0u); d.faceLive.assign(size_t(d.faceCount()), -1); d.setUV.assign(names.size(), d.cornerUV);
        auto idx = [&](const QString& n) { return int(std::find(names.begin(), names.end(), n) - names.begin()); };
        for (int f = 0; f < d.faceCount(); f++) { const RF* r = got[size_t(f)];
          if (!r) { const int s = idx(most); d.faceSets[size_t(f)] = 1u << s; d.faceLive[size_t(f)] = int8_t(s); continue; }   // (no twin found: the set most of the mesh is in)
          for (const QString& n : r->in) d.faceSets[size_t(f)] |= 1u << idx(n); d.faceLive[size_t(f)] = int8_t(idx(r->live));
          for (const QString& n : r->in) { if (n == r->live) continue; const auto& v = r->uvOf.at(n); const int s = idx(n);   // (its other sets' own layouts: corner by corner, by place)
            for (int q = d.faceStart[size_t(f)]; q < d.faceStart[size_t(f) + 1]; q++) { const Vec3& p = d.pos[size_t(d.faceVerts[size_t(q)])]; double bd = 1e300; std::array<float, 2> bu = d.cornerUV[size_t(q)];
              for (const auto& [pp, uu] : v) { const double dd = (pp - p).lengthSq(); if (dd < bd) { bd = dd; bu = uu; } } d.setUV[size_t(s)][size_t(q)] = bu; } } }
        QJsonArray us; for (const QString& n : names) us.append(QJsonObject{{"name", n}, {"mats", QJsonArray()}}); it.raw.insert("uvSets", us);
        it.base = nb; it.poly = nb; it.raw.insert("src", QJsonObject{{"ext", "pmesh"}, {"live", true}});   // (written anew on save, as any edit of the room)
        QStringList ns; for (const QString& n : names) { int c = 0; for (uint32_t b : d.faceSets) c += int(b >> idx(n) & 1u); ns << QString("%1 %2").arg(n).arg(c); }
        rep << QString("%1 (%2 of %3 polygons found): %4").arg(it.name).arg(hit).arg(d.faceCount()).arg(ns.join(", ")); }
      viewport_->beforeFrame(); log << "restoresets: " + (rep.isEmpty() ? QString("nothing") : rep.join(" | ")); continue; }
    if (w.size() >= 2 && w[0] == "merge") {   // merge origin | centre | <mesh id> — the picked meshes merged, the pivot there
      const auto ms = viewport_->mergeableMeshes(); Vec3 p; Box3 b; for (int id : ms) if (const Item* it = scene_.item(id); it && it->poly) b.unite(it->poly->bounds());
      if (w[1] == "centre") p = b.center(); else if (w[1] != "origin") { const int id = w[1].toInt(); if (const Item* it = scene_.item(id)) p = meshmods::meshMatrix(scene_, *it).apply(viewport_->meshPivotLocal(id)); }
      log << "merge: " + viewport_->mergeMeshes(ms, p) + QString(" · pivot %1 %2 %3").arg(p.x).arg(p.y).arg(p.z); if (!viewport_->selection().items.empty()) if (const Item* r = scene_.item(viewport_->selection().items[0])) log << "merge: in " + (scene_.item(r->parent) ? scene_.item(r->parent)->name : QString("root")); continue; }
    if (w.size() == 4 && w[0] == "olmove") { std::vector<int> ids; for (const QString& s : w[1].split(QChar(0x2c))) ids.push_back(s.toInt()); outliner_->moveItems(ids, w[2].toInt(), w[3]); const Item* it = scene_.item(ids[0]); log << QString("olmove: %1 in %2").arg(it ? it->name : "-").arg(it && scene_.item(it->parent) ? scene_.item(it->parent)->name : QString("root")); continue; }   // olmove <ids> <target> into|before|after|root
    if (w.size() == 5 && w[0] == "penfill") { log << "penfill: " + viewport_->testPenFill(Vec3(w[1].toDouble(), w[2].toDouble(), 0), Vec3(w[3].toDouble(), w[4].toDouble(), 0)); continue; }   // penfill <anchor x y on screen (−1 −1: none)> <pointer x y>
    if (w.size() == 3 && w[0] == "lastv") { log << "lastv: " + viewport_->testNearVerts(w[1].toInt(), -w[2].toDouble()); continue; }   // lastv <mesh> <k>: its last k vertices on screen
    if (w.size() == 4 && w[0] == "movesel") { log << "movesel: " + viewport_->testMoveSel(Vec3(w[1].toDouble(), w[2].toDouble(), w[3].toDouble())); continue; }   // movesel <dx dy dz>: the whole mesh dragged with Snap — vertices stuck together
    if (w.size() == 2 && w[0] == "esnap") { viewport_->tset.esnap = w[1] != "off"; if (w[1] != "off") viewport_->tset.esnapMode = w[1]; log << "esnap " + w[1]; continue; }   // esnap vert|edge|face|grid|off
#ifdef HM_DEVTOOLS
#ifdef HM_DEVTOOLS
    if (w.size() == 1 && w[0] == "devcrash") { volatile int* p = nullptr; *p = 1; }   // devcrash: a crash on purpose (the dump writer checked)
#endif
    if (w.size() == 2 && w[0] == "devrec") { if (w[1] == "on") dev_->start(); else dev_->stop(); log << "devrec " + w[1] + " " + dev_->folder(); continue; }   // devrec on|off: the development session's record
#endif
    // (the Paint room's views: vset save <name> · vset show <name> · vset pane <i> <name|-> [sync] · vwin open|close|migrate · vwin shot <png> · vwin menu <png> · vwin info)
    // (the Paint room's lists) pv key <hide|hideObj|only|onlyObj|showAll> · pv nopaint|eye|lock <item id> 0|1 · pv geye|glock <mesh> <group> 0|1 · pv set <key> · pv seteye|setlock <key> 0|1
    // · pv info · pv mask <png> · pv shot <png> (the left column) · pv full 0|1 · pv roll <deg> · pv tool <id>
    // (the manager window) mgr mode 0|1 · mgr kind <face|loop|ring|fill|part|rect|lasso> · mgr mouse <phase 0-3> <x> <y> [shift|ctrl|alt] · mgr key <op> [x y] (at that point of the
    // preview — its middle by default) · mgr undo|redo · mgr frame · mgr follow 0|1 · mgr ghosts 0|1 · mgr shot <png> (the preview's picture) · mgr left <png> (the whole column) · mgr info
    if (w.size() >= 2 && w[0] == "mgr" && paintLeft_) { QString r; const QString o = w[1]; PaintPreview* pp = paintLeft_->preview();
      if (o == "mode" && w.size() > 2) { paintLeft_->setMgrMode(w[2].toInt()); QApplication::processEvents(); }
      else if (o == "kind" && w.size() > 2) paintLeft_->setMgrKind(w[2]);
      else if (o == "mouse" && w.size() > 4) { Qt::KeyboardModifiers m; if (w.contains("shift")) m |= Qt::ShiftModifier; if (w.contains("ctrl")) m |= Qt::ControlModifier; if (w.contains("alt")) m |= Qt::AltModifier;
        pp->test(w[2].toInt(), QPointF(w[3].toDouble(), w[4].toDouble()), m); }
      else if (o == "key" && w.size() > 2) { const QPointF at = w.size() > 4 ? QPointF(w[3].toDouble(), w[4].toDouble()) : QPointF(pp->width() / 2.0, pp->height() / 2.0); pp->test(3, at, {}); r = pp->keyAt(w[2], at) ? "done" : "not taken"; }
      else if (o == "undo" || o == "redo") r = paintLeft_->visUndo(o == "redo") ? "done" : "nothing";
      else if (o == "frame") pp->frameShown();
      else if (o == "follow" && w.size() > 2) { pp->follow = w[2] == "1"; if (pp->follow) pp->takeMainView(); }
      else if (o == "ghosts" && w.size() > 2) { pp->ghosts = w[2] == "1"; pp->refresh(); }
      else if (o == "shot" && w.size() > 2) r = QString("saved %1").arg(pp->grabShot().save(w[2]));
      else if (o == "left" && w.size() > 2) { paintLeft_->refresh(); QApplication::processEvents(); r = QString("saved %1").arg(paintLeft_->grab().save(w[2])); }
      log << "mgr " + w.mid(1).join(' ') + (r.isEmpty() ? QString() : ": " + r) + " · " + pp->status() + QString(" · sel %1").arg(viewport_->pvSelCount()); continue; }
    if (w.size() >= 2 && w[0] == "pv" && paint_ && (w[1] == "addadj" || w[1] == "tones" || w[1] == "layerlist" || w[1] == "layersfrom" || w[1] == "addfx" || w[1] == "addshadow" || w[1] == "split" || w[1] == "fxset" || w[1] == "rectpaint")) { QString r; const QString o = w[1];   // (checks: Curves, layers from a file)
      if (o == "rectpaint" && w.size() > 6) { paint_->ensureDoc(); paint_->doc.ensurePixels(paint_->layer); if (paint::Layer* l = paint_->active(); l && !l->image.isNull()) {   // (checks: pv rectpaint u0 v0 u1 v1 #colour — a filled rectangle of the texture on the active layer)
        QPainter g(&l->image); g.fillRect(QRectF(QPointF(w[2].toDouble() * l->image.width(), w[3].toDouble() * l->image.height()), QPointF(w[4].toDouble() * l->image.width(), w[5].toDouble() * l->image.height())), QColor(w[6])); g.end(); emit paint_->pixelsChanged({}); r = "ok"; } }
      if (o == "addshadow" && w.size() > 2) { QStringList n = w.mid(2).join(' ').split(',', Qt::SkipEmptyParts);
        if (n == QStringList{"auto"}) { n.clear(); for (const auto& [i, it] : scene_.items) if (it.type == ItemType::IMesh && it.poly && it.name.contains("hair", Qt::CaseInsensitive) && !n.contains(it.name)) n << it.name; }
        paint_->ensureDoc(); const int id = paint_->addShadow(n); r = QString("shadow layer %1").arg(id); }   // (checks: pv addshadow Hair,Hair_2 — object names)
      if (o == "split" ) { const int id = paint_->splitShapeColour(paint_->layer); const paint::Layer* c = paint_->doc.layer(id); const auto& L = paint_->doc.layers(); const int i = paint_->doc.indexOf(id); const paint::Layer* b = i > 0 ? &L[size_t(i - 1)] : nullptr;
        r = c ? QString("colour %1 '%2' clip %3 · under it '%4' clip %5").arg(id).arg(c->name).arg(c->clip).arg(b ? b->name : QString("-")).arg(b && b->clip) : QString("no split"); }   // (checks: pv split — the active layer into shape + colour)
      if (o == "addfx" && w.size() > 2) { const int id = paint_->addEffect(paint_->layer, w[2]); r = QString("fx layer %1").arg(id); }   // (checks: pv addfx stroke|inner|bevel — on the active layer)
      if (o == "fxset" && w.size() > 3) { if (paint::Layer* l = paint_->active(); l && !l->fx.isEmpty()) { if (w[2] == "vd" && w[3] == "cam") { const Vec3 d = viewport_->camera().dir(); l->fx.insert("vd", QJsonArray{-d.x, -d.y, -d.z}); l->fx.insert("az", std::round(std::atan2(-d.x, -d.z) * 180 / M_PI) + 30); l->fx.insert("el", 40); paint_->requestFx(0); r = "ok"; } else { bool num = false; const double d = w[3].toDouble(&num); l->fx.insert(w[2], w[3] == "true" ? QJsonValue(true) : w[3] == "false" ? QJsonValue(false) : num ? QJsonValue(d) : QJsonValue(w[3])); paint_->requestFx(0); r = "ok"; } } }
      if (o == "addadj" && w.size() > 2) { paint_->layer = paint_->doc.addAdjust(paint::adjustFromKey(w[2]), {}, paint_->layer); emit paint_->layersChanged(); emit paint_->pixelsChanged({}); r = QString("layer %1").arg(paint_->layer); }   // (checks: pv addadj curves)
      else if (o == "tones" && w.size() > 3) { if (paint::Layer* l = paint_->active()) { auto& t = l->tones[size_t(std::clamp(w[2].toInt(), 0, 3))]; t.clear(); for (int i = 3; i < w.size(); i++) { const QStringList q = w[i].split(','); if (q.size() == 2) t.push_back(QPointF(q[0].toDouble(), q[1].toDouble())); } emit paint_->pixelsChanged({}); } }   // (checks: pv tones <0 rgb|1 r|2 g|3 b> x,y …)
      else if (o == "layerlist" && w.size() > 2) { QString e; for (const auto& x : paint_->layersInFile(w.mid(2).join(' '), &e)) r += QString("[%1 %2 · %3] ").arg(x.id).arg(x.name, x.kind); if (r.isEmpty()) r = "none: " + e; }
      else if (o == "layersfrom" && w.size() > 3) { std::vector<int> ids; for (const QString& x : w[2].split(',')) ids.push_back(x.toInt()); r = paint_->takeLayersFrom(w.mid(3).join(' '), ids); }   // (checks: pv layersfrom <id,id> <project>)
      log << "pv " + w.mid(1).join(' ') + ": " + r; continue; }
    if (w.size() >= 2 && w[0] == "pv") { QString r; const QString o = w[1]; const bool on = w.size() > 3 ? w.last() == "1" : true; const int id = w.size() > 2 ? w[2].toInt() : -1;
      if (o == "key" && w.size() > 2) r = viewport_->pvKey(w[2]);
      else if (o == "nopaint") viewport_->pvSetNoPaint({id}, on);
      else if (o == "eye") viewport_->pvSetShown({id}, on);
      else if (o == "lock") viewport_->pvSetLocked({id}, on);
      else if ((o == "geye" || o == "glock") && w.size() > 4) viewport_->pvGroupSet(id, {w[3].toInt()}, o == "glock", on);
      else if (o == "set" && w.size() > 2) viewport_->pvSetApply(w[2]);
      else if ((o == "seteye" || o == "setlock") && w.size() > 3) viewport_->pvSetToggle(w[2], o == "setlock", on);
      else if (o == "mask" && w.size() > 2) { const QImage m = viewport_->pvAllowMask(paint_->doc.activeSet(), paint_->doc.width(), paint_->doc.height()); r = m.isNull() ? "no mask (nothing blocked)" : QString("%1x%2 saved %3").arg(m.width()).arg(m.height()).arg(m.save(w[2])); }
      else if (o == "shot" && w.size() > 2) { paintLeft_->refresh(); QApplication::processEvents(); r = QString("saved %1").arg(paintLeft_->grab().save(w[2])); }
      else if (o == "full") { paintLeft_->setFull(w.size() > 2 && w[2] == "1"); QApplication::processEvents(); }
      // (pv modes <png> — the drawing modes' menu opened and held, its picture saved)
      else if (o == "dockshow" && w.size() > 2) { if (QDockWidget* d = paintDock(w[2])) { d->show(); d->raise(); if (d->isFloating()) d->move(mapToGlobal(QPoint(width() - 520, 120))); QApplication::processEvents(); r = QString("shown %1x%2").arg(d->width()).arg(d->height()); } }   // (pv dockshow <name> — a Paint panel opened, as from ▦ Panels)
      else if (o == "refload" && w.size() > 2) { QDockWidget* d = paintDock("pdReference"); if (d) { QSettings().setValue("paint/refImage", w[2]); QWidget* old = d->widget(); d->setWidget(makePaintReferencePanel(paint_)); delete old; r = "loaded"; } }   // (pv refload <file> — the reference opened with that picture)
      else if (o == "dockgrab" && w.size() > 3) { if (QDockWidget* d = paintDock(w[2])) { QApplication::processEvents(); r = QString("saved %1").arg(d->grab().save(w[3])); } }
      else if (o == "dockopen") { for (auto* p : dpPaint_) p->setCollapsed(false); QApplication::processEvents(); }   // (pv dockopen — every panel of the Paint room's right column unfolded)
      else if (o == "dockshot" && w.size() > 2) { QApplication::processEvents(); r = QString("saved %1").arg(dock_->grab().save(w[2])); }   // (pv dockshot <png> — the right column: the palette, the layers…)
      // (pv menus <prefix> — every menu of the left column's settings and of the right column's panels, as pictures <prefix>N.png; a submenu — <prefix>N_M.png)
      else if (o == "menus" && w.size() > 2) { paintLeft_->refresh(); QApplication::processEvents(); QList<QMenu*> ms;
        for (QWidget* x : QApplication::allWidgets()) { if (!x->isVisible()) continue;   // (every button on screen that opens a menu — any room, any panel)
          if (auto* b = qobject_cast<QPushButton*>(x)) { if (b->menu()) ms << b->menu(); } else if (auto* tb = qobject_cast<QToolButton*>(x)) { if (tb->menu()) ms << tb->menu(); } }
        int n = 0; QStringList names;
        for (QMenu* m : ms) { m->ensurePolished(); m->adjustSize(); m->grab().save(w[2] + QString::number(n) + ".png"); QStringList it; int k = 0;
          for (QAction* a : m->actions()) { if (a->isSeparator()) continue; it << a->text(); if (a->menu()) { a->menu()->ensurePolished(); a->menu()->adjustSize(); a->menu()->grab().save(w[2] + QString("%1_%2.png").arg(n).arg(k++)); } }
          names << QString("%1: %2").arg(n).arg(it.join(" | ")); n++; }
        r = names.join(" /// "); }
      // (pv modeslive — the menu as it lives: opened under the pointer it must stay; opened under its button with the pointer elsewhere it must go at once)
      else if (o == "modeslive") { auto wait = [](int ms) { QElapsedTimer t; t.start(); while (t.elapsed() < ms) QApplication::processEvents(); };
        paintLeft_->modesMenu(true); wait(250); const bool a = paintLeft_->modesWidget() && paintLeft_->modesWidget()->isVisible(); if (paintLeft_->modesWidget()) paintLeft_->modesWidget()->close(); wait(60);
        paintLeft_->modesMenu(false); wait(250); const bool b = paintLeft_->modesWidget() && paintLeft_->modesWidget()->isVisible();
        r = QString("under the pointer: %1 · under its button, the pointer elsewhere: %2").arg(a ? "stays" : "gone", b ? "stays" : "gone"); }
      else if (o == "modes" && w.size() > 2) { paintLeft_->modesMenu(true, true); QApplication::processEvents(); QWidget* m = paintLeft_->modesWidget(); r = m ? QString("%1x%2 saved %3").arg(m->width()).arg(m->height()).arg(m->grab().save(w[2])) : QString("no menu"); if (m) m->close(); }
      else if (o == "roll" && w.size() > 2) viewport_->rollView(w[2].toDouble());
      else if (o == "rollkey" && w.size() > 2) viewport_->keyRoll = w[2] == "1";   // (pv rollkey 0|1 — R held: the next left drag rolls the view)
      else if (o == "disp" && w.size() > 2) viewport_->setPaintDisp(w[2]);
      // (pv freeze 0|1 · clear · invert · solid [smooth soft] · hard 0|1 · fromlayer 0|1 · keep <name> · use|add|sub <name> · drop <name> · fillat u,v [free] · png <file> · stat)
      else if (o == "freeze" && w.size() > 2) { const QString a = w[2]; QString x;
        if (a == "clear") paint_->freezeClear(); else if (a == "invert") paint_->freezeInvert();
        else if (a == "solid") { if (w.size() > 4) { paint_->freezeSmooth = w[3].toFloat(); paint_->freezeSoft = w[4].toFloat(); } paint_->freezeSolid(); }
        else if (a == "hard" && w.size() > 3) paint_->freezeHard = w[3] == "1";
        else if (a == "fromlayer" && w.size() > 3) x = QString(" ok %1").arg(paint_->freezeFromLayer(w[3] == "1"));
        else if (a == "keep" && w.size() > 3) paint_->freezeSave(w[3]);
        else if ((a == "use" || a == "add" || a == "sub") && w.size() > 3) x = QString(" ok %1").arg(paint_->freezeLoad(w[3], a == "use" ? 0 : a == "add" ? 1 : 2));
        else if (a == "drop" && w.size() > 3) paint_->freezeDrop(w[3]);
        else if (a == "fillat" && w.size() > 3) { const QStringList p = w[3].split(QChar(0x2c)); x = QString(" ok %1").arg(paint_->freezeFillAt(QPointF(p[0].toDouble() * paint_->doc.width(), p[1].toDouble() * paint_->doc.height()), w.size() > 4 && w[4] == "free")); }
        else if (a == "png" && w.size() > 3) { const QImage* m = paint_->freezeOf(false); x = QString(" saved %1").arg(m && m->save(w[3])); }
        else if (a == "stat") { qint64 full = 0, part = 0; if (const QImage* m = paint_->freezeOf(false)) for (int y = 0; y < m->height(); y++) { const uchar* q = m->constScanLine(y); for (int i = 0; i < m->width(); i++) { if (q[i] == 255) full++; else if (q[i]) part++; } } x = QString(" full %1 partly %2 kept [%3]").arg(full).arg(part).arg(paint_->freezeNames().join(", ")); }
        else paint_->freezePaint = a == "1";
        paintLeft_->syncTools(); r = QString("paint %1 any %2%3").arg(paint_->freezePaint).arg(paint_->freezeAny()).arg(x); }
      // (pv fillclick x,y [inv] — a click of the Fill tool at a window point · pv layerpng <file> — the active layer's pixels · pv layerstat — its painted / partly painted texels)
      else if (o == "fillclick" && w.size() > 2) { const QStringList p = w[2].split(QChar(0x2c)); const QPointF m(p[0].toDouble(), p[1].toDouble()); paintLeft_->tools()->fillAt(m, m, w.size() > 3 && w[3] == "inv"); r = ui::lastToast(); }
      // (pv setspng <prefix> — the colour map of every UV set as the document holds it, 512 px: <prefix><set>.png; the set shown stays)
      else if (o == "setspng" && w.size() > 2) { const QString cur = paint_->doc.activeSet(); const int cw = paint_->doc.width(), ch = paint_->doc.height(); QStringList done; const auto sets = paint_->doc.sets();
        auto one = [&](const QString& k) { const QImage f = paint_->doc.flatten(paint::Channel::Color); done << QString("%1 %2x%3 %4").arg(k).arg(f.width()).arg(f.height()).arg(f.isNull() ? 0 : int(f.scaled(512, 512).save(w[2] + k + ".png"))); };
        one(cur); for (const auto& x : sets) { if (x.key == cur || x.w <= 0) continue; paint_->doc.setActiveSet(x.key, x.w, x.h); one(x.key); } paint_->doc.setActiveSet(cur, cw, ch); r = done.join(" · "); }
      else if (o == "flatpng" && w.size() > 2) r = QString("saved %1").arg(paint_->doc.compose(paint::Channel::Color).save(w[2]));   // (checks: the active set's colour map, whole)
      else if (o == "islandpng" && w.size() > 2) { const paint::IslandMap* im = paint_->islands ? paint_->islands() : nullptr; r = QString("saved %1").arg(im && !im->ids.isNull() && im->ids.save(w[2])); }   // (checks: the island under every texel, its number in RGB)
      else if (o == "layerpng" && w.size() > 2) { const paint::Layer* l = paint_->active(); r = QString("saved %1").arg(l && l->image.save(w[2])); }
      else if (o == "layerstat") { qint64 full = 0, part = 0, noisy = 0; if (const paint::Layer* l = paint_->active(); l && l->image.format() == QImage::Format_ARGB32) for (int y = 0; y < l->image.height(); y++) { const QRgb* q = reinterpret_cast<const QRgb*>(l->image.constScanLine(y)); for (int i = 0; i < l->image.width(); i++) { const int a = qAlpha(q[i]); if (a == 255) full++; else if (a) part++;
            if (i > 0 && i + 1 < l->image.width() && std::abs(a - qAlpha(q[i - 1])) > 120 && std::abs(a - qAlpha(q[i + 1])) > 120) noisy++; } }   // (noisy: a texel whose density jumps away from both its neighbours — salt and pepper)
        r = QString("full %1 partly %2 single-texel jumps %3").arg(full).arg(part).arg(noisy); }
      // (pv mode stroke|polyline|lasso|rect|ellipse|faces · pv drawopt flat|through|smooth|outline 0|1 · pv linew <start> <end> <taper> · pv recolor <op>)
      else if ((o == "mode" || o == "shape") && w.size() > 2) { paintLeft_->tools()->cancel(); paint_->drawMode = w[2]; paintLeft_->syncTools(); }
      else if (o == "drawopt" && w.size() > 3) { const bool v = w[3] == "1"; if (w[2] == "flat") paint_->drawFlat = v; else if (w[2] == "through") paint_->setThrough(v, false); else if (w[2] == "smooth") paint_->lineSmooth = v; else if (w[2] == "outline") paint_->shapeOutline = v; }
      else if (o == "linew" && w.size() > 4) { paint_->lineStart = w[2].toFloat(); paint_->lineEnd = w[3].toFloat(); paint_->lineTaper = w[4].toFloat(); }
      else if (o == "recolor" && w.size() > 2) paint_->recolorOp = paint::regionOpFromKey(w[2]);
      // (pv region erase|copy|cut|adjust <frozen|free> [adjust kind key] · pv blend <key> · pv grad <pos:#colour,pos:#colour…> — the active Gradient Map's stops · pv compstat u,v — the Colour map there)
      else if (o == "region" && w.size() > 3) { const bool fr = w[3] == "free";
        if (w[2] == "erase") r = QString("ok %1").arg(paint_->regionErase(fr)); else if (w[2] == "copy" || w[2] == "cut") r = QString("layer %1").arg(paint_->regionLift(fr, w[2] == "cut"));
        else if (w[2] == "adjust" && w.size() > 4) r = QString("layer %1").arg(paint_->regionAdjust(paint::adjustFromKey(w[4]), fr)); }
      // (pv layerat u,v — the layer painted there · pv smartname — the active layer renamed · pv solo 0|1 · pv names — the layers' names, top first)
      // (pv piece lasso u,v u,v u,v… — a piece outlined on the texture · pv piece move du,dv · turn deg · scale k · flip x|y · cut 0|1 · newlayer 0|1 · lay · drop)
      else if (o == "piece" && w.size() > 2 && paintLeft_) { PaintTools* T = paintLeft_->tools(); PaintCanvas* cv = paintLeft_->canvas(); const double W = paint_->doc.width(), H = paint_->doc.height(); const QString a = w[2];
        auto pt = [&](const QString& sx) { const QStringList p = sx.split(QChar(0x2c)); return QPointF(p[0].toDouble() * W, p[1].toDouble() * H); };
        if (a == "lasso" && w.size() > 5) { paintLeft_->setTool("piece"); cv->toolInput(0, pt(w[3]), Qt::NoModifier); for (int i = 4; i < w.size(); i++) { const QPointF b0 = pt(w[i - 1]), b1 = pt(w[i]); for (int k = 1; k <= 8; k++) cv->toolInput(1, b0 + (b1 - b0) * (k / 8.0), Qt::NoModifier); } cv->toolInput(2, pt(w.last()), Qt::NoModifier); }
        else if (a == "move" && w.size() > 3) { T->pieceNudge(pt(w[3])); }
        else if (a == "turn" && w.size() > 3) T->pieceTurn(w[3].toDouble()); else if (a == "scale" && w.size() > 3) T->pieceScale(w[3].toDouble());
        else if (a == "flip" && w.size() > 3) T->pieceFlip(w[3] == "x"); else if (a == "cut" && w.size() > 3) T->pieceCut = w[3] == "1"; else if (a == "newlayer" && w.size() > 3) T->pieceNewLayer = w[3] == "1";
        else if (a == "lay") T->pieceApply(); else if (a == "drop") T->pieceDrop();
        r = QString("floating %1 · %2").arg(T->pieceOn()).arg(ui::lastToast()); }
      else if (o == "strokems" && w.size() > 2) {               // (pv strokems <brush size, texels> — a stroke across the texture with that brush: how long each step took)
        PaintCanvas* cv = paintLeft_->canvas(); const float was = paint_->brush.size; paint_->brush.size = w[2].toFloat(); QElapsedTimer t; double worst = 0, sum = 0; const int n = 40;
        for (int i = 0; i <= n; i++) { t.restart(); cv->strokeTex(i ? 1 : 0, QPointF(0.2 + 0.6 * i / n, 0.3 + 0.4 * i / n), false); QApplication::processEvents(); const double ms = t.nsecsElapsed() / 1e6; worst = std::max(worst, ms); sum += ms; }
        t.restart(); cv->strokeTex(2, {}, false); QApplication::processEvents(); paint_->brush.size = was;
        r = QString("brush %1 px on %2x%3: %4 ms a step on average, %5 ms the slowest, the end %6 ms").arg(w[2]).arg(paint_->doc.width()).arg(paint_->doc.height()).arg(sum / (n + 1), 0, 'f', 1).arg(worst, 0, 'f', 1).arg(t.nsecsElapsed() / 1e6, 0, 'f', 1); }
      else if (o == "psdsend" && w.size() > 2) r = paint_->sendLayers(paintWireImage(), w[2]);   // (pv psdsend <file> — all the layers written there · pv psdback <file> — taken back from it)
      else if (o == "psdback" && w.size() > 2) r = paint_->takeLayers(w[2]);
      else if (o == "layerat" && w.size() > 2) { const QStringList p = w[2].split(QChar(0x2c)); const int id = paint_->doc.layerAt(paint_->modeChannel(), int(p[0].toDouble() * paint_->doc.width()), int(p[1].toDouble() * paint_->doc.height())); r = QString("%1 %2").arg(id).arg(paint_->doc.layer(id) ? paint_->doc.layer(id)->name : QString("-")); }
      else if (o == "smartname") { if (paint::Layer* l = paint_->active()) { l->name = paint_->doc.smartName(l->id); r = l->name; emit paint_->layersChanged(); } }
      else if (o == "solo" && w.size() > 2) { paint_->doc.solo = w[2] == "1" ? paint_->layer : -1; emit paint_->pixelsChanged({}); }
      else if (o == "names") { QStringList n; const auto& L = paint_->doc.layers(); for (int i = int(L.size()) - 1; i >= 0; i--) n << L[size_t(i)].name + (paint_->doc.isBlank(L[size_t(i)]) ? " (blank)" : ""); r = n.join(" | "); }
      else if (o == "dellayer") { paint_->doc.remove(paint_->layer); paint_->layer = paint_->doc.layers().empty() ? -1 : paint_->doc.layers().back().id; emit paint_->layersChanged(); }
      else if (o == "adjp" && w.size() > 4) { if (paint::Layer* l = paint_->active()) { l->p1 = w[2].toFloat(); l->p2 = w[3].toFloat(); l->p3 = w[4].toFloat(); emit paint_->pixelsChanged({}); } }   // (pv adjp p1 p2 p3: the active adjustment's parameters)
      else if (o == "blend" && w.size() > 2) { if (paint::Layer* l = paint_->active()) { l->blend = paint::blendFromKey(w[2]); emit paint_->pixelsChanged({}); r = paint::blendName(l->blend); } }
      else if (o == "grad" && w.size() > 2) { if (paint::Layer* l = paint_->active()) { l->grad.clear(); for (const QString& st : w[2].split(QChar(0x2c))) { const QStringList p = st.split(':'); if (p.size() == 2) l->grad.push_back({p[0].toFloat(), QColor(p[1])}); } emit paint_->pixelsChanged({}); r = QString("%1 stops").arg(l->grad.size()); } }
      else if (o == "pick" && w.size() > 3) { paint_->pickSize = w[2].toInt(); paint_->pickFrom = w[3]; }   // (pv pick <1|3|5|0 — under the brush> <all|layer|screen>)
      else if (o == "toolkey" && w.size() > 2) r = QString("taken %1").arg(viewport_->paintToolKey && viewport_->paintToolKey(w[2] == "enter" ? Qt::Key_Return : w[2] == "esc" ? Qt::Key_Escape : Qt::Key_Backspace));
      // (pv stampmode 0|1|2 [all 0|1] — the Stamp's mode · pv mirrorbox — where the active layer's paint lands under the symmetry, in UV ·
      // pv redin u0,v0,u1,v1 — the strongly red texels of the active layer inside that box)
      // (pv tin x,y,phase … — the room's tool input at window points, phases 0 press · 1 drag · 2 release · pv tkey enter|esc|back — a key to the tool;
      // both answer with the undo steps on the stack and the active layer's painted texels)
      else if ((o == "tin" && w.size() > 2) || (o == "tkeyp" && w.size() > 2)) {
        if (o == "tin") for (int i = 2; i < w.size(); i++) { const QStringList p = w[i].split(QChar(0x2c)); if (p.size() >= 3) viewport_->paintToolInput(p[2].toInt(), QPointF(p[0].toDouble(), p[1].toDouble()), {}); }
        else viewport_->paintToolKey(w[2] == "enter" ? Qt::Key_Return : w[2] == "esc" ? Qt::Key_Escape : Qt::Key_Backspace);
        QApplication::processEvents(); qint64 n = 0; if (const paint::Layer* l = paint_->active(); l && l->image.format() == QImage::Format_ARGB32) for (int y = 0; y < l->image.height(); y += 2) { const QRgb* q = reinterpret_cast<const QRgb*>(l->image.constScanLine(y)); for (int x = 0; x < l->image.width(); x += 2) n += qAlpha(q[x]) > 64; }
        r = QString("undo %1 · painted %2 · %3 · %4").arg(paint_->undo.size()).arg(n).arg(paintLeft_->tools()->lineState()).arg(ui::lastToast()); }
      else if (o == "match") r = paint_->matchColourBelow(paint_->layer);   // (pv match — the active layer's colour matched to what lies under it)
      else if (o == "meanin" && w.size() > 2) { const QStringList q = w[2].split(QChar(0x2c)); const paint::Layer* l = paint_->active(); double R = 0, G = 0, B = 0; qint64 n = 0;   // (pv meanin u0,v0,u1,v1 — the active layer's mean colour there)
        if (l && !l->image.isNull() && q.size() == 4) { const int W = l->image.width(), H = l->image.height(); for (int y = int(q[1].toDouble() * H); y < int(q[3].toDouble() * H); y += 3) for (int x = int(q[0].toDouble() * W); x < int(q[2].toDouble() * W); x += 3) { const QRgb c = l->image.pixel(std::clamp(x, 0, W - 1), std::clamp(y, 0, H - 1)); if (qAlpha(c) > 128) { R += qRed(c); G += qGreen(c); B += qBlue(c); n++; } } }
        r = n ? QString("mean %1,%2,%3 of %4").arg(int(R / n)).arg(int(G / n)).arg(int(B / n)).arg(n) : QString("nothing"); }
      else if (o == "stampmode" && w.size() > 2) { paint_->stampMode = w[2].toInt(); if (w.size() > 3) paint_->stampAll = w[3] == "1"; paintLeft_->syncTools(); r = QString("mode %1 all %2").arg(paint_->stampMode).arg(paint_->stampAll); }
      else if (o == "mirrorbox") { const paint::Layer* l = paint_->active(); PaintCanvas* cv = paintLeft_->canvas(); QRectF bx; int n = 0, m = 0;
        if (l && !l->image.isNull() && cv->mirrorOf) for (int y = 0; y < l->image.height(); y += 6) for (int x = 0; x < l->image.width(); x += 6) if (qAlpha(l->image.pixel(x, y)) > 64) { n++;
          if (const auto q = cv->mirrorOf(QPointF(x + 0.5, y + 0.5))) { m++; const QPointF u(q->x() / l->image.width(), q->y() / l->image.height()); bx = bx.isNull() ? QRectF(u, QSizeF(1e-6, 1e-6)) : bx.united(QRectF(u, QSizeF(1e-6, 1e-6))); } }
        r = QString("painted %1 mirrored %2 box %3,%4,%5,%6").arg(n).arg(m).arg(bx.left(), 0, 'f', 4).arg(bx.top(), 0, 'f', 4).arg(bx.right(), 0, 'f', 4).arg(bx.bottom(), 0, 'f', 4); }
      else if (o == "redin" && w.size() > 2) { const QStringList q = w[2].split(QChar(0x2c)); const paint::Layer* l = paint_->active(); int red = 0, any = 0;
        if (l && !l->image.isNull() && q.size() == 4) { const int W = l->image.width(), H = l->image.height(); for (int y = int(q[1].toDouble() * H); y < int(q[3].toDouble() * H); y++) for (int x = int(q[0].toDouble() * W); x < int(q[2].toDouble() * W); x++) { const QRgb c = l->image.pixel(std::clamp(x, 0, W - 1), std::clamp(y, 0, H - 1)); if (qAlpha(c) > 64) { any++; if (qRed(c) > 180 && qBlue(c) < 90) red++; } } }
        r = QString("painted %1, red %2").arg(any).arg(red); }
      else if (o == "texstroke" && w.size() > 3) { const QStringList a = w[2].split(QChar(0x2c)), b = w[3].split(QChar(0x2c)); PaintCanvas* cv = paintLeft_->canvas();   // (a stroke on the texture itself: u,v → u,v)
        for (int i = 0; i <= 24; i++) { const double k = i / 24.0; cv->strokeTex(i ? 1 : 0, QPointF(a[0].toDouble() + (b[0].toDouble() - a[0].toDouble()) * k, a[1].toDouble() + (b[1].toDouble() - a[1].toDouble()) * k), false); } cv->strokeTex(2, {}, false); }
      else if (o == "mat" && w.size() > 2) { if (w[2] == "open") { r = "the material builder is taken out of the Paint room (19.11)"; } else if (w[2] == "toon") { paint_->material = paint::MatGraph::toon(); emit paint_->settingsChanged(); }
        else if (w[2] == "shot" && w.size() > 3) { QApplication::processEvents(); r = QString("saved %1").arg(materialEditorShot(w[3])); } else if (w[2] == "close") closeMaterialEditor(); }
      else if (o == "tkey" && w.size() > 2) { if (w.size() > 3) { const QStringList p = w[3].split(QChar(0x2c)); viewport_->setMouseAt(QPointF(p[0].toDouble(), p[1].toDouble())); } r = viewport_->tKeyUnder(w[2]) ? "taken: " + ui::lastToast() : "not taken: " + viewport_->tKeyWhy_; }
      else if (o == "proj" && w.size() > 2 && paintProj_) { if (w[2] == "stencil") r = QString("stencil %1").arg(paintProj_->setStencil(w.size() > 3 ? w[3] : QString())); else if (w[2] == "strip") r = QString("strip %1").arg(paintProj_->setStrip(w.size() > 3 ? w[3] : QString()));
        else if (w[2] == "stat") r = paintProj_->stat; else if (w[2] == "stats") { r = "\n" + paintProj_->statAll; paintProj_->statAll.clear(); } else if (w[2] == "stscale" && w.size() > 3) paintProj_->stScale = w[3].toDouble(); else if (w[2] == "send") r = paintProj_->sendView(w.size() > 3 ? w[3] : QString()); else if (w[2] == "angle" && w.size() > 6) { paintProj_->angleFull = w[3].toDouble(); paintProj_->angleNone = w[4].toDouble(); paintProj_->skipEdge = w[5] == "1"; paintProj_->checkSeen = w[6] == "1"; r = "ok"; } else if (w[2] == "back" && w.size() > 3) r = (w[3].endsWith(".psd", Qt::CaseInsensitive) ? paintProj_->takeBackFile(w[3]) : paintProj_->takeBack(QImage(w[3]))) + " | " + paintProj_->stat; else paintProj_->on = w[2] == "1"; }
      else if (o == "facesin") { const QRectF bx(0, 0, viewport_->paneW(), viewport_->paneH()); const auto a = viewport_->pvFacesIn(bx, paint_->doc.activeSet(), true, false), b = viewport_->pvFacesIn(bx, paint_->doc.activeSet(), true, true); int mir = 0; for (const auto& f : b) mir += f.mirror > 0; QStringList fc; for (int m : viewport_->pvMeshes()) { const Item* x = scene_.item(m); if (x->poly && x->base && x->poly != x->base) fc << QString("%1 poly %2 base %3").arg(x->name).arg(x->poly->faceCount()).arg(x->base->faceCount()); } log << fc.join(" | ");
        r = viewport_->pvFacesWhy_ + QString(" plain %1 · with mirrors %2 (mirrored %3) · pane %4x%5 · proj %6").arg(a.size()).arg(b.size()).arg(mir).arg(viewport_->paneW()).arg(viewport_->paneH()).arg(paintProj_ ? paintProj_->on : -1); }
      else if (o == "fillkind" && w.size() > 2) paintLeft_->tools()->fillKind = w[2];
      else if (o == "gradient" && w.size() > 2) paintLeft_->tools()->fillGradient = w[2] == "1";
      else if (o == "fillaslayer" && w.size() > 2) paintLeft_->tools()->fillAsLayer = w[2] == "1";
      else if (o == "sizescreen" && w.size() > 2) viewport_->paintSizeScreen = w[2] == "1";
      else if (o == "layerflag" && w.size() > 3) { if (paint::Layer* l = paint_->active()) { if (w[2] == "clip") l->clip = w[3] == "1"; else l->lockAlpha = w[3] == "1"; emit paint_->pixelsChanged({}); } }
      else if (o == "scalp" && w.size() > 2) r = QString("scalp %1").arg(viewport_->pvSetScalp(w[2] == "1"));   // (checks) pv scalp 1|0: the selected polygons marked / unmarked
      else if (o == "scalpnear" && w.size() > 3) { paint_->scalpReach = w[3].toFloat(); const QImage n = viewport_->pvScalpNear(paint_->doc.activeSet(), paint_->doc.width(), paint_->doc.height(), paint_->scalpReach * viewport_->pvSceneSize()); r = QString("saved %1").arg(!n.isNull() && n.save(w[2])); }
      else if (o == "cavity" && w.size() > 2) paint_->cavity = w[2].toInt();
      else if ((o == "import" || o == "importgrow") && w.size() > 2) { const bool g = o == "importgrow"; r = paint_->importLayers(w.mid(2).join(QChar(0x20)), [this, g](int fw, int fh) { if (!g) return 0; viewport_->setUvSetSize(paint_->doc.activeSet(), fw, fh); return 1; }); }   // (checks) pv import[grow] <file>: layers from a file (grow: the set made the file's size)   // (checks) pv import <file>: layers from a file
      // (checks of the live layers: pv livegrad — a live gradient top → bottom of the UV set shown · pv livespheres · pv sphere x,y [ctrl|to x,y]
      //  · pv spherer <share of the model> <hardness> [link] · pv gstops #a,#b · pv rasterize — the active layer an ordinary one · pv layerinfo)
      else if (o == "livegrad" || o == "livespheres") { paint_->ensureDoc(); const bool sph = o == "livespheres"; const int id = paint_->doc.addFill(paint_->modeChannel(), paint_->color, paint_->doc.freeName(sph ? "Spheres" : "Gradient"), paint_->layer);
        paint::Layer* f = paint_->doc.layer(id); f->gradLive = true; if (sph) f->liveKind = "spheres"; if (!paint_->doc.activeSet().isEmpty()) f->onlySets = QStringList{paint_->doc.activeSet()};
        if (!sph) { Box3 b; const auto& G = viewport_->pvBakeGeom(paint_->doc.activeSet(), paint_->doc.width(), paint_->doc.height());
          for (size_t q = 0; q + 2 < G.p.size(); q += 3) if (G.n[q] != 0 || G.n[q + 1] != 0 || G.n[q + 2] != 0) b.expand(Vec3(G.p[q], G.p[q + 1], G.p[q + 2]));
          const Vec3 c = b.center(); f->g0[0] = float(c.x); f->g0[1] = float(b.max.y); f->g0[2] = float(c.z); f->g1[0] = float(c.x); f->g1[1] = float(b.min.y); f->g1[2] = float(c.z);
          f->grad = {{0.f, paint_->color}, {1.f, paint_->color2}}; r = QString("box y %1…%2").arg(b.min.y).arg(b.max.y); }
        paint_->layer = id; paint_->sphereSel = -1; emit paint_->layersChanged(); emit paint_->pixelsChanged({}); }
      else if (o == "sphere" && w.size() > 2) { const QStringList a = w[2].split(QChar(0x2c)); const QPointF pa(a[0].toDouble(), a[1].toDouble()); QPointF pb = pa;
        if (w.size() > 4 && w[3] == "to") { const QStringList b2 = w[4].split(QChar(0x2c)); pb = QPointF(b2[0].toDouble(), b2[1].toDouble()); }
        paintLeft_->setTool("fill"); paintLeft_->tools()->spheresAt(pa, pb, w.size() > 3 && w[3] == "ctrl"); const paint::Layer* l = paint_->active(); r = QString("%1 spheres, picked %2 · %3").arg(l ? int(l->spheres.size()) : -1).arg(paint_->sphereSel).arg(ui::lastToast()); }
      else if (o == "spherer" && w.size() > 3) { paint::Layer* l = paint_->active(); if (l && paint_->sphereSel >= 0 && paint_->sphereSel < int(l->spheres.size())) { auto& sp = l->spheres[size_t(paint_->sphereSel)]; sp.r = float(w[2].toDouble() * viewport_->pvSceneSize()); sp.hard = w[3].toFloat(); if (w.size() > 4) sp.link = w[4].toInt(); emit paint_->pixelsChanged({}); r = "set"; } }
      else if (o == "gstops" && w.size() > 2) { if (paint::Layer* l = paint_->active()) { const QStringList c = w[2].split(QChar(0x2c)); l->grad.clear(); for (int i = 0; i < c.size(); i++) l->grad.push_back({c.size() > 1 ? float(i) / (c.size() - 1) : 0.f, QColor(c[i])}); emit paint_->pixelsChanged({}); } }
      else if (o == "rasterize") { QElapsedTimer t; t.start(); r = QString("%1 in %2 ms").arg(paint_->doc.rasterize(paint_->layer)).arg(t.elapsed()); emit paint_->layersChanged(); emit paint_->pixelsChanged({}); }
      else if (o == "layerinfo") { if (const paint::Layer* l = paint_->active()) r = QString("%1 kind %2 live %3 %4 sets %5 blend %6 op %7 image %8").arg(l->name).arg(int(l->kind)).arg(l->gradLive).arg(l->liveKind).arg(l->onlySets.join("+")).arg(paint::blendName(l->blend)).arg(l->opacity).arg(!l->image.isNull()); }
      else if (o == "blendhover" && w.size() > 2) {   // (the blend list opened, an item under the pointer, closed with no choice: what the layer showed, and after)
        QComboBox* cb = nullptr; for (QComboBox* c : findChildren<QComboBox*>()) if (c->toolTip().contains("blend. With the list open") && c->isVisible()) cb = c;
        if (!cb) r = "no blend list"; else { const QString was = paint_->active() ? paint::blendName(paint_->active()->blend) : "-"; cb->showPopup(); QApplication::processEvents(); emit cb->highlighted(w[2].toInt());
          const QString during = paint_->active() ? paint::blendName(paint_->active()->blend) : "-"; cb->hidePopup(); QApplication::processEvents(); const QString after = paint_->active() ? paint::blendName(paint_->active()->blend) : "-";
          r = QString("before %1 · hovered %2 · closed %3").arg(was, during, after); } }
      else if (o == "fillat" && w.size() > 3) { const QStringList a = w[2].split(QChar(0x2c)), b = w[3].split(QChar(0x2c)); paintLeft_->setTool("fill");   // (pv fillat x,y x,y [ctrl]: Fill's drag in the viewport's own points)
        paintLeft_->tools()->fillAt(QPointF(a[0].toDouble(), a[1].toDouble()), QPointF(b[0].toDouble(), b[1].toDouble()), w.size() > 4 && w[4] == "ctrl"); r = ui::lastToast(); }
      else if (o == "gpick" && w.size() > 3) { const QStringList a = w[3].split(QChar(0x2c)); paint_->gradPick = w[2].toInt(); const QPointF pt(a[0].toDouble(), a[1].toDouble());   // (pv gpick 1|2 x,y: Pick then a click)
        viewport_->paintToolInput(0, pt, {}); viewport_->paintToolInput(2, pt, {}); r = ui::lastToast(); }
      else if (o == "gpoints") { if (const paint::Layer* l = paint_->active()) { r = QString("start %1 %2 %3 · end %4 %5 %6").arg(l->g0[0]).arg(l->g0[1]).arg(l->g0[2]).arg(l->g1[0]).arg(l->g1[1]).arg(l->g1[2]);
          for (int k = 1; k <= 2; k++) if (const auto e = paintLeft_->tools()->gradEndScreen(k)) r += QString(" · screen%1 %2,%3").arg(k).arg(e->x()).arg(e->y()); } }
      else if (o == "dock" && w.size() > 3) { QDockWidget* d = paintDock(w[2]); if (!d) r = "no dock";   // (pv dock <name> left|right|bottom|float|tab:<other> — a panel moved as a drag would)
        else { if (w[3] == "float") d->setFloating(true); else if (w[3].startsWith("tab:")) { if (QDockWidget* o2 = paintDock(w[3].mid(4))) tabifyDockWidget(o2, d); }
          else addDockWidget(w[3] == "left" ? Qt::LeftDockWidgetArea : w[3] == "bottom" ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea, d); d->show(); QApplication::processEvents(); r = "moved"; } }
      else if (o == "docks") { QStringList l; for (QDockWidget* d : paintDocks_) l << QString("%1:%2%3%4").arg(d->objectName()).arg(dockWidgetArea(d) == Qt::LeftDockWidgetArea ? "L" : dockWidgetArea(d) == Qt::RightDockWidgetArea ? "R" : dockWidgetArea(d) == Qt::BottomDockWidgetArea ? "B" : "-").arg(d->isFloating() ? "F" : "").arg(d->isVisible() ? "" : d->isHidden() ? "(hidden)" : "(not shown)") + (d->isFloating() ? QString("@%1,%2").arg(d->x()).arg(d->y()) : QString()); r = l.join(" "); }
      else if (o == "mgrsize" && w.size() > 3) { paintLeft_->setFloatSize(w[2].toInt(), w[3].toInt()); QApplication::processEvents(); r = QString("canvas %1x%2").arg(paintLeft_->canvas()->width()).arg(paintLeft_->canvas()->height()); }   // (pv mgrsize w h: the floating manager sized)
      else if (o == "fullshot" && w.size() > 2) { QApplication::processEvents(); QImage im = grab().toImage();   // (pv fullshot <png>: the whole window with the manager's own panel drawn where it floats)
        if (QWidget* f = paintLeft_->floatWindow(); f && f->isVisible()) { QPainter q(&im); const QPoint at = f->mapToGlobal(QPoint(0, 0)) - mapToGlobal(QPoint(0, 0)); const qreal k = im.devicePixelRatio(); q.drawImage(QPointF(at) , f->grab().toImage()); r = QString("panel at %1,%2 %3x%4 · ").arg(at.x()).arg(at.y()).arg(f->width()).arg(f->height()); (void)k; }
        r += QString("saved %1").arg(im.save(w[2])); }
      else if (o == "composems") { QElapsedTimer t; t.start(); const QImage c = paint_->doc.compose(paint_->modeChannel()); r = QString("%1x%2 in %3 ms").arg(c.width()).arg(c.height()).arg(t.elapsed()); }
      else if (o == "addmask") { paint_->doc.addMask(paint_->layer, true); paint_->editMask = true; emit paint_->layersChanged(); emit paint_->brushChanged(); }   // (pv addmask — a white mask on the active layer, the brush on it)
      else if (o == "maskfill" && w.size() > 2) { if (paint::Layer* l = paint_->active(); l && !l->mask.isNull()) { QPainter pm(&l->mask); pm.setRenderHint(QPainter::Antialiasing); pm.setPen(Qt::NoPen); pm.setBrush(Qt::black); const int W = l->mask.width(); pm.drawEllipse(QPointF(W * 0.5, W * 0.5), W * w[2].toDouble(), W * w[2].toDouble()); } emit paint_->pixelsChanged({}); emit paint_->layersChanged(); }
      else if (o == "showmask") { paint_->showMask = !paint_->showMask; emit paint_->pixelsChanged({}); emit paint_->layersChanged(); r = QString("show mask %1 · edit mask %2").arg(paint_->showMask).arg(paint_->editMask); }
      else if (o == "dockhelp" && w.size() > 3) { QDockWidget* d = paintDock(w[2]); QToolButton* q = nullptr; if (d && d->titleBarWidget()) for (auto* b : d->titleBarWidget()->findChildren<QToolButton*>()) if (b->text() == "?") q = b;   // (pv dockhelp <dock> <png>: its "?" opened, the note's picture)
        if (q) { q->click(); QApplication::processEvents(); } QWidget* pop = QApplication::activePopupWidget(); r = QString("help %1").arg(pop ? QString("saved %1").arg(pop->grab().save(w[3])) : QString("none")); if (pop) pop->close(); }
      else if (o == "cvrot") { PaintCanvas* cv = paintLeft_->canvas(); const double r0 = cv->rotation();   // (pv cvrot: R held (as the filter hands it on — a Russian layout's К) + a drag on the manager's texture)
        auto key = [&](QEvent::Type t) { QKeyEvent ev(t, 0x41a, Qt::NoModifier, 0x13, 0x52, 0, QString::fromUtf8("к")); static_cast<QObject*>(cv)->event(&ev); };   // (straight to it: the check run's pointer is not over it, the window's key filter would take the key)
        auto mouse = [&](QEvent::Type t, QPointF at, Qt::MouseButtons bs) { QMouseEvent ev(t, at, cv->mapToGlobal(at), Qt::LeftButton, bs, Qt::NoModifier); QCoreApplication::sendEvent(cv, &ev); };
        const QPointF c(cv->width() / 2.0, cv->height() / 2.0); key(QEvent::KeyPress); mouse(QEvent::MouseButtonPress, c + QPointF(60, 0), Qt::LeftButton);
        mouse(QEvent::MouseMove, c + QPointF(40, 40), Qt::LeftButton); mouse(QEvent::MouseMove, c + QPointF(0, 60), Qt::LeftButton); mouse(QEvent::MouseButtonRelease, c + QPointF(0, 60), Qt::NoButton); key(QEvent::KeyRelease);
        r = QString("rotation %1 -> %2").arg(r0).arg(cv->rotation()); }
      else if (o == "uvset") { const QString k = w.size() > 2 ? w[2] : QString(); const auto t = viewport_->uvSetProps(k); paint_->setSet(k, k.isEmpty() ? 2048 : t.w, k.isEmpty() ? 2048 : t.h); QApplication::processEvents(); r = QString("set '%1'").arg(paint_->doc.activeSet()); }   // (pv uvset [name]: the UV set painted; none — the single texture)
      else if (o == "curvefill" && w.size() > 3) { PaintTools* tl = paintLeft_->tools(); paintLeft_->setTool("fill"); tl->fillCurve = true; tl->fillGradient = tl->fillSpheres = false;   // (pv curvefill x,y x,y …: Fill → Along a curve, drawn through those viewport points)
        for (int i = 2; i < w.size(); i++) { const QStringList p = w[i].split(QChar(0x2c)); viewport_->paintToolInput(i == 2 ? 0 : 1, QPointF(p[0].toDouble(), p[1].toDouble()), {}); }
        { const QStringList p = w.last().split(QChar(0x2c)); viewport_->paintToolInput(2, QPointF(p[0].toDouble(), p[1].toDouble()), {}); } QApplication::processEvents(); r = ui::lastToast(); }
      else if (o == "fillall") { paintLeft_->tools()->fillAll(); r = ui::lastToast(); }
      else if (o == "alpha" && w.size() > 2) { const bool ok = paint_->applyAlpha(w[2]); QApplication::processEvents();   // (pv alpha <key>: as a click on that tile)
        r = QString("%1 · key %2 · kind %3 · hardness %4 · soft %5 · stand-in %6").arg(ok).arg(paint_->alphaKey).arg(int(paint_->brush.tipKind)).arg(paint_->brush.hardness).arg(paint_->brush.tipSoft).arg(paint_->alphaStandIn); }
      else if (o == "altline" && w.size() > 2) { QDockWidget* d = paintDock("pdLayers"); QListWidget* lw = d ? d->findChild<QListWidget*>() : nullptr; const int row = w[2].toInt();   // (pv altline <row>: Alt + click on the line under that row, as Photoshop's clipping)
        if (lw && row >= 0 && row + 1 < lw->count()) { const QRect a = lw->visualItemRect(lw->item(row)); const QPointF pt(a.center().x(), a.bottom() + 1);
          QMouseEvent pr(QEvent::MouseButtonPress, pt, lw->viewport()->mapToGlobal(pt), Qt::LeftButton, Qt::LeftButton, Qt::AltModifier); QApplication::sendEvent(lw->viewport(), &pr);
          QMouseEvent rl(QEvent::MouseButtonRelease, pt, lw->viewport()->mapToGlobal(pt), Qt::LeftButton, Qt::NoButton, Qt::AltModifier); QApplication::sendEvent(lw->viewport(), &rl); QApplication::processEvents(); }
        QStringList st; for (const auto& l : paint_->doc.layers()) if (l.channel == paint_->modeChannel()) st << QString("%1%2").arg(l.name).arg(l.clip ? "(clip)" : ""); r = st.join(", "); }
      else if (o == "rename" && w.size() > 2) { QDockWidget* d = paintDock("pdLayers"); QListWidget* lw = d ? d->findChild<QListWidget*>() : nullptr; QListWidgetItem* it = lw ? lw->currentItem() : nullptr;   // (pv rename <name>: the active layer's row edited in place)
        if (it) { lw->editItem(it); QApplication::processEvents(); if (auto* e = lw->findChild<QLineEdit*>()) { e->setText(w.mid(2).join(' ')); QKeyEvent k(QEvent::KeyPress, Qt::Key_Return, {}); QApplication::sendEvent(e, &k); } QApplication::processEvents(); }
        r = QString("editor %1 · active %2").arg(it != nullptr).arg(paint_->active() ? paint_->active()->name : QString("none")); }
      else if (o == "toppop" && w.size() > 2) { QPushButton* b = w[2] == "bake" ? bakeBtn_ : brgBtn_; if (b) b->click(); QApplication::processEvents(); QFrame* f = w[2] == "bake" ? bakePop_ : bridgePop_;   // (pv toppop bake|bridge [png])
        r = QString("open %1").arg(f && f->isVisible()); if (f && w.size() > 3) r += QString(" saved %1").arg(f->grab().save(w[3])); }
      else if (o == "addlayer") { paint_->layer = paint_->doc.addPaint(paint_->modeChannel(), {}, paint_->layer); emit paint_->layersChanged(); }
      else if (o == "color" && w.size() > 2) { paint_->color = QColor(w[2]); emit paint_->brushChanged(); }
      else if (o == "size" && w.size() > 2) { paint_->brush.size = w[2].toFloat(); emit paint_->brushChanged(); }
      else if ((o == "opacity" || o == "hardness" || o == "flow") && w.size() > 2) { const float v = w[2].toFloat(); if (o == "opacity") paint_->brush.opacity = v; else if (o == "flow") paint_->brush.flow = v; else { paint_->brush.hardness = v; paint_->brush.tip = {}; } emit paint_->brushChanged(); }
      // (pv thicktube <R> <thick> — a tube with a thickness for the wrap check · pv linewalls any|facing — how a laid line picks the wall: the old way / the new)
      else if (o == "thicktube" && w.size() > 3) { const int last = viewport_->testThickTube(w[2].toDouble(), w[3].toDouble()); paintTubeId_ = last; r = QString("tube %1").arg(last); }
      else if (o == "linewalls" && w.size() > 2) { hm::g_lineAnyWall = w[2] == "any"; r = w[2]; }
      // (pv wraptest <item> <a> <b> [n] — a closed line laid round a limb by n points on its surface (Line); then where the paint went:
      // every painted texel taken back onto the model — its distance from the ring's plane, and round the ring's axis which of 36 sectors it is in)
      else if (o == "wraptest" && w.size() > 4) {   // (or: pv wraptest <x,y window point> <share of the size> <n>)
        Vec3 c, ax; double rad = 0; if (w[2].contains(QChar(0x2c))) { const QStringList q = w[2].split(QChar(0x2c)); r = viewport_->testWrapLineAt(QPointF(q[0].toDouble(), q[1].toDouble()), w[3].toDouble(), w[4].toInt(), &c, &ax, &rad); }
        else r = viewport_->testWrapLine(w[2] == "tube" ? paintTubeId_ : w[2].toInt(), w[3].toDouble(), w[4].toDouble(), w.size() > 5 ? w[5].toInt() : 8, &c, &ax, &rad);
        if (const paint::Layer* l = paint_->active(); l && l->image.format() == QImage::Format_ARGB32 && rad > 0) {
          const QImage& im = l->image; const double W = im.width(), H = im.height(); std::vector<double> dz; std::vector<int> sect(36, 0); int painted = 0, onModel = 0, inward = 0; std::vector<double> rIn, rOut; QRect inBox; std::vector<Vec3> opP, opN; int opItem = -1;
          const Vec3 e1 = (std::abs(ax.y) < 0.9 ? Vec3(0, 1, 0) : Vec3(1, 0, 0)).cross(ax).normalized(), e2 = ax.cross(e1).normalized();
          for (int y = 0; y < im.height(); y += 2) { const QRgb* q = reinterpret_cast<const QRgb*>(im.constScanLine(y)); for (int x = 0; x < im.width(); x += 2) if (qAlpha(q[x]) > 64) { painted++;
            const auto h = viewport_->paintHitOfUv(QPointF((x + 0.5) / W, (y + 0.5) / H)); if (!h) continue; onModel++; const Vec3 d = h->point - c; dz.push_back(std::abs(d.dot(ax)) / rad); { const Vec3 rr = (d - ax * d.dot(ax)).normalized(); const double rd = (d - ax * d.dot(ax)).length() / rad; if (opP.size() < 4000 && (x + y) % 4 == 0) { opP.push_back(h->point); opN.push_back(h->normal); opItem = h->itemId; } if (h->normal.dot(rr) < -0.2) { inward++; rIn.push_back(rd); inBox = inBox.united(QRect(x, y, 1, 1)); } else rOut.push_back(rd); }
            const double ang = std::atan2(d.dot(e2), d.dot(e1)); sect[size_t(std::clamp(int((ang + 3.14159265) / 6.2831853 * 36), 0, 35))]++; } }
          std::sort(dz.begin(), dz.end()); int filled = 0; QString bar; for (int k : sect) { filled += k > 0; bar += k > 0 ? "#" : "."; }
          auto pc = [&](double f) { return dz.empty() ? -1.0 : dz[size_t(std::min<double>(dz.size() - 1, f * dz.size()))]; };
          r += QString(" · painted %1 (on the model %2) · off the ring's plane, of the radius: median %3, 99% %4, max %5 · sectors round the axis %6/36 [%7]")
                 .arg(painted).arg(onModel).arg(pc(0.5), 0, 'f', 2).arg(pc(0.99), 0, 'f', 2).arg(dz.empty() ? -1.0 : dz.back(), 0, 'f', 2).arg(filled).arg(bar) + QString(" · facing inward (the inner wall) %1").arg(inward) + [&] { auto med = [](std::vector<double> v) { if (v.empty()) return -1.0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }; return QString(" · distance from the axis, of the radius: facing out %1, facing in %2 · the inward ones in the texture %3,%4 %5x%6").arg(med(rOut), 0, 'f', 2).arg(med(rIn), 0, 'f', 2).arg(inBox.x()).arg(inBox.y()).arg(inBox.width()).arg(inBox.height()); }() + " · sides: " + viewport_->testOpenSides(opItem, opP, opN); } }
      else if (o == "uvat" && w.size() > 2) { QStringList out; for (int i = 2; i < w.size(); i++) { const QStringList p = w[i].split(QChar(0x2c)); const auto uv = viewport_->paintUvAt(QPointF(p[0].toDouble(), p[1].toDouble())); out << (uv ? QString("%1,%2").arg(uv->x(), 0, 'f', 3).arg(uv->y(), 0, 'f', 3) : QString("-")); } r = out.join(" ") + QString(" · pane %1x%2").arg(viewport_->paneW()).arg(viewport_->paneH()); }   // (pv uvat x,y …: the texture point under each window point)
      else if (o == "layer" && w.size() > 2) { paint_->setLayer(w[2].toInt()); r = QString("active %1").arg(paint_->active() ? paint_->active()->name : QString("none")); }
      else if (o == "filled" && w.size() > 2) paint_->shapeOutline = w[2] != "1";
      else if (o == "selkind" && w.size() > 2) paintLeft_->tools()->selKind = w[2];
      else if (o == "sel") r = QString("%1 polygons selected").arg(viewport_->pvSelCount());
      else if (o == "tscopy" && w.size() > 2) r = paintLeft_->tools()->topoCopy(w[2].toInt());
      else if (o == "tspick" && w.size() > 3) { const QStringList p = w[2].split(QChar(0x2c)); r = viewport_->pvTsPick(QPointF(p[0].toDouble(), p[1].toDouble()), w[3] == "blue"); }
      else if (o == "lay") paintLeft_->tools()->layCurve();
      else if (o == "tsstat") r = viewport_->pvTsStat_;
      else if (o == "asktext" && w.size() > 2) { const QString t = w.mid(2).join(' '); paintLeft_->tools()->askText = [t](const QString&) { return t; }; }
      else if (o == "olshot" && w.size() > 2) { rebuildOutliner(); QApplication::processEvents(); r = QString("saved %1").arg(outliner_->grab().save(w[2])); }
      else if (o == "tool" && w.size() > 2) paintLeft_->setTool(w[2]);
      else if (o == "info") { QStringList l; for (int m : viewport_->pvMeshes()) { const Item* it = scene_.item(m); int nb = 0; for (char c : viewport_->pvBlocked(*it)) nb += c;
          l << QString("%1 %2%3%4 blocked %5 groups %6").arg(m).arg(it->name).arg(viewport_->pvShown(m) ? "" : " HID").arg(viewport_->pvLocked(m) ? " LOCK" : "").arg(nb).arg(viewport_->pvGroups(m).size()); }
        QStringList ss; for (const auto& x : viewport_->pvSets()) ss << QString("%1%2%3").arg(x.key).arg(x.shown ? "" : " hid").arg(x.locked ? " lock" : ""); r = l.join(" | ") + " || sets: " + ss.join(", "); }
      viewport_->beforeFrame(); QApplication::processEvents(); log << QString("pv %1: %2").arg(w.mid(1).join(' ')).arg(r); continue; }
    if (w.size() >= 2 && w[0] == "vset") { QString r;
      if (w[1] == "save") r = "set " + viewport_->visSetSave(w.value(2)); else if (w[1] == "show") { viewport_->visSetShow(w.value(2)); r = "shown"; }
      else if (w[1] == "pane") { Viewport::PaneVis v; v.set = w.value(3) == "-" ? QString() : w.value(3); v.sync = w.value(4) == "sync"; viewport_->setPaneVis(w.value(2).toInt(), v); r = QString("pane %1 set '%2' sync %3").arg(w.value(2), v.set).arg(v.sync); }
      else if (w[1] == "list") r = viewport_->visSetNames().join(", ");
      viewport_->beforeFrame(); log << "vset " + w[1] + ": " + r; continue; }
    if (w.size() >= 2 && w[0] == "vwin") { QString r;
      if (w[1] == "open") { openViewWindow(this, viewport_); r = QString("views %1").arg(viewport_->detached()); } else if (w[1] == "close") { closeViewWindow(); r = "closed"; }
      else if (w[1] == "migrate") { viewport_->migrateDetached(); closeViewWindow(); r = QString("split %1 layout %2").arg(viewport_->split()).arg(viewport_->splitLayout()); }
      else if (w[1] == "menu") { toggleViewWindowMenu(mapToGlobal(QPoint(300, 120))); QElapsedTimer t; t.start(); while (t.elapsed() < 300) QApplication::processEvents(); r = viewWindowShot(w.value(2), true) ? "menu shot" : "menu shot FAILED"; toggleViewWindowMenu(QPoint()); }
      else if (w[1] == "shot") { viewport_->requestUpdate(); QElapsedTimer t; t.start(); while (t.elapsed() < 500) QApplication::processEvents(); r = viewWindowShot(w.value(2), false) ? "shot" : "shot FAILED"; }
      else if (w[1] == "mouse" && w.size() >= 4) {               // (vwin mouse press|move|release|rpress|rrelease x,y [alt] [ctrl] [shift])
        const QStringList p = w[3].split(QChar(0x2c)); const QPointF at(p[0].toDouble(), p[1].toDouble()); Qt::KeyboardModifiers md;
        for (int i = 4; i < w.size(); i++) { if (w[i] == "ctrl") md |= Qt::ControlModifier; if (w[i] == "shift") md |= Qt::ShiftModifier; if (w[i] == "alt") md |= Qt::AltModifier; }
        const bool rmb = w[2].startsWith('r') && w[2] != "release"; const Qt::MouseButton b = rmb ? Qt::RightButton : Qt::LeftButton;
        const QEvent::Type ty = w[2].endsWith("press") ? QEvent::MouseButtonPress : w[2] == "move" ? QEvent::MouseMove : QEvent::MouseButtonRelease;
        static Qt::MouseButtons vheld; if (ty == QEvent::MouseButtonPress) vheld = b;
        const bool ok = viewWindowMouse(int(ty), at, int(ty == QEvent::MouseMove ? Qt::NoButton : b), int(ty == QEvent::MouseButtonRelease ? Qt::MouseButtons() : vheld), int(md));
        if (ty == QEvent::MouseButtonRelease) vheld = {};
        viewport_->beforeFrame(); QApplication::processEvents();
        const Camera& c = viewport_->camera(); r = QString("%1 · active %2 cam %3,%4,%5").arg(ok).arg(viewport_->activePane()).arg(c.pos.x, 0, 'f', 2).arg(c.pos.y, 0, 'f', 2).arg(c.pos.z, 0, 'f', 2); }
      else if (w[1] == "info") { QStringList l; for (int i : viewport_->panesInUse()) { const QRect pr = viewport_->paneRect(i); l << QString("%1%2 %3 %4x%5 set '%6'%7").arg(i).arg(viewport_->paneDetached(i) ? "w" : "").arg(viewport_->paneCamera(i).view).arg(pr.width()).arg(pr.height()).arg(viewport_->paneVis(i).set).arg(viewport_->paneVis(i).sync ? " sync" : "") + QString(" cam %1,%2,%3").arg(viewport_->paneCamera(i).pos.x, 0, 'f', 2).arg(viewport_->paneCamera(i).pos.y, 0, 'f', 2).arg(viewport_->paneCamera(i).pos.z, 0, 'f', 2); } r = l.join(" | "); }
      log << "vwin " + w[1] + ": " + r; continue; }
    if (w.size() == 2 && w[0] == "camshot") { openCameraMenu(this, viewport_, mapToGlobal(QPoint(300, 60))); QElapsedTimer t; t.start(); while (t.elapsed() < 300) QApplication::processEvents(); log << QString("camshot %1").arg(cameraMenuShot(w[1])); closeCameraMenu(); continue; }
    if (w.size() == 2 && w[0] == "holeloops") { log << "holeloops: " + viewport_->testHoleLoops(w[1].toInt()); continue; }
    if (w.size() == 2 && w[0] == "mkmanifold") { Item* it = scene_.item(w[1].toInt()); if (it && it->poly) { auto p = std::make_shared<PolyMesh>(*(it->base ? it->base : it->poly)); const int n = tedit::makeManifold(*p); it->base = p; it->poly = p; scene_.revision++; log << QString("mkmanifold %1 changes").arg(n); } continue; }   // mkmanifold <mesh>: the generated meshes' pass, on any mesh (checks)
    if (w.size() == 1 && w[0] == "nmall") { for (auto& [id, it] : scene_.items) if (it.type == ItemType::IMesh && it.poly) log << "nm " + viewport_->testNonManifold(id); continue; }   // nmall: every mesh's non-manifold count
    if (w.size() == 2 && w[0] == "selt") { viewport_->setSelTool(w[1]); log << "selt " + w[1]; continue; }   // selt <kind>: the Select Cursor's kind
    if (w.size() == 2 && w[0] == "hardspot") { log << "hardspot " + viewport_->testHardSpot(w[1].toInt()); continue; }
    if (w.size() == 3 && w[0] == "loopcut") { log << "loopcut: " + viewport_->testLoop(w[1].toInt(), w[2].toDouble()); continue; }   // loopcut <edge> <t>: a Loop cut through that edge of the active mesh
    if (w.size() == 2 && w[0] == "dblinfo") { log << "dblinfo: " + viewport_->testDoubles(w[1].toInt()); continue; }   // dblinfo <mesh>: its double walls
    if (w.size() == 2 && w[0] == "pxparts") { log << "pxparts: " + viewport_->testProxyParts(w[1].toInt()); continue; }   // pxparts <mesh>: its small parts kept by the mask proxy
    if (w.size() == 3 && w[0] == "repl") { log << "repl: " + viewport_->testReplace(w[1].toDouble(), w[2].toDouble()); continue; }   // repl <x> <r>: Mirror → Replace of the polygons round there
    if (w.size() == 3 && w[0] == "pensym") { log << "pensym: " + viewport_->testPenSym(w[1], w[2].toInt(), 0); continue; }   // pensym f|e|v|loop <face>: PolyPen with Symmetry on that face
    if (w.size() == 1 && w[0] == "mirhalf") { log << "mirhalf: " + viewport_->testMirrorHalf(); continue; }   // mirhalf: the active mesh halved and mirrored back
    if (w.size() == 2 && w[0] == "mirn") { log << "mirn: " + viewport_->testMirrorNormals(w[1].toInt()); continue; }   // mirn <mesh>: its corner normals against their mirror
    if (w.size() == 3 && w[0] == "nearv") { log << "nearv: " + viewport_->testNearVerts(w[1].toInt(), w[2].toDouble()); continue; }   // nearv <mesh> <px>: unwelded vertices close on screen
    if (w.size() == 2 && w[0] == "holes") {   // holes <mesh>: its open outlines (sizes), its hidden polygons — the PolyPen fill's checks
      const Item* it = scene_.item(w[1].toInt()); if (!it) { log << "holes: no mesh"; continue; }
      auto T = topo::ttopo(it->base ? it->base : it->poly); std::map<int, int> deg; for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] < 0) { deg[T->ea[size_t(e)]]++; deg[T->eb[size_t(e)]]++; }
      int rimE = 0; for (int e = 0; e < T->ne; e++) rimE += T->ef1[size_t(e)] < 0;
      const auto hid = meshmods::hiddenFaces(*it, *(it->base ? it->base : it->poly)); int nh = 0; for (char c : hid) nh += c;
      { const auto W = it->poly->pos; std::vector<std::vector<int>> nb(size_t(T->n)); for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] < 0) { nb[size_t(T->ea[size_t(e)])].push_back(T->eb[size_t(e)]); nb[size_t(T->eb[size_t(e)])].push_back(T->ea[size_t(e)]); }
        std::set<std::pair<int, int>> used;
        for (int v0 = 0; v0 < T->n; v0++) for (int first : nb[size_t(v0)]) { if (used.count({std::min(v0, first), std::max(v0, first)})) continue;
          std::vector<int> L{v0}; int prev = v0, cur = first; used.insert({std::min(v0, first), std::max(v0, first)});
          for (int g = 0; cur != v0 && g < T->n + 2; g++) { L.push_back(cur); int nx = -1; for (int x : nb[size_t(cur)]) if (x != prev && !used.count({std::min(cur, x), std::max(cur, x)})) { nx = x; break; } if (nx < 0) break; used.insert({std::min(cur, nx), std::max(cur, nx)}); prev = cur; cur = nx; }
          Vec3 c, n; for (size_t i = 0; i < L.size(); i++) { const Vec3& a = W[size_t(L[i])]; const Vec3& b = W[size_t(L[(i + 1) % L.size()])]; c += a; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); }
          c = c * (1.0 / double(L.size())); n = n.normalized();
          log << QString("  loop %1 verts, closed %2, centre %3 %4 %5, normal %6 %7 %8").arg(L.size()).arg(cur == v0).arg(c.x).arg(c.y).arg(c.z).arg(n.x).arg(n.y).arg(n.z); } }
      log << QString("holes %1: rim edges %2, rim vertices %3, hidden polygons %4, hidGroups %5").arg(it->name).arg(rimE).arg(deg.size()).arg(nh).arg(QString::fromUtf8(QJsonDocument(it->raw.value("hidGroups").toArray()).toJson(QJsonDocument::Compact))); continue; }
    if (w.size() == 1 && w[0] == "grpat") { auto c = viewport_->selectionCentre(); viewport_->createEmptyGroup(c ? &*c : nullptr); const Item* g = viewport_->selection().items.empty() ? nullptr : scene_.item(viewport_->selection().items.back());
      log << QString("grpat: %1 (%4) in %3 %2").arg(g ? g->name : "-").arg(g ? QString::fromUtf8(QJsonDocument(g->raw.value("gpivot").toArray()).toJson(QJsonDocument::Compact)) : "-").arg(g && scene_.item(g->parent) ? scene_.item(g->parent)->name : QString("root")).arg(g ? g->id : -1); continue; }
    if (w.size() >= 2 && (w[0] == "fillnear" || w[0] == "fillnear0" || w[0] == "fillnearsel")) { for (const QString& s : w[1].split(QChar(0x2c))) for (int sd : {1, -1}) log << "fillnear " + viewport_->fillTestNear(s.toInt(), sd, w[0] == "fillnear" ? 1 : w[0] == "fillnearsel" ? 2 : 0); continue; }   // (fillnearsel: the selection as it is)
    if (w.size() == 1 && w[0] == "segcurves") { QStringList q; for (auto& [id, it] : scene_.items) if (it.type == ItemType::Curve && it.seg && scene_.isVisible(it)) q << QString::number(id); log << "segcurves " + q.join(QChar(0x2c)); continue; }
    if ((w.size() == 3 || w.size() == 4) && w[0] == "fillat") { const bool ok = viewport_->startFillAt(viewport_->mapToGlobal(QPoint(w[1].toInt(), w[2].toInt()))); log << QString("fillat %1 %2: %3 '%4'").arg(w[1], w[2]).arg(ok).arg(viewport_->fillTitle); if (ui::Pie* pp = ui::Pie::current(); pp && w.value(3) != "keep") pp->close(); continue; }   // (fillat x y [keep]: keep — its pie left open, the outline in the picture)
    if (w.size() == 2 && w[0] == "select") { std::vector<int> ids; for (const QString& s : w[1].split(',')) ids.push_back(s.toInt()); viewport_->setSelectedItems(ids); log << "select " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "tool") { pickTool(w[1]); log << "tool " + viewport_->tool(); continue; }
    if (w.size() == 6 && w[0] == "gpoint") { viewport_->setGroupPoint(w[1].toInt(), Vec3(w[2].toDouble(), w[3].toDouble(), w[4].toDouble()), w[5] == "1"); viewport_->beforeFrame(); log << "gpoint"; continue; }   // gpoint <group> x y z <carry 0|1>
    if (w.size() == 3 && w[0] == "gcolor") { const int g = w[1].toInt(); const QString c = w[2] == "none" ? QString() : w[2]; viewport_->runEdit([g, c](Scene& s, Selection&, const CurveCache&) { if (Item* x = s.item(g)) x->color = c; s.revision++; return QString(); }, false); viewport_->sceneChanged(false); log << "gcolor"; continue; }   // gcolor <group> <#hex|none>
    if (w.size() == 1 && w[0] == "histlist") { for (int i = 0; i < viewport_->historySize(); i++) log << QString("hist %1%2 %3").arg(i).arg(i == viewport_->historyIndex() ? "*" : "").arg(viewport_->historyLabel(i)); continue; }   // histlist: the history steps (* — the current one)
    if (w.size() == 2 && w[0] == "histdel") { log << QString("histdel %1: %2").arg(w[1]).arg(viewport_->historyDelete(w[1].toInt())); viewport_->beforeFrame(); continue; }
    if (w.size() == 2 && w[0] == "histgo") { viewport_->historyGoTo(w[1].toInt()); viewport_->beforeFrame(); log << "histgo " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "rawdump") { const Item* x = scene_.item(w[1].toInt()); log << "rawdump " + (x ? QString::fromUtf8(QJsonDocument(x->raw).toJson(QJsonDocument::Compact)).left(600) : QString("-")); continue; }
    if (w.size() == 1 && w[0] == "collinfo") { std::map<int, double> off; const auto L = viewport_->collisionMeshes(true, off); for (auto& pm : L) log << QString("coll %1 tris %2 %3").arg(pm.itemId).arg(pm.trisRef ? pm.trisRef->size() / 3 : pm.tris.size() / 3).arg(off[pm.itemId]); continue; }
    // Head Collision: coll — the group and its objects; colladd <shape>; collfrom <mesh>; collshape <id> <shape>; collflag off|lock 0|1
    if (w.size() == 1 && w[0] == "coll") { const int g = coll::group(scene_); const Item* G = scene_.item(g);
      log << QString("coll group %1 \"%2\" root-first %3 visible %4 off %5 lock %6 active %7").arg(g).arg(G ? G->name : "-").arg(!scene_.roots.empty() && scene_.roots.front() == g).arg(G && G->visible).arg(G && G->raw.value("collOff").toBool()).arg(coll::locked(scene_)).arg(coll::active(scene_));
      if (G) for (int c : G->children) if (const Item* x = scene_.item(c)) { Box3 b = x->poly ? x->poly->bounds() : Box3();
        log << QString("  obj %1 \"%2\" %3 head %4 prim %5 src %6 faces %7 box %8,%9,%10 .. %11,%12,%13").arg(c).arg(x->name).arg(itemTypeKey(x->type)).arg(x->headTag).arg(x->raw.value("prim").toObject().value("type").toString("-")).arg(x->raw.value("collSrc").toInt(-1))
               .arg(x->poly ? x->poly->faceCount() : 0).arg(b.min.x, 0, 'f', 2).arg(b.min.y, 0, 'f', 2).arg(b.min.z, 0, 'f', 2).arg(b.max.x, 0, 'f', 2).arg(b.max.y, 0, 'f', 2).arg(b.max.z, 0, 'f', 2); }
      int old = 0; for (auto& [id, it] : scene_.items) if (it.raw.value("headTag").toBool() || it.raw.value("collide").toBool() || it.raw.contains("collCap")) old++;
      QStringList sl; for (int s : viewport_->selection().items) sl << QString::number(s);
      log << QString("coll old marks left %1, room %2, selected [%3]").arg(old).arg(scene_.room).arg(sl.join(",")); continue; }
    if (w.size() == 2 && w[0] == "answer") { const bool yes = w[1] == "yes";   // answer yes|no: the next question box answered (a moment after it opens)
      auto* t = new QTimer(this); t->setInterval(100); auto left = std::make_shared<int>(50);
      connect(t, &QTimer::timeout, this, [t, yes, left] { if (auto* mb = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { if (auto* b = mb->button(yes ? QMessageBox::Yes : QMessageBox::No)) b->click(); t->deleteLater(); return; } if (--*left <= 0) t->deleteLater(); });
      t->start(); log << "answer " + w[1]; continue; }
    if (w.size() >= 2 && w[0] == "drop") { QStringList fs = w.mid(1).join(' ').split('|'); const int n0 = int(scene_.items.size()); importDropped(fs, QPoint(-1, -1), false); viewport_->beforeFrame();   // drop <file>|<file>…: as if dropped on the window (models straight in, no window)
      log << QString("drop %1: items %2 -> %3, room %4").arg(fs.join(", ")).arg(n0).arg(scene_.items.size()).arg(scene_.room); continue; }
    if (w.size() >= 3 && w[0] == "dropwin") {                   // dropwin <png> <file>|<file>…: dropped as by the mouse — the window that opens shot, then closed
      const QStringList fs = w.mid(2).join(QChar(32)).split(QChar(124)); auto shot = std::make_shared<QString>();
      QTimer::singleShot(1200, this, [png = w[1], shot] { if (auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget())) { *shot = d->windowTitle(); d->grab().save(png); d->reject(); } });
      importDropped(fs); log << QString("dropwin: %1").arg(shot->isEmpty() ? QString("no window opened") : "window '" + *shot + "' shot to " + w[1]); continue; }
    if (w.size() >= 4 && w[0] == "dropat") {                    // dropat <x> <y> <file>|<file>…: dropped at that point of the viewport; the models' maps logged
      const QStringList fs = w.mid(3).join(QChar(32)).split(QChar(124)); importDropped(fs, viewport_->mapToGlobal(QPoint(w[1].toInt(), w[2].toInt())), false); viewport_->beforeFrame();
      const auto pk = viewport_->pickMeshFace(QPointF(w[1].toDouble(), w[2].toDouble())); QString m = QString("dropat %1 %2: on %3 face %4").arg(w[1], w[2]).arg(pk.first).arg(pk.second);
      for (auto& [id, it] : scene_.items) if (it.type == ItemType::IMesh && it.raw.value("look").isObject()) { const QJsonObject L = it.raw.value("look").toObject(); m += QString(" | %1 tex=%2").arg(it.name).arg(L.value("tex").toBool());
        for (const QJsonValue& sv : L.value("sets").toArray()) { const QJsonObject so = sv.toObject(), mp = so.value("maps").toObject(); QStringList ks; for (const QString& k : mp.keys()) ks << k + "=" + mp.value(k).toObject().value("name").toString(); m += QString(" [%1: %2]").arg(so.value("name").toString(), ks.join(",")); } }
      log << m; continue; }
    if (w.size() == 2 && w[0] == "shapetiles") { shapeTilesShot(viewport_, &scene_, w[1]); log << "shapetiles " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "colladd") { const int id = viewport_->collAdd(w[1]); viewport_->beforeFrame(); log << QString("colladd %1 -> %2").arg(w[1]).arg(id); continue; }
    if (w.size() == 2 && w[0] == "collfrom") { const int id = viewport_->collCreateFrom(w[1].toInt()); viewport_->beforeFrame(); log << QString("collfrom %1 -> %2").arg(w[1]).arg(id); continue; }
    if (w.size() == 3 && w[0] == "collshape") { viewport_->collSetShape(w[1].toInt(), w[2]); viewport_->beforeFrame(); log << "collshape " + w[1] + " " + w[2]; continue; }
    if (w.size() == 3 && w[0] == "collflag") { const int g = coll::group(scene_); const bool v = w[2] == "1";
      if (g >= 0) viewport_->runEdit([g, k = w[1], v](Scene& s, Selection&, const CurveCache&) { s.item(g)->raw.insert(k == "off" ? "collOff" : "selLock", v); return QString(); }, false);
      viewport_->beforeFrame(); log << "collflag " + w[1] + " " + w[2]; continue; }
    if (w.size() == 2 && w[0] == "headtag") { if (Item* it = scene_.item(w[1].toInt())) { it->headTag = true; it->raw.insert("headTag", true); viewport_->sceneChanged(true); } log << "headtag " + w[1]; continue; }
    if (w.size() >= 2 && w[0] == "capmake") { const int o = w[1].toInt();   // capmake <mesh> [dx dy dz]: its collision a capsule — a mesh of its own; moved by d; logs its box in the world
      if (Item* it = scene_.item(o)) { QJsonObject c = it->raw.value("coll").toObject(); c.insert("type", "capsule"); it->raw.insert("coll", c); }
      int cap = viewport_->capsuleItem(o); if (cap < 0) cap = viewport_->makeCapsuleItem(o, true);
      if (cap >= 0 && w.size() == 5) { Affine T; T.t = Vec3(w[2].toDouble(), w[3].toDouble(), w[4].toDouble()); meshmods::setMeshMatrix(scene_, cap, Affine::mul(T, meshmods::meshMatrix(scene_, *scene_.item(cap)))); viewport_->sceneChanged(true); }
      viewport_->beforeFrame(); Box3 b; for (auto& q : viewport_->capsuleWorld(cap)) b.expand(q);
      log << QString("capmake %1: capsule %2, box %3,%4,%5 .. %6,%7,%8").arg(o).arg(cap).arg(b.min.x, 0, 'f', 1).arg(b.min.y, 0, 'f', 1).arg(b.min.z, 0, 'f', 1).arg(b.max.x, 0, 'f', 1).arg(b.max.y, 0, 'f', 1).arg(b.max.z, 0, 'f', 1); continue; }
    if (w.size() == 2 && w[0] == "capinfo") { const auto& C = viewport_->capsuleOf(w[1].toInt()); log << QString("capinfo %1 pts, box %2,%3,%4 .. %5,%6,%7").arg(C.pos.size()).arg(C.bx.min.x).arg(C.bx.min.y).arg(C.bx.min.z).arg(C.bx.max.x).arg(C.bx.max.y).arg(C.bx.max.z); continue; }
    if (w.size() == 1 && w[0] == "tree") { for (auto& [id, it] : scene_.items) { const QJsonArray gp = it.raw.value("gpivot").toArray(); QString ex = it.type == ItemType::Surface ? QString(" a%1 b%2").arg(it.a).arg(it.b) : it.junc ? QString(" junc") : QString();
        if (it.type == ItemType::Curve && !it.anchors.empty()) { const Vec3 p = scene_.anchor(it.anchors[0])->pos; ex += QString(" a0 %1,%2,%3").arg(p.x, 0, 'f', 3).arg(p.y, 0, 'f', 3).arg(p.z, 0, 'f', 3); }
        log << QString("item %1 %2 \"%3\" in %4 %5%6%7").arg(id).arg(it.typeStr, it.name).arg(it.parent).arg(it.color + " tint" + scene_.tintOf(it)).arg(gp.size() == 3 ? QString(" gp %1,%2,%3").arg(gp[0].toDouble()).arg(gp[1].toDouble()).arg(gp[2].toDouble()) : QString()).arg(ex); } continue; }   // tree: every item, its parent, colour, point
    if (w.size() == 2 && w[0] == "action") { runAction(w[1]); log << "action " + w[1]; continue; }
    if (w.size() == 3 && w[0] == "headmark") { if (Item* it = scene_.item(w[1].toInt())) { it->raw.insert("headMark", w[2] == "1"); scene_.revision++; viewport_->sceneChanged(false); } log << "headmark " + w[1] + " " + w[2]; continue; }   // (checks: the outliner's HEAD tag)
    if (w.size() == 1 && w[0] == "visstate") {   // (checks: what H and its kin left shown)
      int shown = 0, all = 0, hidF = 0, pHid = 0; for (auto& [id, it] : scene_.items) { if (it.type == ItemType::Group) continue; all++; shown += scene_.isVisible(it); hidF += !it.raw.value("hidF").toString().isEmpty(); pHid += it.raw.contains("pHid") || it.raw.contains("pHidF"); }
      log << QString("visstate %1: shown %2 of %3, isolation %4 (keeps %5), meshes with hidden polygons %6, paint-hidden %7").arg(scene_.room).arg(shown).arg(all).arg(scene_.iso ? "on" : "off").arg(scene_.iso ? int(scene_.iso->keep.size()) : 0).arg(hidF).arg(pHid); continue; }
    if (w.size() == 2 && w[0] == "save") { log << (saveProjectTo(w[1]) ? "saved " : "save FAILED ") + w[1]; continue; }
    if (w.size() == 2 && w[0] == "savebg") {                    // savebg <file>: the project written, the paint layers in the background — how long the UI was held and how long they took
      QElapsedTimer t; t.start(); const bool ok = saveProjectTo(w[1]); const qint64 held0 = t.elapsed(); bool done = false, all = false; t.restart();
      if (paint_) paint_->saveFilesAsync(w[1], true, [&](bool a) { done = true; all = a; }); const qint64 held = t.elapsed();
      while (paint_ && !done && t.elapsed() < 120000) QApplication::processEvents(QEventLoop::AllEvents, 20);
      log << QString("savebg %1: project %2 in %3 ms · layers: UI held %4 ms, written in %5 ms, all %6 · dirty %7").arg(QFileInfo(w[1]).fileName()).arg(ok).arg(held0).arg(held).arg(t.elapsed()).arg(all).arg(dirty()); continue; }
    if (w.size() == 1 && w[0] == "dirty") { log << QString("dirty %1 (changes %2, paint edits %3)").arg(dirty()).arg(changeTotal()).arg(paint_ ? paint_->edits : -1); continue; }
    if (w.size() == 1 && w[0] == "waitlod") { QElapsedTimer t; t.start(); viewport_->beforeFrame(); while (viewport_->heavyPending() && t.elapsed() < 300000) { QApplication::processEvents(QEventLoop::AllEvents, 50); viewport_->requestUpdate(); }
      log << QString("waitlod: %1 ms, %2 pending").arg(t.elapsed()).arg(viewport_->heavyPending()); continue; }
    if (w.size() == 2 && w[0] == "lodview") { viewport_->spinNav_ = w[1] == "1"; viewport_->requestUpdate(); log << "lodview " + w[1]; continue; }
    if (w.size() == 3 && w[0] == "tdrag") { log << "tdrag: " + viewport_->testDrag(w[1].toInt(), w[2].toDouble()); continue; }
    if (w.size() == 4 && w[0] == "wheel") {           // wheel x,y notches count — fast scrolling, each event drawn: ms per event
      auto p = pointOf(w[1]); const int n = w[3].toInt(); QElapsedTimer t; t.start(); double worst = 0;
      for (int i = 0; i < n && p; i++) { QElapsedTimer f; f.start(); QWheelEvent ev(*p, viewport_->mapToGlobal(*p), QPoint(), QPoint(0, int(w[2].toDouble() * 120)), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(viewport_, &ev); const int f0 = viewport_->framesDrawn; viewport_->requestUpdate();
        QElapsedTimer to; to.start(); while (viewport_->framesDrawn == f0 && to.elapsed() < 10000) QApplication::processEvents(QEventLoop::AllEvents, 5); worst = std::max(worst, f.nsecsElapsed() / 1e6); }
      log << QString("wheel: %1 ms an event (worst %2), LOD frames %3").arg(t.elapsed() / double(std::max(1, n)), 0, 'f', 1).arg(worst, 0, 'f', 1).arg(viewport_->lodFrames); continue; }
    if (w.size() == 2 && w[0] == "thinkshot") { bool ok = ui::thinkSample(tr("Laying the Topology Ghost: its points onto the surface — the snap mask first"), 0.52, QSize(700, 400)).save(w[1]); log << QString("thinkshot %1").arg(ok); continue; }
    if ((w.size() == 5 || w.size() == 6) && w[0] == "fillbench") { const bool raw = w.size() == 6; viewport_->tset.fillMode = w[2]; const int m = w[1].toInt(), f0 = w[3].toInt(), n = w[4].toInt(); QElapsedTimer t; t.start(); size_t tot = 0;   // (checks: Fill's region asked for n polygons in a row — as the pointer moving over them)
      for (int i = 0; i < n; i++) tot += (raw ? viewport_->fillRegionCalc(m, f0 + i, false) : viewport_->fillRegion(m, f0 + i)).size(); log << QString("fillbench %1 %2: %3 ms for %4 (%5 polygons in all)").arg(m).arg(w[2]).arg(t.elapsed()).arg(n).arg(tot); continue; }
    if ((w.size() == 2 || w.size() == 3) && w[0] == "framems") { const int n = std::clamp(w[1].toInt(), 1, 200); const bool orb = w.size() == 3; QElapsedTimer t; t.start();
      for (int i = 0; i < n; i++) { if (orb) { Camera& c = viewport_->camera(); const double a = 0.02, dx = c.pos.x - c.pivot.x, dz = c.pos.z - c.pivot.z; c.pos.x = c.pivot.x + dx * std::cos(a) - dz * std::sin(a); c.pos.z = c.pivot.z + dx * std::sin(a) + dz * std::cos(a); viewport_->requestUpdate(); }   // (orbit: the camera turned a little every frame)
        viewport_->beforeFrame(); (void)viewport_->grab(); }   // (checks: n frames drawn and read back — the mean ms a frame)
      log << QString("framems %1: %2 ms a frame").arg(n).arg(double(t.elapsed()) / n, 0, 'f', 1); continue; }
    if (w.size() == 1 && w[0] == "time") { static QElapsedTimer tt; log << QString("time %1 ms").arg(tt.isValid() ? tt.restart() : (tt.start(), 0)); continue; }
    if (w.size() == 2 && w[0] == "genmesh") {         // genmesh Q — a torus of about Q quads (the stress checks), selected
      const qint64 Q = w[1].toLongLong(); const int nu = std::max(8, int(std::sqrt(double(Q) * 3))), nv = std::max(4, int(Q / nu));
      PolyMesh m; m.pos.reserve(size_t(nu) * size_t(nv)); m.faceVerts.reserve(size_t(nu) * size_t(nv) * 4); m.faceStart.reserve(size_t(nu) * size_t(nv) + 1);
      const double R = 100, r = 35;
      for (int i = 0; i < nu; i++) for (int j = 0; j < nv; j++) { double a = 2 * M_PI * i / nu, b = 2 * M_PI * j / nv; m.pos.push_back({(R + r * std::cos(b)) * std::cos(a) + 600, r * std::sin(b) + 3000, (R + r * std::cos(b)) * std::sin(a)}); }
      for (int i = 0; i < nu; i++) for (int j = 0; j < nv; j++) { int q[4] = {i * nv + j, ((i + 1) % nu) * nv + j, ((i + 1) % nu) * nv + (j + 1) % nv, i * nv + (j + 1) % nv}; m.addFace(q, 4); }
      for (int i = 0; i < nu; i++) for (int j = 0; j < nv; j++) for (int k = 0; k < 4; k++) { (void)k; }
      m.faceGroup.assign(size_t(m.faceCount()), 0); m.groupNames = {"Torus"};
      int id = -1; viewport_->runEdit([&](Scene&, Selection&, const CurveCache&) { id = viewport_->topoNewMesh(scene_.item(4) ? scene_.item(4)->parent : -1, QString("Torus %1").arg(m.faceCount()), m); return QString(); }, true);
      viewport_->setSelectedItems({id}); viewport_->sceneChanged(false); viewport_->beforeFrame();
      log << QString("genmesh: id %1 · %2 quads · %3 vertices").arg(id).arg(m.faceCount()).arg(m.pos.size()); continue; }
    if (w.size() >= 2 && w[0] == "spin") {            // spin N [pan|dolly] — the camera moved N frames, each drawn; the average frame
      const int n = w[1].toInt(); const QString how = w.value(2, "orbit"); QElapsedTimer t; t.start(); double worst = 0; QElapsedTimer f; const int lod0 = viewport_->lodFrames;
      for (int i = 0; i < n; i++) { f.start(); Camera& c = viewport_->camera(); if (how == "pan") c.pan(3, 1); else if (how == "dolly") c.dolly(i % 2 ? 2 : -2); else c.orbit(6, 0);
        viewport_->spinFrame(); int f0 = viewport_->framesDrawn; viewport_->requestUpdate();
        QElapsedTimer to; to.start(); while (viewport_->framesDrawn == f0 && to.elapsed() < 5000) QApplication::processEvents(QEventLoop::AllEvents, 5);
        worst = std::max(worst, f.nsecsElapsed() / 1e6); }
      viewport_->spinFrame(true);
      log << QString("spin %1 %2: %3 ms a frame (worst %4 ms), LOD in %5").arg(n).arg(how).arg(t.elapsed() / double(std::max(1, n)), 0, 'f', 1).arg(worst, 0, 'f', 1).arg(viewport_->lodFrames - lod0); continue; }
    if (w.size() == 2 && w[0] == "tselall") { int m = viewport_->topoMesh(); const Item* it = m >= 0 ? scene_.item(m) : nullptr; const PolyMesh* B = it ? (it->base ? it->base.get() : it->poly.get()) : nullptr;
      if (B) { const char k = w[1][0].toLatin1(); auto T = topo::ttopo(it->base ? it->base : it->poly); int n = k == 'f' ? T->nf : k == 'e' ? T->ne : T->n; std::vector<int> l(static_cast<size_t>(n)); std::iota(l.begin(), l.end(), 0);
        viewport_->tselApply(m, l, "set", k); viewport_->beforeFrame(); log << QString("tselall %1: %2").arg(w[1]).arg(n); } continue; }
    if (w.size() == 2 && w[0] == "pick") { auto p = pointOf(w[1]); auto h = p ? viewport_->pickPoint(*p) : std::nullopt;
      log << (h ? QString("pick %1: %2 %3 %4").arg(w[1]).arg(h->x, 0, 'f', 3).arg(h->y, 0, 'f', 3).arg(h->z, 0, 'f', 3) : "pick " + w[1] + QString(": nothing (cam %1x%2 pane %3 dpr %4)").arg(viewport_->camera().width).arg(viewport_->camera().height).arg(viewport_->activePane()).arg(viewport_->devicePixelRatio())); continue; }
    if (w.size() == 2 && w[0] == "uvwin") { QApplication::processEvents(); bool ok = uvtools_ && uvtools_->isVisible() && uvtools_->grab().save(w[1]); log << (ok ? "uv window shot " : "uv window shot FAILED ") + w[1]; continue; }
    if (w.size() == 1 && w[0] == "captest") {   /* (End's capture: Ctrl + J pressed and let go — a key on a test button; then Esc and End refused) */      auto* btn = new QPushButton("CapTestBtn", this); btn->setToolTip("tip"); btn->show(); int clicks = 0; connect(btn, &QPushButton::clicked, this, [&clicks] { clicks++; });      ui::KeyCapture::start(this, "CapTestBtn", [](const QString& c) { ui::Keymap::get().bindButton("CapTestBtn", "CapTestBtn", "tip", c); });      auto send = [this](QEvent::Type t, int key, Qt::KeyboardModifiers m) { QKeyEvent ev(t, key, m); QApplication::sendEvent(this, &ev); };      send(QEvent::KeyPress, Qt::Key_End, {}); const bool stillOn = ui::KeyCapture::active();      send(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier); send(QEvent::KeyPress, Qt::Key_J, Qt::ControlModifier); send(QEvent::KeyRelease, Qt::Key_J, Qt::ControlModifier); send(QEvent::KeyRelease, Qt::Key_Control, {});      QString id; for (auto& b : ui::Keymap::get().custom()) if (b.text == "CapTestBtn") id = b.id;      log << QString("captest: End refused, still capturing %1 · capture over %2 · bound %3 → %4").arg(stillOn).arg(!ui::KeyCapture::active()).arg(id).arg(ui::Keymap::get().key(id));      send(QEvent::KeyPress, Qt::Key_J, Qt::ControlModifier); QApplication::processEvents(); log << QString("captest: Ctrl+J clicks the button: %1").arg(clicks);      ui::KeyCapture::start(this, "x", [](const QString&) {}); send(QEvent::KeyPress, Qt::Key_Escape, {}); log << QString("captest: Esc cancels: %1").arg(!ui::KeyCapture::active());      ui::Keymap::get().removeCustom(id); delete btn; continue; }
    if (w.size() == 1 && w[0] == "tgeyekeep") {   // (the polygroup list scrolled down, an eye clicked on the row at its top: the scroll before / after the list is made anew)
      auto find = [] { QTreeWidget* t = nullptr; for (QWidget* x : QApplication::allWidgets()) if (x->objectName() == "tgTree" && x->isVisible()) t = qobject_cast<QTreeWidget*>(x); return t; };
      QTreeWidget* tw = find(); if (!tw) { log << "tgeyekeep: no list"; continue; }
      tw->verticalScrollBar()->setValue(tw->verticalScrollBar()->maximum()); for (int i = 0; i < 4; i++) QApplication::processEvents();
      const int before = tw->verticalScrollBar()->value(), mx = tw->verticalScrollBar()->maximum(); QTreeWidgetItem* it = tw->itemAt(5, 5); if (!it) { log << "tgeyekeep: no row"; continue; }
      QPoint p(tw->columnViewportPosition(2) + tw->columnWidth(2) / 2, tw->visualItemRect(it).center().y());
      QMouseEvent pr(QEvent::MouseButtonPress, p, tw->viewport()->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton, {}); QApplication::sendEvent(tw->viewport(), &pr);
      QMouseEvent rl(QEvent::MouseButtonRelease, p, tw->viewport()->mapToGlobal(p), Qt::LeftButton, {}, {}); QApplication::sendEvent(tw->viewport(), &rl);
      QElapsedTimer t; t.start(); while (t.elapsed() < 400) QApplication::processEvents();
      QTreeWidget* tw2 = find(); log << QString("tgeyekeep: scroll %1 (max %2) → %3").arg(before).arg(mx).arg(tw2 ? tw2->verticalScrollBar()->value() : -1); continue; }
if (w.size() == 1 && w[0] == "tgeye") {   /* (a click on the first polygroup row's eye: hidden, the row not picked) */      QTreeWidget* tw = nullptr; for (QWidget* x : QApplication::allWidgets()) if (x->objectName() == "tgTree" && x->isVisible()) tw = qobject_cast<QTreeWidget*>(x);      if (!tw || !tw->topLevelItemCount()) { log << "tgeye: no list"; continue; }      QTreeWidgetItem* it = tw->topLevelItem(0); QRect r = tw->visualItemRect(it); QPoint p(tw->columnViewportPosition(2) + tw->columnWidth(2) / 2, r.center().y());      QMouseEvent pr(QEvent::MouseButtonPress, p, tw->viewport()->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton, {}); QApplication::sendEvent(tw->viewport(), &pr);      QMouseEvent rl(QEvent::MouseButtonRelease, p, tw->viewport()->mapToGlobal(p), Qt::LeftButton, {}, {}); QApplication::sendEvent(tw->viewport(), &rl);      for (int i = 0; i < 4; i++) QApplication::processEvents(); const Item* m = scene_.item(viewport_->topoMesh());      log << QString("tgeye: rows picked %1 · hidden groups %2 · tgSel %3").arg(tw->selectedItems().size()).arg(m ? QString::fromUtf8(QJsonDocument(m->raw.value("hidGroups").toArray()).toJson(QJsonDocument::Compact)) : QString()).arg(viewport_->tgSel.gi); continue; }
    if (w.size() >= 3 && w[0] == "keysdlg") {   // keysdlg <row> <png> [search…] — the hotkey manager on that section, its picture
      ui::HotkeyDialog d(this); d.show(); if (auto* l = d.findChild<QListWidget*>()) l->setCurrentRow(w[1].toInt()); if (w.size() > 3) if (auto* s = d.findChild<QLineEdit*>()) s->setText(w.mid(3).join(" "));
      for (int i = 0; i < 5; i++) QApplication::processEvents(); log << QString("keysdlg %1: %2").arg(w[1]).arg(d.grab().save(w[2])); continue; }
    if (w.size() == 2 && w[0] == "tlocal") { viewport_->tlocal = w[1].toInt() != 0; auto tg = viewport_->ttTarget(); auto vals = viewport_->ttValues(tg); QStringList o;
      for (auto& r : vals) o << (r ? QString("%1 %2 %3").arg((*r)[0], 0, 'f', 3).arg((*r)[1], 0, 'f', 3).arg((*r)[2], 0, 'f', 3) : QString("-")); log << "tlocal " + w[1] + ": " + o.join(" | "); continue; }
    if (w.size() >= 4 && w[0] == "rawset") {   // rawset <id> <key> <json…> — an item's raw value set (testing), the scene remade
      Item* it = scene_.item(w[1].toInt()); const QJsonDocument d = QJsonDocument::fromJson(("[" + w.mid(3).join(" ") + "]").toUtf8());
      if (it && d.isArray() && !d.array().isEmpty()) { it->raw.insert(w[2], d.array()[0]); scene_.revision++; viewport_->sceneChanged(true); viewport_->beforeFrame(); }
      log << "rawset " + w.mid(1).join(" ") + (it && d.isArray() ? "" : " FAILED"); continue; }
    if (w.size() == 1 && w[0] == "polybox") {   // polybox — each imported mesh's shown box (its virtual symmetry's copies with it)
      viewport_->beforeFrame(); QStringList o; for (auto& [id, it] : scene_.items) if (it.type == ItemType::IMesh && it.poly) { const Box3 b = it.poly->bounds();
        o << QString("%1[%2 v, parent %3]: x %4…%5 y %6…%7 z %8…%9").arg(id).arg(it.poly->pos.size()).arg(it.parent).arg(b.min.x, 0, 'f', 2).arg(b.max.x, 0, 'f', 2).arg(b.min.y, 0, 'f', 2).arg(b.max.y, 0, 'f', 2).arg(b.min.z, 0, 'f', 2).arg(b.max.z, 0, 'f', 2); }
      log << "polybox: " + o.join(" ; "); continue; }
    if (w.size() == 5 && w[0] == "grpmove") {   // grpmove <group> dx dy dz — the group moved (what is in it goes along, as Move)
      const Item* g = scene_.item(w[1].toInt()); const QJsonArray a = g ? g->raw.value("gpivot").toArray() : QJsonArray(); const Vec3 p0 = a.size() == 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3();
      viewport_->setGroupPoint(w[1].toInt(), p0 + Vec3(w[2].toDouble(), w[3].toDouble(), w[4].toDouble()), true); log << "grpmove " + w.mid(1).join(" "); continue; }
    if (w.size() == 1 && w[0] == "tscount") {   // tscount — how many topoState signals / panel remakings in a second of nothing done
      int n = 0, p = 0; auto c1 = connect(viewport_, &Viewport::topoState, this, [&n] { n++; }); const int p0 = panelBuilds_;
      QElapsedTimer t; t.start(); while (t.elapsed() < 1000) QApplication::processEvents(); disconnect(c1); p = panelBuilds_ - p0;
      log << QString("tscount: topoState %1, panels %2").arg(n).arg(p); continue; }
    if (w.size() >= 2 && w[0] == "clicktip") {   // clicktip <tip start…> — the button whose tooltip starts so clicked (the panels' own), the picked UV after
      const QString want = w.mid(1).join(" "); QAbstractButton* hit = nullptr;
      for (QWidget* x : QApplication::allWidgets()) if (auto* b = qobject_cast<QAbstractButton*>(x); b && b->isVisible() && b->toolTip().startsWith(want)) hit = b;
      if (hit) { hit->click(); QElapsedTimer t; t.start(); while (t.elapsed() < 200) QApplication::processEvents(); }
      log << QString("clicktip %1: %2 · picked %3 %4 %5 · mode %6, %7 polygons selected").arg(want, hit ? "clicked" : "none").arg(viewport_->uvPicked.id).arg(viewport_->uvPicked.set).arg(viewport_->uvPicked.mat).arg(viewport_->topoMode).arg(viewport_->topoSel().f.size()); continue; }
    if (w.size() >= 3 && w[0] == "keysrun") {   // keysrun <tool> <button text…> — that tool's options button kept as a key (no tool with it), run from the Cursor
      auto& K = ui::Keymap::get(); pickTool(w[1]); ttools_->sync(); QAbstractButton* src = nullptr; const QString want = w.mid(2).join(" ");
      for (QWidget* x : QApplication::allWidgets()) if (auto* b = qobject_cast<QAbstractButton*>(x); b && b->text().trimmed() == want && b->parentWidget() && b->window() == ttools_) src = b;
      if (!src) { QStringList st; for (QWidget* x : QApplication::allWidgets()) if (auto* b = qobject_cast<QAbstractButton*>(x); b && b->text().trimmed() == want) { QStringList ch; for (QWidget* p = b; p; p = p->parentWidget()) ch << QString("%1%2").arg(p->metaObject()->className()).arg(p->isHidden() ? "(h)" : ""); st << ch.join("<"); }
        log << "keysrun: no button " + want + QString(" (tool %1, panel shown %2) %3").arg(viewport_->tool()).arg(ttools_->isVisible()).arg(st.join(" ; ")); continue; }
      const QString id = K.buttonId(src->text(), src->text(), src->toolTip()); pickTool("cursor"); ttools_->sync();
      auto nsum = [this] { const int m = viewport_->topoMesh(); const Item* it = scene_.item(m); double s = 0; if (it) { const PolyMesh* B = it->base ? it->base.get() : it->poly.get(); for (auto& n : B->cornerNormal) s += std::abs(n[0]) * 1.3 + std::abs(n[1]) * 1.7 + std::abs(n[2]); } return s; };
      const double s0 = nsum(); runAction(id); viewport_->beforeFrame(); const double s1 = nsum(); const ui::CustomBind* cb = K.customBind(id);
      log << QString("keysrun: tool now %1 · kept with %2 · normals %3 → %4").arg(viewport_->tool(), cb ? cb->tool : "?").arg(s0, 0, 'f', 3).arg(s1, 0, 'f', 3);
      K.removeCustom(id); continue; }
    if (w.size() >= 4 && w[0] == "keysdrop") {   // keysdrop <slot|-1> <png> <button text…> — that button of the window dropped onto the pies page (the settings' pies put back after)
      auto& K = ui::Keymap::get(); const auto pies0 = K.pies(); std::set<QString> cust0; for (auto& c : K.custom()) cust0.insert(c.id);
      QAbstractButton* src = nullptr; const QString want = w.mid(3).join(" ");
      for (QWidget* x : QApplication::allWidgets()) if (auto* b = qobject_cast<QAbstractButton*>(x); b && b->text().trimmed() == want && (!src || b->isVisible())) src = b;
      if (!src) { log << "keysdrop: no button " + want; continue; }
      ui::HotkeyDialog d(this); d.show(); if (auto* l = d.findChild<QListWidget*>()) l->setCurrentRow(l->count() - 4);
      for (int i = 0; i < 5; i++) QApplication::processEvents();
      auto drop = [&](int slot) { QPoint at(d.width() / 2, d.height() - 60);
        if (slot >= 0) for (QWidget* c : d.findChildren<QWidget*>()) if (c->isVisible() && c->property("pieSlot").toInt() == slot && c->property("pieSlot").isValid()) at = c->mapTo(&d, c->rect().center());
        auto* md = new QMimeData; md->setData("application/x-hmc-func", QJsonDocument(QJsonObject{{"label", src->text()}, {"text", src->text()}, {"tip", src->toolTip()}}).toJson(QJsonDocument::Compact));
        QDragEnterEvent en(at, Qt::CopyAction, md, Qt::LeftButton, {}); QApplication::sendEvent(&d, &en);
        QDropEvent dr(QPointF(at), Qt::CopyAction, md, Qt::LeftButton, {}); QApplication::sendEvent(&d, &dr); delete md;
        for (int i = 0; i < 5; i++) QApplication::processEvents(); };
      drop(-1); drop(w[1].toInt());
      QStringList got; for (auto& p : K.pies()) got << p.name + ": " + p.items.join(",") + " [" + [&] { QStringList l; for (auto& i : p.items) l << K.labelOf(i); return l.join(" | "); }() + "]";
      log << QString("keysdrop: %1 · shot %2").arg(got.join(" ; ")).arg(d.grab().save(w[2]));
      K.setPies(pies0); for (auto& c : std::vector<ui::CustomBind>(K.custom())) if (!cust0.count(c.id)) K.removeCustom(c.id);
      continue; }
    if (w.size() == 2 && w[0] == "navtest") { const QStringList p = w[1].split(QChar(0x2c)); log << QString("navtest %1: %2").arg(w[1]).arg(ui::Keymap::get().navFor(p[0].toInt(), p[1].toInt())); continue; }
    if (w.size() == 2 && w[0] == "navpaint") { ui::NavScheme s = ui::Keymap::get().nav(); s.paint = w[1]; ui::Keymap::get().setNav(s); log << "navpaint " + w[1]; continue; }   // (checks) the Paint room's controls: coat | ps
    if (w.size() == 2 && w[0] == "navpreset") { ui::Keymap::get().setNav(ui::Keymap::navPreset(w[1])); log << "navpreset " + w[1]; continue; }
    if (w.size() == 2 && w[0] == "keycast") {   // keycast ctrl+shift+k | alt+lmb | rmb | mmb | wheelup | wheeldown | off — as if pressed, the key display open
      if (w[1] == "off") { insp_->showKeyCast(false); log << "keycast off"; continue; }
      if (w[1].startsWith("look=")) { const QStringList v = w[1].mid(5).split(QChar(0x2c)); insp_->keyCast()->setLook(v.value(0).toDouble(), v.value(1).toDouble()); log << "keycast " + w[1]; continue; }   // look=<size>,<opacity>
      if (w[1] == "real") {                        // (real events through the app — to a widget no one shows: the filter's own path)
        if (!insp_->keyCast()->isVisible()) insp_->showKeyCast(true);
        QWidget dummy; QWidget child(&dummy); ulong ts = 1000;
        auto send = [&](QInputEvent* e) { e->setTimestamp(ts); QApplication::sendEvent(&child, e); QApplication::sendEvent(&dummy, e); };   // (twice: as a propagated one)
        { QKeyEvent a(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier); send(&a); ts++; QKeyEvent b(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier); send(&b); ts++;
          QKeyEvent c(QEvent::KeyRelease, Qt::Key_D, Qt::ControlModifier); send(&c); ts++; QKeyEvent d(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier); send(&d); ts++; }
        { QMouseEvent a(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1), Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier); send(&a); ts++;
          QMouseEvent b(QEvent::MouseButtonRelease, QPointF(1, 1), QPointF(1, 1), Qt::MiddleButton, Qt::NoButton, Qt::NoModifier); send(&b); ts++; }
        for (int i = 0; i < 3; i++) { QWheelEvent e(QPointF(1, 1), QPointF(1, 1), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false); send(&e); ts++; }
        QApplication::processEvents(); log << "keycast real"; continue; }
      KeyCastWindow* kc = insp_->keyCast(); if (!kc->isVisible()) insp_->showKeyCast(true);
      QStringList parts = w[1].toLower().split('+'); const QString last = parts.takeLast(); int mods = 0;
      for (const QString& m : parts) mods |= m == "ctrl" ? Qt::ControlModifier : m == "shift" ? Qt::ShiftModifier : m == "alt" ? Qt::AltModifier : 0;
      for (const QString& m : parts) kc->testKey(m == "ctrl" ? Qt::Key_Control : m == "shift" ? Qt::Key_Shift : Qt::Key_Alt, mods, true);
      if (last == "wheelup" || last == "wheeldown") kc->testMouse(0, mods, true, last == "wheelup" ? 1 : -1);
      else if (last == "lmb" || last == "rmb" || last == "mmb") { const int b = last == "rmb" ? 2 : last == "mmb" ? 3 : 1; kc->testMouse(b, mods, true); kc->testMouse(b, mods, false); }
      else { const int k = last.size() == 1 ? QKeySequence(last.toUpper())[0].key() : last == "space" ? Qt::Key_Space : last == "esc" ? Qt::Key_Escape : last == "tab" ? Qt::Key_Tab : Qt::Key_F1;
        kc->testKey(k, mods, true); kc->testKey(k, mods, false); }
      for (const QString& m : parts) kc->testKey(m == "ctrl" ? Qt::Key_Control : m == "shift" ? Qt::Key_Shift : Qt::Key_Alt, 0, false);
      QApplication::processEvents(); log << "keycast " + w[1] + QString(" · %1x%2").arg(kc->width()).arg(kc->height()); continue; }
    if (w.size() == 1 && w[0] == "shottime") { QApplication::processEvents(); QElapsedTimer t; t.start(); const QImage im = windowShot(true); const qint64 a = t.restart(); const QString tmp = QDir::temp().filePath("hm_note_shot.png"); im.save(tmp); const qint64 b = t.restart(); QImage back(tmp); const qint64 c = t.elapsed();
      log << QString("shottime %1x%2: grab %3 ms, png save %4 ms, png load %5 ms").arg(im.width()).arg(im.height()).arg(a).arg(b).arg(c); continue; }
    if (w.size() == 1 && w[0] == "venter") { QEvent ev(QEvent::Enter); QCoreApplication::sendEvent(viewport_, &ev); viewport_->beforeFrame(); log << "venter"; continue; }   // (checks) the pointer came into the viewport: its rings shown
    if (w.size() == 2 && w[0] == "winshot") { QApplication::processEvents(); log << (saveWindowShot(w[1]) ? "window shot " : "window shot FAILED ") + w[1]; continue; }
    if (w.size() >= 3 && w[0] == "pen") {   // pen press|move|release x,y [pressure]: a tablet pen at a place of the main window (the texture window takes the pen itself — no mouse events are made of it)
      const auto pp = pointOf(w[2]); if (!pp || !windowHandle()) { log << "bad pen point"; continue; }
      static QPointingDevice* penDev = new QPointingDevice("test pen", 77, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
      const QEvent::Type ty = w[1] == "press" ? QEvent::TabletPress : w[1] == "release" ? QEvent::TabletRelease : QEvent::TabletMove; const double pr = ty == QEvent::TabletRelease ? 0.0 : w.size() > 3 ? w[3].toDouble() : 0.6;
      QTabletEvent ev(ty, penDev, *pp, mapToGlobal(*pp), pr, 0, 0, 0, 0, 0, {}, ty == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton, ty == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
      QCoreApplication::sendEvent(windowHandle(), &ev); QApplication::processEvents();
      log << QString("pen %1 %2,%3 p %4 accepted %5").arg(w[1]).arg(pp->x()).arg(pp->y()).arg(pr).arg(ev.isAccepted()); continue; }
    if (w.size() < 2) continue;
    auto p = pointOf(w[1]); if (!p) { log << "bad point: " + w[1]; continue; }
    Qt::KeyboardModifiers mods; if (w.contains("shift")) mods |= Qt::ShiftModifier; if (w.contains("ctrl")) mods |= Qt::ControlModifier; if (w.contains("alt")) mods |= Qt::AltModifier;
    QEvent::Type t = w[0] == "press" ? QEvent::MouseButtonPress : w[0] == "release" ? QEvent::MouseButtonRelease : QEvent::MouseMove;
    Qt::MouseButton b = w.contains("right") ? Qt::RightButton : w.contains("middle") ? Qt::MiddleButton : Qt::LeftButton;
    if (t == QEvent::MouseButtonPress) held = b;
    QMouseEvent ev(t, *p, viewport_->mapToGlobal(*p), t == QEvent::MouseMove ? Qt::NoButton : b, held, mods);
    if (t == QEvent::MouseButtonRelease) held = {};
    QElapsedTimer evT; evT.start();
    QCoreApplication::sendEvent(viewport_, &ev);
    const qint64 evMs = evT.elapsed();
    viewport_->beforeFrame();
    log << QStringLiteral("%1 %2,%3 · %4 ms").arg(w[0]).arg(p->x(), 0, 'f', 1).arg(p->y(), 0, 'f', 1).arg(evMs);   // (how long the program was busy with it)
  }
  const Selection& s = viewport_->selection();
  QStringList it, an; for (int id : s.items) it << QString::number(id); for (int id : s.anchors) an << QString::number(id);
  log << "selected items: " + it.join(',') << "selected anchors: " + an.join(',');
  for (int id : s.anchors) if (const Anchor* a = scene_.anchor(id)) log << QStringLiteral("anchor %1 at %2 %3 %4").arg(id).arg(a->pos.x, 0, 'f', 3).arg(a->pos.y, 0, 'f', 3).arg(a->pos.z, 0, 'f', 3);
  { std::vector<int> cs; for (auto& [id, it] : scene_.items) if (it.type == ItemType::Curve) for (int a : s.anchors) if (std::find(it.anchors.begin(), it.anchors.end(), a) != it.anchors.end()) cs.push_back(id);
    QStringList ov; for (int id : cs) ov << QStringLiteral("%1:%2").arg(id).arg(scene_.item(id)->ovr.size()); log << "frozen handles of their curves: " + ov.join(' '); }
  { int nc = 0, ns = 0; for (auto& [id, it] : scene_.items) { nc += it.type == ItemType::Curve; ns += it.type == ItemType::Surface; }
    log << QStringLiteral("curves %1, walls %2, anchors %3").arg(nc).arg(ns).arg(scene_.anchors.size()); }
  log << QStringLiteral("pivot %1 %2 %3").arg(viewport_->camera().pivot.x, 0, 'f', 3).arg(viewport_->camera().pivot.y, 0, 'f', 3).arg(viewport_->camera().pivot.z, 0, 'f', 3);
  return log.join('\n');
}

// ======================= outliner =======================
void MainWindow::rebuildOutliner() { outliner_->rebuild(); }

bool MainWindow::dirty() const { return forcedDirty_ || viewDirty_ || changeTotal() != savedChanges_; }
QString MainWindow::displayName() const { return scene_.path.isEmpty() ? projName_ : QFileInfo(scene_.path).fileName(); }

void MainWindow::updateTitle() {
  QString name = displayName();
  if (dirty()) name += " *";
  setWindowTitle(name + QStringLiteral(" — HairHarpy"));
}

// the UV polygons of the set painted, drawn at the texture's size on a clear ground (light lines with a dark edge: seen over any colour)
// The Paint room shows the paint of every UV set on the model at once: the set shown now — live, from the canvas; the others — each
// one's picture composed when the brush left it (PaintSession::setLeaving), or all of them here: on coming into the room, on another map
QImage MainWindow::paintSetPicture(const QString& set, const QRect& r) {
  PaintCanvas* cv = paintLeft_->canvas(); paint::Document& d = paint_->doc; if (set == d.activeSet()) return cv->mapPicture(r);
  const QSize sz = d.setSize(set); if (sz.isEmpty()) return {}; const QString cur = d.activeSet(); const int cw = d.width(), ch = d.height();
  d.setActiveSet(set, sz.width(), sz.height()); QImage p = cv->mapPicture(r); d.setActiveSet(cur, cw, ch);   // (shown for the composing only: no pixel is copied)
  return p;
}
// (a set not shown: its picture for the model made anew - all of it, or the part that changed)
void MainWindow::paintOtherChanged(const QString& set, const QRect& r) {
  if (scene_.room != "paint" || !paintLeft_ || set == paint_->doc.activeSet() || paint_->view.startsWith("bake:")) return;
  auto& pv = scene_.paintView; const QSize sz = paint_->doc.setSize(set); if (sz.isEmpty()) return;
  const bool fresh = !pv.others.count(set) || pv.others[set].first.isNull(); auto& o = pv.others[set];
  if (r.isNull() || o.first.size() != sz) { o.first = paintSetPicture(set, {}); pv.othersFull.insert(set); }
  else { const QRect rr = r & QRect(QPoint(0, 0), sz); const QImage part = rr.isEmpty() ? QImage() : paintSetPicture(set, rr);
    if (!part.isNull()) { QPainter p(&o.first); p.setCompositionMode(QPainter::CompositionMode_Source); p.drawImage(rr.topLeft(), part);
      TileMask& tm = pv.othersDirty[set]; if (tm.w != sz.width() || tm.h != sz.height()) tm.reset(sz.width(), sz.height()); tm.mark(rr); } }
  o.second = ++paintOtherRev_;
  if (fresh) viewport_->paintViewChanged(); else viewport_->requestUpdate();
}
void MainWindow::paintOthersRefresh() {
  ui::DevOp devT("paintOthersRefresh", 15);
  auto& pv = scene_.paintView; const bool had = !pv.others.empty(); pv.others.clear(); if (!paint_ || !paintLeft_ || paint_->doc.width() <= 0 || paint_->view.startsWith("bake:")) return;
  const QString cur = paint_->doc.activeSet(); if (cur.isEmpty()) return;
  const auto sets = paint_->doc.sets();
  for (const auto& x : sets) { if (x.key == cur || x.key.isEmpty() || x.w <= 0 || x.h <= 0) continue;
    paint_->doc.ensureLoaded(x.key); if (!paint_->doc.hasPixelsIn(x.key)) continue;   // (a project just opened: the sets' pixels read now — every set's paint is on the model from the start)
    const QImage f = paintSetPicture(x.key, {}); if (!f.isNull()) pv.others[x.key] = {f, ++paintOtherRev_}; }
  if (had || !pv.others.empty()) viewport_->paintViewChanged();
}
// (the model's own textures as the layer "Model texture" of every UV set at once - on coming into the room, so no stroke waits for
// it when it first comes over a set)
void MainWindow::paintImportAll() {
  ui::DevOp devT("paintImportAll", 15);
  if (!paint_ || paint_->doc.width() <= 0) return; paint::Document& d = paint_->doc; const QString cur = d.activeSet(); if (cur.isEmpty()) return;
  const int cw = d.width(), ch = d.height(); int n = 0;
  for (const QString& name : viewport_->uvSetNamesAll()) { if (name == cur || paint_->texIn.contains(name)) continue; const auto p = viewport_->uvSetProps(name); if (p.w <= 0 || p.h <= 0) continue;
    if (!n) QApplication::setOverrideCursor(Qt::WaitCursor);
    d.setActiveSet(name, p.w, p.h); paintImportTexture(true); n++; }
  if (n) { d.setActiveSet(cur, cw, ch); QApplication::restoreOverrideCursor(); emit paint_->layersChanged(); }
}
QImage MainWindow::paintWireImage() const {
  if (!paint_ || paint_->doc.width() <= 0) return {};
  const int W = paint_->doc.width(), H = paint_->doc.height(); viewport_->paintIslands(paint_->doc.activeSet(), W, H);
  QImage im(W, H, QImage::Format_ARGB32); im.fill(0); QPainter p(&im); p.setRenderHint(QPainter::Antialiasing, false); p.setBrush(Qt::NoBrush);
  for (auto [c, w] : {std::pair{QColor(0, 0, 0, 130), 3.0}, std::pair{QColor(255, 255, 255, 220), 1.0}}) { p.setPen(QPen(c, w)); for (const QPolygonF& q : viewport_->paintUvWire()) p.drawPolygon(q); }
  return im;
}

int MainWindow::changeTotal() const { return viewport_->changeCount() + (paint_ ? paint_->edits : 0); }

QDockWidget* MainWindow::paintDock(const QString& name) const { for (QDockWidget* d : paintDocks_) if (d->objectName() == name) return d; return nullptr; }
// the room's panels as they are at first (the author's sketch, 17.11): on the left the tools' buttons | the brushes | the settings side by
// side, the manager under them across their width; on the right the palette over the layers, the bake / bridge / lighting tabbed under them
void MainWindow::paintDocksDefault() {
  auto D = [this](const char* n) { return paintDock(n); };
  QDockWidget *tl = D("pdTools"), *br = D("pdBrushes"), *st = D("pdSettings"), *mg = D("pdManager"), *pa = D("pdPalette"), *la = D("pdLayers");
  for (QDockWidget* d : paintDocks_) { d->setFloating(false); removeDockWidget(d); }
  addDockWidget(Qt::LeftDockWidgetArea, tl, Qt::Vertical); splitDockWidget(tl, br, Qt::Horizontal); splitDockWidget(br, st, Qt::Horizontal);
  addDockWidget(Qt::LeftDockWidgetArea, mg, Qt::Vertical);
  addDockWidget(Qt::RightDockWidgetArea, pa, Qt::Vertical); addDockWidget(Qt::RightDockWidgetArea, la, Qt::Vertical);
  for (QDockWidget* d : paintDocks_) d->show();
  if (QDockWidget* rf = D("pdReference")) { addDockWidget(Qt::RightDockWidgetArea, rf); rf->setFloating(true); rf->resize(420, 460); rf->move(mapToGlobal(QPoint(width() - 760, 140))); rf->hide(); }   // (the reference: a floating window, opened from ▦ Panels)
  resizeDocks({tl, br, st}, {46, 200, 352}, Qt::Horizontal); resizeDocks({tl, mg}, {470, 430}, Qt::Vertical);
  resizeDocks({pa}, {280}, Qt::Horizontal); resizeDocks({pa, la}, {330, 600}, Qt::Vertical);
  paintDocksMade_ = true;
}
void MainWindow::paintPop(QFrame*& pop, QPushButton* anchor, const std::function<QWidget*()>& make) {
  if (!pop) { pop = new QFrame(this, Qt::Popup); pop->setObjectName("pdPop"); pop->setStyleSheet(paintLeft_->sheet() + "#pdPop{background:#262626;border:1px solid #555;border-radius:6px}");
    auto* v = new QVBoxLayout(pop); v->setContentsMargins(8, 6, 8, 8); QWidget* body = make(); body->setMinimumWidth(320); v->addWidget(body);
    if (const QString h = helpOf(body); !h.isEmpty()) { auto* hq = new QToolButton; hq->setText("?"); hq->setToolTip(tr("About this panel")); hq->setCursor(Qt::PointingHandCursor); v->addWidget(hq, 0, Qt::AlignRight);   // (its notes under the "?")
      const QString title = anchor->text().remove(QString::fromUtf8(" ▾")).remove(QString::fromUtf8("🔗 "));
      connect(hq, &QToolButton::clicked, pop, [this, hq, title, body] { showPanelHelp(this, hq, title, helpOf(body)); }); } }
  if (pop->isVisible()) { pop->hide(); return; }
  pop->adjustSize(); QPoint p = anchor->mapToGlobal(QPoint(0, anchor->height() + 4));
  if (QScreen* sc = anchor->screen()) { const QRect a = sc->availableGeometry(); p.setX(std::min(p.x(), a.right() - pop->width())); }
  pop->move(p); pop->show();
}
void MainWindow::paintDocksEnter() {
  if (paintDocksOn_) return;
  if (!paintDocksMade_) paintDocksDefault();                   // (made once: every dock placed, so a kept arrangement has them all to restore)
  const QByteArray st = QSettings().value("layout/paint").toByteArray();
  if (st.isEmpty() || !restoreState(st, 1)) for (QDockWidget* d : paintDocks_) if (d->objectName() != "pdReference") d->show();   // (the reference opens from ▦ Panels)
  paintDocksOn_ = true;                                        // (only now: the moves made while restoring are not kept half-way)
}
void MainWindow::paintDocksLeave() {
  if (!paintDocksOn_) return; paintDocksSave(); paintDocksOn_ = false;
  for (QDockWidget* d : paintDocks_) { if (d->isFloating()) d->hide(); else d->hide(); }
}
void MainWindow::paintDocksSave() {
  if (paintDocksOn_ && isVisible()) QSettings().setValue("layout/paint", saveState(1)); }

void MainWindow::closeEvent(QCloseEvent* e) {
  if (!unsavedOk()) { e->ignore(); return; }
#ifdef HM_DEVTOOLS
  if (dev_ && dev_->recording()) dev_->stop(true);              // (the record closed with its last line)
#endif
  if (paint_ && paint_->saving()) {                             // (the layers still being written in the background: the program waits for them — a file cut short is worse than a second's wait)
    ui::ThinkScope th(tr("Writing the paint layers"), 0); th.now(); QElapsedTimer t; t.start();
    while (paint_->saving() && t.elapsed() < 120000) QApplication::processEvents(QEventLoop::AllEvents, 50); }
  paintDocksSave(); paintDocksOn_ = false;                     // (kept now; the docks' windows closing after this change nothing)
  QSettings st;
  st.setValue("main/geometry", saveGeometry());
  report_->hide();
  e->accept();
  // (the window closed — the program ends: its own windows (the tools, UV, Hotkeys…) never left hanging on their own)
  for (QWidget* w : QApplication::topLevelWidgets()) if (w != this && w->isVisible()) w->hide();
  QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

}  // namespace hm
