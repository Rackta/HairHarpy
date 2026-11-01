#include "app/ConvertPanel.h"
#include "scene/Braid.h"
#include "scene/Convert.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>

namespace hm {

namespace {
QString fmtLen(double v) {                           // (+v).toPrecision(3)
  if (v == 0) return "0.00";
  int e = int(std::floor(std::log10(std::abs(v))));
  if (e >= 3 || e < -6) return QString::number(v, 'e', 2);
  return QString::number(v, 'f', std::max(0, 2 - e));
}
// a number label: double-click — type it
class NumLabel : public QLabel {
public:
  std::function<void()> onEdit;
  using QLabel::QLabel;
protected:
  void mouseDoubleClickEvent(QMouseEvent*) override { if (onEdit) onEdit(); }
};
QWidget* block(const QString& head, QVBoxLayout*& lay) {
  auto* w = new QWidget; lay = new QVBoxLayout(w); lay->setContentsMargins(0, 6, 0, 0); lay->setSpacing(4);
  w->setStyleSheet("QWidget#cb{border-top:1px solid #3a3a3a}"); w->setObjectName("cb");
  auto* l = new QLabel(head); l->setStyleSheet("font-size:11px;color:#9a9a9a"); lay->addWidget(l);
  return w;
}
std::pair<QPushButton*, QPushButton*> modes(QVBoxLayout* lay, const QString& tip) {
  auto* h = new QHBoxLayout; h->setSpacing(4);
  auto* off = new QPushButton(QObject::tr("Off")); auto* on = new QPushButton(QObject::tr("On"));
  for (auto* b : {off, on}) { b->setCheckable(true); b->setToolTip(tip); h->addWidget(b, 1); }
  lay->addLayout(h);
  return {off, on};
}
}  // namespace

ConvertPanel::ConvertPanel(Viewport* vp, Scene* sc, QWidget* parent) : QWidget(parent), vp_(vp), sc_(sc) {
  auto* v = new QVBoxLayout(this); v->setSpacing(6);
  list_ = new QWidget; list_->setObjectName("convList"); list_->setStyleSheet("#convList{background:#1f1f1f;border-radius:4px}");
  listLay_ = new QVBoxLayout(list_); listLay_->setContentsMargins(3, 3, 3, 3); listLay_->setSpacing(0);
  v->addWidget(list_);

  // Loop step (its lock groups'; none selected — the project's), Set reference
  { auto* h = new QHBoxLayout; h->setSpacing(6);
    auto* l = new QLabel(tr("Loop step"));
    auto* nv = new NumLabel("—"); nv->setStyleSheet("color:#fff"); stepVal_ = nv;
    nv->setToolTip(tr("Distance between neighbouring loops along the lock. Double-click — type it"));
    nv->onEdit = [this] {
      bool ok = false; double cur = vp_->refCube ? vp_->refCube->height : vp_->convValue("step").toDouble();
      double t = QInputDialog::getDouble(this, tr("Loop step"), tr("Loop step:"), cur, 1e-6, 1e9, 4, &ok);
      if (!ok || !(t > 0)) return;
      if (vp_->refCube) { vp_->setRefHeight(t); sync(); return; }               // (the cube's height while it is there)
      vp_->setConv("step", t, true); sync();
    };
    refBtn_ = new QPushButton(tr("Set reference")); refBtn_->setStyleSheet("padding:2px 7px");
    refBtn_->setToolTip(tr("Set the loop step with a reference cube: press Set reference, adjust the cube's height, press it again"));
    refBtn_->setCheckable(true);
    connect(refBtn_, &QPushButton::clicked, this, [this] { vp_->toggleReference(); sync(); });
    h->addWidget(l); h->addWidget(nv, 1); h->addWidget(refBtn_); v->addLayout(h); }
  // Deviation
  { auto* h = new QHBoxLayout; h->setSpacing(6);
    auto* l = new QLabel(tr("Deviation"));
    l->setToolTip(tr("How much a loop's size may differ from the loop step, by the curvature: step 10 and Deviation 50% — loops from 5 to 15 (shorter on bends, longer on straight parts or where loops must line up). Gaps longer than step × (1 + deviation) always get extra loops"));
    dev_ = new QSlider(Qt::Horizontal); dev_->setRange(0, 20); dev_->setToolTip(l->toolTip());
    devVal_ = new QLabel("30%"); devVal_->setFixedWidth(34);
    connect(dev_, &QSlider::valueChanged, this, [this](int x) { if (syncing_) return; devVal_->setText(QString("%1%").arg(x * 5)); vp_->setConv("dev", x * 0.05, false); });
    connect(dev_, &QSlider::sliderReleased, this, [this] { vp_->setConv("dev", dev_->value() * 0.05, true); });
    h->addWidget(l); h->addWidget(dev_, 1); h->addWidget(devVal_); v->addLayout(h); }

  // Auto weld points
  { QVBoxLayout* lay; QWidget* b = block(tr("Auto weld points"), lay);
    auto [off, on] = modes(lay, tr("Off — no points welded; On — the points closer than the distance are welded into one (on by default)"));
    weldOff_ = off; weldOn_ = on;
    auto setOn = [this](bool x) { QJsonObject w = sc_->fileRaw.value("weld").toObject(); w.insert("on", x); if (!w.contains("dist")) w.insert("dist", QJsonValue()); vp_->setWeldSetting("weld", w); sync(); };
    connect(off, &QPushButton::clicked, this, [setOn] { setOn(false); });
    connect(on, &QPushButton::clicked, this, [setOn] { setOn(true); });
    weldDistRow_ = new QWidget; auto* h = new QHBoxLayout(weldDistRow_); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(6);
    auto* nv = new NumLabel("auto"); nv->setStyleSheet("color:#fff"); weldDist_ = nv;
    nv->setToolTip(tr("How close two points must be to become one (auto — a quarter of the loop step). At the tips the distance is smaller. Double-click — type it"));
    nv->onEdit = [this] {
      QJsonObject w = sc_->fileRaw.value("weld").toObject(); bool ok = false;
      double cur = w.value("dist").toDouble(0); if (!(cur > 0)) cur = 0.25 * projectStep(*sc_);
      double t = QInputDialog::getDouble(this, tr("Weld distance"), tr("Weld distance:"), cur, 1e-6, 1e9, 4, &ok);
      if (!ok || !(t > 0)) return;
      if (!w.contains("on")) w.insert("on", true);
      w.insert("dist", t); vp_->setWeldSetting("weld", w); sync(); ui::toast(tr("Weld distance = %1").arg(fmtLen(t)));
    };
    auto* au = new QPushButton(tr("auto")); au->setStyleSheet("padding:2px 7px"); au->setToolTip(tr("Back to auto (a quarter of the loop step)"));
    connect(au, &QPushButton::clicked, this, [this] {
      QJsonObject w = sc_->fileRaw.value("weld").toObject(); if (!w.contains("on")) w.insert("on", true); w.insert("dist", QJsonValue());
      vp_->setWeldSetting("weld", w); sync(); ui::toast(tr("Weld distance: auto (a quarter of the loop step)"));
    });
    auto* pk = new QPushButton(tr("◎ Pick")); pk->setStyleSheet("padding:2px 7px"); pk->setCheckable(true); pickBtn_ = pk;
    pk->setToolTip(tr("A circle at the cursor (Ctrl + wheel / Ctrl+RMB drag — its size); a click sets its diameter as the weld distance. Esc — cancel"));
    connect(pk, &QPushButton::clicked, this, [this] { vp_->setMergePick(!vp_->mergePick); sync(); });
    h->addWidget(new QLabel(tr("Weld distance"))); h->addWidget(nv, 1); h->addWidget(pk); h->addWidget(au);
    lay->addWidget(weldDistRow_);
    v->addWidget(b); }

  // Weld locks
  { QVBoxLayout* lay; QWidget* b = block(tr("Weld locks"), lay);
    auto [off, on] = modes(lay, tr("On — the converted locks that touch become one mesh: their points closer than a tenth of the loop step are joined (the parent's stay), the walls they share and the faces inside another lock go, the small holes left are closed"));
    lockOff_ = off; lockOn_ = on;
    connect(off, &QPushButton::clicked, this, [this] { vp_->setWeldSetting("lockWeld", false); sync(); });
    connect(on, &QPushButton::clicked, this, [this] { vp_->setWeldSetting("lockWeld", true); sync(); });
    check_ = new QPushButton(tr("Check connections")); check_->setIcon(ui::icon("insp_runcheck")); check_->setIconSize(QSize(20, 20));
    check_->setToolTip(tr("Weld the selected cocoons' locks as Convert would (nothing is made) and show in red the places where two locks don't meet — those connections need redoing"));
    connect(check_, &QPushButton::clicked, this, [this] { vp_->checkConnections(true); });
    lay->addWidget(check_);
    v->addWidget(b); }

  auto* conv = new QPushButton(tr("Convert")); conv->setProperty("primary", true);
  conv->setToolTip(tr("The selected cocoons' locks and braids → meshes in the Meshes group (their groups hidden; Ctrl+Z — all back). A braid's tubes are a mesh of their own, welded to nothing"));
  connect(conv, &QPushButton::clicked, this, [this] { vp_->convertSelected(); });
  auto* hb = new QHBoxLayout; hb->addWidget(conv); hb->addStretch(1); v->addLayout(hb);
  sync();
}

void ConvertPanel::sync() {
  syncing_ = true;
  // the selected cocoons, as their tree (each under its nearest selected ancestor)
  while (QLayoutItem* it = listLay_->takeAt(0)) { if (it->widget()) { it->widget()->hide(); it->widget()->deleteLater(); } delete it; }
  std::vector<int> ks; for (int id : vp_->selection().items) if (const Item* k = sc_->item(id); k && k->type == ItemType::Cocoon) ks.push_back(id);
  std::vector<int> bs; for (int id : vp_->selection().items) if (const int b = braid::braidOfItem(*sc_, id); b >= 0 && std::find(bs.begin(), bs.end(), b) == bs.end()) bs.push_back(b);
  for (int b : bs) {   // (a braid: its tubes a mesh of their own, welded to nothing)
    auto* l = new QPushButton(QString::fromUtf8("⌇ ") + sc_->item(b)->name + tr(" — separate tubes, not welded")); l->setFlat(true);
    l->setStyleSheet("QPushButton{text-align:left;border:none;background:none;color:#dcf;padding:2px 4px}QPushButton:hover{background:#2a2a2a}");
    connect(l, &QPushButton::clicked, this, [this, b] { vp_->setSelectedItems({b}); }); listLay_->addWidget(l); }
  if (ks.empty() && bs.empty()) {
    auto* l = new QLabel(tr("Select cocoons or braids to convert (in the outliner or in the viewport)")); l->setWordWrap(true);
    l->setStyleSheet("color:#777;padding:3px 4px"); listLay_->addWidget(l);
  } else {
    auto up = [&](int k) { for (const Item* p = sc_->item(sc_->item(k)->parent); p && p->type == ItemType::Cocoon; p = sc_->item(p->parent)) if (std::find(ks.begin(), ks.end(), p->id) != ks.end()) return p->id; return -1; };
    std::function<void(int, int)> walk = [&](int k, int d) {
      auto* b = new QPushButton("◍ " + sc_->item(k)->name); b->setFlat(true);
      b->setStyleSheet(QString("QPushButton{text-align:left;border:none;background:none;color:#cfe;padding:2px 4px 2px %1px}QPushButton:hover{background:#2a2a2a}").arg(4 + d * 12));
      b->setToolTip(sc_->item(k)->name + tr(" — click: select only it"));
      connect(b, &QPushButton::clicked, this, [this, k] { vp_->setSelectedItems({k}); });
      listLay_->addWidget(b);
      for (int c : ks) if (up(c) == k) walk(c, d + 1);
    };
    for (int k : ks) if (up(k) < 0) walk(k, 0);
  }
  list_->setMaximumHeight(120);
  stepVal_->setText(fmtLen(vp_->refCube ? vp_->refCube->height : vp_->convValue("step").toDouble()));
  refBtn_->setChecked(vp_->refCube.has_value()); pickBtn_->setChecked(vp_->mergePick);
  double dv = vp_->convValue("dev").toDouble(0.3); dev_->setValue(int(std::lround(dv / 0.05))); devVal_->setText(QString("%1%").arg(int(std::lround(dv * 100))));
  QJsonObject w = sc_->fileRaw.value("weld").toObject(); bool on = w.value("on").toBool(true), lw = sc_->fileRaw.value("lockWeld").toBool();
  weldOff_->setChecked(!on); weldOn_->setChecked(on); lockOff_->setChecked(!lw); lockOn_->setChecked(lw);
  double d = w.value("dist").toDouble(0);
  weldDist_->setText(d > 0 ? fmtLen(d) : tr("auto (%1)").arg(fmtLen(0.25 * projectStep(*sc_))));
  weldDistRow_->setEnabled(on);
  check_->setEnabled(lw);
  syncing_ = false;
}

}  // namespace hm
