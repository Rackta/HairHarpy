#include "ui/ToolBox.h"
#include "ui/Theme.h"
#include <iterator>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollArea>
#include <QSizeGrip>
#include <QSplitter>
#include <QToolTip>
#include <QTextDocument>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSettings>
#include <algorithm>
#include <QPushButton>
#include <QVBoxLayout>

namespace hm::ui {

namespace {
struct Def { const char *id, *icon, *tip; };
const Def TOP[] = {
  {"cursor", "tool_cursor", "Cursor (Q)"},
  {"move", "tool_move", "Move (E). Shift+W — local / world axes, Shift+X / Y / Z — lock an axis"},
  {"rotate", "tool_rotate", "Rotate (R). Shift — 45° steps. Shift+W — local / world axes, Shift+X / Y / Z — lock an axis"},
  {"gizmo", "tool_scale", "Scale (T). Shift — 0.1 steps down to 0 (flatten), Ctrl — uniform. Shift+W — local / world axes, Shift+X / Y / Z — lock an axis"},
  {"brush", "tool_brush", "Brush (M). Shift — smooth; with curves selected only their anchors; Ctrl+RMB: ←→ radius, ↑↓ hardness"},
};
const Def CURVES[] = {
  {"draw", "tool_draw", "Curve (C): draw a curve; Esc — finish. Mode Edges (V): a mesh's edges picked and made into curves. Mode Lock (Active Tool): a flat lock around the curve — the circle at the cursor is its width (Ctrl + wheel / Ctrl+RMB drag), two anchors selected before — it starts between them"},
  {"cut", "tool_cut", "Scissors (K): RMB on a curve picks it (or select it before), then drag a cut line across it → pie: split into two / split and delete the upper or the lower part. Modes in the tool panel: NURBS surface (cut a wall), Split ends (a click splits a curve's end into a Y)"},
  {"modifier", "tool_curvemods", "Curve modifiers (A): Profile — sweep a cross-section along a curve, then convert it to curves and walls; Extra — mask, thickness, twist, noise, curls on top of it, and Cap (close the open ends of the locks)"},
  {"braid", "tool_braid", "Braid: select a curve and press Create braid — strands woven along it (classic, Dutch, fishtail, rope), each strand's cross-section editable; the shape along it, sharpened lobes, squashed crossings; the ends separate (a movable connector round each tube), shared (one tube) or a point; out of it — separate tubes, a mesh on Convert, or locks"},
  {"crown", "tool_crown", "Crown (S): the top of the head around the parting — click on the head where the whorl is, or press Create"},
  {"lockex", "tool_lockex", "Lock Extrude: click a flat lock (Shift — add one more), then LMB ←→ — extrude it away from the head / toward it (its tip stays welded), ↑↓ — the back's segments (curves in its middle, standing out: one — a pentagon across); RMB ←→ — the thickness narrowing at the root, ↑↓ — narrowing toward the tip. Another tool or another object — it is done"},
  {"lockfix", "tool_lockfix", "Lock lines: the lock's curves found on its mesh — by the polypaint's contrast lines, the hard edges or the creases; the ends found and sewn. Settings: the mode, a cocoon, Generate"},
};
const Def SHELL = {"guide", "tool_shellmods", "Shell modifiers (G): on a cocoon, shaping its rings — Topology: Density (more / fewer rings where painted), Parent influence (the parent's pull weakened / added where painted); Guidelines: a line across the lock or a NURBS disc across the cocoon — a ring lies on it"};
}  // namespace

const std::vector<ToolBox::Tool>& ToolBox::ringTools() {
  static const std::vector<Tool> t = {{"cursor", "Cursor", "tool_cursor"}, {"move", "Move", "tool_move"}, {"rotate", "Rotate", "tool_rotate"},
    {"gizmo", "Scale", "tool_scale"}, {"brush", "Brush", "tool_brush"}, {"draw", "Curve", "tool_draw"}, {"cut", "Scissors", "tool_cut"},
    {"modifier", "Curve modifiers", "tool_curvemods"}, {"guide", "Shell modifiers", "tool_shellmods"}, {"crown", "Crown", "tool_crown"}, {"braid", "Braid", "tool_braid"},
    {"lockex", "Lock Extrude", "tool_lockex"}, {"lockfix", "Lock lines", "tool_lockfix"}};
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
  for (QPushButton* b : {ab, su}) b->setStyleSheet("font-size:10px;padding:0 5px");   // (both names whole in the column's width)
  top->addWidget(sel); top->addStretch(1); top->addWidget(ab); top->addWidget(su);
  v->addLayout(top);
  connect(sel, &QPushButton::clicked, this, &ToolBox::selCopy);
  connect(ab, &QPushButton::clicked, this, &ToolBox::about);
  connect(su, &QPushButton::clicked, this, &ToolBox::support);

// (the tools: in a widget of their own — the Hair room floats it as a window, as the Topology room's tools are)
  rows_ = new QWidget; auto* rl = new QVBoxLayout(rows_); rl->setContentsMargins(0, 0, 0, 0); rl->setSpacing(4); v->addWidget(rows_);
  auto row = [&](const Def* defs, int n, const char* label, bool withCocoon) {
    if (label) { auto* l = new QLabel(label); l->setStyleSheet("font-size:10px;color:#8a8f96;letter-spacing:1px;margin:3px 0 0 2px"); rl->addWidget(l); }
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
      if (i > 0 && i % 5 == 0) { h->addStretch(1); rl->addLayout(h); h = new QHBoxLayout; h->setSpacing(3); }   // (five to a row)
      auto* b = tbtn(defs[i].icon, tr(defs[i].tip)); b->setCheckable(true);
      QString id = defs[i].id; btns_[id] = b; b->setProperty("hmAction", id);   // (End over it: its key is the action's)
      connect(b, &QPushButton::clicked, this, [this, id] { emit toolPicked(id); });
      h->addWidget(b);
    }
    h->addStretch(1); rl->addLayout(h);
  };
  row(TOP, int(std::size(TOP)), nullptr, false);
  row(CURVES, int(std::size(CURVES)), "Curves", false);   // (every tool of the list — a count by hand dropped the last one added)
  row(&SHELL, 1, "Cocoons", true);
  // Snap: the anchors being dragged stick to the reference meshes' surface
  { auto* l = new QLabel("Snapping · Symmetry"); l->setStyleSheet("font-size:10px;color:#8a8f96;letter-spacing:1px;margin:3px 0 0 2px"); rl->addWidget(l);
    auto* h = new QHBoxLayout; h->setSpacing(3);
    snap_ = new QPushButton; snap_->setProperty("tool", true); snap_->setFixedSize(40, 40); snap_->setIcon(ui::icon("topo_snap")); snap_->setIconSize(QSize(34, 34)); snap_->setCheckable(true); snap_->setFocusPolicy(Qt::NoFocus);
    snap_->setToolTip(tr("Snap: the anchors being dragged stick to the surface of the reference meshes (a mesh's settings → Snapping → Reference; its snap mask, if on, limits where)"));
    connect(snap_, &QPushButton::clicked, this, [this] { emit snapClicked(); });
    h->addWidget(snap_);
    // (Symmetry — Shift+S, the same as the Topology room's: the dragged anchors' twins follow mirrored; its settings by it)
    sym_ = new QPushButton; sym_->setProperty("tool", true); sym_->setFixedSize(40, 40); sym_->setIcon(ui::icon("topo_sym")); sym_->setIconSize(QSize(34, 34)); sym_->setCheckable(true); sym_->setFocusPolicy(Qt::NoFocus);
    sym_->setToolTip(tr("Symmetry (Shift+S): an anchor dragged — its twin (the anchor where the mirror puts it) follows mirrored across the head's centre; the planes shown. Its settings: the button by it"));
    connect(sym_, &QPushButton::clicked, this, [this] { emit symClicked(); });
    symSet_ = new QPushButton; symSet_->setProperty("tool", true); symSet_->setFixedSize(40, 40); symSet_->setIcon(ui::icon("topo_snapsettings")); symSet_->setIconSize(QSize(34, 34)); symSet_->setFocusPolicy(Qt::NoFocus);
    symSet_->setToolTip(tr("Symmetry settings: planar / topology / radial, the axes X Y Z, the space (Local — the head's centre), Restrict, the tolerance, the planes shown"));
    connect(symSet_, &QPushButton::clicked, this, [this] { emit symSettings(symSet_); });
    h->addWidget(sym_); h->addWidget(symSet_); h->addStretch(1); rl->addLayout(h); }
  setCurrent("cursor");
}
void ToolBox::setSnap(bool on) { if (snap_) snap_->setChecked(on); }
void ToolBox::setSym(bool on) { if (sym_) sym_->setChecked(on); }

void ToolBox::setCurrent(const QString& tool) { const QString t = tool == "edge" ? QString("draw") : tool;   // (Edges: a mode of the Curve tool)
  for (auto it = btns_.begin(); it != btns_.end(); ++it) it.value()->setChecked(it.key() == t); }

QString ToolBox::tip(const QString& tool) const { auto* b = btns_.value(tool); return b ? b->toolTip() : QString(); }

void ToolBox::setRoom(const QString& room) {
  // (the Topology room: its tools in a floating window of their own — here only ⧉ SEL and the author's links stay)
  bool hair = room == "hair"; if (rows_) rows_->setVisible(hair);
  for (auto* b : btns_) b->setVisible(hair);
  if (cocoon_) cocoon_->setVisible(hair);
  if (snap_) snap_->setVisible(hair); if (sym_) sym_->setVisible(hair); if (symSet_) symSet_->setVisible(hair);
  for (auto* l : findChildren<QLabel*>()) l->setVisible(hair);
}

}  // namespace hm::ui

namespace hm::ui {

HairToolWin::HairToolWin(QWidget* window, QWidget* rows) : QFrame(window, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
  setObjectName("htoolWin"); setAttribute(Qt::WA_TranslucentBackground);
  setStyleSheet("#htoolWin{background:transparent} #htPane{background:#262626;border:1px solid #555;border-radius:6px} #htbar{background:#2b2b2b;border-bottom:1px solid #3a3a3a;border-top-left-radius:6px;border-top-right-radius:6px}"
                " #htHead{color:#8a8f96;font-size:10px;letter-spacing:1px} QSplitter::handle{background:#333} QSplitter::handle:hover{background:#4a6fb0}");
  auto* outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
  auto* pane = new QFrame; pane->setObjectName("htPane"); outer->addWidget(pane);
  auto* v = new QVBoxLayout(pane); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
  bar_ = new QWidget; bar_->setObjectName("htbar"); bar_->setFixedHeight(14); bar_->setCursor(Qt::SizeAllCursor); bar_->installEventFilter(this);
  bar_->setToolTip(tr("Drag — move the ToolBar")); v->addWidget(bar_);
  auto head = [](const QString& t) { auto* l = new QLabel(t); l->setObjectName("htHead"); return l; };
  split_ = new QSplitter(Qt::Horizontal); split_->setChildrenCollapsible(false); split_->setHandleWidth(3); v->addWidget(split_, 1);
  // ---- the tools ----
  auto* left = new QWidget; auto* lv = new QVBoxLayout(left); lv->setContentsMargins(6, 4, 6, 6); lv->setSpacing(3);
  lv->addWidget(head(tr("TOOLS"))); rows->setParent(left); lv->addWidget(rows); rows->show(); lv->addStretch(1);
  left->setFixedWidth(left->sizeHint().width());   // (no empty strip between the tools and the options: the tools column is as wide as its five buttons)
  split_->addWidget(left);
  // ---- the options of the tool in use (scrolled when the window is short) ----
  auto* right = new QWidget; auto* rv = new QVBoxLayout(right); rv->setContentsMargins(6, 4, 4, 4); rv->setSpacing(2);
  { auto* hr = new QHBoxLayout; hr->setContentsMargins(0, 0, 0, 0); hr->addWidget(head(tr("TOOL OPTIONS"))); hr->addStretch(1);
    help_ = new QPushButton("?"); help_->setFixedSize(16, 16); help_->setCursor(Qt::PointingHandCursor); help_->setFocusPolicy(Qt::NoFocus);
    help_->setStyleSheet("QPushButton{border:1px solid #666;border-radius:8px;padding:0;font-size:9px;color:#aaa;background:none}QPushButton:hover{color:#fff;border-color:#aaa}");
    connect(help_, &QPushButton::clicked, this, [this] { QToolTip::showText(help_->mapToGlobal(QPoint(0, help_->height())), help_->toolTip(), help_); });
    help_->hide(); hr->addWidget(help_); rv->addLayout(hr); }
  sa_ = new QScrollArea; sa_->setWidgetResizable(true); sa_->setFrameShape(QFrame::NoFrame); sa_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  sa_->setStyleSheet("QScrollArea{background:transparent} QScrollArea > QWidget > QWidget{background:transparent}");
  rv->addWidget(sa_, 1); right->setMinimumWidth(200);
  split_->addWidget(right); split_->setStretchFactor(0, 0); split_->setStretchFactor(1, 1);
  // ---- the corner grip: the window's size ----
  auto* gr = new QHBoxLayout; gr->setContentsMargins(0, 0, 2, 2); gr->addStretch(1); auto* grip = new QSizeGrip(pane); grip->setFixedSize(12, 12); gr->addWidget(grip); v->addLayout(gr);
  setMinimumSize(left->sizeHint().width() + 210, std::max(180, left->sizeHint().height() + 34));   // (every row of tools seen)
  const QJsonObject st = QJsonDocument::fromJson(QSettings().value("hair/tools").toByteArray()).object();
  resize(std::max(st.value("w").toInt(left->sizeHint().width() + 370), minimumWidth()), std::max(st.value("h").toInt(420), minimumHeight()));
  split_->setSizes({left->sizeHint().width(), 360});
  connect(split_, &QSplitter::splitterMoved, this, [this] { save(); });
  setOptions(nullptr);
}
void HairToolWin::setOptions(QWidget* w) {
  if (!w) { w = new QLabel(tr("This tool has no options")); w->setStyleSheet("color:#8a8f96;font-size:11px;margin:4px 2px"); static_cast<QLabel*>(w)->setAlignment(Qt::AlignLeft | Qt::AlignTop); }
  QWidget* old = sa_->takeWidget(); sa_->setWidget(w); w->show(); if (old) old->deleteLater();
  // (the grey paragraphs of 90+ characters leave the window — under the ? on its title, as in the docked panels)
  helpText_.clear(); for (QLabel* l : w->findChildren<QLabel*>()) { if (!l->property("mut").toBool() || !l->findChildren<QWidget*>().isEmpty()) continue; const QString s = l->text().trimmed(); if (s.size() < 90 || Qt::mightBeRichText(s)) continue; helpText_ << s; l->hide(); }
  if (help_) { help_->setVisible(!helpText_.isEmpty()); help_->setToolTip("<p style='white-space:pre-wrap'>" + helpText_.join(QStringLiteral("\n\n")).toHtmlEscaped() + "</p>"); }
}
void HairToolWin::resizeEvent(QResizeEvent* e) { QFrame::resizeEvent(e); if (isVisible()) save(); }   // (its size kept)
void HairToolWin::save() const {
  const QPoint p = pos() - parentWidget()->mapToGlobal(QPoint(0, 0));
  QJsonArray sp; if (split_) for (int s : split_->sizes()) sp.append(s);
  QSettings().setValue("hair/tools", QJsonDocument(QJsonObject{{"x", p.x()}, {"y", p.y()}, {"w", width()}, {"h", height()}, {"split", sp}}).toJson(QJsonDocument::Compact));
}
void HairToolWin::place(QWidget* vpBox) {
  const QJsonObject st = QJsonDocument::fromJson(QSettings().value("hair/tools").toByteArray()).object();
  const QList<QVariant> old = QSettings().value("hair/toolsPos").toList();          // (the place the tools-only window had)
  QWidget* w = parentWidget(); const QPoint o = w->mapToGlobal(QPoint(0, 0));
  QPoint p = st.contains("x") ? o + QPoint(st.value("x").toInt(), st.value("y").toInt()) : old.size() == 2 ? o + QPoint(old[0].toInt(), old[1].toInt()) : vpBox->mapToGlobal(QPoint(10, 34));
  const QRect g(o, w->size()); p.setX(std::clamp(p.x(), g.left(), g.right() - 60)); p.setY(std::clamp(p.y(), g.top(), g.bottom() - 40));
  move(p);
}
bool HairToolWin::eventFilter(QObject* o, QEvent* e) {
  if (o != bar_) return QFrame::eventFilter(o, e);
  if (e->type() == QEvent::MouseButtonPress) { auto* m = static_cast<QMouseEvent*>(e); if (m->button() == Qt::LeftButton) { drag_ = true; grab_ = m->globalPosition().toPoint() - pos(); } return true; }
  if (e->type() == QEvent::MouseMove && drag_) { move(static_cast<QMouseEvent*>(e)->globalPosition().toPoint() - grab_); return true; }
  if (e->type() == QEvent::MouseButtonRelease && drag_) { drag_ = false; save(); return true; }
  return QFrame::eventFilter(o, e);
}

}  // namespace hm::ui
