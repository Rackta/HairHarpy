// MainWindow — files: the File window's actions, ＋ New project, projects opened as copies, migration, presets, autosave,
// the recovery copy, ⟳ Reload, broken walls, files dropped onto the window (the HTML version's "Project save / open",
// "Autosave", "Recovery copy", "File window" and drag & drop parts).
#include "app/ExportPanels.h"
#include "app/FileWindow.h"
#include "app/Extras.h"
#include "ui/ToolBox.h"
#include "ui/DockPanel.h"
#include "app/ImportMesh.h"
#include "app/ImportWindow.h"
#include "app/MainWindow.h"
#include "app/PaintLeft.h"
#include "app/PaintSession.h"
#include "app/PaintPanels.h"
#include "app/TexWindow.h"
#include "app/RefCanvas.h"
#include "scene/Looks.h"
#include "app/Outliner.h"
#include "app/ReportWindow.h"
#include "app/RefBoard.h"
#include "io/Migrate.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include "scene/Topo.h"
#include "io/ProjectIO.h"
#include "scene/Convert.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QPointer>
#include <thread>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <functional>

namespace hm {

namespace {
const QStringList MODEL_EXTS = {"obj", "glb", "gltf", "fbx"};
bool isProj(const QString& f) { QString e = QFileInfo(f).suffix().toLower(); return e == "hairmesh" || e == "json"; }
bool isModel(const QString& f) { return MODEL_EXTS.contains(QFileInfo(f).suffix().toLower()); }
bool writeBytes(const QString& path, const QByteArray& data) { QDir().mkpath(QFileInfo(path).absolutePath()); QSaveFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit(); }

// files dragged over the window: the window lit — dropped anywhere, each file goes in as what it is (a window of its own —
// over the viewport too)
class DropOverlay : public QWidget {
public:
  std::function<void(const QStringList&, const QPoint&)> dropped;   // (the files, the point on the screen)
  // (owned by the main window, not "on top of all": it lies just over our window — another program's window above ours
  // stays above it, so a file dragged from Explorer to that program isn't caught by us on the way)
  explicit DropOverlay(QWidget* host, QWidget* owner) : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint), host_(host) {
    setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setAcceptDrops(true);
  }
  void cover() { QRect r = host_->rect(); setGeometry(QRect(host_->mapToGlobal(r.topLeft()), r.size())); show(); }   // (no raise: it would lift it over the other programs)
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), QColor(0, 0, 0, 110));
    p.setPen(QPen(QColor(0x6a, 0x9a, 0xe0), 3, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(QRectF(rect()).adjusted(12, 12, -12, -12), 14, 14);
    QFont f = font(); f.setPixelSize(22); f.setBold(true); p.setFont(f); p.setPen(QColor(0xee, 0xee, 0xee)); p.drawText(rect(), Qt::AlignCenter, QObject::tr("⇩"));
  }
  void dragEnterEvent(QDragEnterEvent* e) override { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
  void dragMoveEvent(QDragMoveEvent* e) override { e->acceptProposedAction(); }
  void dragLeaveEvent(QDragLeaveEvent*) override { hide(); }
  void dropEvent(QDropEvent* e) override {
    QStringList fs; for (const QUrl& u : e->mimeData()->urls()) if (u.isLocalFile()) fs << u.toLocalFile();
    const QPoint at = mapToGlobal(e->position().toPoint()); e->acceptProposedAction(); hide();
    if (dropped) QTimer::singleShot(0, this, [this, fs, at] { dropped(fs, at); });
  }
  void mousePressEvent(QMouseEvent*) override { hide(); }
private:
  QWidget* host_;
};
}  // namespace

// ---------- the File window ----------
void MainWindow::openFiles(const QString& tab) {
  files::Host h;
  h.open = [this](const QString& p) { if (!unsavedOk()) return false; forget_ = true; bool ok = openProject(p); forget_ = false; return ok; };
  h.openDialog = [this] { runAction("openDialog"); };
  h.newProject = [this](const QString& room) { if (newProject(true) && room != "hair") { goRoom(room); } };
  h.openCopy = [this](const QString& p, const QString& n, bool unsaved) { return openProjectCopy(p, n, unsaved); };
  h.migrate = [this](const QString& p, const QString& n, bool wrap) { return migrateFile(p, n, wrap); };
  h.migrateDialog = [this] {
    QSettings st; QString p = QFileDialog::getOpenFileName(this, tr("Migrate a project"), st.value("lastDir").toString(), tr("HairHarpy project (*.hairmesh *.json)"));
    if (!p.isEmpty() && migrateFile(p, QString(), true)) files::closeFileWindow();
  };
  h.presetGroupName = [this] { return presetGroupName(); };
  h.createPreset = [this](const QString& d) { return createPreset(d); };
  h.autosaveSettings = [this] { files::openAutosaveDialog(this, files::baseName(displayName()), asMsg_, [this] { autosaveNow(true); }); };
  files::openFileWindow(this, h, tab);
}

void MainWindow::afterLoad(const QString& path) {
  QString th = files::fileThumb(path);
  files::addRecent(path, th);
  if (th.isEmpty()) QTimer::singleShot(1500, this, [this, path] { if (scene_.path == path) files::addRecent(path, thumbDataUrl()); });   // (a file without a preview: one from the view)
}

// ＋ New project: an empty scene — no head, no hair, names counted from 1 — as the program starts
bool MainWindow::newProject(bool ask) {
  if (ask && !unsavedOk()) return false;
  waitSave(); propsPin_ = -1;
  scene_.clear(); scene_.iso.reset(); scene_.nextId = 1; scene_.nextOrder = 0;
  scene_.fileRaw = QJsonObject{{"app", "anime-hair-modeler"}, {"settings", QJsonObject{{"rootLogic", 2}}}};
  if (paint_) { paint_->resetForProject(); emit paint_->layersChanged(); emit paint_->pixelsChanged({}); }
  if (paintLeft_) paintLeft_->historyReset();
  meshesGroup(scene_); scene_.reindex(); scene_.revision++;
  report_->hide(); if (board_) board_->reload(); if (refCanvas_) refCanvas_->reset();
  viewport_->sceneChanged(true);                             // (the hair — cocoons — rebuilt on the new scene first: the outliner reads them; before, it read the cocoons of the project closed — a crash)
  rebuildOutliner();
  viewport_->resetPanes(!ask && !startedOnce_);                 // (the program's start: the windows as the user left them)
  startedOnce_ = true;
  Box3 b; b.expand({-0.5, 0, -0.5}); b.expand({0.5, 1, 0.5}); viewport_->fitToBox(b);   // (the start-up frame)
  savedChanges_ = changeTotal(); viewDirty_ = false; forcedDirty_ = false; projName_ = "hair-project.hairmesh";
  updateTitle(); syncPanels(); if (syncDisplay_) syncDisplay_();
  if (ask) { files::closeFileWindow(); ui::toast(tr("New empty project — import a head (or migrate hair) to start")); }
  return true;
}

// an example / an autosave: opened from its file, kept as a copy (no file of its own — Save asks where)
bool MainWindow::openProjectCopy(const QString& path, const QString& name, bool unsaved) {
  if (!unsavedOk()) return false;
  forget_ = true; bool ok = openProject(path); forget_ = false;
  if (!ok) return false;
  files::removeRecent(path);                                  // (not one of the user's projects)
  scene_.path.clear(); projName_ = name; forcedDirty_ = unsaved;
  updateTitle();
  return true;
}

// ---------- migration, presets ----------
bool MainWindow::migrateFile(const QString& path, const QString& name, bool wrap) {
  QFile f(path); if (!f.open(QIODevice::ReadOnly)) { ui::toast(tr("Could not read %1").arg(QFileInfo(path).fileName())); return false; }
  QJsonObject P = QJsonDocument::fromJson(f.readAll()).object();
  if (P.isEmpty()) { ui::toast(tr("Not a project file")); return false; }
  QString nm = name;
  if (nm.isEmpty()) { bool pre = P.value("preset").toBool(); nm = pre ? P.value("name").toString(files::baseName(path)) : files::baseName(path); wrap = !pre; }
  auto fit = headFit(scene_, P, path);
  bool ok = false;
  viewport_->runEdit([&](Scene& s, Selection& sel, const CurveCache&) -> QString {
    MigrateResult r = migrateData(s, P, nm, wrap, fit); if (!r.ok) return "!" + r.message;
    ok = true; sel.anchors.clear(); sel.items = r.select; return r.message;
  }, true);
  if (!ok) return false;
  bool head = false; for (auto& [id, it] : scene_.items) if (it.headTag && it.poly) head = true;
  if (!head) { Box3 b; for (auto& [id, a] : scene_.anchors) b.expand(a.pos); if (!b.empty()) viewport_->fitToBox(b); }   // (fitToHair)
  rebuildOutliner();
  return true;
}

QString MainWindow::presetGroupName() const {
  for (int id : viewport_->selection().items) if (const Item* g = scene_.item(id); g && g->type == ItemType::Group && !g->raw.value("guidesGroup").toBool() && !g->raw.value("meshesGroup").toBool() && !g->raw.value("importGroup").toBool() && !g->raw.value("collGroup").toBool()) return g->name;
  return {};
}

// ＋ Create preset: the selected group (and what is inside — no meshes) as a file of its own, with a picture of it
bool MainWindow::createPreset(const QString& dir) {
  int gid = -1;
  for (int id : viewport_->selection().items) if (const Item* g = scene_.item(id); g && g->type == ItemType::Group && !g->raw.value("guidesGroup").toBool() && !g->raw.value("meshesGroup").toBool() && !g->raw.value("importGroup").toBool() && !g->raw.value("collGroup").toBool()) { gid = id; break; }
  if (gid < 0) { ui::toast(tr("Select a group in the scene first — it becomes the preset")); return false; }
  if (dir.isEmpty() || !QDir().mkpath(dir)) { ui::toast(tr("Choose the presets folder first")); return false; }
  // its picture: the group framed (the camera and the selection back afterwards)
  Camera keepCam = viewport_->camera(); std::vector<int> keepSel = viewport_->selection().items;
  viewport_->setSelectedItems({gid}); viewport_->frameSelection(); viewport_->setSelectedItems({});
  QString thumb = thumbDataUrl();
  viewport_->camera() = keepCam; viewport_->setSelectedItems(keepSel); viewport_->liveView();
  // the group's records (no meshes), as the project writes them
  Scene tmp; std::function<void(int)> add = [&](int id) {
    const Item* it = scene_.item(id); if (!it || it->type == ItemType::Mesh || it->type == ItemType::IMesh) return;
    Item c = *it; if (id == gid) c.parent = -1; tmp.items.emplace(id, c);
    for (int a : it->anchors) if (const Anchor* A = scene_.anchor(a)) tmp.anchors.emplace(a, *A);
    if (it->type == ItemType::Group) for (int ch : it->children) add(ch);
  };
  add(gid); tmp.roots = {gid};
  for (auto& [id, it] : tmp.items) if (it.type == ItemType::Group) { std::vector<int> ch; for (int c : it.children) if (tmp.items.count(c)) ch.push_back(c); it.children = ch; }
  QJsonObject full = QJsonDocument::fromJson(projectJson(tmp, SaveExtras{})).object(); expandSharedImages(full);
  const QString name = scene_.item(gid)->name;
  // (written key by key in this order — the File window reads "preset", the name and the picture from the file's head; a QJsonObject
  // would sort the keys and put the whole of anchors and items first)
  auto val = [](const QJsonValue& x) { const QByteArray b = QJsonDocument(QJsonArray{x}).toJson(QJsonDocument::Compact); return b.mid(1, b.size() - 2); };
  QByteArray P = "{\"app\":\"anime-hair-modeler\",\"version\":1,\"preset\":true,\"name\":" + val(name) + ",\"thumb\":" + val(thumb)
               + ",\"roots\":" + val(full.value("roots")) + ",\"anchors\":" + val(full.value("anchors")) + ",\"items\":" + val(full.value("items")) + "}";
  QString base = QString(name).replace(QRegularExpression("[\\\\/:*?\"<>|]+"), "_").trimmed(); if (base.isEmpty()) base = "Preset";
  QString fn = base + ".hairmesh"; for (int i = 2; QFile::exists(QDir(dir).filePath(fn)); i++) fn = QString("%1 %2.hairmesh").arg(base).arg(i);
  if (!writeBytes(QDir(dir).filePath(fn), P)) { ui::toast(tr("Could not save the preset")); return false; }
  ui::toast(tr("Preset \"%1\" saved: %2").arg(name, fn));
  return true;
}

// ---------- autosave, the recovery copy, reload ----------
// Autosave: every N minutes (only when something changed since the last copy) into its folder, as
// <project>_autosave_1…K.hairmesh, overwritten in turn
void MainWindow::autosaveNow(bool manual) {
  ui::DevOp devT("autosave", 0, {{"manual", manual}});
  files::AutosaveSettings a = files::autosaveSettings();
  if (a.dir.isEmpty() || !QDir().mkpath(a.dir)) { asMsg_ = tr("No folder chosen"); if (manual) ui::toast(tr("Autosave: choose a folder in ⏱ Autosave")); return; }
  QString base = files::baseName(displayName()); QSettings st; QString key = "autosave/slot/" + base;
  int k = st.value(key, 1).toInt(); if (k < 1 || k > a.copies) k = 1;
  QString name = QString("%1_autosave_%2.hairmesh").arg(base).arg(k);
  viewport_->beforeFrame();
  ui::BusyScope bz(tr("Autosaving"));
  SaveExtras x; x.camera = viewport_->cameraJson(); x.thumb = thumbDataUrl(); x.cocoons = viewport_->hair().cocoonSystem();
  x.convChecked = convChecked(scene_, viewport_->hair(), viewport_->selection().items);
  if (!manual) {                                                // (by the timer: made into the file and written in the background — no pause)
    if (asBusy_) return; asBusy_ = true;
    auto sc = std::make_shared<Scene>(fileScene()); const QString path = QDir(a.dir).filePath(name); const int copies = a.copies, cc = changeTotal();
    QPointer<MainWindow> self(this);
    snapCocoons(x, *sc);
    std::thread([self, sc, x, path, name, key, k, copies, cc] {
      const bool ok = writeBytes(path, projectJson(*sc, x));
      QMetaObject::invokeMethod(qApp, [self, ok, name, key, k, copies, cc, path] {
        if (!self) return; self->asBusy_ = false;
        if (!ok) { self->asMsg_ = tr("Autosave failed: could not write %1").arg(name); ui::toast(self->asMsg_); return; }
        QSettings().setValue(key, k % copies + 1); self->asLast_ = QDateTime::currentMSecsSinceEpoch(); self->asLastChange_ = cc;
        if (self->paint_) self->paint_->saveFilesAsync(path, false);   // (the paint layers beside the copy, in its own folder — each copy is whole: a version to go back to)
        self->asMsg_ = tr("Last: %1 at %2").arg(name, QTime::currentTime().toString("HH:mm:ss")); ui::toast(tr("Autosaved: %1").arg(name)); }, Qt::QueuedConnection);
    }).detach();
    return;
  }
  if (!writeBytes(QDir(a.dir).filePath(name), projectJson(fileScene(), x))) { asMsg_ = tr("Autosave failed: could not write %1").arg(name); ui::toast(asMsg_); return; }
  st.setValue(key, k % a.copies + 1);
  if (paint_) paint_->saveFilesAsync(QDir(a.dir).filePath(name), false);
  asLast_ = QDateTime::currentMSecsSinceEpoch(); asLastChange_ = changeTotal();
  asMsg_ = tr("Last: %1 at %2").arg(name, QTime::currentTime().toString("HH:mm:ss")); ui::toast(tr("Autosaved: %1").arg(name));
}

// the unsaved scene into the program's own folder once a minute when it changed (and before heavy operations): a crash
// loses nothing. Saving the project clears it; the next start offers it back
// a mouse / pen button held or a stroke on the model going on: the saves by the timer wait for it to end (3D-Coat's autosave froze the
// window for 2.6 s in the middle of a stroke every ten minutes — the author's recording of 2026-11-16)
bool MainWindow::handsBusy() const { return QApplication::mouseButtons() != Qt::NoButton || (viewport_ && viewport_->paintDragging()); }
void MainWindow::saveRecovery() {
  if (!dirty() || changeTotal() == recChange_ || recBusy_) return;
  if (handsBusy()) { QTimer::singleShot(2000, this, [this] { saveRecovery(); }); return; }
  ui::DevOp devT("recovery", 0);
  // (the scene taken as it is now — its meshes shared, not copied — then made into the file and written in the background:
  // a heavy project's copy no longer stops the window for a second every minute)
  SaveExtras x; x.camera = viewport_->cameraJson(); x.cocoons = viewport_->hair().cocoonSystem();
  auto sc = std::make_shared<Scene>(fileScene()); const QString file = QDir(files::appDataDir()).filePath("recovery.hairmesh");
  const QString name = displayName(), orig = scene_.path; const int cc = changeTotal(), gen = recGen_; recBusy_ = true;
  QPointer<MainWindow> self(this);
  snapCocoons(x, *sc);
  std::thread([self, sc, x, file, name, orig, cc, gen] {
    const bool ok = writeBytes(file, projectJson(*sc, x));
    QMetaObject::invokeMethod(qApp, [self, ok, file, name, orig, cc, gen] {
      if (!self) return; self->recBusy_ = false;
      if (gen != self->recGen_) { QFile::remove(file); return; }   // (saved meanwhile: no recovery copy wanted)
      if (!ok) return;
      QSettings st; st.setValue("recovery/name", name); st.setValue("recovery/path", orig); st.setValue("recovery/time", QDateTime::currentMSecsSinceEpoch());
      self->recChange_ = cc; }, Qt::QueuedConnection);
  }).detach();
}
void MainWindow::clearRecovery() { recGen_++; QFile::remove(QDir(files::appDataDir()).filePath("recovery.hairmesh")); QSettings().remove("recovery"); recChange_ = -1; }

void MainWindow::offerRecovery() {
  QString file = QDir(files::appDataDir()).filePath("recovery.hairmesh"); if (!QFile::exists(file)) return;
  QSettings st; QString name = st.value("recovery/name").toString(), orig = st.value("recovery/path").toString();
  // (a copy whose record is gone — the settings cleared, the file left: named and dated by the file itself, not "" of 1970)
  if (name.isEmpty()) name = tr("an unsaved project");
  const qint64 t0 = st.value("recovery/time").toLongLong();
  QString when = QLocale().toString(t0 > 0 ? QDateTime::fromMSecsSinceEpoch(t0) : QFileInfo(file).lastModified(), QLocale::ShortFormat);
  QMessageBox m(QMessageBox::Question, tr("Restore unsaved work?"), tr("The app was closed or froze with unsaved changes. A recovery copy of \"%1\" from %2 was kept.").arg(name, when), QMessageBox::NoButton, this);
  auto* rs = m.addButton(tr("Restore"), QMessageBox::AcceptRole); auto* ds = m.addButton(tr("Discard"), QMessageBox::DestructiveRole); m.addButton(tr("Later"), QMessageBox::RejectRole);
  ds->setToolTip(tr("Delete the recovery copy")); m.exec();
  if (m.clickedButton() == ds) { clearRecovery(); return; }
  if (m.clickedButton() != rs) return;
  QFile f(file); if (!f.open(QIODevice::ReadOnly)) return;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  LoadResult r = loadProjectData(f.readAll(), orig.isEmpty() ? file : orig, scene_);
  QApplication::restoreOverrideCursor();
  if (!r.ok) { QMessageBox::warning(this, tr("Restore"), r.error); return; }
  scene_.path.clear(); projName_ = name.isEmpty() ? "hair-project.hairmesh" : name; looks::projectTexDir(scene_);
  viewport_->sceneChanged(true); rebuildOutliner();   /* (the hair — cocoons — rebuilt on the new scene first: the outliner reads them) */ if (board_) board_->reload(); if (refCanvas_) refCanvas_->reset();
  // (the Paint room's layers as the project's last save left them — the copy holds the list, the pixels lie beside the project.
  // Without this the room started empty, and a save over the project then removed its painted layers)
  if (paint_) { paint_->fromJson(scene_.fileRaw.value("paint").toObject()); paint_->loadFiles(orig); }
  if (paintLeft_) paintLeft_->historyReset();
  propsPin_ = -1;
  savedChanges_ = changeTotal(); viewDirty_ = false; forcedDirty_ = true;
  updateTitle(); syncPanels(); if (syncDisplay_) syncDisplay_();
  files::closeFileWindow();
  ui::toast(tr("Restored unsaved work from %1").arg(when));
}

// ⟳ Reload: the program restarts and opens the same project — saved or not (unsaved work kept, Ctrl+S writes the same file)
void MainWindow::reloadApp() {
  viewport_->beforeFrame();
  SaveExtras x; x.camera = viewport_->cameraJson(); x.cocoons = viewport_->hair().cocoonSystem();
  x.convChecked = convChecked(scene_, viewport_->hair(), viewport_->selection().items);
  const QString rf = QDir(files::appDataDir()).filePath("reload.hairmesh");
  if (!writeBytes(rf, projectJson(fileScene(), x))) { ui::toast(tr("Could not keep the project for the reload")); return; }
  // (the Paint room's layers and freeze masks beside the reload copy: strokes not saved yet go over the reload too)
  const bool px = paint_ && paint_->doc.width() > 0;
  if (px) { if (!paint_->saveFiles(rf)) { ui::toast(tr("Could not keep the paint layers for the reload")); return; } }
  QSettings st; st.setValue("reload/paint", px); st.setValue("reload/name", displayName()); st.setValue("reload/path", scene_.path); st.setValue("reload/unsaved", dirty()); st.setValue("reload/time", QDateTime::currentMSecsSinceEpoch());
  st.setValue("reload/ach", extras::earned());              // (the achievements of this window go on over the reload)
  st.setValue("reload/room", scene_.room);                   // (and the room shown)
  st.setValue("main/geometry", saveGeometry());
  paintDocksSave(); st.sync();                                 // (the panels' arrangement kept before the new copy starts reading it)
  // (Linux AppImage: the exe lives in a mount that goes with this process — the .AppImage file itself is started)
  const QString self = qEnvironmentVariableIsSet("APPIMAGE") ? qEnvironmentVariable("APPIMAGE") : QCoreApplication::applicationFilePath();
  if (!QProcess::startDetached(self, {})) { ui::toast(tr("Could not restart the program")); return; }
  forget_ = true; close();
}

void MainWindow::startup(bool projectGiven) {
  // autosave (checked every 15 s), the recovery copy (every minute)
  auto* as = new QTimer(this); as->setInterval(15000);
  connect(as, &QTimer::timeout, this, [this] {
    files::AutosaveSettings a = files::autosaveSettings(); qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!a.on || now - asLast_ < qint64(a.minutes) * 60000) return;
    if (!dirty() || changeTotal() == asLastChange_) { asLast_ = now; return; }   // (nothing unsaved, or nothing new since the last copy)
    if (handsBusy()) { QTimer::singleShot(1500, this, [this] { if (!handsBusy() && !asBusy_) autosaveNow(false); }); return; }   // (a stroke or a drag going on: not in the middle of it — once the button is let go)
    autosaveNow(false);
  });
  asLast_ = QDateTime::currentMSecsSinceEpoch(); as->start();
  auto* rc = new QTimer(this); rc->setInterval(60000); connect(rc, &QTimer::timeout, this, [this] { saveRecovery(); }); rc->start();
  extras::startChimera(tools_, dpTool_);                      // (one launch in five she visits)
  if (projectGiven) return;                                   // (a project opened from its file: nothing else to show)
  // after ⟳ Reload: straight back into that project
  QSettings st; QString rf = QDir(files::appDataDir()).filePath("reload.hairmesh");
  if (QFile::exists(rf) && QDateTime::currentMSecsSinceEpoch() - st.value("reload/time").toLongLong() < 10 * 60000) {
    QString name = st.value("reload/name").toString(), path = st.value("reload/path").toString(), room = st.value("reload/room").toString(); bool unsaved = st.value("reload/unsaved").toBool(), px = st.value("reload/paint").toBool();
    extras::restore(st.value("reload/ach").toStringList());
    QFile f(rf);
    if (f.open(QIODevice::ReadOnly)) {
      LoadResult r = loadProjectData(f.readAll(), path.isEmpty() ? rf : path, scene_); f.close();
      QFile::remove(rf); st.remove("reload");
      if (r.ok) {
        scene_.path = path; projName_ = name.isEmpty() ? "hair-project.hairmesh" : name; looks::projectTexDir(scene_);
        viewport_->sceneChanged(true); rebuildOutliner();   /* (the hair — cocoons — rebuilt on the new scene first: the outliner reads them) */ if (board_) board_->reload(); if (refCanvas_) refCanvas_->reset();
        savedChanges_ = changeTotal(); viewDirty_ = false; forcedDirty_ = unsaved;
        updateTitle(); syncPanels(); if (syncDisplay_) syncDisplay_();
        // (the layers as they were at the reload — written beside the reload copy; its folder stays until the next reload: the UV sets not looked at yet are read from it)
        if (paint_) { paint_->fromJson(scene_.fileRaw.value("paint").toObject()); paint_->loadFiles(px ? rf : path); }
        if (room == "topo" || room == "paint") { goRoom(room); }
        return;
      }
    }
  }
  QFile::remove(rf);
  newProject(false);
  // the very first start (nothing remembered yet) — the Examples; later — Recent projects; the recovery offer over it
  bool first = !st.value("started").toBool() && files::recentList().empty(); st.setValue("started", true);
  openFiles(first ? "examples" : "recent");
  // (the recovery question only once the viewport has drawn its first frame: asked earlier, the window waited under it with a white
  // viewport — the author's recording, 17.11, 16:35)
  auto t0 = std::make_shared<QElapsedTimer>(); t0->start(); auto ask = std::make_shared<std::function<void()>>();
  *ask = [this, t0, ask] { if (viewport_->framesDrawn > 0 || t0->elapsed() > 5000) { offerRecovery(); *ask = nullptr; return; } QTimer::singleShot(50, this, [ask] { if (*ask) (*ask)(); }); };
  QTimer::singleShot(300, this, [ask] { if (*ask) (*ask)(); });
}

// ---------- broken walls ----------
// a wall whose side curve is no longer in the project can't be built: opening finds them and offers to delete them
void MainWindow::offerBrokenWalls() {
  auto ok = [&](int id) { const Item* c = scene_.item(id); return c && c->type == ItemType::Curve; };
  std::vector<int> L; for (auto& [id, it] : scene_.items) if (it.type == ItemType::Surface && (!ok(it.a) || !ok(it.b))) L.push_back(id);
  if (L.empty()) return;
  QStringList names; for (int id : L) { const Item* w = scene_.item(id); const Item* g = scene_.item(w->parent); names << "▦ " + w->name + (g ? " — " + g->name : QString()); }
  QMessageBox m(QMessageBox::Warning, tr("Broken wall%1: %2").arg(L.size() > 1 ? "s" : "").arg(L.size()),
                (L.size() > 1 ? tr("These walls refer to a curve that is no longer in the project — they can't be built. Delete them?") : tr("This wall refers to a curve that is no longer in the project — it can't be built. Delete it?")),
                QMessageBox::NoButton, this);
  m.setDetailedText(names.join("\n"));
  m.addButton(tr("Keep"), QMessageBox::RejectRole); auto* sel = m.addButton(tr("Select them"), QMessageBox::ActionRole); auto* del = m.addButton(tr("Delete %1").arg(L.size()), QMessageBox::AcceptRole);
  m.exec();
  if (m.clickedButton() == sel) { viewport_->setSelectedItems(L); return; }
  if (m.clickedButton() != del) return;
  viewport_->runEdit([L](Scene& s, Selection& sl, const CurveCache&) -> QString {
    for (int id : L) { s.removeFromParent(id); s.items.erase(id); }
    std::vector<int> keep; for (int id : sl.items) if (s.item(id)) keep.push_back(id); sl.items = keep; s.reindex();
    return QObject::tr("Deleted %1 broken wall%2 (Ctrl+Z — back)").arg(L.size()).arg(L.size() > 1 ? "s" : "");
  }, true);
  rebuildOutliner();
}

// ---------- the bridge's ⇪ Send button, files dropped onto the window ----------
void MainWindow::updateSendBtn() { if (!sendBtn_) return; sendBtn_->setVisible(bridgeReady()); sendBtn_->setText(sendLabel()); sendBtn_->setToolTip(sendTip()); }

void MainWindow::showDropOverlay() {
  if (!dropOverlay_) { auto* o = new DropOverlay(centralWidget(), this); o->dropped = [this](const QStringList& f, const QPoint& at) { importDropped(f, at); }; dropOverlay_ = o; }
  static_cast<DropOverlay*>(dropOverlay_)->cover();
}
// ---- items dragged between two HairHarpys (18.11: two projects open — an item of one dragged from its outliner into the other) ----
static bool foreignItems(const QMimeData* m) { return m->hasFormat(kItemsMime) && m->data(kPidMime).toLongLong() != QCoreApplication::applicationPid(); }
QByteArray MainWindow::exportDragItems(const std::vector<int>& ids) {
  QJsonObject P = QJsonDocument::fromJson(projectJson(scene_, SaveExtras{})).object(); expandSharedImages(P);
  QJsonObject sub = dragSubset(P, ids);
  // (every imported mesh with its geometry inside: a mesh read from a file of its own would be looked for beside the other project)
  QJsonArray items; for (const QJsonValue& v : sub.value("items").toArray()) { QJsonObject o = v.toObject();
    if (o.value("type").toString() == "imesh") { const QJsonObject src = o.value("src").toObject();
      if (!src.contains("b64") && !src.contains("text")) if (const Item* it = scene_.item(o.value("id").toInt()); it && (it->base || it->poly)) {
        const PolyMesh& B = it->base ? *it->base : *it->poly; o.insert("src", QJsonObject{{"ext", "pmesh"}, {"b64", QString::fromLatin1(topo::packPMesh(B).toBase64())}, {"blobKind", "b64"}, {"live", true}}); } }
    items.append(o); }
  sub.insert("items", items); sub.insert("name", QFileInfo(displayName()).completeBaseName());
  { Box3 hb; for (auto& [id, it] : scene_.items) if (it.headTag && it.poly) hb.unite(it.poly->bounds());   // (this project's head: the other one fits what comes to its own head)
    if (!hb.empty()) sub.insert("srcHead", QJsonArray{hb.min.x, hb.min.y, hb.min.z, hb.max.x, hb.max.y, hb.max.z}); }
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/HairHarpy/Drag"; QDir().mkpath(dir);
  const QString path = QString("%1/%2_%3.hairmesh").arg(dir).arg(QCoreApplication::applicationPid()).arg(QDateTime::currentMSecsSinceEpoch());
  if (!writeBytes(path, QJsonDocument(sub).toJson(QJsonDocument::Compact))) return {};
  return path.toUtf8();
}
void MainWindow::importDragItems(const QString& path) {
  QFile f(path); if (!f.open(QIODevice::ReadOnly)) { ui::toast(tr("The dragged items could not be read")); return; }
  const QJsonObject P = QJsonDocument::fromJson(f.readAll()).object(); f.close(); QFile::remove(path);
  if (!P.value("dragged").toBool()) { ui::toast(tr("Not items from HairHarpy")); return; }
  const QString name = P.value("name").toString(tr("another project")); QString msg; bool ok = false; std::vector<int> got;
  // (20.11: of another scale than what this scene has — asked whether to fit it; the offered scale: the other project's head onto
  // this one's (as Migrate fits), else the sizes matched; the user's own taken as given)
  std::optional<HeadFit> fit;
  { Box3 cb; for (const QJsonValue& v : P.value("anchors").toArray()) { const QJsonArray p = v.toObject().value("p").toArray(); if (p.size() == 3) cb.expand(Vec3(p[0].toDouble(), p[1].toDouble(), p[2].toDouble())); }
    for (const QJsonValue& v : P.value("items").toArray()) { const QJsonObject o = v.toObject();
      if (o.value("type").toString() == "imesh" && o.value("src").isObject()) if (auto m = readMeshSource(o.value("src").toObject(), QString())) cb.unite(m->bounds()); }
    Box3 db; for (auto& [id, it] : scene_.items) if (it.headTag && it.poly) db.unite(it.poly->bounds());
    const QJsonArray sh = P.value("srcHead").toArray(); Box3 sb; if (sh.size() == 6) { sb.expand(Vec3(sh[0].toDouble(), sh[1].toDouble(), sh[2].toDouble())); sb.expand(Vec3(sh[3].toDouble(), sh[4].toDouble(), sh[5].toDouble())); }
    auto top = [](const Box3& b) { return Vec3((b.min.x + b.max.x) / 2, b.max.y, (b.min.z + b.max.z) / 2); };
    std::optional<double> best; Vec3 from, to;
    if (!db.empty() && !sb.empty()) { const Vec3 ss = sb.size(), ds = db.size(); const double k = std::cbrt((ds.x / ss.x) * (ds.y / ss.y) * (ds.z / ss.z));
      if (k > 0 && std::isfinite(k)) { best = k; from = top(sb); to = top(db); } }
    if (auto k = offerFitScale(viewport_, sceneContentBox(scene_), cb, name, best)) fit = HeadFit{*k, from, to}; }
  viewport_->runEdit([&](Scene& s, Selection& sel, const CurveCache&) -> QString {
    std::set<int> had; for (auto& [id, it] : s.items) had.insert(id);
    MigrateResult r = migrateData(s, P, name, false, fit, true); if (!r.ok) return "!" + r.message;
    if (fit) { std::vector<int> ms; for (auto& [id, it] : s.items) if (!had.count(id)) ms.push_back(id); scaleMeshes(s, ms, fit->k, fit->from, fit->to);   // (its meshes too: through their Transform)
      r.message += tr(" — scaled ×%1 to the project").arg(QString::number(fit->k, 'g', 4)); }
    ok = true; msg = r.message; got = r.select; sel.anchors.clear(); sel.items = r.select; return r.message; }, true);
  viewport_->sceneChanged(true); rebuildOutliner();   /* (the hair — cocoons — rebuilt on the new scene first: the outliner reads them) */
  // (20.11: what came in selected and in view — a project of another scale put it far from this one's head, out of reach)
  if (ok && !got.empty()) { viewport_->setSelectedItems(got); viewport_->beforeFrame(); viewport_->frameSelection(true); }
  if (ok) { activateWindow(); raise(); }
}
void MainWindow::dragMoveEvent(QDragMoveEvent* e) { if (foreignItems(e->mimeData())) { e->setDropAction(Qt::CopyAction); e->accept(); return; } QMainWindow::dragMoveEvent(e); }
void MainWindow::dropEvent(QDropEvent* e) {
  if (!foreignItems(e->mimeData())) { QMainWindow::dropEvent(e); return; }
  const QString path = QString::fromUtf8(e->mimeData()->data(kItemsMime)); e->setDropAction(Qt::CopyAction); e->accept();
  if (path.isEmpty()) { ui::toast(tr("The other HairHarpy gave nothing")); return; }
  QTimer::singleShot(0, this, [this, path] { importDragItems(path); });   // (after the drop: the other program is let go first)
}
void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (foreignItems(e->mimeData())) { e->setDropAction(Qt::CopyAction); e->accept(); return; }
  if (!e->mimeData()->hasUrls()) return;
  bool onlyImages = true; for (const QUrl& u : e->mimeData()->urls()) if (!QRegularExpression("\\.(png|jpe?g|webp|bmp|gif|avif)$", QRegularExpression::CaseInsensitiveOption).match(u.toLocalFile()).hasMatch()) onlyImages = false;
  if (onlyImages) return;                                     // (pictures: the reference board)
  e->acceptProposedAction(); showDropOverlay();
}
// files dropped on the window — each as what it is: a preset (.hairmesh / .json with "preset")
// comes into this project; a project opens (the first one; any other comes in as its hair); models (.obj / .glb / .gltf /
// .fbx, with the textures and .mtl dropped with them) open the import window (20.11: what and how — asked, as by the import button)
void MainWindow::importDropped(const QStringList& fs, const QPoint& at, bool ask) {
  QStringList json, rest, pics; bool models = false;
  static const QRegularExpression picRe("\\.(png|jpe?g|webp|bmp|gif|tga|psd)$", QRegularExpression::CaseInsensitiveOption);
  for (const QString& f : fs) { if (isProj(f)) json << f; else { rest << f; models = models || isModel(f); if (picRe.match(f).hasMatch()) pics << f; } }
  if (json.isEmpty() && !models && !pics.isEmpty()) { dropPictures(pics, at); return; }   // (pictures alone: textures — models dropped with them are their textures)
  if (json.isEmpty() && !models) { ui::toast(tr("Nothing to import: projects (.hairmesh / .json), models (.obj / .glb / .gltf / .fbx), textures (pictures — onto the model)")); return; }
  bool opened = false;
  for (const QString& f : json) {
    QFile in(f); const bool preset = in.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(in.readAll()).object().value("preset").toBool(); in.close();
    if (preset || opened) { if (migrateFile(f, QString(), true)) files::closeFileWindow(); continue; }   // (a preset, or a second project: its hair comes in as a group)
    if (!unsavedOk()) return;
    forget_ = true; opened = openProject(f); forget_ = false; if (opened) files::closeFileWindow();
  }
  // (models: the import window with them and the pictures / .mtl dropped with them in it — what and how is asked, as by the import button)
  if (models) { files::closeFileWindow(); if (ask) openImportWindow(viewport_, &scene_, rest); else importAuto(rest, viewport_, &scene_); rebuildOutliner(); }
}
// pictures dropped with no model: the Paint room — layers over the active one (as Layers from a file; a PSD with its layers);
// elsewhere — the maps of the model under the drop (its set under the pointer, or the set the file's name holds)
void MainWindow::dropPictures(const QStringList& pics, const QPoint& at) {
  if (scene_.room == "paint" && paint_) {
    QStringList out; QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const QString& f : pics) out << QFileInfo(f).fileName() + ": " + paint_->importLayers(f, layerSizeAsk(paint_, viewport_));
    QApplication::restoreOverrideCursor(); ui::toast(out.join("\n")); return;
  }
  const QPointF wp = at.x() < 0 && at.y() < 0 ? QPointF(viewport_->width() / 2.0, viewport_->height() / 2.0) : QPointF(viewport_->mapFromGlobal(at));
  const auto [m, f] = viewport_->pickMeshFace(wp);
  ui::toast(dropTextures(viewport_, &scene_, pics, m, f));
}

}  // namespace hm
