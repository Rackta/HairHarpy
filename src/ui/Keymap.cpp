#include "ui/Keymap.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDrag>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <cmath>
#include <map>
#include <QVBoxLayout>

namespace hm::ui {

Keymap& Keymap::get() { static Keymap k; return k; }

Keymap::Keymap() {
  defs_ = {
    {"cursor", "Cursor / previous tool", "KeyQ", false, "Tools"},
    {"move", "Move", "KeyE", false, "Tools"}, {"rotate", "Rotate", "KeyR", false, "Tools"}, {"gizmo", "Scale", "KeyT", false, "Tools"},
    {"draw", "Draw curve", "KeyC", false, "Tools"}, {"cut", "Scissors (cut a curve)", "KeyK", false, "Tools"}, {"brush", "Brush", "KeyM", false, "Tools"},
    {"guide", "Shell modifiers", "KeyG", false, "Tools"}, {"modifier", "Curve modifiers", "KeyA", false, "Tools"}, {"crown", "Crown", "KeyS", false, "Tools"}, {"lockex", "Lock Extrude", "", false, "Tools"}, {"braid", "Braid", "", false, "Tools"},
    {"edge", "Edges (select edges on a mesh)", "KeyV", false, "Tools"},
    {"brushSmaller", "Brush smaller", "BracketLeft", true, "Tools"}, {"brushBigger", "Brush bigger", "BracketRight", true, "Tools"},
    {"gizmoSpace", "Gizmo: local / world axes", "KeyW", false, "Gizmo"},
    {"axisX", "Gizmo: lock / unlock the X axis", "KeyX", false, "Gizmo"}, {"axisY", "Gizmo: lock / unlock the Y axis", "KeyY", false, "Gizmo"},
    {"axisZ", "Gizmo: lock / unlock the Z axis", "KeyZ", false, "Gizmo"},
    {"toolPie", "Tool pie (hold, release towards a tool — or click one)", "Space", false, "Pie menus"},
    {"mainPie", "Main pie at the pointer (as Shift + RMB)", "", false, "Pie menus"},
    {"isolatePie", "Hiding / isolating pie (as H held)", "", false, "Pie menus"},
    {"topoOpsPie", "Topology: the edits pie (as Ctrl + RMB)", "", false, "Pie menus"},
    {"camMenu", "Camera menu + split screen (double press — the active window alone)", "Tab", false, "View"},
    {"frame", "Frame selection (over the outliner: reveal it there)", "KeyF", false, "View"},
    {"hide", "Hide / show selected", "KeyH", false, "Selection & visibility"}, {"isolate", "Isolate selection", "Ctrl+KeyH", false, "Selection & visibility"},
    {"isolateHead", "Isolate selection + head", "Shift+KeyH", false, "Selection & visibility"}, {"isolateGroup", "Isolate the working group", "Ctrl+Shift+KeyH", false, "Selection & visibility"},
    {"deselect", "Deselect (the Topology room)", "Ctrl+KeyD", false, "Selection & visibility"},
    {"selAll", "Select all (Topology: the mode's components)", "Ctrl+KeyA", false, "Selection & visibility"}, {"invertSel", "Invert the selection (Topology)", "Ctrl+KeyI", false, "Selection & visibility"},
    {"copySel", "Copy the selection — the curves for ⇶ Snap to path, their data to the clipboard (as ⧉ SEL)", "Ctrl+KeyC", false, "Selection & visibility"},
    {"duplicate", "Duplicate", "KeyD", false, "Edit"}, {"del", "Delete", "Delete", false, "Edit"},
    {"groupSel", "Group the selected (a new group in their place)", "Alt+KeyG", false, "Edit"}, {"parent", "Parent to the last selected", "KeyP", false, "Edit"}, {"unparent", "Unparent (out of hierarchy)", "Shift+KeyP", false, "Edit"},
    {"rename", "Rename", "F2", false, "Edit"}, {"smartRename", "Smart Rename (tidy names, prefixes, find & replace, sort)", "Shift+F2", false, "Edit"},
    {"relax", "Relax the mesh (Topology)", "Ctrl+KeyR", false, "Edit"},
    {"symmetry", "Symmetry on / off (both rooms; the Topology room's UV too) — its settings by its button", "Shift+KeyX", false, "Edit"},
    {"undo", "Undo", "Ctrl+KeyZ", true, "Edit"}, {"redo", "Redo", "Ctrl+Shift+KeyZ", true, "Edit"}, {"redo2", "Redo (alternative)", "Ctrl+KeyY", true, "Edit"},
    {"save", "Save project", "Ctrl+KeyS", false, "Files"}, {"saveAs", "Save project as", "Ctrl+Shift+KeyS", false, "Files"},
    {"open", "File window (open / migrate)", "Ctrl+KeyO", false, "Files"},
    {"bridgeSend", "Send the meshes over the bridge (⇪ Send; set up in 🔗 Bridge)", "Ctrl+Shift+KeyE", false, "Files"},
  };
  load();
}

// ---- kept in the settings ----
void Keymap::load() {
  { QSettings st; st.beginGroup("keys"); for (auto& d : defs_) keys_[d.id] = st.contains(d.id) ? st.value(d.id).toString() : d.defKey; }
  if (keys_.value("braid") == "KeyB") keys_["braid"] = QString();   // (B is held for an anchor's smoothness — the braid's old key kept from earlier settings dropped)
  const QJsonObject x = QJsonDocument::fromJson(QSettings().value("keysExtra").toByteArray()).object();
  custom_.clear(); pies_.clear(); nav_ = NavScheme();
  for (const QJsonValue& v : x.value("custom").toArray()) { const QJsonObject o = v.toObject(); custom_.push_back({o.value("id").toString(), o.value("label").toString(), o.value("text").toString(), o.value("tip").toString(), o.value("combo").toString(), o.value("tool").toString()}); }
  for (const QJsonValue& v : x.value("pies").toArray()) { const QJsonObject o = v.toObject(); CustomPie p{o.value("id").toString(), o.value("name").toString(), o.value("combo").toString(), {}};
    for (const QJsonValue& i : o.value("items").toArray()) p.items << i.toString(); pies_.push_back(p); }
  if (x.value("nav").isObject()) {
    const QJsonObject n = x.value("nav").toObject(); auto b = [&](const char* k, NavBind& t) { const QJsonArray a = n.value(k).toArray(); if (a.size() == 2) t = {a[0].toInt(), a[1].toInt()}; };
    nav_.preset = n.value("preset").toString("standard"); b("orbit", nav_.orbit); b("pan", nav_.pan); b("pan2", nav_.pan2); b("zoom", nav_.zoom); b("mainPie", nav_.mainPie); b("opsPie", nav_.opsPie);
    nav_.rotation = n.value("rotation").toString("turntable"); }
}
void Keymap::save() const {
  { QSettings st; st.beginGroup("keys"); for (auto& d : defs_) st.setValue(d.id, keys_.value(d.id)); }
  QJsonObject x = toJson(); x.remove("keys"); QSettings().setValue("keysExtra", QJsonDocument(x).toJson(QJsonDocument::Compact));
}
QJsonObject Keymap::toJson() const {
  QJsonObject k; for (auto& d : defs_) k.insert(d.id, keys_.value(d.id));
  QJsonArray c; for (auto& b : custom_) c.append(QJsonObject{{"id", b.id}, {"label", b.label}, {"text", b.text}, {"tip", b.tip}, {"combo", b.combo}, {"tool", b.tool}});
  QJsonArray p; for (auto& q : pies_) p.append(QJsonObject{{"id", q.id}, {"name", q.name}, {"combo", q.combo}, {"items", QJsonArray::fromStringList(q.items)}});
  auto b = [](const NavBind& t) { return QJsonArray{t.button, t.mods}; };
  QJsonObject n{{"preset", nav_.preset}, {"orbit", b(nav_.orbit)}, {"pan", b(nav_.pan)}, {"pan2", b(nav_.pan2)}, {"zoom", b(nav_.zoom)}, {"mainPie", b(nav_.mainPie)}, {"opsPie", b(nav_.opsPie)}, {"rotation", nav_.rotation}};
  return QJsonObject{{"app", "hmc-hotkeys"}, {"version", 1}, {"keys", k}, {"custom", c}, {"pies", p}, {"nav", n}};
}
bool Keymap::fromJson(const QJsonObject& o) {
  if (o.value("app").toString() != "hmc-hotkeys") return false;
  const QJsonObject k = o.value("keys").toObject();
  { QSettings st; st.beginGroup("keys"); for (auto& d : defs_) st.setValue(d.id, k.contains(d.id) ? k.value(d.id).toString() : d.defKey); }
  QJsonObject x = o; x.remove("keys"); QSettings().setValue("keysExtra", QJsonDocument(x).toJson(QJsonDocument::Compact));
  load(); return true;
}

const ActionDef* Keymap::def(const QString& id) const { for (auto& d : defs_) if (d.id == id) return &d; return nullptr; }
QString Keymap::key(const QString& id) const {
  if (keys_.contains(id)) return keys_.value(id);
  for (auto& b : custom_) if (b.id == id) return b.combo;
  for (auto& p : pies_) if (p.id == id) return p.combo;
  return {};
}
QString Keymap::labelOf(const QString& id) const {
  if (auto* d = def(id)) return d->label;
  for (auto& b : custom_) if (b.id == id) return b.label;
  for (auto& p : pies_) if (p.id == id) return QObject::tr("Pie: %1").arg(p.name);
  return id;
}
QStringList Keymap::categories() const { QStringList c; for (auto& d : defs_) if (!c.contains(d.cat)) c << d.cat; return c; }

QString Keymap::clashWith(const QString& id, const QString& combo) const {
  if (combo.isEmpty()) return {};
  for (auto& d : defs_) if (d.id != id && keys_.value(d.id) == combo) return d.id;
  for (auto& b : custom_) if (b.id != id && b.combo == combo) return b.id;
  for (auto& p : pies_) if (p.id != id && p.combo == combo) return p.id;
  return {};
}
void Keymap::setKey(const QString& id, const QString& combo) {
  if (!combo.isEmpty() && !assignable(combo)) return;
  const QString c = clashWith(id, combo);
  auto put = [&](const QString& x, const QString& v) { if (keys_.contains(x)) keys_[x] = v; for (auto& b : custom_) if (b.id == x) b.combo = v; for (auto& p : pies_) if (p.id == x) p.combo = v; };
  if (!c.isEmpty()) put(c, QString());
  put(id, combo); save();
}
void Keymap::reset() { for (auto& d : defs_) keys_[d.id] = d.defKey; save(); }
void Keymap::factoryReset() { for (auto& d : defs_) keys_[d.id] = d.defKey; custom_.clear(); pies_.clear(); nav_ = NavScheme(); save(); }
QString Keymap::actionFor(const QString& combo) const {
  if (combo.isEmpty()) return {};
  for (auto& d : defs_) if (keys_.value(d.id) == combo) return d.id;
  for (auto& b : custom_) if (b.combo == combo) return b.id;
  for (auto& p : pies_) if (p.combo == combo) return p.id;
  return {};
}
bool Keymap::assignable(const QString& combo) { const QString c = combo.section('+', -1); return !combo.isEmpty() && c != "Escape" && c != "End"; }

// ---- keys on buttons ----
QString Keymap::buttonId(const QString& label, const QString& text, const QString& tip, const QString& tool) {
  QString id; for (auto& b : custom_) if (b.text == text && b.tip == tip) { id = b.id; if (!tool.isEmpty() && b.tool != tool) { b.tool = tool; save(); } }
  if (id.isEmpty()) { int n = 1; for (auto& b : custom_) n = std::max(n, b.id.mid(2).toInt() + 1); id = QString("c:%1").arg(n); custom_.push_back({id, label, text, tip, QString(), tool}); save(); }
  return id;
}
QString Keymap::bindButton(const QString& label, const QString& text, const QString& tip, const QString& combo, const QString& tool) {
  const QString id = buttonId(label, text, tip, tool); setKey(id, combo); return id;
}
void Keymap::setCustomTool(const QString& id, const QString& tool) { for (auto& b : custom_) if (b.id == id && b.tool != tool) { b.tool = tool; save(); } }
void Keymap::removeCustom(const QString& id) { custom_.erase(std::remove_if(custom_.begin(), custom_.end(), [&](auto& b) { return b.id == id; }), custom_.end()); save(); }
QAbstractButton* Keymap::customButton(const QString& id, bool justMade) const {
  const CustomBind* c = customBind(id); if (!c) return nullptr;
  // (just made: not shown yet — none on its way up to its shown window hidden on purpose)
  auto willShow = [](QWidget* b) { for (QWidget* p = b; p; p = p->parentWidget()) { if (p->isWindow()) return p->isVisible(); if (p->isHidden() && p->testAttribute(Qt::WA_WState_ExplicitShowHide)) return false; } return false; };
  for (QWidget* w : QApplication::allWidgets()) { auto* b = qobject_cast<QAbstractButton*>(w); if (!b || !(justMade ? willShow(b) : b->isVisible()) || !b->isEnabled()) continue;
    if (b->text() == c->text && b->toolTip() == c->tip) return b; }
  return nullptr;
}
bool Keymap::triggerCustom(const QString& id, bool justMade) const { if (auto* b = customButton(id, justMade)) { b->click(); return true; } return false; }
void Keymap::setPies(const std::vector<CustomPie>& p) { pies_ = p; save(); }

// ---- the mouse ----
void Keymap::setNav(const NavScheme& n) { nav_ = n; save(); }
NavScheme Keymap::navPreset(const QString& name) {
  NavScheme s; s.preset = name;
  if (name == "blender") { s.orbit = {2, 0}; s.pan = {2, 2}; s.pan2 = {0, 0}; s.zoom = {2, 1}; }
  else if (name == "maya") { s.orbit = {1, 4}; s.pan = {2, 4}; s.pan2 = {0, 0}; s.zoom = {3, 4}; }
  else if (name == "max") { s.orbit = {2, 4}; s.pan = {2, 0}; s.pan2 = {0, 0}; s.zoom = {2, 5}; }
  return s;
}
int Keymap::buttonOf(int b) { return b == Qt::LeftButton ? 1 : b == Qt::MiddleButton ? 2 : b == Qt::RightButton ? 3 : 0; }
int Keymap::modsOf(int m) { return (m & Qt::ControlModifier ? 1 : 0) | (m & Qt::ShiftModifier ? 2 : 0) | (m & Qt::AltModifier ? 4 : 0); }
int Keymap::navFor(int button, int mods) const {
  int best = 0, bestBits = -1;
  auto take = [&](const NavBind& b, int what) { if (!b.button || b.button != button || (mods & b.mods) != b.mods) return;
    if ((mods & ~b.mods) && !(what == 1 && (mods & ~b.mods) == 2)) return;   // (extra keys held: only Shift on an orbit — it snaps to the views)
    int bits = 0; for (int x = b.mods; x; x >>= 1) bits += x & 1; if (bits > bestBits) { bestBits = bits; best = what; } };
  take(nav_.orbit, 1); take(nav_.pan, 2); take(nav_.pan2, 2); take(nav_.zoom, 3);
  return best;
}
bool Keymap::pieMouse(const QString& which, int button, int mods) const { const NavBind& b = which == "main" ? nav_.mainPie : nav_.opsPie; return b.button && b.button == button && (mods & b.mods) == b.mods && (b.mods || !mods); }
QString Keymap::navLabel(const NavBind& b) {
  if (!b.button) return QObject::tr("—");
  QString s; if (b.mods & 1) s += "Ctrl + "; if (b.mods & 2) s += "Shift + "; if (b.mods & 4) s += "Alt + ";
  return s + (b.button == 1 ? "LMB" : b.button == 2 ? "MMB" : "RMB");
}

// (Windows virtual keys: the same for a letter key in any layout — as the browser's e.code)
QString Keymap::codeOf(const QKeyEvent* e) {
  quint32 vk = e->nativeVirtualKey();
  if (vk >= 'A' && vk <= 'Z') return QString("Key%1").arg(QChar(char(vk)));
  if (vk >= '0' && vk <= '9') return QString("Digit%1").arg(QChar(char(vk)));
  if (vk >= 0x70 && vk <= 0x87) return QString("F%1").arg(vk - 0x6f);
  switch (vk) {
    case 0x20: return "Space"; case 0x09: return "Tab"; case 0x2e: return "Delete"; case 0x08: return "Backspace";
    case 0x0d: return "Enter"; case 0x1b: return "Escape"; case 0xdb: return "BracketLeft"; case 0xdd: return "BracketRight";
    case 0xbd: return "Minus"; case 0xbb: return "Equal"; case 0xba: return "Semicolon"; case 0xde: return "Quote";
    case 0xbc: return "Comma"; case 0xbe: return "Period"; case 0xbf: return "Slash"; case 0xdc: return "Backslash";
    case 0xc0: return "Backquote"; case 0x2d: return "Insert"; case 0x24: return "Home"; case 0x23: return "End";
    case 0x21: return "PageUp"; case 0x22: return "PageDown"; case 0x25: return "ArrowLeft"; case 0x26: return "ArrowUp";
    case 0x27: return "ArrowRight"; case 0x28: return "ArrowDown";
    default: break;
  }
  int k = e->key();                                              // (no native key — a synthetic event: Qt's key)
  if (k >= Qt::Key_A && k <= Qt::Key_Z) return QString("Key%1").arg(QChar(char('A' + k - Qt::Key_A)));
  if (k >= Qt::Key_0 && k <= Qt::Key_9) return QString("Digit%1").arg(QChar(char('0' + k - Qt::Key_0)));
  if (k >= Qt::Key_F1 && k <= Qt::Key_F24) return QString("F%1").arg(k - Qt::Key_F1 + 1);
  switch (k) {
    case Qt::Key_Space: return "Space"; case Qt::Key_Tab: return "Tab"; case Qt::Key_Delete: return "Delete";
    case Qt::Key_Backspace: return "Backspace"; case Qt::Key_Return: case Qt::Key_Enter: return "Enter";
    case Qt::Key_Escape: return "Escape"; case Qt::Key_BracketLeft: return "BracketLeft"; case Qt::Key_BracketRight: return "BracketRight";
    case Qt::Key_End: return "End"; case Qt::Key_Home: return "Home";
    default: return {};
  }
}
QString Keymap::comboOf(const QKeyEvent* e) {
  QString c = codeOf(e); if (c.isEmpty()) return {};
  Qt::KeyboardModifiers m = e->modifiers();
  return QString(m & Qt::ControlModifier ? "Ctrl+" : "") + (m & Qt::ShiftModifier ? "Shift+" : "") + (m & Qt::AltModifier ? "Alt+" : "") + c;
}
QString Keymap::label(const QString& combo) {
  if (combo.isEmpty()) return QStringLiteral("—");
  QString s = combo;
  s.replace(QRegularExpression("Key([A-Z])"), "\\1"); s.replace(QRegularExpression("Digit(\\d)"), "\\1");
  s.replace("BracketLeft", "["); s.replace("BracketRight", "]");
  s.replace("+", " + ");
  return s;
}

// ======================= the hotkey manager =======================
namespace {
QLabel* mut(const QString& t) { auto* l = new QLabel(t); l->setWordWrap(true); l->setProperty("mut", true); return l; }
QLabel* head(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setStyleSheet("margin-top:8px"); return l; }
const char* PAGE_PIES = "Pie menus of my own";
const char* PAGE_BUTTONS = "Keys on buttons (End)";
const char* PAGE_MOUSE = "Mouse & camera";
const char* PAGE_PRESETS = "Presets & reset";
}  // namespace

HotkeyDialog::HotkeyDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Hotkeys")); resize(820, 680);
  auto* v = new QVBoxLayout(this);
  search_ = new QLineEdit; search_->setPlaceholderText(tr("🔍 Search an action or a key…")); search_->setClearButtonEnabled(true); v->addWidget(search_);
  auto* body = new QHBoxLayout; v->addLayout(body, 1);
  cats_ = new QListWidget; cats_->setFixedWidth(210);
  for (const QString& c : Keymap::get().categories()) cats_->addItem(c);
  for (const char* p : {PAGE_PIES, PAGE_BUTTONS, PAGE_MOUSE, PAGE_PRESETS}) cats_->addItem(tr(p));
  cats_->setCurrentRow(0); body->addWidget(cats_);
  auto* sa = new QScrollArea; sa->setWidgetResizable(true); sa->setFrameShape(QFrame::NoFrame);
  auto* w = new QWidget; rows_ = new QVBoxLayout(w); rows_->setSpacing(3); rows_->setContentsMargins(6, 0, 8, 0);
  sa->setWidget(w); body->addWidget(sa, 1);
  auto* bar = new QHBoxLayout;
  bar->addWidget(mut(tr("Hover any button or tool of the window and press End — then the keys for it. Esc and End can't be hotkeys.")), 1);
  auto* cl = new QPushButton(tr("Close")); bar->addWidget(cl); v->addLayout(bar);
  connect(cl, &QPushButton::clicked, this, &QDialog::accept);
  connect(cats_, &QListWidget::currentRowChanged, this, [this] { editPie_ = -1; fill(); });
  connect(search_, &QLineEdit::textChanged, this, [this] { fill(); });
  setAcceptDrops(true); qApp->installEventFilter(this);
  fill();
}

// ---- a button or tool of the window dragged in ----
namespace { const char* FUNC_MIME = "application/x-hmc-func"; }
bool HotkeyDialog::eventFilter(QObject* o, QEvent* e) {
  if (e->type() != QEvent::MouseButtonPress && e->type() != QEvent::MouseMove && e->type() != QEvent::MouseButtonRelease) return false;
  auto* w = qobject_cast<QWidget*>(o); if (!w || w->window() == this) return false;
  auto* me = static_cast<QMouseEvent*>(e);
  if (e->type() == QEvent::MouseButtonPress) {
    dragSrc_ = nullptr; if (me->button() != Qt::LeftButton) return false;
    for (QWidget* x = w; x; x = x->parentWidget()) if (x->property("hmAction").isValid() || qobject_cast<QAbstractButton*>(x)) { dragSrc_ = x; dragAt_ = me->globalPosition().toPoint(); break; }
    return false; }
  if (e->type() == QEvent::MouseButtonRelease) { dragSrc_ = nullptr; return false; }
  if (!dragSrc_ || !(me->buttons() & Qt::LeftButton) || (me->globalPosition().toPoint() - dragAt_).manhattanLength() < QApplication::startDragDistance()) return false;
  QWidget* x = dragSrc_; dragSrc_ = nullptr;
  QJsonObject j;
  if (x->property("hmAction").isValid() && (Keymap::get().def(x->property("hmAction").toString()) || !qobject_cast<QAbstractButton*>(x))) j.insert("action", x->property("hmAction").toString());
  else { auto* b = qobject_cast<QAbstractButton*>(x);
    QString name = b->text().trimmed(); if (name.isEmpty()) { name = b->toolTip().section('\n', 0, 0); name = name.section(" — ", 0, 0).section(": ", 0, 0).left(60); }
    if (name.isEmpty()) name = tr("a button");
    j = QJsonObject{{"label", name}, {"text", b->text()}, {"tip", b->toolTip()}, {"tool", Keymap::get().toolOf ? Keymap::get().toolOf(b) : QString()}}; }
  auto* md = new QMimeData; md->setData(FUNC_MIME, QJsonDocument(j).toJson(QJsonDocument::Compact));
  auto* dr = new QDrag(x); dr->setMimeData(md); const QPixmap px = x->grab(); if (!px.isNull()) { dr->setPixmap(px); dr->setHotSpot(QPoint(px.width() / 2, px.height() / 2)); }
  raise(); dr->exec(Qt::CopyAction);
  if (auto* b = qobject_cast<QAbstractButton*>(x)) b->setDown(false);
  return true;
}
void HotkeyDialog::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasFormat(FUNC_MIME) || e->mimeData()->hasFormat("application/x-hmc-pieslot")) e->acceptProposedAction(); }
void HotkeyDialog::dragMoveEvent(QDragMoveEvent* e) { if (e->mimeData()->hasFormat(FUNC_MIME) || e->mimeData()->hasFormat("application/x-hmc-pieslot")) e->acceptProposedAction(); }
void HotkeyDialog::dropEvent(QDropEvent* e) {
  if (e->mimeData()->hasFormat("application/x-hmc-pieslot")) {   // (a place of the ring dragged onto another: the two swap)
    int slot = -1; for (QWidget* c = childAt(e->position().toPoint()); c && c != this; c = c->parentWidget()) if (c->property("pieSlot").isValid()) { slot = c->property("pieSlot").toInt(); break; }
    e->acceptProposedAction(); if (slot >= 0) swapSlots(e->mimeData()->data("application/x-hmc-pieslot").toInt(), slot); return; }
  if (!e->mimeData()->hasFormat(FUNC_MIME)) return;
  const QJsonObject j = QJsonDocument::fromJson(e->mimeData()->data(FUNC_MIME)).object();
  const QString id = j.contains("action") ? j.value("action").toString() : Keymap::get().buttonId(j.value("label").toString(), j.value("text").toString(), j.value("tip").toString(), j.value("tool").toString());
  int slot = -1; for (QWidget* c = childAt(e->position().toPoint()); c && c != this; c = c->parentWidget()) if (c->property("pieSlot").isValid()) { slot = c->property("pieSlot").toInt(); break; }
  e->acceptProposedAction(); dropFunc(id, slot);
}
void HotkeyDialog::dropFunc(const QString& id, int slot) {
  auto P = Keymap::get().pies();
  if (editPie_ < 0 || editPie_ >= int(P.size())) {
    if (P.empty()) { P.push_back({"pie:1", tr("My pie %1").arg(1), QString(), {}}); }
    editPie_ = 0; }
  auto& it = P[size_t(editPie_)].items;
  if (slot < 0) { slot = 0; while (slot < it.size() && !it[slot].isEmpty()) slot++; }
  if (slot >= 8) { emit message(tr("“%1” is full — drop onto a slot to replace it").arg(P[size_t(editPie_)].name)); return; }
  while (it.size() <= slot) it << QString(); it[slot] = id; while (!it.isEmpty() && it.last().isEmpty()) it.removeLast();
  const int pie = editPie_; Keymap::get().setPies(P);
  emit message(tr("“%1” → %2, slot %3").arg(Keymap::get().labelOf(id), P[size_t(pie)].name).arg(slot + 1));
  const auto found = cats_->findItems(tr(PAGE_PIES), Qt::MatchExactly);
  { QSignalBlocker b1(cats_), b2(search_); search_->clear(); if (!found.isEmpty()) cats_->setCurrentItem(found.first()); }
  editPie_ = pie; fill(); activateWindow();
}

void HotkeyDialog::keyRow(QVBoxLayout* v, const QString& id, const QString& label, bool removable) {
  auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 0, 0, 0);
  auto* l = new QLabel(label); l->setWordWrap(true); h->addWidget(l, 1);
  auto* b = new QPushButton(Keymap::label(Keymap::get().key(id))); b->setMinimumWidth(150); h->addWidget(b);
  const ActionDef* d = Keymap::get().def(id);
  if (d && Keymap::get().key(id) != d->defKey) { b->setStyleSheet("color:#ffcf7a"); b->setToolTip(tr("Changed — the standard is %1").arg(Keymap::label(d->defKey))); }
  connect(b, &QPushButton::clicked, this, [this, b, id] {
    if (captureBtn_) captureBtn_->setStyleSheet({});
    capture_ = id; pending_.clear(); captureBtn_ = b; b->setText(tr("Press the keys…")); b->setStyleSheet("background:#6a4a1a;border-color:#c08030");
    onCaptured_ = [this, id](const QString& combo) { const QString clash = Keymap::get().clashWith(id, combo);
      if (!clash.isEmpty()) emit message(tr("\"%1\" lost its key").arg(Keymap::get().labelOf(clash))); Keymap::get().setKey(id, combo); };
    setFocus(); });
  if (removable) { auto* x = new QPushButton("✕"); x->setFixedWidth(28); x->setToolTip(tr("Forget this key")); h->addWidget(x);
    connect(x, &QPushButton::clicked, this, [this, id] { Keymap::get().removeCustom(id); fill(); }); }
  v->addWidget(r);
}

void HotkeyDialog::fill() {
  while (QLayoutItem* it = rows_->takeAt(0)) { if (QWidget* x = it->widget()) { x->hide(); x->setParent(nullptr); x->deleteLater(); } delete it; }
  captureBtn_ = nullptr; capture_.clear(); pending_.clear();
  auto& K = Keymap::get(); const QString q = search_->text().trimmed().toLower();
  if (!q.isEmpty()) {                                         // (a search: every key that matches, wherever it is)
    int n = 0;
    for (auto& d : K.defs()) if (d.label.toLower().contains(q) || Keymap::label(K.key(d.id)).toLower().contains(q) || d.cat.toLower().contains(q)) { keyRow(rows_, d.id, d.label + "   ·  " + d.cat, false); n++; }
    for (auto& b : K.custom()) if (b.label.toLower().contains(q) || Keymap::label(b.combo).toLower().contains(q)) { keyRow(rows_, b.id, b.label + "   ·  " + tr("button"), true); n++; }
    for (auto& p : K.pies()) if (p.name.toLower().contains(q) || Keymap::label(p.combo).toLower().contains(q)) { keyRow(rows_, p.id, tr("Pie: %1").arg(p.name), false); n++; }
    if (!n) rows_->addWidget(mut(tr("Nothing found.")));
    rows_->addStretch(1); return;
  }
  const QString page = cats_->currentItem() ? cats_->currentItem()->text() : QString();
  if (page == tr(PAGE_PIES)) piesPage(rows_);
  else if (page == tr(PAGE_BUTTONS)) {
    rows_->addWidget(head(tr(PAGE_BUTTONS)));
    rows_->addWidget(mut(tr("Any button or tool of the window can have a key: hover it, press End, press the keys and let go (Esc — cancel). They show here.")));
    if (K.custom().empty()) rows_->addWidget(mut(tr("None yet.")));
    for (auto& b : K.custom()) keyRow(rows_, b.id, b.label, true);
  }
  else if (page == tr(PAGE_MOUSE)) mousePage(rows_);
  else if (page == tr(PAGE_PRESETS)) presetsPage(rows_);
  else {
    rows_->addWidget(head(page));
    for (auto& d : K.defs()) if (d.cat == page) keyRow(rows_, d.id, d.label, false);
    if (page == "Selection & visibility") rows_->addWidget(mut(tr("Fixed: H tapped — hide / show, held — the hiding pie; B held — smoothness (Topology: Soft Selection); ~ — the help of what is under the pointer.")));
  }
  rows_->addStretch(1);
}

// ---- a pie of one's own laid out as it opens: the ring, eight places on it ----
namespace {
const char* SLOT_MIME = "application/x-hmc-pieslot";
// one place on the ring: its action's name (empty — a faint ＋); a drag takes it to another place (the two swap), a click
// picks its action from the list, RMB empties it; a button of the program dropped on it — put there
class SlotBtn : public QPushButton {
public:
  SlotBtn(int slot, const QString& id) : slot_(slot), id_(id) {
    setProperty("pieSlot", slot); setCursor(Qt::PointingHandCursor); setFixedSize(132, 34);
    setText(id.isEmpty() ? QStringLiteral("＋") : Keymap::get().labelOf(id)); setToolTip(id.isEmpty() ? QObject::tr("Empty — click to pick an action, or drop a button of the program here")
                                                                                                    : QObject::tr("%1 — drag to another place; click — another action; RMB — empty it").arg(Keymap::get().labelOf(id)));
    setStyleSheet(id.isEmpty() ? "QPushButton{background:rgba(255,255,255,0.03);border:1px dashed #555;border-radius:15px;color:#777;font-size:15px} QPushButton:hover{border-color:#7a9fe0;color:#cfe0ff}"
                               : "QPushButton{background:#353535;border:1px solid #5a5a5a;border-radius:15px;color:#e8e8e8;padding:0 8px} QPushButton:hover{background:#4a6fb0;border-color:#7a9fe0}");
    const QString t = fontMetrics().elidedText(text(), Qt::ElideRight, width() - 16); setText(t);
  }
  std::function<void(int)> pick, clear;
protected:
  void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::RightButton) { if (clear && !id_.isEmpty()) clear(slot_); return; } press_ = e->position().toPoint(); QPushButton::mousePressEvent(e); }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (id_.isEmpty() || !(e->buttons() & Qt::LeftButton) || (e->position().toPoint() - press_).manhattanLength() < QApplication::startDragDistance()) return;
    setDown(false); auto* md = new QMimeData; md->setData(SLOT_MIME, QByteArray::number(slot_));
    auto* d = new QDrag(this); d->setMimeData(md); const QPixmap px = grab(); d->setPixmap(px); d->setHotSpot(QPoint(px.width() / 2, px.height() / 2)); d->exec(Qt::MoveAction); dragged_ = true; }
  void mouseReleaseEvent(QMouseEvent* e) override { const bool was = dragged_; dragged_ = false; QPushButton::mouseReleaseEvent(e); if (!was && e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) && pick) pick(slot_); }
private:
  int slot_; QString id_; QPoint press_; bool dragged_ = false;
};
// the ring: the disk and its centre (the pie's name), the places round it — slot 1 at the top, on clockwise
class PieRing : public QWidget {
public:
  PieRing(const QString& name, const QStringList& items) : name_(name) {
    setFixedSize(470, 330);
    for (int s = 0; s < 8; s++) { auto* b = new SlotBtn(s, s < items.size() ? items[s] : QString()); b->setParent(this); slots_.push_back(b);
      const double a = -M_PI / 2 + s * M_PI / 4; const QPointF c(width() / 2.0 + std::cos(a) * 160, height() / 2.0 + std::sin(a) * 118);
      b->move(int(c.x() - b->width() / 2.0), int(c.y() - b->height() / 2.0)); }
  }
  std::vector<SlotBtn*> slots_;
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter g(this); g.setRenderHint(QPainter::Antialiasing); const QPointF c(width() / 2.0, height() / 2.0);
    g.setPen(QPen(QColor(255, 255, 255, 22), 1)); g.setBrush(QColor(20, 20, 20, 140)); g.drawEllipse(c, 226.0, 160.0);
    g.setPen(QPen(QColor(255, 255, 255, 14), 1)); for (int s = 0; s < 8; s++) { const double a = -M_PI / 2 + s * M_PI / 4; g.drawLine(c + QPointF(std::cos(a) * 26, std::sin(a) * 26), c + QPointF(std::cos(a) * 96, std::sin(a) * 70)); }
    g.setPen(QPen(QColor(0xbb, 0xbb, 0xbb), 2)); g.setBrush(QColor(0, 0, 0, 90)); g.drawEllipse(c, 12, 12);
    QFont f = font(); f.setPixelSize(11); g.setFont(f); g.setPen(QColor(0x99, 0x99, 0x99)); g.drawText(QRectF(c.x() - 120, c.y() + 18, 240, 18), Qt::AlignCenter, name_);
  }
private:
  QString name_;
};
}  // namespace

// a place of the pie being edited set to an action (empty — cleared), the page drawn anew
void HotkeyDialog::setSlot(int slot, const QString& id) {
  auto P = Keymap::get().pies(); if (editPie_ < 0 || editPie_ >= int(P.size()) || slot < 0 || slot >= 8) return;
  auto& it = P[size_t(editPie_)].items; while (it.size() <= slot) it << QString(); it[slot] = id; while (!it.isEmpty() && it.last().isEmpty()) it.removeLast();
  Keymap::get().setPies(P); QTimer::singleShot(0, this, [this] { fill(); });
}
void HotkeyDialog::swapSlots(int a, int b) {
  auto P = Keymap::get().pies(); if (editPie_ < 0 || editPie_ >= int(P.size()) || a == b || a < 0 || b < 0 || a >= 8 || b >= 8) return;
  auto& it = P[size_t(editPie_)].items; while (it.size() < 8) it << QString(); std::swap(it[a], it[b]); while (!it.isEmpty() && it.last().isEmpty()) it.removeLast();
  Keymap::get().setPies(P); QTimer::singleShot(0, this, [this] { fill(); });
}

// the pies of one's own: a name, a key, up to 8 actions round the ring
void HotkeyDialog::piesPage(QVBoxLayout* v) {
  auto& K = Keymap::get(); auto pies = K.pies();
  v->addWidget(head(tr(PAGE_PIES)));
  v->addWidget(mut(tr("A ring of up to 8 actions under a key of its own: held — release towards one, or click it.")));
  v->addWidget(mut(tr("While this window is open, drag any button or tool of the program onto a slot — or anywhere here: into the first empty slot.")));
  for (int i = 0; i < int(pies.size()); i++) {
    auto& p = pies[size_t(i)];
    auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(new QLabel(QString("◔ %1  (%2)").arg(p.name).arg(p.items.size())), 1);
    auto* ed = new QPushButton(editPie_ == i ? tr("Done") : tr("Edit")); h->addWidget(ed);
    connect(ed, &QPushButton::clicked, this, [this, i] { editPie_ = editPie_ == i ? -1 : i; fill(); });
    v->addWidget(r);
    keyRow(v, p.id, tr("Its key"), false);
    if (editPie_ != i) continue;
    auto* name = new QLineEdit(p.name); v->addWidget(name);
    connect(name, &QLineEdit::editingFinished, this, [i, name] { auto P = Keymap::get().pies(); if (i < int(P.size())) { P[size_t(i)].name = name->text(); Keymap::get().setPies(P); } });
    // (the ring as it opens: each action at the place it will have — dragged round, picked, emptied)
    auto* ring = new PieRing(p.name, p.items); v->addWidget(ring, 0, Qt::AlignHCenter);
    for (SlotBtn* b : ring->slots_) {
      b->clear = [this](int s) { setSlot(s, QString()); };
      b->pick = [this, b](int s) {
        QMenu m(this); auto& Km = Keymap::get();
        m.addAction(tr("— Empty —"), this, [this, s] { setSlot(s, QString()); }); m.addSeparator();
        std::map<QString, QMenu*> cats;
        for (auto& d : Km.defs()) { QMenu*& sub = cats[d.cat]; if (!sub) sub = m.addMenu(d.cat); const QString id = d.id; sub->addAction(d.label, this, [this, s, id] { setSlot(s, id); }); }
        if (!Km.custom().empty()) { QMenu* sub = m.addMenu(tr("Buttons of the program")); for (auto& c : Km.custom()) { const QString id = c.id; sub->addAction(c.label, this, [this, s, id] { setSlot(s, id); }); } }
        m.exec(b->mapToGlobal(QPoint(0, b->height()))); };
    }
    v->addWidget(mut(tr("Drag an action round the ring to another place (the two swap); click a place — pick its action; RMB — empty it. A button of the program dropped on a place goes there.")));
    auto* del = new QPushButton(tr("Delete this pie")); v->addWidget(del);
    connect(del, &QPushButton::clicked, this, [this, i] { auto P = Keymap::get().pies(); if (i < int(P.size())) P.erase(P.begin() + i); Keymap::get().setPies(P); editPie_ = -1; fill(); });
  }
  auto* add = new QPushButton(tr("＋ New pie")); v->addWidget(add);
  connect(add, &QPushButton::clicked, this, [this] { auto P = Keymap::get().pies(); int n = 1; for (auto& p : P) n = std::max(n, p.id.mid(4).toInt() + 1);
    P.push_back({QString("pie:%1").arg(n), tr("My pie %1").arg(n), QString(), {}}); Keymap::get().setPies(P); editPie_ = int(P.size()) - 1; fill(); });
  v->addWidget(head(tr("The program's pies")));
  for (auto& d : K.defs()) if (d.cat == "Pie menus") keyRow(v, d.id, d.label, false);
  v->addWidget(mut(tr("Their mouse buttons (Shift + RMB, Ctrl + RMB) — in %1.").arg(tr(PAGE_MOUSE))));
}

// the camera's mouse buttons (a preset or one's own), its turning
void HotkeyDialog::mousePage(QVBoxLayout* v) {
  auto& K = Keymap::get(); const NavScheme n = K.nav();
  v->addWidget(head(tr("Camera controls")));
  auto* pr = new QComboBox;
  const std::vector<std::pair<QString, QString>> P{{"standard", tr("Standard (this program): Alt + LMB / MMB / RMB, MMB — pan")}, {"blender", tr("Blender-like: MMB orbit, Shift + MMB pan, Ctrl + MMB zoom")},
                                                   {"maya", tr("Maya-like: Alt + LMB orbit, Alt + MMB pan, Alt + RMB zoom")}, {"max", tr("3ds Max-like: Alt + MMB orbit, MMB pan, Ctrl + Alt + MMB zoom")}, {"custom", tr("My own (set below)")}};
  for (auto& [k, l] : P) pr->addItem(l, k);
  pr->setCurrentIndex(std::max(0, pr->findData(n.preset))); v->addWidget(pr);
  connect(pr, &QComboBox::activated, this, [this, pr](int) { const QString k = pr->currentData().toString(); NavScheme s = k == "custom" ? Keymap::get().nav() : Keymap::navPreset(k);
    s.preset = k; s.rotation = Keymap::get().nav().rotation; if (k != "custom") { s.mainPie = Keymap::get().nav().mainPie; s.opsPie = Keymap::get().nav().opsPie; } Keymap::get().setNav(s); fill(); });
  auto bindRow = [&](const QString& label, NavBind NavScheme::*field) {
    auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(new QLabel(label), 1);
    const NavBind b = n.*field;
    auto* btn = new QComboBox; for (auto [t, x] : std::vector<std::pair<QString, int>>{{tr("—"), 0}, {"LMB", 1}, {"MMB", 2}, {"RMB", 3}}) btn->addItem(t, x); btn->setCurrentIndex(std::max(0, btn->findData(b.button))); h->addWidget(btn);
    std::vector<QCheckBox*> ms; for (auto [t, bit] : std::vector<std::pair<QString, int>>{{"Ctrl", 1}, {"Shift", 2}, {"Alt", 4}}) { auto* c = new QCheckBox(t); c->setChecked(b.mods & bit); c->setProperty("bit", bit); ms.push_back(c); h->addWidget(c); }
    auto apply = [this, btn, ms, field] { NavScheme s = Keymap::get().nav(); NavBind nb; nb.button = btn->currentData().toInt(); for (auto* c : ms) if (c->isChecked()) nb.mods |= c->property("bit").toInt();
      s.*field = nb; if (field != &NavScheme::mainPie && field != &NavScheme::opsPie) s.preset = "custom"; Keymap::get().setNav(s); QMetaObject::invokeMethod(this, [this] { fill(); }, Qt::QueuedConnection); };
    connect(btn, &QComboBox::activated, this, [apply](int) { apply(); }); for (auto* c : ms) connect(c, &QCheckBox::clicked, this, [apply](bool) { apply(); });
    v->addWidget(r); };
  bindRow(tr("Orbit"), &NavScheme::orbit); bindRow(tr("Pan"), &NavScheme::pan); bindRow(tr("Pan (second)"), &NavScheme::pan2); bindRow(tr("Zoom (drag)"), &NavScheme::zoom);
  v->addWidget(mut(tr("Shift during an orbit — snaps to the nearest standard view. The wheel always zooms.")));
  v->addWidget(head(tr("Camera turning")));
  auto* tt = new QRadioButton(tr("Turntable — the up stays up (round the vertical, then tilt)")); auto* tb = new QRadioButton(tr("Trackball — free, round the view's own axes (it can roll)"));
  (n.rotation == "trackball" ? tb : tt)->setChecked(true); v->addWidget(tt); v->addWidget(tb);
  connect(tt, &QRadioButton::toggled, this, [](bool on) { NavScheme s = Keymap::get().nav(); s.rotation = on ? "turntable" : "trackball"; Keymap::get().setNav(s); });
  v->addWidget(head(tr("Pie menus on the mouse")));
  bindRow(tr("Main pie"), &NavScheme::mainPie); bindRow(tr("Topology: edits pie"), &NavScheme::opsPie);
}

void HotkeyDialog::presetsPage(QVBoxLayout* v) {
  v->addWidget(head(tr(PAGE_PRESETS)));
  v->addWidget(mut(tr("A preset holds every key, the keys on buttons, the pies of your own and the camera controls.")));
  auto* sv = new QPushButton(tr("💾 Save the preset to a file…")); auto* im = new QPushButton(tr("📂 Import a preset…"));
  auto* rk = new QPushButton(tr("Reset the action keys to the standard")); auto* rf = new QPushButton(tr("⟲ Restore factory settings"));
  for (auto* b : {sv, im, rk, rf}) v->addWidget(b);
  connect(sv, &QPushButton::clicked, this, [this] { const QString f = QFileDialog::getSaveFileName(this, tr("Save hotkeys preset"), QSettings().value("keysPresetDir").toString() + "/hotkeys.json", tr("Hotkeys preset (*.json)"));
    if (f.isEmpty()) return; QSettings().setValue("keysPresetDir", QFileInfo(f).absolutePath()); QFile o(f); if (o.open(QIODevice::WriteOnly)) { o.write(QJsonDocument(Keymap::get().toJson()).toJson()); emit message(tr("Hotkeys preset saved")); } });
  connect(im, &QPushButton::clicked, this, [this] { const QString f = QFileDialog::getOpenFileName(this, tr("Import hotkeys preset"), QSettings().value("keysPresetDir").toString(), tr("Hotkeys preset (*.json)"));
    if (f.isEmpty()) return; QFile i(f); if (!i.open(QIODevice::ReadOnly)) return;
    if (Keymap::get().fromJson(QJsonDocument::fromJson(i.readAll()).object())) { emit message(tr("Hotkeys preset imported")); fill(); } else QMessageBox::warning(this, tr("Import"), tr("Not a hotkeys preset of this program.")); });
  connect(rk, &QPushButton::clicked, this, [this] { Keymap::get().reset(); emit message(tr("Action keys — the standard ones")); fill(); });
  connect(rf, &QPushButton::clicked, this, [this] { if (QMessageBox::question(this, tr("Restore factory settings"), tr("Every key, the keys on buttons, your pies and the camera controls back to the standard?")) != QMessageBox::Yes) return;
    Keymap::get().factoryReset(); emit message(tr("Hotkeys: factory settings")); fill(); });
}

// a combination: pressed (the modifiers with it), set when the key is let go; Esc — cancel, Backspace — clear, End — never
void HotkeyDialog::keyPressEvent(QKeyEvent* e) {
  if (capture_.isEmpty()) { QDialog::keyPressEvent(e); return; }
  if (e->isAutoRepeat() || e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt || e->key() == Qt::Key_Meta) return;
  if (e->key() == Qt::Key_Escape) { fill(); return; }
  if (e->key() == Qt::Key_Backspace) { capture_.clear(); if (onCaptured_) onCaptured_(QString()); fill(); return; }
  const QString c = Keymap::comboOf(e);
  if (!Keymap::assignable(c)) { if (captureBtn_) captureBtn_->setText(tr("Esc / End — not allowed")); return; }
  pending_ = c; if (captureBtn_) captureBtn_->setText(Keymap::label(c));
}
void HotkeyDialog::keyReleaseEvent(QKeyEvent* e) {
  if (capture_.isEmpty() || pending_.isEmpty() || e->isAutoRepeat()) { QDialog::keyReleaseEvent(e); return; }
  if (e->key() == Qt::Key_Control || e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt || e->key() == Qt::Key_Meta) return;
  const QString c = pending_; capture_.clear(); pending_.clear(); if (onCaptured_) onCaptured_(c); fill();
}

// ======================= End: a key for what is under the pointer =======================
namespace { KeyCapture* g_cap = nullptr; }
bool KeyCapture::active() { return g_cap; }
void KeyCapture::start(QWidget* window, const QString& name, std::function<void(const QString&)> done) { if (g_cap) return; g_cap = new KeyCapture(window, name, std::move(done)); }
KeyCapture::KeyCapture(QWidget* window, const QString& name, std::function<void(const QString&)> done)
    : QWidget(window, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), name_(name), done_(std::move(done)) {
  setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setAttribute(Qt::WA_DeleteOnClose);
  resize(600, 96); const QRect g = window->geometry(); move(g.center().x() - width() / 2, g.top() + g.height() / 3);
  qApp->installEventFilter(this); show(); raise();
}
void KeyCapture::paintEvent(QPaintEvent*) {
  QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setPen(QColor(0xc0, 0x80, 0x30)); p.setBrush(QColor(0x26, 0x22, 0x1c, 240)); p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 8, 8);
  QFont f = font(); f.setPointSizeF(10.5); p.setFont(f); p.setPen(QColor(0xee, 0xe6, 0xd8));
  const QString t = tr("Press a key or a key combination to assign to “%1”").arg(name_) + "\n" + (combo_.isEmpty() ? (note_.isEmpty() ? tr("Esc — cancel") : note_) : Keymap::label(combo_) + tr("   — let go to set it"));
  p.drawText(rect().adjusted(14, 8, -14, -8), Qt::AlignCenter | Qt::TextWordWrap, t);
}
bool KeyCapture::eventFilter(QObject*, QEvent* e) {
  if (e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }
  if (e->type() != QEvent::KeyPress && e->type() != QEvent::KeyRelease) return false;
  auto* k = static_cast<QKeyEvent*>(e); if (k->isAutoRepeat()) return true;
  const bool mod = k->key() == Qt::Key_Control || k->key() == Qt::Key_Shift || k->key() == Qt::Key_Alt || k->key() == Qt::Key_Meta;
  if (e->type() == QEvent::KeyPress) {
    if (k->key() == Qt::Key_Escape) { finish(false); return true; }
    if (mod) return true;
    const QString c = Keymap::comboOf(k);
    if (!Keymap::assignable(c)) { combo_.clear(); note_ = tr("End and Esc can't be hotkeys — another one (Esc — cancel)"); update(); return true; }
    combo_ = c; note_.clear(); update(); return true;
  }
  if (!mod && !combo_.isEmpty()) finish(true);                  // (the keys let go: set)
  return true;
}
void KeyCapture::finish(bool ok) {
  qApp->removeEventFilter(this); g_cap = nullptr; auto d = done_; const QString c = combo_;
  close(); if (ok && d) d(c);
}

}  // namespace hm::ui
