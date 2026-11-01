#include "app/ExportPanels.h"
#include "app/ImportWindow.h"
#include "io/Export.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QGridLayout>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>

namespace hm {

namespace {

const QStringList FORMATS = {"obj", "fbx", "gltf", "ply", "stl"};
bool fbxGroups() { return QSettings().value("export/fbxGroups", false).toBool(); }
struct Bridge { const char* key; const char* label; int port; const char* file; };
const Bridge BRIDGES[] = {{"blender", "Blender", 8790, "hmc_bridge_blender.py"}, {"maya", "Maya", 8791, "hmc_bridge_maya.py"}, {"folder", "Folder", 0, ""}};
const Bridge* bridgeOf(const QString& k) { for (auto& b : BRIDGES) if (k == b.key) return &b; return nullptr; }
std::map<QString, std::pair<bool, QString>> g_status;           // a live bridge's last test / send: ok, message

QString st(const QString& k, const QString& def) { return QSettings().value(k, def).toString(); }
int bridgePort(const QString& t) { const Bridge* b = bridgeOf(t); return QSettings().value("bridge/port_" + t, b ? b->port : 0).toInt(); }

QFrame* popup(QPushButton* anchor) {
  auto* f = new QFrame(anchor->window(), Qt::Popup); f->setAttribute(Qt::WA_DeleteOnClose); f->setObjectName("xpBar");
  f->setStyleSheet("#xpBar{background:#262626;border:1px solid #555;border-radius:6px} QLabel[lbl=\"true\"]{color:#aaa;min-width:70px}");
  return f;
}
void place(QFrame* f, QPushButton* anchor) { f->adjustSize(); QPoint p = anchor->mapToGlobal(QPoint(0, anchor->height() + 4)); f->move(p); f->show(); }
QWidget* row(const QString& lab, std::initializer_list<QWidget*> ws) {
  auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(6);
  auto* l = new QLabel(lab); l->setProperty("lbl", true); h->addWidget(l); for (QWidget* x : ws) h->addWidget(x, 1); return w;
}
QLabel* hint(const QString& t) { auto* l = new QLabel(t); l->setWordWrap(true); l->setProperty("mut", true); l->setTextFormat(Qt::RichText); l->setMaximumWidth(380); return l; }
QComboBox* fmtCombo(const QString& cur) { auto* c = new QComboBox; for (auto& f : FORMATS) c->addItem(exportLabel(f), f); c->setCurrentIndex(std::max(0, int(FORMATS.indexOf(cur)))); return c; }
QComboBox* whatCombo(const QString& cur) { auto* c = new QComboBox; c->addItem(QObject::tr("Selected (+ in selected groups)"), "sel"); c->addItem(QObject::tr("All meshes"), "all"); c->setCurrentIndex(cur == "all" ? 1 : 0); return c; }
QDoubleSpinBox* scaleBox(double v) { auto* s = new QDoubleSpinBox; s->setDecimals(4); s->setRange(0.0001, 100000); s->setValue(v > 0 ? v : 1); return s; }

void noMeshes(const QString& what, const QString& verb) {
  ui::toast(what == "all" ? QObject::tr("No meshes yet — convert the hair first (Convert to geometry)") : QObject::tr("Select geometry (or a group containing geometry) to %1 — or choose Meshes: All").arg(verb));
}
bool writeFile(const QString& path, const QByteArray& data, QString* err) {
  QSaveFile f(path); if (!f.open(QIODevice::WriteOnly)) { if (err) *err = f.errorString(); return false; }
  f.write(data); if (!f.commit()) { if (err) *err = f.errorString(); return false; } return true;
}

// a live bridge's call: plain-text JSON POST to 127.0.0.1; not answering → a clear message
QJsonObject liveCall(const QString& t, const QString& cmd, const QByteArray& body, QString* err) {
  QNetworkAccessManager nam; QNetworkRequest rq(QUrl(QString("http://127.0.0.1:%1/%2").arg(bridgePort(t)).arg(cmd)));
  rq.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain"); rq.setTransferTimeout(cmd == "ping" ? 3000 : 60000);
  QNetworkReply* r = nam.post(rq, body.isEmpty() ? QByteArray("{}") : body);
  QEventLoop loop; QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
  const Bridge* b = bridgeOf(t);
  if (r->error() == QNetworkReply::ConnectionRefusedError || r->error() == QNetworkReply::TimeoutError || r->error() == QNetworkReply::OperationCanceledError || r->error() == QNetworkReply::HostNotFoundError) {
    *err = QObject::tr("%1 doesn't answer on port %2 — is it running with the bridge %3 on?").arg(b->label).arg(bridgePort(t)).arg(t == "blender" ? QObject::tr("add-on") : QObject::tr("script"));
    r->deleteLater(); return {};
  }
  QByteArray data = r->readAll(); int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); r->deleteLater();
  QJsonObject j = QJsonDocument::fromJson(data).object();
  if (j.isEmpty() && code >= 400) { *err = QString("HTTP %1").arg(code); return {}; }
  if (j.contains("error")) { *err = j.value("error").toString(); return {}; }
  if (code >= 400) { *err = QString("HTTP %1").arg(code); return {}; }
  return j;
}

}  // namespace

bool bridgeReady() {
  QString t = st("bridge/target", "");
  return t == "folder" ? !st("bridge/dir", "").isEmpty() : (t == "blender" || t == "maya");
}
QString sendLabel() { const Bridge* b = bridgeOf(st("bridge/target", "")); return b ? QString("⇪ %1").arg(b->label) : QString("⇪ Send"); }
QString sendTip() {
  QString t = st("bridge/target", ""); const Bridge* b = bridgeOf(t); if (!b) return {};
  QString what = st("bridge/what", "sel") == "all" ? QObject::tr("meshes (all)") : QObject::tr("selected meshes");
  QString to = t == "folder" ? QObject::tr("the folder %1 (%2.%3)").arg(st("bridge/dir", "")).arg(st("bridge/file", "hair")).arg(st("bridge/fmt", "obj")) : QString("%1 (port %2)").arg(b->label).arg(bridgePort(t));
  return QObject::tr("Send the %1 to %2 — Ctrl+Shift+E. Set up in 🔗 Bridge").arg(what, to);
}

// the export window (as the import window): in the middle of the screen; the file's path at the top (typed, pasted or
// picked — its extension is the format), the meshes (each ticked in or out), the scale and the normals type: as Maya
// keeps them (per corner, split at the hard edges) or as 3ds Max (smoothing groups)
namespace {
class ExportDialog : public QDialog {
public:
  ExportDialog(QWidget* parent, Viewport* vp, Scene* sc) : QDialog(parent), vp_(vp), sc_(sc) {
    setObjectName("ioWin"); setStyleSheet(ioWindowStyle()); setWindowTitle(tr("Export")); setMinimumSize(460, 340);
    auto* v = new QVBoxLayout(this); v->setContentsMargins(10, 8, 10, 10); v->setSpacing(6);
    QSettings s; const QString ft0 = st("export/fmt", "obj");
    QString dir = s.value("export/dir", s.value("lastDir").toString()).toString();
    if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    // ---- the path ----
    auto* pr = new QHBoxLayout; pr->setSpacing(4);
    auto* pl = new QLabel(tr("Path")); pl->setObjectName("rowName"); pr->addWidget(pl);
    path_ = new QLineEdit(QDir::toNativeSeparators(QDir(dir).filePath(st("export/name", "hair") + "." + ft0)));
    path_->setToolTip(tr("Where the file is written — typed, pasted or picked; its extension is the format"));
    pr->addWidget(path_, 1);
    auto* br = new QPushButton(tr("Browse…")); pr->addWidget(br); v->addLayout(pr);
    // ---- the settings ----
    auto* g = new QGridLayout; g->setHorizontalSpacing(8); g->setVerticalSpacing(4); int r = 0;
    auto name = [&](const QString& t) { auto* l = new QLabel(t); l->setObjectName("rowName"); g->addWidget(l, r, 0); };
    name(tr("Format")); fmt_ = fmtCombo(ft0); g->addWidget(fmt_, r++, 1);
    name(tr("Meshes")); what_ = whatCombo(st("export/what", "sel")); g->addWidget(what_, r++, 1);
    name(tr("Scale")); scale_ = scaleBox(s.value("export/scale", 1.0).toDouble()); g->addWidget(scale_, r++, 1);
    normL_ = new QLabel(tr("Normals")); normL_->setObjectName("rowName"); g->addWidget(normL_, r, 0);
    auto* nb = new QWidget; auto* nh = new QHBoxLayout(nb); nh->setContentsMargins(0, 0, 0, 0); nh->setSpacing(12);
    maya_ = new QRadioButton(tr("Maya — normals (hard / soft edges)")); max_ = new QRadioButton(tr("3ds Max — smoothing groups"));
    maya_->setToolTip(tr("A normal per polygon corner, split at the hard edges — how Maya, Blender, ZBrush keep them"));
    max_->setToolTip(tr("Smoothing groups: the polygons the soft edges join share a group, a hard edge parts them — how 3ds Max (and Cinema 4D) keep them"));
    (fbxGroups() ? max_ : maya_)->setChecked(true); nh->addWidget(maya_); nh->addWidget(max_); nh->addStretch(1); g->addWidget(nb, r++, 1);
    g->setColumnStretch(1, 1); v->addLayout(g);
    // ---- the meshes: those written into the one file (left) and those each in a file of its own (right — folders in
    // it are folders on the disk, beside the file); dragged between them, ticked in or out ----
    auto* lists = new QHBoxLayout; lists->setSpacing(6);
    auto mkTree = [&](const QString& title, const QString& tip, QTreeWidget*& t) {
      auto* box = new QVBoxLayout; box->setSpacing(3); auto* l = new QLabel(title); l->setObjectName("rowName"); l->setToolTip(tip); box->addWidget(l);
      t = new QTreeWidget; t->setHeaderHidden(true); t->setToolTip(tip); t->setSelectionMode(QAbstractItemView::ExtendedSelection);
      t->setDragDropMode(QAbstractItemView::DragDrop); t->setDefaultDropAction(Qt::MoveAction); t->setDragEnabled(true); t->setAcceptDrops(true); t->setDropIndicatorShown(true);
      box->addWidget(t, 1); lists->addLayout(box, 1);
      connect(t, &QTreeWidget::itemChanged, this, [this] { count(); });
      connect(t->model(), &QAbstractItemModel::rowsInserted, this, [this] { QTimer::singleShot(0, this, [this] { tidy(); count(); }); });
      return box; };
    mkTree(tr("One file"), tr("These meshes go into the one file of the path above"), one_);
    auto* sepBox = mkTree(tr("Separate files"), tr("Each of these meshes a file of its own, named after it, beside the file above; a folder here — a folder on the disk"), sep_);
    auto* sb = new QHBoxLayout; sb->setSpacing(4);
    auto* nf = new QPushButton(tr("New folder")); nf->setToolTip(tr("A folder among the separate files (into the folder picked, if one is)"));
    auto* toSep = new QPushButton(tr("→")); toSep->setToolTip(tr("The meshes picked in One file — each a file of its own"));
    auto* toOne = new QPushButton(tr("←")); toOne->setToolTip(tr("The meshes picked among the separate files — into the one file"));
    sb->addWidget(toOne); sb->addWidget(toSep); sb->addStretch(1); sb->addWidget(nf); sepBox->addLayout(sb);
    v->addLayout(lists, 1);
    connect(nf, &QPushButton::clicked, this, [this] { auto* f = folderItem(tr("Folder")); QTreeWidgetItem* at = sep_->currentItem();
      if (at && at->data(0, Qt::UserRole).toInt() < 0) at->addChild(f); else sep_->addTopLevelItem(f); f->setExpanded(true); sep_->editItem(f); count(); });
    connect(toSep, &QPushButton::clicked, this, [this] { for (auto* it : one_->selectedItems()) sep_->addTopLevelItem(one_->takeTopLevelItem(one_->indexOfTopLevelItem(it))); count(); });
    connect(toOne, &QPushButton::clicked, this, [this] { for (auto* it : sep_->selectedItems()) { if (it->data(0, Qt::UserRole).toInt() < 0) continue;
        QTreeWidgetItem* p = it->parent(); QTreeWidgetItem* x = p ? p->takeChild(p->indexOfChild(it)) : sep_->takeTopLevelItem(sep_->indexOfTopLevelItem(it)); one_->addTopLevelItem(x); } count(); });
    hint_ = new QLabel; hint_->setObjectName("hint"); hint_->setWordWrap(true); v->addWidget(hint_);
    auto* bar = new QHBoxLayout; bar->addStretch(1);
    auto* cn = new QPushButton(tr("Cancel")); go_ = new QPushButton(tr("Export")); go_->setProperty("primary", true); go_->setDefault(true);
    bar->addWidget(cn); bar->addWidget(go_); v->addLayout(bar);
    connect(cn, &QPushButton::clicked, this, &QDialog::reject);
    connect(go_, &QPushButton::clicked, this, [this] { run(); });
    connect(br, &QPushButton::clicked, this, [this] {
      const QString ft = fmt_->currentData().toString();
      QString p = QFileDialog::getSaveFileName(this, tr("Export"), path_->text(), exportLabel(ft) + QString(" (*.%1)").arg(ft));
      if (!p.isEmpty()) { path_->setText(QDir::toNativeSeparators(p)); fromPath(); } });
    connect(fmt_, &QComboBox::currentIndexChanged, this, [this] {      // (the format → the path's extension)
      const QString ft = fmt_->currentData().toString(); QSettings().setValue("export/fmt", ft);
      QFileInfo fi(path_->text()); if (!path_->text().trimmed().isEmpty()) path_->setText(QDir::toNativeSeparators(QDir(fi.path()).filePath(fi.completeBaseName() + "." + ft)));
      sync(); });
    connect(path_, &QLineEdit::editingFinished, this, [this] { fromPath(); });
    connect(what_, &QComboBox::currentIndexChanged, this, [this] { QSettings().setValue("export/what", what_->currentData()); fill(); });
    connect(maya_, &QRadioButton::toggled, this, [this] { QSettings().setValue("export/fbxGroups", max_->isChecked()); });
    fill(); sync();
    resize(QSettings().value("exportWindow/size", QSize(560, 460)).toSize());
  }
  ~ExportDialog() override { QSettings().setValue("exportWindow/size", size()); }

private:
  // a path typed with a known extension: that format
  void fromPath() {
    QString p = path_->text().trimmed(); const QChar q('"');
    if (p.size() > 1 && p.startsWith(q) && p.endsWith(q)) { p = p.mid(1, p.size() - 2); path_->setText(p); }
    const int i = fmt_->findData(QFileInfo(p).suffix().toLower()); if (i >= 0 && i != fmt_->currentIndex()) fmt_->setCurrentIndex(i);
  }
  void sync() {
    const QString ft = fmt_->currentData().toString(); const bool n = ft == "obj" || ft == "fbx";
    normL_->setEnabled(n); maya_->setEnabled(n); max_->setEnabled(n);
    hint_->setText((sc_->room == "topo" ? tr("OBJ / PLY / FBX keep the quads; glTF / STL are triangles. OBJ and FBX carry the normals as chosen and the UVs (FBX: the creases too).")
                                        : tr("OBJ / PLY / FBX keep the quads; glTF / STL are triangles. Convert the hair first (Convert to geometry)."))
                   + (n ? QString() : tr(" The normals type: OBJ and FBX only.")));
  }
  QTreeWidgetItem* folderItem(const QString& name) {
    auto* f = new QTreeWidgetItem(QStringList{name}); f->setData(0, Qt::UserRole, -1);
    f->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled); return f; }
  QTreeWidgetItem* meshItem(size_t i) {
    const PolyMesh& m = *meshes_[i].mesh;
    QStringList has; if (!m.cornerUV.empty()) has << tr("UVs"); if (!m.hardEdges.empty()) has << tr("%1 hard edges").arg(m.hardEdges.size()); if (!m.creases.empty()) has << tr("creases");
    auto* it = new QTreeWidgetItem(QStringList{meshes_[i].name + "   " + tr("%1 polygons").arg(QLocale().toString(m.faceCount())) + (has.isEmpty() ? QString() : " · " + has.join(" · "))});
    it->setData(0, Qt::UserRole, int(i)); it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsUserCheckable); it->setCheckState(0, Qt::Checked); return it; }
  // the meshes into the two lists: those last written separately where they were (by name, in their folders), the rest
  // into the one file
  void fill() {
    one_->clear(); sep_->clear();
    meshes_ = exportMeshes(*sc_, vp_->selection().items, what_->currentData().toString());
    std::map<QString, QString> wasSep; for (const QString& e : QSettings().value("export/separate").toStringList()) { const int b = e.lastIndexOf('|'); if (b >= 0) wasSep[e.mid(b + 1)] = e.left(b); }
    for (size_t i = 0; i < meshes_.size(); i++) {
      auto w = wasSep.find(meshes_[i].name); if (w == wasSep.end()) { one_->addTopLevelItem(meshItem(i)); continue; }
      QTreeWidgetItem* at = nullptr;
      for (const QString& part : w->second.split('/', Qt::SkipEmptyParts)) { QTreeWidgetItem* found = nullptr; const int n = at ? at->childCount() : sep_->topLevelItemCount();
        for (int k = 0; k < n && !found; k++) { QTreeWidgetItem* c = at ? at->child(k) : sep_->topLevelItem(k); if (c->data(0, Qt::UserRole).toInt() < 0 && c->text(0) == part) found = c; }
        if (!found) { found = folderItem(part); if (at) at->addChild(found); else sep_->addTopLevelItem(found); found->setExpanded(true); } at = found; }
      if (at) at->addChild(meshItem(i)); else sep_->addTopLevelItem(meshItem(i)); }
    if (meshes_.empty()) { auto* l = new QTreeWidgetItem(QStringList{what_->currentData() == "all" ? tr("No meshes yet") : tr("Nothing selected — select meshes (or a group), or choose All meshes")}); l->setFlags(Qt::NoItemFlags); one_->addTopLevelItem(l); }
    count();
  }
  // after a drop: the one file's list flat — a folder dropped there given up, its meshes kept; a mesh dropped onto a
  // mesh among the separate files put beside it
  void tidy() {
    for (int k = one_->topLevelItemCount() - 1; k >= 0; k--) { QTreeWidgetItem* t = one_->topLevelItem(k); if (t->data(0, Qt::UserRole).toInt() >= 0) continue;
      std::vector<QTreeWidgetItem*> ms; std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* x) { for (int c = 0; c < x->childCount(); c++) { QTreeWidgetItem* y = x->child(c); if (y->data(0, Qt::UserRole).toInt() >= 0) ms.push_back(y); else walk(y); } };
      walk(t); for (auto* x : ms) { x->parent()->removeChild(x); one_->addTopLevelItem(x); } delete one_->takeTopLevelItem(one_->indexOfTopLevelItem(t)); }
  }
  // the separate meshes ticked: each with its folder's path (a/b)
  void sepList(std::vector<std::pair<QString, int>>& out, bool all) const {
    std::function<void(QTreeWidgetItem*, const QString&)> walk = [&](QTreeWidgetItem* x, const QString& dir) {
      const int i = x->data(0, Qt::UserRole).toInt();
      if (i >= 0) { if (all || x->checkState(0) == Qt::Checked) out.push_back({dir, i}); return; }
      const QString d = dir.isEmpty() ? x->text(0) : dir + "/" + x->text(0); for (int c = 0; c < x->childCount(); c++) walk(x->child(c), d); };
    for (int k = 0; k < sep_->topLevelItemCount(); k++) walk(sep_->topLevelItem(k), {});
  }
  std::vector<int> oneList() const { std::vector<int> o; for (int k = 0; k < one_->topLevelItemCount(); k++) { QTreeWidgetItem* t = one_->topLevelItem(k); const int i = t->data(0, Qt::UserRole).toInt(); if (i >= 0 && t->checkState(0) == Qt::Checked) o.push_back(i); } return o; }
  void count() {
    std::vector<std::pair<QString, int>> s; sepList(s, false); const size_t n1 = oneList().size(), n = n1 + s.size(); go_->setEnabled(n > 0);
    go_->setText(s.empty() ? (n1 > 1 ? tr("Export %1 meshes").arg(n1) : tr("Export")) : tr("Export %1 file%2").arg((n1 ? 1 : 0) + s.size()).arg((n1 ? 1 : 0) + s.size() == 1 ? "" : "s")); }
  void run() {
    fromPath();
    QString path = path_->text().trimmed(); const QString ft = fmt_->currentData().toString();
    if (path.isEmpty()) { ui::toast(tr("Type the path (or Browse…)")); return; }
    if (QFileInfo(path).suffix().toLower() != ft) path += "." + ft;
    std::vector<ExportMesh> list; for (int i : oneList()) list.push_back(meshes_[size_t(i)]);
    std::vector<std::pair<QString, int>> seps; sepList(seps, false);
    if (list.empty() && seps.empty()) return;
    if (!QFileInfo(path).dir().exists()) { ui::toast(tr("No such folder: %1").arg(QFileInfo(path).path())); return; }
    QSettings s; s.setValue("export/dir", QFileInfo(path).absolutePath()); s.setValue("export/name", QFileInfo(path).completeBaseName()); s.setValue("export/scale", scale_->value());
    { std::vector<std::pair<QString, int>> all; sepList(all, true); QStringList keep; for (auto& [d, i] : all) keep << d + "|" + meshes_[size_t(i)].name; s.setValue("export/separate", keep); }   // (where each separate one was: the next time)
    QString err; int files = 0;
    if (!list.empty()) { if (!writeFile(path, exportText(ft, list, scale_->value(), max_->isChecked()), &err)) { ui::toast(tr("Export failed: %1").arg(err)); return; } files++; }
    // (each separate one: its name as the file's, in its folders beside the file)
    static const QRegularExpression bad(R"([\\/:*?"<>|])");
    for (auto& [d, i] : seps) { QDir dir = QFileInfo(path).dir(); if (!d.isEmpty()) { QString clean; for (const QString& p : d.split('/')) clean += (clean.isEmpty() ? "" : "/") + QString(p).replace(bad, "_"); if (!dir.mkpath(clean)) { ui::toast(tr("Could not make the folder %1").arg(clean)); return; } dir = QDir(dir.filePath(clean)); }
      const QString f = dir.filePath(QString(meshes_[size_t(i)].name).replace(bad, "_") + "." + ft);
      if (!writeFile(f, exportText(ft, {meshes_[size_t(i)]}, scale_->value(), max_->isChecked()), &err)) { ui::toast(tr("Export failed: %1").arg(err)); return; } files++; }
    ui::toast(seps.empty() ? tr("Exported %1 mesh%2 → %3").arg(list.size()).arg(list.size() > 1 ? "es" : "").arg(QFileInfo(path).fileName())
                           : tr("Exported %1 file%2 into %3").arg(files).arg(files == 1 ? "" : "s").arg(QDir::toNativeSeparators(QFileInfo(path).absolutePath())));
    accept();
  }

  Viewport* vp_; Scene* sc_;
  QLineEdit* path_ = nullptr; QComboBox *fmt_ = nullptr, *what_ = nullptr; QDoubleSpinBox* scale_ = nullptr; QLabel *normL_ = nullptr, *hint_ = nullptr;
  QRadioButton *maya_ = nullptr, *max_ = nullptr; QTreeWidget *one_ = nullptr, *sep_ = nullptr; QPushButton* go_ = nullptr;
  std::vector<ExportMesh> meshes_;
};
}  // namespace

void openExportPanel(QPushButton* anchor, Viewport* vp, Scene* sc) {
  QWidget* mw = anchor ? anchor->window() : nullptr;
  ExportDialog d(mw, vp, sc);
  QScreen* scr = mw ? mw->screen() : QGuiApplication::primaryScreen();
  if (scr) { QRect a = scr->availableGeometry(); QSize s = d.size().boundedTo(a.size()); d.resize(s); d.move(a.center() - QPoint(s.width() / 2, s.height() / 2)); }
  d.exec();
}

void bridgeSend(QWidget* parent, Viewport* vp, Scene* sc, const std::function<void()>& openSetup) {
  if (!bridgeReady()) { if (openSetup) openSetup(); return; }
  QString t = st("bridge/target", ""), what = st("bridge/what", "sel"); double scale = QSettings().value("bridge/scale", 1.0).toDouble();
  auto list = exportMeshes(*sc, vp->selection().items, what); if (list.empty()) { noMeshes(what, QObject::tr("send")); return; }
  Q_UNUSED(parent);
  if (t == "folder") {
    QString fmt = st("bridge/fmt", "obj"), name = st("bridge/file", "hair"); name.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_"); name += "." + fmt;
    QString err; if (!writeFile(QDir(st("bridge/dir", "")).filePath(name), exportText(fmt, list, scale, fbxGroups()), &err)) { ui::toast(QObject::tr("Send failed: %1").arg(err)); return; }
    ui::toast(QObject::tr("Sent %1 mesh%2 → %3").arg(list.size()).arg(list.size() > 1 ? "es" : "").arg(name));
    return;
  }
  QString err; QApplication::setOverrideCursor(Qt::WaitCursor);
  QJsonObject r = liveCall(t, "send", bridgePayload(list, scale, QSettings().value("bridge/replace", true).toBool()), &err);
  QApplication::restoreOverrideCursor();
  const Bridge* b = bridgeOf(t);
  if (!err.isEmpty()) { g_status[t] = {false, err}; ui::toast(QObject::tr("Send failed: %1").arg(err)); return; }
  QStringList names; for (const QJsonValue& x : r.value("objects").toArray()) names << x.toString();
  g_status[t] = {true, QObject::tr("%1: sent %2").arg(r.value("program").toString(b->label)).arg(names.size())};
  ui::toast(QObject::tr("Sent to %1: %2").arg(b->label, names.join(", ")));
}

void openBridgePanel(QPushButton* anchor, Viewport* vp, Scene* sc, const std::function<void()>& changed) {
  auto* f = popup(anchor); auto* v = new QVBoxLayout(f); v->setContentsMargins(10, 8, 10, 8); v->setSpacing(5);
  QString t = st("bridge/target", "");
  auto* tc = new QComboBox; tc->addItem(QObject::tr("— choose —"), "");
  for (auto& b : BRIDGES) tc->addItem(QString(b.key) == "folder" ? QObject::tr("Folder (Unreal, Unity, Godot, ZBrush…)") : QString("%1 (live)").arg(b.label), b.key);
  tc->setCurrentIndex(std::max(0, tc->findData(t)));
  v->addWidget(row(QObject::tr("Program"), {tc}));
  auto reopen = [f, anchor, vp, sc, changed] { f->close(); QTimer::singleShot(0, anchor, [anchor, vp, sc, changed] { openBridgePanel(anchor, vp, sc, changed); }); };
  QObject::connect(tc, &QComboBox::currentIndexChanged, f, [tc, changed, reopen] { QSettings().setValue("bridge/target", tc->currentData()); if (changed) changed(); reopen(); });
  if (t == "blender" || t == "maya") {
    const Bridge* b = bridgeOf(t);
    auto* port = new QSpinBox; port->setRange(1024, 65535); port->setValue(bridgePort(t));
    auto* test = new QPushButton(QObject::tr("Test"));
    v->addWidget(row(QObject::tr("Port"), {port, test}));
    auto* save = new QPushButton(QObject::tr("⭳ Save %1…").arg(b->file));
    v->addWidget(row(t == "blender" ? QObject::tr("Add-on") : QObject::tr("Script"), {save}));
    v->addWidget(hint(t == "blender" ? QObject::tr("<b>Blender:</b> Edit → Preferences → Add-ons → ▾ → Install from Disk… → this file, tick it. It listens while Blender runs (port in the add-on's preferences); the meshes go into the <b>HairMesh</b> collection.")
                                     : QObject::tr("<b>Maya</b> (2018+): drag this file into the viewport — it installs itself and starts with Maya from then on. The meshes go into the <b>HairMesh</b> group; the port is set in the script (PORT).")));
    auto* repl = new QCheckBox(QObject::tr("Replace objects of the same name (update in place, materials kept)")); repl->setChecked(QSettings().value("bridge/replace", true).toBool()); v->addWidget(repl);
    auto it = g_status.find(t);
    auto* stl = new QLabel(it == g_status.end() ? QObject::tr("Not tested yet") : it->second.second); stl->setWordWrap(true); stl->setMaximumWidth(380);
    stl->setStyleSheet(it == g_status.end() ? "color:#8a8a8a" : it->second.first ? "color:#7fe07f" : "color:#e07060"); v->addWidget(stl);
    QObject::connect(port, &QSpinBox::editingFinished, f, [port, t, changed] { QSettings().setValue("bridge/port_" + t, port->value()); g_status.erase(t); if (changed) changed(); });
    QObject::connect(test, &QPushButton::clicked, f, [t, stl, b] {
      QString err; QApplication::setOverrideCursor(Qt::WaitCursor); QJsonObject r = liveCall(t, "ping", {}, &err); QApplication::restoreOverrideCursor();
      g_status[t] = err.isEmpty() ? std::make_pair(true, QObject::tr("Connected: %1 %2").arg(r.value("program").toString(b->label), r.value("version").toVariant().toString()).trimmed()) : std::make_pair(false, err);
      stl->setText(g_status[t].second); stl->setStyleSheet(g_status[t].first ? "color:#7fe07f" : "color:#e07060");
    });
    QObject::connect(save, &QPushButton::clicked, f, [f, anchor, b, t] {
      QFile src(QString(":/bridge/%1").arg(b->file)); if (!src.open(QIODevice::ReadOnly)) { ui::toast(QObject::tr("The script is missing from the program")); return; }
      QByteArray code = src.readAll(); f->close();
      QSettings s; QString path = QFileDialog::getSaveFileName(anchor->window(), QObject::tr("Save the bridge script"), QDir(s.value("bridge/scriptDir").toString()).filePath(b->file), QObject::tr("Python script (*.py)"));
      if (path.isEmpty()) return; s.setValue("bridge/scriptDir", QFileInfo(path).absolutePath());
      QString err; if (!writeFile(path, code, &err)) { ui::toast(QObject::tr("Could not save: %1").arg(err)); return; }
      ui::toast(QObject::tr("Saved %1 — %2").arg(QFileInfo(path).fileName(), t == "blender" ? QObject::tr("install it in Blender: Preferences → Add-ons → Install from Disk") : QObject::tr("drag it into Maya's viewport")));
    });
    QObject::connect(repl, &QCheckBox::toggled, f, [](bool x) { QSettings().setValue("bridge/replace", x); });
  } else if (t == "folder") {
    QString dir = st("bridge/dir", "");
    auto* path = new QLabel(dir.isEmpty() ? QObject::tr("— not chosen —") : dir); path->setWordWrap(true); path->setMaximumWidth(260);
    auto* ch = new QPushButton(QObject::tr("Choose…"));
    v->addWidget(row(QObject::tr("Folder"), {path, ch}));
    auto* fmt = fmtCombo(st("bridge/fmt", "obj")); v->addWidget(row(QObject::tr("Format"), {fmt}));
    auto* fn = new QLineEdit(st("bridge/file", "hair")); auto* ext = new QLabel("." + st("bridge/fmt", "obj")); v->addWidget(row(QObject::tr("File name"), {fn, ext}));
    v->addWidget(hint(QObject::tr("Each send overwrites that file. <b>Unreal:</b> Editor Preferences → Loading &amp; Saving → Auto Reimport → <i>Auto Create Assets</i>, the folder inside Content. <b>Unity / Godot:</b> a folder inside the project. Others — import / reload that file.")));
    QObject::connect(ch, &QPushButton::clicked, f, [f, anchor, changed, reopen] {
      f->close(); QString d = QFileDialog::getExistingDirectory(anchor->window(), QObject::tr("Bridge folder"), st("bridge/dir", ""));
      if (d.isEmpty()) return; QSettings().setValue("bridge/dir", d); if (changed) changed(); reopen();
    });
    QObject::connect(fmt, &QComboBox::currentIndexChanged, f, [fmt, ext, changed] { QSettings().setValue("bridge/fmt", fmt->currentData()); ext->setText("." + fmt->currentData().toString()); if (changed) changed(); });
    QObject::connect(fn, &QLineEdit::editingFinished, f, [fn, changed] { QString n = fn->text().trimmed(); if (n.isEmpty()) n = "hair"; fn->setText(n); QSettings().setValue("bridge/file", n); if (changed) changed(); });
  } else v->addWidget(hint(QObject::tr("Choose where ⇪ Send puts the meshes. Once a bridge is set up, ⇪ Send in the top bar sends with one click (Ctrl+Shift+E).")));
  if (!t.isEmpty()) {
    auto* what = whatCombo(st("bridge/what", "sel")); v->addWidget(row(QObject::tr("Meshes"), {what}));
    auto* scl = scaleBox(QSettings().value("bridge/scale", 1.0).toDouble()); v->addWidget(row(QObject::tr("Scale"), {scl}));
    auto* go = new QPushButton(QObject::tr("⇪ Send now")); go->setEnabled(bridgeReady()); v->addWidget(go);
    QObject::connect(what, &QComboBox::currentIndexChanged, f, [what, changed] { QSettings().setValue("bridge/what", what->currentData()); if (changed) changed(); });
    QObject::connect(scl, &QDoubleSpinBox::valueChanged, f, [](double x) { QSettings().setValue("bridge/scale", x); });
    QObject::connect(go, &QPushButton::clicked, f, [f, anchor, vp, sc, reopen] { bridgeSend(anchor->window(), vp, sc, nullptr); if (st("bridge/target", "") != "folder") reopen(); else f->close(); });
  }
  place(f, anchor);
}

}  // namespace hm

namespace hm {
void exportWindowShot(Viewport* vp, Scene* sc, const QString& png) {
  QWidget* mw = nullptr; for (QWidget* w : QApplication::topLevelWidgets()) if (w->inherits("QMainWindow")) mw = w;
  ExportDialog d(mw, vp, sc); d.show(); QApplication::processEvents(); QApplication::processEvents();
  d.grab().save(png);
}
}  // namespace hm
