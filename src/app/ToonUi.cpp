#include "app/ToonUi.h"
#include "scene/Scene.h"
#include "scene/Toon.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QLineEdit>
#include <QFileInfo>
#include <QSettings>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace hm {

ToonHub* ToonHub::get() { static ToonHub* h = new ToonHub; return h; }

ToonButton::ToonButton(int px, QWidget* parent) : QPushButton(parent) {
  setProperty("tool", true); setFixedSize(36, 36); setIcon(ui::icon("shader_toon")); setIconSize(QSize(px, px)); setFocusPolicy(Qt::NoFocus); setCheckable(true);
  setToolTip(tr("Shader mode (HairHarpy Toon): the model as the toon shader draws it — the same shader carried into Unreal, the website and "
                "Unity. Click — on / off; double click — Materials & Shader: materials, their instances, the parameters, who wears which"));
  auto* hub = ToonHub::get();
  auto sync = [this, hub] { QSignalBlocker b(this); setChecked(hub->isOn && hub->isOn()); };
  connect(hub, &ToonHub::changed, this, sync); sync();
  connect(this, &QPushButton::clicked, this, [hub, sync](bool on) { if (hub->setOn) hub->setOn(on); sync(); });
}
void ToonButton::mouseDoubleClickEvent(QMouseEvent* e) {
  // (the first click of the two switched the mode already; the second press comes as this: the window opened, the mode on — what is
  //  set up there is seen at once)
  auto* hub = ToonHub::get(); if (hub->setOn && hub->isOn && !hub->isOn()) hub->setOn(true);
  if (hub->openMenu) hub->openMenu(); e->accept();
}

namespace {

class ToonWin : public QWidget {
public:
  ToonWin(Scene* sc, Viewport* vp, std::function<void()> changed, QWidget* parent) : QWidget(parent, Qt::Tool), sc_(sc), vp_(vp), changed_(std::move(changed)) {
    setWindowTitle(tr("Materials & Shader")); resize(820, 640);
    auto* v = new QVBoxLayout(this); v->setContentsMargins(6, 6, 6, 6); v->setSpacing(6);
    auto* split = new QSplitter(Qt::Horizontal); v->addWidget(split, 1);
    // ---- left: the materials with their instances under them
    auto* left = new QWidget; auto* lv = new QVBoxLayout(left); lv->setContentsMargins(0, 0, 0, 0); lv->setSpacing(4);
    tree_ = new QTreeWidget; tree_->setHeaderHidden(true); tree_->setColumnCount(1); lv->addWidget(tree_, 1);
    auto* bar = new QHBoxLayout; bar->setSpacing(3);
    auto btn = [&](const QString& t, const QString& tip) { auto* b = new QToolButton; b->setText(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); bar->addWidget(b); return b; };
    auto* addM = btn(QString::fromUtf8("＋M"), tr("A new material — every parameter its own"));
    auto* addI = btn(QString::fromUtf8("＋I"), tr("A new instance of the material picked: it keeps the material's values, only the ones changed in it are its own"));
    auto* dup = btn(QString::fromUtf8("⧉"), tr("Duplicate the picked material or instance"));
    auto* ren = btn(QString::fromUtf8("✎"), tr("Rename"));
    auto* del = btn(QString::fromUtf8("✕"), tr("Delete (a material — with its instances; what wore them wears the default)"));
    auto* defB = btn(QString::fromUtf8("★"), tr("The default instance: worn wherever nothing is assigned"));
    auto* help = btn("?", tr("How the shader mode works"));
    bar->addStretch(1); lv->addLayout(bar);
    split->addWidget(left);
    // ---- right: the parameters of what is picked
    auto* right = new QWidget; auto* rv = new QVBoxLayout(right); rv->setContentsMargins(0, 0, 0, 0); rv->setSpacing(4);
    title_ = new QLabel; title_->setStyleSheet("font-weight:600"); rv->addWidget(title_);
    scroll_ = new QScrollArea; scroll_->setWidgetResizable(true); rv->addWidget(scroll_, 1);
    split->addWidget(right); split->setSizes({250, 570});
    // ---- bottom: who wears what (each object's UV set)
    auto* wl = new QLabel(tr("Who wears what — an object's UV set; the face and the eyes on one texture can wear two materials")); wl->setProperty("mut", true); v->addWidget(wl);
    auto* wb = new QHBoxLayout; auto* onlySel = new QCheckBox(tr("Only the selected objects")); onlySel->setChecked(true); wb->addWidget(onlySel);
    auto* giveSel = new QPushButton(tr("Selected objects ← the picked instance")); giveSel->setToolTip(tr("Every UV set of the selected objects wears the instance picked on the left")); wb->addWidget(giveSel); wb->addStretch(1); v->addLayout(wb);
    table_ = new QTableWidget(0, 4); table_->setHorizontalHeaderLabels({tr("Object"), tr("UV set"), tr("Instance"), tr("Role")}); table_->verticalHeader()->hide();
    table_->horizontalHeader()->setStretchLastSection(true); table_->setMinimumHeight(170); table_->setEditTriggers(QAbstractItemView::NoEditTriggers); v->addWidget(table_);
    table_->horizontalHeaderItem(3)->setToolTip(tr("Hair — the hair (the parts over the hair show through it); Over the hair — brows, lashes, the eyes: seen through the hair where the face looks at the camera (the head locator)"));
    onlySel_ = onlySel;
    // ---- the head locator (where the head is and where it faces — no joints in HairHarpy) and the export
    auto* hb = new QHBoxLayout; hb->setSpacing(4);
    headOn_ = new QCheckBox(tr("Head locator")); headOn_->setToolTip(tr("Where the head is and where it faces: the over-the-hair parts fade by it from the side and the back, the face SDF turns by it. Drawn in the view: forward blue, up green"));
    hb->addWidget(headOn_);
    auto spin = [&](double lo, double hi, int dec, const QString& tip) { auto* sp = new QDoubleSpinBox; sp->setRange(lo, hi); sp->setDecimals(dec); sp->setToolTip(tip); sp->setFixedWidth(78); hb->addWidget(sp); return sp; };
    for (int i = 0; i < 3; i++) hpos_[i] = spin(-1e6, 1e6, 2, tr("Its place: %1").arg(QString("XYZ")[i]));
    hyaw_ = spin(-180, 180, 1, tr("Its turn about the up axis (0 — facing the front view)")); hpitch_ = spin(-89, 89, 1, tr("Its tilt up (+) / down (−)"));
    auto* place = new QPushButton(tr("On the head")); place->setToolTip(tr("Put it in the head's middle (Head Collision, else the parts over the hair, else the Head object)")); hb->addWidget(place);
    auto* face = new QPushButton(tr("Facing the camera")); face->setToolTip(tr("Turn it to face the view's camera (as the model faces you)")); hb->addWidget(face);
    hb->addStretch(1);
    auto* exp = new QPushButton(tr("Export…")); exp->setToolTip(tr("Write the shader for Unreal, the website and Unity into a folder: the maps of every UV set, the materials, the shader's code and the import scripts")); hb->addWidget(exp);
    v->addLayout(hb);
    auto headSet = [this] { if (syncing_) return; auto& H = lib().head; H.on = headOn_->isChecked(); for (int i = 0; i < 3; i++) H.pos[i] = hpos_[i]->value(); H.yaw = hyaw_->value(); H.pitch = hpitch_->value(); paramsChanged(); };
    connect(headOn_, &QCheckBox::toggled, this, headSet);
    for (QDoubleSpinBox* sp : {hpos_[0], hpos_[1], hpos_[2], hyaw_, hpitch_}) connect(sp, qOverload<double>(&QDoubleSpinBox::valueChanged), this, headSet);
    connect(place, &QPushButton::clicked, this, [this] { double c[3], sz = 20; auto* hub = ToonHub::get(); if (!hub->headGuess || !hub->headGuess(c, sz)) { ui::toast(tr("No head found: Head Collision, the parts over the hair or an object named Head")); return; }
      auto& H = lib().head; H.on = true; for (int i = 0; i < 3; i++) H.pos[i] = c[i]; H.size = sz; syncHead(); paramsChanged(); });
    connect(face, &QPushButton::clicked, this, [this] { const Vec3 d = vp_->camera().dir() * -1.0; auto& H = lib().head;
      H.yaw = std::atan2(d.x, d.z) * 180 / M_PI; H.pitch = std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI; H.on = true; syncHead(); paramsChanged(); });
    connect(exp, &QPushButton::clicked, this, [this] { auto* hub = ToonHub::get(); if (!hub->exportTo) return;
      const QString dir = QFileDialog::getExistingDirectory(this, tr("Export the shader into"), QSettings().value("toon/exportDir").toString()); if (dir.isEmpty()) return;
      QSettings().setValue("toon/exportDir", dir); const QString r = hub->exportTo(dir);
      if (r.startsWith('!')) QMessageBox::warning(this, tr("Export"), r.mid(1)); else QMessageBox::information(this, tr("Export"), r); });

    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { buildParams(); });
    connect(addM, &QToolButton::clicked, this, [this] { auto& L = lib(); toon::Material m{L.newId("m"), tr("Material %1").arg(L.mats.size() + 1), toon::defaults()}; L.mats.push_back(m);
      toon::Instance i{L.newId("i"), m.name, m.id, {}}; L.insts.push_back(i); commit(); pick(i.id); });
    connect(addI, &QToolButton::clicked, this, [this] { auto& L = lib(); const QString mid = pickedMaterial(); if (mid.isEmpty()) return;
      const toon::Material* m = L.mat(mid); toon::Instance i{L.newId("i"), tr("%1 instance").arg(m ? m->name : QString()), mid, {}}; L.insts.push_back(i); commit(); pick(i.id); });
    connect(dup, &QToolButton::clicked, this, [this] { auto& L = lib(); const auto [kind, id] = picked();
      if (kind == 'm') { if (const toon::Material* m = L.mat(id)) { toon::Material c = *m; c.id = L.newId("m"); c.name += tr(" copy"); L.mats.push_back(c); toon::Instance i{L.newId("i"), c.name, c.id, {}}; L.insts.push_back(i); commit(); pick(i.id); } }
      else if (kind == 'i') { if (const toon::Instance* s = L.inst(id)) { toon::Instance c = *s; c.id = L.newId("i"); c.name += tr(" copy"); L.insts.push_back(c); commit(); pick(c.id); } } });
    connect(ren, &QToolButton::clicked, this, [this] { auto& L = lib(); const auto [kind, id] = picked(); QString* name = nullptr;
      if (kind == 'm') { if (auto* m = L.mat(id)) name = &m->name; } else if (kind == 'i') { if (auto* i = L.inst(id)) name = &i->name; }
      if (!name) return; bool ok = false; const QString n = QInputDialog::getText(this, tr("Rename"), tr("Name"), QLineEdit::Normal, *name, &ok); if (ok && !n.trimmed().isEmpty()) { *name = n.trimmed(); commit(); } });
    connect(del, &QToolButton::clicked, this, [this] { auto& L = lib(); const auto [kind, id] = picked();
      if (kind == 'm') { if (L.mats.size() <= 1) return; L.mats.erase(std::remove_if(L.mats.begin(), L.mats.end(), [&](const toon::Material& m) { return m.id == id; }), L.mats.end());
        L.insts.erase(std::remove_if(L.insts.begin(), L.insts.end(), [&](const toon::Instance& i) { return i.material == id; }), L.insts.end()); }
      else if (kind == 'i') { if (L.insts.size() <= 1) return; L.insts.erase(std::remove_if(L.insts.begin(), L.insts.end(), [&](const toon::Instance& i) { return i.id == id; }), L.insts.end()); }
      L.ensureDefault(); for (const QString& k : L.assign.keys()) if (!L.inst(L.assign.value(k).toString())) L.assign.remove(k); commit(); });
    connect(defB, &QToolButton::clicked, this, [this] { const auto [kind, id] = picked(); if (kind != 'i') return; lib().def = id; commit(); });
    connect(help, &QToolButton::clicked, this, [this] { QMessageBox::information(this, tr("Shader mode"), tr(
      "The shader mode draws the model with HairHarpy Toon — the toon shader that goes into Unreal, the website and Unity, so what is "
      "painted is seen as it will be there.\n\nA material holds every parameter; an instance wears its material and changes only some "
      "of them (shown bold, ↺ — back to the material's). An object's UV set wears an instance — two objects on one texture (the face "
      "and the eyes) can wear two. Where nothing is assigned the default (★) is worn.\n\nThe maps come from the Paint room's layers of "
      "each UV set — Base colour, Shade colour, Shadow mask, Highlight mask, Rim mask, Emission. A map not painted is not needed: its "
      "module takes its plain value (the shade made from the base colour, the masks everywhere). The normal map is the Paint room's Normal "
      "(the painted bump, the bake through its mask); the hair's band runs along the Flow map or the UV.\n\nRoles (the table): Hair; Over "
      "the hair — brows, lashes, the eyes: seen through the hair, fading from the side and the back by the head locator (paint the Over hair "
      "map for where and how much). The head locator also turns the face SDF.\n\nExport writes the maps, hh_toon.json, the shading core "
      "and the import scripts for Unreal, the website (three.js) and Unity into a folder — its README.md says how.\n\nThe light follows "
      "the camera.")); });
    connect(onlySel, &QCheckBox::toggled, this, [this] { buildTable(); });
    connect(giveSel, &QPushButton::clicked, this, [this] { const auto [kind, id] = picked(); QString inst = kind == 'i' ? id : QString();
      if (kind == 'm') for (const auto& i : lib().insts) if (i.material == id) { inst = i.id; break; }
      if (inst.isEmpty()) return; auto& L = lib(); int n = 0;
      for (int it : vp_->selection().items) { const Item* x = sc_->item(it); if (!x || x->type != ItemType::IMesh) continue;
        for (const auto& s : vp_->uvSets(it)) { L.assign[QString("%1:%2").arg(it).arg(s.name)] = inst; n++; } }
      if (!n) { ui::toast(tr("Select the objects first (in the view or the list)")); return; } commit(); ui::toast(tr("%1 UV set(s) wear it now").arg(n)); });
    connect(ToonHub::get(), &ToonHub::changed, this, [this] { if (isVisible() && !busy_) rebuild(); });
    connect(vp, &Viewport::selectionChanged, this, [this] { if (isVisible()) buildTable(); });
  }
  void rebuild() { syncHead(); const auto p = picked(); buildTree(); if (!p.second.isEmpty()) pick(p.second); else if (tree_->topLevelItemCount()) tree_->setCurrentItem(tree_->topLevelItem(0)); buildTable(); }

private:
  Scene* sc_; Viewport* vp_; std::function<void()> changed_; bool busy_ = false;
  QTreeWidget* tree_; QLabel* title_; QScrollArea* scroll_; QTableWidget* table_; QCheckBox* onlySel_;
  QCheckBox* headOn_ = nullptr; QDoubleSpinBox* hpos_[3] = {}; QDoubleSpinBox* hyaw_ = nullptr; QDoubleSpinBox* hpitch_ = nullptr; bool syncing_ = false;
  void syncHead() { syncing_ = true; const auto& H = lib().head; headOn_->setChecked(H.on); for (int i = 0; i < 3; i++) hpos_[i]->setValue(H.pos[i]); hyaw_->setValue(H.yaw); hpitch_->setValue(H.pitch); syncing_ = false; }
  toon::Lib& lib() { return sc_->toonView.lib; }
  void commit() { busy_ = true; changed_(); busy_ = false; buildTree(); buildTable(); }
  void paramsChanged() { busy_ = true; changed_(); busy_ = false; }
  std::pair<char, QString> picked() const { const QTreeWidgetItem* c = tree_->currentItem(); if (!c) return {0, {}}; const QString d = c->data(0, Qt::UserRole).toString(); return {d.isEmpty() ? char(0) : d[0].toLatin1(), d.mid(2)}; }
  QString pickedMaterial() { const auto [k, id] = picked(); if (k == 'm') return id; if (k == 'i') if (const auto* i = lib().inst(id)) return i->material; return {}; }
  void pick(const QString& id) { for (int a = 0; a < tree_->topLevelItemCount(); a++) { QTreeWidgetItem* t = tree_->topLevelItem(a); if (t->data(0, Qt::UserRole).toString().mid(2) == id) { tree_->setCurrentItem(t); return; }
      for (int b = 0; b < t->childCount(); b++) if (t->child(b)->data(0, Qt::UserRole).toString().mid(2) == id) { tree_->setCurrentItem(t->child(b)); return; } } }
  void buildTree() {
    QSignalBlocker b(tree_); const auto keep = picked(); tree_->clear(); auto& L = lib();
    for (const auto& m : L.mats) { auto* t = new QTreeWidgetItem(tree_, {m.name}); t->setData(0, Qt::UserRole, "m:" + m.id); QFont f = t->font(0); f.setBold(true); t->setFont(0, f);
      for (const auto& i : L.insts) if (i.material == m.id) { auto* c = new QTreeWidgetItem(t, {(i.id == L.def ? QString::fromUtf8("★ ") : QString()) + i.name}); c->setData(0, Qt::UserRole, "i:" + i.id); }
      t->setExpanded(true); }
    if (!keep.second.isEmpty()) pick(keep.second);
  }
  void buildParams() {
    const auto [kind, id] = picked(); auto& L = lib();
    toon::Material* M = kind == 'm' ? L.mat(id) : nullptr; toon::Instance* I = kind == 'i' ? L.inst(id) : nullptr;
    if (!M && !I) { title_->clear(); scroll_->setWidget(new QWidget); return; }
    const toon::Material* base = I ? L.mat(I->material) : M;
    title_->setText(I ? tr("Instance %1 — of %2: only the bold values are its own").arg(I->name, base ? base->name : QString()) : tr("Material %1").arg(M->name));
    auto* w = new QWidget; auto* form = new QVBoxLayout(w); form->setContentsMargins(4, 4, 4, 4); form->setSpacing(3);
    const QJsonObject eff = I ? L.paramsOf(I->id) : toon::merged(M->params, {});
    for (const auto& mod : toon::modules()) {
      auto* h = new QLabel(tr(mod.name)); h->setToolTip(tr(mod.tip)); h->setStyleSheet("font-weight:600;color:#ddd;margin-top:8px;border-bottom:1px solid #3a3a3a"); form->addWidget(h);
      for (const auto& d : toon::params()) { if (qstrcmp(d.module, mod.key)) continue;
        auto* row = new QHBoxLayout; row->setSpacing(6); auto* lab = new QLabel(tr(d.label)); lab->setFixedWidth(110); if (*d.tip) lab->setToolTip(tr(d.tip));
        const bool own = I && I->over.contains(d.key); if (own) { QFont f = lab->font(); f.setBold(true); lab->setFont(f); }
        row->addWidget(lab);
        const QString key = QString::fromLatin1(d.key);
        auto set = [this, M, I, key](const QJsonValue& val) { if (I) I->over[key] = val; else M->params[key] = val; paramsChanged(); };
        if (d.kind == 'f') {
          auto* sl = new QSlider(Qt::Horizontal); sl->setRange(0, 1000); auto* sp = new QDoubleSpinBox; sp->setRange(d.lo, d.hi); sp->setDecimals(3); sp->setSingleStep((d.hi - d.lo) / 100); sp->setFixedWidth(80);
          const double val = toon::num(eff, d.key); sp->setValue(val); sl->setValue(int((val - d.lo) / (d.hi - d.lo) * 1000));
          const double lo = d.lo, hi = d.hi;
          connect(sl, &QSlider::valueChanged, sp, [sp, lo, hi](int x) { sp->setValue(lo + (hi - lo) * x / 1000.0); });
          connect(sp, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [sl, set, lo, hi, lab](double x) { QSignalBlocker b(sl); sl->setValue(int((x - lo) / (hi - lo) * 1000)); set(x); });
          row->addWidget(sl, 1); row->addWidget(sp);
        } else if (d.kind == 'b') {
          auto* cb = new QCheckBox; cb->setChecked(toon::flag(eff, d.key)); connect(cb, &QCheckBox::toggled, this, [set](bool on) { set(on); }); row->addWidget(cb); row->addStretch(1);
        } else if (d.kind == 'e') {
          auto* cb = new QComboBox; for (const QString& o : toon::enumOpts(d.key)) cb->addItem(tr(o.toUtf8().constData())); cb->setCurrentIndex(int(toon::num(eff, d.key) + 0.5));
          connect(cb, qOverload<int>(&QComboBox::activated), this, [set](int i) { set(double(i)); }); row->addWidget(cb, 1);
        } else if (d.kind == 's') {
          auto* le = new QLineEdit(toon::str(eff, d.key)); le->setReadOnly(true); le->setPlaceholderText(tr("none"));
          auto* pk = new QToolButton; pk->setText(QString::fromUtf8("…")); pk->setToolTip(tr("Choose the picture")); auto* cl = new QToolButton; cl->setText(QString::fromUtf8("✕")); cl->setToolTip(tr("None"));
          connect(pk, &QToolButton::clicked, this, [this, le, set] { const QString f = QFileDialog::getOpenFileName(this, tr("Picture"), QSettings().value("toon/lastDir").toString(), tr("Images (*.png *.tga *.jpg *.jpeg *.bmp *.tif *.tiff)"));
            if (f.isEmpty()) return; QSettings().setValue("toon/lastDir", QFileInfo(f).absolutePath()); le->setText(f); set(f); });
          connect(cl, &QToolButton::clicked, this, [le, set] { le->clear(); set(QString()); });
          row->addWidget(le, 1); row->addWidget(pk); row->addWidget(cl);
        } else {
          auto* cb = new QPushButton; cb->setFixedSize(60, 20); const QColor c0 = toon::col(eff, d.key); cb->setStyleSheet(QString("background:%1;border:1px solid #555").arg(c0.name()));
          connect(cb, &QPushButton::clicked, this, [this, cb, set, key, eff] { const QColor c = QColorDialog::getColor(toon::col(eff, key.toLatin1().constData()), this, tr("Colour"));
            if (!c.isValid()) return; cb->setStyleSheet(QString("background:%1;border:1px solid #555").arg(c.name())); set(c.name()); });
          row->addWidget(cb); row->addStretch(1);
        }
        if (I) { auto* rs = new QToolButton; rs->setText(QString::fromUtf8("↺")); rs->setToolTip(tr("Back to the material's value")); rs->setEnabled(own); rs->setFocusPolicy(Qt::NoFocus);
          connect(rs, &QToolButton::clicked, this, [this, I, key] { I->over.remove(key); paramsChanged(); QTimer::singleShot(0, this, [this] { buildParams(); }); }); row->addWidget(rs); }
        form->addLayout(row); }
    }
    form->addStretch(1); scroll_->setWidget(w);
  }
  void buildTable() {
    auto& L = lib(); table_->setRowCount(0);
    std::vector<int> ids; for (const auto& [id, it] : sc_->items) if (it.type == ItemType::IMesh && it.poly && !it.headTag && (!onlySel_->isChecked() || vp_->isItemSelected(id))) ids.push_back(id);
    for (int id : ids) { const Item* it = sc_->item(id);
      for (const auto& s : vp_->uvSets(id)) {
        const int r = table_->rowCount(); table_->insertRow(r);
        table_->setItem(r, 0, new QTableWidgetItem(it->name)); table_->setItem(r, 1, new QTableWidgetItem(s.name));
        auto* cb = new QComboBox; const QString key = QString("%1:%2").arg(id).arg(s.name), cur = L.assign.value(key).toString();
        cb->addItem(tr("— default (%1)").arg(L.inst(L.def) ? L.inst(L.def)->name : QString()), QString());
        for (const auto& i : L.insts) cb->addItem(i.name, i.id);
        cb->setCurrentIndex(std::max(0, cb->findData(cur)));
        connect(cb, qOverload<int>(&QComboBox::activated), this, [this, cb, key] { const QString v = cb->currentData().toString(); if (v.isEmpty()) lib().assign.remove(key); else lib().assign[key] = v; busy_ = true; changed_(); busy_ = false; });
        table_->setCellWidget(r, 2, cb);
        auto* rc = new QComboBox; rc->addItem(QString::fromUtf8("—"), QString()); rc->addItem(tr("Hair"), QString("hair")); rc->addItem(tr("Over the hair"), QString("over"));
        rc->setCurrentIndex(std::max(0, rc->findData(L.roles.value(key).toString())));
        connect(rc, qOverload<int>(&QComboBox::activated), this, [this, rc, key] { const QString v = rc->currentData().toString(); if (v.isEmpty()) lib().roles.remove(key); else lib().roles[key] = v; busy_ = true; changed_(); busy_ = false; });
        table_->setCellWidget(r, 3, rc); } }
    table_->resizeColumnsToContents();
  }
};

}  // namespace

QWidget* toonWindow(Scene* sc, Viewport* vp, std::function<void()> libChanged, QWidget* parent) {
  static QPointer<ToonWin> w; if (!w) w = new ToonWin(sc, vp, std::move(libChanged), parent);
  w->rebuild(); w->show(); w->raise(); w->activateWindow(); return w;
}

}  // namespace hm
