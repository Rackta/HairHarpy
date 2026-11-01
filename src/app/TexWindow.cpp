#include "app/TexWindow.h"
#include "app/MaterialWindow.h"
#include "scene/Looks.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <map>

namespace hm {

namespace {

const char* MIME = "application/x-hm-tex";

// thumbnails (a folder image, a kept image): made once per image
std::map<QString, QPixmap>& thumbCache() { static std::map<QString, QPixmap> c; return c; }
QString thumbKey(const looks::TexFile& f, int sz) { return QString("%1:%2:%3:%4").arg(f.abs).arg(f.mtime).arg(f.size).arg(sz); }
// (made in a worker thread: a big image takes a while to read)
QImage thumbImage(const QString& abs, int sz) {
  QImageReader r(abs); QImage img;
  if (r.canRead()) { QSize s = r.size(); if (s.isValid()) r.setScaledSize(s.scaled(sz, sz, Qt::KeepAspectRatio)); img = r.read(); }
  if (img.isNull()) img = looks::loadImage(abs);
  return img.isNull() ? QImage() : img.scaled(sz, sz, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
QPixmap thumbOfMap(const looks::MapSrc& s, int sz) {
  if (s.kind == looks::MapSrc::None) return {};
  QString key = s.key + ":" + QString::number(sz);
  auto it = thumbCache().find(key); if (it != thumbCache().end()) return it->second;
  QImage img = s.kind == looks::MapSrc::Linked ? looks::loadImage(s.path) : looks::decodeImage(s.bytes);
  QPixmap p = img.isNull() ? QPixmap() : QPixmap::fromImage(img.scaled(sz, sz, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  thumbCache()[key] = p; return p;
}

// the folder's thumbnails: dragged onto a map
class ThumbList : public QListWidget {
public:
  using QListWidget::QListWidget;
protected:
  QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
    auto* m = new QMimeData; if (!items.isEmpty()) m->setData(MIME, items[0]->data(Qt::UserRole).toString().toUtf8()); return m;
  }
  QStringList mimeTypes() const override { return {MIME}; }
};

// a map row: click — chosen (a folder thumbnail goes into it), a thumbnail dropped on it — into it
class SlotRow : public QFrame {
public:
  std::function<void()> onClick; std::function<void(const QString&)> onDrop;
  SlotRow() { setAcceptDrops(true); setCursor(Qt::PointingHandCursor); }
protected:
  void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton && onClick) onClick(); }
  void dragEnterEvent(QDragEnterEvent* e) override { if (e->mimeData()->hasFormat(MIME)) { e->acceptProposedAction(); setProperty("tgt", true); style()->polish(this); } }
  void dragLeaveEvent(QDragLeaveEvent*) override { setProperty("tgt", property("sel").toBool()); style()->polish(this); }
  void dropEvent(QDropEvent* e) override { if (onDrop) onDrop(QString::fromUtf8(e->mimeData()->data(MIME))); }
};

struct Slot { int id = -1, set = -1, k = -1; bool operator==(const Slot& o) const { return id == o.id && set == o.set && k == o.k; } };

class TexWindow : public QWidget {
public:
  TexWindow(QWidget* parent, Viewport* vp, Scene* sc) : QWidget(parent, Qt::Tool), vp_(vp), sc_(sc) {
    setAttribute(Qt::WA_DeleteOnClose); setMinimumSize(760, 460);
    setStyleSheet("SlotRow{border:1px solid #3a3a3a;border-radius:4px;background:#262626} SlotRow[tgt=\"true\"]{border-color:#e0a030;background:#3a3020}"
                  " #card{border:1px solid #3a3a3a;border-radius:5px;background:#232323} #card[off=\"true\"]{opacity:0.6} QLabel[miss=\"true\"]{color:#e07060}");
    auto* v = new QVBoxLayout(this); v->setContentsMargins(8, 8, 8, 8);
    auto* cols = new QHBoxLayout; v->addLayout(cols, 1);
    // the folder
    auto* fp = new QVBoxLayout; auto* fw = new QWidget; fw->setLayout(fp); fw->setFixedWidth(300); fp->setContentsMargins(0, 0, 0, 0);
    auto* fh = new QHBoxLayout; folderName_ = new QLabel; folderName_->setStyleSheet("font-weight:bold"); fh->addWidget(folderName_, 1);
    connectBtn_ = new QPushButton; connectBtn_->setToolTip(tr("A folder with the texture images (sub-folders too): its thumbnails show here — click one to put it into the chosen map, or drag it onto a map"));
    QObject::connect(connectBtn_, &QPushButton::clicked, this, [this] {
      QString d = QFileDialog::getExistingDirectory(this, tr("Texture folder"), looks::texDir().root.isEmpty() ? QSettings().value("textures/lastDir").toString() : looks::texDir().root);
      if (d.isEmpty()) return; QSettings().setValue("textures/lastDir", d);
      looks::connectTexDir(d); looks::rememberTexDir(*vp_->scene()); vp_->liveView(); renderFolder(); renderSets();
      ui::toast(tr("Texture folder: %1 — %2 images").arg(looks::texDir().name).arg(looks::texDir().files.size()));
    });
    discBtn_ = new QPushButton("✕"); discBtn_->setToolTip(tr("Disconnect the folder (linked textures stay linked — they show again when it is connected)"));
    QObject::connect(discBtn_, &QPushButton::clicked, this, [this] { looks::connectTexDir(QString()); looks::rememberTexDir(*vp_->scene()); vp_->liveView(); renderFolder(); renderSets(); });
    fh->addWidget(connectBtn_); fh->addWidget(discBtn_); fp->addLayout(fh);
    filter_ = new QLineEdit; QObject::connect(filter_, &QLineEdit::textChanged, this, [this] { fillGrid(); }); fp->addWidget(filter_);
    grid_ = new ThumbList; grid_->setViewMode(QListView::IconMode); grid_->setIconSize(QSize(64, 64)); grid_->setSpacing(2); grid_->setStyleSheet("QListWidget::item{padding:0;margin:0;height:100px}"); grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static); grid_->setDragEnabled(true); grid_->setDragDropMode(QAbstractItemView::DragOnly); grid_->setWordWrap(true);
    QObject::connect(grid_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
      if (slot_.id < 0) { ui::toast(tr("Click a map on the right first (Colour / Opacity of a set)")); return; }
      assign(slot_, it->data(Qt::UserRole).toString());
    });
    fp->addWidget(grid_, 1);
    folderNote_ = new QLabel(tr("Connect a folder to see its textures here and pick them by eye. Linked textures are read from the folder; 💾 keeps them in the project."));
    folderNote_->setWordWrap(true); folderNote_->setProperty("mut", true); fp->addWidget(folderNote_); fp->addStretch(0);
    cols->addWidget(fw);
    // the sets
    scroll_ = new QScrollArea; scroll_->setWidgetResizable(true); cols->addWidget(scroll_, 1);
    // the bottom bar
    auto* bar = new QHBoxLayout; v->addLayout(bar);
    saveBtn_ = new QPushButton; saveBtn_->setToolTip(tr("The textures taken from the folder (🔗, of every model) are put into the project file — it no longer depends on where they are stored"));
    QObject::connect(saveBtn_, &QPushButton::clicked, this, [this] { embedTextures(); });
    applyBtn_ = new QPushButton; QObject::connect(applyBtn_, &QPushButton::clicked, this, [this] { applyToOthers(); });
    auto* note = new QLabel(tr("Shows the selected models · Ctrl+Z undoes every change here.")); note->setProperty("mut", true);
    auto* cl = new QPushButton(tr("Close")); QObject::connect(cl, &QPushButton::clicked, this, &QWidget::close);
    bar->addWidget(saveBtn_); bar->addWidget(applyBtn_); bar->addWidget(note, 1); bar->addWidget(cl);
    QObject::connect(vp_, &Viewport::selectionChanged, this, [this](const std::vector<int>&) { follow(); });
    QObject::connect(vp_, &Viewport::sceneEdited, this, [this] { QTimer::singleShot(0, this, [this] { renderSets(); }); });
    // the folder looked through again: the program gets the focus back, every few seconds while open
    rescan_ = new QTimer(this); rescan_->setInterval(4000);
    QObject::connect(rescan_, &QTimer::timeout, this, [this] { if (isVisible() && looks::rescanTexDir()) { vp_->liveView(); renderFolder(); renderSets(); } });
    rescan_->start();
    QObject::connect(qApp, &QApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) { if (s == Qt::ApplicationActive && looks::rescanTexDir()) { vp_->liveView(); renderFolder(); renderSets(); } });
    QSettings st; if (st.contains("texWindow/geo")) restoreGeometry(st.value("texWindow/geo").toByteArray()); else resize(900, 560);
  }
  ~TexWindow() override { QSettings().setValue("texWindow/geo", saveGeometry()); }

  void open(int id, int mat = -2) {
    auto sel = selTargets(); onlyMat_ = mat;
    ids_ = mat != -2 ? std::vector<int>{id} : std::find(sel.begin(), sel.end(), id) != sel.end() ? sel : std::vector<int>{id};
    if (slot_.id >= 0 && std::find(ids_.begin(), ids_.end(), slot_.id) == ids_.end()) slot_ = {};
    looks::rescanTexDir(); renderFolder(); renderSets(); show(); raise(); activateWindow();
  }

private:
  QString tr(const char* s) const { return QObject::tr(s); }
  // the models the window can show: the selected imported meshes with UVs (a mesh's modifier stands for its mesh)
  std::vector<int> selTargets() const {
    std::vector<int> out;
    for (int id : vp_->selection().items) {
      const Item* it = sc_->item(id); if (it && it->type == ItemType::XMod && it->raw.value("mesh").isDouble()) it = sc_->item(it->raw.value("mesh").toInt());
      if (it && it->type == ItemType::IMesh && looks::hasUV(*it) && std::find(out.begin(), out.end(), it->id) == out.end()) out.push_back(it->id);
    }
    return out;
  }
  void follow() { if (!isVisible()) return; if (onlyMat_ != -2) return; auto s = selTargets(); if (s.empty() || s == ids_) return; ids_ = s; renderSets(); }

  void assign(const Slot& s, const QString& path) {
    const looks::TexFile* f = nullptr; for (auto& x : looks::texDir().files) if (x.path == path) { f = &x; break; }
    if (!f) return;
    QJsonObject o{{"name", f->name}, {"path", f->path}, {"dir", looks::texDir().name}};
    lookChanged(vp_, {s.id}, [&](QJsonObject& L) { setMap(L, s.set, s.k, o); if (s.k == 0) dropColor(L, s.set); });
  }
  static void setMap(QJsonObject& L, int set, int k, const QJsonValue& v) {   // (an undefined value — the key taken off: the file's own)
    QJsonArray S = L.value("sets").toArray(); if (set >= S.size()) return; QJsonObject s = S[set].toObject(); QJsonObject m = s.value("maps").toObject();
    if (v.isUndefined()) m.remove(looks::SLOT_KEY[k]); else m.insert(looks::SLOT_KEY[k], v);
    s.insert("maps", m); S[set] = s; L.insert("sets", S);
  }
  static void setField(QJsonObject& L, int set, const QString& k, const QJsonValue& v) {
    QJsonArray S = L.value("sets").toArray(); if (set >= S.size()) return; QJsonObject s = S[set].toObject();
    if (v.isUndefined()) s.remove(k); else s.insert(k, v); S[set] = s; L.insert("sets", S);
  }
  static void dropColor(QJsonObject& L, int set) { setField(L, set, "color", QJsonValue(QJsonValue::Undefined)); }

  void renderFolder() {
    const auto& D = looks::texDir();
    folderName_->setText("📁 " + (D.name.isEmpty() ? tr("No texture folder") : D.name)); folderName_->setToolTip(D.root);
    connectBtn_->setText(D.name.isEmpty() ? tr("Connect folder…") : tr("Change…"));
    discBtn_->setVisible(!D.name.isEmpty()); filter_->setVisible(!D.name.isEmpty()); grid_->setVisible(!D.name.isEmpty()); folderNote_->setVisible(D.name.isEmpty());
    filter_->setPlaceholderText(tr("filter %1 images").arg(D.files.size()));
    fillGrid();
  }
  void fillGrid() {
    grid_->clear(); pending_.clear();
    const auto& D = looks::texDir(); QString q = filter_->text().toLower(); int n = 0, total = 0;
    for (const auto& f : D.files) {
      if (!q.isEmpty() && !f.path.toLower().contains(q)) continue;
      total++; if (n >= 600) continue; n++;
      static QPixmap blank = [] { QPixmap p(64, 64); p.fill(QColor(0x1a, 0x1a, 0x1a)); return p; }();   // (the cell sized for its thumbnail before it comes)
      auto* it = new QListWidgetItem(QIcon(blank), f.name); it->setData(Qt::UserRole, f.path); it->setSizeHint(QSize(84, 100)); it->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop); it->setToolTip(f.path + tr("\nClick — into the chosen map · drag onto a map"));
      grid_->addItem(it); pending_.push_back({it, f});
    }
    if (!total && !D.files.empty()) grid_->addItem(new QListWidgetItem(tr("Nothing matches")));
    else if (total > 600) grid_->addItem(new QListWidgetItem(tr("… and %1 more — filter").arg(total - 600)));
    // (the thumbnails are read in worker threads — a folder of big images doesn't hold the window)
    const int gen = ++gridGen_;
    for (auto& [it, f] : pending_) {
      QString key = thumbKey(f, 64), path = f.path;
      auto c = thumbCache().find(key); if (c != thumbCache().end()) { it->setIcon(QIcon(c->second)); continue; }
      QPointer<QListWidget> grid = grid_; QString abs = f.abs;
      QThreadPool::globalInstance()->start([this, grid, gen, key, path, abs] {
        QImage img = thumbImage(abs, 64);
        QMetaObject::invokeMethod(grid, [this, grid, gen, key, path, img] {
          QPixmap p = QPixmap::fromImage(img); thumbCache()[key] = p;
          if (!grid || gen != gridGen_) return;
          for (int i = 0; i < grid->count(); i++) if (grid->item(i)->data(Qt::UserRole).toString() == path) { grid->item(i)->setIcon(QIcon(p)); break; }
        }, Qt::QueuedConnection);
      });
    }
    pending_.clear();
  }

  void renderSets() {
    // (the models gone or without UVs left out; none left — the window closes)
    std::vector<int> keep; for (int id : ids_) if (const Item* it = sc_->item(id); it && it->type == ItemType::IMesh && looks::hasUV(*it)) keep.push_back(id);
    ids_ = keep; if (ids_.empty()) { close(); return; }
    if (slot_.id >= 0 && std::find(ids_.begin(), ids_.end(), slot_.id) == ids_.end()) slot_ = {};
    int sy = scroll_->verticalScrollBar()->value();
    auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(4, 4, 4, 4); v->setSpacing(6);
    setWindowTitle(QString("▦ Textures — %1").arg(ids_.size() == 1 ? sc_->item(ids_[0])->name : tr("%1 models").arg(ids_.size())));
    if (onlyMat_ != -2 && ids_.size() == 1) { const PolyMesh* B = looks::baseOf(*sc_->item(ids_[0])); setWindowTitle(windowTitle() + " · " + (B && onlyMat_ >= 0 && size_t(onlyMat_) < B->matNames.size() ? B->matNames[size_t(onlyMat_)] : QString("Default"))); }
    for (int id : ids_) {
      const Item* T = sc_->item(id); QJsonObject L = looks::lookOf(*T); auto sets = looks::setsOf(*T);
      auto* top = new QHBoxLayout;
      if (ids_.size() > 1) { auto* b = new QLabel("⬢ " + T->name); b->setStyleSheet("font-weight:bold"); top->addWidget(b); }
      auto tog = [&](const QString& lab, const char* k, const QString& tip) { auto* c = new QCheckBox(lab); c->setToolTip(tip); c->setChecked(L.value(k).toBool());
        QObject::connect(c, &QCheckBox::toggled, this, [this, id, k](bool on) { lookChanged(vp_, {id}, [&](QJsonObject& x) { x.insert(k, on); }); }); top->addWidget(c); };
      tog(tr("Textures shown"), "tex", tr("Show the texture sets (▦ in the outliner)"));
      tog(tr("Flat (unlit)"), "flat", tr("No lights and shadows: the colours / textures as they are (◐ in the outliner)"));
      top->addStretch(1); v->addLayout(top);
      if (sets.empty()) { auto* m = new QLabel(tr("No materials.")); m->setProperty("mut", true); v->addWidget(m); }
      QJsonArray LS = L.value("sets").toArray();
      if (onlyMat_ != -2) { bool any = false; for (auto& s : sets) any |= s.mat == onlyMat_; if (!any) { auto* m = new QLabel(tr("This mat has no polygons yet — apply it to a selection first.")); m->setProperty("mut", true); v->addWidget(m); } }
      for (size_t i = 0; i < sets.size(); i++) {
        QJsonObject ls = LS[int(i)].toObject(); const int si = int(i);
        if (onlyMat_ != -2 && sets[i].mat != onlyMat_) continue;   // (opened on one UV Mat: only its set)
        auto* card = new QFrame; card->setObjectName("card"); card->setProperty("off", !ls.value("on").toBool(true)); auto* cv = new QVBoxLayout(card); cv->setContentsMargins(6, 4, 6, 6); cv->setSpacing(3);
        auto* hd = new QHBoxLayout;
        auto* on = new QCheckBox; on->setChecked(ls.value("on").toBool(true)); on->setToolTip(tr("This set on / off (off — the plain grey)"));
        QObject::connect(on, &QCheckBox::toggled, this, [this, id, si](bool x) { lookChanged(vp_, {id}, [&](QJsonObject& L2) { setField(L2, si, "on", x); }); });
        auto* nm = new QLabel(sets[i].name); nm->setStyleSheet("font-weight:bold");
        looks::MapSrc m0 = looks::resolve(*T, si, 0);
        QString col = looks::setColor(*T, si, m0.kind != looks::MapSrc::None);
        auto* cb = new QPushButton; cb->setFixedSize(34, 20); cb->setStyleSheet(QString("background:%1;border:1px solid #555").arg(col));
        cb->setToolTip(tr("Colour of the set (multiplies its colour map; with a colour map it starts white)"));
        QObject::connect(cb, &QPushButton::clicked, this, [this, id, si, col] { QColor c = QColorDialog::getColor(QColor(col), this, tr("Set colour")); if (c.isValid()) lookChanged(vp_, {id}, [&](QJsonObject& L2) { setField(L2, si, "color", c.name()); }); });
        hd->addWidget(on); hd->addWidget(nm, 1); hd->addWidget(cb);
        if (ls.contains("color")) { auto* r = new QPushButton("↺"); r->setToolTip(tr("The file's colour")); r->setFixedWidth(26);
          QObject::connect(r, &QPushButton::clicked, this, [this, id, si] { lookChanged(vp_, {id}, [&](QJsonObject& L2) { dropColor(L2, si); }); }); hd->addWidget(r); }
        cv->addLayout(hd);
        const PolyMesh* B = looks::baseOf(*T); int mat = sets[i].mat;
        for (int k = 0; k < 2; k++) {
          looks::MapSrc ms = looks::resolve(*T, si, k); QJsonValue o = ls.value("maps").toObject().value(looks::SLOT_KEY[k]);
          bool fileHas = B && mat >= 0 && size_t(mat) < B->matMaps.size() && (!B->matMaps[size_t(mat)][size_t(k)].data.isEmpty() || !B->matMaps[size_t(mat)][size_t(k)].name.isEmpty());
          Slot me{id, si, k}; bool sel = slot_ == me;
          auto* r = new SlotRow; r->setProperty("tgt", sel); r->setProperty("sel", sel);
          r->setToolTip(tr("Click — choose this map: a click on a folder thumbnail puts the texture here (or drag one onto it)"));
          auto* rh = new QHBoxLayout(r); rh->setContentsMargins(4, 2, 4, 2);
          auto* lab = new QLabel(QObject::tr(looks::SLOT_LABEL[k])); lab->setFixedWidth(52);
          auto* th = new QLabel; th->setFixedSize(40, 40); th->setStyleSheet("background:#1a1a1a;border:1px solid #333"); th->setAlignment(Qt::AlignCenter);
          QPixmap px = thumbOfMap(ms, 40); if (!px.isNull()) th->setPixmap(px);
          auto* what = new QLabel(ms.label); what->setToolTip(ms.label); what->setProperty("miss", ms.missing); what->setMinimumWidth(60);
          what->setTextFormat(Qt::PlainText); what->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
          rh->addWidget(lab); rh->addWidget(th); rh->addWidget(what, 1);
          bool has = ms.kind != looks::MapSrc::None || (o.isObject());
          auto* b = new QPushButton(has ? tr("Replace…") : tr("Load…")); b->setToolTip(tr("Pick an image file for this map — kept in the project (PNG, JPG, WEBP, TGA)"));
          QObject::connect(b, &QPushButton::clicked, this, [this, id, si, k] {
            QSettings st; QString f = QFileDialog::getOpenFileName(this, tr("Texture image"), st.value("textures/pickDir").toString(), tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tga)"));
            if (f.isEmpty()) return; st.setValue("textures/pickDir", QFileInfo(f).absolutePath());
            if (looks::loadImage(f).isNull()) { ui::toast(tr("Could not read the image")); return; }
            QJsonObject img = looks::keptImage(f);
            lookChanged(vp_, {id}, [&](QJsonObject& L2) { setMap(L2, si, k, img); if (k == 0) dropColor(L2, si); });
          });
          rh->addWidget(b);
          if (has) { auto* x = new QPushButton("✕"); x->setFixedWidth(26); x->setToolTip(tr("No map here"));
            QObject::connect(x, &QPushButton::clicked, this, [this, id, si, k] { lookChanged(vp_, {id}, [&](QJsonObject& L2) { setMap(L2, si, k, QJsonValue(QJsonValue::Null)); }); }); rh->addWidget(x); }
          if (!o.isUndefined() && fileHas) { auto* f = new QPushButton("↺"); f->setFixedWidth(26); f->setToolTip(tr("Back to the file's own map"));
            QObject::connect(f, &QPushButton::clicked, this, [this, id, si, k] { lookChanged(vp_, {id}, [&](QJsonObject& L2) { setMap(L2, si, k, QJsonValue(QJsonValue::Undefined)); }); }); rh->addWidget(f); }
          r->onClick = [this, me, sel] { slot_ = sel ? Slot{} : me; renderSets(); };
          r->onDrop = [this, me](const QString& path) { assign(me, path); };
          cv->addWidget(r);
        }
        auto* al = new QCheckBox(tr("Opacity from the colour map's alpha")); al->setChecked(looks::setAlpha(*T, si));
        al->setToolTip(tr("The colour map's own alpha channel makes it see-through (PNG with transparency)"));
        QObject::connect(al, &QCheckBox::toggled, this, [this, id, si](bool x) { lookChanged(vp_, {id}, [&](QJsonObject& L2) { setField(L2, si, "alpha", x); }); });
        cv->addWidget(al);
        v->addWidget(card);
      }
    }
    v->addStretch(1);
    scroll_->setWidget(w); scroll_->verticalScrollBar()->setValue(sy);
    int nl = looks::linkedCount(*sc_);
    saveBtn_->setText(tr("💾 Save textures into the project") + (nl ? tr(" (%1 linked)").arg(nl) : QString())); saveBtn_->setEnabled(nl > 0);
    applyBtn_->setVisible(ids_.size() > 1);
    if (ids_.size() > 1) { applyBtn_->setText(tr("⇉ Apply to all selected (%1)").arg(ids_.size() - 1));
      applyBtn_->setToolTip(tr("The textures of the first model (%1) — maps, colours, sets on / off, Textures shown / Flat — go to the other %2: a set takes the first model's set of the same name, else the one in the same place, else its first set").arg(sc_->item(ids_[0])->name).arg(ids_.size() - 1)); }
  }

  // 💾 the linked images (texture folder) into the project: kept as data from now on, wherever the files go
  void embedTextures() {
    looks::rescanTexDir();
    int n = 0, miss = 0; std::map<QString, QJsonObject> got;
    std::vector<int> all; for (auto& [id, it] : sc_->items) if (it.raw.value("look").isObject()) all.push_back(id);
    vp_->runEdit([&](Scene& s, Selection&, const CurveCache&) -> QString {
      for (int id : all) {
        Item* T = s.item(id); QJsonObject L = T->raw.value("look").toObject(); QJsonArray S = L.value("sets").toArray(); bool ch = false;
        for (int i = 0; i < S.size(); i++) { QJsonObject st = S[i].toObject(); QJsonObject m = st.value("maps").toObject();
          for (const char* k : looks::SLOT_KEY) { QJsonValue v = m.value(k); if (!v.isObject() || v.toObject().value("data").isString()) continue;
            const looks::TexFile* f = looks::texDirFile(v.toObject()); if (!f) { miss++; continue; }
            if (!got.count(f->abs)) got[f->abs] = looks::keptImage(f->abs);
            QJsonObject kept = got[f->abs]; kept.insert("name", v.toObject().value("name").toString(f->name)); m.insert(k, kept); n++; ch = true; }
          st.insert("maps", m); S[i] = st; }
        if (ch) { L.insert("sets", S); T->raw.insert("look", L); }
      }
      if (!n) return "!" + (miss ? tr("%1 linked texture%2 not found — connect their folder first").arg(miss).arg(miss > 1 ? "s" : "") : tr("Every texture is already in the project"));
      return tr("%1 texture%2 saved into the project").arg(n).arg(n > 1 ? "s" : "") + (miss ? tr(" · %1 not found — connect their folder and save again").arg(miss) : QString());
    }, false);
    vp_->liveView();
  }
  // ⇉ the first model's look onto the others (one undo step)
  void applyToOthers() {
    if (ids_.size() < 2) return;
    const Item* F = sc_->item(ids_[0]); QJsonObject src = looks::lookOf(*F); auto fsets = looks::setsOf(*F); QJsonArray SS = src.value("sets").toArray();
    std::vector<int> rest(ids_.begin() + 1, ids_.end());
    lookChanged(vp_, rest, [&](QJsonObject& L) {
      // (which model: the sets tell — lookChanged hands each model's look in turn)
      L.insert("tex", src.value("tex")); L.insert("flat", src.value("flat"));
      QJsonArray S = L.value("sets").toArray(), out;
      for (int i = 0; i < S.size(); i++) {
        QString name = S[i].toObject().value("name").toString(); int k = -1;
        for (size_t j = 0; j < fsets.size(); j++) if (fsets[j].name == name) { k = int(j); break; }
        QJsonObject o = SS[k >= 0 ? k : i < SS.size() ? i : 0].toObject();
        if (o.isEmpty()) { out.append(S[i]); continue; }
        QJsonObject c{{"name", name}, {"on", o.value("on").toBool(true)}, {"maps", o.value("maps")}};
        if (o.contains("color")) c.insert("color", o.value("color")); if (o.contains("alpha")) c.insert("alpha", o.value("alpha"));
        out.append(c);
      }
      L.insert("sets", out);
    });
    ui::toast(tr("Textures of %1 applied to %2 model%3").arg(F->name).arg(rest.size()).arg(rest.size() > 1 ? "s" : ""));
  }

  Viewport* vp_; Scene* sc_;
  std::vector<int> ids_; Slot slot_; int onlyMat_ = -2;   // (opened on one UV Mat: only it; −2 — every set)
  QLabel *folderName_, *folderNote_; QPushButton *connectBtn_, *discBtn_, *saveBtn_, *applyBtn_; QLineEdit* filter_; ThumbList* grid_; QScrollArea* scroll_;
  QTimer* rescan_ = nullptr; int gridGen_ = 0; std::vector<std::pair<QListWidgetItem*, looks::TexFile>> pending_;
};

QPointer<TexWindow> g_win;

}  // namespace

void lookChanged(Viewport* vp, const std::vector<int>& ids, const std::function<void(QJsonObject& look)>& fn) {
  vp->runEdit([&](Scene& s, Selection&, const CurveCache&) -> QString {
    for (int id : ids) { Item* T = s.item(id); if (!T) continue; QJsonObject L = looks::lookOf(*T); fn(L); T->raw.insert("look", L); }
    return QString();
  }, false);
  vp->liveView();                                            // (the meshes drawn again with their new looks)
}

void openTexWindow(QWidget* parent, Viewport* vp, Scene* sc, int id, int mat) {
  const Item* T = sc->item(id); if (!T) return;
  if (!looks::hasUV(*T)) { ui::toast(QObject::tr("This mesh has no UVs — textures can't be placed on it")); return; }
  if (!g_win) g_win = new TexWindow(parent ? parent->window() : nullptr, vp, sc);
  g_win->open(id, mat);
  g_win->move(viewportZone(vp, g_win->frameGeometry().size()).topLeft());   // (over the viewport's top right, kept on the screen)
}

std::vector<int> headLookTargets(const Scene& sc) {
  std::vector<int> out;
  for (auto& [id, it] : sc.items) if (it.type == ItemType::IMesh && it.headTag && looks::hasUV(it)) out.push_back(id);
  std::sort(out.begin(), out.end(), [&](int a, int b) { return sc.item(a)->order < sc.item(b)->order; });
  return out;
}

}  // namespace hm
