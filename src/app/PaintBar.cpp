// The Paint room's bars. The top one: Symmetry and its settings, how the model is shown (Standard — 1, Flat with no light — 2, the
// normal map — 3) and the map being painted (the room's mode). The second one: the brush's quick settings and what the map itself has
// (the bump's strength in the normal map).
#include "app/PaintBar.h"
#include "app/PaintSession.h"
#include "app/SymPanel.h"
#include "app/ToolPanels.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QCheckBox>
#include <QComboBox>
#include <QSettings>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <cmath>

namespace hm {

using namespace paint;

namespace {
QString pct(double x) { return QString::number(int(std::lround(x * 100))) + "%"; }
QPushButton* barBtn(const QString& t, const QString& tip) { auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); return b; }
QLabel* dim(const QString& t) { auto* l = new QLabel(t); l->setProperty("mut", true); return l; }
}  // namespace

// (19.11: "Standard + wireframe" and the material builder's display taken out — the first is of no use (W shows the wire), the second
// was slow and there is no engine material to set up yet)
QStringList paintDisplayModes() { return {"std", "flat", "normal"}; }
QString paintDisplayName(const QString& k) {
  return k == "flat" ? QObject::tr("Flat — no light") : k == "normal" ? QObject::tr("Normal map") : k == "wire" ? QObject::tr("Standard + wireframe") : k == "material" ? QObject::tr("Material (the builder's)") : QObject::tr("Standard");
}

QWidget* makePaintTopBar(PaintSession* s, Viewport* vp, std::function<void()> openMaterial) {
  Q_UNUSED(openMaterial);   // (the material builder's button taken out, 19.11)
  auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(6);
  auto* sym = barBtn(QString::fromUtf8("⇋ Symmetry"), QObject::tr("Symmetry (Shift+S): a stroke on the model is laid on its mirrored side too — across the plane of the symmetry's settings (the first mirror). On the texture itself it is not mirrored")); sym->setCheckable(true);
  auto* symS = barBtn(QString::fromUtf8("⚙"), QObject::tr("The symmetry's settings: the axis, the space, where its plane lies"));
  QObject::connect(sym, &QPushButton::clicked, w, [vp](bool on) { vp->setSymmetry(on); });
  QObject::connect(symS, &QPushButton::clicked, w, [vp, symS] { openSymPanel(vp, symS, false); });
  auto syncSym = [vp, sym] { QSignalBlocker b(sym); sym->setChecked(vp->tset.sym); };
  QObject::connect(vp, &Viewport::topoState, w, syncSym); QObject::connect(vp, &Viewport::changed, w, syncSym); QObject::connect(vp, &Viewport::roomChanged, w, syncSym); QObject::connect(vp, &Viewport::paintVisChanged, w, syncSym); syncSym();
  // the brush's Through: only what the camera sees, or the walls behind it too (Y)
  auto* thr = new QPushButton; thr->setIcon(ui::icon("pt_through")); thr->setIconSize(QSize(20, 20)); thr->setCheckable(true); thr->setFocusPolicy(Qt::NoFocus);
  thr->setToolTip(QObject::tr("Through (Y — a tap switches it, held — only while the key is down). Off — the brush paints only what the camera sees: the side of a lock facing you. On — the walls behind it too, as far back as the brush's radius: both sides of a thin lock at once, a general tone, a place where the visible-only stroke leaves gaps. The hair farther back is not touched. On the model the ring is doubled and amber while it is on. The shapes (lasso, rectangle, ellipse) take the same switch"));
  QObject::connect(thr, &QPushButton::clicked, w, [s](bool on) { s->setThrough(on); });
  auto syncThr = [s, thr] { QSignalBlocker b(thr); thr->setChecked(s->drawThrough); };
  QObject::connect(s, &PaintSession::throughChanged, w, syncThr); syncThr();
  // the alpha laid along the strand under the brush (the brush's "Turned: along the strand") — one click, not the brush panel's list
  auto* str = new QPushButton; str->setIcon(ui::icon("pt_alongstrand")); str->setIconSize(QSize(20, 20)); str->setCheckable(true); str->setFocusPolicy(Qt::NoFocus);
  str->setToolTip(QObject::tr("Along the strand: the brush's alpha lies along the hair strand under it (its UV island's long axis), whichever way the hand goes — a long thin alpha gives strokes that follow the lock. Off — the alpha keeps its own angle. The same as the brush's Turned → Along the strand"));
  QObject::connect(str, &QPushButton::clicked, w, [s](bool on) { s->brush.orient = on ? 2 : 0; s->brush.followStroke = false; emit s->brushChanged(); });
  auto syncStr = [s, str] { QSignalBlocker b(str); str->setChecked(s->brush.orient == 2); };
  QObject::connect(s, &PaintSession::brushChanged, w, syncStr); syncStr();
  // how the model is shown
  auto* disp = new QComboBox; disp->setFocusPolicy(Qt::NoFocus); int n = 1;
  for (const QString& k : paintDisplayModes()) disp->addItem(QString("%1   (%2)").arg(paintDisplayName(k)).arg(n++), k);
  disp->setToolTip(QObject::tr("How the model is shown: Standard (1) — the map under the light; Flat (2) — the map as it is, no light: the colours true; Normal map (3) — the normal map as it will be written (the bake through its mask + the painted bump), seen after each stroke"));
  QObject::connect(disp, &QComboBox::activated, w, [vp, disp](int) { vp->setPaintDisp(disp->currentData().toString()); });
  auto syncDisp = [vp, disp] { QSignalBlocker b(disp); disp->setCurrentIndex(std::max(0, disp->findData(vp->paintDisp))); };
  QObject::connect(vp, &Viewport::paintVisChanged, w, syncDisp); syncDisp();
  // the map being painted
  auto* map = new QComboBox; map->setFocusPolicy(Qt::NoFocus);
  for (const MapInfo& m : maps()) { map->addItem(QObject::tr(m.name), QString::fromLatin1(m.key)); map->setItemData(map->count() - 1, QObject::tr(m.about), Qt::ToolTipRole);
    if (m.ch == Channel::Height) { map->addItem(QObject::tr("Normal map (the result)"), "normalBump"); map->setItemData(map->count() - 1, QObject::tr("The normal map as it will be written: the baked normals where the Bake mask is painted, the painted bump over them. Painting here goes into the bump"), Qt::ToolTipRole); } }
  map->setToolTip(QObject::tr("The map being painted — the room's mode: each map has its own layers, and the brush writes what that map holds (colour, a gray mask, the bump's relief, the flow's direction)"));
  QObject::connect(map, &QComboBox::activated, s, [s, map](int) { s->setView(map->currentData().toString()); });
  auto syncMap = [s, map] { QSignalBlocker b(map); const int i = map->findData(s->view); if (i >= 0) map->setCurrentIndex(i); };
  QObject::connect(s, &PaintSession::viewChanged, w, syncMap); syncMap();
  h->addWidget(sym); h->addWidget(symS); h->addSpacing(8); h->addWidget(thr); h->addWidget(str); h->addSpacing(8); h->addWidget(dim(QObject::tr("Display"))); h->addWidget(disp); h->addSpacing(8); h->addWidget(dim(QObject::tr("Map"))); h->addWidget(map);
  return w;
}

QWidget* makePaintModeBar(PaintSession* s, Viewport* vp, std::function<void(bool)> setProj) {
  auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(10);
  auto row = [&](const QString& label, double lo, double hi, double step, double val, std::function<QString(double)> fmt, const QString& tip, int width = 200) {
    auto* r = new SliderRow(label, lo, hi, step, val, fmt); r->setTip(tip); r->setFixedWidth(width); h->addWidget(r); return r; };
  vp->paintSizeScreen = QSettings().value("paint/sizeScreen", true).toBool();
  auto* size = row(QObject::tr("Size"), 1, 512, 1, s->brush.size, [](double x) { return QString::number(int(x)) + " px"; }, QObject::tr("The brush's size: screen pixels on the model (Size on screen), texture pixels on the texture ([ ] · Alt + RMB drag)"), 210);
  auto* opac = row(QObject::tr("Opacity"), 0, 1, 0.01, s->brush.opacity, pct, QObject::tr("The most a stroke can lay down"));
  auto* flow = row(QObject::tr("Flow"), 0.01, 1, 0.01, s->brush.flow, pct, QObject::tr("What one dab adds: low flow builds up"), 180);
  auto* hard = row(QObject::tr("Hardness"), 0, 1, 0.01, s->brush.hardness, pct, QObject::tr("Of a round tip; a tip from a file has its own shape"));
  auto* smooth = row(QObject::tr("Smoothing"), 0, 1, 0.01, s->brush.smoothing, pct, QObject::tr("The stroke kept smooth: the brush follows the pointer on a string"));
  // the map's own: the bump
  auto* relief = row(QObject::tr("Relief"), 0, 1, 0.01, s->brush.strength, pct, QObject::tr("The Normal mode: the relief a full stroke raises (Ctrl — lowers)"), 190);
  auto* nstr = row(QObject::tr("Normal strength"), 0.05, 5, 0.05, s->bumpStrength, [](double x) { return QString::number(x, 'f', 2); }, QObject::tr("The Normal mode: the slope of the painted bump in the normal map"), 240);
  auto* scr = new QCheckBox(QObject::tr("Size on screen")); scr->setFocusPolicy(Qt::NoFocus); scr->setChecked(vp->paintSizeScreen);
  scr->setToolTip(QObject::tr("On — the brush's Size is screen pixels on the model: the same width wherever the UV is stretched, bigger as you zoom out. Off — texture pixels, as on the texture itself"));
  QObject::connect(scr, &QCheckBox::toggled, w, [vp](bool on) { vp->paintSizeScreen = on; QSettings().setValue("paint/sizeScreen", on); vp->paintOverlayChanged(); }); h->addWidget(scr);
  auto* prj = new QCheckBox(QObject::tr("Projection")); prj->setFocusPolicy(Qt::NoFocus); prj->setChecked(QSettings().value("paint/projection", true).toBool());
  prj->setToolTip(QObject::tr("On — a stroke on the model is laid on the screen and carried into the texture: round and even, whole across the UV seams, its size in screen pixels; the stencil and the strip need it. Off — the dabs are laid in the texture itself (the bump, the flow and the masks are always painted so)"));
  QObject::connect(prj, &QCheckBox::toggled, w, [setProj](bool on) { QSettings().setValue("paint/projection", on); if (setProj) setProj(on); }); h->addWidget(prj);
  auto* isl = new QCheckBox(QObject::tr("Stay in the island")); isl->setFocusPolicy(Qt::NoFocus); isl->setChecked(s->islandLock);
  isl->setToolTip(QObject::tr("On — a stroke paints only the UV island it began on (and its padding): the inner side of a lock without touching its neighbours — on the model and on the texture (3D-Coat's Connective painting)"));
  QObject::connect(isl, &QCheckBox::toggled, w, [s](bool on) { s->islandLock = on; QSettings().setValue("paint/islandLock", on); }); h->addWidget(isl);
  auto* cavC = new QComboBox; cavC->setFocusPolicy(Qt::NoFocus); cavC->addItems({QObject::tr("Relief: off"), QObject::tr("Hollows only"), QObject::tr("Ridges only"), QObject::tr("Near the scalp only"), QObject::tr("Shine (from the view)")}); cavC->setCurrentIndex(s->cavity);
  cavC->setToolTip(QObject::tr("The brush held back: by the baked curvature (Bake → Curvature) — in the hollows only or on the ridges only, dirt in the folds, wear on the edges; or near the scalp only — the roots of the locks darkened where they leave the cap, fading with the distance (mark the scalp: Select tool → Freeze menu → Mark as the scalp); or Shine — only where the light catches the surface as the model is seen when the stroke begins, as strongly as the highlight is there: metal drawn by hand in its own colour, the light set round the view (Light side / height), Shine width — the roughness"));
  h->addWidget(cavC);
  auto* reach = row(QObject::tr("Reach"), 0.005, 0.3, 0.005, s->scalpReach, pct, QObject::tr("Near the scalp: how far from the scalp the brush still paints — a share of the model's size; it fades to nothing there"), 170);
  QObject::connect(reach, &SliderRow::changed, w, [s](double x) { s->scalpReach = float(x); });
  reach->setVisible(s->cavity == 3);
  auto deg = [](double x) { return QString::number(int(std::lround(x))) + QString::fromUtf8("\u00b0"); };
  auto* shR = row(QObject::tr("Shine width"), 0.05, 1, 0.01, s->shineRough, [](double x) { return QString::number(x, 'f', 2); }, QObject::tr("Shine: how wide the highlight the brush follows is — the roughness of the metal"), 150);
  auto* shA = row(QObject::tr("Light side"), -90, 90, 1, s->shineAz, deg, QObject::tr("Shine: the light's side round the view — 0 straight from the eye, + to the right"), 150);
  auto* shE = row(QObject::tr("Light height"), -60, 80, 1, s->shineEl, deg, QObject::tr("Shine: how high the light is over the view"), 150);
  QObject::connect(shR, &SliderRow::changed, w, [s](double x) { s->shineRough = float(x); }); QObject::connect(shA, &SliderRow::changed, w, [s](double x) { s->shineAz = float(x); }); QObject::connect(shE, &SliderRow::changed, w, [s](double x) { s->shineEl = float(x); });
  for (auto* r : {shR, shA, shE}) r->setVisible(s->cavity == 4);
  QObject::connect(cavC, &QComboBox::activated, w, [s, reach, shR, shA, shE](int i) { s->cavity = i; reach->setVisible(i == 3); for (auto* r : {shR, shA, shE}) r->setVisible(i == 4); });
  auto bind = [&](SliderRow* r, float Brush::*m) { QObject::connect(r, &SliderRow::changed, w, [s, m](double x) { s->brush.*m = float(x); emit s->brushChanged(); }); };
  bind(size, &Brush::size); bind(opac, &Brush::opacity); bind(flow, &Brush::flow); bind(hard, &Brush::hardness); bind(smooth, &Brush::smoothing); bind(relief, &Brush::strength);
  QObject::connect(nstr, &SliderRow::changed, w, [s](double x) { s->bumpStrength = float(x); emit s->pixelsChanged({}); });
  QObject::connect(s, &PaintSession::pixelsChanged, nstr, [s, nstr](const QRect& r) { if (r.isNull() && std::abs(nstr->value() - s->bumpStrength) > 1e-4) { QSignalBlocker b(nstr); nstr->setValue(s->bumpStrength); } });   // (the Bake panel's Bump strength is the same value: followed)
  auto sync = [=] { QSignalBlocker b0(size), b1(opac), b2(flow), b3(hard), b4(smooth), b5(relief);
    size->setValue(s->brush.size); opac->setValue(s->brush.opacity); flow->setValue(s->brush.flow); hard->setValue(s->brush.hardness); smooth->setValue(s->brush.smoothing); relief->setValue(s->brush.strength); };
  auto mode = [=] { const bool bump = !s->view.startsWith("bake:") && s->modeChannel() == Channel::Height; relief->setVisible(bump); nstr->setVisible(bump); hard->setVisible(!bump); };
  QObject::connect(s, &PaintSession::brushChanged, w, sync); QObject::connect(s, &PaintSession::viewChanged, w, mode); sync(); mode();
  return w;
}

}  // namespace hm
