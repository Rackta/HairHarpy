// The material builder's window (the Paint room): the graph drawn as boxes joined by lines, as in the engines' material editors.
// Drag from a node's output (the dot on its right) onto an input — joined; drag a joined input away — parted. RMB on the empty
// sheet — a new node; RMB on a node — its values, delete; a double click — its own value (the map, the colour, the number).
// Every change is worked out at once and shown on the model while Display is Material.
#include "app/MaterialEditor.h"
#include "app/PaintSession.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include <QColorDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <map>

namespace hm {

using namespace paint;

namespace {
constexpr double NW = 150, HEAD = 22, ROW = 20, RAD = 5.5;

class Editor;
class NodeItem : public QGraphicsItem {
public:
  NodeItem(Editor* e, const MatNode& n) : ed_(e), n_(n) { setPos(n.pos); setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges); setZValue(1); }
  int id() const { return n_.id; }
  int rows() const { const MatNodeType* t = matNodeType(n_.type); return std::max(1, t ? int(t->inputs.size()) : 1); }
  QRectF boundingRect() const override { return QRectF(-RAD - 1, -1, NW + 2 * RAD + 2, HEAD + rows() * ROW + 8); }
  QPointF inPos(int i) const { return pos() + QPointF(0, HEAD + ROW * i + ROW / 2); }
  QPointF outPos() const { return pos() + QPointF(NW, HEAD + ROW / 2); }
  void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override;
protected:
  QVariant itemChange(GraphicsItemChange c, const QVariant& v) override;
private:
  Editor* ed_; MatNode n_;
};

class Editor : public QWidget {
public:
  Editor(QWidget* parent, PaintSession* s, std::function<void()> changed, std::function<void()> show);
  void nodeMoved() { relink(); }
  QString summary(const MatNode& n) const;
  const MatGraph& graph() const { return s_->material; }
protected:
  bool eventFilter(QObject* o, QEvent* e) override;
private:
  PaintSession* s_; std::function<void()> changed_; QGraphicsScene* sc_; QGraphicsView* view_;
  std::map<int, NodeItem*> items_; std::vector<std::pair<MatLink, QGraphicsPathItem*>> links_; QGraphicsPathItem* temp_ = nullptr; int dragFrom_ = -1;
  void rebuild(); void relink(); void commit() { rebuild(); if (changed_) changed_(); }
  static QPainterPath wire(const QPointF& a, const QPointF& b) { QPainterPath p(a); const double dx = std::max(40.0, std::abs(b.x() - a.x()) * 0.5); p.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b); return p; }
  bool socketAt(const QPointF& p, int& node, int& input) const;   // (input −1: the output)
  void savePositions() { for (auto& [id, it] : items_) if (MatNode* n = s_->material.node(id)) n->pos = it->pos(); }
  void edit(int id); void inputValue(int id, int input);
  void addMenu(const QPoint& global, const QPointF& at); void nodeMenu(const QPoint& global, int id);
};

void NodeItem::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) {
  const MatNodeType* t = matNodeType(n_.type); const double h = HEAD + rows() * ROW + 6; p->setRenderHint(QPainter::Antialiasing);
  const bool out = n_.type == "output"; const QColor head = out ? QColor(0x7a, 0x4a, 0x9a) : n_.type == "map" ? QColor(0x2f, 0x6a, 0x8a) : (n_.type == "lambert" || n_.type == "fresnel") ? QColor(0x8a, 0x6a, 0x2f) : QColor(0x4a, 0x4e, 0x58);
  p->setPen(QPen(isSelected() ? QColor(0xff, 0xd8, 0x4a) : QColor(0x16, 0x16, 0x18), isSelected() ? 2 : 1)); p->setBrush(QColor(0x2e, 0x2f, 0x33)); p->drawRoundedRect(QRectF(0, 0, NW, h), 6, 6);
  p->setPen(Qt::NoPen); p->setBrush(head); p->drawRoundedRect(QRectF(1, 1, NW - 2, HEAD - 1), 5, 5);
  p->setPen(Qt::white); QFont f = p->font(); f.setBold(true); p->setFont(f); p->drawText(QRectF(8, 0, NW - 16, HEAD), Qt::AlignVCenter | Qt::AlignLeft, t ? QObject::tr(t->name.toUtf8().constData()) : n_.type); f.setBold(false); p->setFont(f);
  if (t) for (int i = 0; i < t->inputs.size(); i++) { const double y = HEAD + ROW * i; bool joined = false; for (const MatLink& l : ed_->graph().links) if (l.to == n_.id && l.input == i) joined = true;
    p->setPen(QColor(0xc8, 0xc8, 0xc8)); p->drawText(QRectF(12, y, NW - 24, ROW), Qt::AlignVCenter | Qt::AlignLeft, t->inputs[i] + (joined ? QString() : QString("  %1").arg(n_.p.value(QString("d%1").arg(i)).toDouble(t->defaults[size_t(i)]), 0, 'g', 3)));
    p->setPen(QPen(QColor(0x16, 0x16, 0x18), 1)); p->setBrush(joined ? QColor(0x7f, 0xc8, 0xff) : QColor(0x70, 0x70, 0x74)); p->drawEllipse(QPointF(0, y + ROW / 2), RAD, RAD); }
  if (t && t->inputs.isEmpty()) { const QString s = ed_->summary(n_); if (n_.type == "color") { p->setPen(QPen(QColor(0x16, 0x16, 0x18), 1)); p->setBrush(QColor(n_.p.value("color").toString("#c8c8c8"))); p->drawRoundedRect(QRectF(10, HEAD + 3, 26, ROW - 6), 3, 3); }
    p->setPen(QColor(0xc8, 0xc8, 0xc8)); p->drawText(QRectF(n_.type == "color" ? 42 : 12, HEAD, NW - 54, ROW), Qt::AlignVCenter | Qt::AlignLeft, s); }
  if (!out) { p->setPen(QPen(QColor(0x16, 0x16, 0x18), 1)); p->setBrush(QColor(0xff, 0xb0, 0x40)); p->drawEllipse(QPointF(NW, HEAD + ROW / 2), RAD, RAD); }
}
QVariant NodeItem::itemChange(GraphicsItemChange c, const QVariant& v) { if (c == ItemPositionHasChanged) ed_->nodeMoved(); return QGraphicsItem::itemChange(c, v); }

Editor::Editor(QWidget* parent, PaintSession* s, std::function<void()> changed, std::function<void()> show) : QWidget(parent, Qt::Tool), s_(s), changed_(std::move(changed)) {
  setWindowTitle(QObject::tr("Material builder")); resize(1120, 640); setAttribute(Qt::WA_DeleteOnClose);
  auto* v = new QVBoxLayout(this); v->setContentsMargins(6, 6, 6, 6); v->setSpacing(6);
  auto* bar = new QHBoxLayout; auto btn = [&](const QString& t, const QString& tip, std::function<void()> f) { auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); QObject::connect(b, &QPushButton::clicked, this, f); bar->addWidget(b); return b; };
  btn(QObject::tr("＋ Node"), QObject::tr("A new node (RMB on the empty sheet — the same, where the pointer is)"), [this] { addMenu(QCursor::pos(), view_->mapToScene(view_->viewport()->rect().center())); });
  // (the presets: the starting graph, a graph saved and loaded: used once in a while, one menu)
  QMenu* pm = ui::moreMenu(btn(QObject::tr("Presets") + QString::fromUtf8("  \u25BE"), QObject::tr("The starting toon graph; this graph saved into a file; a saved one loaded in its place"), [] {}));
  ui::menuItem(pm, QObject::tr("Toon preset"), QObject::tr("The starting graph: the base colour, tinted and darker where the light doesn't reach — a hard step"), this, [this] { s_->material = MatGraph::toon(); commit(); });
  auto presetDir = [] { const QString d = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/HairHarpy Materials"; QDir().mkpath(d); return d; };
  ui::menuItem(pm, QObject::tr("Save preset…"), QObject::tr("The graph into a file (Documents\\HairHarpy Materials) — to use in another project"), this, [this, presetDir] { savePositions();
    const QString f = QFileDialog::getSaveFileName(this, QObject::tr("Save the material"), presetDir() + "/material.hhmat", QObject::tr("HairHarpy material (*.hhmat)")); if (f.isEmpty()) return;
    QFile o(f); if (o.open(QIODevice::WriteOnly)) { o.write(QJsonDocument(s_->material.toJson()).toJson()); ui::toast(QObject::tr("Material saved")); } });
  ui::menuItem(pm, QObject::tr("Load preset…"), QObject::tr("A saved graph in place of this one"), this, [this, presetDir] {
    const QString f = QFileDialog::getOpenFileName(this, QObject::tr("Load a material"), presetDir(), QObject::tr("HairHarpy material (*.hhmat)")); if (f.isEmpty()) return;
    QFile i(f); if (!i.open(QIODevice::ReadOnly)) return; const QJsonObject j = QJsonDocument::fromJson(i.readAll()).object(); if (j.value("nodes").toArray().isEmpty()) { ui::toast(QObject::tr("Not a material file")); return; }
    s_->material.fromJson(j); commit(); });
  btn(QObject::tr("Show on the model"), QObject::tr("Display → Material (5): the model shows what the graph makes"), show);
  auto* hint = new QLabel(QObject::tr("Drag from an output (the orange dot) onto an input — joined · drag a joined input away — parted · RMB — add / values / delete · double click — the node's own value · Delete — the selected nodes")); hint->setProperty("mut", true); hint->setWordWrap(true);
  bar->addWidget(hint, 1); v->addLayout(bar);
  sc_ = new QGraphicsScene(this); sc_->setBackgroundBrush(QColor(0x22, 0x22, 0x24)); sc_->installEventFilter(this);
  view_ = new QGraphicsView(sc_); view_->setRenderHint(QPainter::Antialiasing); view_->setDragMode(QGraphicsView::RubberBandDrag); view_->setFrameShape(QFrame::NoFrame); v->addWidget(view_, 1);
  if (s_->material.nodes.empty()) s_->material = MatGraph::toon();
  rebuild();
}
QString Editor::summary(const MatNode& n) const {
  if (n.type == "map") { const QString k = n.p.value("map").toString("color"); for (const MapInfo& m : maps()) if (k == m.key) return QObject::tr(m.name); return k; }
  if (n.type == "color") return n.p.value("color").toString("#c8c8c8");
  if (n.type == "value") return QString::number(n.p.value("v").toDouble(0.5), 'g', 4);
  if (n.type == "fresnel") return QObject::tr("power %1").arg(n.p.value("v").toDouble(3), 0, 'g', 3);
  if (n.type == "lambert") return QObject::tr("N · L");
  return {};
}
void Editor::rebuild() {
  sc_->clear(); items_.clear(); links_.clear(); temp_ = nullptr; dragFrom_ = -1;
  for (const MatNode& n : s_->material.nodes) { auto* it = new NodeItem(this, n); sc_->addItem(it); items_[n.id] = it; }
  for (const MatLink& l : s_->material.links) { auto* p = sc_->addPath({}, QPen(QColor(0x9f, 0xd0, 0xff), 2)); p->setZValue(0); links_.push_back({l, p}); }
  relink(); sc_->setSceneRect(sc_->itemsBoundingRect().adjusted(-70, -60, 70, 60));
}
void Editor::relink() { for (auto& [l, p] : links_) { auto a = items_.find(l.from), b = items_.find(l.to); if (a != items_.end() && b != items_.end()) p->setPath(wire(a->second->outPos(), b->second->inPos(l.input))); } }
bool Editor::socketAt(const QPointF& p, int& node, int& input) const {
  for (const auto& [id, it] : items_) { const MatNode* n = s_->material.node(id); const MatNodeType* t = n ? matNodeType(n->type) : nullptr; if (!t) continue;
    if (n->type != "output" && QLineF(p, it->outPos()).length() < 10) { node = id; input = -1; return true; }
    for (int i = 0; i < t->inputs.size(); i++) if (QLineF(p, it->inPos(i)).length() < 10) { node = id; input = i; return true; } }
  return false;
}
void Editor::edit(int id) {
  MatNode* n = s_->material.node(id); if (!n) return;
  if (n->type == "map") { QMenu m(this); for (const MapInfo& mi : maps()) { const QString k = QString::fromLatin1(mi.key); QAction* a = m.addAction(QObject::tr(mi.name)); a->setCheckable(true); a->setChecked(n->p.value("map").toString("color") == k); QObject::connect(a, &QAction::triggered, this, [this, id, k] { if (MatNode* x = s_->material.node(id)) { x->p["map"] = k; commit(); } }); } m.exec(QCursor::pos()); }
  else if (n->type == "color") { const QColor c = QColorDialog::getColor(QColor(n->p.value("color").toString("#c8c8c8")), this); if (c.isValid()) { n->p["color"] = c.name(); commit(); } }
  else if (n->type == "value" || n->type == "fresnel") { bool ok = false; const double v = QInputDialog::getDouble(this, QObject::tr("Material builder"), n->type == "value" ? QObject::tr("Value") : QObject::tr("Power"), n->p.value("v").toDouble(n->type == "value" ? 0.5 : 3), -1000, 1000, 3, &ok); if (ok) { n->p["v"] = v; commit(); } }
  else { const MatNodeType* t = matNodeType(n->type); if (t && !t->inputs.isEmpty()) inputValue(id, 0); }
}
void Editor::inputValue(int id, int input) {
  MatNode* n = s_->material.node(id); const MatNodeType* t = n ? matNodeType(n->type) : nullptr; if (!t || input >= t->inputs.size()) return;
  bool ok = false; const QString key = QString("d%1").arg(input);
  const double v = QInputDialog::getDouble(this, QObject::tr("Material builder"), QObject::tr("%1 — while nothing is joined to it").arg(t->inputs[input]), n->p.value(key).toDouble(t->defaults[size_t(input)]), -1000, 1000, 3, &ok);
  if (ok) { n->p[key] = v; commit(); }
}
void Editor::addMenu(const QPoint& global, const QPointF& at) {
  QMenu m(this); for (const MatNodeType& t : matNodeTypes()) { if (t.key == "output") continue; QAction* a = m.addAction(QObject::tr(t.name.toUtf8().constData())); a->setToolTip(QObject::tr(t.tip.toUtf8().constData())); const QString k = t.key;
    QObject::connect(a, &QAction::triggered, this, [this, k, at] { savePositions(); s_->material.add(k, at); commit(); }); }
  m.setToolTipsVisible(true); m.exec(global);
}
void Editor::nodeMenu(const QPoint& global, int id) {
  const MatNode* n = s_->material.node(id); const MatNodeType* t = n ? matNodeType(n->type) : nullptr; if (!t) return; QMenu m(this);
  if (t->inputs.isEmpty()) QObject::connect(m.addAction(QObject::tr("Change…")), &QAction::triggered, this, [this, id] { edit(id); });
  for (int i = 0; i < t->inputs.size(); i++) QObject::connect(m.addAction(QObject::tr("%1: its value…").arg(t->inputs[i])), &QAction::triggered, this, [this, id, i] { inputValue(id, i); });
  if (n->type != "output") { m.addSeparator(); QObject::connect(m.addAction(QObject::tr("Delete")), &QAction::triggered, this, [this, id] { savePositions(); s_->material.remove(id); commit(); }); }
  m.exec(global);
}
bool Editor::eventFilter(QObject* o, QEvent* e) {
  if (o != sc_) return QWidget::eventFilter(o, e);
  auto nodeUnder = [&](const QPointF& p) { for (QGraphicsItem* it : sc_->items(p)) if (auto* n = dynamic_cast<NodeItem*>(it)) return n->id(); return -1; };
  switch (e->type()) {
    case QEvent::GraphicsSceneMousePress: { auto* m = static_cast<QGraphicsSceneMouseEvent*>(e); if (m->button() != Qt::LeftButton) break; int node, input;
      if (!socketAt(m->scenePos(), node, input)) break;
      if (input >= 0) { int from = -1; for (const MatLink& l : s_->material.links) if (l.to == node && l.input == input) from = l.from; if (from < 0) return true;   // (a joined input pulled: parted, its line in hand)
        savePositions(); s_->material.unlink(node, input); commit(); dragFrom_ = from; } else dragFrom_ = node;
      temp_ = sc_->addPath({}, QPen(QColor(0xff, 0xd8, 0x4a), 2, Qt::DashLine)); return true; }
    case QEvent::GraphicsSceneMouseMove: if (dragFrom_ >= 0 && temp_) { auto* m = static_cast<QGraphicsSceneMouseEvent*>(e); temp_->setPath(wire(items_[dragFrom_]->outPos(), m->scenePos())); return true; } break;
    case QEvent::GraphicsSceneMouseRelease: { auto* m = static_cast<QGraphicsSceneMouseEvent*>(e);
      if (dragFrom_ >= 0) { int node, input; const int from = dragFrom_; dragFrom_ = -1; if (temp_) { sc_->removeItem(temp_); delete temp_; temp_ = nullptr; }
        if (socketAt(m->scenePos(), node, input) && input >= 0) { savePositions(); const size_t was = s_->material.links.size(); s_->material.link(from, node, input);
          if (s_->material.links.size() == was && !s_->material.reaches(from, node)) ui::toast(QObject::tr("That would make a loop: the node would feed itself")); commit(); }
        return true; }
      savePositions(); break; }
    case QEvent::GraphicsSceneMouseDoubleClick: { auto* m = static_cast<QGraphicsSceneMouseEvent*>(e); const int id = nodeUnder(m->scenePos()); if (id >= 0) { int node, input; if (socketAt(m->scenePos(), node, input) && input >= 0) inputValue(node, input); else edit(id); return true; } break; }
    case QEvent::GraphicsSceneContextMenu: { auto* m = static_cast<QGraphicsSceneContextMenuEvent*>(e); const int id = nodeUnder(m->scenePos()); if (id >= 0) nodeMenu(m->screenPos(), id); else addMenu(m->screenPos(), m->scenePos()); return true; }
    case QEvent::KeyPress: if (static_cast<QKeyEvent*>(e)->key() == Qt::Key_Delete) { std::vector<int> ids; for (QGraphicsItem* it : sc_->selectedItems()) if (auto* n = dynamic_cast<NodeItem*>(it)) ids.push_back(n->id());
        if (!ids.empty()) { savePositions(); for (int id : ids) s_->material.remove(id); commit(); return true; } } break;
    default: break;
  }
  return QWidget::eventFilter(o, e);
}

QPointer<Editor> g_ed;
}  // namespace

void openMaterialEditor(QWidget* parent, PaintSession* s, std::function<void()> changed, std::function<void()> show) {
  if (g_ed) { g_ed->raise(); g_ed->activateWindow(); return; }
  g_ed = new Editor(parent, s, std::move(changed), std::move(show)); g_ed->show();
}
void closeMaterialEditor() { if (g_ed) g_ed->close(); }
bool materialEditorShot(const QString& png) { return g_ed && g_ed->grab().save(png); }

}  // namespace hm
