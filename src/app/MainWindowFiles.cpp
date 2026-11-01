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
#include "scene/Looks.h"
#include "app/Outliner.h"
#include "app/ReportWindow.h"
#include "app/RefBoard.h"
#include "io/Migrate.h"
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
#include <QVBoxLayout>
#include <functional>

namespace hm {

namespace {
const QStringList MODEL_EXTS = {"obj", "glb", "gltf", "fbx"};
bool isProj(const QString& f) { QString e = QFileInfo(f).suffix().toLower(); return e == "hairmesh" || e == "json"; }
bool isModel(const QString& f) { return MODEL_EXTS.contains(QFileInfo(f).suffix().toLower()); }
bool writeBytes(const QString& path, const QByteArray& data) { QDir().mkpath(QFileInfo(path).absolutePath()); QSaveFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit(); }

// files dragged over the window: boxes to drop them on (a window of its own — over the viewport too)
class DropOverlay : public QWidget {
public:
  std::function<void(const QStringList&, const QString&)> dropped;
  // (owned by the main window, not "on top of all": it lies just over our window — another program's window above ours
  // stays above it, so a file dragged from Explorer to that program isn't caught by us on the way)
  explicit DropOverlay(QWidget* host, QWidget* owner) : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint), host_(host) {
    setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setAcceptDrops(true);
    auto* v = new QVBoxLayout(this); v->addStretch(1); auto* h = new QHBoxLayout; h->addStretch(1); v->addLayout(h); v->addStretch(1);
    const char* Z[4][3] = {{"open", "Open project", ".hairmesh / .json — instead of the current one"}, {"migrate", "Migrate into this project", ".hairmesh / .json — its hair comes in as a group"},
                           {"head", "Import as head", ".obj / .glb / .gltf / .fbx — the reference model"}, {"mesh", "Import as mesh", ".obj / .glb / .gltf / .fbx — into \"Imported\""}};
    for (auto& z : Z) {
      auto* f = new QFrame; f->setObjectName("dz"); f->setProperty("zone", z[0]); f->setFixedSize(200, 120);
      auto* fv = new QVBoxLayout(f); auto* b = new QLabel(QObject::tr(z[1])); b->setStyleSheet("font-weight:bold;font-size:14px;color:#eee"); b->setAlignment(Qt::AlignCenter);
      auto* s = new QLabel(QObject::tr(z[2])); s->setWordWrap(true); s->setAlignment(Qt::AlignCenter); s->setStyleSheet("color:#aaa");
      fv->addWidget(b); fv->addWidget(s); h->addWidget(f); zones_.push_back(f);
    }
    h->addStretch(1); paintZones(nullptr);
  }
  void cover() { QRect r = host_->rect(); setGeometry(QRect(host_->mapToGlobal(r.topLeft()), r.size())); show(); }   // (no raise: it would lift it over the other programs)
protected:
  void paintEvent(QPaintEvent*) override { QPainter p(this); p.fillRect(rect(), QColor(0, 0, 0, 150)); }
  void dragEnterEvent(QDragEnterEvent* e) override { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
  void dragMoveEvent(QDragMoveEvent* e) override { e->acceptProposedAction(); paintZones(zoneAt(e->position().toPoint())); }
  void dragLeaveEvent(QDragLeaveEvent*) override { hide(); }
  void dropEvent(QDropEvent* e) override {
    QFrame* z = zoneAt(e->position().toPoint()); QStringList fs; for (const QUrl& u : e->mimeData()->urls()) if (u.isLocalFile()) fs << u.toLocalFile();
    e->acceptProposedAction(); hide();
    QString zone = z ? z->property("zone").toString() : QString();
    if (dropped) QTimer::singleShot(0, this, [this, fs, zone] { dropped(fs, zone); });
  }
  void mousePressEvent(QMouseEvent*) override { hide(); }
private:
  QFrame* zoneAt(const QPoint& p) { for (QFrame* f : zones_) if (f->geometry().contains(f->parentWidget()->mapFrom(this, p))) return f; return nullptr; }
  void paintZones(QFrame* hot) { for (QFrame* f : zones_) f->setStyleSheet(QString("#dz{background:%1;border:2px dashed %2;border-radius:10px}").arg(f == hot ? "#2c3a52" : "#262626", f == hot ? "#6a9ae0" : "#666")); }
  QWidget* host_; std::vector<QFrame*> zones_;
};
}  // namespace

// ---------- the File window ----------
void MainWindow::openFiles(const QString& tab) {
  files::Host h;
  h.open = [this](const QString& p) { if (!unsavedOk()) return false; forget_ = true; bool ok = openProject(p); forget_ = false; return ok; };
  h.openDialog = [this] { runAction("openDialog"); };
  h.newProject = [this](const QString& room) { if (newProject(true) && room == "topo") { viewport_->switchRoom("topo"); applyRoom(scene_.room); } };
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
  scene_.clear(); scene_.iso.reset(); scene_.nextId = 1; scene_.nextOrder = 0;
  scene_.fileRaw = QJsonObject{{"app", "anime-hair-modeler"}, {"settings", QJsonObject{{"rootLogic", 2}}}};
  meshesGroup(scene_); scene_.reindex(); scene_.revision++;
  report_->hide(); if (board_) board_->reload();
  rebuildOutliner();
  viewport_->sceneChanged(true);
  Box3 b; b.expand({-0.5, 0, -0.5}); b.expand({0.5, 1, 0.5}); viewport_->fitToBox(b);   // (the start-up frame)
  savedChanges_ = viewport_->changeCount(); viewDirty_ = false; forcedDirty_ = false; projName_ = "hair-project.hairmesh";
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
  for (int id : viewport_->selection().items) if (const Item* g = scene_.item(id); g && g->type == ItemType::Group && !g->raw.value("guidesGroup").toBool() && !g->raw.value("meshesGroup").toBool() && !g->raw.value("importGroup").toBool()) return g->name;
  return {};
}

// ＋ Create preset: the selected group (and what is inside — no meshes) as a file of its own, with a picture of it
bool MainWindow::createPreset(const QString& dir) {
  int gid = -1;
  for (int id : viewport_->selection().items) if (const Item* g = scene_.item(id); g && g->type == ItemType::Group && !g->raw.value("guidesGroup").toBool() && !g->raw.value("meshesGroup").toBool() && !g->raw.value("importGroup").toBool()) { gid = id; break; }
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
  QJsonObject full = QJsonDocument::fromJson(projectJson(tmp, SaveExtras{})).object();
  const QString name = scene_.item(gid)->name;
  QJsonObject P{{"app", "anime-hair-modeler"}, {"version", 1}, {"preset", true}, {"name", name}, {"thumb", thumb}, {"roots", full.value("roots")}, {"anchors", full.value("anchors")}, {"items", full.value("items")}};
  QString base = QString(name).replace(QRegularExpression("[\\\\/:*?\"<>|]+"), "_").trimmed(); if (base.isEmpty()) base = "Preset";
  QString fn = base + ".hairmesh"; for (int i = 2; QFile::exists(QDir(dir).filePath(fn)); i++) fn = QString("%1 %2.hairmesh").arg(base).arg(i);
  if (!writeBytes(QDir(dir).filePath(fn), QJsonDocument(P).toJson(QJsonDocument::Compact))) { ui::toast(tr("Could not save the preset")); return false; }
  ui::toast(tr("Preset \"%1\" saved: %2").arg(name, fn));
  return true;
}

// ---------- autosave, the recovery copy, reload ----------
// Autosave: every N minutes (only when something changed since the last copy) into its folder, as
// <project>_autosave_1…K.hairmesh, overwritten in turn
void MainWindow::autosaveNow(bool manual) {
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
    auto sc = std::make_shared<Scene>(fileScene()); const QString path = QDir(a.dir).filePath(name); const int copies = a.copies, cc = viewport_->changeCount();
    QPointer<MainWindow> self(this);
    std::thread([self, sc, x, path, name, key, k, copies, cc] {
      const bool ok = writeBytes(path, projectJson(*sc, x));
      QMetaObject::invokeMethod(qApp, [self, ok, name, key, k, copies, cc] {
        if (!self) return; self->asBusy_ = false;
        if (!ok) { self->asMsg_ = tr("Autosave failed: could not write %1").arg(name); ui::toast(self->asMsg_); return; }
        QSettings().setValue(key, k % copies + 1); self->asLast_ = QDateTime::currentMSecsSinceEpoch(); self->asLastChange_ = cc;
        self->asMsg_ = tr("Last: %1 at %2").arg(name, QTime::currentTime().toString("HH:mm:ss")); ui::toast(tr("Autosaved: %1").arg(name)); }, Qt::QueuedConnection);
    }).detach();
    return;
  }
  if (!writeBytes(QDir(a.dir).filePath(name), projectJson(fileScene(), x))) { asMsg_ = tr("Autosave failed: could not write %1").arg(name); ui::toast(asMsg_); return; }
  st.setValue(key, k % a.copies + 1);
  asLast_ = QDateTime::currentMSecsSinceEpoch(); asLastChange_ = viewport_->changeCount();
  asMsg_ = tr("Last: %1 at %2").arg(name, QTime::currentTime().toString("HH:mm:ss")); ui::toast(tr("Autosaved: %1").arg(name));
}

// the unsaved scene into the program's own folder once a minute when it changed (and before heavy operations): a crash
// loses nothing. Saving the project clears it; the next start offers it back
void MainWindow::saveRecovery() {
  if (!dirty() || viewport_->changeCount() == recChange_ || recBusy_) return;
  // (the scene taken as it is now — its meshes shared, not copied — then made into the file and written in the background:
  // a heavy project's copy no longer stops the window for a second every minute)
  SaveExtras x; x.camera = viewport_->cameraJson(); x.cocoons = viewport_->hair().cocoonSystem();
  auto sc = std::make_shared<Scene>(fileScene()); const QString file = QDir(files::appDataDir()).filePath("recovery.hairmesh");
  const QString name = displayName(), orig = scene_.path; const int cc = viewport_->changeCount(), gen = recGen_; recBusy_ = true;
  QPointer<MainWindow> self(this);
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
  QString when = QLocale().toString(QDateTime::fromMSecsSinceEpoch(st.value("recovery/time").toLongLong()), QLocale::ShortFormat);
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
  rebuildOutliner(); viewport_->sceneChanged(true); if (board_) board_->reload();
  savedChanges_ = viewport_->changeCount(); viewDirty_ = false; forcedDirty_ = true;
  updateTitle(); syncPanels(); if (syncDisplay_) syncDisplay_();
  files::closeFileWindow();
  ui::toast(tr("Restored unsaved work from %1").arg(when));
}

// ⟳ Reload: the program restarts and opens the same project — saved or not (unsaved work kept, Ctrl+S writes the same file)
void MainWindow::reloadApp() {
  viewport_->beforeFrame();
  SaveExtras x; x.camera = viewport_->cameraJson(); x.cocoons = viewport_->hair().cocoonSystem();
  x.convChecked = convChecked(scene_, viewport_->hair(), viewport_->selection().items);
  if (!writeBytes(QDir(files::appDataDir()).filePath("reload.hairmesh"), projectJson(fileScene(), x))) { ui::toast(tr("Could not keep the project for the reload")); return; }
  QSettings st; st.setValue("reload/name", displayName()); st.setValue("reload/path", scene_.path); st.setValue("reload/unsaved", dirty()); st.setValue("reload/time", QDateTime::currentMSecsSinceEpoch());
  st.setValue("reload/ach", extras::earned());              // (the achievements of this window go on over the reload)
  st.setValue("reload/room", scene_.room);                   // (and the room shown)
  st.setValue("main/geometry", saveGeometry());
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
    if (!dirty() || viewport_->changeCount() == asLastChange_) { asLast_ = now; return; }   // (nothing unsaved, or nothing new since the last copy)
    autosaveNow(false);
  });
  asLast_ = QDateTime::currentMSecsSinceEpoch(); as->start();
  auto* rc = new QTimer(this); rc->setInterval(60000); connect(rc, &QTimer::timeout, this, [this] { saveRecovery(); }); rc->start();
  extras::startChimera(tools_, dpTool_);                      // (one launch in five she visits)
  if (projectGiven) return;                                   // (a project opened from its file: nothing else to show)
  // after ⟳ Reload: straight back into that project
  QSettings st; QString rf = QDir(files::appDataDir()).filePath("reload.hairmesh");
  if (QFile::exists(rf) && QDateTime::currentMSecsSinceEpoch() - st.value("reload/time").toLongLong() < 10 * 60000) {
    QString name = st.value("reload/name").toString(), path = st.value("reload/path").toString(), room = st.value("reload/room").toString(); bool unsaved = st.value("reload/unsaved").toBool();
    extras::restore(st.value("reload/ach").toStringList());
    QFile f(rf);
    if (f.open(QIODevice::ReadOnly)) {
      LoadResult r = loadProjectData(f.readAll(), path.isEmpty() ? rf : path, scene_); f.close();
      QFile::remove(rf); st.remove("reload");
      if (r.ok) {
        scene_.path = path; projName_ = name.isEmpty() ? "hair-project.hairmesh" : name; looks::projectTexDir(scene_);
        rebuildOutliner(); viewport_->sceneChanged(true); if (board_) board_->reload();
        savedChanges_ = viewport_->changeCount(); viewDirty_ = false; forcedDirty_ = unsaved;
        updateTitle(); syncPanels(); if (syncDisplay_) syncDisplay_();
        if (room == "topo") { viewport_->switchRoom(room); applyRoom(scene_.room); }
        return;
      }
    }
  }
  QFile::remove(rf);
  newProject(false);
  // the very first start (nothing remembered yet) — the Examples; later — Recent projects; the recovery offer over it
  bool first = !st.value("started").toBool() && files::recentList().empty(); st.setValue("started", true);
  openFiles(first ? "examples" : "recent");
  QTimer::singleShot(300, this, [this] { offerRecovery(); });
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
  if (!dropOverlay_) { auto* o = new DropOverlay(centralWidget(), this); o->dropped = [this](const QStringList& f, const QString& z) { importDropped(f, z); }; dropOverlay_ = o; }
  static_cast<DropOverlay*>(dropOverlay_)->cover();
}
void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (!e->mimeData()->hasUrls()) return;
  bool onlyImages = true; for (const QUrl& u : e->mimeData()->urls()) if (!QRegularExpression("\\.(png|jpe?g|webp|bmp|gif|avif)$", QRegularExpression::CaseInsensitiveOption).match(u.toLocalFile()).hasMatch()) onlyImages = false;
  if (onlyImages) return;                                     // (pictures: the reference board)
  e->acceptProposedAction(); showDropOverlay();
}
void MainWindow::importDropped(const QStringList& fs, const QString& zone) {
  if (zone.isEmpty()) { ui::toast(tr("Drop the file onto one of the boxes")); return; }
  QStringList json, models; for (const QString& f : fs) { if (isProj(f)) json << f; else if (isModel(f)) models << f; }
  if (zone == "open") {
    if (json.isEmpty()) { ui::toast(tr("A project is a .hairmesh (or an old .json) file")); return; }
    if (!unsavedOk()) return;
    forget_ = true; bool ok = openProject(json[0]); forget_ = false; if (ok) files::closeFileWindow();
  } else if (zone == "migrate") {
    if (json.isEmpty()) { ui::toast(tr("A project is a .hairmesh (or an old .json) file")); return; }
    bool ok = false; for (const QString& f : json) ok = migrateFile(f, QString(), true) || ok;
    if (ok) files::closeFileWindow();
  } else {
    if (models.isEmpty()) { ui::toast(tr("Models: .obj, .glb, .gltf or .fbx")); return; }
    files::closeFileWindow();
    openImportWindow(viewport_, &scene_, fs, zone == "head");
    rebuildOutliner();
  }
}

}  // namespace hm
