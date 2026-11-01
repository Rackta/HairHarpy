#include "app/ImportWindow.h"
#include "app/ImportMesh.h"
#include "scene/Primitives.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <functional>
#include <map>

namespace hm {

const char* ioWindowStyle() {
  return "#ioWin{background:#262626} #ioWin *{font-size:11px} QLabel{color:#cfcfcf} #rowName{color:#bdbdbd} #hint{color:#8a8a8a;font-size:10px}"
         " #fileCard{background:#2e2e2e;border:1px solid #3a3a3a;border-radius:4px} #fileName{color:#e8e8e8;font-weight:bold}"
         " #fileTag{color:#9ad;background:#20303a;border-radius:3px;padding:0 4px} #fileSum{color:#8a8a8a} #fileMiss{color:#e06060}"
         " QLineEdit{background:#1b1b1b;border:1px solid #3a3a3a;color:#e8e8e8;padding:2px 4px}"
         " QPushButton{padding:2px 8px;min-height:16px} QToolButton{border:none;color:#bbb}"
         " QScrollArea{border:none;background:transparent} #cards{background:transparent}";
}

namespace {

// a primitive's picture: its shape shaded from the top left
QIcon primIcon(const QString& t) {
  QPixmap pm(68, 68); pm.fill(Qt::transparent); QPainter g(&pm); g.setRenderHint(QPainter::Antialiasing); g.setPen(QPen(QColor(0x55, 0x55, 0x55), 1.2));
  QRadialGradient sh(QPointF(26, 22), 46); sh.setColorAt(0, QColor(0xf0, 0xf0, 0xf0)); sh.setColorAt(1, QColor(0x80, 0x80, 0x80)); g.setBrush(sh);
  QLinearGradient side(QPointF(14, 0), QPointF(54, 0)); side.setColorAt(0, QColor(0xd8, 0xd8, 0xd8)); side.setColorAt(1, QColor(0x86, 0x86, 0x86));
  const QColor top(0xe8, 0xe8, 0xe8);
  if (t == "sphere" || t == "psphere") { g.drawEllipse(QRectF(12, 12, 44, 44)); if (t == "psphere") { g.setBrush(Qt::NoBrush); g.drawEllipse(QRectF(12, 26, 44, 16)); g.drawEllipse(QRectF(26, 12, 16, 44)); } }
  else if (t == "cone") { g.setBrush(side); QPolygonF p{QPointF(34, 8), QPointF(56, 52), QPointF(12, 52)}; g.drawPolygon(p); g.drawEllipse(QRectF(12, 46, 44, 12)); }
  else if (t == "cyl" || t == "pcyl") { g.setBrush(side); g.drawRect(QRectF(14, 16, 40, 36)); g.drawEllipse(QRectF(14, 46, 40, 12)); g.setBrush(top); g.drawEllipse(QRectF(14, 10, 40, 12));
    if (t == "pcyl") { g.drawLine(QPointF(27, 22), QPointF(27, 57)); g.drawLine(QPointF(41, 22), QPointF(41, 57)); } }
  else if (t == "pcube") { g.setBrush(top); g.drawPolygon(QPolygonF{QPointF(14, 22), QPointF(34, 12), QPointF(54, 22), QPointF(34, 32)});
    g.setBrush(QColor(0xc8, 0xc8, 0xc8)); g.drawPolygon(QPolygonF{QPointF(14, 22), QPointF(34, 32), QPointF(34, 58), QPointF(14, 48)});
    g.setBrush(QColor(0x8e, 0x8e, 0x8e)); g.drawPolygon(QPolygonF{QPointF(34, 32), QPointF(54, 22), QPointF(54, 48), QPointF(34, 58)}); }
  else if (t == "pplane") { g.setBrush(top); g.drawPolygon(QPolygonF{QPointF(8, 38), QPointF(34, 24), QPointF(60, 38), QPointF(34, 52)}); }
  else if (t == "ring") { QPainterPath r; r.addEllipse(QRectF(8, 18, 52, 32)); r.addEllipse(QRectF(22, 27, 24, 14)); r.setFillRule(Qt::OddEvenFill); g.setBrush(sh); g.drawPath(r); }
  return QIcon(pm);
}

const QStringList MODELS = {"obj", "glb", "gltf", "fbx"};
const QStringList EXTRAS = {"mtl", "bin", "png", "jpg", "jpeg", "webp", "bmp", "gif", "tga", "avif"};
QString ext(const QString& f) { return QFileInfo(f).suffix().toLower(); }

// the paths in the field: separated by ; (or new lines), each maybe in quotes (Explorer's "Copy as path"), file:/// URLs
QStringList parsePaths(const QString& text) {
  QStringList out; static const QRegularExpression re("\"([^\"]+)\"|([^;|\\n\\r\"]+)");
  for (auto it = re.globalMatch(text); it.hasNext();) {
    auto m = it.next(); QString p = (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1)).trimmed();
    if (p.isEmpty()) continue;
    if (p.startsWith("file:", Qt::CaseInsensitive)) p = QUrl(p).toLocalFile();
    p = QDir::toNativeSeparators(p);
    QFileInfo fi(p);
    if (fi.isDir()) {                                    // (a folder: the models and their textures in it)
      for (const QFileInfo& f : QDir(p).entryInfoList(QDir::Files, QDir::Name))
        if (MODELS.contains(f.suffix().toLower()) || EXTRAS.contains(f.suffix().toLower())) if (!out.contains(QDir::toNativeSeparators(f.absoluteFilePath()))) out << QDir::toNativeSeparators(f.absoluteFilePath());
      continue;
    }
    if (!out.contains(p)) out << p;
  }
  return out;
}

struct Choice { const char* key; const char* label; const char* tip; bool TopoImport::* f; };
const Choice CHOICES[] = {
  {"groups", "Import polygroups", "Keep the file's polygroups (OBJ g / o, the meshes of a glTF / FBX file); off — one mesh, one polygroup", &TopoImport::groups},
  {"objects", "Split polygroups into separate meshes", "Every polygroup a mesh of its own, in a group named after the file", &TopoImport::objects},
  {"head", "Import as head", "The model is the head — the reference the hair is made on (a head there already: the model joins it)", &TopoImport::head},
  {"uvs", "Import UVs", "The file's UV layout (needed for the textures)", &TopoImport::uvs},
  {"textures", "Import textures", "The textures the file carries or refers to, and the images / .mtl picked with it", &TopoImport::textures},
  {"polypaint", "Import polypaint", "The colours painted on the vertices (ZBrush polypaint, vertex colours) — shown with the standard material", &TopoImport::polypaint},
  {"creases", "Import creases", "The edge creases the file keeps (subdivision)", &TopoImport::creases},
  {"hard", "Import hard edges",
   "The file's hard edges. A model from 3ds Max or Cinema 4D keeps smoothing groups / split normals instead of hard edges: "
   "the normals are worked out so that its hard edges are found", &TopoImport::hard},
  {"curves", "Import curves", "The file's curves (OBJ lines, FBX line / NURBS curves) — made this program's curves, anchors fitted along them", &TopoImport::curves},
  {"parts", "Split separate parts into polygroups", "Every separate (unconnected) part of the model a polygroup of its own — for a model of pieces without polygroups", &TopoImport::parts},
};

class ImportDialog : public QDialog {
public:
  ImportDialog(QWidget* parent, Viewport* vp, Scene* sc, const QStringList& files, bool head) : QDialog(parent), vp_(vp), sc_(sc), headDefault_(head) {
    topo_ = sc->room == "topo";
    setObjectName("ioWin"); setStyleSheet(ioWindowStyle()); setWindowTitle(tr("Import"));
    setMinimumSize(460, 320);
    auto* v = new QVBoxLayout(this); v->setContentsMargins(10, 8, 10, 10); v->setSpacing(6);
    // ---- the primitives: a click makes one (at the point the camera turns round) instead of importing ----
    { auto* pw = new QWidget; pw->setObjectName("prims"); pw->setStyleSheet("#prims{background:#2b2b2b;border:1px solid #3a3a3a;border-radius:4px} #prims QToolButton{color:#cfcfcf;padding:3px 2px} #prims QToolButton:hover{background:#3a3f4a;border-radius:4px}");
      auto* ph = new QHBoxLayout(pw); ph->setContentsMargins(4, 4, 4, 4); ph->setSpacing(2);
      for (const QString& t : prim::types()) {
        auto* b = new QToolButton; b->setText(prim::label(t)); b->setIcon(primIcon(t)); b->setIconSize(QSize(34, 34)); b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setToolTip(tr("%1 — made at the point the camera turns round; its settings in the mesh's panel, Convert to object there makes it a plain mesh").arg(prim::label(t)));
        b->setMinimumWidth(62); ph->addWidget(b, 1);
        connect(b, &QToolButton::clicked, this, [this, t] { reject(); Viewport* vp = vp_; QTimer::singleShot(0, vp, [vp, t] { vp->createPrimitive(t); }); }); }
      v->addWidget(pw); }
    // ---- the path ----
    auto* pr = new QHBoxLayout; pr->setSpacing(4);
    auto* pl = new QLabel(tr("Path")); pl->setObjectName("rowName"); pr->addWidget(pl);
    path_ = new QLineEdit; path_->setPlaceholderText(tr("Paste or type the path — several separated by ;  (a folder: its models and textures)"));
    path_->setToolTip(tr("What is imported: files (OBJ / GLB / GLTF / FBX, and their textures / .mtl / .bin) or a folder; several — separated by ;"));
    pr->addWidget(path_, 1);
    auto* br = new QPushButton(tr("Browse…")); br->setToolTip(tr("Pick the files (several at once, of different formats)")); pr->addWidget(br);
    v->addLayout(pr);
    // ---- the files ----
    auto* sa = new QScrollArea; sa->setWidgetResizable(true); sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    cards_ = new QWidget; cards_->setObjectName("cards"); list_ = new QVBoxLayout(cards_); list_->setContentsMargins(0, 0, 0, 0); list_->setSpacing(4); list_->addStretch(1);
    sa->setWidget(cards_); v->addWidget(sa, 1);
    hint_ = new QLabel; hint_->setObjectName("hint"); hint_->setWordWrap(true); v->addWidget(hint_);
    // ---- the buttons ----
    auto* bar = new QHBoxLayout; bar->addStretch(1);
    auto* cn = new QPushButton(tr("Cancel")); go_ = new QPushButton(tr("Import")); go_->setProperty("primary", true); go_->setDefault(true);
    bar->addWidget(cn); bar->addWidget(go_); v->addLayout(bar);
    connect(cn, &QPushButton::clicked, this, &QDialog::reject);
    connect(go_, &QPushButton::clicked, this, [this] { rebuild(); if (!models().isEmpty()) { keepChoices(); accept(); } });
    connect(br, &QPushButton::clicked, this, [this] { browse(); });
    auto* deb = new QTimer(this); deb->setSingleShot(true); deb->setInterval(250);
    connect(deb, &QTimer::timeout, this, [this] { rebuild(); });
    connect(path_, &QLineEdit::textEdited, deb, qOverload<>(&QTimer::start));
    connect(path_, &QLineEdit::returnPressed, this, [this] { rebuild(); });
    QStringList q; for (const QString& f : files) q << "\"" + QDir::toNativeSeparators(f) + "\"";
    path_->setText(q.join("; ")); rebuild();
    resize(QSettings().value("importWindow/size", QSize(600, 520)).toSize());
  }
  ~ImportDialog() override { QSettings().setValue("importWindow/size", size()); }

  // what to import: each model with its choices, the extras (textures / .mtl / .bin) going with every one
  QStringList models() const { QStringList m; for (const QString& f : paths_) if (MODELS.contains(ext(f)) && QFileInfo::exists(f)) m << f; return m; }
  QStringList extras() const { QStringList m; for (const QString& f : paths_) if (EXTRAS.contains(ext(f)) && QFileInfo::exists(f)) m << f; return m; }
  TopoImport choices(const QString& f) const { auto it = opts_.find(f); return it != opts_.end() ? it->second : fresh(); }

private:
  TopoImport fresh() const { TopoImport o; o.groups = true; o.objects = false; o.head = headDefault_; o.uvs = o.textures = o.creases = o.hard = o.polypaint = true; o.curves = false; o.parts = false;
    const QJsonObject s = QJsonDocument::fromJson(QSettings().value("import/choices").toByteArray()).object();   // (the choices of the last import — as they were left)
    for (const Choice& ch : CHOICES) if (QString(ch.key) != "head" && s.contains(ch.key)) o.*ch.f = s.value(ch.key).toBool();
    return o; }
  void keepChoices() const { if (opts_.empty()) return; const TopoImport& o = opts_.begin()->second; QJsonObject s; for (const Choice& ch : CHOICES) if (QString(ch.key) != "head") s.insert(ch.key, o.*ch.f);
    QSettings().setValue("import/choices", QJsonDocument(s).toJson(QJsonDocument::Compact)); }

  void browse() {
    QSettings st;
    QStringList fs = QFileDialog::getOpenFileNames(this, tr("Import"), st.value("importDir").toString(),
                                                   tr("Models and their textures (*.obj *.glb *.gltf *.fbx *.mtl *.bin *.png *.jpg *.jpeg *.webp *.bmp *.gif *.tga);;All files (*)"));
    if (fs.isEmpty()) return;
    st.setValue("importDir", QFileInfo(fs[0]).absolutePath());
    QStringList q; for (const QString& f : paths_) q << "\"" + f + "\"";
    for (const QString& f : fs) { QString n = QDir::toNativeSeparators(f); if (!paths_.contains(n)) q << "\"" + n + "\""; }
    path_->setText(q.join("; ")); rebuild();
  }
  void removePath(const QString& f) {
    QStringList q; for (const QString& p : paths_) if (p != f) q << "\"" + p + "\"";
    path_->setText(q.join("; ")); rebuild();
  }

  void rebuild() {
    paths_ = parsePaths(path_->text());
    while (list_->count() > 1) { QLayoutItem* li = list_->takeAt(0); if (QWidget* w = li->widget()) { w->hide(); w->deleteLater(); } delete li; }
    int n = 0, at = 0;
    auto shown = [](const QString& f) { return MODELS.contains(ext(f)) || !EXTRAS.contains(ext(f)) || !QFileInfo::exists(f); };
    for (const QString& f : paths_) if (MODELS.contains(ext(f)) && QFileInfo::exists(f)) { if (!opts_.count(f)) opts_[f] = fresh(); n++; }
    for (const QString& f : paths_) if (shown(f)) list_->insertWidget(at++, card(f, n == 1));
    const QStringList ex = extras();
    if (!ex.isEmpty()) list_->insertWidget(at++, extrasCard(ex));
    const int nm = int(models().size());
    go_->setEnabled(nm > 0); go_->setText(nm > 1 ? tr("Import %1 files").arg(nm) : tr("Import"));
    hint_->setText(paths_.isEmpty() ? tr("Models: OBJ, GLB, GLTF, FBX — pick their textures (and the .mtl of an OBJ) with them")
                                    : tr("Click a file to open its choices · the textures / .mtl / .bin go with every model"));
  }

  // a card's header: ▸ name FORMAT summary ✕ (a click opens / closes the card; its open state kept under `key`)
  QWidget* header(QWidget* card, const QString& key, const QString& name, const QString& tag, const QString& sum, bool miss, QWidget* body, std::function<void()> remove, QLabel** sumOut = nullptr) {
    auto* h = new QHBoxLayout; h->setSpacing(6); h->setContentsMargins(0, 0, 0, 0);
    auto* arrow = new QToolButton; arrow->setText(body && !body->isHidden() ? "▾" : "▸"); arrow->setFixedWidth(18); arrow->setStyleSheet("QToolButton{color:#cfcfcf;padding:0;font-size:12px}"); arrow->setEnabled(body != nullptr);
    if (!body) arrow->setText("");
    auto* nm = new QLabel(name); nm->setObjectName("fileName");
    auto* tg = new QLabel(tag); tg->setObjectName("fileTag");
    auto* sm = new QLabel(sum); sm->setObjectName(miss ? "fileMiss" : "fileSum"); sm->setMinimumWidth(10); if (sumOut) *sumOut = sm;
    auto* x = new QToolButton; x->setText("✕"); x->setToolTip(tr("Leave this out"));
    h->addWidget(arrow); h->addWidget(nm); h->addWidget(tg); h->addWidget(sm, 1); h->addWidget(x);
    auto* hw = new QWidget; hw->setLayout(h); hw->setCursor(Qt::PointingHandCursor);
    if (body) {
      auto toggle = [this, body, arrow, key] { body->setVisible(body->isHidden()); arrow->setText(body->isHidden() ? "▸" : "▾"); open_[key] = !body->isHidden(); };
      connect(arrow, &QToolButton::clicked, card, toggle);
      hw->installEventFilter(new ClickFilter(hw, toggle));
    }
    connect(x, &QToolButton::clicked, card, [this, remove] { QTimer::singleShot(0, this, remove); });
    return hw;
  }

  QString summary(const TopoImport& o) const {
    QStringList s;
    if (o.head) s << tr("head");
    if (o.groups) s << (o.objects ? tr("polygroups → meshes") : tr("polygroups"));
    if (o.uvs) s << tr("UVs");
    if (o.uvs && o.textures) s << tr("textures");
    if (o.polypaint) s << tr("polypaint");
    if (o.creases) s << tr("creases");
    if (o.hard) s << tr("hard edges");
    if (o.curves) s << tr("curves");
    if (topo_ && o.parts) s << tr("parts → polygroups");
    return s.isEmpty() ? tr("geometry only") : s.join(" · ");
  }

  QWidget* card(const QString& f, bool only) {
    auto* c = new QFrame; c->setObjectName("fileCard"); c->setToolTip(f);
    auto* v = new QVBoxLayout(c); v->setContentsMargins(6, 4, 6, 5); v->setSpacing(3);
    const QFileInfo fi(f); const QString name = fi.fileName().isEmpty() ? f : fi.fileName();
    if (!fi.exists() || !MODELS.contains(ext(f))) {
      v->addWidget(header(c, f, name, ext(f).toUpper(), !fi.exists() ? tr("not found: %1").arg(f) : tr("not a format that can be imported"), true, nullptr, [this, f] { removePath(f); }));
      return c;
    }
    auto* body = new QWidget(c); auto* g = new QGridLayout(body); g->setContentsMargins(20, 2, 0, 2); g->setHorizontalSpacing(14); g->setVerticalSpacing(2);
    auto it = open_.find(f); body->setVisible(it != open_.end() ? it->second : only);
    // (one column of what makes the mesh, one of what is taken from the file)
    std::map<QString, QCheckBox*> cb; int left = 0, right = 0;
    for (const Choice& ch : CHOICES) {
      const QString k = ch.key; if (k == "parts" && !topo_) continue;
      auto* b = new QCheckBox(tr(ch.label)); b->setToolTip(tr(ch.tip)); b->setChecked(opts_[f].*ch.f); cb[k] = b;
      const bool mesh = k == "groups" || k == "objects" || k == "head" || k == "parts";
      if (k == "objects") b->setStyleSheet("margin-left:16px");
      if (mesh) g->addWidget(b, left++, 0); else g->addWidget(b, right++, 1);
    }
    auto* all = new QPushButton(tr("Same for all files")); all->setToolTip(tr("These choices for every file in the list")); all->setVisible(models().size() > 1);
    connect(all, &QPushButton::clicked, c, [this, f] { const TopoImport o = opts_[f]; for (auto& [k, v] : opts_) v = o; QTimer::singleShot(0, this, [this] { rebuild(); }); });
    g->addWidget(all, std::max(left, right), 0, 1, 2, Qt::AlignLeft);
    QLabel* sumL = nullptr;
    v->addWidget(header(c, f, name, ext(f).toUpper(), summary(opts_[f]), false, body, [this, f] { removePath(f); }, &sumL));
    v->addWidget(body);
    auto sync = [this, f, cb, sumL] { TopoImport& O = opts_[f];
      for (const Choice& ch : CHOICES) if (cb.count(ch.key)) O.*ch.f = cb.at(ch.key)->isChecked();
      cb.at("objects")->setEnabled(O.groups || (topo_ && O.parts)); cb.at("textures")->setEnabled(O.uvs);
      if (!O.uvs && O.textures) { O.textures = false; QSignalBlocker k(cb.at("textures")); cb.at("textures")->setChecked(false); }
      sumL->setText(summary(O)); };   // (no UVs — no textures: unticked with them)
    for (auto& [k, b] : cb) connect(b, &QCheckBox::toggled, c, sync);
    sync();
    return c;
  }

  QWidget* extrasCard(const QStringList& ex) {
    auto* c = new QFrame; c->setObjectName("fileCard");
    auto* v = new QVBoxLayout(c); v->setContentsMargins(6, 4, 6, 5); v->setSpacing(2);
    auto* body = new QWidget(c); auto* b = new QVBoxLayout(body); b->setContentsMargins(20, 0, 0, 0); b->setSpacing(1);
    for (const QString& f : ex) {
      auto* row = new QHBoxLayout; auto* l = new QLabel(QFileInfo(f).fileName()); l->setToolTip(f); auto* tg = new QLabel(ext(f).toUpper()); tg->setObjectName("fileTag");
      auto* x = new QToolButton; x->setText("✕"); x->setToolTip(tr("Leave this file out"));
      connect(x, &QToolButton::clicked, c, [this, f] { QTimer::singleShot(0, this, [this, f] { removePath(f); }); });
      row->addWidget(l); row->addWidget(tg); row->addStretch(1); row->addWidget(x); b->addLayout(row);
    }
    auto it = open_.find("*extras"); body->setVisible(it != open_.end() ? it->second : false);
    v->addWidget(header(c, "*extras", tr("Textures and materials"), QString::number(ex.size()), tr("go with every model"), false, body, [this, ex] {
      QStringList q; for (const QString& p : paths_) if (!ex.contains(p)) q << "\"" + p + "\""; path_->setText(q.join("; ")); rebuild(); }));
    v->addWidget(body);
    return c;
  }

  // a click on a card's header opens / closes it
  class ClickFilter : public QObject {
  public:
    ClickFilter(QObject* p, std::function<void()> f) : QObject(p), f_(std::move(f)) {}
    bool eventFilter(QObject*, QEvent* e) override { if (e->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton) { f_(); return true; } return false; }
  private:
    std::function<void()> f_;
  };

  Viewport* vp_; Scene* sc_; bool topo_ = false, headDefault_ = false;
  QLineEdit* path_ = nullptr; QWidget* cards_ = nullptr; QVBoxLayout* list_ = nullptr; QLabel* hint_ = nullptr; QPushButton* go_ = nullptr;
  QStringList paths_; std::map<QString, TopoImport> opts_; std::map<QString, bool> open_;
};

QWidget* mainWindow() { for (QWidget* w : QApplication::topLevelWidgets()) if (w->inherits("QMainWindow")) return w; return nullptr; }

}  // namespace

void openImportWindow(Viewport* vp, Scene* sc, const QStringList& files, bool head) {
  QWidget* mw = mainWindow();
  ImportDialog d(mw, vp, sc, files, head);
  // (the middle of the screen the program is on)
  QScreen* scr = mw ? mw->screen() : QGuiApplication::primaryScreen();
  if (scr) { QRect a = scr->availableGeometry(); QSize s = d.size().boundedTo(a.size()); d.resize(s); d.move(a.center() - QPoint(s.width() / 2, s.height() / 2)); }
  if (d.exec() != QDialog::Accepted) return;
  const QStringList models = d.models(), extras = d.extras();
  for (const QString& m : models) {
    const TopoImport o = d.choices(m);
    if (sc->room == "topo") topoImportFiles(QStringList{m} + extras, o, vp, sc);
    else importFilesWith(QStringList{m} + extras, o, vp, sc);
  }
}

}  // namespace hm

namespace hm {
void importWindowShot(Viewport* vp, Scene* sc, const QStringList& files, const QString& png) {
  ImportDialog d(mainWindow(), vp, sc, files, false); d.show(); QApplication::processEvents(); QApplication::processEvents();
  d.grab().save(png);
}
}  // namespace hm
