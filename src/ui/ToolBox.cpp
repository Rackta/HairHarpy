#include "ui/ToolBox.h"
#include "ui/Theme.h"
#include <iterator>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace hm::ui {

namespace {
struct Def { const char *id, *icon, *tip; };
const Def TOP[] = {
  {"cursor", "tool_cursor", "Cursor (Q)"},
  {"move", "tool_move", "Move (E). W — local / world axes, X / Y / Z — lock an axis"},
  {"rotate", "tool_rotate", "Rotate (R). Shift — 45° steps. W — local / world axes, X / Y / Z — lock an axis"},
  {"gizmo", "tool_scale", "Scale (T). Shift — 0.1 steps down to 0 (flatten), Ctrl — uniform. W — local / world axes, X / Y / Z — lock an axis"},
  {"brush", "tool_brush", "Brush (M). Shift — smooth; with curves selected only their anchors; Ctrl+RMB: ←→ radius, ↑↓ hardness"},
};
const Def CURVES[] = {
  {"draw", "tool_draw", "Curve (C): draw a curve; Esc — finish. Mode Edges (V): a mesh's edges picked and made into curves. Mode Lock (Active Tool): a flat lock around the curve — the circle at the cursor is its width (Ctrl + wheel / Ctrl+RMB drag), two anchors selected before — it starts between them"},
  {"cut", "tool_cut", "Scissors (K): RMB on a curve picks it (or select it before), then drag a cut line across it → pie: split into two / split and delete the upper or the lower part. Modes in the tool panel: NURBS surface (cut a wall), Split ends (a click splits a curve's end into a Y)"},
  {"modifier", "tool_curvemods", "Curve modifiers (A): Profile — sweep a cross-section along a curve, then convert it to curves and walls; Extra — mask, thickness, twist, noise, curls on top of it, and Cap (close the open ends of the locks)"},
  {"crown", "tool_crown", "Crown (S): the top of the head around the parting — click on the head where the whorl is, or press Create"},
  {"lockex", "tool_lockex", "Lock Extrude: click a flat lock (Shift — add one more), then LMB ←→ — extrude it away from the head / toward it (its tip stays welded), ↑↓ — the back's segments (curves in its middle, standing out: one — a pentagon across); RMB ←→ — the thickness narrowing at the root, ↑↓ — narrowing toward the tip. Another tool or another object — it is done"},
  {"braid", "tool_braid", "Braid: select a curve and press Create braid — strands woven along it (classic, Dutch, fishtail, rope), each strand's cross-section editable; the shape along it, sharpened lobes, squashed crossings; the ends separate (a movable connector round each tube), shared (one tube) or a point; out of it — separate tubes, a mesh on Convert, or locks"},
};
const Def SHELL = {"guide", "tool_shellmods", "Shell modifiers (G): on a cocoon, shaping its rings — Topology: Density (more / fewer rings where painted), Parent influence (the parent's pull weakened / added where painted); Guidelines: a line across the lock or a NURBS disc across the cocoon — a ring lies on it"};
}  // namespace

const std::vector<ToolBox::Tool>& ToolBox::ringTools() {
  static const std::vector<Tool> t = {{"cursor", "Cursor", "tool_cursor"}, {"move", "Move", "tool_move"}, {"rotate", "Rotate", "tool_rotate"},
    {"gizmo", "Scale", "tool_scale"}, {"brush", "Brush", "tool_brush"}, {"draw", "Curve", "tool_draw"}, {"cut", "Scissors", "tool_cut"},
    {"modifier", "Curve modifiers", "tool_curvemods"}, {"guide", "Shell modifiers", "tool_shellmods"}, {"crown", "Crown", "tool_crown"}, {"braid", "Braid", "tool_braid"},
    {"lockex", "Lock Extrude", "tool_lockex"}};
  return t;
}

ToolBox::ToolBox(QWidget* parent) : QFrame(parent) {
  setObjectName("toolbar"); setStyleSheet("#toolbar{background:#262626;border-bottom:1px solid #3a3a3a}");
  auto* v = new QVBoxLayout(this); v->setContentsMargins(8, 6, 8, 6); v->setSpacing(4);
  auto* top = new QHBoxLayout; top->setSpacing(6);
  auto mk = [](const QString& t, const QString& tip) { auto* b = new QPushButton(t); b->setProperty("flat2", true); b->setToolTip(tip); b->setFixedHeight(22); return b; };
  auto* sel = mk("⧉ SEL", tr("Copy the selection (Ctrl+C): the list of what is selected (names, types, ids, groups, anchors) and the camera go to the clipboard — paste it into a message; copied curves are what ⇶ Snap to path glues onto"));
  sel->setStyleSheet("font-size:9px"); sel->setFixedWidth(42);
  auto* ab = mk(tr("About the author"), tr("HairHarpy · catarackta.netlify.app — opens in the browser"));
  auto* su = mk(tr("Support the author"), tr("Support the author on Boosty (boosty.to/catarackta) — opens in the browser"));
  top->addWidget(sel); top->addStretch(1); top->addWidget(ab); top->addWidget(su);
  v->addLayout(top);
  connect(sel, &QPushButton::clicked, this, &ToolBox::selCopy);
  connect(ab, &QPushButton::clicked, this, &ToolBox::about);
  connect(su, &QPushButton::clicked, this, &ToolBox::support);

  auto row = [&](const Def* defs, int n, const char* label, bool withCocoon) {
    if (label) { auto* l = new QLabel(label); l->setStyleSheet("font-size:10px;color:#8a8f96;letter-spacing:1px;margin:3px 0 0 2px"); v->addWidget(l); }
    auto* h = new QHBoxLayout; h->setSpacing(3);
    auto tbtn = [&](const QString& icon, const QString& tip) {
      auto* b = new QPushButton; b->setProperty("tool", true); b->setFixedSize(40, 40); b->setIcon(ui::icon(icon)); b->setIconSize(QSize(34, 34));
      b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); return b;
    };
    if (withCocoon) {
      cocoon_ = tbtn("tool_cocoon", tr("Cocoon: select a lock (its group, its curves or its walls) and press — a see-through cocoon round it. It follows the lock's shape when you edit it; branches added to the lock later stay out of it"));
      connect(cocoon_, &QPushButton::clicked, this, &ToolBox::cocoonClicked); h->addWidget(cocoon_);
    }
    for (int i = 0; i < n; i++) {
      if (i > 0 && i % 6 == 0) { h->addStretch(1); v->addLayout(h); h = new QHBoxLayout; h->setSpacing(3); }   // (six to a row — the panel's width)
      auto* b = tbtn(defs[i].icon, tr(defs[i].tip)); b->setCheckable(true);
      QString id = defs[i].id; btns_[id] = b; b->setProperty("hmAction", id);   // (End over it: its key is the action's)
      connect(b, &QPushButton::clicked, this, [this, id] { emit toolPicked(id); });
      h->addWidget(b);
    }
    h->addStretch(1); v->addLayout(h);
  };
  row(TOP, int(std::size(TOP)), nullptr, false);
  row(CURVES, int(std::size(CURVES)), "Curves", false);   // (every tool of the list — a count by hand dropped the last one added)
  row(&SHELL, 1, "Cocoons", true);
  // Snap: the anchors being dragged stick to the reference meshes' surface
  { auto* l = new QLabel("Snapping · Symmetry"); l->setStyleSheet("font-size:10px;color:#8a8f96;letter-spacing:1px;margin:3px 0 0 2px"); v->addWidget(l);
    auto* h = new QHBoxLayout; h->setSpacing(3);
    snap_ = new QPushButton; snap_->setProperty("tool", true); snap_->setFixedSize(40, 40); snap_->setIcon(ui::icon("topo_snap")); snap_->setIconSize(QSize(34, 34)); snap_->setCheckable(true); snap_->setFocusPolicy(Qt::NoFocus);
    snap_->setToolTip(tr("Snap: the anchors being dragged stick to the surface of the reference meshes (a mesh's settings → Snapping → Reference; its snap mask, if on, limits where)"));
    connect(snap_, &QPushButton::clicked, this, [this] { emit snapClicked(); });
    h->addWidget(snap_);
    // (Symmetry — Shift+X, the same as the Topology room's: the dragged anchors' twins follow mirrored; its settings by it)
    sym_ = new QPushButton; sym_->setProperty("tool", true); sym_->setFixedSize(40, 40); sym_->setIcon(ui::icon("topo_sym")); sym_->setIconSize(QSize(34, 34)); sym_->setCheckable(true); sym_->setFocusPolicy(Qt::NoFocus);
    sym_->setToolTip(tr("Symmetry (Shift+X): an anchor dragged — its twin (the anchor where the mirror puts it) follows mirrored across the head's centre; the planes shown. Its settings: the button by it"));
    connect(sym_, &QPushButton::clicked, this, [this] { emit symClicked(); });
    symSet_ = new QPushButton; symSet_->setProperty("tool", true); symSet_->setFixedSize(40, 40); symSet_->setIcon(ui::icon("topo_snapsettings")); symSet_->setIconSize(QSize(34, 34)); symSet_->setFocusPolicy(Qt::NoFocus);
    symSet_->setToolTip(tr("Symmetry settings: planar / topology / radial, the axes X Y Z, the space (Local — the head's centre), Restrict, the tolerance, the planes shown"));
    connect(symSet_, &QPushButton::clicked, this, [this] { emit symSettings(symSet_); });
    h->addWidget(sym_); h->addWidget(symSet_); h->addStretch(1); v->addLayout(h); }
  setCurrent("cursor");
}
void ToolBox::setSnap(bool on) { if (snap_) snap_->setChecked(on); }
void ToolBox::setSym(bool on) { if (sym_) sym_->setChecked(on); }

void ToolBox::setCurrent(const QString& tool) { const QString t = tool == "edge" ? QString("draw") : tool;   // (Edges: a mode of the Curve tool)
  for (auto it = btns_.begin(); it != btns_.end(); ++it) it.value()->setChecked(it.key() == t); }

QString ToolBox::tip(const QString& tool) const { auto* b = btns_.value(tool); return b ? b->toolTip() : QString(); }

void ToolBox::setRoom(const QString& room) {
  // (the Topology room: its tools in a floating window of their own — here only ⧉ SEL and the author's links stay)
  bool hair = room != "topo";
  for (auto* b : btns_) b->setVisible(hair);
  if (cocoon_) cocoon_->setVisible(hair);
  if (snap_) snap_->setVisible(hair); if (sym_) sym_->setVisible(hair); if (symSet_) symSet_->setVisible(hair);
  for (auto* l : findChildren<QLabel*>()) l->setVisible(hair);
}

}  // namespace hm::ui
