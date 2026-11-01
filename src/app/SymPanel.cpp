// Symmetry's settings popup (the Topology room's and the Hair room's): laid out as a row per setting — its name on the
// left, the control on the right; a change applies at once and is kept
#include "app/SymPanel.h"
#include "view/Viewport.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <algorithm>
#include <cmath>
#include <functional>

namespace hm {

namespace {
double unitScale(const QString& u) { return u == "mm" ? 0.1 : u == "m" ? 100 : u == "in" ? 2.54 : 1; }   // (the Transform table's unit → scene units)
}

void openSymPanel(Viewport* vp, QWidget* anchor, bool uv) {
  auto* p = new QFrame(anchor->window(), Qt::Popup); p->setAttribute(Qt::WA_DeleteOnClose); p->setObjectName("sympanel");
  p->setStyleSheet("QFrame#sympanel{background:#2b2b2b;border:1px solid #444;border-radius:4px} QLabel{color:#c8c8c8} QCheckBox{color:#c8c8c8}"
                   " QPushButton[seg=\"true\"]{background:#3a3a3a;border:1px solid #2a2a2a;color:#d0d0d0;padding:3px 10px;min-width:60px}"
                   " QPushButton[seg=\"true\"]:checked{background:#5a5a5a;color:#fff} QPushButton[seg=\"true\"]:hover{background:#474747}");
  auto* g = new QGridLayout(p); g->setContentsMargins(12, 10, 12, 10); g->setHorizontalSpacing(14); g->setVerticalSpacing(6); g->setColumnMinimumWidth(1, 300);
  int row = 0;
  auto changed = [vp] { vp->symChanged(); };
  auto label = [&](const QString& t) { auto* l = new QLabel(t); g->addWidget(l, row, 0); return l; };
  auto check = [&](const QString& t, bool on, const QString& tip, std::function<void(bool)> fn) { label(t)->setToolTip(tip); auto* c = new QCheckBox; c->setChecked(on); c->setToolTip(tip);
    QObject::connect(c, &QCheckBox::toggled, p, [fn, changed](bool b) { fn(b); changed(); }); g->addWidget(c, row++, 1); return c; };
  // a segmented choice: one of the buttons pressed
  auto seg = [&](const QString& t, const QStringList& keys, const QStringList& names, const QString& cur, const QString& tip, std::function<void(const QString&)> fn) {
    label(t)->setToolTip(tip); auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(0); auto* bg = new QButtonGroup(w);
    for (int i = 0; i < keys.size(); i++) { auto* b = new QPushButton(names[i]); b->setProperty("seg", true); b->setCheckable(true); b->setChecked(keys[i] == cur); b->setFocusPolicy(Qt::NoFocus); b->setToolTip(tip); bg->addButton(b); h->addWidget(b, 1);
      const QString k = keys[i]; QObject::connect(b, &QPushButton::toggled, p, [fn, changed, k](bool on) { if (on) { fn(k); changed(); } }); }
    g->addWidget(w, row++, 1); return w; };
  auto gap = [&] { g->setRowMinimumHeight(row++, 8); };
  Viewport::SymOpts& S = vp->symo;

  QLabel* offHint = nullptr;                                   // (Show Planes ticked, Symmetry off: why no plane is seen)
  auto hint = [&] { if (offHint) offHint->setVisible(!vp->tset.sym && vp->symo.planes); };
  check(QObject::tr("Symmetry"), vp->tset.sym, QObject::tr("Shift+X — what is done to one side done mirrored to the other (each mirror / turn below)"), [vp, p](bool b) { vp->setSymmetry(b); if (auto* h = p->findChild<QLabel*>("symOffHint")) h->setVisible(!b && vp->symo.planes); });
  check(QObject::tr("Show Planes"), S.planes, QObject::tr("The symmetry's planes (the radial axis) shown over the active object while Symmetry is on"), [vp, p](bool b) { vp->symo.planes = b; if (auto* h = p->findChild<QLabel*>("symOffHint")) h->setVisible(!vp->tset.sym && b); });
  offHint = new QLabel(QObject::tr("Symmetry is off — the planes show once it is on (Shift+X)")); offHint->setObjectName("symOffHint"); offHint->setStyleSheet("color:#e0b060;font-size:11px"); g->addWidget(offHint, row++, 1); hint();
  { label(QObject::tr("Context"))->setToolTip(QObject::tr("Where the plane lies along its axis: Auto — at the space's origin when the object is symmetric about it, else through its middle; Origin — always at the origin; Box — through the middle of the object's box"));
    auto* cb = new QComboBox; cb->addItem(QObject::tr("Auto"), "auto"); cb->addItem(QObject::tr("Origin"), "origin"); cb->addItem(QObject::tr("Box"), "box");
    cb->setCurrentIndex(std::max(0, cb->findData(S.context))); cb->setToolTip(QObject::tr("Where the plane lies along its axis"));
    QObject::connect(cb, &QComboBox::activated, p, [vp, cb, changed](int) { vp->symo.context = cb->currentData().toString(); changed(); });
    g->addWidget(cb, row++, 1); }
  QWidget* copiesRow = nullptr; QLabel* copiesL = nullptr;
  seg(QObject::tr("Type"), {"planar", "topology", "radial"}, {QObject::tr("Planar"), QObject::tr("Topology"), QObject::tr("Radial")}, S.type,
      QObject::tr("Planar — mirrored across each checked axis' plane (by position); Topology — the twins found by position, then followed through the mesh's own edges: holds on a posed or slightly uneven mesh; Radial — copies turned round the first checked axis"),
      [vp, p](const QString& k) { vp->symo.type = k; for (const char* n : {"copies", "copiesL"}) if (auto* w = p->findChild<QWidget*>(n)) w->setVisible(k == "radial"); });
  { copiesL = label(QObject::tr("Copies")); auto* sp = new QSpinBox; sp->setRange(2, 64); sp->setValue(S.radial); sp->setToolTip(QObject::tr("Radial: how many copies round the axis (the original among them)"));
    QObject::connect(sp, &QSpinBox::valueChanged, p, [vp, changed](int n) { vp->symo.radial = n; changed(); }); copiesRow = sp; sp->setObjectName("copies"); copiesL->setObjectName("copiesL"); g->addWidget(sp, row++, 1);
    copiesRow->setVisible(S.type == "radial"); copiesL->setVisible(S.type == "radial"); }
  gap();
  for (int i = 0; i < 3; i++) check(QString("XYZ"[i]), S.ax[i], QObject::tr("Mirrored across the plane across this axis (several: each, and their combinations — four or eight copies); Radial — the first checked is the axis"), [vp, i](bool b) { vp->symo.ax[i] = b; });
  gap();
  QWidget* customRow = nullptr; QLabel* customL = nullptr;
  seg(QObject::tr("Space"), {"local", "global", "workplane", "custom"}, {QObject::tr("Local"), QObject::tr("Global"), QObject::tr("Workplane"), QObject::tr("Custom")}, S.space,
      QObject::tr("Local — the object's own axes and origin; Global — the world's; Workplane — the gizmo put by Alt + drag (its place and axes); Custom — the point below, the world's axes"),
      [vp, p](const QString& k) { vp->symo.space = k; for (const char* n : {"custom", "customL"}) if (auto* w = p->findChild<QWidget*>(n)) w->setVisible(k == "custom"); });
  { customL = label(QObject::tr("Point")); auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(4);
    for (int i = 0; i < 3; i++) { auto* sp = new QDoubleSpinBox; sp->setRange(-1e6, 1e6); sp->setDecimals(2); sp->setPrefix(QString("XYZ"[i]) + " "); sp->setKeyboardTracking(false);
      sp->setValue(i == 0 ? S.custom.x : i == 1 ? S.custom.y : S.custom.z); h->addWidget(sp, 1);
      QObject::connect(sp, &QDoubleSpinBox::valueChanged, p, [vp, i, changed](double x) { (i == 0 ? vp->symo.custom.x : i == 1 ? vp->symo.custom.y : vp->symo.custom.z) = x; changed(); }); }
    customRow = w; w->setObjectName("custom"); customL->setObjectName("customL"); g->addWidget(w, row++, 1); customRow->setVisible(S.space == "custom"); customL->setVisible(S.space == "custom"); }
  gap();
  check(QObject::tr("Restrict"), S.restrict_, QObject::tr("A point on the plane (on the axis — radial) stays on it when moved: the middle never torn open"), [vp](bool b) { vp->symo.restrict_ = b; });
  { label(QObject::tr("Tolerance"))->setToolTip(QObject::tr("How far a point's twin may lie from where the mirror (the turn) puts it — in the Transform table's unit"));
    const double k = unitScale(vp->tunit); auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(6);
    auto* sp = new QDoubleSpinBox; sp->setRange(0.001, 1000); sp->setDecimals(3); sp->setSuffix(" " + vp->tunit); sp->setValue(S.tol / k); sp->setKeyboardTracking(false); sp->setMinimumWidth(90);
    auto* sl = new QSlider(Qt::Horizontal); sl->setRange(0, 100); auto toSl = [](double v) { return int(std::lround((std::log10(std::max(1e-3, v)) + 3) / 6 * 100)); }; sl->setValue(toSl(S.tol / k));   // (0.001 … 1000, by its order)
    QObject::connect(sp, &QDoubleSpinBox::valueChanged, p, [vp, k, sl, toSl, changed](double v) { vp->symo.tol = v * k; { QSignalBlocker b(sl); sl->setValue(toSl(v)); } changed(); });
    QObject::connect(sl, &QSlider::valueChanged, p, [sp](int x) { sp->setValue(std::pow(10.0, x / 100.0 * 6 - 3)); });
    h->addWidget(sp); h->addWidget(sl, 1); g->addWidget(w, row++, 1); }
  if (uv) {                                                    // (the UV editor's part: its symmetric unfold, the tint, its axis, Symmetrize)
    auto* line = new QFrame; line->setFrameShape(QFrame::HLine); line->setStyleSheet("color:#444"); g->addWidget(line, row++, 0, 1, 2);
    auto* uvl = new QLabel(QObject::tr("UV")); uvl->setStyleSheet("color:#9a9a9a;font-size:11px"); g->addWidget(uvl, row++, 0);
    check(QObject::tr("Symmetric unfold"), vp->uvo.symUnfold, QObject::tr("Unfold lays a shell lying across X = 0 (a face, a torso) symmetric: its middle line straight up, its halves mirror images. Its seams must be symmetric too"), [vp](bool b) { vp->uvo.symUnfold = b; vp->saveUvPrefs(); });
    check(QObject::tr("Tint −X (burgundy)"), vp->uvo.symTint, QObject::tr("The −X half — every polygon with a mirror twin — burgundy in the UV Editor and on the model"), [vp](bool b) { vp->uvo.symTint = b; vp->saveUvPrefs(); vp->uvChanged(); });
    { label(QObject::tr("Mirror axis U")); auto* sp = new QDoubleSpinBox; sp->setRange(-10, 10); sp->setDecimals(3); sp->setSingleStep(0.05); sp->setValue(vp->uvo.symAxis); sp->setKeyboardTracking(false);
      sp->setToolTip(QObject::tr("The vertical line in the UV the mirror shells are mirrored across (Alt+Y — through the UV selection)"));
      QObject::connect(sp, &QDoubleSpinBox::valueChanged, p, [vp](double a) { vp->uvo.symAxis = a; vp->saveUvPrefs(); vp->uvChanged(); emit vp->topoState(); }); g->addWidget(sp, row++, 1); }
    auto* bw = new QWidget; auto* bh = new QHBoxLayout(bw); bh->setContentsMargins(0, 0, 0, 0); bh->setSpacing(6);
    auto* sz = new QPushButton(QObject::tr("⇋ Symmetrize UV (Shift+Y)")); QObject::connect(sz, &QPushButton::clicked, p, [vp] { vp->uvSymmetrize(); });
    auto* uu = new QPushButton(QObject::tr("Unfold symmetric (Alt+U)")); QObject::connect(uu, &QPushButton::clicked, p, [vp] { vp->uvUnfold(false, true); });
    bh->addWidget(sz); bh->addWidget(uu); g->addWidget(bw, row++, 0, 1, 2);
  }
  p->adjustSize();
  // (by the button: to its right, else — the Hair room's button by the window's right edge — to its left; kept on its screen)
  QPoint at = anchor->mapToGlobal(QPoint(anchor->width() + 4, 0));
  if (QScreen* scr = anchor->screen()) { const QRect A = scr->availableGeometry();
    if (at.x() + p->width() > A.right()) at.setX(anchor->mapToGlobal(QPoint(0, 0)).x() - p->width() - 4);
    at.setX(std::clamp(at.x(), A.left(), std::max(A.left(), A.right() - p->width()))); at.setY(std::clamp(at.y(), A.top(), std::max(A.top(), A.bottom() - p->height()))); }
  p->move(at); p->show();
}

}  // namespace hm
