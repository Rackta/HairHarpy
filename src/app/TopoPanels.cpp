#include "app/TopoPanels.h"
#include "app/SymPanel.h"
#include "scene/Primitives.h"
#include <QCursor>
#include <QListWidget>
#include "app/TexWindow.h"
#include "app/ToolPanels.h"
#include "app/MaterialWindow.h"
#include "scene/TopoEdit.h"
#include "scene/Looks.h"
#include "scene/MeshMods.h"
#include "scene/Topo.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QElapsedTimer>
#include <QApplication>
#include <numeric>
#include <QCheckBox>
#include <QMainWindow>
#include <QPointer>
#include <QDateTime>
#include <QColorDialog>
#include <QComboBox>
#include <QDropEvent>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QActionGroup>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QInputDialog>
#include <QSizeGrip>
#include <QSlider>
#include <QSplitter>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>

namespace hm {

namespace {
QPushButton* btn(const QString& t, const QString& tip = {}) { auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); return b; }
// a help text made readable: each "keys — function, what it does" part on a line of its own — the keys as keycaps,
// the function bright, then the rest dim; a text with no such part stays as it is
bool isKeyWord(const QString& w) {
  static const QStringList K = {"LMB", "RMB", "MMB", "Ctrl", "Shift", "Alt", "Esc", "Enter", "Tab", "Space", "Del", "Delete", "Backspace", "wheel", "Wheel",
                                "click", "Click", "double", "Double", "drag", "Drag", "held", "Held", "tap", "Tap"};
  if (K.contains(w)) return true;
  return w.size() <= 3 && w.startsWith('F') && w.mid(1).toInt() > 0;                // (F1…F12)
}
// how long the keys at the start of s are (0 — it doesn't start with keys)
int keysLen(const QString& s) {
  int i = 0, end = 0; bool any = false;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '+')) i++;
    int j = i; while (j < s.size() && (s[j].isLetterOrNumber())) j++;
    if (j == i || !isKeyWord(s.mid(i, j - i))) break;
    any = true; end = j; i = j;
  }
  return any ? end : 0;
}
QString helpHtml(const QString& t) {
  // the parts: split at the top level (not inside brackets) where a separator is followed by keys and a " — " later
  QStringList parts; QString cur; int depth = 0;
  static const QStringList SEP = {"\n", " · ", "; ", ", ", ": "};
  for (int i = 0; i < t.size(); i++) {
    const QChar c = t[i]; if (c == '(') depth++; if (c == ')') depth = std::max(0, depth - 1);
    bool split = false;
    if (depth == 0) for (const QString& s : SEP) if (t.mid(i, s.size()) == s) {
      const QString rest = t.mid(i + s.size()); const int dash = rest.indexOf(QString::fromUtf8(" — "));
      if (s == "\n" || (keysLen(rest) > 0 && dash > 0 && rest.left(dash).indexOf('\n') < 0)) { if (s == ": ") cur += ':'; parts << cur; cur.clear(); i += int(s.size()) - 1; split = true; }
      break; }
    if (!split) cur += c;
  }
  parts << cur;
  bool anyKey = false; QStringList out;
  for (const QString& p0 : parts) {
    const QString p = p0.trimmed(); if (p.isEmpty()) continue;
    const int kl = keysLen(p), dash = p.indexOf(QString::fromUtf8(" — "));
    if (kl <= 0 || dash < kl) { out << QString("<span style=\"color:#9aa0a8\">%1</span>").arg(p.toHtmlEscaped()); continue; }
    anyKey = true;
    QStringList caps; for (const QString& k : p.left(kl).split(QRegularExpression("\\s*\\+\\s*|\\s+"), Qt::SkipEmptyParts))
      caps << QString("<span style=\"background-color:#4a4f58;color:#ffffff;font-weight:700\">&nbsp;%1&nbsp;</span>").arg(k.toHtmlEscaped());
    const QString qual = p.mid(kl, dash - kl).trimmed(), rest = p.mid(dash + 3).trimmed();
    int cut = -1, d = 0; for (int i = 0; i < rest.size() && cut < 0; i++) { const QChar c = rest[i]; if (c == '(' && d == 0 && i > 0) cut = i; if (c == '(') d++; if (c == ')') d--; if (d == 0 && (c == ',' || c == ';' || c == ':')) cut = i; }
    const QString name = (cut < 0 ? rest : rest.left(cut)).trimmed(), more = cut < 0 ? QString() : rest.mid(cut).trimmed().remove(QRegularExpression("^[,;:]\\s*"));
    QString line = caps.join("<span style=\"color:#777\">+</span>");
    if (!qual.isEmpty()) line += " <span style=\"color:#c8ccd2\">" + qual.toHtmlEscaped() + "</span>";
    line += " <span style=\"color:#777\">—</span> <b style=\"color:#f2c86b\">" + name.toHtmlEscaped() + "</b>";
    if (!more.isEmpty()) line += " <span style=\"color:#9aa0a8\">" + more.toHtmlEscaped() + "</span>";
    out << line;
  }
  return anyKey ? out.join("<br>") : QString();
}
QLabel* muted(const QString& t) {
  auto* l = new QLabel; l->setProperty("mut", true); l->setWordWrap(true);
  if (const QString h = helpHtml(t); !h.isEmpty()) { l->setTextFormat(Qt::RichText); l->setText(h); l->setStyleSheet("line-height:140%"); } else l->setText(t);
  return l;
}
QLabel* sec(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setStyleSheet("margin-top:8px"); return l; }
QCheckBox* check(const QString& t, bool on, const QString& tip = {}) { auto* c = new QCheckBox(t); c->setChecked(on); c->setToolTip(tip); c->setFocusPolicy(Qt::NoFocus); return c; }
QWidget* hrow(std::initializer_list<QWidget*> ws, bool stretch = true) { auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 2, 0, 2); h->setSpacing(4); for (QWidget* x : ws) h->addWidget(x); if (stretch) h->addStretch(1); return w; }
// a picture button of the mesh's settings: checkable; dbl — what a double click (or RMB) opens (its manager)
// (dblKey: the same button in the panel made again — its first click changes the mesh, the panel is rebuilt before the
// second: the double click told by the key and the time, whichever button gets it)
class IconBtn : public QPushButton {
public:
  std::function<void()> dbl; QString dblKey;
protected:
  static QString& lastKey() { static QString k; return k; }
  static qint64& lastT() { static qint64 t = 0; return t; }
  void mousePressEvent(QMouseEvent* e) override {
    if (dbl && e->button() == Qt::RightButton) { e->accept(); QTimer::singleShot(0, dbl); return; }   // (RMB: its settings at once)
    if (dbl && !dblKey.isEmpty() && e->button() == Qt::LeftButton) {
      const qint64 now = QDateTime::currentMSecsSinceEpoch();
      if (lastKey() == dblKey && now - lastT() <= QApplication::doubleClickInterval()) { lastKey().clear(); e->accept(); QTimer::singleShot(0, dbl); return; }
      lastKey() = dblKey; lastT() = now;
    }
    QPushButton::mousePressEvent(e);
  }
  void contextMenuEvent(QContextMenuEvent* e) override { if (dbl) { e->accept(); return; } QPushButton::contextMenuEvent(e); }   // (RMB opened its settings: no menu of the panel's over them)
  void mouseDoubleClickEvent(QMouseEvent* e) override { if (dbl && e->button() == Qt::LeftButton) { lastKey().clear(); e->accept(); QTimer::singleShot(0, dbl); return; } QPushButton::mouseDoubleClickEvent(e); }
};
// a selection set's button: the pointer on it — its elements lit in the viewport; a click — picked (Shift adds, Ctrl
// takes off); RMB — deleted
class SetBtn : public QPushButton {
public:
  std::function<void(bool)> hover; std::function<void()> del;
protected:
  void enterEvent(QEnterEvent* e) override { if (hover) hover(true); QPushButton::enterEvent(e); }
  void leaveEvent(QEvent* e) override { if (hover) hover(false); QPushButton::leaveEvent(e); }
  void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::RightButton && del) { e->accept(); QTimer::singleShot(0, del); return; } QPushButton::mousePressEvent(e); }
};
// ---- the UV Sets and UV Mats as buttons, as the selection sets: a picture, the name under it; the one worked in —
// blue, those the selected polygons lie in — pale lit; a click: its polygons selected (Shift adds, Ctrl takes off) and it
// worked in; a double click: renamed; RMB: its menu ----
namespace {
// (a double click told by the time, not by the button: the panel is made again between the two clicks)
bool secondClick(int id, int key) {
  static int lastId = -1, lastKey = 0; static qint64 lastT = 0; const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const bool dbl = lastId == id && lastKey == key && now - lastT <= QApplication::doubleClickInterval();
  lastId = id; lastKey = key; lastT = dbl ? 0 : now; return dbl;
}
const PolyMesh* uvBase(Viewport* vp, int id) { const Item* mm = vp->scene()->item(id); return mm ? (mm->base ? mm->base.get() : mm->poly.get()) : nullptr; }
std::vector<int> uvSetFaces(Viewport* vp, int id, int i) {
  const PolyMesh* B = uvBase(vp, id); std::vector<int> fs; if (!B) return fs;
  for (int f = 0; f < B->faceCount(); f++) if (!topo::hasUvSets(*B) || (B->faceSets[size_t(f)] >> i & 1u)) fs.push_back(f);
  return fs;
}
// the polygons selected by the click's keys: Shift — added, Ctrl — taken off, else — they alone
void uvSelectFaces(Viewport* vp, int id, const std::vector<int>& fs) {
  const auto mods = QApplication::keyboardModifiers(); const QString op = (mods & Qt::ControlModifier) ? "remove" : (mods & Qt::ShiftModifier) ? "add" : "set";
  if (vp->topoMode != "poly") vp->setTopoMode("poly"); vp->tselApply(id, fs, op, 'f', false);
}
QString askName(const QString& title, const QString& was) { bool ok = false; const QString t = QInputDialog::getText(nullptr, title, QObject::tr("Name"), QLineEdit::Normal, was, &ok); return ok ? t.trimmed() : QString(); }
// one button: its picture, its name; k() — 2 worked in, 1 lit, 0 neither
QWidget* uvSetCell(Viewport* vp, const QString& icon, const QString& name, const QString& tip, std::function<int()> k, std::function<void()> click, std::function<void()> menu) {
  auto* cell = new QWidget; auto* cv = new QVBoxLayout(cell); cv->setContentsMargins(0, 0, 0, 0); cv->setSpacing(1);
  auto* b = new SetBtn; b->setProperty("tool", true); b->setFixedSize(40, 40); b->setIcon(ui::icon(icon)); b->setIconSize(QSize(34, 34)); b->setFocusPolicy(Qt::NoFocus); b->setToolTip(tip);
  auto* l = new QLabel; l->setFixedWidth(44); l->setAlignment(Qt::AlignHCenter); l->setText(QFontMetrics(l->font()).elidedText(name, Qt::ElideRight, 44)); l->setToolTip(name);
  auto look = [b, l, k] { const int s = k(); if (b->property("look").toInt() == s && b->property("lookSet").toBool()) return;
    b->setProperty("look", s); b->setProperty("lookSet", true); l->setStyleSheet(QString("font-size:9px;color:%1").arg(s == 2 ? "#cfe0ff" : "#9a9a9a"));
    b->setStyleSheet(s == 2 ? "QPushButton{background:#3d5f94;border:1px solid #8ab0f0}" : s == 1 ? "QPushButton{background:#34405a;border:1px solid #5d7bb0}" : QString()); };
  look(); QObject::connect(vp, &Viewport::topoState, b, look); QObject::connect(vp, &Viewport::selectionChanged, b, look);
  b->del = std::move(menu);
  QObject::connect(b, &QPushButton::clicked, vp, [vp, click] { QTimer::singleShot(0, vp, click); });
  cv->addWidget(b, 0, Qt::AlignHCenter); cv->addWidget(l); return cell;
}
}  // namespace

// a UV Set's menu (RMB on its button): its polygons, its layout copied / merged / moved, renamed, deleted
void uvSetMenu(Viewport* vp, int id, int i) {
  auto later = [vp](std::function<void()> fn) { QTimer::singleShot(0, vp, std::move(fn)); };
  const auto SS = vp->uvSets(id); if (i < 0 || size_t(i) >= SS.size()) return;
  const bool cur = vp->uvPicked.id == id && vp->uvPicked.set == i && vp->uvPicked.mat == -2;
  QMenu menu; auto act = [&](QMenu* mm, const QString& l, std::function<void()> fn) { QObject::connect(mm->addAction(l), &QAction::triggered, vp, [fn, later] { later(fn); }); };
  act(&menu, cur ? QObject::tr("⊘ Out of it (every polygon shown)") : QObject::tr("▣ Work in it (only it in the UV Editor)"), [vp, id, i] { vp->uvPickSet(id, i); });
  act(&menu, QObject::tr("⇲ Apply to the selected polygons"), [vp, id, i] { vp->uvSetApply(id, i); });
  act(&menu, QObject::tr("⇱ Take the selected polygons out"), [vp, id, i] { vp->uvSetTakeOut(id, i); });
  act(&menu, QObject::tr("◉ Select its polygons"), [vp, id, i] { if (vp->topoMode != "poly") vp->setTopoMode("poly"); vp->tSelectFaces(id, uvSetFaces(vp, id, i)); });
  menu.addSeparator();
  act(&menu, QObject::tr("⧉ Duplicate (a layout of its own)"), [vp, id, i] { vp->uvSetDuplicate(id, i); });
  QMenu* mg = menu.addMenu(QObject::tr("⊕ Merge with"));
  for (size_t j = 0; j < SS.size(); j++) if (int(j) != i) { const int jj = int(j);
    act(mg, QObject::tr("%1 — %2 wins where both lay one out").arg(SS[j].name, SS[size_t(std::min(i, jj))].name), [vp, id, i, jj] { vp->uvSetMerge(id, i, jj, false); }); }
  mg->setEnabled(SS.size() > 1);
  if (i > 0) act(&menu, QObject::tr("◂ Move before (wins a merge)"), [vp, id, i] { vp->uvSetMove(id, i, i - 1); });
  if (size_t(i) + 1 < SS.size()) act(&menu, QObject::tr("▸ Move after"), [vp, id, i] { vp->uvSetMove(id, i, i + 1); });
  act(&menu, QObject::tr("✎ Rename (double click)"), [vp, id, i, n = SS[size_t(i)].name] { const QString t = askName(QObject::tr("Rename the UV Set"), n); if (!t.isEmpty()) vp->uvSetRename(id, i, t); });
  menu.addSeparator(); if (SS.size() > 1) act(&menu, QObject::tr("✕ Delete (its mats use another set)"), [vp, id, i] { vp->uvSetDelete(id, i); });
  menu.exec(QCursor::pos());
}
QWidget* uvSetGrid(Viewport* vp, int id, int cols) {
  auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 4); g->setSpacing(4);
  const auto S = vp->uvSets(id);
  for (size_t i = 0; i < S.size(); i++) {
    const int si = int(i); const QString name = S[i].name;
    const QString tip = QObject::tr("%1: %2 polygons, %3 mat%4 — click: its polygons selected (Shift adds, Ctrl takes off), it worked in (only it in the UV Editor); double click: rename; RMB: more")
                          .arg(name).arg(S[i].faces).arg(S[i].mats.size()).arg(S[i].mats.size() == 1 ? "" : "s");
    auto k = [vp, id, si] { return vp->uvPicked.id == id && vp->uvPicked.set == si ? 2 : vp->uvSetsOfSel(id).count(si) ? 1 : 0; };
    auto click = [vp, id, si, name] {
      if (secondClick(id, si + 1)) { const QString t = askName(QObject::tr("Rename the UV Set"), name); if (!t.isEmpty()) vp->uvSetRename(id, si, t); return; }
      uvSelectFaces(vp, id, uvSetFaces(vp, id, si));
      if (!(vp->uvPicked.id == id && vp->uvPicked.set == si && vp->uvPicked.mat == -2)) vp->uvPickSet(id, si); };
    g->addWidget(uvSetCell(vp, "uv_set", name, tip, k, click, [vp, id, si] { uvSetMenu(vp, id, si); }), si / cols, si % cols);
  }
  g->setColumnStretch(cols, 1); return w;
}
IconBtn* picBtn(const QIcon& icon, const QString& tip, bool on, int size = 60) {
  auto* b = new IconBtn; b->setProperty("tool", true); b->setFixedSize(size, size); b->setIcon(icon); b->setIconSize(QSize(size - 8, size - 8));
  b->setToolTip(tip); b->setCheckable(true); b->setChecked(on); b->setFocusPolicy(Qt::NoFocus); return b;
}
// the light of a picture tinted green (the mask's icon while the mesh has a mask)
QIcon greenLit(const QString& name) {
  QImage img = ui::image(name).toImage().convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < img.height(); y++) { auto* row = reinterpret_cast<QRgb*>(img.scanLine(y));
    for (int x = 0; x < img.width(); x++) { QColor c = QColor::fromRgba(row[x]); const double l = c.lightnessF(), s = c.hslSaturationF();
      if (l < 0.5 || s > 0.2) continue; const double t = std::clamp((l - 0.5) / 0.3, 0.0, 1.0);
      QColor g = QColor::fromRgbF(0.32 * l + 0.0, 0.85 * l, 0.45 * l); row[x] = qRgba(int(c.red() + (g.red() - c.red()) * t), int(c.green() + (g.green() - c.green()) * t), int(c.blue() + (g.blue() - c.blue()) * t), c.alpha()); } }
  return QIcon(QPixmap::fromImage(img));
}
// a section of the mesh's settings that folds (its open state kept)
bool foldSec(QVBoxLayout* v, Viewport* vp, const QString& key, const QString& title, bool def = true, const QString& icon = {}) {
  const bool open = QSettings().value("topo/sec_" + key, def).toBool();
  auto* h = new QPushButton((open ? "▾ " : "▸ ") + title); if (!icon.isEmpty()) { h->setIcon(ui::icon(icon)); h->setIconSize(QSize(22, 22)); } h->setFlat(true); h->setFocusPolicy(Qt::NoFocus); h->setCursor(Qt::PointingHandCursor);
  h->setStyleSheet("QPushButton{text-align:left;font-weight:600;color:#ddd;margin-top:8px;padding:2px 0;border:none} QPushButton:hover{color:#fff}");
  QObject::connect(h, &QPushButton::clicked, vp, [vp, key, open] { QSettings().setValue("topo/sec_" + key, !open); emit vp->topoState(); });
  v->addWidget(h); return open;
}
QPushButton* toolBtn(const QString& icon, const QString& tip) {
  auto* b = new QPushButton; b->setProperty("tool", true); b->setFixedSize(36, 36); b->setIcon(ui::icon(icon)); b->setIconSize(QSize(32, 32)); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); b->setCheckable(true); return b;
}
QColor tgColor(int gi) { return QColor::fromHslF(std::fmod(gi * 137.5, 360.0) / 360.0, 0.55, 0.58); }
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
const char* HEAVY = "A high-poly mesh (over 300 000 triangles) is not edited — Object mode only";
QString fmt3(double v) { double r = std::round(v * 1000) / 1000; if (r == 0) r = 0; return QString::number(r, 'g', 10); }
double unitK(const QString& u) { return u == "mm" ? 0.1 : u == "m" ? 100 : u == "in" ? 2.54 : 1; }
struct MatP { const char* name; const char* color; double rough, metal, spec; };
const MatP MATS[] = {{"Sculpt clay", "#a99a86", 0.55, 0, 0.35}, {"Clay", "#b9a99a", 0.85, 0, 0.2}, {"Skin", "#e6b8a2", 0.55, 0, 0.45}, {"Plastic", "#d4d8de", 0.32, 0, 0.8},
                     {"Rubber", "#3b3b3e", 0.92, 0, 0.1}, {"Metal", "#c9ccd1", 0.3, 1, 1}, {"Gold", "#e3b65a", 0.26, 1, 1}, {"Jade", "#5ea476", 0.4, 0, 0.7}, {"Wax", "#b4423c", 0.6, 0, 0.4}};
QJsonObject matOf(const MatP& m) { return {{"color", m.color}, {"roughness", m.rough}, {"metalness", m.metal}, {"specular", m.spec}}; }
// an edit of the mesh's record (no geometry): one undo step, the look drawn anew
void rawEdit(Viewport* vp, int id, const std::function<void(QJsonObject&)>& fn) {
  vp->runEdit([id, fn](Scene& s, Selection&, const CurveCache&) { if (Item* it = s.item(id)) { fn(it->raw); it->headTag = it->raw.value("headTag").toBool(); } return QString(); }, false);
  vp->topoDisplayChanged();
}
}  // namespace

// ---------- the mode strip ----------
TopoModes::TopoModes(Viewport* vp, QWidget* parent) : QFrame(parent), vp_(vp) {
  setObjectName("tmodes"); setFixedWidth(45); setStyleSheet("#tmodes{background:#262626;border-right:1px solid #3a3a3a}");
  auto* v = new QVBoxLayout(this); v->setContentsMargins(4, 6, 4, 6); v->setSpacing(5); v->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
  struct M { const char *k, *icon, *tip; };
  const M modes[] = {
    {"object", "topo_object", "Object mode: whole meshes — click / box to select, drag to move, Ctrl+Shift — polygroups (hover lights one up, click picks it); Move / Rotate / Scale — the gizmo"},
    {"poly", "topo_polygon", "Polygon mode: the active mesh's polygons — LMB, Shift add, Ctrl deselect (drag — a box), double click — the loop or the area inside a closed frame, Ctrl+Shift — the shortest path; drag a selected one — move. Shift+click — the selection carried into polygons"},
    {"edge", "topo_edge", "Edge mode: the active mesh's edges — LMB, Shift add, Ctrl deselect, double click — the edge loop, Ctrl+Shift — the shortest path; drag — move. Shift+click — the selection carried into edges"},
    {"vert", "topo_vertex", "Vertex mode: the active mesh's vertices — LMB, Shift add, Ctrl deselect, double click — the vertex loop, Ctrl+Shift — the shortest path; drag — move. Shift+click — the selection carried into vertices"}};
  for (const M& m : modes) { auto* b = toolBtn(m.icon, tr(m.tip)); b->setObjectName(QString("tm_") + m.k); QString k = m.k;
    connect(b, &QPushButton::clicked, this, [this, k] { if (QApplication::keyboardModifiers() & Qt::ShiftModifier) vp_->tConvertSel(k); else vp_->setTopoMode(k); sync(); }); v->addWidget(b); }
  // the UV mode (with the modes: picking in the UV Editor follows them)
  auto* uvb = toolBtn("topo_uv", tr("UV mode: the selected mesh's UV in the UV Editor (a window of its own — the camera menu's views too) and a checker on it; its seams red, where the UV breaks teal. The modes pick there too: polygons, edges, UV points — drag to move them (Rotate / Scale tools: turn / scale). Its tools in the UV window"));
  uvb->setObjectName("tt_uv"); connect(uvb, &QPushButton::clicked, this, [this] { vp_->setUvMode(!vp_->tset.uv); sync(); });
  auto mkGap = [] { auto* g = new QFrame; g->setFixedSize(28, 14); g->setStyleSheet("border-bottom:1px solid #3a3a3a"); return g; };   // (a space between the groups)
  auto* sn = toolBtn("topo_snap", tr("Snap: the vertices being edited (moved, turned, scaled, typed in) stick to the surface of the other meshes — only those, nothing else moves")); sn->setObjectName("tt_snap");
  auto* sy = toolBtn("topo_sym", tr("Symmetry: an edit of vertices is mirrored onto their pairs across the mesh's X = 0 (its file's centre plane); vertices on the plane stay on it. The snap mask brush paints mirrored too")); sy->setObjectName("tt_sym");
  connect(sn, &QPushButton::clicked, this, [this] { vp_->tset.snap = !vp_->tset.snap; vp_->saveTopoPrefs(); ui::toast(vp_->tset.snap ? tr("Snap on") : tr("Snap off")); sync(); });
  connect(sy, &QPushButton::clicked, this, [this] { vp_->setSymmetry(!vp_->tset.sym); sync(); });
  auto* ku = toolBtn("topo_keepuv", tr("Preserve UVs: vertices moved (dragged, the gizmo, the transform table, snapped) keep their place in the texture — each takes the UV of where it now lies on the surface as it was, so the texture stays put while the topology is edited"));
  ku->setObjectName("tt_keepuv"); connect(ku, &QPushButton::clicked, this, [this] { vp_->setPreserveUv(!vp_->tset.keepUv); sync(); });
  // (the element snap: on / off, and its settings — what it takes: a vertex, an edge, a polygon, the view's grid; how near)
  auto* es = toolBtn("topo_snap_v2", tr("Element snap: a drag of picked vertices takes hold of the one under the pointer — it goes onto a vertex, an edge, a polygon or the view's grid (the settings next to it), the rest with it by the same offset")); es->setObjectName("tt_esnap");
  connect(es, &QPushButton::clicked, this, [this] { vp_->tset.esnap = !vp_->tset.esnap; vp_->saveTopoPrefs(); ui::toast(vp_->tset.esnap ? tr("Element snap on") : tr("Element snap off")); sync(); });
  auto* est = toolBtn("topo_snapsettings", tr("Element snap settings: what it snaps to, how near; the views' grids")); est->setObjectName("tt_esnapset"); est->setCheckable(false);
  connect(est, &QPushButton::clicked, this, [this, est] {
    QMenu menu(this); auto* grp = new QActionGroup(&menu); grp->setExclusive(true);
    for (auto [k, t] : {std::pair<const char*, const char*>{"vert", "To a vertex"}, {"edge", "To an edge"}, {"face", "To a polygon"}, {"grid", "To the view's grid"}}) {
      QAction* a = menu.addAction(tr(t)); a->setCheckable(true); a->setChecked(vp_->tset.esnapMode == k); grp->addAction(a); const QString key = k;
      connect(a, &QAction::triggered, this, [this, key] { vp_->tset.esnapMode = key; vp_->tset.esnap = true; vp_->saveTopoPrefs(); sync(); }); }
    menu.addSeparator(); QMenu* rm = menu.addMenu(tr("Reach: %1 px").arg(vp_->tset.esnapPx));
    for (double r : {8.0, 14.0, 20.0, 30.0, 45.0}) { QAction* a = rm->addAction(tr("%1 px").arg(r)); a->setCheckable(true); a->setChecked(std::abs(vp_->tset.esnapPx - r) < 0.5);
      connect(a, &QAction::triggered, this, [this, r] { vp_->tset.esnapPx = r; vp_->saveTopoPrefs(); }); }
    menu.addSeparator(); const QString view = vp_->camera().view; const auto g = vp_->viewGrid(view);
    QAction* ga = menu.addAction(tr("This view's grid shown")); ga->setCheckable(true); ga->setChecked(g.on);
    connect(ga, &QAction::triggered, this, [this, view](bool on) { auto G = vp_->viewGrid(view); G.on = on; vp_->setViewGrid(view, G); });
    QMenu* sm = menu.addMenu(tr("Its step: %1").arg(g.step));
    for (double s : {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0}) { QAction* a = sm->addAction(QString::number(s)); a->setCheckable(true); a->setChecked(std::abs(g.step - s) < 1e-9);
      connect(a, &QAction::triggered, this, [this, view, s] { auto G = vp_->viewGrid(view); G.step = s; G.on = true; vp_->setViewGrid(view, G); }); }
    menu.exec(est->mapToGlobal(QPoint(est->width(), 0))); });
  // (the UV mode and Preserve UVs; a space; the snaps — to the references, to an element, its settings; a space; Symmetry.
  // The snap mask brush: in the mesh's settings, Snapping)
  v->addWidget(uvb); v->addWidget(ku); v->addWidget(mkGap(), 0, Qt::AlignHCenter);
  // (Symmetry's settings — as the element snap's: its UV side — the symmetric unfold, the −X shells' tint, the UV axis,
  // Symmetrize)
  auto* syt = toolBtn("topo_snapsettings", tr("Symmetry settings: planar / topology / radial, the axes X Y Z, the space, where the plane lies, Restrict, the tolerance, the planes shown; the UV part — symmetric unfold, the −X tint, the UV axis, Symmetrize")); syt->setObjectName("tt_symset"); syt->setCheckable(false);
  connect(syt, &QPushButton::clicked, this, [this, syt] { openSymPanel(vp_, syt, true); });   // (the panel: Symmetry's settings, the UV part under them)
  v->addWidget(sn); v->addWidget(es); v->addWidget(est); v->addWidget(mkGap(), 0, Qt::AlignHCenter); v->addWidget(sy); v->addWidget(syt);
  connect(vp, &Viewport::topoState, this, &TopoModes::sync);
  connect(vp, &Viewport::toolChanged, this, &TopoModes::sync);
  connect(vp, &Viewport::selectionChanged, this, &TopoModes::sync);
  sync();
}
void TopoModes::sync() {
  ProfScope prof_("TopoModes");
  const bool heavy = vp_->topoHeavy(vp_->topoMesh());
  for (const char* k : {"object", "poly", "edge", "vert"}) if (auto* b = findChild<QPushButton*>(QString("tm_") + k)) {
    b->setChecked(vp_->topoMode == k);
    if (QString(k) != "object") { if (heavy) { auto* e = new QGraphicsOpacityEffect(b); e->setOpacity(0.3); b->setGraphicsEffect(e); } else b->setGraphicsEffect(nullptr); if (heavy) b->setToolTip(tr(HEAVY)); }
  }
  if (auto* b = findChild<QPushButton*>("tt_snap")) b->setChecked(vp_->tset.snap);
  if (auto* b = findChild<QPushButton*>("tt_esnap")) b->setChecked(vp_->tset.esnap);
  if (auto* b = findChild<QPushButton*>("tt_sym")) b->setChecked(vp_->tset.sym);
  if (auto* b = findChild<QPushButton*>("tt_uv")) b->setChecked(vp_->tset.uv);
  if (auto* b = findChild<QPushButton*>("tt_keepuv")) b->setChecked(vp_->tset.keepUv);
  if (auto* b = findChild<QPushButton*>("tt_brush")) b->setChecked(vp_->tool() == "tmask");
}

// ---------- the UV mode's window ----------
namespace {
// a picture from a glyph (a function with no icon of its own yet)
QIcon glyphIcon(const QString& g) {
  QPixmap p(64, 64); p.fill(Qt::transparent); QPainter q(&p); q.setRenderHint(QPainter::TextAntialiasing);
  QFont f("Segoe UI Symbol"); f.setPixelSize(40); q.setFont(f); q.setPen(QColor(0xd0, 0xd0, 0xd8)); q.drawText(QRect(0, 0, 64, 64), Qt::AlignCenter, g); q.end();
  return QIcon(p);
}
QIcon mirroredIcon(const QString& name) { return QIcon(QPixmap::fromImage(ui::image(name).toImage().mirrored(true, false))); }
// the stretch's scale: its colours from squeezed to stretched, the UV area to the 3D area marked under them
class StretchScale : public QWidget {
public:
  StretchScale() { setFixedHeight(40); setToolTip(QObject::tr("The stretch: each polygon's UV area to its 3D area, against the whole mesh's. ×1 grey — as it should be; red — squeezed (less UV than its size), blue — stretched (more). Hover a polygon in the UV Editor — its own number")); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter g(this); g.setRenderHint(QPainter::Antialiasing); const QRect bar(6, 3, width() - 12, 12); const double span = 4.0 / 3.0 * 1.15;
    for (int x = 0; x < bar.width(); x++) { const double l2 = (double(x) / std::max(1, bar.width() - 1) * 2 - 1) * span; g.setPen(Viewport::stretchColor(std::pow(2.0, l2))); g.drawLine(bar.left() + x, bar.top(), bar.left() + x, bar.bottom()); }
    g.setPen(QColor(0x55, 0x55, 0x5c)); g.setBrush(Qt::NoBrush); g.drawRect(bar.adjusted(0, 0, -1, -1));
    QFont f = font(); f.setPixelSize(10); g.setFont(f);
    for (double q : {0.4, 0.63, 1.0, 1.6, 2.5}) { const int x = bar.left() + int(std::lround((std::log2(q) / span + 1) / 2 * (bar.width() - 1)));
      g.setPen(QColor(0xc8, 0xc8, 0xd0)); g.drawLine(x, bar.bottom() + 1, x, bar.bottom() + 4);
      const QString t = q == 1.0 ? QString("×1") : QString("×%1").arg(q, 0, 'g', 2); g.drawText(QRect(x - 22, bar.bottom() + 4, 44, 12), Qt::AlignCenter, t); }
    g.setPen(QColor(0x8a, 0x8a, 0x92)); g.drawText(QRect(bar.left(), bar.bottom() + 16, bar.width(), 12), Qt::AlignLeft, QObject::tr("squeezed"));
    g.drawText(QRect(bar.left(), bar.bottom() + 16, bar.width(), 12), Qt::AlignHCenter, QObject::tr("UV area ÷ 3D area"));
    g.drawText(QRect(bar.left(), bar.bottom() + 16, bar.width(), 12), Qt::AlignRight, QObject::tr("stretched"));
  }
};
QIcon textIcon(const QString& t) {                  // (a picture of a short word — a switch with no icon of its own)
  QPixmap p(96, 96); p.fill(Qt::transparent); QPainter q(&p); q.setRenderHint(QPainter::Antialiasing); q.setRenderHint(QPainter::TextAntialiasing);
  q.setPen(QPen(QColor(0xc8, 0xc8, 0xd0), 4)); q.setBrush(Qt::NoBrush); q.drawRoundedRect(QRectF(10, 22, 76, 52), 10, 10);
  QFont f("Segoe UI"); f.setPixelSize(28); f.setBold(true); q.setFont(f); q.drawText(QRect(0, 0, 96, 96), Qt::AlignCenter, t); q.end(); return QIcon(p);
}
}  // namespace

UvTools::UvTools(QWidget* window, Viewport* vp) : QFrame(window, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), vp_(vp) {
  setObjectName("uvWin"); setStyleSheet("#uvWin{background:#262626;border:1px solid #555;border-radius:6px} #uvbar{background:#2b2b2b;border-bottom:1px solid #3a3a3a}"
                                         " QPushButton{padding:2px 6px} QLabel[h5=\"true\"]{color:#9a9a9a;font-size:11px;margin-top:4px}"
                                         " QToolButton#uvb{background:#2e2e2e;border:1px solid #3c3c3c;border-radius:4px;color:#ccc;font-size:10px;padding:2px 0 1px 0}"
                                         " QToolButton#uvb:hover{background:#3a3f4a;border-color:#5b6b88} QToolButton#uvb:pressed, QToolButton#uvb:checked{background:#34507a;border-color:#6f9fe8}"
                                         " QDoubleSpinBox{max-height:20px}");
  auto* v = new QVBoxLayout(this); v->setContentsMargins(0, 0, 0, 6); v->setSpacing(3);
  bar_ = new QWidget; bar_->setObjectName("uvbar"); bar_->setFixedHeight(18); bar_->setCursor(Qt::SizeAllCursor); bar_->installEventFilter(this);
  { auto* h = new QHBoxLayout(bar_); h->setContentsMargins(8, 0, 4, 0); auto* l = new QLabel(tr("UV")); l->setStyleSheet("color:#bbb;font-size:11px;font-weight:600"); h->addWidget(l); h->addStretch(1);
    // (the hotkeys switch: on — the keys work on the UV)
    auto* kb = new QToolButton; kb->setText(tr("⌨ UV keys")); kb->setCheckable(true); kb->setChecked(vp->uvKeys); kb->setCursor(Qt::ArrowCursor);
    kb->setStyleSheet("QToolButton{background:transparent;border:1px solid #444;border-radius:3px;color:#aaa;font-size:10px;padding:0 5px} QToolButton:checked{background:#34507a;border-color:#6f9fe8;color:#fff}");
    kb->setToolTip(tr("<b>The hotkeys switched to the UV</b> (the Topology room's selection, view and gizmo keys stay: Ctrl+A / D / I, F, H, B, E / R / T, Shift+X)<br>"
                      "<b>Seams</b> — C seam (polygons: cut off into a shell of their own) · Shift+C off · Alt+C Auto (the closed shells) · W sew · Shift+W snap together<br>"
                      "<b>Unfold</b> — U the selected · Shift+U all · Alt+U symmetric (mirror pairs too) · O Optimize<br>"
                      "<b>Face</b> — L lips / lids open · Shift+L shut · Alt+L off<br>"
                      "<b>Shape</b> — J grid · N line · I by edge · Shift+I upright · M / Shift+M mirror in U / V · [ / ] turn ±90° · K / Shift+K stack / unstack<br>"
                      "<b>Move</b> — arrows a texel (Shift: 10) · Alt+arrows align · Alt+Shift+arrows space out<br>"
                      "<b>Symmetry</b> — Shift+X on / off · Shift+Y Symmetrize · Alt+Y the axis from the selection<br>"
                      "<b>Density</b> — Alt+D take · Shift+D give to the selected<br>"
                      "<b>Pack</b> — Shift+B all · Alt+B the selected · Alt+H lock · G / Shift+G group / ungroup<br>"
                      "<b>Pins</b> — Alt+V / Alt+T the edges straightened up / across and pinned · Alt+Shift+C the pins off<br>"
                      "<b>Brushes</b> — Alt+R Relax (Shift: the border too) · Alt+P Pin (Ctrl: unpin) · [ ] size · Shift+[ ] strength · Esc off<br>"
                      "<b>View</b> — V in turn: plain → stretch by area → by angles → the problems<br>"
                      "With the UV Editor open, the shell under the pointer in 3D shows unfolded in it"));
    connect(kb, &QToolButton::toggled, this, [this](bool on) { vp_->setUvKeys(on); });
    h->addWidget(kb); }
  v->addWidget(bar_);
  auto* body = new QWidget; auto* b = new QVBoxLayout(body); b->setContentsMargins(8, 2, 8, 0); b->setSpacing(3);
  // (the body scrolls when the window would be taller than the program's — the rows never squeezed into one another)
  scroll_ = new QScrollArea; scroll_->setWidget(body); scroll_->setWidgetResizable(true); scroll_->setFrameShape(QFrame::NoFrame); scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll_->setStyleSheet("QScrollArea{background:transparent} QScrollArea > QWidget > QWidget{background:transparent}"); body_ = body; v->addWidget(scroll_);
  auto h5 = [&](const QString& s) { auto* l = new QLabel(s); l->setProperty("h5", true); b->addWidget(l); };
  // a big button: its picture over its name
  auto big = [this](const QIcon& icon, const QString& label, const QString& tip, std::function<void()> fn) {
    auto* x = new QToolButton; x->setObjectName("uvb"); x->setIcon(icon); x->setIconSize(QSize(38, 38)); x->setText(label); x->setToolTip("<b>" + label.toHtmlEscaped() + "</b> — " + tip.toHtmlEscaped());   // (compact, as RizomUV's bars: the icon only, its name first in the tip)
    x->setToolButtonStyle(Qt::ToolButtonIconOnly); x->setFixedSize(44, 44);   // (the size of the Topology tools window's) x->setFocusPolicy(Qt::NoFocus);
    connect(x, &QToolButton::clicked, this, [fn] { fn(); }); return x; };
  auto pic = [](const QString& name) { return ui::icon(name); };
  auto grid = [&](std::initializer_list<QWidget*> ws) { auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 2, 0, 2); g->setSpacing(3); int i = 0;
    for (QWidget* x : ws) { g->addWidget(x, i / 6, i % 6); i++; } g->setColumnStretch(6, 1); b->addWidget(w); };
  status_ = new QLabel; status_->setWordWrap(true); status_->setProperty("mut", true); status_->setMaximumWidth(292); b->addWidget(status_);
  // ---- UV Sets: the selected shells into a new set or an existing one; the list — a click enters a set (again — out),
  // its polygons a shade of blue; Isolate — only its shells shown ----
  h5(tr("UV Sets — the selected shells laid out apart"));
  { auto* nw = big(glyphIcon("＋"), tr("New set"), tr("A new UV Set for the selected shells (whole — a polygon of each is enough) or the selected objects (Object mode: all of them): a layout of their own, worked in at once"), [this] { vp_->uvSetFromShells(-1); });
    setAdd_ = big(glyphIcon("⇲"), tr("Add to ▾"), tr("The selected shells into an existing UV Set"), [this] {
      const int m = vp_->topoMesh(); if (m < 0) return; auto S = vp_->uvSets(m); QMenu menu;
      for (size_t i = 0; i < S.size(); i++) { const int si = int(i); menu.addAction(S[i].name, this, [this, si] { vp_->uvSetFromShells(si); }); }
      menu.exec(QCursor::pos()); });
    setExit_ = big(glyphIcon("⊘"), tr("Exit set"), tr("Out of the UV Set worked in — every polygon shown again"), [this] { const auto P = vp_->uvPicked; if (P.id >= 0 && P.set >= 0) vp_->uvPickSet(P.id, P.set); });
    setIso_ = big(glyphIcon("◐"), tr("Isolate"), tr("Only the shells of the UV Set worked in shown in the UV Editor (off — the rest shown darkened)"), [] {});
    setIso_->setCheckable(true); setIso_->setChecked(vp_->uvSetIso);
    connect(setIso_, &QToolButton::toggled, this, [this](bool on) { vp_->uvSetIso = on; vp_->uvChanged(); emit vp_->topoState(); });
    setDel_ = big(glyphIcon("✕"), tr("Delete set"), tr("Delete the UV Set worked in — its polygons (its mats) go to the first other set; Ctrl+Z brings it back"), [this] { const auto P = vp_->uvPicked; if (P.id >= 0 && P.set >= 0) vp_->uvSetDelete(P.id, P.set); });
    grid({nw, setAdd_, setExit_, setIso_, setDel_});
    sets_ = new QWidget; sets_->setMaximumWidth(300); auto* sl = new QVBoxLayout(sets_); sl->setContentsMargins(0, 0, 0, 0);   // (the sets as buttons — made again in syncSets)
    b->addWidget(sets_);
    // the texture the set worked in (else the mesh) is laid out for: its size (a rectangle too) and the padding in pixels
    auto size = [this](const QString& tip) { auto* c = new QComboBox; c->setEditable(true); c->setFocusPolicy(Qt::ClickFocus); c->setToolTip(tip); c->setMinimumWidth(66);
      for (int s : {256, 512, 1024, 2048, 4096, 8192}) c->addItem(QString::number(s)); return c; };
    texW_ = size(tr("The texture's width in pixels")); texH_ = size(tr("The texture's height in pixels — another than the width: a rectangular texture, the UV Editor's tile drawn in its proportion and the shells kept texel-square in it"));
    texPad_ = new QDoubleSpinBox; texPad_->setRange(0, 256); texPad_->setDecimals(0); texPad_->setSuffix(tr(" px")); texPad_->setFocusPolicy(Qt::ClickFocus); texPad_->setKeyboardTracking(false);
    texPad_->setToolTip(tr("Padding: the gap between the shells in the texture's pixels (half of it off the tile's edges) — Pack keeps it; the bleed a baker / a mip map needs"));
    padShow_ = new QPushButton(tr("▣")); padShow_->setCheckable(true); padShow_->setFixedWidth(28); padShow_->setFocusPolicy(Qt::NoFocus); padShow_->setChecked(vp_->uvShowPad);
    padShow_->setToolTip(tr("Show the padding: round each shell a band of half of it — bands overlapping (darker) or past the tile's edge: the shells closer than the padding"));
    auto apply = [this] { const int m = vp_->topoMesh(); if (m < 0) return; Viewport::UvTex t = vp_->uvTex(m);
      t.w = std::clamp(texW_->currentText().toInt(), 16, 16384); t.h = std::clamp(texH_->currentText().toInt(), 16, 16384); t.pad = texPad_->value();
      const auto o = vp_->uvTex(m); if (o.w != t.w || o.h != t.h || o.pad != t.pad) vp_->uvSetTex(m, t); };
    for (QComboBox* c : {texW_, texH_}) { connect(c, &QComboBox::activated, this, apply); connect(c->lineEdit(), &QLineEdit::editingFinished, this, apply); }   // (a pick, or typed and Enter — not every key)
    connect(texPad_, &QDoubleSpinBox::valueChanged, this, apply);
    connect(padShow_, &QPushButton::toggled, this, [this](bool on) { vp_->uvShowPad = on; vp_->uvChanged(); });
    auto* tr0 = new QWidget; auto* tl = new QHBoxLayout(tr0); tl->setContentsMargins(0, 2, 0, 2); tl->setSpacing(4);
    tl->addWidget(new QLabel(tr("Texture"))); tl->addWidget(texW_); tl->addWidget(new QLabel("×")); tl->addWidget(texH_);
    tl->addWidget(new QLabel(tr("Padding"))); tl->addWidget(texPad_); tl->addWidget(padShow_); tl->addStretch(1); b->addWidget(tr0);
    // the texel density: the one the set wants (texture pixels per the Transform table's unit) and the selected shells'
    tdWant_ = new QDoubleSpinBox; tdWant_->setRange(0, 1e6); tdWant_->setDecimals(2); tdWant_->setKeyboardTracking(false); tdWant_->setFocusPolicy(Qt::ClickFocus); tdWant_->setSpecialValueText(tr("—"));
    tdWant_->setToolTip(tr("Texel density wanted: texture pixels per unit of the model (the unit of the Transform table: mm, cm, m, in) — for this UV Set's texture. Get — from the selected shells; Set — the selected shells scaled to it, each in its place"));
    connect(tdWant_, &QDoubleSpinBox::valueChanged, this, [this](double v) { const int m = vp_->topoMesh(); if (m < 0) return; auto t = vp_->uvTex(m); const double td = v / unitK(vp_->tunit); if (std::abs(td - t.td) > 1e-9) { t.td = td; vp_->uvSetTex(m, t); } });
    auto* tg = new QPushButton(tr("Get")); tg->setFocusPolicy(Qt::NoFocus); tg->setToolTip(tr("Alt+D — the selected shells' texel density becomes the wanted one")); connect(tg, &QPushButton::clicked, this, [this] { vp_->uvDensityGet(); });
    auto* ts = new QPushButton(tr("Set")); ts->setFocusPolicy(Qt::NoFocus); ts->setToolTip(tr("Shift+D — the selected shells scaled to the wanted texel density, each about its own middle")); connect(ts, &QPushButton::clicked, this, [this] { vp_->uvDensitySet(); });
    tdNow_ = new QLabel; tdNow_->setProperty("mut", true); tdNow_->setToolTip(tr("The selected shells' texel density (all of their area) — against the wanted one"));
    auto* td0 = new QWidget; auto* tdl = new QHBoxLayout(td0); tdl->setContentsMargins(0, 2, 0, 2); tdl->setSpacing(4);
    tdl->addWidget(new QLabel(tr("Texel density"))); tdl->addWidget(tdWant_); tdl->addWidget(tg); tdl->addWidget(ts); tdl->addStretch(1); b->addWidget(td0); b->addWidget(tdNow_); }
  // ---- seams ----
  h5(tr("Seams — where Unfold cuts"));
  grid({big(pic("uv_seams"), tr("Seam"), tr("C — UV Seams: the selected edges become seams (polygons selected: their outline)"), [this] { vp_->uvMarkSeams(true); }),
        big(pic("uv_sew"), tr("Sew"), tr("W — UV Sew: the UV joined along the selected edges — both sides' points brought together, the seam gone"), [this] { vp_->uvSew(); }),
        big(pic("uv_seamclear"), tr("Clear"), tr("Shift+C — the selected edges are seams no more (polygons selected: their outline)"), [this] { vp_->uvMarkSeams(false); }),
        big(pic("uv_seamclearall"), tr("Clear all"), tr("Every seam of the mesh cleared"), [this] { vp_->uvClearSeams(); })});
  auto* ang = new QSpinBox; ang->setRange(1, 179); ang->setSuffix("°"); ang->setValue(int(vp_->uvo.angle)); ang->setToolTip(tr("The angle for By angle"));
  connect(ang, &QSpinBox::valueChanged, this, [this](int x) { vp_->uvo.angle = x; vp_->saveUvPrefs(); });
  grid({big(pic("uv_seambyangle"), tr("By angle"), tr("Seams where the polygons meet at more than the angle (added to those there are)"), [this] { vp_->uvSeamsByAngle(vp_->uvo.angle); }),
        big(pic("uv_seamfromuv"), tr("From UV"), tr("The seams = where the mesh's UV breaks now (the shells' borders)"), [this] { vp_->uvSeamsFromBorders(); }),
        big(glyphIcon("✂"), tr("Auto"), tr("Alt+C — the closed shells of the selection (none selected — all shown) given seams that open each into one piece: its handles cut, a path at the back, below, along sharp edges between its far ends, branches while the stretch drops. Unfold does it by itself to a closed shell"), [this] { vp_->uvAutoSeams(); }), ang});
  // ---- unfold, hair strands ----
  h5(tr("Unfold — along your seams; Optimize — the UV as it lies, less stretch"));
  grid({big(pic("uv_unfold"), tr("Unfold"), tr("U — the selection's shells unfolded along the seams you marked (least squares conformal, then SLIM: even stretch, no triangle turned over), each where its UV was, at the density of the rest; pinned edges kept"), [this] { vp_->uvUnfold(false); }),
        big(pic("uv_unfoldall"), tr("Unfold all"), tr("Shift+U — every shell unfolded along the seams (the locked left), then packed (each UV Mat into its own tile)"), [this] { vp_->uvUnfold(true); }),
        big(ui::image("uv_unwrapopt").isNull() ? pic("uv_unfoldall") : pic("uv_unwrapopt"), tr("Optimize"), tr("O — the selection's shells (none picked — all) relaxed as they lie (SLIM): the stretch evened out, no triangle turned over, no seams cut, not moved or packed; pinned edges held"), [this] { vp_->uvOptimize(); })});
  grid({big(pic("uv_straighten"), tr("Straighten"), tr("Hair strands: the selected shells unbent along their length — not into strips (they would stretch): their bends made smaller by Straighten, the width and shape kept; upright when asked"), [this] { vp_->uvStraightenStrands(false); }),
        big(pic("uv_strandsall"), tr("Strands all"), tr("Every shown shell straightened as a hair strand"), [this] { vp_->uvStraightenStrands(true); }),
        big(ui::image("uv_rectangle").isNull() ? glyphIcon("▯") : pic("uv_rectangle"), tr("Rectangle"), tr("Hair strands: the selected shells laid out as strips — along the middle line, the width made one all along (it stretches the UV; Straighten keeps the shape)"), [this] { vp_->uvRectangleStrands(false); }),
        big(ui::image("uv_rectangleall").isNull() ? glyphIcon("▯▯") : pic("uv_rectangleall"), tr("Rect. all"), tr("Every shown shell laid out as a strip"), [this] { vp_->uvRectangleStrands(true); })});
  { auto* s = new SliderRow(tr("Straighten"), 0, 1, 0.05, vp_->uvo.strAmt, [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; });
    s->setTip(tr("How much of the strands' bends goes: 0 — none, 100% — straight (a strip — it stretches the UV)"));
    connect(s, &SliderRow::changed, this, [this](double x) { vp_->uvo.strAmt = x; }); connect(s, &SliderRow::released, this, [this](double) { vp_->saveUvPrefs(); });
    auto* up = check(tr("Upright"), vp_->uvo.strUp, tr("Each straightened strand turned to run along V (up the tile)"));
    connect(up, &QCheckBox::toggled, this, [this](bool x) { vp_->uvo.strUp = x; vp_->saveUvPrefs(); });
    auto* sp = check(tr("Spread out"), vp_->uvo.strSpread, tr("The straightened shells laid apart in the tile as they stand (not turned), the margin between them — none over another"));
    connect(sp, &QCheckBox::toggled, this, [this](bool x) { vp_->uvo.strSpread = x; vp_->saveUvPrefs(); });
    b->addWidget(s); b->addWidget(hrow({up, sp})); }
  // ---- pack, the tile's edges ----
  // ---- the face: lips and lids whose points face each other (open), or are shut (one on another) ----
  h5(tr("Face — lips and lids facing each other"));
  grid({big(glyphIcon("⇕"), tr("Open"), tr("L — the lips' / the eye's rim selected (Edge mode: the loop round the hole), or the holes of the selected shells: the upper and the lower points on one vertical each, the corners level — the texture's strokes go straight up and down; the shell unfolded again. Symmetry on — the other eye too"), [this] { vp_->uvFeatureKey(0); }),
        big(glyphIcon("⇊"), tr("Shut"), tr("Shift+L — as Open, but each upper point on its lower one: the hole zipped — the lids (the lips) laid as if closed, to paint them shut"), [this] { vp_->uvFeatureKey(1); }),
        big(glyphIcon("⊘"), tr("Off"), tr("Alt+L — the lips / lids of the selection (none selected — all) laid free again by the next Unfold"), [this] { vp_->uvFeatureKey(2); })});
  // ---- the selection ----
  h5(tr("The selection — shells, turns, mirrors, lining up"));
  grid({big(pic("uv_shells"), tr("Shells"), tr("The whole shells of what is selected (as polygons)"), [this] { vp_->uvSelectShells(); }),
        big(pic("uv_rotate90"), tr("+90°"), tr("[ — turn the selection's UV anticlockwise about its box's middle"), [this] { vp_->uvTurn(1); }),
        big(mirroredIcon("uv_rotate90"), tr("−90°"), tr("] — turn the selection's UV clockwise"), [this] { vp_->uvTurn(-1); }),
        big(pic("uv_flipu"), tr("Flip U"), tr("M — mirror the selection's UV in U (left ↔ right)"), [this] { vp_->uvFlip(true); }),
        big(pic("uv_flipv"), tr("Flip V"), tr("Shift+M — mirror the selection's UV in V (up ↔ down)"), [this] { vp_->uvFlip(false); }),
        big(pic("uv_alignu"), tr("Align U"), tr("Line the selected UV points up along U (one V: a horizontal line)"), [this] { vp_->uvStraighten(true); }),
        big(pic("uv_alignv"), tr("Align V"), tr("Line the selected UV points up along V (one U: a vertical line)"), [this] { vp_->uvStraighten(false); })});
  { auto* pv = new QPushButton(tr("Gizmo pivot: each shell")); pv->setCheckable(true); pv->setChecked(vp_->uvo.pivShells); pv->setFocusPolicy(Qt::NoFocus);
    pv->setToolTip(tr("The Move / Rotate / Scale gizmo in the UV Editor (the tools E / R / T): on — Rotate and Scale turn and size each selected shell about its own middle; off — the whole selection about its box's middle"));
    connect(pv, &QPushButton::toggled, this, [this](bool on) { vp_->uvo.pivShells = on; vp_->saveUvPrefs(); }); b->addWidget(pv); }
  // ---- lining up: to each other, spaced evenly; a grid, a loop straight, turned by an edge / to the up; stacks ----
  h5(tr("Line up — the selection to itself (Polygon mode: whole shells)"));
  grid({big(pic("uv_alignleft"), tr("Left"), tr("Alt+← — to the selection's left edge: UV points (Vertex / Edge mode), or each shell's left side (Polygon mode)"), [this] { vp_->uvAlign("left"); }),
        big(pic("uv_alignright"), tr("Right"), tr("Alt+→ — to the selection's right edge"), [this] { vp_->uvAlign("right"); }),
        big(pic("uv_aligntop"), tr("Top"), tr("Alt+↑ — to the selection's top edge"), [this] { vp_->uvAlign("top"); }),
        big(pic("uv_alignbottom"), tr("Bottom"), tr("Alt+↓ — to the selection's bottom edge"), [this] { vp_->uvAlign("bottom"); }),
        big(pic("uv_aligncentre"), tr("Mid U"), tr("To the selection's middle across U (one vertical line)"), [this] { vp_->uvAlign("hmid"); }),
        big(mirroredIcon("uv_aligncentre"), tr("Mid V"), tr("To the selection's middle across V (one horizontal line)"), [this] { vp_->uvAlign("vmid"); }),
        big(glyphIcon("⇹"), tr("Space U"), tr("Alt+Shift+← / → — the selected shells spaced evenly along U between the outer two"), [this] { vp_->uvDistribute(true); }),
        big(glyphIcon("⇳"), tr("Space V"), tr("Alt+Shift+↑ / ↓ — the selected shells spaced evenly along V"), [this] { vp_->uvDistribute(false); }),
        big(glyphIcon("▦"), tr("Grid"), tr("J — the selected shells' quads laid as a grid: each quad strip level across, upright along, the lengths even (Follow Quads); in its place, its size"), [this] { vp_->uvGrid(); }),
        big(glyphIcon("⟋"), tr("Line"), tr("N — the selected chain of edges (Edge mode) on a line from its first end to its last, the points spaced as in 3D"), [this] { vp_->uvStraightenLoop(); }),
        big(glyphIcon("∟"), tr("By edge"), tr("I — the selected edge's shell turned so the edge lies level or upright (the nearer)"), [this] { vp_->uvOrientEdge(); }),
        big(glyphIcon("⇡"), tr("Upright"), tr("Shift+I — the selected shells turned so the model's up goes up their V"), [this] { vp_->uvOrientUpright(); }),
        big(glyphIcon("⧉"), tr("Stack"), tr("K — the selected shells laid over the last picked one — sized to it, turned (mirrored) to fit best: one texture for alike parts"), [this] { vp_->uvStack(); }),
        big(glyphIcon("⧈"), tr("Unstack"), tr("Shift+K — shells lying over each other set apart along U, the padding between"), [this] { vp_->uvUnstack(); }),
        big(glyphIcon("⇆"), tr("Snap"), tr("Shift+W — along the selected seam (Edge mode): the smaller shell turned and moved onto the bigger, then sewn there"), [this] { vp_->uvSnapTogether(); })});
  // ---- the UV symmetry: what is done to the selected shells, done mirrored to their mirror shells ----
  h5(tr("Symmetry — the mirrored shells follow"));
  { auto* sb = symBtn_ = new QPushButton(tr("Symmetry")); sb->setCheckable(true); sb->setChecked(vp_->tset.sym); sb->setFocusPolicy(Qt::NoFocus);
    sb->setToolTip(tr("Moving, turning, flipping, reshaping the selected shells moves their mirror shells (the mesh's other side across its X) mirrored across the axis"));
    auto* ax = symAx_ = new QDoubleSpinBox; ax->setRange(-10, 10); ax->setDecimals(3); ax->setSingleStep(0.01); ax->setValue(vp_->uvo.symAxis); ax->setPrefix(tr("U = ")); ax->setToolTip(tr("The vertical line the mirrored shells are mirrored across"));
    auto* fs = new QPushButton(tr("From selection")); fs->setFocusPolicy(Qt::NoFocus); fs->setToolTip(tr("The axis through the middle of the selection — a shell lying across it, or a pair of shells"));
    connect(sb, &QPushButton::toggled, this, [this](bool on) { vp_->setSymmetry(on); });
    connect(ax, &QDoubleSpinBox::valueChanged, this, [this](double x) { vp_->uvo.symAxis = x; vp_->saveUvPrefs(); vp_->uvChanged(); });
    connect(fs, &QPushButton::clicked, this, [this, ax] { if (vp_->uvSymAxisFromSel()) { QSignalBlocker b(ax); ax->setValue(vp_->uvo.symAxis); } });
    auto* row = new QWidget; auto* rl = new QHBoxLayout(row); rl->setContentsMargins(0, 0, 0, 0); rl->setSpacing(4); rl->addWidget(sb); rl->addWidget(ax, 1); rl->addWidget(fs); b->addWidget(row);
    auto* su = new QPushButton(tr("Symmetric unfold")); su->setCheckable(true); su->setChecked(vp_->uvo.symUnfold); su->setFocusPolicy(Qt::NoFocus);
    su->setToolTip(tr("Unfold lays a shell lying across the mesh's middle (X = 0: a face, a torso) symmetric — its middle line straight up, its halves mirror images of each other. Its seams must be symmetric too (mark them with the Topology Symmetry on). With Symmetry on (or Alt+U) a pair of mirror shells (ears, hands) — one unfolded, the other its mirror image across the axis"));
    connect(su, &QPushButton::toggled, this, [this](bool on) { vp_->uvo.symUnfold = on; vp_->saveUvPrefs(); });
    auto* sz = new QPushButton(tr("⇋ Symmetrize")); sz->setFocusPolicy(Qt::NoFocus);
    sz->setToolTip(tr("Shift+Y — the selected side's UV copied mirrored onto the other side (the mesh's mirror across its X = 0): a pair's other shell across the axis, a shell across the middle across its own middle line. A whole shell across the middle selected — from its +X half"));
    connect(sz, &QPushButton::clicked, this, [this] { vp_->uvSymmetrize(); });
    auto* row2 = new QWidget; auto* r2 = new QHBoxLayout(row2); r2->setContentsMargins(0, 0, 0, 0); r2->setSpacing(4); r2->addWidget(su, 1); r2->addWidget(sz, 1); b->addWidget(row2); }
  h5(tr("Pack and snap — into the 0…1 tile, the margin off its edges"));
  auto* pad = new QDoubleSpinBox; pad->setRange(0, 10); pad->setDecimals(1); pad->setSingleStep(0.5); pad->setSuffix(" %"); pad->setValue(vp_->uvo.pad * 100);
  pad->setToolTip(tr("Margin: off the tile's edges for the snaps (left / right / top / bottom / fit), in percent of the tile. Pack keeps the texture's padding in pixels (UV Sets)"));
  connect(pad, &QDoubleSpinBox::valueChanged, this, [this](double x) { vp_->uvo.pad = x / 100; vp_->saveUvPrefs(); });
  auto* rot = check(tr("Turn"), vp_->uvo.rotate, tr("Pack: a shell taller than wide turned by 90° to lie flat"));
  connect(rot, &QCheckBox::toggled, this, [this](bool x) { vp_->uvo.rotate = x; vp_->saveUvPrefs(); });
  grid({big(pic("uv_pack"), tr("Pack"), tr("Alt+B — the shells of the selection packed into the tile by their outlines (the others stay), at the texture's padding; Symmetry on (Shift+X) — symmetric: across the middle on the axis, pairs mirrored, the rest in the gaps"), [this] { vp_->uvPack(false); }),
        big(pic("uv_packall"), tr("Pack all"), tr("Shift+B — every shell packed into the tile by their outlines (each UV Mat into its own), at the texture's padding; Symmetry on (Shift+X) — symmetric"), [this] { vp_->uvPack(true); }),
        big(ui::image("uv_rizom").isNull() ? textIcon("Rizom") : pic("uv_rizom"), tr("RizomUV"), tr("Send the mesh to RizomUV (its UV kept; each UV Mat a tile of its own; shells stacked over each other — groups its Pack keeps stacked; the pins pinned; the texture's size and padding its Pack's): unwrap there and Save — the UV comes back here on every save. Shift + click — pick rizomuv.exe again"),
            [this] { vp_->uvToRizom(QApplication::keyboardModifiers() & Qt::ShiftModifier); })});
  b->addWidget(hrow({new QLabel(tr("Margin")), pad, rot}));
  // each shell's own settings for Pack: kept where it is, never turned, a texel density of its own, laid by its group's
  grid({big(glyphIcon("🔒"), tr("Lock"), tr("Alt+H — the selected shells locked (again — unlocked): Pack and Unfold all leave them where they are, the rest packed round them. Slate in the editor"), [this] { vp_->uvShellSet("lock"); }),
        big(glyphIcon("⤓"), tr("No turn"), tr("The selected shells never turned by Pack (again — may be turned): hair strips, text, a pattern that must run one way"), [this] { vp_->uvShellSet("noturn"); }),
        big(glyphIcon("▦"), tr("Group"), tr("G — the selected shells one pack group: Pack lays them close together (the parts of one thing — a shoe, a glove). A colour of its own in the editor"), [this] { vp_->uvShellSet("group"); }),
        big(glyphIcon("▢"), tr("Ungroup"), tr("Shift+G — the selected shells out of their pack groups"), [this] { vp_->uvShellSet("ungroup"); }),
        big(glyphIcon("✕"), tr("Clear"), tr("The selected shells' own settings cleared: unlocked, may be turned, ×1, no group"), [this] { vp_->uvShellSet("clear"); })});
  { auto* sc = new QDoubleSpinBox; sc->setRange(0.05, 20); sc->setDecimals(2); sc->setSingleStep(0.25); sc->setPrefix("× "); sc->setValue(2); sc->setKeyboardTracking(false); sc->setFocusPolicy(Qt::ClickFocus);
    sc->setToolTip(tr("The texel density of the selected shells against the rest's: ×2 — the face twice as sharp, ×0.5 — the soles half. Pack and Texel density Set keep it"));
    auto* ap = new QPushButton(tr("Apply")); ap->setFocusPolicy(Qt::NoFocus); ap->setToolTip(tr("The selected shells get this density factor (×1 — as the rest)"));
    connect(ap, &QPushButton::clicked, this, [this, sc] { vp_->uvShellSet("scale", sc->value()); });
    b->addWidget(hrow({new QLabel(tr("Shell density")), sc, ap})); }
  grid({big(pic("uv_alignleft"), tr("Left"), tr("The selection against the tile's left edge (the margin off it)"), [this] { vp_->uvSnapEdge("left"); }),
        big(pic("uv_alignright"), tr("Right"), tr("The selection against the tile's right edge (the margin off it)"), [this] { vp_->uvSnapEdge("right"); }),
        big(pic("uv_aligntop"), tr("Top"), tr("The selection against the tile's top edge (the margin off it)"), [this] { vp_->uvSnapEdge("top"); }),
        big(pic("uv_alignbottom"), tr("Bottom"), tr("The selection against the tile's bottom edge (the margin off it)"), [this] { vp_->uvSnapEdge("bottom"); }),
        big(pic("uv_aligncentre"), tr("Centre"), tr("The selection into the tile's middle"), [this] { vp_->uvSnapEdge("centre"); }),
        big(pic("uv_fit"), tr("Fit"), tr("The selection scaled (evenly) to fill the tile inside the margin"), [this] { vp_->uvSnapEdge("fit"); })});
  h5(tr("Warp, puppet, brushes — the UV reshaped by hand (move / turn / scale: the frame of E / R / T)"));
  { auto* wp = big(ui::image("uv_warp").isNull() ? glyphIcon("▦") : pic("uv_warp"), tr("Warp"), tr("A warp mesh over the selection (as Photoshop's): drag its anchors — the UV bends smoothly with them. Again — off"), [this] { vp_->uvTfStart('w'); sync(); });
    auto* pp = big(ui::image("uv_puppet").isNull() ? glyphIcon("📌") : pic("uv_puppet"), tr("Puppet"), tr("Puppet Warp (as Photoshop's): click the selection's UV — pins; drag a pin — the UV follows, kept stiff round the pins (each polygon bent as little as it can be); Ctrl + click a pin — it goes. Again — off"), [this] { vp_->uvTfStart('p'); sync(); });
    pp->setCheckable(true); ppBtn_ = pp;
    wp->setCheckable(true); wpBtn_ = wp;
    auto* gn = new QSpinBox; gn->setRange(1, 12); gn->setValue(vp_->uvo.warpN); gn->setToolTip(tr("The warp mesh: cells along the selection's longer side")); gn->setPrefix(tr("Cells "));
    connect(gn, &QSpinBox::valueChanged, this, [this](int x) { vp_->uvo.warpN = x; vp_->saveUvPrefs(); if (vp_->uvTfKind() == 'w') { vp_->uvTfStop(); vp_->uvTfStart('w'); } });
    grid({wp, pp, gn}); }
  { auto* rb = big(glyphIcon("≈"), tr("Relax brush"), tr("Alt+R — drag over the UV: the points under the circle relaxed (SLIM — the stretch evened out, nothing turned over, the rest held); Shift — the shell's border too (never into an overlap). [ ] the size, Shift+[ ] the strength, Esc off"), [this] { vp_->setUvBrush('r'); sync(); });
    auto* pb = big(glyphIcon("📍"), tr("Pin brush"), tr("Alt+P — drag over the UV points: pinned (yellow dots) — Unfold lays the shell round them, Optimize and the Relax brush hold them; Ctrl — unpinned. Alt+Shift+C — the selection's pins off"), [this] { vp_->setUvBrush('p'); sync(); });
    rb->setCheckable(true); pb->setCheckable(true); rbBtn_ = rb; pbBtn_ = pb;
    auto* bs = brR_ = new QSpinBox; bs->setRange(6, 600); bs->setValue(int(vp_->uvBrushR)); bs->setSuffix(" px"); bs->setToolTip(tr("The brush's radius on the screen ( [ ] )"));
    connect(bs, &QSpinBox::valueChanged, this, [this](int x) { vp_->uvBrushR = x; });
    auto* bk = brK_ = new QDoubleSpinBox; bk->setRange(0.1, 1); bk->setSingleStep(0.1); bk->setDecimals(1); bk->setValue(vp_->uvBrushK); bk->setToolTip(tr("The Relax brush's strength ( Shift+[ ] )"));
    connect(bk, &QDoubleSpinBox::valueChanged, this, [this](double x) { vp_->uvBrushK = x; });
    grid({rb, pb, bs, bk}); }
  // ---- display (on the UV): the checker, the stretch, the texture under it; their opacity ----
  // ---- the checks: what is wrong for the set's texture — each kind counted, its polygons selected on a click ----
  h5(tr("Check — for the texture (each UV Mat's tile apart)"));
  { auto* show = new QPushButton(tr("Show the problems")); show->setCheckable(true); show->setChecked(vp_->uvo.diag); show->setFocusPolicy(Qt::NoFocus);
    show->setToolTip(tr("In the UV Editor: over another polygon — magenta, past the tile — orange, nearer than the padding — yellow, off the wanted texel density — cyan, a mirror pair not mirrored — violet; turned over — red always"));
    connect(show, &QPushButton::toggled, this, [this](bool on) { vp_->uvo.diag = on; vp_->saveUvPrefs(); vp_->uvChanged(); });
    b->addWidget(show);
    auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setHorizontalSpacing(6); g->setVerticalSpacing(2); int row = 0;
    for (auto [kind, name, col] : {std::tuple<const char*, const char*, const char*>{"flipped", "Turned over", "#e03030"}, {"overlap", "Over another", "#e040e0"}, {"outside", "Past the tile", "#ff8c1a"},
                                   {"padding", "Nearer than the padding", "#f0d020"}, {"density", "Off the texel density", "#30c0e0"}, {"asym", "Mirror pair not mirrored", "#9060ff"}}) {
      auto* sw = new QLabel; sw->setFixedSize(10, 10); sw->setStyleSheet(QString("background:%1;border-radius:2px").arg(col));
      auto* lb = new QLabel(tr(name)); auto* n = new QLabel("—"); n->setMinimumWidth(48); n->setAlignment(Qt::AlignRight | Qt::AlignVCenter); checkN_[kind] = n;
      auto* sel = new QPushButton(tr("Select")); sel->setFocusPolicy(Qt::NoFocus); sel->setFixedHeight(20); const QString k = kind;
      connect(sel, &QPushButton::clicked, this, [this, k] { vp_->uvCheckSelect(k); });
      g->addWidget(sw, row, 0); g->addWidget(lb, row, 1); g->addWidget(n, row, 2); g->addWidget(sel, row, 3); row++; }
    g->setColumnStretch(1, 1); b->addWidget(w); }
  h5(tr("Display — on the UV"));
  auto* chk = big(pic("display_uvchecker"), tr("Checker"), tr("The labelled checker under the UV in the UV Editor (the object's own: the strip on the left)"), [] {});
  chk->setCheckable(true); chk->setChecked(vp_->uvo.checker);
  connect(chk, &QToolButton::toggled, this, [this](bool x) { vp_->uvo.checker = x; vp_->saveUvPrefs(); vp_->uvChanged(); sync(); });
  auto* stb = big(pic("display_uvstretch"), tr("Stretch"), tr("In the UV Editor each polygon tinted by its UV area to its 3D area against the mesh's: red — squeezed, blue — stretched"), [] {});
  stb->setCheckable(true); stb->setChecked(vp_->uvo.stretch);
  connect(stb, &QToolButton::toggled, this, [this](bool x) { vp_->uvo.stretch = x; vp_->saveUvPrefs(); vp_->uvChanged(); sync(); });
  auto* sta = big(glyphIcon("∠"), tr("Angles"), tr("The stretch shown by the angles instead of the area: grey — kept, amber — 1.5 times longer one way than the other, red — twice and more (Stretch on)"), [] {});
  sta->setCheckable(true); sta->setChecked(vp_->uvo.stretchAngle);
  connect(sta, &QToolButton::toggled, this, [this](bool x) { vp_->uvo.stretchAngle = x; vp_->saveUvPrefs(); vp_->uvChanged(); });
  auto* txb = big(glyphIcon("🖼"), tr("Texture"), tr("The mesh's texture under its UV in the UV Editor (the picked UV Mat's; the checker, when on, is shown instead)"), [] {});
  txb->setCheckable(true); txb->setChecked(vp_->uvo.tex);
  connect(txb, &QToolButton::toggled, this, [this](bool x) { vp_->uvo.tex = x; vp_->saveUvPrefs(); vp_->uvChanged(); sync(); });
  auto* den = new QSpinBox; den->setRange(8, 256); den->setSingleStep(8); den->setValue(int(vp_->uvo.density)); den->setToolTip(tr("The checker's cells across the tile (on the object too)")); den->setPrefix(tr("Cells "));
  connect(den, &QSpinBox::valueChanged, this, [this](int x) { vp_->uvo.density = x; vp_->saveUvPrefs(); vp_->uvChanged(); });
  grid({chk, stb, sta, txb, den});
  stretchScale_ = new StretchScale; b->addWidget(stretchScale_);   // (the stretch's scale: while it shows — here or on the object)
  dispOp_ = new SliderRow(tr("Opacity"), 0.05, 1, 0.05, vp_->uvo.dispOp, [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; });
  dispOp_->setTip(tr("How strongly the checker / the texture / the stretch show under the UV (and the stretch on the object)"));
  connect(dispOp_, &SliderRow::changed, this, [this](double x) { vp_->uvo.dispOp = x; vp_->uvChanged(); }); connect(dispOp_, &SliderRow::released, this, [this](double) { vp_->saveUvPrefs(); });
  b->addWidget(dispOp_);
  // ---- the transform: the selection's box typed in ----
  h5(tr("Transform — the selection's box in UV"));
  { auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setHorizontalSpacing(4); g->setVerticalSpacing(3);
    const char* names[4] = {"U", "V", "W", "H"};
    const char* tips[4] = {"Its place in U (its left side, or its middle)", "Its place in V (its bottom, or its middle)", "Its width in U", "Its height in V"};
    for (int i = 0; i < 4; i++) {
      auto* s = new QDoubleSpinBox; s->setRange(i < 2 ? -16 : 0, 16); s->setDecimals(4); s->setSingleStep(0.01); s->setKeyboardTracking(false); s->setToolTip(tr(tips[i])); s->setEnabled(false); box_[i] = s;
      auto* l = new QLabel(names[i]); l->setStyleSheet("color:#9a9a9a"); g->addWidget(l, i / 2, (i % 2) * 2); g->addWidget(s, i / 2, (i % 2) * 2 + 1);
      connect(s, &QDoubleSpinBox::editingFinished, this, [this] { if (!box_[0]->isEnabled()) return; vp_->uvSetBox(box_[0]->value(), box_[1]->value(), box_[2]->value(), box_[3]->value()); });
    }
    b->addWidget(w);
    auto* mid = check(tr("By its middle"), vp_->uvo.boxMid, tr("U, V — the box's middle (off — its bottom left corner)"));
    connect(mid, &QCheckBox::toggled, this, [this](bool x) { vp_->uvo.boxMid = x; vp_->saveUvPrefs(); sync(); });
    b->addWidget(mid); }
  auto* keys = muted(tr("In the UV Editor: LMB pick (Shift add, Ctrl take off), a drag on empty space — a box, double click — the shell / the edge loop; drag the selection — move (Shift: one axis); the frame (E / R / T) — its sides and corners scale, its knob turns; Alt+LMB / MMB — pan, Alt+RMB / wheel — zoom, F — frame"));
  keys->setMaximumWidth(292); b->addWidget(keys);
  connect(vp, &Viewport::topoState, this, &UvTools::sync);
  connect(vp, &Viewport::selectionChanged, this, &UvTools::sync);   // (the selection's texel density, its box)
  connect(vp, &Viewport::selectionChanged, this, &UvTools::sync);
  setFixedWidth(326); sync();
}
void UvTools::sync() {
  ProfScope prof_("UvTools");
  if (!isVisible()) return;                                    // (hidden — nothing to show: the UV's numbers not worked out)
  if (scroll_ && body_) { const int maxH = parentWidget() ? std::max(240, parentWidget()->height() - 90) : 900; scroll_->setFixedHeight(std::min(body_->sizeHint().height() + 2, maxH)); }
  if (status_) status_->setText(vp_->uvStatus());
  if (dispOp_) dispOp_->setVisible(vp_->uvo.checker || vp_->uvo.stretch || vp_->uvo.tex || vp_->uvo.s3d); if (stretchScale_) stretchScale_->setVisible(vp_->uvo.stretch || vp_->uvo.s3d);   // (the opacity: while one of them shows)
  if (ppBtn_) ppBtn_->setChecked(vp_->uvTfKind() == 'p'); if (wpBtn_) wpBtn_->setChecked(vp_->uvTfKind() == 'w');
  if (rbBtn_) { QSignalBlocker b1(rbBtn_); rbBtn_->setChecked(vp_->uvBrush == 'r'); } if (pbBtn_) { QSignalBlocker b2(pbBtn_); pbBtn_->setChecked(vp_->uvBrush == 'p'); }
  if (brR_ && !brR_->hasFocus()) { QSignalBlocker b3(brR_); brR_->setValue(int(vp_->uvBrushR)); } if (brK_ && !brK_->hasFocus()) { QSignalBlocker b4(brK_); brK_->setValue(vp_->uvBrushK); }
  auto bx = vp_->uvSelBox(); const bool mid = vp_->uvo.boxMid;
  for (int i = 0; i < 4; i++) if (box_[i] && !box_[i]->hasFocus()) { QSignalBlocker bl(box_[i]); box_[i]->setEnabled(bx.has_value());
    if (!bx) { box_[i]->setValue(0); continue; } const auto& B = *bx;
    const double val = i == 0 ? (mid ? (B[0] + B[2]) / 2 : B[0]) : i == 1 ? (mid ? (B[1] + B[3]) / 2 : B[1]) : i == 2 ? B[2] - B[0] : B[3] - B[1]; box_[i]->setValue(val); }
  if (symBtn_) { QSignalBlocker bl(symBtn_); symBtn_->setChecked(vp_->tset.sym); }
  if (symAx_ && !symAx_->hasFocus()) { QSignalBlocker bl(symAx_); symAx_->setValue(vp_->uvo.symAxis); }
  syncSets();
  adjustSize();
}
// the UV Sets' list: the one worked in — blue, those the selected polygons lie in — lit
void UvTools::syncSets() {
  if (!sets_) return;
  const int m = vp_->topoMesh(); const auto S = m >= 0 ? vp_->uvSets(m) : std::vector<Viewport::UvSetL>{};
  const auto P = vp_->uvPicked; const int cur = P.id == m && P.mat == -2 ? P.set : -1;
  // (made again only when the sets changed — their look follows the selection by itself)
  QString sig = QString::number(m); for (auto& s : S) sig += "|" + s.name + QString::number(s.faces) + "," + QString::number(s.mats.size());
  if (sig != sets_->property("sig").toString()) { sets_->setProperty("sig", sig);
    auto* L = sets_->layout(); while (QLayoutItem* it = L->takeAt(0)) { if (QWidget* x = it->widget()) { x->hide(); x->setParent(nullptr); x->deleteLater(); } delete it; }
    if (m >= 0 && !S.empty()) L->addWidget(uvSetGrid(vp_, m, 6)); else L->addWidget(new QLabel(tr("No UV Sets"))); }
  if (setExit_) setExit_->setEnabled(cur >= 0); if (setDel_) setDel_->setEnabled(cur >= 0); if (setAdd_) setAdd_->setEnabled(!S.empty());
  if (setIso_) { QSignalBlocker bl(setIso_); setIso_->setChecked(vp_->uvSetIso); }
  if (texW_ && m >= 0) { const auto t = vp_->uvTex(m);                   // (the texture of the set worked in, else the mesh's)
    if (!texW_->hasFocus()) { QSignalBlocker bl(texW_); texW_->setCurrentText(QString::number(t.w)); }
    if (!texH_->hasFocus()) { QSignalBlocker bl(texH_); texH_->setCurrentText(QString::number(t.h)); }
    if (!texPad_->hasFocus()) { QSignalBlocker bl(texPad_); texPad_->setValue(t.pad); } }
  if (padShow_) { QSignalBlocker bl(padShow_); padShow_->setChecked(vp_->uvShowPad); }
  if (!checkN_.empty()) {                                        // (the checks' counts — the report kept till the UV changes)
    const auto& r = vp_->uvCheck();
    const std::map<QString, size_t> n = {{"flipped", r.flipped.size()}, {"overlap", r.overlap.size()}, {"outside", r.outside.size()}, {"padding", r.padding.size()}, {"density", r.density.size()}, {"asym", r.asym.size()}};
    for (auto& [k, lb] : checkN_) { const size_t c = m >= 0 ? n.at(k) : 0; lb->setText(c ? QString::number(c) : tr("✓")); lb->setStyleSheet(c ? "color:#ffb070" : "color:#7ad07a"); }
    if (auto* o = checkN_["overlap"]; o && r.stacks) o->setToolTip(tr("%1 stack%2 (alike shells one on another) not counted").arg(r.stacks).arg(r.stacks == 1 ? "" : "s")); }
  if (tdWant_ && m >= 0) {                                       // (the texel density: in the Transform table's unit — and in px/m, the usual)
    const auto t = vp_->uvTex(m); const double k = unitK(vp_->tunit);
    if (!tdWant_->hasFocus()) { QSignalBlocker bl(tdWant_); tdWant_->setSuffix(tr(" px/%1").arg(vp_->tunit)); tdWant_->setValue(t.td * k); }
    const double d = vp_->uvDensityOfSel();
    if (d <= 0) tdNow_->setText(tr("Selected: — (select polygons of a shell)"));
    else { QString s = tr("Selected: %1 px/%2 (%3 px/m)").arg(d * k, 0, 'f', 2).arg(vp_->tunit).arg(d * 100, 0, 'f', 0);
      if (t.td > 0) { const double r = d / t.td - 1; s += std::abs(r) < 0.005 ? tr(" — as wanted") : tr(" — %1%2% of the wanted").arg(r > 0 ? "+" : "−").arg(std::abs(r) * 100, 0, 'f', 1); }
      tdNow_->setText(s); } }
}
void UvTools::save() const {
  QPoint p = pos() - parentWidget()->mapToGlobal(QPoint(0, 0));
  QSettings().setValue("topo/uvWin", QJsonDocument(QJsonObject{{"x", p.x()}, {"y", p.y()}}).toJson(QJsonDocument::Compact));
}
void UvTools::place(QWidget* vpBox) {
  adjustSize();
  QJsonObject st = QJsonDocument::fromJson(QSettings().value("topo/uvWin").toByteArray()).object();
  QWidget* w = parentWidget(); QPoint o = w->mapToGlobal(QPoint(0, 0));
  QPoint p = st.contains("x") ? o + QPoint(st.value("x").toInt(), st.value("y").toInt()) : vpBox->mapToGlobal(QPoint(vpBox->width() - width() - 130, 34));
  QRect g(o, w->size()); p.setX(std::clamp(p.x(), g.left(), g.right() - 60)); p.setY(std::clamp(p.y(), g.top(), g.bottom() - 40));
  move(p);
}
bool UvTools::eventFilter(QObject* o, QEvent* e) {
  if (o != bar_) return QFrame::eventFilter(o, e);
  if (e->type() == QEvent::MouseButtonPress) { auto* m = static_cast<QMouseEvent*>(e); if (m->button() == Qt::LeftButton) { drag_ = true; grab_ = m->globalPosition().toPoint() - pos(); } return true; }
  if (e->type() == QEvent::MouseMove && drag_) { move(static_cast<QMouseEvent*>(e)->globalPosition().toPoint() - grab_); return true; }
  if (e->type() == QEvent::MouseButtonRelease && drag_) { drag_ = false; save(); return true; }
  return QFrame::eventFilter(o, e);
}

// ---------- the tools' floating window ----------
// the ToolBar: one window — its top strip dragged; two sections side by side (the tools by groups: transform — Move,
// Rotate, Scale, the move brush; selection — the Select Cursor and the selection kinds; the tool's options), the line
// between them dragged to share the width, the corner grip — the window's size; its place and sizes kept
TopoTools::TopoTools(QWidget* window, Viewport* vp, std::function<void(const QString&)> pick) : QFrame(window, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), vp_(vp), pick_(std::move(pick)) {
  setObjectName("ttoolWin"); setAttribute(Qt::WA_TranslucentBackground);   // (round the window's corners: nothing — no black)
  setStyleSheet("#ttoolWin{background:transparent} #ttPane{background:#262626;border:1px solid #555;border-radius:6px} #ttbar{background:#2b2b2b;border-bottom:1px solid #3a3a3a;border-top-left-radius:6px;border-top-right-radius:6px}"
                " #ttHead{color:#8a8f96;font-size:10px;letter-spacing:1px} QSplitter::handle{background:#333} QSplitter::handle:hover{background:#4a6fb0}"
                " #ttOpts QLabel{font-size:11px} #ttOpts QPushButton{font-size:11px;padding:2px 6px;min-height:18px} #ttOpts QCheckBox{font-size:11px}");
  auto* outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
  auto* pane = new QFrame; pane->setObjectName("ttPane"); outer->addWidget(pane);
  auto* v = new QVBoxLayout(pane); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
  bar_ = new QWidget; bar_->setObjectName("ttbar"); bar_->setFixedHeight(14); bar_->setCursor(Qt::SizeAllCursor); bar_->installEventFilter(this);
  bar_->setToolTip(tr("Drag — move the ToolBar")); v->addWidget(bar_);
  auto head = [](const QString& t) { auto* l = new QLabel(t); l->setObjectName("ttHead"); return l; };
  split_ = new QSplitter(Qt::Horizontal); split_->setChildrenCollapsible(false); split_->setHandleWidth(3); v->addWidget(split_, 1);
  // ---- the tools ----
  auto* left = new QWidget; auto* lv = new QVBoxLayout(left); lv->setContentsMargins(6, 4, 6, 6); lv->setSpacing(3);
  lv->addWidget(head(tr("TOOLS")));
  row_ = new QWidget; auto* g = new QGridLayout(row_); g->setContentsMargins(0, 0, 0, 0); g->setSpacing(4); lv->addWidget(row_); lv->addStretch(1);
  struct T { const char *id, *icon, *tip; int r, c; };
  // (by what they do, a row each: moving, selecting, building, cutting, welding and cleaning, shaping, normals, symmetry)
  const T ts[] = {
    {"move", "tool_move", "Move (E) — the gizmo; its axes and the soft selection in the options. Alt + a handle: the gizmo itself", 0, 0},
    {"rotate", "tool_rotate", "Rotate (R) — Shift: 45° steps. Alt + a ring: the gizmo's axes turned", 0, 1},
    {"gizmo", "tool_scale", "Scale (T) — Shift: 0.1 steps down to 0 (flatten), Ctrl: uniform; Ctrl + drag with polygons selected — inset (new polygons round them), then scaled", 0, 2},
    {"tbrush", "tool_brush", "Brush — LMB: as its mode (RMB — the modes); Shift: smooth; Shift+Ctrl: relax; Ctrl: along the normals; Ctrl+RMB drag: size ←→, hardness ↑↓. With a selection — only it", 0, 3},
    {"tslide", "tt_slide", "Slide — selects as the Select Cursor; a drag in empty space slides the selected vertices along their edges: LMB proportionally, RMB in parallel", 0, 4},
    {"sel:cursor", "tsel_cursor", "Select Cursor (Q) — a click picks, a held LMB paints the selection", 1, 0},
    {"sel:rect", "tsel_rect", "Rectangle — a held LMB draws a box to select in", 1, 1},
    {"sel:lasso", "tsel_lasso", "Lasso — a held LMB draws the outline to select inside", 1, 2},
    {"sel:loop", "tsel_loop", "Loop — the loop under the pointer lit yellow, a click picks it", 1, 3},
    {"sel:ring", "tsel_ring", "Ring — the parallel ring under the pointer lit yellow, a click picks it", 1, 4},
    {"sel:fill", "tsel_fill", "Fill — a click picks the region (up to a cut or the selection's frame); Ctrl — the selected piece taken off", 2, 0},
    {"sel:outline", "tsel_outline", "Outline — a click picks the open outline (or the picked polygons' border)", 2, 1},
    {"sel:hard", "tsel_hard", "Hard edges — the line of hard edges under the pointer lit, a click picks it (Shift — adds, Ctrl — takes off); it runs on through each vertex two hard edges meet at", 2, 3},
    {"act:ngons", "insp_ngons", "Select N-gons — every polygon of more than four sides of the active mesh (Shift — added to the selection)", 2, 2},
    {"tpen", "tt_polypen", "PolyPen — the element under it lit, a held LMB drags it; clicks draw a polygon (the first point or a double click closes, Esc drops); Ctrl + drag — pull out / extrude, Ctrl + click — delete; Shift + click — an anchor, Shift+RMB — fill between points; Ctrl+Shift — Loop", 3, 0},
    {"tbridge", "tt_bridge", "Bridge — a click: polygons between two chains of edges (or two groups of polygons)", 3, 1},
    {"tfill", "tt_fillhole", "Fill Hole — the hole nearest the pointer (or those the selection touches) shown filled: LMB fills, RMB — the next variant, the wheel — the kind; Shift / Ctrl + RMB drag — Edge Flow / Subdivision", 3, 2},
    {"tbevel", "tt_bevel", "Bevel / Extrude — the selected polygons pushed out: drag sideways — how far; the bevel, caps and what is selected in the options", 3, 3},
    {"tinset", "tt_inset", "Inset — the selected polygons' rim drawn in: drag sideways — how far; on vertices — a circle round each one (click)", 3, 4},
    {"tloop", "tt_loop", "Loop — a cut across the ring of polygons: the wheel — how many, Shift — onto the marks, Ctrl — parallel; LMB cuts, RMB cuts and selects", 4, 0},
    {"tknife", "tt_knife", "Knife — clicks on the mesh draw a smooth cut (Esc ends a curve, Enter cuts), a drag from empty space — a straight cut; Ctrl + drag — move an anchor; Shift+RMB — its menu", 4, 1},
    {"tplane", "tt_planecut", "Plane Cut — drag a line: the mesh cut by the plane through it (cut, split, one side removed)", 4, 2},
    {"treveal", "topo_reveal", "Reveal Brush — paint over the Topology Ghost: its polygons under the stroke become real, welded onto what is built there (no mesh picked — a new one). Ctrl+wheel / [ ] — its size", 4, 3},
    {"tpatch", "tt_patch", "PatchTool — LMB clicks place anchors of a curve on the references (double click / Enter ends it); RMB — pick an anchor and drag it, on a curve — a new anchor, Ctrl+RMB — delete one. Shift + drag along the open edges — an edge curve on them (its divisions their vertices). Crossing curves (or the mesh's open rim) close contours shown filled; Enter makes the patch", 4, 4},
    {"tlobe", "tool_braid", "Lobe Brush — paint the lobes of a braid on the high-poly mesh, each a layer: LMB paints the active lobe (white), Ctrl erases it, Shift fills up to the crevices, Ctrl+Shift picks the lobe under the pointer; Enter — a new lobe. Build makes a quad mesh of them, a grid per lobe", 5, 3},
    {"tweld", "tt_weld", "Weld — select, then RMB: in a point (onto the vertex under the pointer, else their middle) or in pairs (two chains of edges)", 5, 0},
    {"act:collapse", "tt_collapse", "Collapse Edge — the selected edges each into a vertex (edges joined to each other — one)", 5, 1},
    {"tfixmesh", "tt_fixmesh", "FixMesh — the Fix button in its options: collapsed and doubled polygons taken out, parts welded where they only touch (teeth sewn along their bite) cut apart, each part's polygons turned one way, the separate parts made meshes of their own", 5, 4},
    {"toptimize", "tt_optimize", "Optimize — selects as the Select Cursor; the Optimize button in its options: the clean-up — points in one place welded (the distance — the options, or picked by a drag), unused points and polygons of one or two points taken out, doubled polygons made one; the selection, else all", 5, 2},
    {"tshaper", "tt_shaper", "Edge Shaper — selects as the Select Cursor; a drag → in empty space: LMB even spacing, RMB Edge Flow, Shift+LMB flat on a plane (three vertices — their plane), Shift+RMB straight", 6, 0},
    {"tchain", "tt_edgechain", "Smooth EdgeChain — select edges, Shift+RMB → a curve they follow; drag its anchors, a click on it — a new anchor; smooth / even them in the options", 6, 1},
    {"act:relax", "tt_relax", "Relax Mesh (Ctrl+R) — the selected vertices (none — all) evened out on the surface, the rim kept", 6, 2},
    {"act:snapgeom", "tt_snapgeom", "Snap to the references — the selected vertices (none — all) onto the reference meshes' surface", 6, 3},
    {"tnormals", "tt_normals", "Normals — selects as the Select Cursor, the normals shown; its options: Smooth (hard at the border), Turn (then a drag in the viewport), Copy from a mesh (then a click on it)", 7, 0},
    {"act:alignN", "tt_alignnormals", "Align Normals — the selected polygons (none — all) turned to face one way, each piece outward", 7, 1},
    {"act:reverseN", "tt_reversenormals", "Reverse Normals — the selected polygons (none — all) turned round", 7, 2},
    {"tmirror", "tt_mirror", "Mirror — the Mirror button in its options: a copy by the axis (the side found by itself, the seam welded), or round it — radial, kaleidoscope, half a flower; the axis and kind in the options", 8, 0},
    {"act:vsym", "tt_vsymgroup", "Virtual symmetry group — the selected meshes into a group whose symmetry is only shown (in any place); on such a group: Apply makes it real", 8, 1},
    {"act:identical", "tt_identical", "Apply To Identical — the selected retopology polygons (none — the whole mesh) of one part of a high-poly copied onto every topologically identical part of it (and onto copies of that mesh); UVs, seams, creases, hard edges carried", 8, 2}};
  for (const T& t : ts) {
    auto* b = toolBtn(t.icon, tr(t.tip)); b->setFixedSize(44, 44); b->setIconSize(QSize(38, 38)); QString id = t.id;
    b->setObjectName("tw_" + id.replace(':', '_')); id = t.id;
    connect(b, &QPushButton::clicked, this, [this, id] {
      if (id.startsWith("act:")) { runAct(id.mid(4)); sync(); return; }
      if (id.startsWith("sel:")) { pick_("cursor"); vp_->setSelTool(id.mid(4)); } else pick_(id);
      sync(); });
    g->addWidget(b, t.r, t.c);
  }
  left->setMinimumWidth(left->sizeHint().width());
  split_->addWidget(left);
  // ---- the options (scrolled when the window is short) ----
  auto* right = new QWidget; auto* rv = new QVBoxLayout(right); rv->setContentsMargins(6, 4, 4, 4); rv->setSpacing(2);
  rv->addWidget(head(tr("TOOL OPTIONS")));
  auto* sa = new QScrollArea; sa->setWidgetResizable(true); sa->setFrameShape(QFrame::NoFrame); sa->setStyleSheet("QScrollArea{background:transparent} QScrollArea > QWidget > QWidget{background:transparent}");
  opts_ = new QWidget; opts_->setObjectName("ttOpts"); auto* ol = new QVBoxLayout(opts_); ol->setContentsMargins(0, 2, 2, 2); ol->setSpacing(2);
  sa->setWidget(opts_); rv->addWidget(sa, 1);
  right->setMinimumWidth(150);
  split_->addWidget(right); split_->setStretchFactor(0, 0); split_->setStretchFactor(1, 1);
  // ---- the corner grip: the window's size ----
  auto* gr = new QHBoxLayout; gr->setContentsMargins(0, 0, 2, 2); gr->addStretch(1); auto* grip = new QSizeGrip(pane); grip->setFixedSize(12, 12); gr->addWidget(grip); v->addLayout(gr);
  setMinimumSize(360, std::max(150, left->sizeHint().height() + 34));   // (every row of tools seen)
  const QJsonObject st = QJsonDocument::fromJson(QSettings().value("topo/tools").toByteArray()).object();
  resize(st.value("w").toInt(470), std::max(st.value("h").toInt(200), minimumHeight()));
  if (st.contains("split")) split_->setSizes({st.value("split").toArray().at(0).toInt(), st.value("split").toArray().at(1).toInt()});
  else split_->setSizes({left->sizeHint().width(), 200});
  connect(split_, &QSplitter::splitterMoved, this, [this] { save(); });
  connect(vp, &Viewport::toolChanged, this, &TopoTools::sync);
  connect(vp, &Viewport::topoState, this, &TopoTools::sync);
  sync();
}
void TopoTools::resizeEvent(QResizeEvent* e) { QFrame::resizeEvent(e); if (isVisible()) save(); }   // (its size kept)

void TopoTools::sync() {
  ProfScope prof_("TopoTools");
  const QString tool = vp_->tool(), st = vp_->tset.selTool;
  for (auto* b : findChildren<QPushButton*>()) {
    const QString n = b->objectName(); if (!n.startsWith("tw_")) continue;
    const QString id = n.mid(3);
    b->setCheckable(true);
    b->setChecked(id.startsWith("sel_") ? tool == "cursor" && st == id.mid(4) : tool == id);
  }
  buildOptions();
}

// the options of the tool in use (made again only when what they show changed)
void TopoTools::buildOptions() {
  auto& S = vp_->tset; const QString tool = vp_->tool();
  const QString sig = tool + "|" + S.selTool + "|" + S.gzOrient + "|" + QString::number(S.gzFree) + S.gzWhat + QString::number(S.gzSlide) + S.alAxis + S.alTo + "|" + S.brushMode + "|" + QString::number(S.soft) + S.softMode + QString::number(S.selThrough)
                      + QString::number(S.loopStopSel) + QString::number(S.loopStopNgon) + QString::number(S.loopStopTri) + QString::number(S.relaxStars) + QString::number(vp_->topoMode == "object")
                      + S.brushKind + vp_->ttools.weldMode + vp_->ttools.weldPos + vp_->ttools.planeMode + QString::number(vp_->penAnchorCount() > 0) + QString::number(vp_->chainActive()) + vp_->topoMode
                      + QString::number(vp_->ttools.loopCuts) + QString::number(vp_->ttools.loopParallel) + QString::number(vp_->ttools.chainEqual) + vp_->ttools.fillMode + vp_->ttools.mirType + QString::number(vp_->ttools.mirAxis) + QString::number(vp_->ttools.mirLocal) + vp_->ttools.penBrush + QString::number(int(vp_->ttools.penSmooth));
  QString psig; if (tool == "tpatch") { vp_->patchBuild(); const auto& P = vp_->pt_; psig = QString("|p%1,%2,%3,%4%5%6,%7,%8,%9").arg(P.selC).arg(P.selC >= 0 && size_t(P.selC) < P.c.size() ? P.c[size_t(P.selC)].div : 0)
      .arg(P.selC >= 0 && size_t(P.selC) < P.c.size() ? QChar(P.c[size_t(P.selC)].mode) : QChar(0x2d)).arg(P.c.size()).arg(P.toDense).arg(P.triFan).arg(P.zoneChoice).arg(P.F.size()).arg(P.note) + QChar(P.kind) + QString::number(P.stripAcross) + QString("|o%1,%2").arg(P.ordering).arg(P.order.size()); }
  if (tool == "tlobe") { const int m = vp_->lobeTarget(); psig = QString("|l%1,%2,%3").arg(m).arg(m >= 0 ? vp_->lobeActive(m) : 0).arg(m >= 0 ? vp_->scene()->item(m)->raw.value("lobeF").toString().size() : 0); if (m >= 0) for (const QString& n : vp_->lobeNames(m)) psig += "," + n; if (m >= 0) { psig += vp_->lobeGhostInfo() + QString("|%1").arg(int(vp_->lb_.gh.size())); for (const QJsonValue& x : vp_->scene()->item(m)->raw.value("lobeFlip").toArray()) psig += "f" + QString::number(x.toInt()); } psig += vp_->lb_.ghost ? "G" : "-"; }
  if (tool == "tnormals") psig += "|n" + vp_->ttools.normMode;      // (Face: the Nose slider in place of Smoothness)
  if (sig + psig == optSig_) return;
  optSig_ = sig + psig;
  auto* L = static_cast<QVBoxLayout*>(opts_->layout());
  while (QLayoutItem* it = L->takeAt(0)) { if (QWidget* w = it->widget()) { w->hide(); w->setParent(nullptr); w->deleteLater(); } delete it; }
  Viewport* vp = vp_;
  // a column of choices, the current one lit
  auto choices = [&](std::vector<std::pair<QString, QString>> items, const QString& cur, std::function<void(const QString&)> set, const QString& tip = {}) {
    auto* w = new QWidget; auto* gl = new QGridLayout(w); gl->setContentsMargins(0, 0, 0, 0); gl->setSpacing(3); int i = 0;
    for (auto& [key, label] : items) { auto* b = btn(label, tip); b->setCheckable(true); b->setChecked(key == cur); QString k = key;
      connect(b, &QPushButton::clicked, this, [set, k] { set(k); }); gl->addWidget(b, i / 2, i % 2); i++; }
    L->addWidget(w);
  };
  auto head = [&](const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setStyleSheet("margin-top:4px"); L->addWidget(l); };
  auto saved = [vp] { vp->saveTopoPrefs(); vp->topoChanged(); emit vp->topoState(); };
  auto softBlock = [&] {
    head(tr("Soft Selection (B)"));
    auto* on = check(tr("On"), S.soft, tr("The vertices near the selection follow it, the further the less (B — on / off; B held + move sideways — the radius)"));
    connect(on, &QCheckBox::toggled, this, [vp, saved](bool x) { vp->tset.soft = x; saved(); }); L->addWidget(on);
    if (!S.soft) return;
    auto* r = new SliderRow(tr("Radius, cm"), 0.05, 50, 0.05, std::min(50.0, S.softR), [](double x) { return QString::number(x, 'g', 3); });
    connect(r, &SliderRow::changed, this, [vp](double x) { vp->tset.softR = x; vp->topoChanged(); });
    connect(r, &SliderRow::released, this, [vp](double) { vp->saveTopoPrefs(); }); L->addWidget(r);
    choices({{"surface", tr("Object only")}, {"volume", tr("Radius")}}, S.softMode, [vp, saved](const QString& k) { vp->tset.softMode = k; saved(); },
            tr("Object only — only what is joined to the selection moves (the parts near it don't); Radius — everything of the mesh within the radius"));
  };
  if (tool == "move" || tool == "rotate" || tool == "gizmo") {
    head(tr("Orientation"));
    choices({{"world", tr("World")}, {"local", tr("Local")}, {"normal", tr("Normal")}, {"camera", tr("Camera")}, {"custom", tr("Custom")}}, S.gzOrient,
            [vp](const QString& k) { vp->setGzOrient(k); }, tr("The gizmo's axes: the world's, the mesh's own, along the selection's normal, the view's, or where Alt + drag put them"));
    auto* fr = check(tr("Free gizmo"), S.gzFree, tr("A drag of a handle moves / turns the gizmo itself (as Alt + drag) — the model stays"));
    connect(fr, &QCheckBox::toggled, this, [vp, saved](bool x) { vp->tset.gzFree = x; saved(); }); L->addWidget(fr);
    if (tool == "move") {   // (Shift while moving: in steps of this length along the gizmo's axes)
      auto* st = new SliderRow(tr("Shift step, cm"), 0.01, 100, 0.01, S.gzStep, [](double x) { return QString::number(x, 'g', 3); });
      st->setLog(true); st->setReset(1); st->setTip(tr("Hold Shift while moving with the gizmo: the move goes in steps of this length along its axes (Rotate: 45°, Scale: 0.1 steps). Double click — 1 cm"));
      connect(st, &SliderRow::changed, this, [vp](double x) { vp->tset.gzStep = x; });
      connect(st, &SliderRow::released, this, [vp](double) { vp->saveTopoPrefs(); }); L->addWidget(st);
    }
    if (vp->topoMode != "object") {
      head(tr("What moves"));
      choices({{"all", tr("The selection")}, {"each", tr("Components")}}, S.gzWhat, [vp, saved](const QString& k) { vp->tset.gzWhat = k; saved(); },
              tr("The selection — one piece about one centre; Components — each polygon / edge / vertex about its own centre, on its own axes with Normal (Normal + Components, polygons, Z — inflated along the normals)"));
      auto* sl = check(tr("Slide along the edges (parallel)"), S.gzSlide, tr("Move: each vertex slides along its edge that goes the drag's way, all by the same distance — an edge pulled stays parallel, on the surface"));
      connect(sl, &QCheckBox::toggled, this, [vp, saved](bool x) { vp->tset.gzSlide = x; saved(); }); L->addWidget(sl);
      L->addWidget(muted(tr("Ctrl + drag a handle: extrude (polygons / edges), then move. A drag in free space (off the mesh) moves as the gizmo's centre; Ctrl + drag there — the selection cleared.")));
    }
    L->addWidget(muted(vp->topoMode == "object" ? tr("Alt + drag a handle: the gizmo itself moved / turned — kept in the mesh's Transform (Bake: they become its own axes).")
                                                : tr("Alt + drag a handle: the gizmo itself moved / turned (Custom) — the model stays.")));
    head(tr("Align"));
    choices({{"x", "X"}, {"y", "Y"}, {"z", "Z"}, {"xy", tr("Plane XY")}, {"yz", tr("Plane YZ")}, {"xz", tr("Plane XZ")}}, S.alAxis, [vp, saved](const QString& k) { vp->tset.alAxis = k; saved(); },
            tr("The axis turned (or the plane — the axis square to it)"));
    choices({{"sel", tr("The selection")}, {"normal", tr("Normal")}, {"view", tr("The view")}, {"world", tr("World")}}, S.alTo, [vp, saved](const QString& k) { vp->tset.alTo = k; saved(); },
            tr("The selection — one vertex: the axis points at it, the centre stays; vertices / edges in a line: the axis along it; in a plane: the axis lies in it (twice — two axes), the centre moves to the selection's; Normal — the selection's; The view — at the camera; World — the world's axes back"));
    { auto* go = btn(tr("Align"), tr("Turn the object's own gizmo so (its axes and centre kept in the mesh's Transform) — the selection in a component mode gives the target")); go->setProperty("primary", true);
      connect(go, &QPushButton::clicked, this, [vp] { vp->gzAlign(vp->tset.alAxis, vp->tset.alTo); }); L->addWidget(go); }
    if (vp->topoMode != "object") softBlock(); else L->addWidget(muted(tr("Soft Selection: in a component mode.")));
  } else if (tool == "tbrush") {
    head(tr("Mode"));
    choices({{"screen", tr("Screen")}, {"surface", tr("Surface")}, {"topo", tr("Topology surface")}}, S.brushMode, [vp, saved](const QString& k) { vp->tset.brushMode = k; saved(); },
            tr("Screen — moved in the view's plane; Surface — slid along each mesh's surface (the shape kept); Topology surface — as Surface, only the mesh under the brush's centre (or the selection)"));
    auto* sz = new SliderRow(tr("Size, px"), 5, 400, 1, std::clamp(vp->brushR, 5.0, 400.0), [](double x) { return QString::number(int(x)); });
    connect(sz, &SliderRow::changed, this, [vp](double x) { vp->brushR = x; }); L->addWidget(sz);
    auto* sg = new SliderRow(tr("Strength"), 0.02, 1, 0.01, S.brushStr, [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; });
    connect(sg, &SliderRow::changed, this, [vp](double x) { vp->tset.brushStr = x; });
    connect(sg, &SliderRow::released, this, [vp](double) { vp->saveTopoPrefs(); }); L->addWidget(sg);
    auto* rs = check(tr("Relax: keep the stars even"), S.relaxStars, tr("Relax (Shift+Ctrl): the vertices of other than four edges (stars) evened out too — off: they stay where they are and the rest lines up to them"));
    connect(rs, &QCheckBox::toggled, this, [vp, saved](bool x) { vp->tset.relaxStars = x; saved(); }); L->addWidget(rs);
    auto* ba = check(tr("All meshes"), S.brushAll, tr("Off — only the active mesh is touched; on — every visible mesh under the brush"));
    connect(ba, &QCheckBox::toggled, this, [vp, saved](bool x) { vp->tset.brushAll = x; saved(); }); L->addWidget(ba);
    head(tr("LMB does"));
    choices({{"move", tr("Move")}, {"smooth", tr("Smooth")}, {"relax", tr("Relax")}, {"inflate", tr("Inflate")}, {"shrink", tr("Shrink")}}, S.brushKind, [vp, saved](const QString& k) { vp->tset.brushKind = k; saved(); },
            tr("What LMB does — also RMB on the viewport"));
    L->addWidget(muted(tr("RMB — the modes · Shift — smooth · Shift+Ctrl — relax · Ctrl — along the normals (drag) · Ctrl+RMB drag: ←→ size, ↑↓ hardness · Ctrl + wheel — size. With a selection only it is touched. Symmetry: the side the stroke is on.")));
  } else if (tool == "cursor") {
    const QString st = S.selTool;
    if (st == "cursor" || st == "rect" || st == "lasso") {
      head(tr("Selection"));
      choices({{"0", tr("Visible only")}, {"1", tr("Through")}}, S.selThrough ? "1" : "0", [vp, saved](const QString& k) { vp->tset.selThrough = k == "1"; saved(); },
              tr("Visible only — what the camera sees; Through — the hidden side too"));
    }
    if (st == "loop" || st == "ring") {
      head(tr("Stop"));
      auto add = [&](const QString& t, bool on, bool Viewport::TSet::*field) { auto* c = check(t, on); connect(c, &QCheckBox::toggled, this, [vp, saved, field](bool x) { vp->tset.*field = x; saved(); }); L->addWidget(c); };
      add(tr("Before selected"), S.loopStopSel, &Viewport::TSet::loopStopSel);
      add(tr("Before n-gon"), S.loopStopNgon, &Viewport::TSet::loopStopNgon);
      add(tr("Before tris"), S.loopStopTri, &Viewport::TSet::loopStopTri);
    }
    if (st == "fill") L->addWidget(muted(tr("A click — the region up to a cut or the selection's frame. Shift adds, Ctrl takes the selected piece off.")));
    if (st == "outline") L->addWidget(muted(tr("A click on a mesh — every border of it (all its open holes and edges, however far), or the selected polygons' border.")));
    L->addWidget(muted(tr("Shift+RMB — every selection kind and the edits.")));
  } else if (vp->isModelTool()) buildModelOptions(L);
  else L->addWidget(muted(tr("No options.")));
  L->addStretch(1);
  if (tool == "cursor") {                                         // (the bottom: the edits of the selection — as the Ctrl+RMB menu has them)
    auto* sep = new QFrame; sep->setFrameShape(QFrame::HLine); sep->setStyleSheet("color:#3a3a3a"); L->addWidget(sep);
    head(tr("Edits of the selection (Ctrl+RMB)"));
    auto* w = new QWidget; auto* gl = new QGridLayout(w); gl->setContentsMargins(0, 0, 0, 0); gl->setSpacing(3); int i = 0;
    auto op = [&](const QString& icon, const QString& tip, std::function<void()> fn) {
      auto* b = new QPushButton; b->setIcon(ui::icon(icon)); b->setIconSize(QSize(28, 28)); b->setFixedSize(34, 34); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus);
      connect(b, &QPushButton::clicked, this, [vp, fn, tip] { vp->heavyWork(tip.section(QChar(0x2014), 0, 0).trimmed(), fn); }); gl->addWidget(b, i / 5, i % 5); i++; return b; };
    op("top_mirror", tr("Mirror — a mirrored copy of the selected polygons in the same polygroup"), [vp] { vp->tMirrorGeo(); });
    op("top_melt", tr("Melt — edges / polygons: the edges between them gone; vertices: dissolved"), [vp] { vp->tMelt(); });
    auto* sm = op("top_selmirror", tr("Select Mirror — the selection copied onto the other side of X"), [] {});
    { auto* menu = new QMenu(sm); menu->addAction(tr("+X → −X  (right onto left)"), vp, [vp] { vp->tMirrorSel(1); }); menu->addAction(tr("−X → +X  (left onto right)"), vp, [vp] { vp->tMirrorSel(-1); });
      connect(sm, &QPushButton::clicked, this, [sm, menu] { menu->exec(sm->mapToGlobal(QPoint(0, sm->height()))); }); }
    op("top_selset", tr("Selection Set — the selection kept as a set (the mesh's settings, Selection sets)"), [vp] { vp->tSelSetCreate(); });
    op("top_shrink", tr("Shrink — the selection one ring smaller"), [vp] { vp->tGrowSel(true); });
    op("top_grow", tr("Grow — the selection one ring bigger"), [vp] { vp->tGrowSel(false); });
    op("top_invert", tr("Invert — the selection inverted (Ctrl+I)"), [vp] { vp->tInvertSel(); });
    op("top_splitpg", tr("Split2PG — the selected polygons into a new polygroup"), [vp] { vp->tSplitToGroup(); });
    op("top_hard", tr("Hard Edges — the shading broken along the selected edges"), [vp] { vp->tHardEdges(true); });
    op("top_soft", tr("Soft Edges — smooth across the selected edges again"), [vp] { vp->tHardEdges(false); });
    op("top_split", tr("Split — the selected polygons no longer welded to the rest"), [vp] { vp->tSplitSel(); });
    op("top_detach", tr("Detach — the selected polygons a separate mesh"), [vp] { vp->tDetachSel(); });
    gl->setColumnStretch(5, 1); L->addWidget(w);
  }
}

// the modelling tools' options: a change of what shapes the last edit makes it again (Loop's place, the Plane Cut,
// Bevel / Extrude, Inset, Bridge)
void ghostSettings(QVBoxLayout* v, Viewport* vp);   // (the Topology Ghost's settings — below)
QColor lobeColor(int k);                                   // (ViewportLobes.cpp: a lobe's colour)
void TopoTools::buildModelOptions(QVBoxLayout* L) {
  Viewport* vp = vp_; auto& T = vp->ttools; const QString tool = vp->tool();
  auto head = [&](const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setStyleSheet("margin-top:4px"); L->addWidget(l); };
  auto again = [vp](bool redo) { vp->saveModelPrefs(); if (redo) vp->modelReapply(); emit vp->topoState(); };
  auto slider = [&](const QString& label, double lo, double hi, double step, const double& val, std::function<QString(double)> fm, std::function<void(double)> set, bool redo, const QString& tip = {}) {
    auto* r = new SliderRow(label, lo, hi, step, std::clamp(val, lo, hi), std::move(fm)); if (!tip.isEmpty()) r->setTip(tip);
    { static const Viewport::TTools D0{}; const char* p = reinterpret_cast<const char*>(&val); const char* b = reinterpret_cast<const char*>(&vp->ttools);   // (its default: the tool's own, as it comes)
      if (p >= b && p + sizeof(double) <= b + sizeof(D0)) r->setReset(std::clamp(*reinterpret_cast<const double*>(reinterpret_cast<const char*>(&D0) + (p - b)), lo, hi)); }
    if (label == tr("Weld distance, mm")) { r->setLog(true); r->setReset(0.1); }           // (Optimize: tenths of a millimetre to centimetres — a log scale)
    connect(r, &SliderRow::changed, this, [vp, set](double x) { set(x); vp->topoChanged(); });
    connect(r, &SliderRow::released, this, [again, redo](double) { again(redo); }); L->addWidget(r); };
  auto box = [&](const QString& label, bool on, std::function<void(bool)> set, bool redo, const QString& tip = {}) {
    auto* c = check(label, on, tip); connect(c, &QCheckBox::toggled, this, [set, again, redo](bool x) { set(x); again(redo); }); L->addWidget(c); };
  auto choices = [&](std::vector<std::pair<QString, QString>> items, const QString& cur, std::function<void(const QString&)> set, bool redo, const QString& tip = {}) {
    auto* w = new QWidget; auto* gl = new QGridLayout(w); gl->setContentsMargins(0, 0, 0, 0); gl->setSpacing(3); int i = 0;
    for (auto& [key, label] : items) { auto* b = btn(label, tip); b->setCheckable(true); b->setChecked(key == cur); QString k = key;
      connect(b, &QPushButton::clicked, this, [set, k, again, redo] { set(k); again(redo); }); gl->addWidget(b, i / 2, i % 2); i++; }
    L->addWidget(w); };
  auto action = [&](const QString& label, const QString& tip, std::function<void()> fn, const QString& icon = {}) {
    auto* b = btn(label, tip); if (!icon.isEmpty()) { b->setIcon(ui::icon(icon)); b->setIconSize(QSize(18, 18)); }
    connect(b, &QPushButton::clicked, this, [fn] { fn(); }); L->addWidget(b); };
  auto px = [](double x) { return QString::number(int(std::lround(x))) + " px"; };
  auto num = [](double x) { return QString::number(x, 'g', 3); };
  auto pct = [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; };
  auto whole = [](double x) { return QString::number(int(std::lround(x))); };
  if (tool == "treveal") {
    head(tr("Reveal Brush"));
    auto* sz = new SliderRow(tr("Size, px"), 8, 800, 1, std::clamp(vp->brushR, 8.0, 800.0), [](double x) { return QString::number(int(x)); }); sz->setReset(70);
    connect(sz, &SliderRow::changed, this, [vp](double x) { vp->brushR = x; vp->requestUpdate(); }); L->addWidget(sz);
    L->addWidget(muted(tr("Paint over the Topology Ghost: its polygons under the stroke become real when you let go, welded onto what is built there; no mesh picked — a new RetopoMesh. The wheel over the geometry, Ctrl+wheel, [ / ] — the size.")));
    head(tr("Topology Ghost")); ghostSettings(L, vp);
    return;
  }
  if (tool == "tlobe") {
    auto& Lb = vp->lb_; const int m = vp->lobeTarget(); const Item* ti = m >= 0 ? vp->scene()->item(m) : nullptr;
    head(tr("Lobe Brush"));
    L->addWidget(muted(ti ? tr("Painting on: %1").arg(ti->name) : tr("Paint on the high-poly mesh — the first stroke starts Lobe 1.")));
    auto* sz = new SliderRow(tr("Size, px"), 5, 400, 1, std::clamp(vp->brushR, 5.0, 400.0), px); sz->setReset(70);
    connect(sz, &SliderRow::changed, this, [vp](double x) { vp->brushR = x; vp->requestUpdate(); }); L->addWidget(sz);
    slider(tr("Crevices"), 0, 1, 0.01, Lb.sens, pct, [vp](double x) { vp->lb_.sens = x; }, false,
           tr("Where the fill (Shift + click) stops: the seams folded sharper than 70° … 20° as it goes up; over 50% — the hollows of the surface too (a smooth sculpt)"));
    box(tr("Paint over the other lobes"), Lb.overwrite, [vp](bool x) { vp->lb_.overwrite = x; }, false,
        tr("Off — the brush, the eraser and the fill keep to the active lobe and the unpainted surface"));
    head(tr("Lobes"));
    if (ti) { const auto names = vp->lobeNames(m); const auto cnt = vp->lobeCounts(m); const int act = vp->lobeActive(m);
      for (size_t i = 0; i < names.size(); i++) { const int k = int(i) + 1;
        auto* row = new QWidget; auto* h = new QHBoxLayout(row); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(3);
        auto* sw = new QLabel; sw->setFixedSize(12, 12); sw->setStyleSheet(QString("background:%1;border:1px solid #1a1a1a;border-radius:2px").arg((k == act ? QColor(Qt::white) : lobeColor(k)).name())); h->addWidget(sw);
        auto* b = btn(QString("%1  ·  %2").arg(names[i]).arg(size_t(k) < cnt.size() ? cnt[size_t(k)] : 0), tr("The lobe painted now (Ctrl+Shift + click on it in the view) — its polygons counted"));
        b->setCheckable(true); b->setChecked(k == act); connect(b, &QPushButton::clicked, this, [vp, k] { vp->lobeSetActive(k); }); h->addWidget(b, 1);
        auto* fl = btn(QString(QChar(0x27F2)), tr("Lay the template on this lobe the next best way — when its copy came out turned or mirrored wrong")); fl->setFixedWidth(26); fl->setCheckable(true); fl->setChecked(vp->lobeFlipped(m, k)); connect(fl, &QPushButton::clicked, this, [vp, k] { vp->lobeFlip(k); }); h->addWidget(fl);
        auto* del = btn(QString(QChar(0x2715)), tr("Delete this lobe (its polygons unpainted)")); del->setFixedWidth(26); connect(del, &QPushButton::clicked, this, [vp, k] { vp->lobeDelete(k); }); h->addWidget(del);
        L->addWidget(row); }
      if (names.empty()) L->addWidget(muted(tr("No lobes yet."))); }
    action(tr("New lobe (Enter)"), tr("The active lobe done — a new one to paint (the active one white, the others in their colours)"), [vp] { vp->lobeNew(); });
    if (ti && !vp->lobeNames(m).empty()) action(tr("Clear all lobes"), tr("Every lobe of this mesh gone"), [vp] { vp->lobeClear(); });
    head(tr("Retopology"));
    const bool any = ti && !vp->lobeNames(m).empty();
    box(tr("Show it on the other lobes"), Lb.ghost, [vp](bool x) { vp->lb_.ghost = x; vp->topoChanged(); }, false,
        tr("The retopology you make on one lobe (in any tool, on the mesh you build) shown on every lobe that has none yet — cyan; ⟲ on a lobe's row turns its copy round"));
    if (any) L->addWidget(muted(vp->lobeGhostInfo()));
    { auto* go = btn(tr("Apply to the other lobes"), tr("The copies made real in the mesh you build — their points welded onto its vertices and onto each other where they meet")); go->setProperty("primary", true);
      go->setEnabled(any && !Lb.gh.empty()); connect(go, &QPushButton::clicked, this, [vp] { vp->lobeApply(); }); L->addWidget(go); }
    L->addWidget(muted(tr("LMB — paint · Ctrl — erase · Shift + click — fill up to the crevices · Ctrl+Shift + click — pick the lobe · Ctrl+RMB drag, Ctrl+wheel, [ / ] — size · Enter — a new lobe")));
    return;
  }
  if (tool == "tpatch") {
    auto& P = vp->pt_; vp->patchBuild();
    head(tr("PatchTool"));
    choices({{"p", tr("Patch")}, {"l", tr("Loft")}, {"s", tr("Strip")}}, QString(QChar(P.kind)), [vp](const QString& k) { vp->pt_.kind = k.at(0).toLatin1(); vp->patchChanged(); }, false,
            tr("Patch — the zones the curves close (with each other or the mesh's rim) filled; Loft — a surface through the curves in turn (a curve drawn between two — a section between them; the wheel over a span — its rows); Strip — each curve the middle of a band"));
    if (P.kind == 's') {
      slider(tr("Strip width, cm"), 0.01, 50, 0.01, P.stripW, [](double x) { return QString::number(x, 'g', 3); }, [vp](double x) { vp->pt_.stripW = x; vp->patchTouch(); }, false, tr("How wide the band is — the curve its middle"));
      slider(tr("Across"), 1, 16, 1, P.stripAcross, whole, [vp](double x) { vp->pt_.stripAcross = int(std::lround(x)); vp->patchTouch(); }, false, tr("Polygons across the band"));
    }
    head(tr("Preview"));
    box(tr("Show the preview"), P.show, [vp](bool x) { vp->pt_.show = x; vp->patchTouch(); }, false, tr("Off — only the curves and their anchors; the polygons they would make hidden (Create patch still makes them)"));
    slider(tr("Fill opacity"), 0, 1, 0.05, P.fillOp, [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; }, [vp](double x) { vp->pt_.fillOp = x; vp->patchTouch(); }, false,
           tr("How solid the preview's polygons are (their edges stay)"));
    if (P.kind == 'l') {
      head(tr("Loft order"));
      { auto* ob = btn(P.ordering ? tr("Done (%1 in order)").arg(P.order.size()) : tr("Set order — click the curves"), tr("Click the curves one after another — the surface goes through them in that order (a curve clicked again — it and the ones after it out). The curves left out follow by nearness"));
        ob->setCheckable(true); ob->setChecked(P.ordering); connect(ob, &QPushButton::clicked, this, [vp] { vp->pt_.ordering = !vp->pt_.ordering; if (vp->pt_.ordering) ui::hud(QObject::tr("Loft order: click the curves in turn — the button again or Esc ends it"), 2000); vp->patchChanged(); }); L->addWidget(ob); }
      if (!P.order.empty()) action(tr("Clear order"), tr("Back to the automatic order — from the curve farthest out, each next the nearest"), [vp] { vp->pt_.order.clear(); vp->patchStore(); vp->patchChanged(); });
      L->addWidget(muted(P.order.empty() ? tr("Order: automatic (by nearness)") : tr("Order: %1 curve%2 clicked").arg(P.order.size()).arg(P.order.size() == 1 ? "" : "s")));
    }
    if (P.kind == 'l') L->addWidget(muted(tr("The wheel over a span of the preview — its rows between those two curves; over a curve — that curve's own edges (curves of different edges meet in the middle row: quads, a triangle for each edge more)")));
    if (P.selC >= 0 && size_t(P.selC) < P.c.size()) {
      const auto& C = P.c[size_t(P.selC)];
      slider(tr("Edges"), 1, 64, 1, C.div, whole, [vp](double x) { auto& Q = vp->pt_; if (Q.selC >= 0 && size_t(Q.selC) < Q.c.size()) { Q.c[size_t(Q.selC)].div = int(std::lround(x)); vp->patchTouch(); } }, false,
             tr("How many edges the selected curve makes (shared among its pieces between crossings by their length)"));
      // (Equal — Match mixed by a weight, Adaptive added by one: the buttons set them at once)
      auto setW = [vp](double mw, double aw, bool rebuild = true) { auto& Q = vp->pt_; if (Q.selC < 0 || size_t(Q.selC) >= Q.c.size()) return; auto& K = Q.c[size_t(Q.selC)];
        if (mw >= 0) K.mw = mw; if (aw >= 0) K.aw = aw; K.mode = K.mw > 0 ? 'm' : K.aw > 0 ? 'a' : 'e'; K.modeAuto = false; if (rebuild) vp->patchChanged(); else vp->patchTouch(); };
      choices({{"e", tr("Equal")}, {"a", tr("Adaptive")}, {"m", tr("Match")}}, C.mw >= 1 ? "m" : C.aw >= 1 && C.mw <= 0 ? "a" : C.mw <= 0 && C.aw <= 0 ? "e" : "", [setW](const QString& k) { if (k == "e") setW(0, 0); else if (k == "a") setW(0, 1); else setW(1, -1); }, false,
              tr("Equal — the vertices evenly along it; Adaptive — denser where it bends; Match — the step of the opposite side repeated (the existing topology continued). The sliders below mix them"));
      slider(tr("Equal ↔ Match"), 0, 1, 0.05, C.mw, [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; }, [setW](double x) { setW(x, -1, false); }, false,
             tr("0 — the vertices evenly along the curve; 100% — each across from the opposite side's; between — in between"));
      slider(tr("Adaptive"), 0, 1, 0.05, C.aw, [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; }, [setW](double x) { setW(-1, x, false); }, false,
             tr("How much denser the vertices gather where the curve bends"));
      action(tr("Delete curve"), tr("The selected curve gone (Delete; Delete on a picked anchor — the anchor)"), [vp] { vp->patchDeleteCurve(); });
    } else L->addWidget(muted(tr("Pick an anchor (RMB) — the curve's edges and how they are laid.")));
    if (P.kind == 'p') {
    head(tr("Patch"));
    box(tr("Detail to the denser side"), P.toDense, [vp](bool x) { vp->pt_.toDense = x; vp->patchChanged(); }, false,
        tr("Unequal opposite sides: the rows change in the first strip by the denser side (triangles there) — the rest a plain grid of the smaller count"));
    L->addWidget(muted(tr("Three-sided patch:")));
    choices({{"q", tr("Three quads")}, {"f", tr("Converging")}}, P.triFan ? "f" : "q", [vp](const QString& k) { vp->pt_.triFan = k == "f"; vp->patchChanged(); }, false,
            tr("Three quads — a middle point, a four-sided grid round each corner; Converging — rows from the side of the most edges narrowing to the opposite corner"));
    if (!P.c.empty()) action(tr("Switch zone"), tr("Where the curves close two zones with the mesh's rim (or with each other) — the other one filled"), [vp] { vp->pt_.zoneAlt = !vp->pt_.zoneAlt; vp->patchChanged(); });
    L->addWidget(muted(tr("The mesh's side of the patch:")));
    choices({{"rim", tr("Along the rim")}, {"short", tr("Shortest edges")}}, P.rimShort ? "short" : "rim", [vp](const QString& k) { vp->pt_.rimShort = k == "short"; vp->patchChanged(); }, false,
            tr("Along the rim — the patch meets the mesh's open rim, every turn of it; Shortest edges — the shortest way along the mesh's edges between where the curves meet the rim: a tongue sticking out of the rim cut off at its root"));
    }
    if (!P.c.empty()) action(tr("Update"), tr("The zones found anew — when they weren't found, or a mode picked by hand (Switch zone, a curve's Equal / Match) got in the way: those back to automatic"), [vp] { vp->patchRedetect(); });
    { auto* go = btn(tr("Create patch (Enter)"), tr("The previewed polygons made, welded onto the mesh's rim (no mesh picked — a new RetopoMesh)")); go->setProperty("primary", true); go->setEnabled(!P.F.empty());
      connect(go, &QPushButton::clicked, this, [vp] { vp->patchCommit(); }); L->addWidget(go); }
    if (!P.c.empty()) action(tr("Delete all curves (%1)").arg(P.c.size()), tr("Every curve of the PatchTool gone"), [vp] { vp->patchDeleteAll(); });
    L->addWidget(muted(P.F.empty() ? (P.note.isEmpty() ? tr("Draw curves on the references.") : P.note)
                                   : tr("Preview: %1 patch%2, %3 polygons").arg(P.patches).arg(P.patches == 1 ? "" : "es").arg(P.F.size())));
    L->addWidget(muted(tr("LMB — anchors (double click / Enter ends the curve; Backspace — the last one back) · Shift + drag along the mesh's open edges — an edge curve on them, divided at each of their vertices · RMB — pick / drag an anchor, on a curve — a new one · Ctrl+RMB — delete an anchor · an end onto another curve's end — join or keep a corner · Esc — stop / deselect")));
    return;
  }
  if (tool == "tpen") {
    head(tr("PolyPen"));
    slider(tr("Auto weld, px"), 2, 60, 1, T.penWeld, px, [vp](double x) { vp->ttools.penWeld = x; }, false, tr("A vertex let go this near another one is welded to it (the blue circle at the pointer)"));
    slider(tr("Fill radius, px"), 10, 400, 1, T.penFill, px, [vp](double x) { vp->ttools.penFill = x; }, false, tr("Shift+RMB fills between the points within it ([ / ] too — the orange circle)"));
    choices({{"relax", tr("Relax")}, {"smooth", tr("Smooth")}, {"inflate", tr("Inflate")}, {"pinch", tr("Pinch")}}, T.penBrush, [vp](const QString& k) { vp->ttools.penBrush = k; }, false,
            tr("What RMB held does under the circle (Ctrl+wheel switches): Relax — the polygons evened out along the surface; Smooth — towards the neighbours' middle; Inflate — pushed out along the normal; Pinch — pulled towards the centre"));
    slider(tr("Brush radius, px"), 8, 800, 1, T.penSmooth, px, [vp](double x) { vp->ttools.penSmooth = x; }, false, tr("RMB held: the brush's circle (the wheel over the geometry — the camera does not move then)"));
    if (vp->penAnchorCount()) action(tr("Delete anchors (%1)").arg(vp->penAnchorCount()), tr("Every anchor point of the PolyPen gone"), [vp] { ui::toast(vp->penDeleteAnchors()); }, "tt_delanchor");
    L->addWidget(muted(tr("Click — a corner (the first again or a double click closes; Esc drops) · drag — move (onto a vertex: weld) · Ctrl + drag an edge / a polygon — pull out / extrude · Ctrl + click — delete · Shift + click — an anchor · RMB held — the brush (wheel over the mesh: radius · Ctrl+wheel: its kind) · Shift+RMB — fill · Ctrl+Shift — Loop")));
  } else if (tool == "tloop") {
    head(tr("Loop"));
    slider(tr("Cuts"), 1, 32, 1, T.loopCuts, whole, [vp](double x) { vp->ttools.loopCuts = int(std::lround(x)); }, true, tr("How many loops at once (the wheel over an edge too)"));
    slider(tr("Position"), 0.01, 0.99, 0.01, T.loopPct, pct, [vp](double x) { vp->ttools.loopPct = x; }, true, tr("Where along the edge the loop just made lies — it moves there"));
    choices({{"0", tr("Proportional")}, {"1", tr("Parallel")}}, T.loopParallel ? "1" : "0", [vp](const QString& k) { vp->ttools.loopParallel = k == "1"; }, true,
            tr("Proportional — at the same share of every edge (an averaged shape); Parallel — at the same distance from the nearer side"));
    L->addWidget(muted(tr("Wheel — cuts · Shift — onto the 1/2, 1/4, 1/6 … marks · Ctrl — parallel to the nearer side · LMB — cut · RMB — cut and select")));
  } else if (tool == "tknife") {
    head(tr("Knife"));
    slider(tr("Weld radius, px"), 0, 40, 1, T.knifeWeld, px, [vp](double x) { vp->ttools.knifeWeld = x; }, false, tr("A cut this near a corner goes through the corner — no thin triangles there (the red circle at the pointer)"));
    box(tr("Cut through"), T.knifeThrough, [vp](bool x) { vp->ttools.knifeThrough = x; }, false, tr("The far side cut too — a straight cut makes a ring"));
    action(tr("✂ Cut"), tr("Cut along the curves (Enter)"), [vp] { vp->knifeCut(); });
    L->addWidget(muted(tr("Clicks on the mesh — a smooth curve (Esc ends it; a click on its first anchor closes it), laid on the surface — turn the view to draw round a tube · from empty space a drag — a straight cut · Ctrl + drag — an anchor · Shift+RMB — cut, optimise, recompute, delete")));
  } else if (tool == "tchain") {
    head(tr("Smooth EdgeChain"));
    if (!vp->chainActive()) action(tr("Curve from the selected edges"), tr("The selected chain of edges follows a curve from now on (Shift+RMB too)"), [vp] { vp->chainCreate(); });
    else {
      slider(tr("Smooth"), 0, 1, 0.01, T.chainSmooth, pct, [vp](double x) { vp->ttools.chainSmooth = x; vp->chainApply(false); }, false, tr("The curve smoothed — the edges with it"));
      connect(findChildren<SliderRow*>().last(), &SliderRow::released, this, [vp](double) { vp->chainApply(true); });
      box(tr("Even the edges"), T.chainEqual, [vp](bool x) { vp->ttools.chainEqual = x; vp->chainApply(true); }, false, tr("The vertices spaced evenly along the curve"));
      action(tr("Delete the curve"), tr("The chain stays where it is"), [vp] { vp->topoToolKey(Qt::Key_Escape, {}); });
    }
    L->addWidget(muted(tr("Select edges as with the Select Cursor · Shift+RMB — the curve, smooth, optimise, recompute the handles, delete · drag its anchors · a click on it — a new anchor")));
  } else if (tool == "tweld") {
    head(tr("Weld"));
    choices({{"point", tr("To a point")}, {"pair", tr("In pairs")}}, T.weldMode, [vp](const QString& k) { vp->ttools.weldMode = k; }, false,
            tr("To a point — the selected vertices onto the vertex under the pointer (else their middle); In pairs — two chains of edges, vertex by vertex"));
    if (T.weldMode == "pair")
      choices({{"mid", tr("Middle")}, {"side", tr("Onto a side")}}, T.weldPos, [vp](const QString& k) { vp->ttools.weldPos = k; }, false,
              tr("Middle — each pair meets halfway; Onto a side — RMB on a polygon of the side to keep: the other is welded onto it (the button: the side picked last stays)"));
    action(tr("Weld"), tr("Weld the selection (RMB in the viewport — at the pointer)"), [vp] { vp->weldNow(nullptr); });
    L->addWidget(muted(tr("Select as with the Select Cursor, then RMB — weld (to a point: onto the vertex under the pointer)")));
  } else if (tool == "tplane") {
    head(tr("Plane Cut"));
    choices({{"all", tr("Cut all")}, {"split", tr("Split")}, {"removeA", tr("Remove part A")}, {"removeB", tr("Remove part B")}}, T.planeMode, [vp](const QString& k) { vp->ttools.planeMode = k; }, true,
            tr("Cut all — only cut; Split — the two sides apart; Remove part A / B — one side deleted"));
    slider(tr("Number of cuts"), 1, 50, 1, T.planeCuts, whole, [vp](double x) { vp->ttools.planeCuts = int(std::lround(x)); }, true);
    slider(tr("Spacing"), 0.01, 50, 0.01, T.planeSpacing, num, [vp](double x) { vp->ttools.planeSpacing = x; }, true, tr("Between the cuts (scene units)"));
    box(tr("Restrict to selection"), T.planeSel, [vp](bool x) { vp->ttools.planeSel = x; }, true);
    box(tr("Select cuts"), T.planeSelCuts, [vp](bool x) { vp->ttools.planeSelCuts = x; }, true);
    L->addWidget(muted(tr("Drag a line across the mesh — the plane through it, edge-on to the view")));
  } else if (tool == "tbevel") {
    head(tr("Bevel / Extrude"));
    slider(tr("Offset"), -50, 50, 0.01, T.bevOffset, num, [vp](double x) { vp->ttools.bevOffset = x; }, true, tr("How far the polygons go out — below 0: in (a pocket); drag sideways in the viewport too"));
    slider(tr("Bevel"), 0, 20, 0.01, T.bevBevel, num, [vp](double x) { vp->ttools.bevBevel = x; }, true, tr("The top's rim rounded off by this much"));
    slider(tr("Subdivision"), 1, 16, 1, T.bevSeg, whole, [vp](double x) { vp->ttools.bevSeg = int(std::lround(x)); }, true);
    head(tr("Caps"));
    box(tr("Top cap"), T.bevTop, [vp](bool x) { vp->ttools.bevTop = x; }, true, tr("The moved polygons kept (off: an open tube)"));
    box(tr("Bottom cap"), T.bevBottom, [vp](bool x) { vp->ttools.bevBottom = x; }, true, tr("The old place closed too (turned round)"));
    head(tr("Mode"));
    box(tr("Smart"), T.bevSmart, [vp](bool x) { vp->ttools.bevSmart = x; }, true, tr("Where the polygons border other polygons: no walls there — their rim stays and is stitched to them, the rest pushed out (a slope from the neighbours, no inner walls)"));
    box(tr("Proportional (edges)"), T.bevProp, [vp](bool x) { vp->ttools.bevProp = x; }, true, tr("Bevel on edges: each vertex slides the same share of its side edges — a narrow place gets a narrow chamfer, a wide one a wide one (off: the same width everywhere)"));
    head(tr("Select after"));
    box(tr("Top"), T.bevSelTop, [vp](bool x) { vp->ttools.bevSelTop = x; }, true);
    box(tr("Bevel (sides)"), T.bevSelSides, [vp](bool x) { vp->ttools.bevSelSides = x; }, true);
    box(tr("Bottom"), T.bevSelBottom, [vp](bool x) { vp->ttools.bevSelBottom = x; }, true);
    L->addWidget(muted(tr("Select polygons, then drag sideways in the viewport (a click — with the offset here). Shift + drag — the polygons only pulled out, no new ones")));
  } else if (tool == "tinset") {
    head(tr("Inset"));
    slider(tr("Offset"), 0, 20, 0.01, T.insOffset, num, [vp](double x) { vp->ttools.insOffset = x; }, true, tr("How far the rim goes in (drag sideways in the viewport too)"));
    slider(tr("Depth"), -20, 20, 0.01, T.insDepth, num, [vp](double x) { vp->ttools.insDepth = x; }, true, tr("The inset part pushed out (+) or in (−)"));
    slider(tr("Subdivision"), 1, 16, 1, T.insSeg, whole, [vp](double x) { vp->ttools.insSeg = int(std::lround(x)); }, true);
    box(tr("Preserve groups"), T.insGroups, [vp](bool x) { vp->ttools.insGroups = x; }, true, tr("On — the selection inset as a whole; off — each polygon on its own"));
    box(tr("Scale the inside"), T.insScale, [vp](bool x) { vp->ttools.insScale = x; }, true, tr("On — the polygons inside the region drawn in with its rim (scaled about its middle); off — only the rim moves in"));
    head(tr("Circles (vertices)"));
    slider(tr("Radius"), 0.01, 20, 0.01, T.insCircleR, num, [vp](double x) { vp->ttools.insCircleR = x; }, true);
    slider(tr("Sides"), 3, 64, 1, T.insCircleN, whole, [vp](double x) { vp->ttools.insCircleN = int(std::lround(x)); }, true);
    box(tr("A hole"), T.insCircleHole, [vp](bool x) { vp->ttools.insCircleHole = x; }, true, tr("The circle left open"));
    action(tr("Circles round the selected vertices"), tr("A circle of so many sides round each selected vertex (a click in the viewport in the Vertex mode too)"), [vp] { vp->insetCircles(); });
  } else if (tool == "tbridge") {
    head(tr("Bridge"));
    slider(tr("Subdivision"), 0, 32, 1, T.brSeg, whole, [vp](double x) { vp->ttools.brSeg = int(std::lround(x)); }, true);
    slider(tr("Tension"), -2, 2, 0.01, T.brTension, pct, [vp](double x) { vp->ttools.brTension = x; }, true, tr("The bridge bulged out of its two ends' polygons"));
    slider(tr("Spin steps"), -32, 32, 1, T.brSpin, whole, [vp](double x) { vp->ttools.brSpin = int(std::lround(x)); }, true, tr("A loop's pairs turned by so many steps"));
    box(tr("Reverse"), T.brReverse, [vp](bool x) { vp->ttools.brReverse = x; }, true);
    box(tr("Keep original"), T.brKeep, [vp](bool x) { vp->ttools.brKeep = x; }, true, tr("Bridging two groups of polygons: they stay"));
    action(tr("Bridge"), tr("Polygons between the two selected chains"), [vp] { vp->bridgeNow(); });
    L->addWidget(muted(tr("Selects as the Select Cursor (click, Shift, Ctrl, drag). A drag from free space — the bridge; the wheel — its subdivision; RMB held ←→ — its bulge. Chains of unlike counts: triangles where they differ.")));
  } else if (tool == "tshaper") {
    head(tr("Edge Shaper"));
    L->addWidget(muted(tr("Select edges (a chain) as with the Select Cursor, then drag → in empty space or from the selection itself (RMB — anywhere):\nLMB — even spacing, the shape kept\nRMB — Edge Flow (the edges across carry on smoothly)\nShift+LMB — flat on a plane (the chain's ends on it; three vertices picked — the plane through them)\nShift+RMB — straight")));
    L->addWidget(muted(tr("Polygons selected (or edges / vertices covering them): their inside shaped, their rim kept as it is —\nLMB — a smooth surface spanned by the rim\nRMB — smooth lines: every line of edges across it a smooth curve\nShift — flat on the plane the rim lies nearest, the grid evened out")));
  } else if (tool == "tslide") {
    head(tr("Slide"));
    L->addWidget(muted(tr("Select vertices / edges as with the Select Cursor, then drag in empty space or from the selection itself (RMB — anywhere): LMB — proportionally (the same share of each edge), RMB — in parallel (the same distance)")));
  } else if (tool == "tnormals") {
    head(tr("Normals"));
    box(tr("Show the normals"), T.normShow, [vp](bool x) { vp->ttools.normShow = x; vp->saveModelPrefs(); vp->topoChanged(); }, false, tr("The normals' lines in the viewport — off: none drawn (the tool works the same)"));
    slider(tr("Shown length"), 0.01, 5, 0.01, T.normLen, num, [vp](double x) { vp->ttools.normLen = x; vp->saveModelPrefs(); vp->topoChanged(); }, false, tr("The lines' length: 1 — a 20th of the mesh's size"));
    slider(tr("Opacity"), 0.05, 1, 0.01, T.normOp, pct, [vp](double x) { vp->ttools.normOp = x; vp->saveModelPrefs(); vp->topoChanged(); }, false, tr("How visible the normals' lines are"));
    head(tr("Average normals"));
    choices({{"shape", tr("Smoothed shape")}, {"sphere", tr("Sphere")}, {"face", tr("Face")}}, T.normMode, [vp](const QString& k) { const bool swap = (k == "face") != (vp->ttools.normMode == "face"); vp->ttools.normMode = k;
              if (swap) QTimer::singleShot(0, vp, [vp] { emit vp->topoState(); }); }, true,   // (Face: the Nose slider in place of Smoothness — the options made again)
            tr("Smoothed shape — the normals of the selection's surface smoothed over (the small forms melt away); Sphere — from its middle outward; "
               "Face — as game faces have them: one smooth shape over the cheeks and forehead, the nose still read (select the face's polygons, it looks along +Z or where they face)"));
    if (T.normMode == "face")
      slider(tr("Nose"), 0, 2, 0.01, T.normNose, pct, [vp](double x) { vp->ttools.normNose = x; }, true, tr("How much of a game face's shading: 0 — the face one smooth shape (an ellipsoid), 100% — as on game faces (flat cheeks, the nose's shadow), over 100% — stronger still"));
    else
      slider(tr("Smoothness"), 0, 1, 0.01, T.normSmooth, pct, [vp](double x) { vp->ttools.normSmooth = x; }, true, tr("How far the shape is smoothed before its normals are taken"));
    box(tr("Hard border"), T.normHardBorder, [vp](bool x) { vp->ttools.normHardBorder = x; }, true, tr("The selection's border a hard edge — the shading breaks there"));
    action(tr("Average the normals"), tr("Only the shading: the selected polygons' normals averaged as those of a simpler shape — no soft / hard edges or smoothing groups are set, the geometry stays (the settings above change the last one while it is selected)"), [vp] { vp->tNormalsSmooth(); });
    head(tr("Normals brush"));
    box(tr("Paint the normals"), T.nBrush, [vp](bool x) { vp->ttools.nBrush = x; vp->topoChanged(); QTimer::singleShot(0, vp, [vp] { emit vp->topoState(); }); }, false,
        tr("A brush in the viewport: the normals under it given the averaging's share set below (the mode and Smoothness above; the selected polygons' shape, none selected — the whole mesh's)"));
    slider(tr("Averaged"), 0, 1, 0.01, T.nBrushVal, pct, [vp](double x) { vp->ttools.nBrushVal = x; vp->saveModelPrefs(); }, false,
           tr("The share the brush gives: 100% — fully averaged, 0% — the custom normals wiped, the standard shading back"));
    if (T.nBrush) L->addWidget(muted(tr("LMB — this share; Ctrl+LMB (or 0%) — the custom normals wiped, the standard shading; Shift+LMB — the change between normals smoothed. The wheel over the mesh or Ctrl+RMB drag — its size.")));
    head(tr("Blend edges"));
    action(tr("Edges → the back wall's normals"), tr("The selected edges (Edge mode): their vertices take the normal of the wall behind them — the polygons turned away from the camera (orange in two-tone); the edge's band then shades as that wall and the border can't be seen. Turn the view so that wall faces away first"), [vp] { vp->tNormalsFromBack(); });
    head(tr("Turn / copy"));
    head(tr("Set the direction"));
    { auto* w = new QWidget; auto* gl = new QGridLayout(w); gl->setContentsMargins(0, 0, 0, 0); gl->setSpacing(3); int i = 0;
      for (const char* d : {"+x", "-x", "+y", "-y", "+z", "-z"}) { const QString k = d; auto* b = btn(k.toUpper(), tr("The selected vertices' normals along this world axis"));
        connect(b, &QPushButton::clicked, this, [vp, k] { vp->tNormalsSet(k); }); gl->addWidget(b, i / 3, i % 3); i++; }
      L->addWidget(w); }
    action(tr("Toward the camera"), tr("The selected vertices' normals pointed at the viewer — the direction you look from"), [vp] { vp->tNormalsSet("cam"); });
    action(tr("One common normal"), tr("The selected vertices all take their average normal — the area shades as one flat plane"), [vp] { vp->tNormalsSet("one"); });
    action(tr("Reset to standard smoothing"), tr("The selected vertices' normals (nothing selected — the whole mesh's) back to the standard smooth shading: set by hand, averaged, turned or copied — all undone; the hard edges stay"), [vp] { vp->tNormalsSet("reset"); });
    action(tr("Turn the normals…"), tr("The selected vertices / polygons: the next drag in the viewport turns their normals (←→ round the view's up, ↑↓ round its right)"), [vp] { vp->armOnce("nturn"); });
    action(tr("Copy from a mesh…"), tr("The selected polygons (none — all) take the normals of the mesh clicked next in the viewport"), [vp] { vp->armOnce("ncopy"); });
    L->addWidget(muted(tr("In the viewport the tool selects as the Select Cursor; its actions are these buttons.")));
  } else if (tool == "toptimize") {
    head(tr("Optimize"));
    slider(tr("Weld distance, mm"), 0.001, 50, 0.001, T.optWeld * 10, [](double x) { return QString::number(x, 'g', 3); }, [vp](double x) { vp->ttools.optWeld = x / 10; vp->topoChanged(); }, false,
           tr("Vertices nearer than this are welded into one"));
    action(tr("Pick the distance…"), tr("The next drag in the viewport sets the weld distance — a circle, → wider"), [vp] { vp->armOnce("optpick"); });
    choices({{"safe", tr("Safe")}, {"merge", tr("Merge")}}, T.optMode, [vp](const QString& k) { vp->ttools.optMode = k; }, false,
            tr("Safe — no points welded where that would make edges of three polygons or more (non-manifold); Merge — every weld within the distance, and the inner walls it leaves (two faces meeting back to back, e.g. two hair locks sewn together) taken out — the parts one shell"));
    box(tr("Also the points in the middle of a side"), T.optMidSide, [vp](bool x) { vp->ttools.optMidSide = x; }, false, tr("A point between two edges of a straight side (two polygons round it) taken out too — off: the clean-up only"));
    action(tr("Optimize"), tr("The clean-up: the points in one place (within the distance) welded, the unused points taken out, the polygons of one or two points taken out, the doubled polygons made one — the selection, else all"), [vp] { vp->tOptimize(); });
    L->addWidget(muted(tr("In the viewport the tool selects as the Select Cursor; its actions are these buttons.")));
  } else if (tool == "tfixmesh") {
    head(tr("FixMesh"));
    L->addWidget(muted(tr("Fix mesh does these steps on the active mesh (collapsed polygons — slivers, no area — always go):")));
    box(tr("Weld points in one place"), T.fixWeld, [vp](bool x) { vp->ttools.fixWeld = x; }, false, tr("Vertices lying at the very same place made one (a file's seams left open)"));
    box(tr("Remove double walls"), T.fixDoubles, [vp](bool x) { vp->ttools.fixDoubles = x; }, false, tr("Polygons lying one on another — the same corners' places, shared vertices or not — one kept"));
    box(tr("Fix non-manifold"), T.fixManifold, [vp](bool x) { vp->ttools.fixManifold = x; }, false, tr("A side of three polygons or more split into pairs that carry on into each other (the others get their own copy of it); a vertex where parts only touch split"));
    box(tr("Turn each part one way"), T.fixOrient, [vp](bool x) { vp->ttools.fixOrient = x; }, false, tr("Each part's polygons turned the way most of them already face"));
    box(tr("Separate parts as meshes"), T.fixSplit, [vp](bool x) { vp->ttools.fixSplit = x; }, false,
        tr("Each part of the mesh not joined to the rest by a shared side becomes a mesh of its own (the largest stays this one) — e.g. the upper and the lower teeth"));
    action(tr("Make manifold (selected meshes)"), tr("Every selected mesh made manifold, as the Hair room's conversion leaves it: an edge of three polygons or more split into pairs that carry on into each other, a vertex where parts only touch split, doubled polygons out — nothing else changes"), [vp] {
      const std::vector<int> ms = vp->selection().items; vp->runEdit([ms](Scene& s, Selection&, const CurveCache&) { int fixed = 0;
        for (int m : ms) { Item* x = s.item(m); if (!x || !x->poly || (x->type != ItemType::IMesh && x->type != ItemType::Mesh)) continue; auto src = x->base ? x->base : x->poly; auto p = std::make_shared<PolyMesh>(*src); if (!tedit::makeManifold(*p)) continue;
          const bool sameShown = x->poly == src; if (x->base) x->base = p; if (sameShown || !x->base) x->poly = p; fixed++; }
        return fixed ? QObject::tr("%1 mesh%2 made manifold").arg(fixed).arg(fixed == 1 ? "" : "es") : QObject::tr("Already manifold — nothing to clean"); }); });
    action(tr("Fix mesh"), tr("The active mesh: collapsed polygons (no area, slivers) and doubled ones out, parts that only touch at a vertex or share a side with a third polygon cut apart, each part turned the way most of its polygons face"), [vp] { vp->tFixMesh(); });
  } else if (tool == "tmirror") {
    head(tr("Mirror"));
    choices({{"world", tr("World")}, {"local", tr("Local (object's gizmo)")}}, T.mirLocal ? "local" : "world", [vp](const QString& k) { vp->ttools.mirLocal = k == "local"; }, true,
            tr("World — the world's axes through the origin; Local — the object's own gizmo: its axes and centre (set them with Align of the Move tool, or Alt + drag)"));
    choices({{"0", "X"}, {"1", "Y"}, {"2", "Z"}}, QString::number(T.mirAxis), [vp](const QString& k) { vp->ttools.mirAxis = k.toInt(); }, false, tr("The axis — the plane across it through the origin (Local: through the object's gizmo), or the line round which the copies turn"));
    choices({{"mirror", tr("Mirror")}, {"radial", tr("Radial")}, {"radialMirror", tr("Radial mirror")}, {"radialHalf", tr("Radial half")}}, T.mirType, [vp](const QString& k) { vp->ttools.mirType = k; }, false,
            tr("Mirror — by the axis; Radial — copies round it; Radial mirror — each copy and its mirror; Radial half — round half of it, as half a flower"));
    slider(tr("Copies (radial)"), 2, 64, 1, T.mirCount, whole, [vp](double x) { vp->ttools.mirCount = int(std::lround(x)); }, false);
    box(tr("Mirror normals"), T.mirNormals, [vp](bool x) { vp->ttools.mirNormals = x; }, false,
        tr("The copy takes the mesh's own normals, mirrored (turned, for Radial) — off: the copy's normals made anew from its shape"));
    box(tr("Replace the other side"), T.mirReplace, [vp](bool x) { vp->ttools.mirReplace = x; }, false,
        tr("Mirror: the selected polygons take the place of the other side's matching region (found by the selection's border mirrored onto the mesh) — e.g. the left hand remodelled with claws, then the right one replaced by its copy; UVs, seams, creases, hard edges carried"));
    action(tr("Mirror"), tr("The selected polygons (none — the whole mesh) copied; the side found by itself, the seam welded"), [vp] { vp->tMirrorMesh(); });
    action(tr("Virtual symmetry group…"), tr("The selected meshes into a group whose symmetry is only shown — in any place; Apply makes it real"), [this] { vsymMenu(); }, "tt_vsymgroup");
  } else if (tool == "tfill") {
    head(tr("Fill Hole"));
    choices({{"quads", tr("Quads")}, {"quadtri", tr("Quads + tris")}, {"fan", tr("Tris to a centre")}, {"tris", tr("Tris, no centre")}, {"ngon", tr("N-gon")}}, T.fillMode,
            [vp](const QString& k) { vp->ttools.fillMode = k; }, true,
            tr("Quads — a grid over the hole (an odd hole: one triangle); Quads + tris — rows from both ends; Tris to a centre — a fan to a new middle vertex; Tris, no centre — a zigzag; N-gon — one polygon"));
    slider(tr("Subdivision"), 0, 16, 1, T.fillSub, whole, [vp](double x) { vp->ttools.fillSub = int(std::lround(x)); }, true,
           tr("Loops inside the hole before it is filled — rows of quads from its rim inward (e.g. a sphere closed with triangles to a centre: rings round the centre)"));
    slider(tr("Edge Flow"), 0, 1, 0.01, T.fillFlow, pct, [vp](double x) { vp->ttools.fillFlow = x; }, true,
           tr("0 — a flat patch; 1 — the patch carries on the shape round the hole (its new vertices bent with the surface)"));
    action(tr("Fill"), tr("Fill the hole shown (LMB in the viewport)"), [vp] { vp->fillApply(); });
    L->addWidget(muted(tr("After a fill the patch stays selected: Subdivision, Edge Flow and the kind change it — until the selection changes. Shift + RMB drag ←→ — Edge Flow, Ctrl + RMB drag ←→ — Subdivision · the wheel — the kind")));
    L->addWidget(muted(tr("The hole nearest the pointer is shown filled (or every hole the picked edges or vertices touch) · LMB — fill · RMB — the next variant (turned round)")));
  }
}

void TopoTools::runAct(const QString& a) {
  const bool shift = QApplication::keyboardModifiers() & Qt::ShiftModifier;
  if (a == "ngons") vp_->tSelectNgons(shift);
  else if (a == "collapse") vp_->tCollapseEdges();
  else if (a == "relax") vp_->tRelaxMesh();
  else if (a == "snapgeom") vp_->tSnapToGeom();
  else if (a == "alignN") vp_->tAlignNormals();
  else if (a == "reverseN") vp_->tReverseNormals();
  else if (a == "vsym") vsymMenu();
  else if (a == "identical") vp_->tApplyIdentical();
}

void TopoTools::vsymMenu() {
  Viewport* vp = vp_; int g = -1; for (int id : vp->selection().items) if (int x = vp->vsymGroupOf(id); x >= 0) g = x;
  struct K { const char *k, *icon, *label, *tip; };
  const K ks[] = {{"mirror", "sym_mirror", "Mirror", "By the axis: a mirrored copy"}, {"radial", "sym_radial", "Radial", "Round the axis: copies turned evenly (the count in the Mirror tool's options)"},
                  {"radialMirror", "sym_radialmirror", "Radial mirror", "Round the axis, each copy and its mirror — a kaleidoscope"}, {"radialHalf", "sym_radialhalf", "Radial half", "Round half the axis — half a flower"}};
  ui::PieSpec s; s.bare = true;
  for (const K& k : ks) { ui::PieItem it; it.icon = k.icon; it.label = tr(k.label); it.tip = tr(k.tip); QString key = k.k;
    if (g < 0) it.act = [vp, key] { vp->vsymCreate(key); };
    else { it.cur = vp->scene()->item(g)->raw.value("vsym").toObject().value("type").toString() == key;
      it.act = [vp, g, key] { vp->runEdit([g, key](Scene& sc, Selection&, const CurveCache&) { QJsonObject o = sc.item(g)->raw.value("vsym").toObject(); o.insert("type", key); sc.item(g)->raw.insert("vsym", o); return QObject::tr("Symmetry: %1").arg(key); }); }; }
    s.ring.push_back(it); }
  if (g >= 0) {
    ui::PieItem ap; ap.label = tr("✔ Apply — make it real"); ap.tip = tr("The copies become the meshes' own geometry (the seams welded); the group stays, its symmetry gone"); ap.act = [vp, g] { vp->vsymApply(g); };
    ui::PieItem rm; rm.label = tr("✕ Remove the symmetry"); rm.tip = tr("Only the original kept — the preview gone"); rm.act = [vp, g] { vp->runEdit([g](Scene& sc, Selection&, const CurveCache&) { sc.item(g)->raw.remove("vsym"); return QObject::tr("Virtual symmetry removed"); }); };
    s.top = {ap}; s.bottom = {rm};
  } else s.title = tr("A group with a virtual symmetry");
  ui::Pie::open(parentWidget(), QCursor::pos(), s, false);
}

void TopoTools::save() const {
  QPoint p = pos() - parentWidget()->mapToGlobal(QPoint(0, 0));
  QJsonArray sp; if (split_) for (int s : split_->sizes()) sp.append(s);
  QSettings().setValue("topo/tools", QJsonDocument(QJsonObject{{"x", p.x()}, {"y", p.y()}, {"w", width()}, {"h", height()}, {"split", sp}}).toJson(QJsonDocument::Compact));
}
void TopoTools::place(QWidget* vpBox) {
  QJsonObject st = QJsonDocument::fromJson(QSettings().value("topo/tools").toByteArray()).object();
  QWidget* w = parentWidget(); QPoint o = w->mapToGlobal(QPoint(0, 0));
  QPoint p = st.contains("x") ? o + QPoint(st.value("x").toInt(), st.value("y").toInt()) : vpBox->mapToGlobal(QPoint(10, 34));
  QRect g(o, w->size()); p.setX(std::clamp(p.x(), g.left(), g.right() - 60)); p.setY(std::clamp(p.y(), g.top(), g.bottom() - 40));
  move(p);
}
bool TopoTools::eventFilter(QObject* o, QEvent* e) {
  if (o != bar_ && o != bar2_) return QFrame::eventFilter(o, e);
  if (e->type() == QEvent::MouseButtonPress) { auto* m = static_cast<QMouseEvent*>(e); if (m->button() == Qt::LeftButton) { drag_ = true; grab_ = m->globalPosition().toPoint() - pos(); } return true; }
  if (e->type() == QEvent::MouseMove && drag_) { move(static_cast<QMouseEvent*>(e)->globalPosition().toPoint() - grab_); return true; }
  if (e->type() == QEvent::MouseButtonRelease && drag_) { drag_ = false; save(); return true; }
  if (e->type() == QEvent::MouseButtonDblClick) { vert_ = !vert_; if (auto* l = qobject_cast<QBoxLayout*>(row_->layout())) l->setDirection(vert_ ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight); adjustSize(); save(); return true; }
  return QFrame::eventFilter(o, e);
}

// ---------- 👁 Display in the room ----------
QFrame* makeTopoDisplay(QWidget* window, Viewport* vp, Scene* sc, std::function<QJsonObject()> view, std::function<void(const QString&, const QJsonValue&)> setView) {
  auto* f = new QFrame(window, Qt::Popup); f->setObjectName("dispTopo");
  f->setStyleSheet("#dispTopo{background:#262626;border:1px solid #555;border-radius:6px}");
  auto* h = new QHBoxLayout(f); h->setContentsMargins(10, 5, 10, 5); h->setSpacing(6);
  auto mk = [&](const QString& name, const QString& label, const QString& tip) { auto* b = btn(label, tip); b->setObjectName(name); b->setCheckable(true); h->addWidget(b); return b; };
  auto sep = [&] { auto* s = new QFrame; s->setProperty("sep", true); s->setFixedSize(1, 22); h->addWidget(s); };
  auto allMeshes = [sc] { std::vector<int> o; for (auto& [id, it] : sc->items) if (it.type == ItemType::IMesh) o.push_back(id); return o; };
  auto* w = mk("wire", QObject::tr("⬢ Wire"), QObject::tr("Show / hide the meshes' wire (each mesh's own Wire switch — in its settings; none on a high-poly mesh)"));
  QObject::connect(w, &QPushButton::clicked, f, [=](bool on) { setView("showMeshWire", on); syncTopoDisplay(f); });
  sep();
  auto shade = [=](bool smooth) { vp->runEdit([allMeshes, smooth](Scene& s, Selection&, const CurveCache&) { for (int id : allMeshes()) s.item(id)->raw.insert("smoothN", smooth); return QString(); }, false);
    vp->tdisp.unlit = false; vp->saveTopoPrefs(); vp->topoDisplayChanged(); syncTopoDisplay(f); };
  QObject::connect(mk("smooth", QObject::tr("◉ Smooth"), QObject::tr("Every mesh smooth (normals averaged at the vertices), lit")), &QPushButton::clicked, f, [=] { shade(true); });
  QObject::connect(mk("flat", QObject::tr("◆ Flat"), QObject::tr("Every mesh flat — each polygon shaded on its own, lit")), &QPushButton::clicked, f, [=] { shade(false); });
  QObject::connect(mk("unlit", QObject::tr("○ Unlit"), QObject::tr("No lighting: the colours (and textures) as they are, no shading")), &QPushButton::clicked, f, [=] { vp->tdisp.unlit = !vp->tdisp.unlit; vp->saveTopoPrefs(); vp->topoDisplayChanged(); syncTopoDisplay(f); });
  QObject::connect(mk("notex", QObject::tr("▦ No textures"), QObject::tr("Ignore the textures — the meshes in their plain colour / material")), &QPushButton::clicked, f, [=] { vp->tdisp.noTex = !vp->tdisp.noTex; vp->saveTopoPrefs(); vp->topoDisplayChanged(); syncTopoDisplay(f); });
  sep();
  QObject::connect(mk("selThrough", QObject::tr("⬚ Select through"), QObject::tr("Rectangle, Lasso and the Select Cursor's brush pick the hidden side too — everything inside, not only what is seen")), &QPushButton::clicked, f,
                   [=] { vp->tset.selThrough = !vp->tset.selThrough; vp->saveTopoPrefs(); syncTopoDisplay(f); });
  sep();
  auto slider = [&](const QString& name, const QString& label, const QString& tip, double* val, std::function<QString(double)> fm) {
    auto* l = new QLabel(label); l->setProperty("mut", true); l->setToolTip(tip);
    auto* s = new QSlider(Qt::Horizontal); s->setObjectName(name); s->setRange(0, 100); s->setFixedWidth(80); s->setToolTip(tip);
    auto* v = new QLabel; v->setObjectName(name + "v"); v->setFixedWidth(34); v->setProperty("mut", true);
    QObject::connect(s, &QSlider::valueChanged, f, [=](int x) { *val = x / 100.0; v->setText(fm(*val)); vp->topoDisplayChanged(); });
    QObject::connect(s, &QSlider::sliderReleased, f, [=] { vp->saveTopoPrefs(); });
    h->addWidget(l); h->addWidget(s); h->addWidget(v);
  };
  slider("op", QObject::tr("Opacity"), QObject::tr("The opacity of the active geometry (the selected meshes) — see the reference through it"), &vp->tdisp.op, [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; });
  slider("pdo", QObject::tr("PDO"), QObject::tr("Polygon draw offset: the active geometry is drawn nearer the camera than it is, so it covers the meshes it lies on (it stays where it is on the screen and in the scene)"), &vp->tdisp.pdo, [](double x) { return QString::number(int(std::lround(x * 100))); });
  f->setProperty("vp", QVariant::fromValue<void*>(vp)); f->setProperty("sc", QVariant::fromValue<void*>(sc));
  auto* holder = new QObject(f); holder->setObjectName("viewFn");
  f->setProperty("viewWire", view().value("showMeshWire").toBool(true));
  QObject::connect(vp, &Viewport::changed, f, [f, view] { f->setProperty("viewWire", view().value("showMeshWire").toBool(true)); });
  f->hide();
  return f;
}
void syncTopoDisplay(QFrame* f) {
  auto* vp = static_cast<Viewport*>(f->property("vp").value<void*>()); auto* sc = static_cast<Scene*>(f->property("sc").value<void*>()); if (!vp || !sc) return;
  bool sm = true, fl = true, any = false; for (auto& [id, it] : sc->items) if (it.type == ItemType::IMesh) { any = true; if (it.raw.value("smoothN").toBool()) fl = false; else sm = false; }
  QJsonObject vw = sc->fileRaw.value("settings").toObject().value("view").toObject();
  auto set = [f](const char* n, bool on) { if (auto* b = f->findChild<QPushButton*>(n)) { QSignalBlocker k(b); b->setChecked(on); } };
  set("wire", vw.value("showMeshWire").toBool(true)); set("smooth", !vp->tdisp.unlit && any && sm); set("flat", !vp->tdisp.unlit && any && fl);
  set("unlit", vp->tdisp.unlit); set("notex", vp->tdisp.noTex); set("selThrough", vp->tset.selThrough);
  for (auto [n, v] : {std::pair<const char*, double>{"op", vp->tdisp.op}, {"pdo", vp->tdisp.pdo}}) if (auto* s = f->findChild<QSlider*>(n)) { QSignalBlocker k(s); s->setValue(int(std::lround(v * 100)));
    if (auto* l = f->findChild<QLabel*>(QString(n) + "v")) l->setText(QString(n) == "op" ? QString::number(int(std::lround(v * 100))) + "%" : QString::number(int(std::lround(v * 100)))); }
}

// ---------- the transform table ----------
TransformTable::TransformTable(Viewport* vp, QWidget* parent) : QFrame(parent), vp_(vp) {
  setObjectName("ttab"); setStyleSheet("#ttab{background:#262626;border-top:1px solid #3a3a3a;border-bottom:1px solid #3a3a3a} #ttab QLineEdit{height:19px;padding:0 3px;font-size:11px;color:#e8e8e8;background:#1b1b1b;border:1px solid #3a3a3a;border-radius:3px}"
                                        " #ttab QLineEdit:focus{border-color:#6f9fe8} #ttab QLineEdit[on=\"true\"]{background:#2b3a55;border-color:#6f9fe8} #ttab QLineEdit[pend=\"true\"]{border-top-color:#d8b44a;border-right-color:#d8b44a;border-bottom-color:#d8b44a;color:#f0d690} #ttab QLineEdit:disabled{color:#555}");
  auto* v = new QVBoxLayout(this); v->setContentsMargins(8, 6, 8, 8); v->setSpacing(2);
  auto* head = new QHBoxLayout; lab_ = new QLabel(tr("Transform")); lab_->setProperty("mut", true);
  unit_ = new QComboBox; for (const char* u : {"mm", "cm", "m", "in"}) unit_->addItem(u); unit_->setCurrentText(vp->tunit); unit_->setToolTip(tr("Units of the lengths in the table (the scene works in centimetres)")); unit_->setFixedHeight(18);
  connect(unit_, &QComboBox::currentTextChanged, this, [this](const QString& u) { vp_->tunit = u; vp_->saveTopoPrefs(); sync(true); });
  space_ = new QComboBox; space_->addItem(tr("World"), false); space_->addItem(tr("Local"), true); space_->setCurrentIndex(vp->tlocal ? 1 : 0); space_->setFixedHeight(18);
  space_->setToolTip(tr("World — the world's axes; Local — the object's own gizmo axes (its Align / Alt + drag): the object's place along them, a component selection's centre and size about its pivot along them"));
  connect(space_, &QComboBox::currentIndexChanged, this, [this](int i) { vp_->tlocal = i == 1; vp_->saveTopoPrefs(); sync(true); });
  head->addWidget(lab_, 1); head->addWidget(space_); head->addWidget(unit_); v->addLayout(head);
  auto* g = new QGridLayout; g->setSpacing(2); g->setColumnStretch(1, 1); g->setColumnStretch(2, 1); g->setColumnStretch(3, 1);
  const char* rows[3][2] = {{"P", "Position"}, {"R", "Rotation, degrees"}, {"S", "Size (the mesh's box; in a component mode — the selection's box along the world axes: 0 flattens it)"}};
  const char* axc[3] = {"#ff7070", "#7fe07f", "#7fa8ff"};
  for (int r = 0; r < 3; r++) {
    auto* b = new QLabel(rows[r][0]); b->setProperty("mut", true); b->setToolTip(tr(rows[r][1])); b->setAlignment(Qt::AlignCenter); b->setFixedWidth(14); g->addWidget(b, r, 0);
    for (int c = 0; c < 3; c++) {
      auto* e = new QLineEdit; e->setAlignment(Qt::AlignRight); e->setMinimumWidth(30); e->setCursor(Qt::SizeHorCursor);
      e->setToolTip(tr("%1 · %2 — Ctrl + drag: ±1, Ctrl+Shift + drag: ±0.1; drag across cells — select them, then type one number for all").arg(tr(rows[r][1])).arg(QChar("XYZ"[c])));
      e->setStyleSheet(QString("QLineEdit{border-left:2px solid %1}").arg(axc[c]));
      e->installEventFilter(this); g->addWidget(e, r, c + 1); cells_.push_back(e);
    }
  }
  v->addLayout(g);
  hint_ = new QLabel; hint_->setStyleSheet("color:#777;font-size:10px"); hint_->setWordWrap(true); v->addWidget(hint_);
  // (several signals of one step: one look at the selection, after them)
  auto later = [this](bool force) { pendForce_ = pendForce_ || force; if (pend_) return; pend_ = true; QTimer::singleShot(0, this, [this] { pend_ = false; const bool f = pendForce_; pendForce_ = false; sync(f); }); };
  connect(vp, &Viewport::topoState, this, [later] { later(false); });
  connect(vp, &Viewport::changed, this, [later] { later(false); });
  connect(vp, &Viewport::selectionChanged, this, [later] { later(true); });
  sync(true);
}
void TransformTable::select(const std::vector<int>& cells) { sel_ = cells; for (size_t i = 0; i < cells_.size(); i++) { bool on = cells.size() > 1 && std::find(cells.begin(), cells.end(), int(i)) != cells.end(); cells_[i]->setProperty("on", on); cells_[i]->style()->unpolish(cells_[i]); cells_[i]->style()->polish(cells_[i]); } }
void TransformTable::sync(bool force) {
  auto t = vp_->ttTarget(); auto vals = vp_->ttValues(t);
  bool focus = false; for (auto* c : cells_) if (c->hasFocus()) focus = true;
  QByteArray sig = QByteArray::number(t.kind) + "|" + QByteArray::number(t.m) + "|" + vp_->tunit.toLatin1() + (vp_->tlocal ? "L" : "W");
  for (auto& r : vals) if (r) for (double x : *r) sig += "|" + QByteArray::number(x, 'g', 9);
  if (!force && (sig == sig_ || focus || scrub_.cell >= 0)) return;
  sig_ = sig;
  const Item* it = vp_->scene()->item(t.m);
  lab_->setText(t.kind == 0 ? tr("Transform") : t.kind == 1 ? tr("Transform · %1").arg(it ? it->name : QString()) : t.kind == 4 ? tr("Group · %1").arg(it ? it->name : QString()) : t.kind == 2 ? tr("Selection · %1 v").arg(t.verts.size()) : tr("%1 · nothing selected").arg(it ? it->name : QString()));
  if (force && !focus) typed_.clear();                    // (another target: what was typed for the last one dropped)
  for (int k = 0; k < 9; k++) { auto& r = vals[size_t(k / 3)]; QLineEdit* c = cells_[size_t(k)]; c->setEnabled(bool(r)); if (typed_.count(k)) continue; if (!c->hasFocus() || force) c->setText(r ? fmt3((*r)[size_t(k % 3)]) : QString()); }
  markTyped();
  hint_->setText(t.kind == 4 ? (vp_->tlocal ? tr("Local: the group's point and turn from its object (along that object's axes) — what is in it goes along") : tr("The group's point and turn — what is in it goes along")) : t.kind == 0 ? tr("Select a mesh") : t.kind == 2 ? (vp_->tlocal ? tr("Local: P — the selection's centre from the object's pivot, S — its size, along the object's gizmo axes (0 flattens it)") : tr("P — the selection's centre, S — its size along the world axes (0 flattens it)")) : t.kind == 3 ? tr("Select polygons / edges / vertices") : QString());
  hint_->setVisible(!hint_->text().isEmpty());
}
// (the cells typed and waiting for Enter: their frame yellow)
void TransformTable::markTyped() {
  for (size_t i = 0; i < cells_.size(); i++) { const bool p = typed_.count(int(i)); if (cells_[i]->property("pend").toBool() == p) continue; cells_[i]->setProperty("pend", p); cells_[i]->style()->unpolish(cells_[i]); cells_[i]->style()->polish(cells_[i]); }
}
// Enter: every typed cell applied at once — the rotation and the size first, the position last (so it ends where typed:
// a size change about the selection's centre would move it again otherwise); one undo step
void TransformTable::applyTyped(const QString& s) {
  bool ok; double v = QString(s).replace(',', '.').toDouble(&ok); auto t = vp_->ttTarget();
  if (t.kind == 0 || t.kind == 3) { typed_.clear(); sync(true); return; }
  if (ok) for (int c : sel_) typed_[c] = v;
  if (typed_.empty()) { sync(true); return; }
  for (int row : {1, 2, 0}) for (auto& [c, x] : typed_) if (c / 3 == row) { t = vp_->ttTarget(); vp_->ttSet(t, c / 3, c % 3, x); }
  typed_.clear(); vp_->ttDone(t); select({}); sync(true);
}
bool TransformTable::eventFilter(QObject* o, QEvent* e) {
  auto* c = qobject_cast<QLineEdit*>(o); if (!c) return QFrame::eventFilter(o, e);
  const int ci = int(std::find(cells_.begin(), cells_.end(), c) - cells_.begin());
  if (e->type() == QEvent::MouseButtonPress) {
    auto* m = static_cast<QMouseEvent*>(e); if (m->button() != Qt::LeftButton || !c->isEnabled()) return false;
    auto t = vp_->ttTarget(); if (t.kind == 0 || t.kind == 3) return false;
    if (m->modifiers() & Qt::ControlModifier) { auto vals = vp_->ttValues(t); auto& r = vals[size_t(ci / 3)]; if (!r) return true;
      scrub_ = {ci, (*r)[size_t(ci % 3)], m->globalPosition().x(), false}; c->clearFocus(); c->grabMouse(); return true; }
    if (c->hasFocus() && sel_.size() == 1 && sel_[0] == ci) return false;   // (already editing: the caret)
    for (size_t i = 0; i < cells_.size(); i++) if (cells_[i]->hasFocus() && cells_[i]->isModified()) {   // (another cell clicked: the one typed into kept for Enter)
      bool ok2; const double v = cells_[i]->text().replace(',', '.').toDouble(&ok2); if (ok2) { if (sel_.empty()) sel_ = {int(i)}; for (int s : sel_) typed_[s] = v; } cells_[i]->setModified(false); markTyped(); }
    pickA_ = ci; select({ci}); c->grabMouse(); return true;
  }
  if (e->type() == QEvent::MouseMove) {
    auto* m = static_cast<QMouseEvent*>(e);
    if (scrub_.cell >= 0) {
      double dx = m->globalPosition().x() - scrub_.x0, step = (m->modifiers() & Qt::ShiftModifier) ? 0.1 : 1; int k = int(std::lround(dx / 4));
      if (k) scrub_.moved = true;
      double v = std::round((scrub_.v0 + k * step) * 1000) / 1000; auto t = vp_->ttTarget(); vp_->ttSet(t, scrub_.cell / 3, scrub_.cell % 3, v);
      cells_[size_t(scrub_.cell)]->setText(fmt3(v)); return true;
    }
    if (pickA_ >= 0) {
      QPoint gp = m->globalPosition().toPoint(); int over = -1;
      for (size_t i = 0; i < cells_.size(); i++) if (cells_[i]->rect().contains(cells_[i]->mapFromGlobal(gp))) over = int(i);
      if (over >= 0) { int r0 = std::min(pickA_ / 3, over / 3), r1 = std::max(pickA_ / 3, over / 3), c0 = std::min(pickA_ % 3, over % 3), c1 = std::max(pickA_ % 3, over % 3); std::vector<int> s;
        for (int k = 0; k < 9; k++) if (cells_[size_t(k)]->isEnabled() && k / 3 >= r0 && k / 3 <= r1 && k % 3 >= c0 && k % 3 <= c1) s.push_back(k); select(s); }
      return true;
    }
    return false;
  }
  if (e->type() == QEvent::MouseButtonRelease) {
    if (scrub_.cell >= 0) { QLineEdit* sc = cells_[size_t(scrub_.cell)]; sc->releaseMouse(); bool moved = scrub_.moved; scrub_ = {}; if (moved) vp_->ttDone(vp_->ttTarget()); sync(true); return true; }
    if (pickA_ >= 0) { QLineEdit* a = cells_[size_t(pickA_)]; a->releaseMouse(); pickA_ = -1; a->setFocus(); a->selectAll(); return true; }
    return false;
  }
  if (e->type() == QEvent::KeyPress) {
    auto* k = static_cast<QKeyEvent*>(e);
    if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) { if (sel_.empty()) sel_ = {ci}; applyTyped(c->isModified() || typed_.empty() ? c->text() : QString()); c->setModified(false); c->clearFocus(); return true; }
    if (k->key() == Qt::Key_Escape) { typed_.clear(); select({}); c->clearFocus(); sync(true); return true; }
    if (k->key() == Qt::Key_Tab) { if (sel_.empty()) sel_ = {ci};   // (Tab: kept typed — applied with the others on Enter)
      bool ok; const double v = c->text().replace(',', '.').toDouble(&ok); if (ok && c->isModified()) { for (int s : sel_) { typed_[s] = v; cells_[size_t(s)]->setText(fmt3(v)); } } c->setModified(false); markTyped(); int nx = ci; for (int s = 1; s <= 9; s++) { int j = (ci + s) % 9; if (cells_[size_t(j)]->isEnabled()) { nx = j; break; } }
      select({nx}); cells_[size_t(nx)]->setFocus(); cells_[size_t(nx)]->selectAll(); return true; }
    return false;
  }
  if (e->type() == QEvent::FocusOut) { QTimer::singleShot(0, this, [this] { bool f = false; for (auto* x : cells_) if (x->hasFocus()) f = true; if (!f) { select({}); sync(true); } }); return false; }
  return QFrame::eventFilter(o, e);
}

// ---------- a mesh's settings in the room ----------
namespace {
// the polygroups' list: a tree of its own (folders; a polygroup can hold others) — drag a row onto another's middle —
// into it, its edge — before / after; Ctrl / Shift — several; double click — select all of it; F2 — rename; Ctrl+G — group
class TgTree : public QTreeWidget {
public:
  TgTree(Viewport* vp, int m) : vp_(vp), m_(m) {}
  std::function<void()> moved;
protected:
  void dropEvent(QDropEvent* e) override { QTreeWidget::dropEvent(e); if (moved) QTimer::singleShot(0, this, [this] { moved(); }); }
  // (a click on a row's eye / duplicate / delete: that button only — the row isn't picked)
  bool onButton(QMouseEvent* e) const { return itemAt(e->position().toPoint()) && columnAt(int(e->position().x())) >= 2; }
  void mousePressEvent(QMouseEvent* e) override {
    if (onButton(e)) { if (e->button() == Qt::LeftButton) emit itemClicked(itemAt(e->position().toPoint()), columnAt(int(e->position().x()))); e->accept(); return; }
    QTreeWidget::mousePressEvent(e); }
  void mouseReleaseEvent(QMouseEvent* e) override { if (onButton(e)) { e->accept(); return; } QTreeWidget::mouseReleaseEvent(e); }
  void mouseDoubleClickEvent(QMouseEvent* e) override { if (onButton(e)) { e->accept(); return; } QTreeWidget::mouseDoubleClickEvent(e); }
  void keyPressEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_F2 && currentItem()) { editItem(currentItem(), 0); return; }
    if (e->key() == Qt::Key_G && (e->modifiers() & Qt::ControlModifier) && groupPicked) { groupPicked(); return; }
    QTreeWidget::keyPressEvent(e);
  }
public:
  std::function<void()> groupPicked;
  Viewport* vp_; int m_;
};
std::vector<topo::TgNode> treeFromWidget(QTreeWidgetItem* parent, QTreeWidget* w) {
  std::vector<topo::TgNode> out; int n = parent ? parent->childCount() : w->topLevelItemCount();
  for (int i = 0; i < n; i++) { QTreeWidgetItem* it = parent ? parent->child(i) : w->topLevelItem(i);
    topo::TgNode nd; nd.id = it->data(0, Qt::UserRole).toLongLong(); nd.g = it->data(0, Qt::UserRole + 1).toInt(); nd.isFolder = nd.g < 0; if (nd.isFolder) nd.f = it->text(0);
    nd.open = it->isExpanded() || !it->childCount(); nd.kids = treeFromWidget(it, w); out.push_back(std::move(nd)); }
  return out;
}
}  // namespace


// ---- UV Sets and UV Mats: the UV layouts (a set — its own UV over its polygons; the list's order — which wins a merge)
// and the materials, each its texture, each using a set; a click on one — the UV Editor shows only it (again — all); a
// mat's set lit with it. Drag a set — its place; Alt + drag onto another set / mat — merged, the dragged one wins ----
namespace {
class UvList : public QTreeWidget {
public:
  std::function<void(QTreeWidgetItem*)> rename;
  std::function<void(int from, int at, QTreeWidgetItem* onto, bool alt)> dropped;   // (at: the row it goes before; onto: Alt — the row under it)
protected:
  void keyPressEvent(QKeyEvent* e) override { if (e->key() == Qt::Key_F2 && currentItem() && rename) { rename(currentItem()); return; } QTreeWidget::keyPressEvent(e); }
  void mousePressEvent(QMouseEvent* e) override { QTreeWidgetItem* it = itemAt(e->position().toPoint()); from_ = it ? indexOfTopLevelItem(it) : -1; QTreeWidget::mousePressEvent(e); }
  void dragMoveEvent(QDragMoveEvent* e) override { QTreeWidget::dragMoveEvent(e); if (e->source() == this) e->acceptProposedAction(); }
  void dropEvent(QDropEvent* e) override {
    e->setDropAction(Qt::IgnoreAction); e->accept();            // (the rows stay: the list is made again after the edit)
    if (from_ < 0 || !dropped) return;
    QTreeWidgetItem* t = itemAt(e->position().toPoint()); const bool alt = e->modifiers() & Qt::AltModifier;
    int at = t ? indexOfTopLevelItem(t) : topLevelItemCount();
    if (t && dropIndicatorPosition() == QAbstractItemView::BelowItem) at++;
    const int from = from_; QTreeWidgetItem* onto = alt ? t : nullptr; auto fn = dropped;
    QTimer::singleShot(0, this, [fn, from, at, onto] { fn(from, at, onto, onto != nullptr); });
  }
private:
  int from_ = -1;
};
UvList* uvList(int cols, bool drag) {
  auto* t = new UvList; t->setColumnCount(cols); t->setHeaderHidden(true); t->setRootIsDecorated(false); t->setIndentation(0); t->setUniformRowHeights(true);
  t->setEditTriggers(QAbstractItemView::NoEditTriggers); t->setFocusPolicy(Qt::ClickFocus); t->setSelectionMode(QAbstractItemView::SingleSelection);
  t->header()->setStretchLastSection(false); t->header()->setSectionResizeMode(0, QHeaderView::Stretch); for (int c = 1; c < cols; c++) t->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
  t->setStyleSheet("QTreeWidget{background:#1f1f1f;border:1px solid #3a3a3a} QTreeWidget::item{height:22px} QTreeWidget::item:selected{background:transparent}"); t->setContextMenuPolicy(Qt::CustomContextMenu);
  if (drag) { t->setDragEnabled(true); t->setAcceptDrops(true); t->viewport()->setAcceptDrops(true); t->setDropIndicatorShown(true); t->setDragDropMode(QAbstractItemView::InternalMove); }
  return t;
}
void fitRows(QTreeWidget* t) { t->setFixedHeight(std::max(30, std::min(220, t->topLevelItemCount() * 23 + 6))); }
void paintRow(QTreeWidgetItem* it, int cols, const QColor& c) { for (int k = 0; k < cols; k++) it->setBackground(k, c); }
}  // namespace

// the visibility layers (under the polygroups): the whole model first, the layers saved, the hiding as it is now when it is
// none of them (a new layer — save it to keep it); a click shows one; ◎ selects what it shows, ⟳ keeps what is shown now in
// it, ✎ renames, Keep keeps, ✕ drops
void topoVisBlock(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  ProfScope prof_("topoVisBlock");
  const Item* m = sc->item(id); if (!m || !(m->base || m->poly)) return;
  const QString cur = m->raw.value("hidF").toString(); const QJsonArray L = m->raw.value("visSets").toArray();
  bool known = cur.isEmpty(); for (const QJsonValue& x : L) if (x.toObject().value("hid").toString() == cur) known = true;
  const int n = int(L.size()) + 1 + (known ? 0 : 1);
  if (!foldSec(v, vp, "vis", QObject::tr("Visibility layers (%1)").arg(n))) return;
  auto small = [](const QString& t, const QString& tip) { auto* b = new QToolButton; b->setText(t); b->setToolTip(tip); b->setAutoRaise(true); b->setFixedSize(22, 20); b->setFocusPolicy(Qt::NoFocus); return b; };
  auto row = [&](const QString& name, const QString& hid, int idx /* −1 whole, −2 current, else the saved one */) {
    auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(2);
    auto* nb = new QPushButton(name); nb->setCheckable(true); nb->setChecked(hid == cur); nb->setFlat(true); nb->setStyleSheet("QPushButton{text-align:left;padding:2px 6px} QPushButton:checked{background:#34507a}");
    nb->setToolTip(idx == -1 ? QObject::tr("Show the whole model") : QObject::tr("Show this layer — its hidden polygons hidden")); nb->setFocusPolicy(Qt::NoFocus);
    QObject::connect(nb, &QPushButton::clicked, vp, [vp, id, hid] { vp->visApply(id, hid); });
    h->addWidget(nb, 1);
    auto* sb = small("◎", QObject::tr("Select what this layer shows (polygons)")); QObject::connect(sb, &QToolButton::clicked, vp, [vp, id, hid] { vp->visSelect(id, hid); }); h->addWidget(sb);
    if (idx == -2) { auto* kb = small(QObject::tr("Keep"), QObject::tr("Keep this visibility as a layer")); kb->setFixedSize(40, 20);
      QObject::connect(kb, &QToolButton::clicked, vp, [vp, id, hid, k = int(L.size())] { rawEdit(vp, id, [hid, k](QJsonObject& r) { QJsonArray a = r.value("visSets").toArray(); a.append(QJsonObject{{"name", QObject::tr("Layer %1").arg(k + 1)}, {"hid", hid}}); r.insert("visSets", a); }); emit vp->topoState(); });
      h->addWidget(kb); }
    if (idx >= 0) {
      auto* rb = small("✎", QObject::tr("Rename the layer"));
      QObject::connect(rb, &QToolButton::clicked, vp, [vp, id, idx, name] { bool ok = false; const QString t = QInputDialog::getText(nullptr, QObject::tr("Rename the layer"), QObject::tr("Name:"), QLineEdit::Normal, name, &ok).trimmed();
        if (!ok || t.isEmpty()) return; rawEdit(vp, id, [idx, t](QJsonObject& r) { QJsonArray a = r.value("visSets").toArray(); if (idx < a.size()) { QJsonObject o = a[idx].toObject(); o.insert("name", t); a[idx] = o; } r.insert("visSets", a); }); emit vp->topoState(); });
      auto* db = small("✕", QObject::tr("Drop the layer from the list (what is shown stays)"));
      QObject::connect(db, &QToolButton::clicked, vp, [vp, id, idx] { rawEdit(vp, id, [idx](QJsonObject& r) { QJsonArray a = r.value("visSets").toArray(); if (idx < a.size()) a.removeAt(idx); if (a.isEmpty()) r.remove("visSets"); else r.insert("visSets", a); }); emit vp->topoState(); });
      auto* ub = small("⟳", QObject::tr("Update the layer: what is shown now kept in it (show the layer, hide / show parts with H, then update)")); ub->setEnabled(hid != cur);
      QObject::connect(ub, &QToolButton::clicked, vp, [vp, id, idx, cur] { rawEdit(vp, id, [idx, cur](QJsonObject& r) { QJsonArray a = r.value("visSets").toArray(); if (idx < a.size()) { QJsonObject o = a[idx].toObject(); o.insert("hid", cur); a[idx] = o; } r.insert("visSets", a); }); emit vp->topoState(); ui::toast(QObject::tr("Visibility layer updated")); });
      h->addWidget(ub); h->addWidget(rb); h->addWidget(db); }
    v->addWidget(w); };
  row(QObject::tr("The whole model"), QString(), -1);
  for (int i = 0; i < L.size(); i++) { const QJsonObject o = L[i].toObject(); row(o.value("name").toString(QObject::tr("Layer %1").arg(i + 1)), o.value("hid").toString(), i); }
  if (!known) row(QObject::tr("Now (not kept)"), cur, -2);
  if (L.isEmpty() && known) v->addWidget(muted(QObject::tr("Hide a part (H) — the visibility then shows up here as a new layer to keep")));
}

// the Topology Ghost's settings (the source mesh's block; the Reveal Brush's options too)
void ghostSettings(QVBoxLayout* v, Viewport* vp) {
  auto* reach = new SliderRow(QObject::tr("Reach, edges"), 0.1, 20, 0.1, vp->gho.dist, [](double x) { return QString::number(x, 'g', 3); });
  reach->setLog(true); reach->setReset(4); reach->setTip(QObject::tr("How far from the source the ghost looks for the surface (along each vertex's normal, both ways), in the source's polygon edges — the same on a model of any scale; farther — that part is not shown (drawn by hand). Loose clothes: more"));
  QObject::connect(reach, &SliderRow::changed, vp, [vp](double x) { vp->gho.dist = x; }); QObject::connect(reach, &SliderRow::released, vp, [vp](double) { vp->saveGhostPrefs(); vp->ghostChanged(); }); v->addWidget(reach);
  auto* rel = new SliderRow(QObject::tr("Relax"), 0, 10, 1, vp->gho.relax, [](double x) { return QString::number(int(x)); }); rel->setReset(3);
  rel->setTip(QObject::tr("Passes of evening the ghost out on the surface — no folds where the rays cross in the hollows"));
  QObject::connect(rel, &SliderRow::changed, vp, [vp](double x) { vp->gho.relax = int(std::lround(x)); }); QObject::connect(rel, &SliderRow::released, vp, [vp](double) { vp->saveGhostPrefs(); vp->ghostChanged(); }); v->addWidget(rel);
  { auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(4); auto* l = new QLabel(QObject::tr("Density")); l->setProperty("mut", true); h->addWidget(l);
    for (int d : {1, 2}) { auto* b = btn(d == 1 ? QObject::tr("×1 as the source") : QObject::tr("×2 subdivided"), d == 1 ? QObject::tr("The source's own loops") : QObject::tr("One more loop between each two of the source's — its loops kept"));
      b->setCheckable(true); b->setChecked(vp->gho.density == d); QObject::connect(b, &QPushButton::clicked, vp, [vp, d] { vp->gho.density = d; vp->saveGhostPrefs(); vp->ghostChanged(); emit vp->topoState(); }); h->addWidget(b, 1); }
    v->addWidget(w); }
  auto* tr2 = check(QObject::tr("Trim to the border"), vp->gho.trim, QObject::tr("A ghost polygon that only reached the surface's edge is left out — the ghost ends on the border of the garment / the gear"));
  QObject::connect(tr2, &QCheckBox::toggled, vp, [vp](bool x) { vp->gho.trim = x; vp->saveGhostPrefs(); vp->ghostChanged(); }); v->addWidget(tr2);
  auto* sh = check(QObject::tr("Show the ghost"), vp->gho.show, QObject::tr("The ghost drawn (green — close, yellow — stretched, red — doubtful); off — not drawn, nothing snaps to it"));
  QObject::connect(sh, &QCheckBox::toggled, vp, [vp](bool x) { vp->gho.show = x; vp->saveGhostPrefs(); vp->ghostChanged(); }); v->addWidget(sh);
  v->addWidget(muted(QObject::tr("Lies on the references (else every shown mesh but the edited one); their snap masks pick the layer. Works with the source hidden. Build by it: the Reveal Brush, PolyPen (snaps to its vertices and edges), Shift+RMB — its polygon under the pointer.")));
  auto* info = muted(vp->ghostInfo()); v->addWidget(info);
}
// the source mesh's Topology Ghost block (above the transform)
void topoGhostBlock(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  ProfScope prof_("topoGhostBlock");
  const Item* m = sc->item(id); if (!m || !m->raw.value("tghost").toBool()) return;
  if (!foldSec(v, vp, "ghost", QObject::tr("Topology Ghost"), true, "topo_ghost")) return;
  ghostSettings(v, vp);
}

// a UV Mat's menu (RMB on its button): its polygons, its textures, its UV Set, merged, renamed, deleted
void uvMatMenu(Viewport* vp, Scene* sc, int id, int k) {
  auto later = [vp](std::function<void()> fn) { QTimer::singleShot(0, vp, std::move(fn)); };
  auto mainWin = [] { for (QWidget* w : QApplication::topLevelWidgets()) if (qobject_cast<QMainWindow*>(w)) return w; return static_cast<QWidget*>(nullptr); };
  const auto MM = vp->uvMats(id); QString name; for (auto& x : MM) if (x.idx == k) name = x.name;
  const bool cur = vp->uvPicked.id == id && vp->uvPicked.mat == k;
  QMenu menu; auto act = [&](QMenu* mm, const QString& l, std::function<void()> fn) { QObject::connect(mm->addAction(l), &QAction::triggered, vp, [fn, later] { later(fn); }); };
  act(&menu, cur ? QObject::tr("⊘ Out of it (every polygon shown)") : QObject::tr("▣ Work in it (only it in the UV Editor)"), [vp, id, k] { vp->uvPickMat(id, k); });
  act(&menu, QObject::tr("⇲ Apply to the selected polygons"), [vp, id, k] { vp->uvMatApply(id, k); });
  act(&menu, QObject::tr("◉ Select its polygons"), [vp, id, k] { vp->uvSelectMats(id, {k}); });
  act(&menu, QObject::tr("▦ Textures…"), [vp, sc, id, k, mainWin] { openTexWindow(mainWin(), vp, sc, id, k); });
  QMenu* ms = menu.addMenu(QObject::tr("⇢ Use the UV Set")); auto SS = vp->uvSets(id); const int cs = vp->uvSetOfMat(id, k);
  for (size_t i = 0; i < SS.size(); i++) if (int(i) != cs) { const int ii = int(i); act(ms, SS[i].name, [vp, id, k, ii] { vp->uvMatToSet(id, k, ii); }); }
  ms->setEnabled(SS.size() > 1);
  QMenu* mg = menu.addMenu(QObject::tr("⊕ Merge into it"));
  for (auto& x : MM) if (x.idx != k) { const int o = x.idx; act(mg, x.name, [vp, id, k, o] { vp->uvMatMerge(id, k, o); }); }
  mg->setEnabled(MM.size() > 1);
  act(&menu, QObject::tr("✎ Rename (double click)"), [vp, id, k, name] { const QString t = askName(QObject::tr("Rename the UV Mat"), name); if (!t.isEmpty()) vp->uvMatRename(id, k, t); });
  menu.addSeparator(); if (k >= 0) act(&menu, QObject::tr("✕ Delete (its polygons to Default)"), [vp, id, k] { vp->uvMatDelete(id, k); });
  menu.exec(QCursor::pos());
}
QWidget* uvMatGrid(Viewport* vp, Scene* sc, int id, int cols) {
  auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 4); g->setSpacing(4);
  const auto M = vp->uvMats(id); const auto S = vp->uvSets(id); int n = 0;
  for (auto& x : M) {
    const int mk = x.idx; const QString name = x.name; const int si = vp->uvSetOfMat(id, mk);
    const QString tip = QObject::tr("%1: %2 polygons, UV Set: %3 — click: its polygons selected (Shift adds, Ctrl takes off), it worked in (only it in the UV Editor); double click: rename; RMB: more (textures…)")
                          .arg(name).arg(x.faces).arg(si >= 0 && size_t(si) < S.size() ? S[size_t(si)].name : QObject::tr("none"));
    auto k = [vp, id, mk] { if (vp->uvPicked.id == id && vp->uvPicked.mat == mk) return 2;
      if (vp->topoSel().id != id || vp->topoMode == "object") return 0; const PolyMesh* B = uvBase(vp, id); if (!B) return 0;
      for (int f : vp->tSelFaces(id)) if ((size_t(f) < B->faceMat.size() ? B->faceMat[size_t(f)] : -1) == mk) return 1; return 0; };
    auto click = [vp, id, mk, name] {
      if (secondClick(id, 10000 + mk)) { const QString t = askName(QObject::tr("Rename the UV Mat"), name); if (!t.isEmpty()) vp->uvMatRename(id, mk, t); return; }
      uvSelectFaces(vp, id, vp->uvMatFaces(id, {mk}));
      if (!(vp->uvPicked.id == id && vp->uvPicked.mat == mk)) vp->uvPickMat(id, mk); };
    g->addWidget(uvSetCell(vp, "uv_mats", name, tip, k, click, [vp, sc, id, mk] { uvMatMenu(vp, sc, id, mk); }), n / cols, n % cols); n++;
  }
  g->setColumnStretch(cols, 1); return w;
}

void topoUvBlocks(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  ProfScope prof_("topoUvBlocks");
  const Item* m = sc->item(id); if (!m) return;
  auto mainWin = [] { for (QWidget* w : QApplication::topLevelWidgets()) if (qobject_cast<QMainWindow*>(w)) return w; return static_cast<QWidget*>(nullptr); };
  const auto S = vp->uvSets(id); const auto M = vp->uvMats(id); const auto& P = vp->uvPicked; const bool mine = P.id == id;
  const QColor ON(0x35, 0x50, 0x78), SOFT(0x2c, 0x3c, 0x55);
  const Qt::ItemFlags rowFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
  auto glyph = [](const QString& g, const QString& tip) { auto* b = new QPushButton(g); b->setFixedSize(34, 26); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); b->setStyleSheet("font-size:14px;padding:0"); return b; };
  auto later = [vp](std::function<void()> fn) { QTimer::singleShot(0, vp, std::move(fn)); };
  auto matName = [&M](int k) { for (auto& x : M) if (x.idx == k) return x.name; return QString("Default"); };
  // ---- UV Sets ----
  if (foldSec(v, vp, "uvsets", QObject::tr("UV Sets (%1)").arg(S.size()), true, "uv_set")) {
    v->addWidget(uvSetGrid(vp, id, 5));
    const int cur = mine ? P.set : -1;
    auto* nw = glyph("＋", QObject::tr("A new UV Set — a layout of its own over the selected polygons (their UV as they are; their mats use it)"));
    auto* ap = glyph("⇲", QObject::tr("The selected polygons into the picked UV Set's layout: a mat wholly selected uses it from then on, a part of a mat becomes a new mat in it"));
    auto* du = glyph("⧉", QObject::tr("Duplicate the picked UV Set — a second layout of the same polygons, edited on its own"));
    auto* se = glyph("◉", QObject::tr("Select the picked set's polygons"));
    auto* rm = glyph("✕", QObject::tr("Delete the picked UV Set (its polygons show their other set; its mats use another set)"));
    ap->setEnabled(cur >= 0); du->setEnabled(cur >= 0); se->setEnabled(cur >= 0); rm->setEnabled(cur >= 0 && S.size() > 1);
    QObject::connect(nw, &QPushButton::clicked, vp, [vp, id, later] { later([vp, id] { vp->uvSetNew(id); }); });
    QObject::connect(ap, &QPushButton::clicked, vp, [vp, id, cur, later] { later([vp, id, cur] { vp->uvSetApply(id, cur); }); });
    QObject::connect(du, &QPushButton::clicked, vp, [vp, id, cur, later] { later([vp, id, cur] { vp->uvSetDuplicate(id, cur); }); });
    QObject::connect(se, &QPushButton::clicked, vp, [vp, id, cur, later] { later([vp, id, cur] { const Item* mm = vp->scene()->item(id); const PolyMesh* B = mm ? (mm->base ? mm->base.get() : mm->poly.get()) : nullptr; if (!B) return;
      std::vector<int> fs; for (int f = 0; f < B->faceCount(); f++) if (!topo::hasUvSets(*B) || (B->faceSets[size_t(f)] >> cur & 1u)) fs.push_back(f);
      if (vp->topoMode != "poly") vp->setTopoMode("poly"); vp->tSelectFaces(id, fs); }); });
    QObject::connect(rm, &QPushButton::clicked, vp, [vp, id, cur, later] { later([vp, id, cur] { vp->uvSetDelete(id, cur); }); });
    v->addWidget(hrow({nw, ap, du, se, rm}));
  }
  // ---- UV Mats ----
  if (foldSec(v, vp, "uvmats", QObject::tr("UV Mats (%1)").arg(M.size()), true, "uv_mats")) {
    v->addWidget(uvMatGrid(vp, sc, id, 5));
    const int cur = mine ? P.mat : -2;
    auto* nw = glyph("＋", QObject::tr("A new UV Mat using the picked UV Set (else the first) — the selected polygons get it"));
    auto* ap = glyph("⇲", QObject::tr("The selected polygons get the picked UV Mat"));
    auto* se = glyph("◉", QObject::tr("Select the picked mat's polygons"));
    auto* tx = glyph("▦", QObject::tr("The texture manager for the picked mat (double click on a mat — the same)"));
    auto* rm = glyph("✕", QObject::tr("Delete the picked UV Mat (its polygons go to Default)"));
    ap->setEnabled(cur != -2); se->setEnabled(cur != -2); tx->setEnabled(cur != -2); rm->setEnabled(cur >= 0);
    QObject::connect(nw, &QPushButton::clicked, vp, [vp, id, later] { later([vp, id] { vp->uvMatNew(id); }); });
    QObject::connect(ap, &QPushButton::clicked, vp, [vp, id, cur, later] { later([vp, id, cur] { vp->uvMatApply(id, cur); }); });
    QObject::connect(se, &QPushButton::clicked, vp, [vp, id, cur, later] { later([vp, id, cur] { vp->uvSelectMats(id, {cur}); }); });
    QObject::connect(tx, &QPushButton::clicked, vp, [vp, sc, id, cur, mainWin] { QTimer::singleShot(0, vp, [vp, sc, id, cur, mainWin] { openTexWindow(mainWin(), vp, sc, id, cur); }); });
    QObject::connect(rm, &QPushButton::clicked, vp, [vp, id, cur, later] { later([vp, id, cur] { vp->uvMatDelete(id, cur); }); });
    v->addWidget(hrow({nw, ap, se, tx, rm}));
    v->addWidget(muted(QObject::tr("A UV Set is a UV layout of its own: one polygon can be in several (two layouts of a head); the picked set's layout is what its polygons show. Pack all / Unfold all — each mat into its own tile.")));
  }
}

void topoMeshProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  ProfScope prof_("topoMeshProps");
  const Item* m = sc->item(id); const PolyMesh* B = baseOf(*m); if (!B) { v->addWidget(muted(QObject::tr("Loading…"))); return; }
  const bool heavy = meshmods::heavy(*m);
  // ---- a primitive: Convert to object on top, then its settings (the mesh made again as they change) ----
  if (m->raw.value("prim").isObject()) {
    const QJsonObject pr = m->raw.value("prim").toObject(); const QString type = pr.value("type").toString(); const QJsonObject pp = pr.value("p").toObject();
    auto* cv = new QPushButton(QObject::tr("◆ Convert to object")); cv->setProperty("primary", true);
    cv->setToolTip(QObject::tr("A plain polygonal mesh from now on: the primitive's settings are dropped, its polygons stay — edit them as any mesh"));
    QObject::connect(cv, &QPushButton::clicked, vp, [vp, id] { vp->primConvert(id); });
    v->addWidget(cv);
    v->addWidget(sec(QObject::tr("%1 — settings").arg(prim::label(type))));
    for (const auto& d : prim::params(type)) {
      const bool integer = d.integer;
      auto* r = new SliderRow(QObject::tr(d.label), d.lo, d.hi, d.step, pp.value(d.key).toDouble(d.def), [integer](double x) { return integer ? QString::number(int(std::lround(x))) : QString::number(x, 'f', 2); });
      const QString key = d.key;
      QObject::connect(r, &SliderRow::changed, vp, [vp, id, key](double x) { vp->primSet(id, key, x, false); });
      QObject::connect(r, &SliderRow::released, vp, [vp, id, key](double x) { vp->primSet(id, key, x, true); });
      v->addWidget(r); }
  }
  v->addWidget(muted(QObject::tr("%1 vertices · %2 polygons · %3 triangles").arg(B->pos.size()).arg(B->faceCount()).arg(topo::triCount(*B))));
  // ---- polygroups ----
  const int G = int(B->groupNames.size());
  v->addWidget(sec(QObject::tr("Polygroups (%1)").arg(G)));
  std::vector<int> cnt = vp->groupCountsOf(id); cnt.resize(size_t(G), 0);
  std::set<int> hid; for (const QJsonValue& x : m->raw.value("hidGroups").toArray()) hid.insert(x.toInt());
  auto tree = topo::tgTree(m->raw.value("gTree"), G, m->raw.value("gOrder"));
  auto* tw = new TgTree(vp, id); tw->setObjectName("tgTree"); tw->setColumnCount(5); tw->setHeaderHidden(true); tw->setRootIsDecorated(true); tw->setIndentation(13);
  tw->setSelectionMode(QAbstractItemView::ExtendedSelection); tw->setDragDropMode(QAbstractItemView::InternalMove); tw->setDefaultDropAction(Qt::MoveAction);
  tw->setEditTriggers(QAbstractItemView::NoEditTriggers); tw->setUniformRowHeights(true); tw->setFocusPolicy(Qt::ClickFocus);
  tw->header()->setStretchLastSection(false); tw->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int c = 1; c < 5; c++) tw->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
  tw->setStyleSheet("QTreeWidget{background:#1f1f1f;border:1px solid #3a3a3a} QTreeWidget::item{height:22px} QTreeWidget::item:selected{background:#3d6aa8;color:#fff}");   // (the picked row — clearly apart from the rows the selection lies in)
  auto dot = [](const QColor& c) { QPixmap p(10, 10); p.fill(Qt::transparent); QPainter g(&p); g.setRenderHint(QPainter::Antialiasing); g.setBrush(c); g.setPen(Qt::NoPen); g.drawEllipse(0, 0, 10, 10); return QIcon(p); };
  const std::set<qint64> LS = vp->tgList.id == id ? vp->tgList.set : std::set<qint64>{};
  std::set<int> selG;                                           // (the polygroups of the selected polygons: their rows lit)
  const size_t selN = vp->topoSel().id == id ? vp->topoSel().f.size() + vp->topoSel().e.size() + vp->topoSel().v.size() : 0;
  if (selN && selN <= 200000 && vp->topoMode != "object" && int(B->faceGroup.size()) == B->faceCount()) for (int f : vp->tSelFaces(id)) if (size_t(f) < B->faceGroup.size()) selG.insert(B->faceGroup[size_t(f)]);
  std::function<void(const std::vector<topo::TgNode>&, QTreeWidgetItem*)> add = [&](const std::vector<topo::TgNode>& L, QTreeWidgetItem* parent) {
    for (const auto& n : L) {
      auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tw);
      auto gs = topo::tgUnder(n); bool allHid = !gs.empty() && std::all_of(gs.begin(), gs.end(), [&](int g) { return hid.count(g); });
      it->setData(0, Qt::UserRole, qlonglong(n.id)); it->setData(0, Qt::UserRole + 1, n.isFolder ? -1 : n.g);
      it->setText(0, n.isFolder ? n.f : (n.g < G ? B->groupNames[size_t(n.g)] : QString()));
      it->setIcon(0, n.isFolder ? QIcon() : dot(tgColor(n.g)));
      int c = 0; for (int g : gs) if (g < G) c += cnt[size_t(g)]; it->setText(1, QString::number(n.isFolder ? c : (n.g < G ? cnt[size_t(n.g)] : 0)));
      it->setForeground(1, QColor(0x8a, 0x8a, 0x8a));
      const bool picked = LS.count(n.id) || (!n.isFolder && ((vp->tgSel.id == id && vp->tgSel.gi == n.g) || (vp->tgPick.id == id && vp->tgPick.set.count(n.g))));
      if (!picked && std::any_of(gs.begin(), gs.end(), [&](int g) { return selG.count(g) > 0; })) for (int c = 0; c < 5; c++) it->setBackground(c, QColor(0x2a, 0x30, 0x3a));   // (the selection lies in it: a faint tint — the picked one is the bright row)
      bool off = n.isFolder ? allHid : hid.count(n.g); it->setIcon(2, ui::eyeIcon(!off)); it->setToolTip(2, n.isFolder ? QObject::tr("Show / hide every polygroup in it") : QObject::tr("Show / hide this polygroup"));
      if (!n.isFolder) { it->setText(3, "⧉"); it->setToolTip(3, QObject::tr("Duplicate (a copy lying on it, a group of its own)")); it->setText(4, "✕"); it->setToolTip(4, QObject::tr("Delete the polygroup and its polygons"));
        it->setToolTip(0, QObject::tr("%1: %2 polygons — click: pick (Ctrl — add, Shift — a range), double click: select all of it, F2: rename, Ctrl+G: group the picked ones, RMB: more; drag: onto a row's middle — into it, its edge — before / after").arg(it->text(0)).arg(it->text(1))); }
      else { it->setText(4, "✕"); it->setToolTip(4, QObject::tr("Remove the folder (its polygroups stay)")); it->setForeground(0, QColor(0x99, 0xaa, 0xbb));
        it->setToolTip(0, QObject::tr("%1: a folder of %2 polygroup%3 — double click: select them all, F2: rename, RMB: more").arg(n.f).arg(gs.size()).arg(gs.size() == 1 ? "" : "s")); }
      if (off) for (int k = 0; k < 2; k++) it->setForeground(k, QColor(0x66, 0x66, 0x66));
      it->setFlags(it->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
      add(n.kids, it);
      it->setExpanded(n.open);
      if (picked) it->setSelected(true);
    }
  };
  { QSignalBlocker b(tw); add(tree, nullptr); }
  // (the list as tall as its open rows, up to the kept height — again when a folder opens / closes)
  auto fitList = [tw] {
    int rows = 0; std::function<void(QTreeWidgetItem*)> countRows = [&](QTreeWidgetItem* it) { rows++; if (it->isExpanded()) for (int i = 0; i < it->childCount(); i++) countRows(it->child(i)); };
    for (int i = 0; i < tw->topLevelItemCount(); i++) countRows(tw->topLevelItem(i));
    int hKept = QSettings().value("topo/tglH", 200).toInt(); tw->setFixedHeight(std::max(48, std::min(hKept, rows * 23 + 8)));
  };
  fitList();
  // (the list made anew — a polygroup shown / hidden, picked …: where it was scrolled kept, per mesh)
  { static QHash<int, int> scrolled; const int keep = scrolled.value(id, 0);
    QTimer::singleShot(0, tw, [tw, id, keep] { tw->verticalScrollBar()->setValue(keep); QObject::connect(tw->verticalScrollBar(), &QScrollBar::valueChanged, tw, [id](int x) { scrolled[id] = x; }); }); }
  auto underOf = [](QTreeWidgetItem* it) { std::vector<int> o; std::function<void(QTreeWidgetItem*)> w = [&](QTreeWidgetItem* x) { int g = x->data(0, Qt::UserRole + 1).toInt(); if (g >= 0) o.push_back(g); for (int i = 0; i < x->childCount(); i++) w(x->child(i)); }; w(it); return o; };
  QObject::connect(tw, &QTreeWidget::itemSelectionChanged, tw, [vp, id, tw] {
    vp->tgList = {}; vp->tgList.id = id; for (auto* it : tw->selectedItems()) vp->tgList.set.insert(it->data(0, Qt::UserRole).toLongLong());
    QTreeWidgetItem* cur = tw->currentItem(); vp->tgList.last = cur ? cur->data(0, Qt::UserRole).toLongLong() : -1;
    int g = cur && cur->isSelected() ? cur->data(0, Qt::UserRole + 1).toInt() : -1;
    vp->tgSel = g >= 0 ? Viewport::TgSel{id, g} : vp->tgList.set.empty() ? Viewport::TgSel{} : vp->tgSel; vp->tgFocus = id;
    if (g >= 0 && baseOf(*vp->scene()->item(id))->faceCount() < 200000) { auto sim = topo::similarFaces(vp->scene()->item(id)->base ? vp->scene()->item(id)->base : vp->scene()->item(id)->poly, g); if (!sim.empty()) ui::hud(QObject::tr("%1: and %2 polygon%3 on topologically identical parts").arg(cur->text(0)).arg(sim.size()).arg(sim.size() == 1 ? "" : "s"), 1400); }
    vp->topoChanged();
  });
  QObject::connect(tw, &QTreeWidget::itemClicked, tw, [vp, id, underOf](QTreeWidgetItem* it, int col) {
    int g = it->data(0, Qt::UserRole + 1).toInt(); qint64 nid = it->data(0, Qt::UserRole).toLongLong();
    if (col == 2) { auto gs = underOf(it); const Item* mm = vp->scene()->item(id); std::set<int> h; for (const QJsonValue& x : mm->raw.value("hidGroups").toArray()) h.insert(x.toInt());
      // (Alt: only this one shown — every other polygroup hidden; Alt on it again — the visibility as it was before)
      static QHash<int, std::pair<qint64, std::set<int>>> solo;
      if (QApplication::keyboardModifiers() & Qt::AltModifier) {
        const int G = int(baseOf(*mm)->groupNames.size()); std::vector<int> all(static_cast<size_t>(G), 0); std::iota(all.begin(), all.end(), 0);
        if (solo.contains(id) && solo[id].first == nid) { const std::set<int> prev = solo[id].second; solo.remove(id);
          std::vector<int> sh, hd; for (int x : all) (prev.count(x) ? hd : sh).push_back(x); vp->tgSetVisible(id, sh, true); vp->tgSetVisible(id, hd, false);
          ui::hud(QObject::tr("Polygroups: as they were"), 900); }
        else { if (!solo.contains(id)) solo[id] = {nid, h}; else solo[id].first = nid;
          std::set<int> keep(gs.begin(), gs.end()); std::vector<int> others; for (int x : all) if (!keep.count(x)) others.push_back(x);
          vp->tgSetVisible(id, gs, true); vp->tgSetVisible(id, others, false); ui::hud(QObject::tr("Only %1 shown — Alt on its eye again: all back as it was").arg(it->text(0)), 1200); }
        vp->runEdit([](Scene&, Selection&, const CurveCache&) { return QString(); }, false); return; }
      solo.remove(id);
      bool show = std::all_of(gs.begin(), gs.end(), [&](int x) { return h.count(x); }); vp->tgSetVisible(id, gs, show); vp->runEdit([](Scene&, Selection&, const CurveCache&) { return QString(); }, false); }
    else if (col == 3 && g >= 0) QTimer::singleShot(0, vp, [vp, id, g] { vp->tgDuplicate(id, g); });
    else if (col == 4) { if (g >= 0) QTimer::singleShot(0, vp, [vp, id, g] { vp->tgDelete(id, g); });
      else QTimer::singleShot(0, vp, [vp, id, nid] { vp->tgTreeEdit(id, [nid](std::vector<topo::TgNode>& t) { std::vector<topo::TgNode>* arr = nullptr; topo::TgNode* n = topo::tgFind(t, nid, &arr); if (!n) return;
        auto kids = n->kids; auto at = arr->begin() + (n - arr->data()); at = arr->erase(at); arr->insert(at, kids.begin(), kids.end()); }); }); }
  });
  QObject::connect(tw, &QTreeWidget::itemDoubleClicked, tw, [vp, id, underOf](QTreeWidgetItem* it, int col) {
    if (col >= 2) return; const PolyMesh* BB = baseOf(*vp->scene()->item(id)); std::vector<int> fs; for (int g : underOf(it)) { auto f = topo::groupFaces(*BB, g); fs.insert(fs.end(), f.begin(), f.end()); }
    QString name = it->text(0); QTimer::singleShot(0, vp, [vp, id, fs, name] { int k = vp->tSelectFaces(id, fs); ui::hud(QObject::tr("%1: %2 selected").arg(name).arg(k), 1000); });
  });
  QObject::connect(tw, &QTreeWidget::itemChanged, tw, [vp, id](QTreeWidgetItem* it, int col) {
    if (col != 0) return; int g = it->data(0, Qt::UserRole + 1).toInt(); qint64 nid = it->data(0, Qt::UserRole).toLongLong(); QString name = it->text(0).trimmed();
    if (g >= 0) QTimer::singleShot(0, vp, [vp, id, g, name] { vp->tgRename(id, g, name); });
    else if (!name.isEmpty()) QTimer::singleShot(0, vp, [vp, id, nid, name] { vp->tgTreeEdit(id, [nid, name](std::vector<topo::TgNode>& t) { if (auto* n = topo::tgFind(t, nid)) n->f = name; }); });
  });
  auto save = [vp, id, tw] { auto t = treeFromWidget(nullptr, tw); vp->tgTreeEdit(id, [t](std::vector<topo::TgNode>& x) { x = t; }); };
  tw->moved = save;
  QObject::connect(tw, &QTreeWidget::itemExpanded, tw, [vp, id, tw, fitList](QTreeWidgetItem*) { fitList(); auto t = treeFromWidget(nullptr, tw); QTimer::singleShot(0, vp, [vp, id, t] { vp->tgTreeEdit(id, [t](std::vector<topo::TgNode>& x) { x = t; }, false, true); }); });
  QObject::connect(tw, &QTreeWidget::itemCollapsed, tw, [vp, id, tw, fitList](QTreeWidgetItem*) { fitList(); auto t = treeFromWidget(nullptr, tw); QTimer::singleShot(0, vp, [vp, id, t] { vp->tgTreeEdit(id, [t](std::vector<topo::TgNode>& x) { x = t; }, false, true); }); });
  // the picked rows into a new folder (where the first of them was)
  auto groupPicked = [vp, id, tw] {
    std::vector<qint64> ids; for (auto* it : tw->selectedItems()) ids.push_back(it->data(0, Qt::UserRole).toLongLong());
    if (ids.empty()) { ui::toast(QObject::tr("Pick polygroups in the list first (Ctrl / Shift — several)")); return; }
    QTimer::singleShot(0, vp, [vp, id, ids] { vp->tgTreeEdit(id, [ids](std::vector<topo::TgNode>& t) {
      int k = 1; topo::tgWalk(t, [&](const topo::TgNode& n, int) { if (n.isFolder && n.f.startsWith("Group ")) k = std::max(k, n.f.mid(6).toInt() + 1); });
      std::vector<topo::TgNode> picked; std::vector<topo::TgNode>* firstArr = nullptr; long firstAt = -1;
      for (qint64 nid : ids) { std::vector<topo::TgNode>* arr = nullptr; topo::TgNode* n = topo::tgFind(t, nid, &arr); if (!n) continue;
        bool inside = false; for (qint64 o : ids) if (o != nid) { std::vector<topo::TgNode>* a2 = nullptr; if (topo::TgNode* p = topo::tgFind(t, o, &a2)) if (topo::tgFind(p->kids, nid)) inside = true; }
        if (inside) continue;
        if (!firstArr) { firstArr = arr; firstAt = long(n - arr->data()); }
        picked.push_back(*n); arr->erase(arr->begin() + (n - arr->data())); if (arr == firstArr && firstAt > long(arr->size())) firstAt = long(arr->size()); }
      if (!firstArr) return;
      topo::TgNode f; f.id = topo::tgNid(); f.isFolder = true; f.f = QString("Group %1").arg(k); f.kids = picked;
      firstArr->insert(firstArr->begin() + std::min<long>(firstAt, long(firstArr->size())), f);
    }); ui::toast(QObject::tr("Grouped — F2 to name it")); });
  };
  tw->groupPicked = groupPicked;
  tw->setContextMenuPolicy(Qt::CustomContextMenu);
  QObject::connect(tw, &QTreeWidget::customContextMenuRequested, tw, [vp, id, tw, underOf, groupPicked](const QPoint& p) {
    QTreeWidgetItem* it = tw->itemAt(p); if (!it) return;
    if (!it->isSelected()) { tw->clearSelection(); it->setSelected(true); tw->setCurrentItem(it); }
    const int g = it->data(0, Qt::UserRole + 1).toInt(); const qint64 nid = it->data(0, Qt::UserRole).toLongLong();
    const PolyMesh* BB = baseOf(*vp->scene()->item(id)); const int GG = int(BB->groupNames.size());
    QMenu menu; menu.setToolTipsVisible(true);
    auto act = [&](const QString& l, std::function<void()> fn, bool dis = false, const QString& tip = {}) { QAction* a = menu.addAction(l); a->setEnabled(!dis); a->setToolTip(tip); QObject::connect(a, &QAction::triggered, vp, [fn] { QTimer::singleShot(0, fn); }); return a; };
    if (g < 0) {
      act(QObject::tr("✎ Rename (F2)"), [tw, it] { tw->editItem(it, 0); });
      act(QObject::tr("▣ Group the picked (Ctrl+G)"), groupPicked);
      act(QObject::tr("⇱ Ungroup (the polygroups stay)"), [vp, id, nid] { vp->tgTreeEdit(id, [nid](std::vector<topo::TgNode>& t) { std::vector<topo::TgNode>* arr = nullptr; topo::TgNode* n = topo::tgFind(t, nid, &arr); if (!n) return; auto kids = n->kids; auto at = arr->erase(arr->begin() + (n - arr->data())); arr->insert(at, kids.begin(), kids.end()); }); });
      menu.addSeparator(); auto gs = underOf(it);
      act(QObject::tr("◉ Select its polygroups"), [vp, id, gs, BB] { std::vector<int> fs; for (int x : gs) { auto f = topo::groupFaces(*BB, x); fs.insert(fs.end(), f.begin(), f.end()); } vp->tSelectFaces(id, fs); }, gs.empty());
      act(QObject::tr("Show all in it"), [vp, id, gs] { vp->tgSetVisible(id, gs, true); vp->runEdit([](Scene&, Selection&, const CurveCache&) { return QString(); }, false); }, gs.empty());
      act(QObject::tr("— Hide all in it"), [vp, id, gs] { vp->tgSetVisible(id, gs, false); vp->runEdit([](Scene&, Selection&, const CurveCache&) { return QString(); }, false); }, gs.empty());
    } else {
      act(QObject::tr("⤴ Detach as a separate mesh"), [vp, id, g] { vp->tgDetach(id, g); }, GG < 2, QObject::tr("This polygroup becomes a mesh of its own (placed where it is)"));
      QMenu* mg = menu.addMenu(QObject::tr("⊕ Merge with")); mg->setEnabled(GG >= 2);
      auto order = topo::tgTree(vp->scene()->item(id)->raw.value("gTree"), GG, vp->scene()->item(id)->raw.value("gOrder")); std::vector<int> gl; topo::tgWalk(order, [&](const topo::TgNode& n, int) { if (!n.isFolder) gl.push_back(n.g); });
      for (int o : gl) if (o != g) { QAction* a = mg->addAction(BB->groupNames[size_t(o)]); QObject::connect(a, &QAction::triggered, vp, [vp, id, g, o] { QTimer::singleShot(0, [vp, id, g, o] { vp->tgMerge(id, g, o); }); }); }
      menu.addSeparator();
      act(QObject::tr("▢ Select the border"), [vp, id, g] { vp->tGroupBorder(id, g); }, false, QObject::tr("The edges along the group's edge — as edges, vertices or polygons by the mode (Object mode: edges)"));
      const auto& ts = vp->topoSel();
      act(QObject::tr("⇲ Move the selected polygons here"), [vp, id, g] { vp->tgMoveSelHere(id, g); }, !(ts.id == id && !ts.f.empty()), QObject::tr("The polygons selected now (Polygon mode) go into this group"));
      menu.addSeparator();
      act(QObject::tr("≡ Select all topologically identical parts"), [vp, id, g] { vp->tSelectIdentical(id, g); }, false, QObject::tr("Every part of the mesh with the same topology as this group's part(s)"));
      act(QObject::tr("⇋ Select the symmetric ones"), [vp, id, g] { vp->tSelectSymmetric(id, g); }, false, QObject::tr("The mirrored copy of this group's part — or, on one symmetric part, the polygons mirrored across X"));
      menu.addSeparator();
      { auto a = vp->smArr(id); auto gs = vp->smPickedGroups(id); gs.insert(g); int n = 0; for (size_t f = 0; f < a.size(); f++) if (a[f] && gs.count(BB->faceGroup[f])) n++;
        int out = 0; for (int f = 0; f < BB->faceCount(); f++) if (gs.count(BB->faceGroup[size_t(f)]) && (size_t(f) >= a.size() || !a[size_t(f)])) out++;
        act(gs.size() > 1 ? QObject::tr("▣ Mask — add to the snap mask (%1 polygroups)").arg(gs.size()) : QObject::tr("▣ Mask — add to the snap mask"), [vp, id, gs] { vp->smAddGroups(id, gs); }, !out,
            QObject::tr("Its polygons (and the other picked polygroups') into this mesh's snap mask — vertices then snap onto them"));
        act(gs.size() > 1 ? QObject::tr("⌫ Remove from the snap mask (%1 polygroups)").arg(gs.size()) : QObject::tr("⌫ Remove from the snap mask"), [vp, id, gs] { vp->smRemoveGroups(id, gs); }, !n,
            QObject::tr("Its polygons (and the other picked polygroups') out of this mesh's snap mask")); }
      menu.addSeparator();
      act(QObject::tr("✎ Rename (F2)"), [tw, it] { tw->editItem(it, 0); });
      act(QObject::tr("▣ Group the picked (Ctrl+G)"), groupPicked, false, QObject::tr("The polygroups picked in the list (Ctrl / Shift — several) into a folder"));
      act(QObject::tr("⧉ Duplicate"), [vp, id, g] { vp->tgDuplicate(id, g); });
      act(QObject::tr("✕ Delete"), [vp, id, g] { vp->tgDelete(id, g); }, GG < 2);
    }
    menu.exec(tw->viewport()->mapToGlobal(p));
  });
  v->addWidget(tw);
  // ---- the polygroups' actions: up, down (Shift — to the end), duplicate, a new empty one, the selected polygons into the
  // picked one, delete ----
  {
    auto cur = [tw]() -> QTreeWidgetItem* { QTreeWidgetItem* c = tw->currentItem(); if (c && c->isSelected()) return c; auto s = tw->selectedItems(); return s.isEmpty() ? nullptr : s.first(); };
    auto pickedGroups = [tw] { std::vector<int> gs; for (auto* it : tw->selectedItems()) { int g = it->data(0, Qt::UserRole + 1).toInt(); if (g >= 0) gs.push_back(g); } return gs; };
    auto glyph = [](const QString& g, const QString& tip) { auto* b = new QPushButton(g); b->setFixedSize(34, 26); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); b->setStyleSheet("font-size:14px;padding:0"); return b; };
    auto* up = glyph("▲", QObject::tr("Move the picked polygroup (or folder) up the list — Shift: to the top"));
    auto* dn = glyph("▼", QObject::tr("Move the picked polygroup (or folder) down the list — Shift: to the bottom"));
    auto* du = glyph("⧉", QObject::tr("Duplicate the picked polygroup (a copy lying on it, a group of its own)"));
    auto* nw = glyph("＋", QObject::tr("A new empty polygroup (under the picked one) — then move polygons into it with ⇲"));
    auto* mv = glyph("⇲", QObject::tr("The polygons selected now (Polygon mode) into the picked polygroup — pick exactly one"));
    auto* rm = glyph("✕", QObject::tr("Delete the picked polygroup and its polygons"));
    auto move = [vp, id, tw, cur](int dir) {
      QTreeWidgetItem* c = cur(); if (!c) { ui::toast(QObject::tr("Pick a polygroup in the list first")); return; }
      const qint64 nid = c->data(0, Qt::UserRole).toLongLong(); const bool far = QApplication::keyboardModifiers() & Qt::ShiftModifier;
      auto t0 = treeFromWidget(nullptr, tw);                        // (from the list as shown: its ids are the rows')
      QTimer::singleShot(0, vp, [vp, id, nid, dir, far, t0] { vp->tgTreeEdit(id, [nid, dir, far, t0](std::vector<topo::TgNode>& t) {
        t = t0; std::vector<topo::TgNode>* arr = nullptr; topo::TgNode* n = topo::tgFind(t, nid, &arr); if (!n) return;
        const long i = long(n - arr->data()), last = long(arr->size()) - 1, j = far ? (dir < 0 ? 0 : last) : std::clamp(i + dir, 0L, last); if (i == j) return;
        topo::TgNode x = std::move((*arr)[size_t(i)]); arr->erase(arr->begin() + i); arr->insert(arr->begin() + j, std::move(x)); }); });
    };
    QObject::connect(up, &QPushButton::clicked, vp, [move] { move(-1); });
    QObject::connect(dn, &QPushButton::clicked, vp, [move] { move(1); });
    QObject::connect(du, &QPushButton::clicked, vp, [vp, id, pickedGroups] { auto gs = pickedGroups(); if (gs.size() != 1) { ui::toast(QObject::tr("Pick one polygroup in the list")); return; } int g = gs[0]; QTimer::singleShot(0, vp, [vp, id, g] { vp->tgDuplicate(id, g); }); });
    QObject::connect(nw, &QPushButton::clicked, vp, [vp, id, pickedGroups] { auto gs = pickedGroups(); int after = gs.empty() ? -1 : gs.back(); QTimer::singleShot(0, vp, [vp, id, after] { vp->tgAddGroup(id, after); }); });
    QObject::connect(mv, &QPushButton::clicked, vp, [vp, id, pickedGroups] { auto gs = pickedGroups(); if (gs.size() != 1) { ui::toast(QObject::tr("Pick exactly one polygroup in the list — the selected polygons go into it")); return; } int g = gs[0]; QTimer::singleShot(0, vp, [vp, id, g] { vp->tgMoveSelHere(id, g); }); });
    QObject::connect(rm, &QPushButton::clicked, vp, [vp, id, pickedGroups] { auto gs = pickedGroups(); if (gs.size() != 1) { ui::toast(QObject::tr("Pick one polygroup in the list")); return; } int g = gs[0]; QTimer::singleShot(0, vp, [vp, id, g] { vp->tgDelete(id, g); }); });
    const auto& ts = vp->topoSel(); mv->setEnabled(ts.id == id && !ts.f.empty()); if (!mv->isEnabled()) mv->setToolTip(mv->toolTip() + QObject::tr(" (select polygons first)"));
    v->addWidget(hrow({up, dn, du, nw, mv, rm}));
    // (every polygroup picked in the list / none — the Ctrl+Shift picks too)
    auto* sa = btn(QObject::tr("Select all")), *sn = btn(QObject::tr("Deselect all"));
    sa->setToolTip(QObject::tr("Every polygroup picked (for the snap mask, Isolate, …)")); sn->setToolTip(QObject::tr("No polygroup picked — in the list or with Ctrl+Shift"));
    QObject::connect(sa, &QPushButton::clicked, vp, [tw] { tw->selectAll(); });
    QObject::connect(sn, &QPushButton::clicked, vp, [vp, id, tw] { { QSignalBlocker b(tw); tw->clearSelection(); }
      vp->tgList = {}; if (vp->tgPick.id == id) vp->tgPick = {}; vp->tgSel = {}; vp->topoChanged(); emit vp->topoState(); });
    // (Show all: every polygroup of the mesh shown — its eyes all open)
    auto* shw = btn(QObject::tr("Show all")); shw->setToolTip(QObject::tr("Every polygroup of this mesh shown (Alt on an eye: only that one)"));
    shw->setEnabled(!hid.empty());
    QObject::connect(shw, &QPushButton::clicked, vp, [vp, id] { const Item* mm = vp->scene()->item(id); if (!mm) return; const int GG = int(baseOf(*mm)->groupNames.size()); std::vector<int> all(static_cast<size_t>(GG), 0); std::iota(all.begin(), all.end(), 0);
      QTimer::singleShot(0, vp, [vp, id, all] { vp->tgSetVisible(id, all, true); vp->runEdit([](Scene&, Selection&, const CurveCache&) { return QString(); }, false); }); });
    v->addWidget(hrow({sa, sn, shw}));
  }
  if (vp->tgRen.id == id) { qint64 n = vp->tgRen.n; vp->tgRen = {}; QTimer::singleShot(0, tw, [tw, n] { QTreeWidgetItemIterator it(tw); while (*it) { if ((*it)->data(0, Qt::UserRole).toLongLong() == n) { tw->editItem(*it, 0); break; } ++it; } }); }

  topoVisBlock(v, vp, sc, id);                                  // (the visibility layers — under the polygroups)
  // ---- snapping: the mesh as a reference, the mask made, the mask brush; the mask's settings under them ----
  const int cnt0 = vp->smCount(id); const bool smOff = m->raw.value("smOff").toBool(); auto pk = vp->smPickedGroups(id);
  if (foldSec(v, vp, "snap", QObject::tr("Snapping"))) {
  { const bool ref = m->raw.value("tref").toBool();
    auto* rb = picBtn(ui::icon("mesh_ref"), QObject::tr("Use as a reference: vertices of the other meshes snap onto it; it can't be picked in the viewport (only here, in the outliner)"), ref);
    QObject::connect(rb, &QPushButton::clicked, vp, [vp, id, ref] { rawEdit(vp, id, [ref](QJsonObject& r) { if (!ref) r.insert("tref", true); else r.remove("tref"); }); });
    const bool gh = m->raw.value("tghost").toBool();
    auto* gb = picBtn(ui::icon("topo_ghost"), QObject::tr("Topology Ghost: this mesh's topology laid onto the meshes being retopologised (the references) — a ghost of its polygons to build by (the Reveal Brush, PolyPen snapping); it works with this mesh hidden too"), gh);
    QObject::connect(gb, &QPushButton::clicked, vp, [vp, id, gh] { rawEdit(vp, id, [gh](QJsonObject& r) { if (!gh) r.insert("tghost", true); else r.remove("tghost"); }); vp->ghostChanged(); emit vp->topoState();
      ui::toast(gh ? QObject::tr("Topology Ghost off") : QObject::tr("Topology Ghost on — its polygons shown on the references; build with the Reveal Brush")); });
    // (the mask button: the mask on / off — on, its menu opens under the row)
    const bool mOn = !smOff && (cnt0 > 0 || m->raw.value("smOn").toBool());
    auto* mb = picBtn(cnt0 ? greenLit("mask_create") : ui::icon("mask_create"), (mOn ? QObject::tr("Snap mask on — vertices snap only to its polygons; click: off (kept, the whole mesh snapped to)")
                                                                                   : QObject::tr("Snap mask off — vertices snap to the whole mesh; click: on, its menu opens"))
                                                                     + (cnt0 ? QObject::tr(" · green: this mesh has a mask (%1 polygons)").arg(cnt0) : QString()), mOn);
    QObject::connect(mb, &QPushButton::clicked, vp, [vp, id, mOn] { rawEdit(vp, id, [mOn](QJsonObject& r) { if (mOn) { r.insert("smOff", true); r.remove("smOn"); } else { r.remove("smOff"); r.insert("smOn", true); } }); });
    if (sc->room != "topo") { gb->deleteLater(); v->addWidget(hrow({rb, mb})); } else v->addWidget(hrow({rb, gb, mb})); }   // (the Hair room: the mask is painted in the Topology room)
  const bool mOn = !smOff && (cnt0 > 0 || m->raw.value("smOn").toBool());
  v->addWidget(muted(!mOn ? (cnt0 ? QObject::tr("Mask off (%1 polygon%2 kept) — vertices snap to the whole mesh.").arg(cnt0).arg(cnt0 == 1 ? "" : "s") : QObject::tr("No mask — vertices snap to the whole mesh."))
                          : cnt0 ? QObject::tr("%1 polygon%2 — vertices snap only to them (green while shown)").arg(cnt0).arg(cnt0 == 1 ? "" : "s") : QObject::tr("The mask is empty — paint it, or add polygroups")));
  if (mOn) {
    auto* sh = btn(QObject::tr("Show mask"), QObject::tr("Show the mask (green) while its mesh is selected or Snap is on")); sh->setCheckable(true); sh->setChecked(vp->smo.show);
    QObject::connect(sh, &QPushButton::clicked, vp, [vp](bool x) { vp->smo.show = x; vp->saveTopoPrefs(); vp->topoChanged(); });
    v->addWidget(sh);
    { auto* mp = btn(QObject::tr("Mirror +X → −X")), *mn = btn(QObject::tr("Mirror −X → +X"));
      for (auto [b, d] : {std::pair{mp, 1}, std::pair{mn, -1}}) { b->setEnabled(cnt0); b->setToolTip(QObject::tr("The mask of one side copied mirrored onto the other (across the mesh's X = 0); the other side's own mask is replaced")); int dd = d; QObject::connect(b, &QPushButton::clicked, vp, [vp, id, dd] { vp->smMirror(id, dd); }); }
      v->addWidget(hrow({mp, mn})); }
    auto* cl = btn(QObject::tr("Clear mask")); cl->setEnabled(cnt0); cl->setToolTip(QObject::tr("Every polygon taken off the mask")); QObject::connect(cl, &QPushButton::clicked, vp, [vp, id] { vp->smClear(id, false); }); v->addWidget(cl);
    { auto* ga = btn(QObject::tr("+ Selected polygroups")), *gr = btn(QObject::tr("− Selected polygroups"));
      ga->setToolTip(QObject::tr("The picked polygroups added to the mask (Ctrl+Shift in the Object mode, or in the list)")); gr->setToolTip(QObject::tr("The picked polygroups taken off the mask"));
      QObject::connect(ga, &QPushButton::clicked, vp, [vp, id] { vp->smGroups(id, true); }); QObject::connect(gr, &QPushButton::clicked, vp, [vp, id] { vp->smGroups(id, false); });
      v->addWidget(hrow({ga, gr})); }
    auto* hd = btn(QObject::tr("− Hidden parts")); hd->setEnabled(cnt0); hd->setToolTip(QObject::tr("The hidden parts of the mesh (hidden polygroups, polygons hidden with H) taken off the mask"));
    QObject::connect(hd, &QPushButton::clicked, vp, [vp, id] { vp->smHiddenOff(id); }); v->addWidget(hd);
    if (sc->room == "topo") {                                    // (the brush's settings, then the brush)
      v->addWidget(muted(QObject::tr("Mask brush")));
      { auto* bm = btn(QObject::tr("Brush")), *lm = btn(QObject::tr("Lasso"));   // (what a drag of the mask brush does)
        bm->setToolTip(QObject::tr("The mask brush paints a circle (Ctrl erases); Shift + drag — a lasso still")); lm->setToolTip(QObject::tr("The mask brush draws a lasso: what the camera sees inside it painted (Ctrl — erased)"));
        for (auto* b : {bm, lm}) b->setCheckable(true); bm->setChecked(!vp->smo.lasso); lm->setChecked(vp->smo.lasso);
        QObject::connect(bm, &QPushButton::clicked, vp, [vp] { vp->smo.lasso = false; vp->saveTopoPrefs(); emit vp->topoState(); });
        QObject::connect(lm, &QPushButton::clicked, vp, [vp] { vp->smo.lasso = true; vp->saveTopoPrefs(); emit vp->topoState(); });
        v->addWidget(hrow({bm, lm})); }
      auto* th = check(QObject::tr("Paint through (hidden parts too)"), vp->smo.thru, QObject::tr("The brush also paints what the camera doesn't see — the back side, layers under others (the lasso — only what is seen). Off — only what is seen"));
      QObject::connect(th, &QCheckBox::toggled, vp, [vp](bool x) { vp->smo.thru = x; vp->saveTopoPrefs(); }); v->addWidget(th);
      auto* cov = check(QObject::tr("Leave out the covered parts"), vp->smo.cover, QObject::tr("A polygon with another surface of this mesh in front of it (along its normal, within the distance) is left out — the hidden part of a group under another layer"));
      QObject::connect(cov, &QCheckBox::toggled, vp, [vp](bool x) { vp->smo.cover = x; vp->saveTopoPrefs(); QTimer::singleShot(0, vp, [vp] { emit vp->brushChanged(); }); }); v->addWidget(cov);
      if (vp->smo.cover) { auto* d = new SliderRow(QObject::tr("Cover distance"), 0.002, 0.1, 0.001, vp->smo.dist, [](double x) { return QString::number(x * 100, 'f', 1) + "%"; });
        d->setTip(QObject::tr("How far in front of a polygon (a share of the mesh's size) a covering layer is looked for"));
        QObject::connect(d, &SliderRow::changed, vp, [vp](double x) { vp->smo.dist = x; }); QObject::connect(d, &SliderRow::released, vp, [vp](double) { vp->saveTopoPrefs(); }); v->addWidget(d); }
      auto* bb = picBtn(ui::icon("brush_paint"), QObject::tr("Snap mask brush: LMB adds, Ctrl erases ([ / ] — size); Shift + drag — lasso adding, Shift + Ctrl — lasso erasing"), vp->tool() == "tmask");
      QObject::connect(bb, &QPushButton::clicked, vp, [vp] { vp->setTool(vp->tool() == "tmask" ? "cursor" : "tmask"); });
      v->addWidget(hrow({bb})); }
  }
  }   // (snapping)

  // ---- display: wire, textures (double click — their manager), the material (double click — its settings); smooth
  // shading, flat shading, the silhouette ----
  if (foldSec(v, vp, "disp", QObject::tr("Display"))) {

    const bool wireless = meshmods::wireless(*m), wireOn = !wireless && m->raw.value("twire").toBool(false), hasUV = looks::hasUV(*m);
    const bool texOn = hasUV && looks::lookOf(*m).value("tex").toBool(), matOn = m->raw.value("mat").isObject() && !m->raw.value("matOff").toBool();
    { auto* tr = new SliderRow(QObject::tr("Transparency"), 0, 0.9, 0.05, 1 - std::clamp(m->raw.value("meshOp").toDouble(1.0), 0.1, 1.0), [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; });   // (the high-poly seen through while the retopology is built — never all the way)
      tr->setTip(QObject::tr("See through this mesh (the Topology room) — up to 90%, so it never disappears")); tr->setReset(0);
      QObject::connect(tr, &SliderRow::changed, vp, [vp, id](double x) { if (Item* it = vp->scene()->item(id)) { if (x < 0.005) it->raw.remove("meshOp"); else it->raw.insert("meshOp", 1 - std::clamp(x, 0.0, 0.9)); } vp->requestUpdate(); });
      QObject::connect(tr, &SliderRow::released, vp, [vp](double) { vp->runEdit([](Scene&, Selection&, const CurveCache&) { return QString(); }, false); });
      v->addWidget(tr); }
    auto* wb = picBtn(ui::icon("disp_wire"), wireless ? QObject::tr("Wireframe — too many polygons to draw it") : QObject::tr("Wireframe: the mesh's edges over it (👁 Display → Wire switches all of them)"), wireOn);
    wb->setEnabled(!wireless);
    QObject::connect(wb, &QPushButton::clicked, vp, [vp, id, wireOn] { rawEdit(vp, id, [wireOn](QJsonObject& r) { if (!wireOn) r.insert("twire", true); else r.remove("twire"); }); });
    auto* tb = picBtn(ui::icon("disp_tex"), hasUV ? QObject::tr("Textures: the mesh's texture maps — double click or RMB: the textures' manager (colour / opacity maps of every material, the folder)")
                                                  : QObject::tr("Textures — no UVs in the file, no textures"), texOn);
    tb->setEnabled(hasUV);
    QObject::connect(tb, &QPushButton::clicked, vp, [vp, id, texOn] { lookChanged(vp, {id}, [on = !texOn](QJsonObject& L) { L.insert("tex", on); }); vp->topoDisplayChanged(); });
    tb->dblKey = QString("tex:%1").arg(id);
    // (the windows opened from the main window, found when asked: the panel is not in it yet as it is made — and is made
    // again by the first click)
    auto mainWin = [] { for (QWidget* w : QApplication::topLevelWidgets()) if (qobject_cast<QMainWindow*>(w)) return w; return static_cast<QWidget*>(nullptr); };
    tb->dbl = [vp, sc, id, mainWin] { lookChanged(vp, {id}, [](QJsonObject& L) { L.insert("tex", true); }); vp->topoDisplayChanged(); openTexWindow(mainWin(), vp, sc, id); };
    auto* mb = picBtn(ui::icon("disp_mat"), QObject::tr("Custom material: a preset tuned here instead of the plain grey — double click or RMB: its settings (preset, colour, roughness, metal, specular)"), matOn);
    auto matSet = [vp, id](bool on) { rawEdit(vp, id, [on](QJsonObject& r) {
      if (!on) { r.insert("matOff", true); return; }               // (switched off: its settings kept)
      r.remove("matOff"); if (r.value("mat").isObject()) return;
      QString k = r.value("matPreset").toString(); const MatP* p = &MATS[0];   // (first on: Sculpt clay) for (const MatP& x : MATS) if (k == x.name) p = &x;
      r.insert("matPreset", p->name); r.insert("mat", matOf(*p)); }); };
    QObject::connect(mb, &QPushButton::clicked, vp, [matSet, matOn] { matSet(!matOn); });
    mb->dblKey = QString("mat:%1").arg(id);
    mb->dbl = [vp, id, mainWin, matSet] {                              // (on, if the first click switched it off; its settings after the panel is made again)
      if (const Item* it = vp->scene()->item(id); it && (!it->raw.value("mat").isObject() || it->raw.value("matOff").toBool())) matSet(true);
      QTimer::singleShot(120, vp, [vp, id] { openMaterialWindow(vp, id); }); };
    v->addWidget(hrow({wb, tb, mb}));
    const bool smooth = m->raw.value("smoothN").toBool(), unlit = m->raw.value("unlit").toBool();
    auto* sb = picBtn(ui::icon("disp_smooth"), QObject::tr("Smooth shading: the normals averaged at the vertices (only how it looks)"), smooth && !unlit);
    auto* fb = picBtn(ui::icon("disp_flat"), QObject::tr("Flat shading: every polygon flat (only how it looks)"), !smooth && !unlit);
    auto* xb = picBtn(ui::icon("disp_silhouette"), QObject::tr("Flat: the silhouette — its colour, no light: the shape's outline only"), unlit);
    QObject::connect(sb, &QPushButton::clicked, vp, [vp, id] { rawEdit(vp, id, [](QJsonObject& r) { r.insert("smoothN", true); r.remove("unlit"); }); });
    QObject::connect(fb, &QPushButton::clicked, vp, [vp, id] { rawEdit(vp, id, [](QJsonObject& r) { r.insert("smoothN", false); r.remove("unlit"); }); });
    QObject::connect(xb, &QPushButton::clicked, vp, [vp, id] { rawEdit(vp, id, [](QJsonObject& r) { r.insert("unlit", true); }); });
    v->addWidget(hrow({sb, fb, xb}));
    const bool pg = m->raw.value("pgView").toBool();
    auto* pb = picBtn(ui::icon("disp_pgview"), QObject::tr("Polygroups: each polygroup in its own colour (only how it looks)"), pg);
    QObject::connect(pb, &QPushButton::clicked, vp, [vp, id, pg] { rawEdit(vp, id, [pg](QJsonObject& r) { if (pg) r.remove("pgView"); else r.insert("pgView", true); }); vp->topoDisplayChanged(); });
    // (the UV checker / stretch on the object — in every mode)
    auto* c3 = picBtn(ui::icon("display_uvchecker"), QObject::tr("UV Checker on the object: the labelled checker by its UV on the selected mesh — in every mode (the UV Editor has its own)"), vp->uvo.c3d);
    QObject::connect(c3, &QPushButton::clicked, vp, [vp] { vp->uvo.c3d = !vp->uvo.c3d; vp->saveUvPrefs(); vp->uvChanged(); emit vp->topoState(); });
    auto* s3 = picBtn(ui::icon("display_uvstretch"), QObject::tr("UV Stretch on the object: each polygon tinted by its UV area to its 3D area against the mesh's — red squeezed, blue stretched (the scale: the UV window) — in every mode"), vp->uvo.s3d);
    QObject::connect(s3, &QPushButton::clicked, vp, [vp] { vp->uvo.s3d = !vp->uvo.s3d; vp->saveUvPrefs(); vp->uvChanged(); emit vp->topoState(); });
    v->addWidget(hrow({pb, c3, s3}));
  }
  // ---- the head ----
  { auto* hb = picBtn(ui::icon("mesh_head"), QObject::tr("Mark this mesh as the head"), m->headTag, 53);
    QObject::connect(hb, &QPushButton::clicked, vp, [vp, id, on = !m->headTag] { rawEdit(vp, id, [on](QJsonObject& r) { r.insert("headTag", on); }); vp->refreshSceneBox(); vp->sceneChanged(false); });
    auto* l = new QLabel(m->headTag ? QObject::tr("<b>The head</b> — this mesh is a part of the Head: in the Hair room the hair is built over it (the brush, the curves and the walls keep off it)")
                                    : QObject::tr("<b>Head</b> — mark this mesh as the head: in the Hair room every mesh marked so is the Head the hair is built over"));
    l->setWordWrap(true); l->setTextFormat(Qt::RichText); l->setStyleSheet("color:#9a9a9a;font-size:11px");
    auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 10, 0, 2); h->setSpacing(8); h->addWidget(hb, 0, Qt::AlignTop); h->addWidget(l, 1); v->addWidget(w); }
  // ---- selection sets: polygons, edges, vertices — the pointer on one lights it, a click picks it (Shift adds, Ctrl
  // takes off), RMB deletes it; made with Shift+RMB → Selection Set ----
  if (foldSec(v, vp, "selsets", QObject::tr("Selection sets"))) {
    const QJsonArray sets = m->raw.value("selSets").toArray();
    if (sets.isEmpty()) v->addWidget(muted(QObject::tr("None yet — select polygons, edges or vertices, then Shift+RMB → Selection Set.")));
    for (auto [kc, title] : {std::pair<char, const char*>{'f', "Polygons"}, {'e', "Edges"}, {'v', "Vertices"}}) {
      std::vector<int> idx; for (int i = 0; i < sets.size(); i++) if (sets[i].toObject().value("k").toString() == QString(QChar(kc))) idx.push_back(i);
      if (idx.empty()) continue;
      v->addWidget(muted(QObject::tr(title)));
      auto* row = new QWidget; auto* g = new QGridLayout(row); g->setContentsMargins(0, 0, 0, 4); g->setSpacing(4); int col = 0, r = 0;
      for (int i : idx) {
        const QJsonObject s = sets[i].toObject(); auto* b = new SetBtn; b->setProperty("tool", true); b->setFixedSize(40, 40); b->setIcon(ui::icon("top_selset")); b->setIconSize(QSize(34, 34)); b->setFocusPolicy(Qt::NoFocus);
        char k = 0; const bool ok = !vp->tSelSetList(id, i, &k).empty();
        b->setToolTip(QObject::tr("%1 — click: select it, Shift: add, Ctrl: take off; RMB: delete").arg(s.value("name").toString()) + (ok ? QString() : QObject::tr("\n(made for other geometry — the mesh changed since)")));
        if (!ok) b->setStyleSheet("QPushButton{background:#3a2a2a}");   // (stale: made for other geometry)
        else { auto lit = [vp, id, i, b] { const bool on = vp->tSelSetTouched(id, i); if (b->property("lit").toBool() == on) return; b->setProperty("lit", on);
                 b->setStyleSheet(on ? "QPushButton{background:#34405a;border:1px solid #5d7bb0}" : QString()); };   // (a picked element in it: the set's button pale lit)
          lit(); QObject::connect(vp, &Viewport::topoState, b, lit); }
        b->hover = [vp, id, i](bool on) { vp->setSsHover(on ? id : -1, on ? i : -1); };
        b->del = [vp, id, i] { vp->setSsHover(-1, -1); vp->tSelSetDelete(id, i); };
        QObject::connect(b, &QPushButton::clicked, vp, [vp, id, i] { const auto mods = QApplication::keyboardModifiers(); vp->setSsHover(-1, -1);
          vp->tSelSetApply(id, i, (mods & Qt::ControlModifier) ? "remove" : (mods & Qt::ShiftModifier) ? "add" : "set"); });
        g->addWidget(b, r, col); if (++col == 5) { col = 0; r++; }
      }
      g->setColumnStretch(5, 1); v->addWidget(row);
    }
  }
  topoUvBlocks(v, vp, sc, id);                                   // (the blocks added later: above the transform)
  topoGhostBlock(v, vp, sc, id);                                // (a ghost source: its settings)
  // ---- the clean-up: the mesh made manifold (as a conversion leaves it now) — for the meshes made before ----
  if (foldSec(v, vp, "clean", QObject::tr("Clean-up"))) {
    auto* cb = btn(QObject::tr("🧹 Clean the geometry"), QObject::tr("Make the mesh manifold, as the Hair room's conversion now leaves it: an edge of three polygons or more split into pairs that carry on into each other, a vertex where parts only touch split, doubled polygons out — nothing else changes. All the selected meshes"));
    QObject::connect(cb, &QPushButton::clicked, vp, [vp, id] { std::vector<int> ms = vp->selection().items; if (std::find(ms.begin(), ms.end(), id) == ms.end()) ms.push_back(id);
      vp->runEdit([ms](Scene& s, Selection&, const CurveCache&) { int fixed = 0, changes = 0;
        for (int m : ms) { Item* x = s.item(m); if (!x || !x->poly || (x->type != ItemType::IMesh && x->type != ItemType::Mesh)) continue; auto src = x->base ? x->base : x->poly; auto p = std::make_shared<PolyMesh>(*src); const int n = tedit::makeManifold(*p); if (!n) continue;
          const bool sameShown = x->poly == src; if (x->base) x->base = p; if (sameShown || !x->base) x->poly = p; fixed++; changes += n; }
        return fixed ? QObject::tr("Clean geometry: %1 mesh%2 made manifold (%3 changes)").arg(fixed).arg(fixed == 1 ? "" : "es").arg(changes) : QObject::tr("Clean geometry: already manifold — nothing to clean"); }); });
    v->addWidget(cb);
  }
  // ---- the Hair room: the collision — what the drawn curves lie on and keep off ----
  if (vp->room() == "hair") { const bool open = foldSec(v, vp, "coll", QObject::tr("Collision"), true, "collision"); vp->showColl = open;
    if (open) { const Item* cm = sc->item(id); const QJsonObject co = cm->raw.value("coll").toObject(); const bool head = cm->headTag, on = cm->raw.value("collide").toBool();
      if (head) v->addWidget(muted(QObject::tr("The head: drawn curves always lie on it while it is shown")));
      else { auto* cb = new QCheckBox(QObject::tr("⛨ Collision on")); cb->setChecked(on); cb->setToolTip(QObject::tr("Drawn curves stick to this mesh like to the head and go round it — even while it is hidden (⛨ in the outliner)"));
        QObject::connect(cb, &QCheckBox::toggled, vp, [vp, id](bool x) { rawEdit(vp, id, [x](QJsonObject& r) { r.insert("collide", x); }); }); v->addWidget(cb); }
      auto setC = [vp, id](const QString& k, const QJsonValue& x) { rawEdit(vp, id, [k, x](QJsonObject& r) { QJsonObject c = r.value("coll").toObject(); c.insert(k, x); r.insert("coll", c); }); };
      auto* ty = new QComboBox; ty->addItem(QObject::tr("Its geometry, lifted"), "offset"); ty->addItem(QObject::tr("A capsule round it"), "capsule"); ty->addItem(QObject::tr("Another mesh"), "custom");
      ty->setCurrentIndex(std::max(0, ty->findData(Viewport::collType(*cm))));
      ty->setToolTip(QObject::tr("The shape the curves lie on: the mesh itself (a copy lifted by the distance below), a capsule round all of it (simple, smooth — shown in orange), or another mesh standing for it (a low proxy; it may be hidden)"));
      QObject::connect(ty, &QComboBox::currentIndexChanged, vp, [vp, id, ty, setC](int) { const QString k = ty->currentData().toString(); setC("type", k);
        if (k == "capsule" && vp->capsuleItem(id) < 0) { vp->makeCapsuleItem(id, true); vp->sceneChanged(true); } });   // (the capsule: a mesh of its own — placed with the gizmo)
      auto* tr0 = new QWidget; auto* th = new QHBoxLayout(tr0); th->setContentsMargins(0, 0, 0, 0); th->addWidget(new QLabel(QObject::tr("Shape"))); th->addWidget(ty, 1); v->addWidget(tr0);
      if (Viewport::collType(*cm) == "custom") {
        auto* mc = new QComboBox; mc->addItem(QObject::tr("— pick a mesh —"), -1);
        for (auto& [oid, o] : sc->items) if (oid != id && (o.type == ItemType::IMesh || o.type == ItemType::Mesh) && o.poly) mc->addItem(o.name, oid);
        mc->setCurrentIndex(std::max(0, mc->findData(co.value("mesh").toInt(-1))));
        QObject::connect(mc, &QComboBox::currentIndexChanged, vp, [mc, setC](int) { setC("mesh", mc->currentData().toInt()); });
        auto* mr = new QWidget; auto* mh = new QHBoxLayout(mr); mh->setContentsMargins(0, 0, 0, 0); mh->addWidget(new QLabel(QObject::tr("Mesh"))); mh->addWidget(mc, 1); v->addWidget(mr); }
      auto* d = new QDoubleSpinBox; d->setRange(0, 100); d->setDecimals(1); d->setSingleStep(0.5); d->setSuffix(QObject::tr(" mm")); d->setKeyboardTracking(false); d->setValue(Viewport::collOffset(*cm) * 10);
      d->setToolTip(QObject::tr("How far off the shape the curves lie (the scene in centimetres: 2 mm — 0.2)"));
      QObject::connect(d, &QDoubleSpinBox::valueChanged, vp, [setC](double x) { setC("d", x / 10.0); });
      auto* dr = new QWidget; auto* dh = new QHBoxLayout(dr); dh->setContentsMargins(0, 0, 0, 0); dh->addWidget(new QLabel(QObject::tr("Lift off it"))); dh->addWidget(d, 1); v->addWidget(dr);
      if (Viewport::collType(*cm) == "capsule") {                  // (the capsule fitted round the mesh, then made thinner / shorter / moved along its axis)
        auto num = [&](const QString& label, const QString& key, double def, double lo, double hi, double k, int dec, const QString& suffix, const QString& tip) {
          auto* s = new QDoubleSpinBox; s->setRange(lo, hi); s->setDecimals(dec); s->setSingleStep(dec ? 0.5 : 5); s->setSuffix(suffix); s->setKeyboardTracking(false); s->setToolTip(tip);
          s->setValue(co.value(key).toDouble(def) * k);
          QObject::connect(s, &QDoubleSpinBox::valueChanged, vp, [vp, id, setC, key, k](double x) { setC(key, x / k); if (vp->capsuleItem(id) >= 0) { vp->makeCapsuleItem(id, false); vp->sceneChanged(true); } });
          auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->addWidget(new QLabel(label)); h->addWidget(s, 1); v->addWidget(w); };
        num(QObject::tr("Radius"), "rs", 100, 2, 300, 1, 0, " %", QObject::tr("The capsule's radius — a share of the fitted one (100 % holds every point of the mesh)"));
        num(QObject::tr("Cap stretch"), "cs", 100, 2, 400, 1, 0, " %", QObject::tr("How far the round ends reach along the axis — a share of the radius (100 % — half-spheres, less — flatter, more — pointed)"));
        num(QObject::tr("Width"), "fu", 100, 2, 300, 1, 0, " %", QObject::tr("Squashed or widened across — along the mesh's second spread"));
        num(QObject::tr("Depth"), "fw", 100, 2, 300, 1, 0, " %", QObject::tr("Squashed or widened across — along its third spread"));
        num(QObject::tr("Cylinder height"), "hs", 100, 0, 300, 1, 0, " %", QObject::tr("The straight part between the round ends — a share of the fitted one (0 — the ends meet)"));
        num(QObject::tr("Shift"), "sh", 0, -1000, 1000, 10, 1, QObject::tr(" mm"), QObject::tr("The capsule moved along its own axis"));
        auto* rs = btn(QObject::tr("↺ Reset the capsule"), QObject::tr("Back to the automatic fit round the head: everything 100 %, no shift, its moves / turns / scale dropped"));
        QObject::connect(rs, &QPushButton::clicked, vp, [vp, id] { rawEdit(vp, id, [](QJsonObject& r) { QJsonObject c = r.value("coll").toObject(); for (const char* k : {"rs", "ls", "cs", "fu", "fw", "hs", "sh"}) c.remove(k); r.insert("coll", c); });
          vp->makeCapsuleItem(id, true); vp->sceneChanged(true); });
        const int cap = vp->capsuleItem(id);
        auto* sel = btn(cap >= 0 ? QObject::tr("◎ Select the capsule — move / turn / scale it") : QObject::tr("◆ Make the capsule placeable"),
                        QObject::tr("The capsule is a mesh of its own: select it, then Move (E), Rotate (R), Scale (T) — the curves lie on it where you put it (it is not drawn — its rings in orange)"));
        QObject::connect(sel, &QPushButton::clicked, vp, [vp, id] { int c = vp->capsuleItem(id); if (c < 0) { c = vp->makeCapsuleItem(id, true); vp->sceneChanged(true); } if (c >= 0) QTimer::singleShot(0, vp, [vp, c] { vp->setSelectedItems({c}); }); });
        v->addWidget(sel); v->addWidget(rs); } } }
  // ---- the transform: the very last block ----
  v->addWidget(sec(QObject::tr("Transform")));
  { Vec3 c = vp->meshPivotLocal(id); double k = unitK(vp->tunit);
    auto* lab = muted(QObject::tr("Pivot %1 · %2 · %3 %4").arg(fmt3(c.x / k), fmt3(c.y / k), fmt3(c.z / k), vp->tunit));
    auto* toC = btn(QObject::tr("Centre"), QObject::tr("The pivot to the middle of the mesh (its box)")), *toO = btn(QObject::tr("Origin"), QObject::tr("The pivot to the file's origin (0, 0, 0)"));
    QObject::connect(toC, &QPushButton::clicked, vp, [vp, id] { const PolyMesh* BB = baseOf(*vp->scene()->item(id)); vp->setMeshPivot(id, BB->bounds().center()); });
    QObject::connect(toO, &QPushButton::clicked, vp, [vp, id] { vp->setMeshPivot(id, Vec3()); });
    auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 0, 0, 0); h->addWidget(lab, 1); h->addWidget(toC); h->addWidget(toO); v->addWidget(r); }
  int xid = -1; for (int x : meshmods::modsOf(*sc, id)) if (sc->item(x)->raw.value("kind").toString() == "xform") xid = x;
  if (xid < 0) v->addWidget(muted(QObject::tr("As imported — Move / Rotate / Scale or the transform table make one.")));
  else {
    const Item* x = sc->item(xid); QJsonObject q = x->raw.value("p").toObject();
    auto f3 = [&](const char* a, const char* b, const char* c, double d) { return QString("%1 · %2 · %3").arg(fmt3(q.value(a).toDouble(d)), fmt3(q.value(b).toDouble(d)), fmt3(q.value(c).toDouble(d))); };
    auto* info = muted(QObject::tr("Position %1\nRotation %2°\nScale %3").arg(f3("px", "py", "pz", 0), f3("rx", "ry", "rz", 0), f3("sx", "sy", "sz", 1))); v->addWidget(info);
    auto* on = btn(x->visible ? QObject::tr("On") : QObject::tr("Off"), QObject::tr("Switch the transform off / on (the mesh as imported while off)")); on->setIcon(ui::eyeIcon(x->visible)); on->setCheckable(true); on->setChecked(x->visible);
    QObject::connect(on, &QPushButton::clicked, vp, [vp, xid] { vp->runEdit([xid](Scene& s, Selection&, const CurveCache&) { Item* xi = s.item(xid); xi->visible = !xi->visible; return QString(); }, false); });
    auto* rs = btn(QObject::tr("Reset"), QObject::tr("Back to the imported position, rotation and size"));
    QObject::connect(rs, &QPushButton::clicked, vp, [vp, xid] { vp->runEdit([xid](Scene& s, Selection&, const CurveCache&) { Item* xi = s.item(xid); QJsonObject p = xi->raw.value("p").toObject();
      for (const char* k : {"px", "py", "pz", "rx", "ry", "rz"}) p.insert(k, 0); for (const char* k : {"sx", "sy", "sz"}) p.insert(k, 1); xi->raw.insert("p", p); return QString(); }, false); });
    auto* bk = btn(QObject::tr("Bake"), heavy ? QObject::tr(HEAVY) : QObject::tr("Put the transform into the geometry: the points move where they are shown, the transform goes (one undo step)")); bk->setProperty("primary", true);
    bk->setEnabled(x->visible && !heavy); QObject::connect(bk, &QPushButton::clicked, vp, [vp, id] { vp->topoBakeXform(id); });
    v->addWidget(hrow({on, rs, bk}));
  }
}

}  // namespace hm
