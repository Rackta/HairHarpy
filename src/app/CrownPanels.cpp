#include "app/CrownPanels.h"
#include "app/ToolPanels.h"
#include "scene/Crown.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>

namespace hm {

namespace {
QLabel* h4(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setWordWrap(true); return l; }
QLabel* muted(const QString& t) { auto* l = new QLabel(t); l->setProperty("mut", true); l->setWordWrap(true); return l; }
QPushButton* button(const QString& t, const QString& tip = {}, bool primary = false) {
  auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); if (primary) b->setProperty("primary", true); return b;
}
void setP(Scene& sc, int id, const QString& k, const QJsonValue& v) { Item* it = sc.item(id); QJsonObject p = it->raw.value("p").toObject(); p.insert(k, v); it->raw.insert("p", p); }
}  // namespace

QWidget* buildCrownPanel(Viewport* vp, Scene* sc) {
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setSpacing(4);
  v->addWidget(h4(QObject::tr("◠ Crown")));
  v->addWidget(muted(QObject::tr("Click on the head where the whorl (the back end of the parting) should be — a crown is built there from the reference. Create fits it on the top of the head.")));
  // the preset: what a new one is made from
  auto* pr = new QComboBox; pr->addItem(QObject::tr("Crown"), false); pr->addItem(QObject::tr("Bangs"), true); pr->setCurrentIndex(vp->crownBangs ? 1 : 0);
  pr->setToolTip(QObject::tr("Crown: the reference on the top of the head. Bangs: the same shape turned round and shorter front to back — on the front of the head, its open chamfered edge towards the crown"));
  QObject::connect(pr, &QComboBox::currentIndexChanged, vp, [vp, pr](int) { vp->crownBangs = pr->currentData().toBool(); });
  { auto* pw = new QWidget; auto* ph = new QHBoxLayout(pw); ph->setContentsMargins(0, 2, 0, 2); auto* pl = new QLabel(QObject::tr("Preset")); pl->setProperty("mut", true); pl->setFixedWidth(96); ph->addWidget(pl); ph->addWidget(pr, 1); v->addWidget(pw); }
  auto* sym = new QCheckBox(QObject::tr("Symmetric")); sym->setChecked(vp->crownSym); sym->setToolTip(QObject::tr("New crowns are symmetric across the head's centre (X): either side drives the other"));
  QObject::connect(sym, &QCheckBox::toggled, vp, [vp](bool on) { vp->crownSym = on; });
  v->addWidget(sym);
  auto* cont = new QCheckBox(QObject::tr("With a continuation")); cont->setChecked(vp->crownCont);
  cont->setToolTip(QObject::tr("The new crown comes with its continuation: curves down from every anchor of the lower connector and walls between them — a skirt round the head to cut into locks (Scissors → NURBS surface)"));
  QObject::connect(cont, &QCheckBox::toggled, vp, [vp](bool on) { vp->crownCont = on; });
  v->addWidget(cont);
  auto* cb = button(QObject::tr("＋ Create"), QObject::tr("A crown fitted on the top of the head"), true);
  QObject::connect(cb, &QPushButton::clicked, vp, [vp] { vp->createCrownAt(std::nullopt); });
  v->addWidget(cb);
  int k = -1; for (auto it = vp->selection().items.rbegin(); it != vp->selection().items.rend(); ++it) if (const Item* x = sc->item(*it); x && x->type == ItemType::Crown) { k = *it; break; }
  if (k >= 0) { v->addWidget(h4(sc->item(k)->name)); crownProps(v, vp, sc, k); }
  v->addStretch(1);
  return w;
}

void crownProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  const Item* k = sc->item(id); QJsonObject p = k->raw.value("p").toObject();
  auto mi = vp->crownMeshes().find(id);
  v->addWidget(muted(mi != vp->crownMeshes().end() ? QObject::tr("Vertical loops: %1 · rings: %2 · parting vertices: %3").arg(mi->second.loops).arg(mi->second.rings).arg(mi->second.part)
                                                     : QObject::tr("The crown needs its parting line, lower connector and at least one crown curve.")));
  auto* r = new SliderRow(QObject::tr("Rings"), 0, 40, 1, p.value("rings").toInt(0), [](double x) { return x > 0 ? QString::number(int(x)) : QObject::tr("auto"); });
  r->setTip(QObject::tr("Rings between the lower connector and the parting line (0 — from the loop step)"));
  QObject::connect(r, &SliderRow::changed, vp, [vp, sc, id](double x) { setP(*sc, id, "rings", int(x)); vp->shellChanged(); });
  QObject::connect(r, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
  v->addWidget(r);
  // the whorl: how the loops bunching up at a parting vertex are thinned out
  auto* ws = new QComboBox; ws->addItem(QObject::tr("Merge loops"), "merge"); ws->addItem(QObject::tr("Seam on the crown curve"), "seam");
  ws->setCurrentIndex(p.value("whorl").toString() == "seam" ? 1 : 0);
  ws->setToolTip(QObject::tr("Where many loops meet at the whorl. Merge loops: neighbouring loops merge in pairs below it (a triangle each). Seam: the loops next to the crown curve nearest the whorl end on that curve, herringbone-like"));
  QObject::connect(ws, &QComboBox::currentIndexChanged, vp, [vp, id, ws](int) { QString w = ws->currentData().toString(); vp->runEdit([id, w](Scene& s, Selection&, const CurveCache&) { setP(s, id, "whorl", w); return QString(); }, false); });
  auto* wr = new QWidget; auto* wh = new QHBoxLayout(wr); wh->setContentsMargins(0, 2, 0, 2); auto* wl = new QLabel(QObject::tr("Whorl")); wl->setProperty("mut", true); wl->setFixedWidth(96); wh->addWidget(wl); wh->addWidget(ws, 1);
  v->addWidget(wr);
  auto* bar = new QWidget; auto* bg = new QGridLayout(bar); bg->setContentsMargins(0, 6, 0, 0); bg->setSpacing(4);
  auto sel = [vp, sc, id](const QString& lab, int which) {
    auto* b = button(lab);
    QObject::connect(b, &QPushButton::clicked, vp, [vp, sc, id, which] {
      const Item* k = sc->item(id); std::vector<int> ids;
      if (which == 0) ids = {k->raw.value("part").toInt(-1)}; else if (which == 1) ids = {k->raw.value("low").toInt(-1)};
      else for (auto& r : crown::crownCurves(*sc, vp->hair().curves, *k)) ids.push_back(r.c);
      std::vector<int> ok; for (int x : ids) if (sc->item(x)) ok.push_back(x); vp->setSelectedItems(ok);
    });
    return b;
  };
  bg->addWidget(sel(QObject::tr("Parting line"), 0), 0, 0); bg->addWidget(sel(QObject::tr("Lower connector"), 1), 0, 1); bg->addWidget(sel(QObject::tr("Crown curves"), 2), 1, 0);
  auto* ad = button(QObject::tr("+ Crown curve"), QObject::tr("A new crown curve in the widest gap"), true);
  QObject::connect(ad, &QPushButton::clicked, vp, [vp, id] { vp->runEdit([id](Scene& s, Selection& sl, const CurveCache& cc) { return crown::addCrownCurve(s, cc, id, sl); }, true); });
  bg->addWidget(ad, 1, 1); v->addWidget(bar);
  // the continuation: a skirt of walls down from the lower connector, to be cut into locks
  auto* cl = new SliderRow(QObject::tr("Continuation"), 0.5, 6, 0.1, p.value("contLen").toDouble(2.0), [](double x) { return QObject::tr("×%1").arg(x, 0, 'f', 1); });
  cl->setTip(QObject::tr("How long the continuation is: × the crown's height"));
  QObject::connect(cl, &SliderRow::changed, vp, [sc, id](double x) { setP(*sc, id, "contLen", x); });
  v->addWidget(cl);
  auto* cn = button(QObject::tr("＋ Continuation"), QObject::tr("Curves down from every anchor of the lower connector (one a lock starts from already — left out) and walls between them: a skirt round the head growing out of the crown — cut it into locks with the Scissors (NURBS surface)"), true);
  QObject::connect(cn, &QPushButton::clicked, vp, [vp, id] { vp->crownContinue(id); });
  v->addWidget(cn);
  // symmetry (mirror plane — the head's centre, X)
  QJsonObject sy = p.value("sym").toObject(); bool on = sy.value("on").toBool();
  auto* scb = new QCheckBox(QObject::tr("Symmetric")); scb->setChecked(on);
  scb->setToolTip(QObject::tr("One half of the crown mirrored onto the other across the head's centre (X); then moving an anchor on either side moves its pair, the parting line stays in the middle"));
  QObject::connect(scb, &QCheckBox::clicked, vp, [vp, id, scb](bool x) {
    if (x) { scb->setChecked(false); vp->crownSymAsk(id, QCursor::pos()); return; }
    vp->runEdit([id](Scene& s, Selection&, const CurveCache&) { setP(s, id, "sym", QJsonValue()); return QObject::tr("Crown symmetry off — both halves are free"); }, false);
  });
  v->addWidget(scb);
  if (on) {
    v->addWidget(muted(QObject::tr("Symmetric: %1, plane X = %2. Crown curves added later aren't mirrored — Mirror again.")
                         .arg(sy.value("master").toInt() > 0 ? QObject::tr("left (+X) → right (−X)") : QObject::tr("right (−X) → left (+X)")).arg(vp->camera().sceneCenter.x, 0, 'f', 2)));
    auto* ma = button(QObject::tr("⇋ Mirror again…"), QObject::tr("Copy one half onto the other again (e.g. after adding crown curves on one side)"));
    QObject::connect(ma, &QPushButton::clicked, vp, [vp, id] { vp->crownSymAsk(id, QCursor::pos()); });
    v->addWidget(ma, 0, Qt::AlignLeft);
  }
  v->addWidget(muted(QObject::tr("Crown curves run from the lower connector to the parting line (both ends attached) — they shape the crown and set the vertical loops. Lock curves attached to the lower connector add loops there (a lock wall gets the same columns), so the lock's loops meet the crown's; with Weld locks on, Convert welds them into one mesh. An attached curve end is an anchor of the connector: drag it — the connector changes its shape; Ctrl + drag slides it along; Ctrl + click detaches the curve.")));
}

}  // namespace hm
