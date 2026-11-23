#include "app/PaintPanels.h"
#include "app/PaintSession.h"
#include "app/ToolPanels.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QInputDialog>
#include <QPainter>
#include <QLinearGradient>
#include <QHash>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QMouseEvent>
#include <QSettings>
#include <tuple>
#include <QDir>
#include <QCheckBox>
#include <QColorDialog>
#include <QIcon>
#include <QPixmap>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QApplication>
#include <QUrl>
#include <QKeyEvent>
#include <QDateTime>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QClipboard>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QFileDialog>
#include "app/FileWindow.h"
#include "paint/LayerFx.h"
#include <QJsonArray>
#include <QJsonValue>
#include <QDialogButtonBox>
#include <QDialog>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QWheelEvent>
#include <QFile>
#include <QGridLayout>
#include <QShortcut>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <atomic>
#include <cmath>
#include <memory>

namespace hm {

using namespace paint;

namespace {

// (after a delete: the nearest remaining layer of the map shown at or under the old place, else above it — never a layer of another
//  map or a group, which the list does not show; −1: none)
int nearestOfMode(const PaintSession* s, int at) {
  const auto& L = s->doc.layers(); const Channel ch = s->modeChannel(); const int n = int(L.size());
  auto ok = [&](int j) { return L[size_t(j)].kind != LayerKind::Group && L[size_t(j)].channel == ch; };
  for (int j = std::min(at, n - 1); j >= 0; j--) if (ok(j)) return L[size_t(j)].id;
  for (int j = std::max(at, 0); j < n; j++) if (ok(j)) return L[size_t(j)].id;
  return -1;
}
// (a layer deleted as one Ctrl+Z step; the next active one of the map shown)
void deleteLayer(PaintSession* s, int id) {
  const int i = s->doc.indexOf(id); if (i < 0) return;
  s->layersEdit([s, id] { s->doc.remove(id); return true; });
  s->layer = nearestOfMode(s, i - 1 < 0 ? 0 : i - 1); if (!s->active() || s->active()->mask.isNull()) s->editMask = s->showMask = false;
}

QString pct(double x) { return QString::number(int(std::lround(x * 100))) + "%"; }
QString num(double x) { return QString::number(x, 'f', 2); }

SliderRow* slider(QVBoxLayout* v, const QString& label, double lo, double hi, double step, double val, std::function<QString(double)> fmt, const QString& tip = {}) {
  auto* s = new SliderRow(label, lo, hi, step, val, fmt); if (!tip.isEmpty()) s->setTip(tip); v->addWidget(s); return s;
}
}  // namespace
void addHelp(QWidget* panel, const QString& text) { if (!panel || text.isEmpty()) return; QStringList l = panel->property("help").toStringList(); if (!l.contains(text)) l << text; panel->setProperty("help", l); }
QString helpOf(QWidget* panel) { if (!panel) return {}; QStringList out = panel->property("help").toStringList();
  for (QWidget* c : panel->findChildren<QWidget*>()) for (const QString& t : c->property("help").toStringList()) if (!out.contains(t)) out << t; return out.join("\n\n"); }
namespace {
void showSt(QLabel* l, const QString& t) { l->setText(t); l->setVisible(!t.isEmpty()); }   // (a status line there only while it says something)
QLabel* note(QVBoxLayout* v, const QString& t) { auto* l = new QLabel(t); l->setProperty("mut", true); l->setWordWrap(true); v->addWidget(l); return l; }
QCheckBox* check(QVBoxLayout* v, const QString& t, bool on, const QString& tip = {}) { auto* c = new QCheckBox(t); c->setChecked(on); c->setToolTip(tip); c->setFocusPolicy(Qt::NoFocus); v->addWidget(c); return c; }
QPushButton* button(const QString& t, const QString& tip = {}) { auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); return b; }
void paintBtn(QPushButton* b, const QColor& c) { b->setStyleSheet(QString("QPushButton{background:%1;border:1px solid #555;min-height:18px}").arg(c.name())); }
QPushButton* colourBtn(QVBoxLayout* v, const QString& label, QColor init, std::function<void(const QColor&)> set) {
  auto* row = new QHBoxLayout; auto* l = new QLabel(label); auto* b = new QPushButton; b->setFocusPolicy(Qt::NoFocus); paintBtn(b, init); row->addWidget(l); row->addWidget(b, 1); v->addLayout(row);
  QObject::connect(b, &QPushButton::clicked, b, [b, set] { const QColor cur = QColor(b->property("c").toString()); const QColor c = QColorDialog::getColor(cur, b); if (c.isValid()) { b->setProperty("c", c.name()); paintBtn(b, c); set(c); } });
  b->setProperty("c", init.name()); return b;
}
QComboBox* combo(QVBoxLayout* v, const QString& label, const QStringList& items, int cur) {
  auto* row = new QHBoxLayout; row->addWidget(new QLabel(label)); auto* c = new QComboBox; c->addItems(items); c->setCurrentIndex(cur); row->addWidget(c, 1); v->addLayout(row); return c;
}

// a widget that calls a function when it is shown (the lists of the scene's meshes made again)
class ShowHook : public QWidget {
public:
  std::function<void()> onShow;
  void showEvent(QShowEvent* e) override { QWidget::showEvent(e); if (onShow) onShow(); }
};

// a Gradient Map's gradient: the bar and its stops under it. A click on the bar adds a stop (the colour the gradient has there), a stop
// is dragged along, double-clicked for another colour, right-clicked away; the one last touched is the selected one
class GradientBar : public QWidget {
public:
  std::vector<std::pair<float, QColor>> stops; int sel = 0; std::function<void()> changed;
  GradientBar() { setFixedHeight(36); setFocusPolicy(Qt::NoFocus); setMouseTracking(true);
    setToolTip(QObject::tr("The gradient: the darkest of what is under the layer takes the colour on the left, the lightest — the one on the right. Click the bar — a new stop; drag a stop; double-click — its colour; right-click — remove it")); }
  QRect bar() const { return QRect(8, 2, std::max(10, width() - 16), 18); }
  double xOf(float t) const { return bar().left() + double(t) * bar().width(); }
  int hit(const QPointF& p) const { int best = -1; double bd = 8; for (size_t i = 0; i < stops.size(); i++) { const double d = std::abs(p.x() - xOf(stops[i].first)); if (d < bd && p.y() > bar().top()) { bd = d; best = int(i); } } return best; }
  QColor at(float t) const { const auto lut = paint::Document::gradientLut(stops); const int i = std::clamp(int(t * 255 + 0.5f), 0, 255); return QColor::fromRgbF(lut[size_t(i) * 3], lut[size_t(i) * 3 + 1], lut[size_t(i) * 3 + 2]); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); const QRect b = bar(); const auto lut = paint::Document::gradientLut(stops);
    for (int x = 0; x < b.width(); x++) { const int i = std::clamp(x * 255 / std::max(1, b.width() - 1), 0, 255); p.fillRect(b.left() + x, b.top(), 1, b.height(), QColor::fromRgbF(lut[size_t(i) * 3], lut[size_t(i) * 3 + 1], lut[size_t(i) * 3 + 2])); }
    p.setPen(QPen(QColor(90, 90, 96), 1)); p.setBrush(Qt::NoBrush); p.drawRect(b);
    for (size_t i = 0; i < stops.size(); i++) { const double x = xOf(stops[i].first), y = b.bottom() + 2; QPolygonF tri; tri << QPointF(x, y) << QPointF(x - 6, y + 11) << QPointF(x + 6, y + 11);
      p.setPen(QPen(int(i) == sel ? QColor(255, 255, 255) : QColor(20, 20, 20), int(i) == sel ? 2 : 1)); p.setBrush(stops[i].second); p.drawPolygon(tri); }
  }
  void mousePressEvent(QMouseEvent* e) override {
    const int h = hit(e->position());
    if (e->button() == Qt::RightButton) { if (h >= 0 && stops.size() > 2) { stops.erase(stops.begin() + h); sel = std::clamp(sel, 0, int(stops.size()) - 1); update(); if (changed) changed(); } return; }
    if (e->button() != Qt::LeftButton) return;
    if (h >= 0) { sel = h; drag_ = true; update(); if (changed) changed(); return; }
    if (bar().adjusted(0, 0, 0, 14).contains(e->position().toPoint())) { const float t = std::clamp(float((e->position().x() - bar().left()) / bar().width()), 0.f, 1.f); stops.push_back({t, at(t)}); sel = int(stops.size()) - 1; drag_ = true; update(); if (changed) changed(); }
  }
  void mouseMoveEvent(QMouseEvent* e) override { if (!drag_ || sel < 0 || sel >= int(stops.size())) return; stops[size_t(sel)].first = std::clamp(float((e->position().x() - bar().left()) / bar().width()), 0.f, 1.f); update(); if (changed) changed(); }
  void mouseReleaseEvent(QMouseEvent*) override { drag_ = false; }
  void mouseDoubleClickEvent(QMouseEvent* e) override { const int h = hit(e->position()); if (h < 0) return; sel = h; const QColor c = QColorDialog::getColor(stops[size_t(h)].second, this); if (c.isValid()) { stops[size_t(h)].second = c; update(); if (changed) changed(); } }
private:
  bool drag_ = false;
};

// the layer's blend: a mode under the pointer in the open list is shown on the model at once (Photoshop's preview); the list closed
// without a choice — the blend as it was (the author tried seven blends with two clicks and the eye each, 16.11)
class BlendCombo : public QComboBox {
public:
  std::function<void(int)> preview; std::function<void()> restore, opened;
  BlendCombo() { QObject::connect(this, &QComboBox::highlighted, this, [this](int i) { if (open_ && preview) preview(i); }); }
  void showPopup() override { if (opened) opened(); open_ = true; QComboBox::showPopup(); }
  void hidePopup() override { const bool was = open_; open_ = false; QComboBox::hidePopup(); if (was && restore) restore(); }
private:
  bool open_ = false;
};

// a layer's row as in Photoshop / 3D-Coat (the author, 16.11: "where are the layers? what are these check boxes?"): an eye to click,
// the layer's picture, its name, then what it is — its blend and opacity when they are not plain, a mask, a lock
enum LayerRole { RId = Qt::UserRole, RDepth, RVisible, RBadge, RThumb, RDim, RTag, RMask, RMaskEdit, RClip, RBase };
class LayerRowDelegate : public QStyledItemDelegate {
public:
  std::function<void(int)> toggleEye; std::function<void(int, bool)> thumbClick; std::function<void(int)> maskOpen;
  static QRect eyeRect(const QRect& r, int depth) { return QRect(r.left() + 4 + depth * 12, r.top() + (r.height() - 16) / 2, 18, 16); }
  static QRect thumbRect(const QRect& r, int depth, bool clip) { const QRect e = eyeRect(r, depth); return QRect(e.right() + 6 + (clip ? 18 : 0), r.top() + 3, 30, 30); }   // (clipped: indented, as Photoshop)
  static QRect maskRect(const QRect& r, int depth, bool clip) { const QRect t = thumbRect(r, depth, clip); return QRect(t.right() + 5, t.top(), 30, 30); }
  static QRect nameRect(const QRect& r, int depth, bool mask, bool clip) { const QRect t = mask ? maskRect(r, depth, clip) : thumbRect(r, depth, clip); return QRect(t.right() + 8, r.top() + 4, r.right() - t.right() - 12, r.height() - 8); }
  QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& o, const QModelIndex& i) const override { auto* e = QStyledItemDelegate::createEditor(parent, o, i); return e; }
  void updateEditorGeometry(QWidget* e, const QStyleOptionViewItem& o, const QModelIndex& i) const override { e->setGeometry(nameRect(o.rect, i.data(RDepth).toInt(), !i.data(RMask).isNull(), i.data(RClip).toBool())); }
  QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override { return i.data(RId).isValid() ? QSize(o.rect.width(), 36) : QStyledItemDelegate::sizeHint(o, i); }
  void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override {
    if (!i.data(RId).isValid()) { QStyledItemDelegate::paint(p, o, i); return; }
    p->save(); p->setRenderHint(QPainter::Antialiasing); const QRect r = o.rect; const bool sel = o.state & QStyle::State_Selected;
    p->fillRect(r, sel ? QColor(0x3d, 0x2f, 0x52) : (o.state & QStyle::State_MouseOver) ? QColor(0x2e, 0x2e, 0x31) : QColor(0x26, 0x26, 0x28));
    p->setPen(QColor(0x34, 0x34, 0x36)); p->drawLine(r.bottomLeft(), r.bottomRight());
    const int depth = i.data(RDepth).toInt(); const bool vis = i.data(RVisible).toBool(), dim = i.data(RDim).toBool();
    // (the eye: open — shown, a dash — hidden)
    const QRectF e = eyeRect(r, depth); QPainterPath eye; eye.moveTo(e.left() + 1, e.center().y()); eye.quadTo(e.center().x(), e.top() - 3, e.right() - 1, e.center().y()); eye.quadTo(e.center().x(), e.bottom() + 3, e.left() + 1, e.center().y());
    if (vis) { p->setPen(QPen(QColor(210, 210, 214), 1.4)); p->setBrush(Qt::NoBrush); p->drawPath(eye); p->setBrush(QColor(210, 210, 214)); p->drawEllipse(e.center(), 2.6, 2.6); }
    else { p->setPen(QPen(QColor(95, 95, 100), 1.4)); p->drawLine(QPointF(e.left() + 3, e.center().y()), QPointF(e.right() - 3, e.center().y())); }
    // (the picture over a checker — what is clear in it shows as clear)
    const bool clip = i.data(RClip).toBool(); const QRect th = thumbRect(r, depth, clip); const bool hasMask = !i.data(RMask).isNull(), onMask = i.data(RMaskEdit).toBool();
    if (clip) { p->setPen(QPen(QColor(200, 200, 205), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p->setBrush(Qt::NoBrush); const double ax = th.left() - 8, y0 = r.top() + 10, y1 = r.bottom() - 9;   // (the bent arrow down to the base)
      p->drawPolyline(QPolygonF({QPointF(ax - 5, y0), QPointF(ax, y0), QPointF(ax, y1)})); p->drawPolyline(QPolygonF({QPointF(ax - 3.5, y1 - 3.5), QPointF(ax, y1), QPointF(ax + 3.5, y1 - 3.5)})); }
    for (int y = 0; y < 30; y += 6) for (int x = 0; x < 30; x += 6) p->fillRect(QRect(th.left() + x, th.top() + y, 6, 6), ((x + y) / 6) % 2 ? QColor(0x55, 0x55, 0x58) : QColor(0x3e, 0x3e, 0x41));
    const QPixmap pm = i.data(RThumb).value<QPixmap>(); if (!pm.isNull()) p->drawPixmap(th.left() + (30 - pm.width()) / 2, th.top() + (30 - pm.height()) / 2, pm);
    else { p->setPen(QColor(200, 200, 205)); QFont f = p->font(); f.setPixelSize(15); p->setFont(f); p->drawText(th, Qt::AlignCenter, i.data(RTag).toString()); }
    // (the picture being painted framed: the layer's — or its mask's, as Photoshop marks the one the brush writes into)
    p->setBrush(Qt::NoBrush); p->setPen(QPen(sel && !onMask ? QColor(0xe8, 0xe8, 0xee) : QColor(0x4a, 0x4a, 0x4e), sel && !onMask ? 2 : 1)); p->drawRect(QRectF(th).adjusted(-0.5, -0.5, 0.5, 0.5));
    if (hasMask) { const QRect mr = maskRect(r, depth, clip); p->fillRect(mr, Qt::black); const QPixmap mp = i.data(RMask).value<QPixmap>(); if (!mp.isNull()) p->drawPixmap(mr.left() + (30 - mp.width()) / 2, mr.top() + (30 - mp.height()) / 2, mp);
      p->setPen(QPen(sel && onMask ? QColor(0xe8, 0xe8, 0xee) : QColor(0x4a, 0x4a, 0x4e), sel && onMask ? 2 : 1)); p->drawRect(QRectF(mr).adjusted(-0.5, -0.5, 0.5, 0.5)); }
    // (the name, then the badges at the right)
    QFont f = o.font; f.setPixelSize(12); f.setUnderline(i.data(RBase).toBool()); p->setFont(f); const QString badge = i.data(RBadge).toString();
    const int bw = badge.isEmpty() ? 0 : QFontMetrics(f).horizontalAdvance(badge) + 8;
    const QRect nr = nameRect(r, depth, hasMask, clip), tr(nr.left(), r.top(), nr.width() - bw, r.height());
    p->setPen(dim || !vis ? QColor(135, 135, 140) : QColor(225, 225, 228)); p->drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(f).elidedText(i.data(Qt::DisplayRole).toString(), Qt::ElideRight, tr.width()));
    if (bw) { p->setPen(QColor(160, 150, 175)); p->drawText(QRect(r.right() - bw, r.top(), bw - 4, r.height()), Qt::AlignVCenter | Qt::AlignRight, badge); }
    p->restore(); }
  bool editorEvent(QEvent* ev, QAbstractItemModel*, const QStyleOptionViewItem& o, const QModelIndex& i) override {
    if (!i.data(RId).isValid()) return false; const int depth = i.data(RDepth).toInt(), id = i.data(RId).toInt(); const bool hasMask = !i.data(RMask).isNull(), clip = i.data(RClip).toBool();
    if (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseButtonDblClick) { auto* me = static_cast<QMouseEvent*>(ev); const QPoint pt = me->position().toPoint(); if (me->button() != Qt::LeftButton) return false;
      if (eyeRect(o.rect, depth).adjusted(-3, -6, 3, 6).contains(pt)) { if (ev->type() == QEvent::MouseButtonPress && toggleEye) toggleEye(id); return true; }
      const bool onMask = hasMask && maskRect(o.rect, depth, clip).contains(pt), onThumb = thumbRect(o.rect, depth, clip).contains(pt);
      if (ev->type() == QEvent::MouseButtonDblClick) { if (onMask) { if (maskOpen) maskOpen(id); return true; } return !nameRect(o.rect, depth, hasMask, clip).contains(pt); }   // (a double click: on the mask — it is shown; on the name — renamed there)
      if (onMask && (me->modifiers() & Qt::AltModifier)) { if (maskOpen) maskOpen(id); return true; }                                                                       // (Alt + click on the mask — it is shown, as Photoshop)
      if ((onMask || onThumb) && thumbClick) thumbClick(id, onMask); }
    return false; }
};
// a layer's picture for its row: its own pixels (kept by the picture's key — made again only when it changed), a fill's colour,
// a live gradient's colours, the spheres' colours
QIcon layerFlagIcon(int kind) {   // 0 — transparency locked, 1 — clipped to the layer below, 2 — the layer locked
  QPixmap pm(40, 40); pm.setDevicePixelRatio(2); pm.fill(Qt::transparent); QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); const QColor ink(0xd8, 0xd8, 0xdc);
  auto padlock = [&](QRectF body, double w) { p.setPen(QPen(ink, w)); p.setBrush(Qt::NoBrush); const double sw = body.width() * 0.62;
    p.drawArc(QRectF(body.center().x() - sw / 2, body.top() - sw * 0.75, sw, sw * 1.5), 0, 180 * 16); p.setPen(Qt::NoPen); p.setBrush(ink); p.drawRoundedRect(body, 1.2, 1.2); };
  if (kind == 0) { for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) p.fillRect(QRectF(2 + x * 3.6, 4 + y * 3.6, 3.6, 3.6), (x + y) % 2 ? QColor(0x4a, 0x4a, 0x4e) : QColor(0x9a, 0x9a, 0xa0));
    padlock(QRectF(11, 11.5, 7, 5.5), 1.4); }
  else if (kind == 1) { p.setPen(QPen(ink, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(10, 3), QPointF(10, 14)); p.drawPolyline(QPolygonF({QPointF(6.5, 10.5), QPointF(10, 14), QPointF(13.5, 10.5)})); p.drawLine(QPointF(4.5, 17), QPointF(15.5, 17)); }
  else { padlock(QRectF(5, 9, 10, 8), 1.9); p.setBrush(QColor(0x26, 0x26, 0x26)); p.drawEllipse(QPointF(10, 12.6), 1.2, 1.2); }
  return QIcon(pm);
}
// a clipping mask as Photoshop: made — the layer clipped to the one under it; released — it and the clipped layers right above it, back to ordinary ones
class CurveEdit : public QWidget {
public:
  std::function<Curve()> get; std::function<void(const Curve&)> set;
  CurveEdit() { setFixedSize(132, 96); setToolTip(QObject::tr("The pressure's curve: across — how hard the pen presses, up — how much of the effect. Drag a point; a double click adds one; right click takes it off")); }
protected:
  int drag_ = -1;
  QPointF toW(const QPointF& p) const { return QPointF(6 + p.x() * (width() - 12), height() - 6 - p.y() * (height() - 12)); }
  QPointF toC(const QPointF& w) const { return QPointF(std::clamp((w.x() - 6) / (width() - 12), 0.0, 1.0), std::clamp((height() - 6 - w.y()) / (height() - 12), 0.0, 1.0)); }
  int near(const QPointF& w) const { const Curve c = get(); for (size_t i = 0; i < c.pts.size(); i++) if (QLineF(toW(c.pts[i]), w).length() < 8) return int(i); return -1; }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), QColor(30, 30, 32)); p.setPen(QColor(60, 60, 64)); p.drawRect(rect().adjusted(0, 0, -1, -1));
    p.drawLine(toW({0, 0}), toW({1, 1}));
    const Curve c = get(); QPolygonF line; for (int i = 0; i <= 48; i++) { const float x = i / 48.f; line << toW(QPointF(x, c.at(x))); }
    p.setPen(QPen(QColor(110, 170, 255), 2)); p.drawPolyline(line);
    p.setBrush(QColor(230, 230, 230)); p.setPen(Qt::NoPen); for (const QPointF& q : c.pts) p.drawEllipse(toW(q), 3.5, 3.5);
  }
  void mousePressEvent(QMouseEvent* e) override {
    Curve c = get(); const int k = near(e->position());
    if (e->button() == Qt::RightButton) { if (k > 0 && k < int(c.pts.size()) - 1) { c.pts.erase(c.pts.begin() + k); set(c); update(); } return; }
    drag_ = k; }
  void mouseDoubleClickEvent(QMouseEvent* e) override {
    Curve c = get(); const QPointF q = toC(e->position()); c.pts.push_back(q); std::sort(c.pts.begin(), c.pts.end(), [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); }); set(c); update(); }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (drag_ < 0) return; Curve c = get(); if (drag_ >= int(c.pts.size())) return; QPointF q = toC(e->position());
    const double lo = drag_ > 0 ? c.pts[size_t(drag_) - 1].x() + 0.01 : 0, hi = drag_ + 1 < int(c.pts.size()) ? c.pts[size_t(drag_) + 1].x() - 0.01 : 1;
    if (drag_ == 0) q.setX(0); else if (drag_ == int(c.pts.size()) - 1) q.setX(1); else q.setX(std::clamp(q.x(), lo, hi));   // (the ends stay at 0 and 1 across)
    c.pts[size_t(drag_)] = q; set(c); update(); }
  void mouseReleaseEvent(QMouseEvent*) override { drag_ = -1; }
};
// layers from another project / an autosave: the file chosen, its layers listed with ticks, the ticked ones brought over the active one
void layersFromFile(PaintSession* s, QWidget* parent, bool autosave, std::function<void()> after) {
  QSettings st; const QString start = autosave ? files::autosaveSettings().dir : st.value("paint/layersFromDir", st.value("lastDir").toString()).toString();
  const QString path = QFileDialog::getOpenFileName(parent, autosave ? QObject::tr("Layers from an autosave") : QObject::tr("Layers from another project"), start, QObject::tr("HairHarpy projects (*.hairmesh)"));
  if (path.isEmpty()) return; if (!autosave) st.setValue("paint/layersFromDir", QFileInfo(path).absolutePath());
  QApplication::setOverrideCursor(Qt::WaitCursor); QString err; const auto L = s->layersInFile(path, &err); QApplication::restoreOverrideCursor();
  if (L.empty()) { ui::toast(err.isEmpty() ? QObject::tr("No Paint layers in %1").arg(QFileInfo(path).fileName()) : err); return; }
  QDialog d(parent); d.setWindowTitle(QObject::tr("Layers from %1").arg(QFileInfo(path).fileName())); auto* v = new QVBoxLayout(&d);
  auto* lw = new QListWidget; lw->setMinimumSize(340, 300); v->addWidget(lw);
  for (const auto& x : L) { auto* it = new QListWidgetItem(QString(x.depth * 4, ' ') + x.name + "   · " + x.kind, lw); it->setFlags(it->flags() | Qt::ItemIsUserCheckable); it->setCheckState(Qt::Unchecked); it->setData(Qt::UserRole, x.id); }
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); bb->button(QDialogButtonBox::Ok)->setText(QObject::tr("Bring them")); v->addWidget(bb);
  QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept); QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  QObject::connect(lw, &QListWidget::itemDoubleClicked, lw, [](QListWidgetItem* it) { it->setCheckState(it->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked); });
  if (d.exec() != QDialog::Accepted) return;
  std::vector<int> ids; for (int i = 0; i < lw->count(); i++) if (lw->item(i)->checkState() == Qt::Checked) ids.push_back(lw->item(i)->data(Qt::UserRole).toInt());
  if (ids.empty()) return;
  QApplication::setOverrideCursor(Qt::WaitCursor); const QString r = s->takeLayersFrom(path, ids); QApplication::restoreOverrideCursor();
  after(); ui::toast(r);
}
// (an object shadow's casters: the meshes selected in the outliner, else every mesh whose name holds "hair")
QStringList shadowCasters(Viewport* vp) {
  QStringList out; if (!vp || !vp->scene()) return out; const Scene& sc = *vp->scene();
  for (int id : vp->selection().items) if (const Item* it = sc.item(id); it && it->type == ItemType::IMesh && it->poly && !out.contains(it->name)) out << it->name;
  if (out.isEmpty()) for (const auto& [id, it] : sc.items) if (it.type == ItemType::IMesh && it.poly && it.name.contains("hair", Qt::CaseInsensitive) && !out.contains(it.name)) out << it.name;
  return out;
}
void toggleClip(PaintSession* s, int id) {
  Layer* l = s->doc.layer(id); if (!l || !(l->kind == LayerKind::Paint || l->kind == LayerKind::Fill || l->kind == LayerKind::Adjust || l->kind == LayerKind::Group)) return;
  if (!l->clip) l->clip = true;
  else { const auto& L = s->doc.layers(); const int i0 = s->doc.indexOf(id); for (int j = i0; j < int(L.size()); j++) { Layer& u = const_cast<Layer&>(L[size_t(j)]); if (u.parent != l->parent) continue; if (!u.clip) break; u.clip = false; } }
  s->edits++; emit s->pixelsChanged({}); emit s->layersChanged();
}
QPixmap maskThumb(const Layer& l) {   // (its mask small: white shows, black hides)
  static QHash<qint64, QPixmap> cache; if (cache.size() > 300) cache.clear(); if (l.mask.isNull()) return {};
  const qint64 k = l.mask.cacheKey(); if (auto f = cache.constFind(k); f != cache.constEnd()) return *f;
  const QPixmap pm = QPixmap::fromImage(l.mask.scaled(120, 120, Qt::KeepAspectRatio, Qt::FastTransformation).scaled(30, 30, Qt::KeepAspectRatio, Qt::SmoothTransformation)); cache.insert(k, pm); return pm;
}
QPixmap layerThumb(const Layer& l) {
  static QHash<qint64, QPixmap> cache; if (cache.size() > 300) cache.clear();
  const int T = 30;
  if (l.kind == LayerKind::Paint) { if (l.image.isNull()) return {}; const qint64 k = l.image.cacheKey(); if (auto f = cache.constFind(k); f != cache.constEnd()) return *f;
    QImage im = l.image.format() == QImage::Format_ARGB32 ? l.image : l.image.convertToFormat(QImage::Format_ARGB32);
    QImage sm = im.scaled(T * 4, T * 4, Qt::KeepAspectRatio, Qt::FastTransformation).scaled(T, T, Qt::KeepAspectRatio, Qt::SmoothTransformation);   // (a quick step down, then a smooth one)
    const QPixmap pm = QPixmap::fromImage(sm); cache.insert(k, pm); return pm; }
  if (l.kind == LayerKind::Fill) { QPixmap pm(T, T); pm.fill(Qt::transparent); QPainter q(&pm); q.setRenderHint(QPainter::Antialiasing);
    if (l.gradLive && l.liveKind == QLatin1String("spheres")) { q.fillRect(pm.rect(), QColor(40, 40, 44)); int n = 0; for (const auto& sp : l.spheres) { if (n >= 6) break; q.setBrush(sp.col); q.setPen(Qt::NoPen); q.drawEllipse(QPointF(7 + (n % 3) * 8, 10 + (n / 3) * 10), 5, 5); n++; } }
    else if (l.gradLive) { QLinearGradient g(0, 0, 0, T); for (const auto& [at, c] : l.grad) g.setColorAt(std::clamp(double(at), 0.0, 1.0), c); if (l.grad.empty()) { g.setColorAt(0, l.fill); g.setColorAt(1, Qt::black); } q.fillRect(pm.rect(), g); }
    else q.fillRect(pm.rect(), l.fill);
    return pm; }
  return {};
}

QString kindTag(const Layer& l) {
  switch (l.kind) {
    case LayerKind::Group: return QString::fromUtf8("▸ ");
    case LayerKind::Fill: return QString::fromUtf8("▣ ");
    case LayerKind::Adjust: return QString::fromUtf8("◐ ");
    default: return l.channel == Channel::Color ? "" : l.channel == Channel::Height ? "[H] " : "[F] ";
  }
}

}  // namespace

// ================= Layers =================

QWidget* makePaintLayersPanel(PaintSession* s, Viewport* vp) {
  auto* w = new ShowHook; auto* v = new QVBoxLayout(w); v->setContentsMargins(8, 6, 8, 8); v->setSpacing(5);
  w->setStyleSheet("QToolButton{padding:2px 5px}");             // (five buttons in a row: narrow enough for the column — a wider row pushed the whole panel past its right edge)
  // the room's mode — the map being painted: each map its own layers, the brush writes what that map holds
  auto* mode = new QComboBox; mode->setFocusPolicy(Qt::NoFocus);
  for (const MapInfo& m : maps()) { mode->addItem(QObject::tr(m.name), QString::fromLatin1(m.key)); mode->setItemData(mode->count() - 1, QObject::tr(m.about), Qt::ToolTipRole);
    if (m.ch == Channel::Height) { mode->addItem(QObject::tr("Normal map (the result)"), "normalBump"); mode->setItemData(mode->count() - 1, QObject::tr("The normal map as it will be written: the baked normals where the Bake mask is painted, the painted bump over them. Painting here goes into the bump"), Qt::ToolTipRole); } }
  { auto* row = new QHBoxLayout; row->addWidget(new QLabel(QObject::tr("Map"))); row->addWidget(mode, 1); v->addLayout(row); }
  QComboBox* about = mode;                                      // (what the map is for: the picker's own tip — and each row's in the list)
  // the UV set painted in: one list of layers for all of them, each set its own pixels and its own size
  auto* setC = new QComboBox; setC->setFocusPolicy(Qt::NoFocus); setC->setToolTip(QObject::tr("The UV set being painted. The layers are the same for every set — a layer keeps pixels only in the sets it was painted in; the texture's size is the set's own (the UV Editor's Texture block)"));
  { auto* rw = new QWidget; auto* row = new QHBoxLayout(rw); row->setContentsMargins(0, 0, 0, 0); row->addWidget(new QLabel(QObject::tr("UV set"))); row->addWidget(setC, 1); rw->hide(); v->addWidget(rw); }   // (the UV set: the manager window's — not a question of the layers; one list serves every set)
  auto fillSets = [s, vp, setC] {
    QSignalBlocker b(setC); setC->clear();
    const bool single = s->doc.activeSet().isEmpty();
    setC->addItem(single && s->doc.width() ? QObject::tr("Single texture · %1×%2").arg(s->doc.width()).arg(s->doc.height()) : QObject::tr("Single texture"), QString());
    if (vp) for (const QString& n : vp->uvSetNamesAll()) { const auto t = vp->uvSetProps(n); setC->addItem(QString("%1 · %2×%3").arg(n).arg(t.w).arg(t.h), n); }
    const int i = setC->findData(s->doc.activeSet()); setC->setCurrentIndex(i < 0 ? 0 : i); };
  QObject::connect(setC, &QComboBox::activated, s, [s, vp, setC](int) {
    const QString n = setC->currentData().toString();
    if (n.isEmpty()) { int w0 = s->bakeS.size, h0 = s->bakeS.size; for (const auto& x : s->doc.sets()) if (x.key.isEmpty()) { w0 = x.w; h0 = x.h; } s->setSet(QString(), w0, h0); }
    else if (vp) { const auto t = vp->uvSetProps(n); s->setSet(n, t.w, t.h); } });
  auto syncMode = [s, mode, about] { QSignalBlocker b(mode); const int i = mode->findData(s->view); if (i >= 0) mode->setCurrentIndex(i);
    about->setToolTip(QObject::tr("The map being painted — the room's mode: each map has its own layers, and the brush writes what that map holds.\n\n%1").arg(s->view.startsWith("bake:") ? QObject::tr("A baked map is shown: pick a map to paint.") : QObject::tr(mapInfo(s->modeChannel()).about))); };
  QObject::connect(mode, &QComboBox::activated, s, [s, mode](int) { s->setView(mode->currentData().toString()); });
  // (over the list, as in Photoshop: the active layer's blend and opacity)
  auto* topBl = new BlendCombo; topBl->setMaxVisibleItems(24); for (Blend b : blendOrder()) topBl->addItem(blendName(b), int(b)); topBl->setFocusPolicy(Qt::NoFocus);
  topBl->setToolTip(QObject::tr("The active layer's blend. With the list open, the mode under the pointer is shown on the model at once; closed without a choice — the blend stays as it was"));
  auto* topOp = new QSpinBox; topOp->setRange(0, 100); topOp->setSuffix("%"); topOp->setValue(100); topOp->setFixedWidth(62); topOp->setKeyboardTracking(false); topOp->setAccelerated(true);
  topOp->setToolTip(QObject::tr("The active layer's opacity — type it, or the wheel / the arrows over it"));
  { auto* row = new QHBoxLayout; row->setSpacing(4); topBl->setMinimumWidth(60); row->addWidget(topBl, 1); row->addWidget(topOp); v->addLayout(row); }
  auto* list = new QListWidget; list->setMinimumHeight(150); list->setFocusPolicy(Qt::NoFocus); list->setMouseTracking(true); v->addWidget(list, 1);   // (the list takes the panel's height)
  list->setStyleSheet("QListWidget{background:#232325;border:1px solid #3a3a3a} QListWidget::item{padding:0;margin:0}");
  auto* rowDel = new LayerRowDelegate; rowDel->setParent(list); list->setItemDelegate(rowDel);
  rowDel->toggleEye = [s](int id) {
    if (QApplication::keyboardModifiers() & Qt::AltModifier) {   // (Alt + click on the eye, as Photoshop: this layer alone shown — again: all back)
      s->doc.solo = s->doc.solo == id ? -1 : id; if (Layer* l = s->doc.layer(id)) l->visible = true;
      ui::hud(s->doc.solo >= 0 ? QObject::tr("Only the layer %1 — Alt + click on its eye again: all back").arg(s->doc.layer(id) ? s->doc.layer(id)->name : QString()) : QString(), 1600);
      emit s->pixelsChanged({}); emit s->layersChanged(); return; }
    if (Layer* l = s->doc.layer(id)) { l->visible = !l->visible; emit s->pixelsChanged({}); emit s->layersChanged(); } };
  rowDel->thumbClick = [s](int id, bool mask) {
    // (Ctrl + click on the picture, as Photoshop's Ctrl + click on a thumbnail: where this layer has paint — it alone shown while the
    // button is held. 18.11: four Ctrl + click → Ctrl + D in Photoshop "to see where the paste is left" after a soft eraser)
    if (!mask && (QApplication::keyboardModifiers() & Qt::ControlModifier) && s->doc.solo < 0) {
      const Layer* l = s->doc.layer(id); if (!l) return; const bool vis = l->visible; s->doc.solo = id; s->doc.layer(id)->visible = true; emit s->pixelsChanged({});
      ui::hud(QObject::tr("Where %1 has paint — it alone, while the button is held").arg(l->name));
      auto* t = new QTimer(s); t->setInterval(40);
      QObject::connect(t, &QTimer::timeout, s, [s, t, id, vis] { if (QGuiApplication::mouseButtons() & Qt::LeftButton) return; t->stop(); t->deleteLater();
        if (s->doc.solo == id) s->doc.solo = -1; if (Layer* x = s->doc.layer(id)) x->visible = vis; ui::hud({}); emit s->pixelsChanged({}); });
      t->start(); return; }
    if (s->layer != id) s->setLayer(id); if (s->editMask != mask) { s->editMask = mask; emit s->brushChanged(); } QTimer::singleShot(0, s, [s] { emit s->layersChanged(); }); };
  rowDel->maskOpen = [s](int id) { if (s->layer != id) s->setLayer(id); s->showMask = !s->showMask; if (s->showMask) s->editMask = true; emit s->brushChanged(); emit s->pixelsChanged({}); QTimer::singleShot(0, s, [s] { emit s->layersChanged(); });
    ui::toast(s->showMask ? QObject::tr("The mask itself is shown — white shows the layer, black hides it; paint it with black and white. Double click on it again — the map back") : QObject::tr("The map shown again")); };
  list->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
  { struct ClipLine : QObject { QListWidget* lw; std::function<void(int)> toggle; bool on = false; using QObject::QObject;
      int upperAt(const QPoint& pt) const {   // (the layer above the line under the pointer — when both sides of it are layers)
        QListWidgetItem *a = lw->itemAt(pt - QPoint(0, 5)), *b = lw->itemAt(pt + QPoint(0, 5)); if (!a || !b || a == b || !a->data(RId).isValid() || !b->data(RId).isValid()) return -1;
        return lw->row(b) == lw->row(a) + 1 ? a->data(RId).toInt() : -1; }
      bool eventFilter(QObject*, QEvent* e) override {
        if (e->type() == QEvent::MouseMove) { auto* me = static_cast<QMouseEvent*>(e); const bool hit = (me->modifiers() & Qt::AltModifier) && upperAt(me->position().toPoint()) >= 0;
          if (hit != on) { on = hit; if (hit) lw->viewport()->setCursor(QCursor(layerFlagIcon(1).pixmap(20, 20), 10, 10)); else lw->viewport()->unsetCursor(); } }
        if (e->type() == QEvent::MouseButtonPress) { auto* me = static_cast<QMouseEvent*>(e);
          if (me->button() == Qt::LeftButton && (me->modifiers() & Qt::AltModifier)) { const int up = upperAt(me->position().toPoint()); if (up >= 0) { toggle(up); return true; } } }
        if (e->type() == QEvent::Leave && on) { on = false; lw->viewport()->unsetCursor(); }
        return false; } };
    auto* cf = new ClipLine(list); cf->lw = list; cf->toggle = [s](int id) { toggleClip(s, id); }; list->viewport()->installEventFilter(cf); }
  { auto orig = std::make_shared<Blend>(Blend::Normal);
    topBl->preview = [s, topBl](int i) { if (Layer* x = s->active(); x && i >= 0) { const Blend b = Blend(topBl->itemData(i).toInt()); if (x->blend != b) { x->blend = b; emit s->pixelsChanged({}); } } };
    topBl->opened = [s, orig] { if (Layer* x = s->active()) *orig = x->blend; };
    topBl->restore = [s, orig] { if (Layer* x = s->active()) { if (x->blend != *orig) { x->blend = *orig; emit s->pixelsChanged({}); } } };
    QObject::connect(topBl, &QComboBox::activated, s, [s, topBl](int) { if (Layer* x = s->active()) { x->blend = Blend(topBl->currentData().toInt()); emit s->pixelsChanged({}); emit s->layersChanged(); } });
    QObject::connect(topOp, &QSpinBox::valueChanged, s, [s](int x) { if (Layer* y = s->active(); y && std::abs(y->opacity - x / 100.f) > 1e-4f) { y->opacity = x / 100.f; emit s->pixelsChanged({}); QTimer::singleShot(0, s, [s] { emit s->layersChanged(); }); } }); }
  auto building = std::make_shared<bool>(false);
  QObject::connect(list, &QListWidget::itemChanged, list, [s, building](QListWidgetItem* it) { if (*building || !it->data(RId).isValid()) return;   // (renamed in its row)
    if (Layer* l = s->doc.layer(it->data(RId).toInt()); l && !it->text().trimmed().isEmpty() && l->name != it->text().trimmed()) { l->name = it->text().trimmed(); QTimer::singleShot(0, s, [s] { emit s->layersChanged(); }); } });

  // The layers' commands: one row of compact icons under the list, no words on them (what each does is in its tip). What comes in
  // several kinds — a fill or an adjustment layer, the mask's commands — opens as a menu
  auto* tb = new QHBoxLayout; tb->setSpacing(1);
  auto after = [s] { emit s->layersChanged(); emit s->pixelsChanged({}); };
  auto icoBtn = [&](const char* icon, const QString& tip, std::function<void()> f) { auto* b = new QToolButton; b->setIcon(ui::icon(icon)); b->setIconSize(QSize(22, 22)); b->setFixedSize(25, 25); b->setToolTip(tip);
    b->setStyleSheet("QToolButton{padding:0;background:transparent;border:1px solid transparent;border-radius:5px} QToolButton:hover{border:1px solid #92776d} QToolButton:pressed{border:1px solid #cbbfb3} QToolButton::menu-indicator{image:none}"); b->setFocusPolicy(Qt::NoFocus); tb->addWidget(b); if (f) QObject::connect(b, &QToolButton::clicked, s, f); return b; };
  auto menuBtn = [&](const char* icon, const QString& tip) { auto* b = icoBtn(icon, tip, nullptr); b->setPopupMode(QToolButton::InstantPopup); auto* m = new QMenu(b); m->setToolTipsVisible(true); b->setMenu(m); return m; };
  auto item = [s](QMenu* m, const QString& name, const QString& tip, std::function<void()> f, const char* icon = nullptr) { QAction* a = m->addAction(name); a->setToolTip(tip); if (icon) a->setIcon(ui::icon(icon)); QObject::connect(a, &QAction::triggered, s, f); return a; };
  icoBtn("rc_layer_new", QObject::tr("New paint layer of the map shown (the Map above): colour, a gray mask, the bump's relief or the flow's direction — whatever that map holds"), [s, after] {
    s->ensureDoc(); s->layer = s->doc.addPaint(s->modeChannel(), {}, s->layer); after(); });
  { QMenu* m = menuBtn("pt_adjust", QObject::tr("New fill or adjustment layer"));
    item(m, QObject::tr("Solid colour"), QObject::tr("A fill layer of the map shown: one flat value over everything under it, to be masked or blended (white in the Bake mask: the whole bake is used)"), [s, after] {
      s->ensureDoc(); const Channel c = s->modeChannel(); const MapInfo& mi = mapInfo(c);
      s->layer = s->doc.addFill(c, c == Channel::Flow ? QColor(255, 128, 128) : c == Channel::BakeMask ? QColor(255, 255, 255) : mi.gray ? QColor(128, 128, 128) : QColor(128, 128, 128), {}, s->layer); after(); }, "rc_fill_layer");
    item(m, QObject::tr("Live gradient"), QObject::tr("A live gradient over the UV set shown: from the top of the model (the brush's colour) to its bottom (the second colour) — its colours, blend and opacity stay editable; drag with Fill (gradient on) over the model to move its ends. Right-click the layer — an ordinary paint layer of it"), [s, vp, after] {
      if (mapInfo(s->modeChannel()).storage != Storage::Colour || !vp) { ui::toast(QObject::tr("A live gradient belongs to a colour or a mask map")); return; }
      s->ensureDoc(); Box3 b; { const auto& G = vp->pvBakeGeom(s->doc.activeSet(), s->doc.width(), s->doc.height());   // (the height of what this UV set covers — the hair, not the whole figure)
        for (size_t q = 0; q + 2 < G.p.size(); q += 3) if (G.n[q] != 0 || G.n[q + 1] != 0 || G.n[q + 2] != 0) b.expand(Vec3(G.p[q], G.p[q + 1], G.p[q + 2])); }
      if (b.empty()) b = vp->pvSceneBox(); if (b.empty()) { ui::toast(QObject::tr("No model to lay the gradient over")); return; }
      const int id = s->doc.addFill(s->modeChannel(), s->color, s->doc.freeName("Gradient"), s->layer);
      if (paint::Layer* f = s->doc.layer(id)) { const Vec3 c = b.center(); f->gradLive = true; f->g0[0] = float(c.x); f->g0[1] = float(b.max.y); f->g0[2] = float(c.z); f->g1[0] = float(c.x); f->g1[1] = float(b.min.y); f->g1[2] = float(c.z);
        f->grad = {{0.f, QColor(s->color.red(), s->color.green(), s->color.blue())}, {1.f, QColor(s->color2.red(), s->color2.green(), s->color2.blue())}}; if (!s->doc.activeSet().isEmpty()) f->onlySets = QStringList{s->doc.activeSet()}; }
      s->layer = id; after(); }, "pt_gradient");
    item(m, QObject::tr("Colour spheres"), QObject::tr("A live layer of colour spheres over the UV set shown: with Fill, a click on the model puts a sphere in the brush's colour, a drag from a sphere moves it, Ctrl + click removes it; each its own colour, radius and hardness (the Layers panel), spheres of one link change together. Right-click the layer — an ordinary paint layer of it"), [s, after] {
      if (mapInfo(s->modeChannel()).storage != Storage::Colour) { ui::toast(QObject::tr("Colour spheres belong to a colour or a mask map")); return; }
      s->ensureDoc(); const int id = s->doc.addFill(s->modeChannel(), s->color, s->doc.freeName("Spheres"), s->layer);
      if (paint::Layer* f = s->doc.layer(id)) { f->gradLive = true; f->liveKind = "spheres"; if (!s->doc.activeSet().isEmpty()) f->onlySets = QStringList{s->doc.activeSet()}; }
      s->layer = id; s->sphereSel = -1; after(); ui::toast(QObject::tr("Colour spheres: take Fill and click the model — a sphere in the brush's colour")); }, "rc_fill_layer");
    item(m, QObject::tr("Light and shade"), QObject::tr("A light-and-shade layer: Overlay, filled with neutral 50 % gray in this UV set — paint lighter or darker than gray to light or shade what is under it, paint the gray back to take the effect off (the brush's colour is set to that gray)"), [s, after] {
      if (mapInfo(s->modeChannel()).storage != Storage::Colour) { ui::toast(QObject::tr("A light-and-shade layer belongs to a colour map")); return; }
      s->ensureDoc(); const int id = s->doc.addPaint(s->modeChannel(), {}, s->layer); if (paint::Layer* l = s->doc.layer(id)) { l->blend = Blend::Overlay; l->image.fill(QColor(128, 128, 128, 255)); l->name = s->doc.smartName(id); }
      s->layer = id; s->color2 = s->color; s->color = QColor(128, 128, 128); emit s->brushChanged(); after(); }, "pt_light");
    item(m, QObject::tr("Object shadow"), QObject::tr("A live shadow the chosen objects cast onto this UV set — the hair onto the shoulders, say: a Multiply layer worked out from the scene, its own light (round, height), softness and colour in the Layers panel; hidden objects still cast. The casters: the objects selected in the outliner, else the ones named \"hair\". Bake it to paint over it"), [s, vp, after] {
      if (!vp || mapInfo(s->modeChannel()).storage != Storage::Colour) { ui::toast(QObject::tr("An object shadow belongs to a colour map")); return; }
      const QStringList names = shadowCasters(vp); if (names.isEmpty()) { ui::toast(QObject::tr("Select the objects that cast the shadow in the outliner first (no object is named \"hair\")")); return; }
      s->ensureDoc(); const int id = s->addShadow(names); if (id < 0) { ui::toast(QObject::tr("No UV set to lay the shadow on")); return; }
      after(); ui::toast(QObject::tr("The shadow of %1 — made in the background; its light and softness in the Layers panel").arg(names.join(", "))); }, "pt_light");
    item(m, QObject::tr("Scalp shadow…"), QObject::tr("A Multiply layer darkening the locks where they leave the scalp, fading with the distance — in the colour in hand; then paint it as any layer. Mark the scalp first: Select tool → Freeze menu → Mark as the scalp"), [s, vp, after] {
      if (!vp || !vp->pvHasScalp()) { ui::toast(QObject::tr("Mark the scalp first: select its polygons (Select tool) → the Freeze menu → Mark as the scalp")); return; }
      if (mapInfo(s->modeChannel()).storage != Storage::Colour) { ui::toast(QObject::tr("The scalp shadow belongs to a colour map")); return; }
      QDialog d; d.setWindowTitle(QObject::tr("Scalp shadow")); auto* lv = new QVBoxLayout(&d);
      auto* len = new SliderRow(QObject::tr("Length"), 0.005, 0.3, 0.005, s->scalpReach, [](double x) { return QString::number(int(std::lround(x * 1000)) / 10.0) + "%"; });
      len->setTip(QObject::tr("How far down the locks the shadow reaches — a share of the model's size")); lv->addWidget(len);
      auto* str = new SliderRow(QObject::tr("Strength"), 0.05, 1, 0.01, 0.6, [](double x) { return QString::number(int(std::lround(x * 100))) + "%"; }); lv->addWidget(str);
      auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); lv->addWidget(bb);
      QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept); QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
      if (d.exec() != QDialog::Accepted) return;
      s->ensureDoc(); const int W = s->doc.width(), H = s->doc.height(); s->scalpReach = float(len->value());
      QApplication::setOverrideCursor(Qt::WaitCursor); const QImage nearImg = vp->pvScalpNear(s->doc.activeSet(), W, H, len->value() * vp->pvSceneSize()); QApplication::restoreOverrideCursor();
      if (nearImg.isNull()) { ui::toast(QObject::tr("No texel of this UV set lies near the scalp")); return; }
      const int id = s->doc.addPaint(s->modeChannel(), QObject::tr("ScalpShadow"), s->layer); paint::Layer* l = s->doc.layer(id); if (!l) return;
      QImage img(W, H, QImage::Format_ARGB32); img.fill(0); const QColor c = s->color; const double k = str->value();
      for (int y = 0; y < H; y++) { const uchar* p = nearImg.constScanLine(y); QRgb* o = reinterpret_cast<QRgb*>(img.scanLine(y)); for (int x = 0; x < W; x++) if (p[x]) o[x] = qRgba(c.red(), c.green(), c.blue(), int(p[x] * k + 0.5)); }
      l->image = img; l->blend = Blend::Multiply; s->layer = id; s->edits++; after(); ui::toast(QObject::tr("Scalp shadow: a Multiply layer — paint it, erase it, change its opacity as any layer")); }, "pt_light");
    int n = 0;
    for (AdjustKind k : {AdjustKind::BrightContrast, AdjustKind::Levels, AdjustKind::Curves, AdjustKind::HueSat, AdjustKind::Invert, AdjustKind::Posterize, AdjustKind::Threshold, AdjustKind::GradientMap}) {
      if (n == 0 || n == 2 || n == 3) m->addSeparator();
      n++;
      item(m, adjustName(k), QObject::tr("An adjustment layer: changes everything under it in the map shown (in its group), through its mask"), [s, k, after] {
        if (mapInfo(s->modeChannel()).storage != Storage::Colour) { ui::toast(QObject::tr("Adjustments work on colour and mask maps — not on the bump or the flow")); return; }
        s->ensureDoc(); s->layer = s->doc.addAdjust(k, {}, s->layer, s->modeChannel()); after(); }); } }
  icoBtn("pt_psd", QObject::tr("Layers from a file: a texture made elsewhere — 3D-Coat, Photoshop, Substance — brought in over the active layer. A PSD: each of its layers with its name, blend and opacity (the ones empty in this UV set left out); a picture: one layer. Show the map and the UV set first"), [s, vp, after] {
    const QString f = QFileDialog::getOpenFileName(nullptr, QObject::tr("Layers from a file"), QSettings().value("paint/importDir").toString(), QObject::tr("Textures (*.psd *.png *.jpg *.jpeg *.tga *.bmp *.webp)"));
    if (f.isEmpty()) return; QSettings().setValue("paint/importDir", QFileInfo(f).absolutePath());
    QApplication::setOverrideCursor(Qt::WaitCursor); const QString r = s->importLayers(f, layerSizeAsk(s, vp)); QApplication::restoreOverrideCursor(); ui::toast(r); after(); });
  icoBtn("rc_group_new", QObject::tr("New group: layers put into it blend as one"), [s, after] { s->ensureDoc(); s->layer = s->doc.addGroup({}, s->layer); after(); });
  { QMenu* m = menuBtn("rc_mask", QObject::tr("The layer's mask: add, invert, apply, remove"));
    item(m, QObject::tr("Add a white mask"), QObject::tr("All shown: paint it black to hide, white to show again (Erase hides too)"), [s, after] { s->doc.addMask(s->layer, true); after(); });
    item(m, QObject::tr("Add a black mask"), QObject::tr("All hidden"), [s, after] { s->doc.addMask(s->layer, false); after(); });
    m->addSeparator();
    item(m, QObject::tr("Invert the mask"), {}, [s, after] { s->doc.invertMask(s->layer); after(); });
    item(m, QObject::tr("Apply the mask"), QObject::tr("Burn the mask into the layer's alpha"), [s, after] { const Layer* l = s->active(); if (!l || l->mask.isNull()) return; s->layersEdit([s] { s->doc.applyMask(s->layer); const Layer* y = s->active(); return y && y->mask.isNull(); }); if (!s->active() || s->active()->mask.isNull()) s->editMask = s->showMask = false; after(); });
    item(m, QObject::tr("Remove the mask"), {}, [s, after] { const Layer* l = s->active(); if (!l || l->mask.isNull()) return; s->layersEdit([s] { s->doc.removeMask(s->layer); return true; }); s->editMask = false; s->showMask = false; after(); }); }
  tb->addSpacing(6);
  icoBtn("rc_layer_up", QObject::tr("Move the layer up"), [s, after] { s->doc.move(s->layer, 1); after(); });
  icoBtn("rc_layer_down", QObject::tr("Move the layer down"), [s, after] { s->doc.move(s->layer, -1); after(); });
  icoBtn("rc_layer_dup", QObject::tr("Duplicate the layer"), [s, after] { const int n = s->doc.duplicate(s->layer); if (n >= 0) s->layer = n; after(); });
  icoBtn("rc_merge", QObject::tr("Merge down: onto the colour paint layer under (both colour, same group)"), [s, after] { int into = -1; if (!s->layersEdit([s, &into] { return s->doc.mergeDown(s->layer, &into); })) ui::toast(QObject::tr("Merge down: both layers must be colour paint layers")); else { s->layer = into; after(); } });
  icoBtn("pt_smartname", QObject::tr("Smart rename: the layer named after where it lies and what it is — the layers under it give the stem, its blend or kind the tag: a layer over Sleeves and SleevesColor in Overlay becomes SleevesOver_1"), [s] {
    paint::Layer* l = s->active(); if (!l) return; const QString n = s->doc.smartName(l->id); if (n.isEmpty()) return; l->name = n; emit s->layersChanged(); ui::toast(QObject::tr("Renamed: %1").arg(n)); });
  icoBtn("rc_delete", QObject::tr("Delete the layer (a group — with its layers)"), [s, after] { if (!s->active()) return; deleteLayer(s, s->layer); after(); });
  tb->addStretch(1);


  auto* props = new QWidget; auto* pv = new QVBoxLayout(props); pv->setContentsMargins(0, 4, 0, 0); pv->setSpacing(4); v->addWidget(props);
  v->addLayout(tb);   // (the layers' buttons at the very bottom of the panel, the author, 17.11)

  auto buildProps = [s, vp, props, pv, after, building] {
    *building = true;
    while (QLayoutItem* it = pv->takeAt(0)) { if (it->widget()) { it->widget()->hide(); it->widget()->deleteLater(); } else if (it->layout()) { while (QLayoutItem* j = it->layout()->takeAt(0)) { if (j->widget()) { j->widget()->hide(); j->widget()->deleteLater(); } delete j; } } delete it; }   /* (hidden at once: a widget waiting to be deleted is still drawn) */
    Layer* l = s->active();
    if (l) {
      if (l->kind == LayerKind::Paint && (l->blend == Blend::Overlay || l->blend == Blend::SoftLight || l->blend == Blend::HardLight || l->blend == Blend::VividLight || l->blend == Blend::LinearLight || l->blend == Blend::PinLight)) {
        auto* ng = new QToolButton; ng->setText(QObject::tr("Brush ← neutral gray")); ng->setFocusPolicy(Qt::NoFocus); ng->setToolTip(QObject::tr("The brush takes 50 % gray — the colour that changes nothing in this blend: paint with it to take light or shade off again (X swaps back to the colour before)")); pv->addWidget(ng);
        QObject::connect(ng, &QToolButton::clicked, s, [s] { if (s->color != QColor(128, 128, 128)) s->color2 = s->color; s->color = QColor(128, 128, 128); emit s->brushChanged(); }); }
      if (l->kind == LayerKind::Fill) {
        if (l->channel == Channel::Height) { auto* fh = new SliderRow(QObject::tr("Relief"), -1, 1, 0.01, l->fillHeight, num); pv->addWidget(fh);
          QObject::connect(fh, &SliderRow::changed, s, [s](double x) { if (Layer* y = s->active()) { y->fillHeight = float(x); emit s->pixelsChanged({}); } }); }
        else if (l->gradLive && l->liveKind == QLatin1String("spheres")) {   // (the colour spheres: the list, the picked one's colour, radius, hardness, link)
          const double unit = vp ? std::max(1e-9, vp->pvSceneSize()) : 1.0;
          auto* sl = new QListWidget; sl->setFixedHeight(std::min(120, 22 + 18 * int(std::max<size_t>(1, l->spheres.size())))); sl->setFocusPolicy(Qt::NoFocus); pv->addWidget(sl);
          for (size_t i = 0; i < l->spheres.size(); i++) { const auto& sp = l->spheres[i]; QPixmap px(12, 12); px.fill(sp.col);
            const QString linkTxt = sp.link ? QObject::tr(" · link %1").arg(sp.link) : QString();
            sl->addItem(new QListWidgetItem(QIcon(px), QObject::tr("Sphere %1 · r %2 · hard %3").arg(i + 1).arg(QString::number(sp.r / unit * 100, 'f', 1) + "%").arg(QString::number(int(std::lround(sp.hard * 100))) + "%") + linkTxt)); }
          if (l->spheres.empty()) { auto* it = new QListWidgetItem(QObject::tr("No spheres yet: Fill tool → click the model")); it->setFlags(Qt::NoItemFlags); sl->addItem(it); }
          if (s->sphereSel >= 0 && s->sphereSel < int(l->spheres.size())) sl->setCurrentRow(s->sphereSel);
          QObject::connect(sl, &QListWidget::itemClicked, s, [s, sl, vp](QListWidgetItem*) { s->sphereSel = sl->currentRow(); if (vp) vp->paintOverlayChanged(); QTimer::singleShot(0, s, [s] { emit s->activeChanged(); }); });   // (the picked sphere's controls)
          if (s->sphereSel >= 0 && s->sphereSel < int(l->spheres.size())) { const auto sp = l->spheres[size_t(s->sphereSel)];
            // (a change to the picked sphere: every sphere of its link takes it too)
            auto apply = [s](std::function<void(paint::Layer::Sphere&)> f) { Layer* y = s->active(); if (!y || s->sphereSel < 0 || s->sphereSel >= int(y->spheres.size())) return;
              const int lk = y->spheres[size_t(s->sphereSel)].link; for (size_t i = 0; i < y->spheres.size(); i++) if (int(i) == s->sphereSel || (lk && y->spheres[i].link == lk)) f(y->spheres[i]); s->edits++; emit s->pixelsChanged({}); };
            auto* cb = new QPushButton(QObject::tr("Colour")); cb->setFocusPolicy(Qt::NoFocus); paintBtn(cb, sp.col); pv->addWidget(cb);
            QObject::connect(cb, &QPushButton::clicked, s, [s, cb, apply] { Layer* y = s->active(); if (!y || s->sphereSel < 0 || s->sphereSel >= int(y->spheres.size())) return;
              const QColor c = QColorDialog::getColor(y->spheres[size_t(s->sphereSel)].col, cb); if (!c.isValid()) return; paintBtn(cb, c); apply([c](paint::Layer::Sphere& x) { x.col = c; }); QTimer::singleShot(0, s, [s] { emit s->layersChanged(); }); });
            auto* row = new QHBoxLayout; row->setSpacing(3);
            auto sbtn = [&](const QString& t, const QString& tip, std::function<void()> f) { auto* b = new QToolButton; b->setText(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); row->addWidget(b); QObject::connect(b, &QToolButton::clicked, s, f); };
            sbtn(QObject::tr("Colour ← brush"), QObject::tr("The picked sphere (and its link) takes the brush's colour — V picks one off the model"), [s, apply] { const QColor c(s->color.red(), s->color.green(), s->color.blue()); apply([c](paint::Layer::Sphere& x) { x.col = c; }); QTimer::singleShot(0, s, [s] { emit s->layersChanged(); }); });
            sbtn(QObject::tr("Remove"), QObject::tr("The picked sphere removed (Ctrl + click on it with Fill does the same)"), [s, vp] { Layer* y = s->active(); if (!y || s->sphereSel < 0 || s->sphereSel >= int(y->spheres.size())) return; y->spheres.erase(y->spheres.begin() + s->sphereSel); s->sphereSel = std::min(s->sphereSel, int(y->spheres.size()) - 1); s->edits++; if (vp) vp->paintOverlayChanged(); emit s->pixelsChanged({}); QTimer::singleShot(0, s, [s] { emit s->layersChanged(); }); });
            row->addStretch(1); pv->addLayout(row);
            auto* rad = new SliderRow(QObject::tr("Radius"), 0.005, 0.6, 0.005, sp.r / unit, [](double x) { return QString::number(x * 100, 'f', 1) + "%"; }); rad->setTip(QObject::tr("The sphere's radius — a share of the model's size")); pv->addWidget(rad);
            QObject::connect(rad, &SliderRow::changed, s, [apply, unit, vp](double x) { apply([x, unit](paint::Layer::Sphere& y) { y.r = float(x * unit); }); if (vp) vp->paintOverlayChanged(); });
            auto* hard = new SliderRow(QObject::tr("Hardness"), 0, 0.99, 0.01, sp.hard, pct); hard->setTip(QObject::tr("Full colour up to this share of the radius, then a smooth fall to nothing at the radius")); pv->addWidget(hard);
            QObject::connect(hard, &SliderRow::changed, s, [apply, vp](double x) { apply([x](paint::Layer::Sphere& y) { y.hard = float(x); }); if (vp) vp->paintOverlayChanged(); });
            auto* lrow = new QHBoxLayout; lrow->addWidget(new QLabel(QObject::tr("Link"))); auto* lk = new QSpinBox; lk->setRange(0, 9); lk->setValue(sp.link); lk->setSpecialValueText(QObject::tr("none"));
            lk->setToolTip(QObject::tr("Spheres of one link change together: their colour, radius and hardness (each keeps its place). A sphere joining a link takes that link's look")); lrow->addWidget(lk, 1); pv->addLayout(lrow);
            QObject::connect(lk, &QSpinBox::valueChanged, s, [s](int v) { Layer* y = s->active(); if (!y || s->sphereSel < 0 || s->sphereSel >= int(y->spheres.size())) return; auto& me = y->spheres[size_t(s->sphereSel)]; me.link = v;
              if (v) for (size_t i = 0; i < y->spheres.size(); i++) if (int(i) != s->sphereSel && y->spheres[i].link == v) { me.col = y->spheres[i].col; me.r = y->spheres[i].r; me.hard = y->spheres[i].hard; break; }
              s->edits++; emit s->pixelsChanged({}); QTimer::singleShot(0, s, [s] { emit s->layersChanged(); }); }); }
          sl->setToolTip(QObject::tr("Fill tool: click the model — a sphere; drag from a sphere — move it; Ctrl + click — remove it")); }
        else if (l->gradLive) {                                   // (a live gradient: its stops; its ends — a drag with Fill (gradient) over the model)
          auto* gb = new GradientBar; gb->stops = l->grad.empty() ? std::vector<std::pair<float, QColor>>{{0.f, l->fill}, {1.f, QColor(0, 0, 0)}} : l->grad; pv->addWidget(gb);
          gb->setToolTip(QObject::tr("The live gradient: the left end's colour where the drag began on the model, the right end's where it ended. Click the bar — a new stop; drag a stop; double-click — its colour; right-click — remove it"));
          auto push = [s, gb] { if (Layer* y = s->active(); y && y->kind == LayerKind::Fill && y->gradLive) { y->grad = gb->stops; emit s->pixelsChanged({}); } };
          gb->changed = push;
          auto* row = new QHBoxLayout; row->setSpacing(3);
          auto gbtn = [&](const QString& t, const QString& tip, std::function<void()> f) { auto* b = new QToolButton; b->setText(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); row->addWidget(b); QObject::connect(b, &QToolButton::clicked, gb, f); };
          gbtn(QObject::tr("Stop ← brush colour"), QObject::tr("The selected stop takes the brush's colour: pick a colour off the model with V, then press this"), [s, gb, push] { if (gb->sel >= 0 && gb->sel < int(gb->stops.size())) { gb->stops[size_t(gb->sel)].second = QColor(s->color.red(), s->color.green(), s->color.blue()); gb->update(); push(); } });
          gbtn(QObject::tr("Brush ← stop"), QObject::tr("The brush takes the selected stop's colour"), [s, gb] { if (gb->sel >= 0 && gb->sel < int(gb->stops.size())) { s->color = gb->stops[size_t(gb->sel)].second; emit s->brushChanged(); } });
          gbtn(QObject::tr("Reverse"), QObject::tr("The gradient turned round: the colours change ends"), [gb, push] { for (auto& st : gb->stops) st.first = 1 - st.first; gb->update(); push(); });
          row->addStretch(1); pv->addLayout(row);
          // (its two points as 3D-Coat's Point1 / Point2: picked on the model, typed, swapped — the colours run from the start to the end)
          if (l->liveKind != QLatin1String("curve")) {
          for (int k = 1; k <= 2; k++) {
            auto* prow = new QHBoxLayout; prow->setSpacing(3);
            auto* pk = new QToolButton; pk->setText(k == 1 ? QObject::tr("Start") : QObject::tr("End")); pk->setCheckable(true); pk->setChecked(s->gradPick == k); pk->setFocusPolicy(Qt::NoFocus); pk->setFixedWidth(46);
            pk->setToolTip(k == 1 ? QObject::tr("Pick the start: the next click on the model puts it there (off the model — on the plane through it that faces you). The left end's colour is here") : QObject::tr("Pick the end: the next click on the model puts it there (off the model — on the plane through it that faces you). The right end's colour is here — earlier on the model, the colour ends earlier"));
            QObject::connect(pk, &QToolButton::toggled, s, [s, k, vp](bool on) { s->gradPick = on ? k : 0; if (vp) vp->paintOverlayChanged(); if (on) ui::toast(QObject::tr("Click the model where the point goes")); });
            prow->addWidget(pk);
            for (int a = 0; a < 3; a++) { auto* sb = new QDoubleSpinBox; sb->setRange(-1e6, 1e6); sb->setDecimals(2); sb->setButtonSymbols(QAbstractSpinBox::NoButtons); sb->setValue(k == 1 ? l->g0[a] : l->g1[a]); sb->setPrefix(a == 0 ? "X " : a == 1 ? "Y " : "Z "); sb->setKeyboardTracking(false);
              sb->setToolTip(QObject::tr("The point's place in the scene (its units): type a number — e.g. the same X and Z at both ends for a gradient straight up and down"));
              QObject::connect(sb, &QDoubleSpinBox::valueChanged, s, [s, k, a, vp](double v) { Layer* y = s->active(); if (!y || !y->gradLive) return; (k == 1 ? y->g0 : y->g1)[a] = float(v); s->edits++; emit s->pixelsChanged({}); if (vp) vp->paintOverlayChanged(); });
              prow->addWidget(sb, 1); }
            pv->addLayout(prow); }
          { auto* row2 = new QHBoxLayout; row2->setSpacing(3);
            auto* sw = new QToolButton; sw->setText(QObject::tr("Swap the points")); sw->setFocusPolicy(Qt::NoFocus); sw->setToolTip(QObject::tr("The start and the end change places — the colours run the other way along the model"));
            QObject::connect(sw, &QToolButton::clicked, s, [s, vp] { Layer* y = s->active(); if (!y || !y->gradLive) return; for (int i = 0; i < 3; i++) std::swap(y->g0[i], y->g1[i]); s->edits++; emit s->pixelsChanged({}); if (vp) vp->paintOverlayChanged(); QTimer::singleShot(0, s, [s] { emit s->layersChanged(); }); });
            auto* up = new QToolButton; up->setText(QObject::tr("Straight up and down")); up->setFocusPolicy(Qt::NoFocus); up->setToolTip(QObject::tr("The end put right under (or over) the start: the same X and Z — only the height runs the colours (3D-Coat: Point1 X = 0)"));
            QObject::connect(up, &QToolButton::clicked, s, [s, vp] { Layer* y = s->active(); if (!y || !y->gradLive) return; y->g1[0] = y->g0[0]; y->g1[2] = y->g0[2]; s->edits++; emit s->pixelsChanged({}); if (vp) vp->paintOverlayChanged(); QTimer::singleShot(0, s, [s] { emit s->layersChanged(); }); });
            row2->addWidget(sw); row2->addWidget(up); row2->addStretch(1); pv->addLayout(row2); }
          } else gb->setToolTip(QObject::tr("The strand gradient: the left end's colour at the curve's start (the root), the right end's at its end. Click the bar — a new stop; drag a stop; double-click — its colour; right-click — remove. A new curve: Fill → Along a curve, draw over the model with this layer active"));
          gb->setToolTip(gb->toolTip() + QObject::tr("\nOn the model (Fill in hand): the start — a double ring, the end — a single one; drag a ring to move that point, drag elsewhere to set both")); }
        else { auto* cb = new QPushButton(l->channel == Channel::Flow ? QObject::tr("Direction (r, g)") : QObject::tr("Colour")); cb->setFocusPolicy(Qt::NoFocus); paintBtn(cb, l->fill); pv->addWidget(cb);
          QObject::connect(cb, &QPushButton::clicked, s, [s, cb] { Layer* y = s->active(); if (!y) return; const QColor c = QColorDialog::getColor(y->fill, cb); if (c.isValid()) { y->fill = c; paintBtn(cb, c); emit s->pixelsChanged({}); } }); }
      }
      if (!l->fx.isEmpty()) {                                     // (an effect layer: its settings; the pixels made again a moment after any change)
        const QString type = l->fx.value("type").toString(); const Layer* src = s->doc.layer(l->fx.value("src").toInt(-1));
        QStringList casters; for (const auto& v : l->fx.value("casters").toArray()) casters << v.toString();
        auto* who = new QLabel(type == "shadow" ? QObject::tr("Shadow cast by %1").arg(casters.join(", ")) : QObject::tr("%1 of %2").arg(QObject::tr(fxName(type).toUtf8().constData()), src ? src->name : QObject::tr("(the source is gone)")));
        who->setWordWrap(true); who->setStyleSheet("color:#9aa"); pv->addWidget(who);
        auto setV = [s](const char* k, const QJsonValue& v) { if (Layer* y = s->active(); y && !y->fx.isEmpty()) { y->fx.insert(k, v); s->edits++; s->requestFx(120); } };
        auto num2 = [](double x) { return QString::number(x, 'f', 1) + " px"; };
        auto slide = [&](const char* label, const char* key, double lo, double hi, double st, double def) { auto* sr = new SliderRow(QObject::tr(label), lo, hi, st, l->fx.value(key).toDouble(def), num2); pv->addWidget(sr);
          QObject::connect(sr, &SliderRow::changed, s, [setV, key](double x) { setV(key, x); }); };
        auto colour = [&](const char* label, const char* key, const char* def) { colourBtn(pv, QObject::tr(label), QColor(l->fx.value(key).toString(def)), [setV, key](const QColor& c) { setV(key, c.name()); }); };
        if (type == "stroke") {
          slide("Width", "size", 0.5, 64, 0.5, 6);
          auto* where = new QComboBox; where->setFocusPolicy(Qt::NoFocus); for (const char* n : {"Inside", "Outside", "Centre"}) where->addItem(QObject::tr(n));
          const QString ps = l->fx.value("pos").toString("inside"); where->setCurrentIndex(ps == "outside" ? 1 : ps == "center" ? 2 : 0); pv->addWidget(where);
          QObject::connect(where, &QComboBox::currentIndexChanged, s, [setV](int i) { setV("pos", i == 1 ? "outside" : i == 2 ? "center" : "inside"); });
          colour("Colour (the side toward the light)", "c1", "#74674c");
          auto* g = check(pv, QObject::tr("Gradient on the stroke: away from the light — the second colour"), l->fx.value("grad").toBool()); QObject::connect(g, &QCheckBox::toggled, s, [setV](bool on) { setV("grad", on); });
          colour("Second colour (away from the light)", "c2", "#2e261c");
        } else if (type == "inner") {
          slide("Distance", "dist", 0, 32, 0.5, 4); slide("Size", "size", 0.5, 64, 0.5, 8); colour("Shadow colour", "c1", "#4a2e22");
        } else if (type == "shadow") {
          auto deg = [](double x) { return QString::number(x, 'f', 1) + QString::fromUtf8("\u00b0"); };
          { auto* sr = new SliderRow(QObject::tr("Softness"), 0, 20, 0.5, l->fx.value("soft").toDouble(3), deg); sr->setTip(QObject::tr("How wide the light is: the shadow's edge softens the farther it falls from its caster")); pv->addWidget(sr);
            QObject::connect(sr, &SliderRow::changed, s, [setV](double x) { setV("soft", x); }); }
          { auto* sr = new SliderRow(QObject::tr("Quality"), 4, 64, 1, l->fx.value("rays").toDouble(16), [](double x) { return QString::number(int(x)) + QObject::tr(" rays"); }); sr->setTip(QObject::tr("Rays per point: more — a smoother soft edge, slower")); pv->addWidget(sr);
            QObject::connect(sr, &SliderRow::changed, s, [setV](double x) { setV("rays", int(x)); }); }
          colour("Shadow colour", "c1", "#6a5a7a");
          auto* take = new QToolButton; take->setText(QObject::tr("Casters ← selected")); take->setFocusPolicy(Qt::NoFocus);
          take->setToolTip(QObject::tr("The objects selected in the outliner cast this shadow from now on"));
          QObject::connect(take, &QToolButton::clicked, s, [s, vp, after] { if (!vp) return; QStringList n; const Scene* sc = vp->scene();
            for (int id : vp->selection().items) if (const Item* it = sc ? sc->item(id) : nullptr; it && it->type == ItemType::IMesh && it->poly && !n.contains(it->name)) n << it->name;
            if (n.isEmpty()) { ui::toast(QObject::tr("Select the casting objects in the outliner first")); return; }
            if (Layer* y = s->active(); y && !y->fx.isEmpty()) { y->fx.insert("casters", QJsonArray::fromStringList(n)); s->edits++; s->requestFx(0); after(); } });
          pv->addWidget(take);
        } else if (type == "shine") {
          auto frac = [](double x) { return QString::number(x, 'f', 2); };
          for (auto [label, key, lo, hi, def] : {std::tuple<const char*, const char*, double, double, double>{"Roughness", "rough", 0.02, 1, 0.3}, {"Metal", "metal", 0, 1, 0.8}, {"Strength", "power", 0, 3, 1}}) {
            auto* sr = new SliderRow(QObject::tr(label), lo, hi, 0.01, l->fx.value(key).toDouble(def), frac); pv->addWidget(sr); QObject::connect(sr, &SliderRow::changed, s, [setV, key](double x) { setV(key, x); }); }
          colour("Metal's colour", "c1", "#ffd98a");
          auto* fromView = new QToolButton; fromView->setText(QObject::tr("From this view")); fromView->setFocusPolicy(Qt::NoFocus);
          fromView->setToolTip(QObject::tr("The highlight as the model is seen now: the camera's direction kept in the effect (turn the model, press again — the highlight moves)"));
          QObject::connect(fromView, &QToolButton::clicked, s, [vp, setV] { if (!vp) return; const Vec3 d = vp->camera().dir(); setV("vd", QJsonArray{-d.x, -d.y, -d.z}); });
          pv->addWidget(fromView);
        } else { slide("Size", "size", 0.5, 64, 0.5, 8); colour("Lit side", "c1", "#ffffff"); colour("Dark side", "c2", "#000000"); }
        { auto* az = new SliderRow(QObject::tr("Light: round"), -180, 180, 1, l->fx.value("az").toDouble(30), [](double x) { return QString::number(int(x)) + QString::fromUtf8("\u00b0"); }); pv->addWidget(az);
          QObject::connect(az, &SliderRow::changed, s, [setV](double x) { setV("az", x); });
          auto* el = new SliderRow(QObject::tr("Light: height"), 0, 90, 1, l->fx.value("el").toDouble(55), [](double x) { return QString::number(int(x)) + QString::fromUtf8("\u00b0"); }); pv->addWidget(el);
          QObject::connect(el, &SliderRow::changed, s, [setV](double x) { setV("el", x); }); }
        auto* bake = new QToolButton; bake->setText(QObject::tr("Bake into an ordinary layer")); bake->setFocusPolicy(Qt::NoFocus);
        bake->setToolTip(QObject::tr("The effect's pixels kept as they are now; the layer no longer follows its source — paint on it as on any layer"));
        QObject::connect(bake, &QToolButton::clicked, s, [s, after] { if (Layer* y = s->active(); y && !y->fx.isEmpty()) { y->fx = {}; s->edits++; after(); ui::toast(QObject::tr("Baked: an ordinary paint layer now")); } });
        pv->addWidget(bake);
      }
      if (l->kind == LayerKind::Adjust) {
        struct P { const char* n; double lo, hi, st; float Layer::*m; };
        std::vector<P> ps;
        switch (l->adjust) {
          case AdjustKind::BrightContrast: ps = {{"Brightness", -1, 1, 0.01, &Layer::p1}, {"Contrast", -1, 1, 0.01, &Layer::p2}}; break;
          case AdjustKind::HueSat: ps = {{"Hue", -180, 180, 1, &Layer::p1}, {"Saturation", -1, 1, 0.01, &Layer::p2}, {"Lightness", -1, 1, 0.01, &Layer::p3}}; break;
          case AdjustKind::Levels: ps = {{"Black", 0, 1, 0.01, &Layer::p1}, {"White", 0, 1, 0.01, &Layer::p2}, {"Gamma", 0.1, 4, 0.01, &Layer::p3}}; break;
          case AdjustKind::Threshold: ps = {{"Level", 0, 1, 0.01, &Layer::p1}}; break;
          case AdjustKind::Posterize: ps = {{"Steps", 2, 32, 1, &Layer::p1}}; break;
          default: break;
        }
        for (const P& p : ps) { auto* sr = new SliderRow(QObject::tr(p.n), p.lo, p.hi, p.st, l->*p.m, num); pv->addWidget(sr);
          QObject::connect(sr, &SliderRow::changed, s, [s, m = p.m](double x) { if (Layer* y = s->active()) { y->*m = float(x); emit s->pixelsChanged({}); } }); }
        if (l->adjust == AdjustKind::Curves) {                   // (Curves: the channel picked — RGB, R, G, B — and its curve, as Photoshop's)
          auto* chs = new QComboBox; chs->setFocusPolicy(Qt::NoFocus); for (const char* n : {"RGB", "Red", "Green", "Blue"}) chs->addItem(QObject::tr(n));
          auto* ce = new CurveEdit; ce->setFixedSize(220, 180);
          ce->setToolTip(QObject::tr("Across — the tone under the layer, up — what it becomes. Drag a point; a double click adds one; right click takes it off"));
          auto chan = std::make_shared<int>(0);
          ce->get = [s, chan] { Curve c; if (const Layer* y = s->active(); y && y->kind == LayerKind::Adjust && !y->tones[size_t(*chan)].empty()) c.pts = y->tones[size_t(*chan)]; return c; };
          ce->set = [s, chan](const Curve& c) { if (Layer* y = s->active(); y && y->kind == LayerKind::Adjust) { y->tones[size_t(*chan)] = c.linear() ? std::vector<QPointF>() : c.pts; s->edits++; emit s->pixelsChanged({}); } };
          QObject::connect(chs, &QComboBox::currentIndexChanged, ce, [ce, chan](int i) { *chan = std::clamp(i, 0, 3); ce->update(); });
          auto* rs = new QToolButton; rs->setText(QObject::tr("Straight")); rs->setFocusPolicy(Qt::NoFocus); rs->setToolTip(QObject::tr("This channel's curve made straight again — no change"));
          QObject::connect(rs, &QToolButton::clicked, ce, [s, ce, chan] { if (Layer* y = s->active(); y && y->kind == LayerKind::Adjust) { y->tones[size_t(*chan)].clear(); s->edits++; emit s->pixelsChanged({}); ce->update(); } });
          auto* row = new QHBoxLayout; row->setSpacing(3); row->addWidget(chs); row->addWidget(rs); row->addStretch(1); pv->addLayout(row); pv->addWidget(ce); }
        if (l->adjust == AdjustKind::GradientMap) {              // (the gradient itself: its stops, the selected one's colour from the brush — V takes it off the model)
          auto* gb = new GradientBar; gb->stops = l->grad.empty() ? std::vector<std::pair<float, QColor>>{{0.f, QColor(0, 0, 0)}, {1.f, QColor(255, 255, 255)}} : l->grad; pv->addWidget(gb);
          auto push = [s, gb] { if (Layer* y = s->active(); y && y->kind == LayerKind::Adjust) { y->grad = gb->stops; emit s->pixelsChanged({}); } };
          gb->changed = push;
          auto* row = new QHBoxLayout; row->setSpacing(3);
          auto gbtn = [&](const QString& t, const QString& tip, std::function<void()> f) { auto* b = new QToolButton; b->setText(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); row->addWidget(b); QObject::connect(b, &QToolButton::clicked, gb, f); };
          gbtn(QObject::tr("Stop ← brush colour"), QObject::tr("The selected stop takes the brush's colour: pick a colour off the model with V, then press this"), [s, gb, push] { if (gb->sel >= 0 && gb->sel < int(gb->stops.size())) { gb->stops[size_t(gb->sel)].second = QColor(s->color.red(), s->color.green(), s->color.blue()); gb->update(); push(); } });
          gbtn(QObject::tr("Brush ← stop"), QObject::tr("The brush takes the selected stop's colour"), [s, gb] { if (gb->sel >= 0 && gb->sel < int(gb->stops.size())) { s->color = gb->stops[size_t(gb->sel)].second; emit s->brushChanged(); } });
          gbtn(QObject::tr("Reverse"), QObject::tr("The gradient turned round: dark takes the light end's colours"), [gb, push] { for (auto& st : gb->stops) st.first = 1 - st.first; gb->update(); push(); });
          row->addStretch(1); pv->addLayout(row); }
      }
    }
    *building = false;
  };

  auto refill = [s, list, building, buildProps, topBl, topOp] {
    *building = true; list->clear();
    // (the base of a clipping mask: the nearest layer above it in the same place — of this map — is clipped)
    auto clipBase = [&](int id) { const auto& L = s->doc.layers(); const int i0 = s->doc.indexOf(id); if (i0 < 0) return false; const int par = L[size_t(i0)].parent;
      for (int j = i0 + 1; j < int(L.size()); j++) { const Layer& u = L[size_t(j)]; if (u.parent != par) continue; if (u.kind != LayerKind::Group && u.channel != L[size_t(i0)].channel) continue; return u.clip; } return false; };
    std::function<void(int, int)> add = [&](int parent, int depth) {
      const auto& L = s->doc.layers();
      for (int i = int(L.size()) - 1; i >= 0; i--) {
        const Layer& l = L[size_t(i)]; if (l.parent != parent) continue;
        if (l.kind != LayerKind::Group && !s->view.startsWith("bake:") && l.channel != s->modeChannel()) continue;   // (the mode's own layers; the groups hold every map's)
        const bool empty = l.kind == LayerKind::Paint && l.image.isNull();                                             // (not painted in this UV set)
        const bool blank = !empty && s->doc.isBlank(l);                                                                // (it has a picture here — with nothing on it)
        auto* it = new QListWidgetItem(l.name);
        QStringList badge; if (l.blend != Blend::Normal && l.kind != LayerKind::Adjust) badge << blendName(l.blend); if (l.opacity < 0.995f) badge << QString::number(int(std::lround(l.opacity * 100))) + "%";
        if (empty) badge << QString::fromUtf8("·"); else if (blank) badge << QString::fromUtf8("∅");
        it->setData(RClip, l.clip); it->setData(RBase, !l.clip && clipBase(l.id));
        it->setData(RId, l.id); it->setData(RDepth, depth); it->setData(RVisible, l.visible); it->setData(RBadge, badge.join(" ")); it->setData(RDim, empty || blank);
        it->setData(RTag, l.kind == LayerKind::Group ? QString::fromUtf8("▸") : l.kind == LayerKind::Adjust ? QString::fromUtf8("◐") : QString());
        const QPixmap pm = layerThumb(l); if (!pm.isNull()) it->setData(RThumb, pm);
        if (!l.mask.isNull()) { it->setData(RMask, maskThumb(l)); it->setData(RMaskEdit, l.id == s->layer && s->editMask); }
        it->setFlags(it->flags() | Qt::ItemIsEditable);
        it->setToolTip(l.name + "\n" + (blank ? QObject::tr("∅ — an empty layer: nothing is painted on it\n") : empty ? QObject::tr("· — nothing painted in this UV set (the layer may hold paint in another set)\n") : QString())
                       + QObject::tr("The eye — shown / hidden · double click on the name — rename · the mask's picture: a click — paint the mask, a double click (Alt + click) — see it · Alt + click on the line between two layers — a clipping mask · RMB — more"));
        list->addItem(it);
        if (l.id == s->layer) list->setCurrentItem(it);
        if (l.kind == LayerKind::Group) add(l.id, depth + 1);
      } };
    add(-1, 0);
    if (!list->count()) { auto* it = new QListWidgetItem(QObject::tr("No layers in this map yet — ＋ Paint")); it->setFlags(Qt::NoItemFlags); list->addItem(it); }
    { const Layer* a = s->active(); QSignalBlocker b1(topBl); topBl->setEnabled(a && a->kind != LayerKind::Adjust); topOp->setEnabled(a != nullptr);
      if (a) { topBl->setCurrentIndex(topBl->findData(int(a->blend))); QSignalBlocker b2(topOp); topOp->setValue(int(std::lround(a->opacity * 100))); }
}
    *building = false; buildProps();
  };
  // (right-click on a layer: its own commands — a fill or live layer made an ordinary paint layer)
  list->setContextMenuPolicy(Qt::CustomContextMenu);
  { auto* sc = new QShortcut(QKeySequence("Ctrl+Alt+G"), w); sc->setContext(Qt::ApplicationShortcut); QObject::connect(sc, &QShortcut::activated, s, [s] { if (s->active()) toggleClip(s, s->layer); }); }   // (Photoshop's key for it)
  QObject::connect(list, &QListWidget::customContextMenuRequested, w, [s, list, after, vp](const QPoint& at) {
    QListWidgetItem* it = list->itemAt(at); if (!it || !it->data(Qt::UserRole).isValid()) return; const int id = it->data(Qt::UserRole).toInt(); s->setLayer(id);
    const Layer* l = s->doc.layer(id); if (!l) return;
    QMenu m; m.setToolTipsVisible(true);
    { QAction* mc = m.addAction(QObject::tr("Match colour to the layers below")); mc->setEnabled(l->kind == LayerKind::Paint && mapInfo(l->channel).storage == Storage::Colour);
      QObject::connect(mc, &QAction::triggered, s, [s, id, after] { QApplication::setOverrideCursor(Qt::WaitCursor); const QString r = s->matchColourBelow(id); QApplication::restoreOverrideCursor(); ui::toast(r); after(); }); }
    { QMenu* fxm = m.addMenu(QObject::tr("Effects"));   // (live, made from this layer / group: a layer of their own over it)
      const bool can = (l->kind == LayerKind::Paint || l->kind == LayerKind::Group) && l->fx.isEmpty(); fxm->setEnabled(can);
      for (const char* t : {"stroke", "inner", "bevel", "shine"}) QObject::connect(fxm->addAction(QObject::tr(fxName(t).toUtf8().constData())), &QAction::triggered, s, [s, id, t, after, vp] {
        const int nf = s->addEffect(id, t);
        if (nf >= 0 && QString(t) == "shine" && vp) if (Layer* y = s->doc.layer(nf)) { const Vec3 d = vp->camera().dir(); y->fx.insert("vd", QJsonArray{-d.x, -d.y, -d.z}); y->fx.insert("az", std::round(std::atan2(-d.x, -d.z) * 180 / M_PI) + 30); y->fx.insert("el", 40); s->requestFx(0); }   // (seen from this view, the light from the viewer's upper side)
        if (nf < 0) ui::toast(QObject::tr("Effects go on a colour paint layer or a group")); else { after(); ui::toast(QObject::tr("%1: a layer over the source, made again whenever it changes — its settings in the Layers panel").arg(QObject::tr(fxName(t).toUtf8().constData()))); } }); }
    QAction* conv = m.addAction(QObject::tr("Convert to a paint layer")); conv->setEnabled(l->kind == LayerKind::Fill && mapInfo(l->channel).storage == Storage::Colour);
    conv->setToolTip(QObject::tr("The layer as it shows now — a live gradient, colour spheres, a fill — becomes an ordinary paint layer to paint on, in every UV set it shows in; its blend and opacity kept. Not live any more"));
    QObject::connect(conv, &QAction::triggered, s, [s, id, after] { QApplication::setOverrideCursor(Qt::WaitCursor); const bool ok = s->layersEdit([s, id] { return s->doc.rasterize(id); }); QApplication::restoreOverrideCursor();
      if (!ok) { ui::toast(QObject::tr("Only a fill layer of a colour or mask map becomes a paint layer")); return; } s->edits++; after(); ui::toast(QObject::tr("Now an ordinary paint layer")); });
    m.addSeparator();
    { QAction* cl = m.addAction(layerFlagIcon(1), l->clip ? QObject::tr("Release Clipping Mask") : QObject::tr("Create Clipping Mask")); cl->setShortcut(QKeySequence("Ctrl+Alt+G")); cl->setShortcutVisibleInContextMenu(true);
      QObject::connect(cl, &QAction::triggered, s, [s, id] { toggleClip(s, id); }); }
    m.addSeparator();
    QObject::connect(m.addAction(QObject::tr("Layers from another project…")), &QAction::triggered, s, [s, list, after] { layersFromFile(s, list->window(), false, after); });
    QObject::connect(m.addAction(QObject::tr("Layers from an autosave…")), &QAction::triggered, s, [s, list, after] { layersFromFile(s, list->window(), true, after); });
    m.addSeparator();
    QObject::connect(m.addAction(QObject::tr("Duplicate")), &QAction::triggered, s, [s, id, after] { const int n = s->doc.duplicate(id); if (n >= 0) s->layer = n; after(); });
    { QAction* sp = m.addAction(QObject::tr("Split into shape and colour")); sp->setEnabled(l->kind == LayerKind::Paint && l->fx.isEmpty() && mapInfo(l->channel).storage == Storage::Colour);
      sp->setToolTip(QObject::tr("The layer becomes its shape (\"…Mask\"), a copy clipped over it keeps the name and the colour: repaint the colour with any brush — it stays inside the shape; edit the shape on the base"));
      QObject::connect(sp, &QAction::triggered, s, [s, id, after] { const int c = s->splitShapeColour(id); if (c < 0) ui::toast(QObject::tr("Only a colour paint layer splits into a shape and a colour")); else { after(); ui::toast(QObject::tr("The shape below, the colour clipped over it — the colour layer is active")); } }); }
    QObject::connect(m.addAction(QObject::tr("Delete")), &QAction::triggered, s, [s, id, after] { deleteLayer(s, id); after(); });
    m.exec(list->viewport()->mapToGlobal(at)); });
  QObject::connect(s, &PaintSession::viewChanged, w, [syncMode, fillSets] { syncMode(); fillSets(); });
  w->onShow = [syncMode, fillSets] { syncMode(); fillSets(); };
  syncMode(); fillSets();
  QObject::connect(s, &PaintSession::layersChanged, w, refill);
  QObject::connect(s, &PaintSession::activeChanged, w, refill);
  QObject::connect(list, &QListWidget::currentItemChanged, w, [s, building](QListWidgetItem* it, QListWidgetItem*) { if (*building || !it || !it->data(Qt::UserRole).isValid()) return; s->setLayer(it->data(Qt::UserRole).toInt()); });
  // (the rows' pictures after painting: made again a moment after the strokes stop — not at every dab)
  { auto pend = std::make_shared<bool>(false);
    QObject::connect(s, &PaintSession::pixelsChanged, list, [s, list, pend](const QRect&) { if (*pend) return; *pend = true;
      QTimer::singleShot(450, list, [s, list, pend] { *pend = false; for (int r = 0; r < list->count(); r++) { QListWidgetItem* it = list->item(r); if (!it->data(RId).isValid()) continue;
          if (const Layer* l = s->doc.layer(it->data(RId).toInt())) { const QPixmap pm = layerThumb(*l); it->setData(RThumb, pm.isNull() ? QVariant() : QVariant(pm)); } } }); }); }

  // every map that has layers, written as the engines take them: <name>_<UV set>_<Map>.png
  addHelp(w, QObject::tr("• The eye — show / hide the layer; Alt + click on the eye — only this layer shown (again — all back)\n• Ctrl + click on a layer's picture — where it has paint: it alone shown while the button is held\n• Ctrl + Shift + click on the model or the texture — the layer painted there becomes the active one\n• Right click → Match colour to the layers below — the layer's tones fitted to what lies under it (an AI pass over an old painting, say)\n• Double click on the name — rename it in its row\n• The mask is the second picture in the row (the mask button below adds it):\n    a click on the mask — the brush paints the mask: black hides the layer, white shows it\n    a click on the layer's picture — back to painting the layer\n    a double click (or Alt + click) on the mask — the mask itself shown in place of the map; again — back\n• Clipping mask, as Photoshop: hold Alt over the line between two layers and click — the upper one is seen only where the one under it has paint (it moves right, with an arrow; the base's name is underlined). Alt + click on the same line again — released. Also: right click → Create / Release Clipping Mask, Ctrl+Alt+G\n• Right click on a layer — make it an ordinary paint layer, duplicate, delete\n• Right click → Layers from another project / an autosave — tick its layers: they come over the active one with their names, blends, clipping, masks and pixels\n• Curves (an adjustment): RGB or one channel; drag a point, double click — a new one, right click — off\n• Right click → Effects: Stroke (inside / outside / centre, a gradient on it by the light), Inner shadow, Bevel, Shine (the highlight the light gives that part with its roughness and metal, as seen from the view — From this view moves it) — on a layer or a group (one stroke round everything in it); a layer of its own over the source with its own blend, made again a moment after the source changes; the light is set in 3D (round / height) and lies on every UV island as on the model; no stroke along UV seams. Bake — an ordinary layer\n• Right click → Split into shape and colour — the layer becomes its shape (…Mask), a copy clipped over it holds the colour: repaint it with any brush, it stays inside\n• Object shadow (the fill / adjustment button) — the shadow the objects selected in the outliner (else the ones named hair) cast onto this UV set: its own light, softness, quality and colour; hidden objects cast too; Bake — an ordinary layer"));

  refill();
  return w;
}

std::function<int(int, int)> layerSizeAsk(PaintSession* s, Viewport* vp) {
  return [s, vp](int fw, int fh) -> int {
    const QString set = s->doc.activeSet();
    QMessageBox q(QMessageBox::Question, QObject::tr("Layers from a file"), QObject::tr("The file is %1×%2, the texture of %3 is %4×%5.").arg(fw).arg(fh).arg(set.isEmpty() ? QObject::tr("the single texture") : set).arg(s->doc.width()).arg(s->doc.height()));
    QPushButton* up = set.isEmpty() || !vp ? nullptr : q.addButton(QObject::tr("Make the texture %1×%2").arg(fw).arg(fh), QMessageBox::AcceptRole);
    QPushButton* down = q.addButton(QObject::tr("Scale the file down"), QMessageBox::DestructiveRole); q.addButton(QMessageBox::Cancel);
    if (up) { up->setToolTip(QObject::tr("The UV set's texture size raised — the layers already painted in it scaled up with it")); q.setDefaultButton(up); }
    QApplication::restoreOverrideCursor(); q.exec(); QApplication::setOverrideCursor(Qt::WaitCursor);
    if (up && q.clickedButton() == up) { vp->setUvSetSize(set, fw, fh); return 1; }
    return q.clickedButton() == down ? 0 : -1; };
}

void exportPaintMaps(PaintSession* s, QWidget* parent) {
    if (s->doc.width() == 0) { ui::toast(QObject::tr("Nothing to export")); return; }
    const QString d = QFileDialog::getExistingDirectory(parent, QObject::tr("Export the maps into")); if (d.isEmpty()) return;
    // (every UV set that has pixels — each shown in turn, then the one that was shown put back)
    const QString cur = s->doc.activeSet(); const int cw = s->doc.width(), chh = s->doc.height(); int n = 0;
    std::vector<Document::SetInfo> sets = s->doc.sets(); if (sets.empty()) sets.push_back({cur, cw, chh});
    for (const auto& x : sets) {
      s->doc.setActiveSet(x.key, x.w, x.h);
      QString set = x.key; set.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_"); const QString pre = d + "/" + (set.isEmpty() ? QString("Texture") : set) + "_";
      for (const MapInfo& m : maps()) {
        if (m.ch == Channel::Height || m.ch == Channel::BakeMask || !s->doc.hasLayers(m.ch)) continue;
        bool pixels = false; for (const Layer& l : s->doc.layers()) if (l.channel == m.ch && l.visible && l.kind == LayerKind::Paint && !l.image.isNull()) pixels = true;
        if (!pixels && sets.size() > 1) continue;                  // (only fills in this set: nothing of its own to write)
        const QImage im = s->doc.flatten(m.ch, m.ch == Channel::Color); if (!im.isNull() && im.save(pre + m.suffix + ".png")) n++; }
      if (const QImage nm = s->normalResult(); !nm.isNull() && s->doc.hasLayers(Channel::Height) && nm.save(pre + "Normal.png")) n++;
    }
    s->doc.setActiveSet(cur, cw, chh);
    ui::toast(n ? QObject::tr("%1 maps written into %2").arg(n).arg(d) : QObject::tr("No map has layers yet")); 
}

// ================= Brush =================

namespace {
// the pressure curve: a small square, the points dragged; a double click adds one, the right button takes one off (the ends stay)
QLabel* header(QVBoxLayout* v, const QString& t) { auto* l = new QLabel(t); l->setStyleSheet("font-weight:bold;margin-top:6px"); v->addWidget(l); return l; }

// the alpha's turn: the alpha itself on a round ground, turned as it paints — drag round it (Shift — 15° steps), the wheel — 5°, a double click — 0°
class AlphaDial : public QWidget {
public:
  std::function<float()> angle; std::function<void(float)> setAngle; std::function<QImage()> thumb;
  AlphaDial() { setFixedSize(68, 68); setCursor(Qt::SizeAllCursor); setFocusPolicy(Qt::NoFocus);
    setToolTip(QObject::tr("The alpha's turn: drag round the circle (Shift — 15° steps) · the wheel — 5° · double click — 0°")); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform); const QPointF c(width() / 2.0, height() / 2.0); const double R = width() / 2.0 - 2;
    p.setPen(QPen(QColor(0x4a, 0x4a, 0x4e), 1)); p.setBrush(QColor(0x1c, 0x1c, 0x1e)); p.drawEllipse(c, R, R);
    const float a = angle ? angle() : 0.f;
    if (thumb) { const QImage t = thumb(); if (!t.isNull()) { p.save(); p.translate(c); p.rotate(a); p.setClipRegion(QRegion(QRect(-int(R) + 2, -int(R) + 2, int(2 * R) - 4, int(2 * R) - 4), QRegion::Ellipse)); const double k = (R * 1.3) / std::max(t.width(), t.height());
        p.scale(k, k); p.drawImage(QPointF(-t.width() / 2.0, -t.height() / 2.0), t); p.restore(); } }
    const double r = a * M_PI / 180; p.setPen(QPen(QColor(0x94, 0x66, 0xbe), 2)); p.drawLine(c + QPointF(std::cos(r), std::sin(r)) * (R - 9), c + QPointF(std::cos(r), std::sin(r)) * R);
    p.setBrush(QColor(0xe8, 0xe8, 0xee)); p.setPen(Qt::NoPen); p.drawEllipse(c + QPointF(std::cos(r), std::sin(r)) * (R - 4), 3.2, 3.2); }
  void put(const QPointF& w, Qt::KeyboardModifiers m) { const QPointF c(width() / 2.0, height() / 2.0); double a = std::atan2(w.y() - c.y(), w.x() - c.x()) * 180 / M_PI;
    if (m & Qt::ShiftModifier) a = std::round(a / 15) * 15; if (setAngle) setAngle(float(a)); update(); }
  void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) put(e->position(), e->modifiers()); }
  void mouseMoveEvent(QMouseEvent* e) override { if (e->buttons() & Qt::LeftButton) put(e->position(), e->modifiers()); }
  void mouseDoubleClickEvent(QMouseEvent*) override { if (setAngle) setAngle(0); update(); }
  void wheelEvent(QWheelEvent* e) override { if (!setAngle || !angle) return; float a = angle() + (e->angleDelta().y() > 0 ? 5.f : -5.f); if (a > 180) a -= 360; if (a < -180) a += 360; setAngle(a); update(); e->accept(); }
};
// (a picture that follows its width — the brush's stroke preview)
class WideLabel : public QLabel {
public:
  std::function<void()> resized;
protected:
  void resizeEvent(QResizeEvent* e) override { QLabel::resizeEvent(e); if (resized) resized(); }
};
}  // namespace

QWidget* makePaintBrushPanel(PaintSession* s, QWidget** listOut) {
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(8, 6, 8, 8); v->setSpacing(5);
  // (the brush list: in the panel, or a column of its own — the author's sketch, 17.11: tools | the tool's list | its settings)
  auto* lw = new QWidget; auto* lv = new QVBoxLayout(lw); lv->setContentsMargins(listOut ? 4 : 0, listOut ? 6 : 0, listOut ? 4 : 0, listOut ? 6 : 0); lv->setSpacing(4);
  auto* list = new QListWidget; list->setIconSize(QSize(120, 34)); list->setFocusPolicy(Qt::NoFocus); lv->addWidget(list, 1);
  if (listOut) { list->setMinimumHeight(120); list->setIconSize(QSize(64, 22)); *listOut = lw; }   // (a narrow column: a short stroke picture, the whole name)
  else { list->setFixedHeight(120); v->addWidget(lw); }
  // (the brush set's commands: compact icons, no words — what each does is in its tip)
  auto* rowB = new QHBoxLayout; rowB->setSpacing(3);
  auto setBtn = [&](const char* icon, const QString& tip) { auto* b = button({}, tip); b->setIcon(ui::icon(icon)); b->setIconSize(QSize(20, 20)); b->setStyleSheet("padding:2px 5px"); rowB->addWidget(b); return b; };
  auto* imp = setBtn("rc_brush_import", QObject::tr("Import ABR: Photoshop brushes (.abr) — every sampled tip in the file becomes a brush"));
  auto* save = setBtn("rc_brush_new", QObject::tr("Save as: keep the brush in hand (with all its settings) as a new one, under a name"));
  auto* ren = setBtn("pt_rename", QObject::tr("Rename the chosen kept brush")); auto* del = setBtn("rc_delete", QObject::tr("Remove the chosen kept brush"));
  auto* fav = setBtn("pt_star", QObject::tr("Favourite: the chosen brush's tip into the favourite alphas (the ring on A held)"));
  rowB->addStretch(1); lv->addLayout(rowB);

  auto fill = [s, list] {
    QSignalBlocker b(list); list->clear();
    for (const Brush& br : s->lib.brushes) { auto* it = new QListWidgetItem(QIcon(QPixmap::fromImage(br.preview(QSize(120, 34)))), br.name); it->setToolTip(BrushLibrary::setOf(br)); list->addItem(it); }
  };
  fill();
  QObject::connect(list, &QListWidget::currentRowChanged, w, [s](int r) { if (r < 0 || r >= int(s->lib.brushes.size())) return; s->brush = s->lib.brushes[size_t(r)]; s->alphaStandIn = false; s->alphaKey = "own"; emit s->brushChanged(); });
  QObject::connect(imp, &QPushButton::clicked, w, [s, fill, w] {
    const QStringList fs = QFileDialog::getOpenFileNames(w, QObject::tr("Import brushes"), {}, QObject::tr("Photoshop brushes (*.abr)")); int total = 0;
    for (const QString& f : fs) { QString err; const int n = s->lib.importAbr(f, &err); if (n < 0) ui::toast(QObject::tr("%1: %2").arg(QFileInfo(f).fileName(), err)); else total += n; }
    if (total) { fill(); ui::toast(QObject::tr("%1 brushes imported").arg(total)); } });
  QObject::connect(save, &QPushButton::clicked, w, [s, fill, w] {
    bool ok = false; const QString n = QInputDialog::getText(w, QObject::tr("Save the brush"), QObject::tr("Name"), QLineEdit::Normal, s->brush.name + " *", &ok);
    if (!ok || n.trimmed().isEmpty()) return; Brush b = s->brush; b.name = n.trimmed(); if (b.set == "Basic") b.set.clear(); s->lib.brushes.push_back(b); s->lib.save(); fill(); ui::toast(QObject::tr("Kept as \"%1\"").arg(b.name)); });
  QObject::connect(ren, &QPushButton::clicked, w, [s, list, fill, w] {
    const int r = list->currentRow(); if (r < s->lib.fixed() || r >= int(s->lib.brushes.size())) { ui::toast(QObject::tr("The built-in brushes and the preset pack keep their names")); return; }
    bool ok = false; const QString n = QInputDialog::getText(w, QObject::tr("Rename the brush"), QObject::tr("Name"), QLineEdit::Normal, s->lib.brushes[size_t(r)].name, &ok);
    if (!ok || n.trimmed().isEmpty()) return; s->lib.brushes[size_t(r)].name = n.trimmed(); s->lib.save(); fill(); });
  QObject::connect(del, &QPushButton::clicked, w, [s, list, fill] { const int r = list->currentRow(); if (r < s->lib.fixed() || r >= int(s->lib.brushes.size())) { ui::toast(QObject::tr("The built-in brushes stay")); return; } s->lib.brushes.erase(s->lib.brushes.begin() + r); s->lib.save(); fill(); });
  QObject::connect(fav, &QPushButton::clicked, w, [s, list] {
    const int r = list->currentRow(); if (r < 0 || r >= int(s->lib.brushes.size())) { ui::toast(QObject::tr("Choose a brush in the list first")); return; }
    const Brush& b = s->lib.brushes[size_t(r)]; const QString key = "lib:" + BrushLibrary::setOf(b) + "/" + b.name;
    if (s->alphaFavs.contains(key)) { s->alphaFavs.removeAll(key); ui::toast(QObject::tr("%1 taken off the favourite alphas").arg(b.name)); }
    else { s->alphaFavs << key; while (s->alphaFavs.size() > 8) s->alphaFavs.removeFirst(); ui::toast(QObject::tr("%1 among the favourite alphas (A held)").arg(b.name)); }
    s->saveFavs(); });

  // (the stroke as this brush lays it — with the pen's settings: it changes with them)
  auto* prev = new WideLabel; prev->setFixedHeight(54); prev->setMinimumWidth(120); prev->setAlignment(Qt::AlignCenter); prev->setStyleSheet("background:#242424;border:1px solid #3a3a3a");
  prev->setToolTip(QObject::tr("The brush's stroke as the pen lays it — pressed and tilted harder to its middle")); v->addWidget(prev);
  auto prevDue = std::make_shared<bool>(false);
  auto drawPrev = [s, prev, prevDue] { if (*prevDue) return; *prevDue = true; QTimer::singleShot(40, prev, [s, prev, prevDue] { *prevDue = false;
      const QSize sz(std::max(120, prev->width() - 2), 52); prev->setPixmap(QPixmap::fromImage(s->brush.preview(sz, s->color, true))); }); };
  prev->resized = drawPrev;

  // (folding blocks — each remembers whether it is open)
  auto section = [&](const char* key, const QString& title, bool openDef) {
    const QString k = QString("paint/sec/") + key; const bool open = QSettings().value(k, openDef).toBool();
    auto* hb = new QToolButton; hb->setText(title); hb->setCheckable(true); hb->setChecked(open); hb->setArrowType(open ? Qt::DownArrow : Qt::RightArrow); hb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    hb->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); hb->setFocusPolicy(Qt::NoFocus);
    hb->setStyleSheet("QToolButton{border:none;border-bottom:1px solid #3a3a3a;background:#2b2b2e;color:#d8d8dc;font-weight:bold;padding:4px 4px;text-align:left} QToolButton:hover{background:#333337} QToolButton:checked{background:#2b2b2e}");
    v->addWidget(hb); auto* body = new QWidget; auto* bv = new QVBoxLayout(body); bv->setContentsMargins(2, 4, 0, 8); bv->setSpacing(5); body->setVisible(open); v->addWidget(body);
    QObject::connect(hb, &QToolButton::toggled, body, [hb, body, k](bool on) { body->setVisible(on); hb->setArrowType(on ? Qt::DownArrow : Qt::RightArrow); QSettings().setValue(k, on); });
    return bv; };

  // ---- the brush
  QVBoxLayout* vb = section("brush", QObject::tr("Brush"), true);
  auto* mode = combo(vb, QObject::tr("Writes"), {}, 0);
  for (BrushMode m : {BrushMode::Paint, BrushMode::Erase, BrushMode::Raise, BrushMode::Lower, BrushMode::Flatten, BrushMode::Flow, BrushMode::MaskReveal, BrushMode::MaskHide}) mode->addItem(brushModeName(m), int(m));
  mode->setToolTip(QObject::tr("Paint / Erase — colour; Raise / Lower / Flatten — the bump (Height layers); Flow direction — the anisotropy's direction along your stroke (Flow layers); Mask — a layer's mask. On a Height or Flow layer a plain Paint brush does that layer's own thing"));
  QObject::connect(mode, &QComboBox::currentIndexChanged, w, [s, mode](int) { s->brush.mode = BrushMode(mode->currentData().toInt()); });
  // (Through — the same switch as the top bar's icon and Y: asked for among the brush's own settings, 2026-11-22 recording)
  { auto* thr = new QCheckBox(QObject::tr("Through (Y)")); thr->setIcon(ui::icon("pt_through")); thr->setIconSize(QSize(18, 18)); thr->setFocusPolicy(Qt::NoFocus);
    thr->setToolTip(QObject::tr("Off — the brush paints only what the camera sees. On — the walls behind too, as far back as the brush's radius (both sides of a thin lock). Y taps it, held — only while the key is down; the top bar's icon is the same switch"));
    QObject::connect(thr, &QCheckBox::toggled, w, [s](bool on) { if (s->drawThrough != on) s->setThrough(on); });
    auto sync = [s, thr] { QSignalBlocker b(thr); thr->setChecked(s->drawThrough); }; QObject::connect(s, &PaintSession::throughChanged, thr, sync); sync(); vb->addWidget(thr); }
  auto* size = slider(vb, QObject::tr("Size"), 1, 512, 1, s->brush.size, [](double x) { return QString::number(int(x)) + " px"; }, QObject::tr("[ ] · RMB drag (any direction left or right — nothing else changes)"));
  auto* opac = slider(vb, QObject::tr("Opacity"), 0, 1, 0.01, s->brush.opacity, pct, QObject::tr("The most a stroke can lay down · the digit keys 1…0 over the texture"));
  auto* flow = slider(vb, QObject::tr("Flow"), 0.01, 1, 0.01, s->brush.flow, pct, QObject::tr("What one dab adds: low flow builds up"));
  auto* hard = slider(vb, QObject::tr("Hardness"), 0, 1, 0.01, s->brush.hardness, pct, QObject::tr("Of the round alpha · Shift+[ ]"));
  auto* spacing = slider(vb, QObject::tr("Spacing"), 0.01, 1, 0.01, s->brush.spacing, pct);

  // ---- the alphas: tiles, the turn, the alpha's own settings
  QVBoxLayout* va = section("alpha", QObject::tr("Alpha"), true);
  auto* tilesW = new QWidget; auto* grid = new QGridLayout(tilesW); grid->setContentsMargins(0, 0, 0, 0); grid->setSpacing(3); va->addWidget(tilesW);
  auto tilesSig = std::make_shared<QString>();
  std::function<void()> rebuildTiles;
  auto rebuild = std::make_shared<std::function<void()>>();
  *rebuild = [s, grid, tilesW, tilesSig, rebuild] {
    QStringList keys{"own", "round:1", "round:0.5", "round:0", "square:0", "square:0.6", "made:2", "made:4"}; keys << s->userAlphas();
    const QString sig = keys.join('|') + "#" + s->alphaKey + "#" + QString::number(s->brush.tip.cacheKey()) + QString::number(s->brush.hardness, 'f', 2) + QString::number(s->brush.tipSoft, 'f', 2) + QString::number(s->brush.tipFlat, 'f', 2) + QString::number(s->brush.tipCore, 'f', 2) + QString::number(int(s->brush.tipKind)) + QString::number(s->alphaStandIn);
    if (sig == *tilesSig) return; *tilesSig = sig;
    while (QLayoutItem* it = grid->takeAt(0)) { if (it->widget()) it->widget()->deleteLater(); delete it; }
    const int cols = 7; int n = 0;
    auto tile = [&](QToolButton* b) { b->setFixedSize(38, 38); b->setFocusPolicy(Qt::NoFocus); b->setIconSize(QSize(34, 34));
      b->setStyleSheet("QToolButton{background:#1c1c1e;border:1px solid #3a3a3e;border-radius:4px;padding:0} QToolButton:hover{border-color:#92776d} QToolButton:checked{border:2px solid #9466be;background:#2a2238}");
      grid->addWidget(b, n / cols, n % cols); n++; };
    for (const QString& k : keys) {
      auto* b = new QToolButton; b->setCheckable(true); b->setChecked(k == s->alphaKey); b->setIcon(QIcon(QPixmap::fromImage(s->alphaThumb(k, 34))));
      b->setToolTip(k == "own" ? QObject::tr("The brush's own alpha") : k.startsWith("file:") ? s->alphaLabel(k) + QObject::tr("\nRMB — delete this alpha") : s->alphaLabel(k)); tile(b);
      QObject::connect(b, &QToolButton::clicked, tilesW, [s, k] { if (k == "own") { s->restoreTip(); s->alphaKey = "own"; emit s->brushChanged(); } else if (!s->applyAlpha(k)) ui::toast(QObject::tr("The picture could not be read")); });
      if (k.startsWith("file:")) { b->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(b, &QToolButton::customContextMenuRequested, tilesW, [s, k, b, rebuild] { QMenu m; QAction* d = m.addAction(QObject::tr("Delete this alpha"));
          if (m.exec(b->mapToGlobal(QPoint(0, b->height()))) != d) return;
          if (!QFile::moveToTrash(k.mid(5)) && !QFile::remove(k.mid(5))) { ui::toast(QObject::tr("Could not delete it")); return; }   // (into the Recycle Bin)
          if (s->alphaKey == k) { s->restoreTip(); s->alphaKey = "own"; } emit s->brushChanged(); (*rebuild)(); }); } }
    auto* add = new QToolButton; add->setText("+"); add->setToolTip(QObject::tr("Add an alpha: a picture — white paints (or its opaque part, if it has clear parts). Kept in Documents/HairHarpy Brushes/Alphas")); tile(add);
    add->setStyleSheet(add->styleSheet() + " QToolButton{font-size:20px;color:#b9a7d6}");
    QObject::connect(add, &QToolButton::clicked, tilesW, [s, tilesW, rebuild] {
      const QStringList fs = QFileDialog::getOpenFileNames(tilesW->window(), QObject::tr("Add alphas"), {}, QObject::tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tga *.tif *.tiff)")); if (fs.isEmpty()) return;
      QDir().mkpath(PaintSession::alphaFolder()); QString last;
      for (const QString& f : fs) { const QFileInfo fi(f); QString to = PaintSession::alphaFolder() + "/" + fi.fileName(); for (int i = 2; QFile::exists(to); i++) to = PaintSession::alphaFolder() + "/" + fi.completeBaseName() + QString("_%1.").arg(i) + fi.suffix();
        if (QFile::copy(f, to)) last = to; }
      if (last.isEmpty()) { ui::toast(QObject::tr("Could not copy the pictures")); return; }
      s->applyAlpha("file:" + last); (*rebuild)(); }); };
  rebuildTiles = *rebuild;
  auto* turnRow = new QHBoxLayout; turnRow->setSpacing(8); va->addLayout(turnRow);
  auto* dial = new AlphaDial; turnRow->addWidget(dial);
  dial->angle = [s] { return s->brush.angle; }; dial->setAngle = [s](float a) { s->brush.angle = a; emit s->brushChanged(); };
  dial->thumb = [s] { return s->alphaThumb(s->alphaKey, 96); };
  auto* turnCol = new QVBoxLayout; turnCol->setSpacing(4); turnRow->addLayout(turnCol, 1);
  auto* angLbl = new QLabel; angLbl->setStyleSheet("color:#c8c8cc"); turnCol->addWidget(angLbl);
  auto* orient = new QComboBox; orient->addItems({QObject::tr("By the angle only"), QObject::tr("Along the stroke"), QObject::tr("Along the strand")}); orient->setCurrentIndex(s->brush.orient); orient->setFocusPolicy(Qt::NoFocus);
  orient->setToolTip(QObject::tr("How the alpha is turned: by the angle only; along the stroke; along the hair strand under the brush (its UV island's long axis) whichever way the hand goes — the angle added to it"));
  turnCol->addWidget(orient); turnCol->addStretch(1);
  auto* tsoft = slider(va, QObject::tr("Edge softness"), 0, 1, 0.01, s->brush.tipSoft, pct, QObject::tr("How softly the made alpha falls to its sides"));
  auto* tflat = slider(va, QObject::tr("Flat middle"), 0, 1, 0.01, s->brush.tipFlat, pct, QObject::tr("Cylinder: the share of its width that is full before it falls off"));
  auto* tcore = slider(va, QObject::tr("Bright core"), 0, 1, 0.01, s->brush.tipCore, pct, QObject::tr("Cylinder: a brighter line along its middle"));

  // ---- the pen: each setting — on / off (its slider kept as it is), what drives it, how strongly, its curve
  QVBoxLayout* vp = section("pen", QObject::tr("Pen"), true);
  struct PenRow { QCheckBox* on; QComboBox* src; SliderRow* str; };
  auto penRow = [&](const QString& label, bool Brush::*on, int Brush::*src, float Brush::*mn, Curve Brush::*cv, const QString& tip) {
    auto* h = new QHBoxLayout; h->setSpacing(4); vp->addLayout(h);
    auto* c = new QCheckBox(label); c->setChecked(s->brush.*on); c->setToolTip(tip + QObject::tr("\nOff — the setting does not follow the pen; its slider stays as it is")); c->setFocusPolicy(Qt::NoFocus); c->setMinimumWidth(78); h->addWidget(c);
    auto* sc = new QComboBox; sc->addItems({QObject::tr("Pen pressure"), QObject::tr("Pen tilt")}); sc->setCurrentIndex(std::clamp(s->brush.*src, 0, 1)); sc->setFocusPolicy(Qt::NoFocus);
    sc->setToolTip(QObject::tr("What drives it: the pen's pressure, or its tilt (upright — the least, tilted 60° — the most). The mouse counts as full")); h->addWidget(sc, 1);
    auto* cb = new QToolButton; cb->setText(QString::fromUtf8("⌒")); cb->setToolTip(QObject::tr("Its curve: how it rises with the pressure / the tilt")); cb->setFocusPolicy(Qt::NoFocus); cb->setFixedWidth(26); h->addWidget(cb);
    QObject::connect(cb, &QToolButton::clicked, cb, [s, cv, cb] { auto* f = new QFrame(cb->window(), Qt::Popup); f->setAttribute(Qt::WA_DeleteOnClose); f->setStyleSheet("QFrame{background:#262626;border:1px solid #555}");
      auto* fv = new QVBoxLayout(f); fv->setContentsMargins(6, 6, 6, 6); auto* ce = new CurveEdit; ce->setFixedSize(200, 150); ce->get = [s, cv] { return s->brush.*cv; }; ce->set = [s, cv](const Curve& k) { s->brush.*cv = k; emit s->brushChanged(); }; fv->addWidget(ce);
      f->adjustSize(); f->move(cb->mapToGlobal(QPoint(cb->width() - f->width(), cb->height() + 2))); f->show(); });
    auto* st = slider(vp, QObject::tr("  strength"), 0, 1, 0.01, 1 - s->brush.*mn, pct, QObject::tr("How much the pen changes it: 100% — from nothing to full, 0% — not at all"));
    QObject::connect(c, &QCheckBox::toggled, w, [s, on](bool x) { s->brush.*on = x; emit s->brushChanged(); });
    QObject::connect(sc, &QComboBox::activated, w, [s, src](int i) { s->brush.*src = i; emit s->brushChanged(); });
    QObject::connect(st, &SliderRow::changed, w, [s, mn](double x) { s->brush.*mn = float(1 - x); emit s->brushChanged(); });
    return PenRow{c, sc, st}; };
  const PenRow pS = penRow(QObject::tr("Size"), &Brush::pressureSize, &Brush::pSizeSrc, &Brush::pSizeMin, &Brush::pSizeCurve, QObject::tr("The pen makes the brush bigger"));
  const PenRow pO = penRow(QObject::tr("Opacity"), &Brush::pressureOpacity, &Brush::pOpacitySrc, &Brush::pOpacityMin, &Brush::pOpacityCurve, QObject::tr("The pen lets each dab lay down more"));
  const PenRow pF = penRow(QObject::tr("Flow"), &Brush::pressureFlow, &Brush::pFlowSrc, &Brush::pFlowMin, &Brush::pFlowCurve, QObject::tr("The pen makes the colour build up faster"));

  // ---- the rest
  QVBoxLayout* vs = section("stroke", QObject::tr("Stroke"), false);
  auto* smooth = slider(vs, QObject::tr("Steady Stroke"), 0, 1, 0.01, s->brush.smoothing, pct, QObject::tr("The brush follows the pointer on a string (as 3D-Coat's Steady Stroke; 45% ≈ its 1.8) — on the model and on the texture; at the stroke's end it catches up. 0 — off"));
  auto* sblur = slider(vs, QObject::tr("Shift: blur"), 0, 1, 0.01, s->brush.shiftBlur, pct, QObject::tr("Shift held: the brush blurs this strongly (3D-Coat's Smoothing). Shift + a click: a straight stroke from the last one's end"));
  auto* salong = combo(vs, QObject::tr("Shift"), {QObject::tr("Blur"), QObject::tr("Blur along the strand"), QObject::tr("Oil: drag the colour")}, std::clamp(s->brush.shiftMode, 0, 2));
  salong->setToolTip(QObject::tr("What the brush does with Shift held. Blur — the colours run together round the point (3D-Coat's Smoothing). Along the strand — they run together down the lock (its UV island's long axis). Oil — the colour where the stroke begins is taken up and dragged along"));
  QObject::connect(salong, &QComboBox::activated, w, [s](int i) { s->brush.shiftMode = i; s->brush.shiftAlong = i == 1; emit s->brushChanged(); });
  auto* str = slider(vs, QObject::tr("Bump strength"), 0, 1, 0.01, s->brush.strength, pct, QObject::tr("Raise / Lower: the relief a full stroke makes"));
  auto* sj = slider(vs, QObject::tr("Size jitter"), 0, 1, 0.01, s->brush.sizeJitter, pct);
  auto* aj = slider(vs, QObject::tr("Angle jitter"), 0, 1, 0.01, s->brush.angleJitter, pct);
  auto* sc = slider(vs, QObject::tr("Scatter"), 0, 2, 0.01, s->brush.scatter, pct);

  QVBoxLayout* vc = section("colour", QObject::tr("Colour dynamics"), false);
  auto* hj = slider(vc, QObject::tr("Hue jitter"), 0, 1, 0.01, s->brush.hueJ, pct);
  auto* stj = slider(vc, QObject::tr("Saturation jitter"), 0, 1, 0.01, s->brush.satJ, pct);
  auto* bj = slider(vc, QObject::tr("Brightness jitter"), 0, 1, 0.01, s->brush.briJ, pct);
  auto* perStroke = check(vc, QObject::tr("Once per stroke (not per dab)"), !s->brush.colourPerDab);
  auto* wet = slider(vc, QObject::tr("Wet: mix with what is under"), 0, 1, 0.01, s->brush.wet, pct, QObject::tr("How much of the colour under the brush mixes into each dab — as Photoshop's Mixer Brush"));
  auto* load = slider(vc, QObject::tr("Takes the colour on"), 0, 1, 0.01, s->brush.load, pct, QObject::tr("Wet: how fast the brush takes on the colour it passes over"));

  QVBoxLayout* vk = section("pipette", QObject::tr("Pipette (V, Alt over the texture)"), false);
  auto* pk = combo(vk, QObject::tr("Takes"), {QObject::tr("1 texel"), QObject::tr("Mean of 3×3"), QObject::tr("Mean of 5×5"), QObject::tr("Mean under the brush")}, s->pickSize <= 0 ? 3 : s->pickSize >= 5 ? 2 : s->pickSize >= 3 ? 1 : 0);
  pk->setToolTip(QObject::tr("How much is taken: one texel, a small patch, or the mean of everything under the brush's ring — for a texture made of many small spots where the overall colour is wanted"));
  QObject::connect(pk, &QComboBox::currentIndexChanged, w, [s](int i) { s->pickSize = i == 3 ? 0 : i == 2 ? 5 : i == 1 ? 3 : 1; QSettings().setValue("paint/pickSize", s->pickSize); });
  auto* pf = combo(vk, QObject::tr("From"), {QObject::tr("All layers (the texture's own colour)"), QObject::tr("All but the blend layers"), QObject::tr("The active layer alone"), QObject::tr("The screen")},
                   s->pickFrom == "normal" ? 1 : s->pickFrom == "layer" ? 2 : s->pickFrom == "screen" ? 3 : 0);
  pf->setToolTip(QObject::tr("All layers — the colour as the texture holds it, without the lighting. All but the blend layers — the same without the layers in a blend other than Normal and the adjustments. The active layer alone — what that layer itself holds. The screen — the colour as it is seen"));
  QObject::connect(pf, &QComboBox::currentIndexChanged, w, [s](int i) { s->pickFrom = i == 1 ? "normal" : i == 2 ? "layer" : i == 3 ? "screen" : "all"; QSettings().setValue("paint/pickFrom", s->pickFrom); });
  addHelp(w, QObject::tr("Flow direction writes the stroke's own direction (UV space, v up) into a Flow layer: the toon shader's anisotropic highlight runs across it."));
  addHelp(w, QObject::tr("Alpha: a click on a tile — that alpha stands in for the brush's own (the first tile brings it back); + adds your own pictures, RMB on one of them deletes it. The circle under the tiles turns the alpha: drag round it, the wheel, a double click — upright."));
  addHelp(w, QObject::tr("Pen: each setting can follow the pen's pressure or its tilt; its check box switches that off without touching the strength; ⌒ — its curve."));
  v->addStretch(1);

  auto sync = [=] {
    const QSignalBlocker b0(smooth), b1(mode), b2(size), b3(spacing), b4(hard), b5(opac), b6(flow), b7(str), b9(sj), b10(aj), b11(sc), b13(tsoft), b14(tflat), b15(tcore), b16(orient),
      b17(pS.on), b18(pS.src), b19(pS.str), b20(pO.on), b21(pO.src), b22(pO.str), b23(pF.on), b24(pF.src), b25(pF.str), b26(sblur), b27(hj), b28(stj), b29(bj), b30(perStroke), b31(wet), b32(load), b33(salong);
    smooth->setValue(s->brush.smoothing); mode->setCurrentIndex(mode->findData(int(s->brush.mode))); size->setValue(s->brush.size); spacing->setValue(s->brush.spacing); hard->setValue(s->brush.hardness); opac->setValue(s->brush.opacity); flow->setValue(s->brush.flow);
    str->setValue(s->brush.strength); sj->setValue(s->brush.sizeJitter); aj->setValue(s->brush.angleJitter); sc->setValue(s->brush.scatter);
    tsoft->setValue(s->brush.tipSoft); tflat->setValue(s->brush.tipFlat); tcore->setValue(s->brush.tipCore); orient->setCurrentIndex(s->brush.orient);
    const TipKind k = s->brush.tipKind; tsoft->setVisible(k == TipKind::Cylinder || k == TipKind::Square); tflat->setVisible(k == TipKind::Cylinder); tcore->setVisible(k == TipKind::Cylinder);
    hard->setVisible(k == TipKind::Round || (k == TipKind::Image && s->brush.tip.isNull()));
    angLbl->setText(QObject::tr("Turned %1°").arg(int(std::lround(s->brush.angle)))); dial->update();
    pS.on->setChecked(s->brush.pressureSize); pS.src->setCurrentIndex(std::clamp(s->brush.pSizeSrc, 0, 1)); pS.str->setValue(1 - s->brush.pSizeMin);
    pO.on->setChecked(s->brush.pressureOpacity); pO.src->setCurrentIndex(std::clamp(s->brush.pOpacitySrc, 0, 1)); pO.str->setValue(1 - s->brush.pOpacityMin);
    pF.on->setChecked(s->brush.pressureFlow); pF.src->setCurrentIndex(std::clamp(s->brush.pFlowSrc, 0, 1)); pF.str->setValue(1 - s->brush.pFlowMin);
    sblur->setValue(s->brush.shiftBlur); salong->setCurrentIndex(std::clamp(s->brush.shiftMode, 0, 2)); hj->setValue(s->brush.hueJ); stj->setValue(s->brush.satJ); bj->setValue(s->brush.briJ); perStroke->setChecked(!s->brush.colourPerDab); wet->setValue(s->brush.wet); load->setValue(s->brush.load);
    rebuildTiles(); drawPrev(); };
  QObject::connect(s, &PaintSession::brushChanged, w, sync);
  auto bind = [&](SliderRow* r, float Brush::*m) { QObject::connect(r, &SliderRow::changed, w, [s, m](double x) { s->brush.*m = float(x); emit s->brushChanged(); }); };
  bind(smooth, &Brush::smoothing); bind(size, &Brush::size); bind(spacing, &Brush::spacing); bind(hard, &Brush::hardness); bind(opac, &Brush::opacity); bind(flow, &Brush::flow); bind(str, &Brush::strength);
  bind(sj, &Brush::sizeJitter); bind(aj, &Brush::angleJitter); bind(sc, &Brush::scatter); bind(tsoft, &Brush::tipSoft); bind(tflat, &Brush::tipFlat); bind(tcore, &Brush::tipCore); bind(sblur, &Brush::shiftBlur);
  bind(hj, &Brush::hueJ); bind(stj, &Brush::satJ); bind(bj, &Brush::briJ); bind(wet, &Brush::wet); bind(load, &Brush::load);
  QObject::connect(orient, &QComboBox::activated, w, [s](int i) { s->brush.orient = i; s->brush.followStroke = i == 1; emit s->brushChanged(); });
  QObject::connect(perStroke, &QCheckBox::toggled, w, [s](bool on) { s->brush.colourPerDab = !on; emit s->brushChanged(); });
  sync();
  return w;
}

// ================= Bake =================

QWidget* makePaintBakePanel(PaintSession* s, Viewport* vp, Scene* sc) {
  Q_UNUSED(vp);
  auto* w = new ShowHook; auto* v = new QVBoxLayout(w); v->setContentsMargins(8, 6, 8, 8); v->setSpacing(5);
  auto* high = combo(v, QObject::tr("High-poly"), {}, 0); auto* low = combo(v, QObject::tr("Low-poly"), {}, 0);
  high->setToolTip(QObject::tr("The detailed mesh whose normals, height and occlusion are baked")); low->setToolTip(QObject::tr("The mesh with the UVs the maps are baked onto"));
  auto fillMeshes = [s, sc, high, low] {
    QSignalBlocker a(high), b(low); high->clear(); low->clear();
    for (auto& [id, it] : sc->items) if (it.type == ItemType::IMesh && it.poly) { high->addItem(it.name + (it.poly->cornerUV.empty() ? "" : ""), id); low->addItem(it.name + (it.poly->cornerUV.empty() ? QObject::tr("  (no UVs)") : ""), id); }
    int i = high->findData(s->highId); if (i >= 0) high->setCurrentIndex(i); else if (high->count() > 1) high->setCurrentIndex(0);
    i = low->findData(s->lowId); if (i >= 0) low->setCurrentIndex(i); else if (low->count() > 1) low->setCurrentIndex(1);
    s->highId = high->currentIndex() >= 0 ? high->currentData().toInt() : -1; s->lowId = low->currentIndex() >= 0 ? low->currentData().toInt() : -1; };   // (what the lists show is what is baked: the signals are blocked here)
  static_cast<ShowHook*>(w)->onShow = fillMeshes; fillMeshes();
  QObject::connect(high, &QComboBox::currentIndexChanged, w, [s, high](int) { s->highId = high->currentData().toInt(); });
  QObject::connect(low, &QComboBox::currentIndexChanged, w, [s, low](int) { s->lowId = low->currentData().toInt(); });
  auto* refresh = button(QObject::tr("↻ Meshes"), QObject::tr("Read the room's meshes again")); QObject::connect(refresh, &QPushButton::clicked, w, fillMeshes); v->addWidget(refresh);

  auto* sizeC = combo(v, QObject::tr("Size"), {"512", "1024", "2048", "4096"}, std::clamp(int(std::lround(std::log2(std::max(512, s->bakeS.size)))) - 9, 0, 3));
  QObject::connect(sizeC, &QComboBox::currentIndexChanged, w, [s](int i) { s->bakeS.size = 512 << i; });
  auto* aa = combo(v, QObject::tr("Samples"), {QObject::tr("1 per texel"), QObject::tr("2×2 per texel")}, s->bakeS.samples > 1 ? 1 : 0); QObject::connect(aa, &QComboBox::currentIndexChanged, w, [s](int i) { s->bakeS.samples = i + 1; });
  auto* pad = slider(v, QObject::tr("Padding"), 0, 32, 1, s->bakeS.padding, [](double x) { return QString::number(int(x)) + " px"; }); QObject::connect(pad, &SliderRow::changed, w, [s](double x) { s->bakeS.padding = int(x); });
  auto* cage = slider(v, QObject::tr("Cage"), 0.001, 0.2, 0.001, s->bakeS.cage, [](double x) { return QString::number(x * 100, 'f', 1) + "%"; }, QObject::tr("How far the rays start from the low-poly's surface, of the high-poly's size. Raise it if the high-poly rises above the low"));
  QObject::connect(cage, &SliderRow::changed, w, [s](double x) { s->bakeS.cage = float(x); });
  auto* back = slider(v, QObject::tr("Max distance in"), 0.001, 0.2, 0.001, s->bakeS.back, [](double x) { return QString::number(x * 100, 'f', 1) + "%"; }, QObject::tr("How far behind the low-poly's surface the rays still hit"));
  QObject::connect(back, &SliderRow::changed, w, [s](double x) { s->bakeS.back = float(x); });
  auto* rays = slider(v, QObject::tr("AO rays"), 4, 64, 4, s->bakeS.aoRays, [](double x) { return QString::number(int(x)); }); QObject::connect(rays, &SliderRow::changed, w, [s](double x) { s->bakeS.aoRays = int(x); });
  auto* aod = slider(v, QObject::tr("AO distance"), 0.005, 0.5, 0.005, s->bakeS.aoDist, [](double x) { return QString::number(x * 100, 'f', 1) + "%"; }); QObject::connect(aod, &SliderRow::changed, w, [s](double x) { s->bakeS.aoDist = float(x); });
  struct M { const char* n; bool BakeSettings::*m; }; for (M m : {M{"Normal (tangent space)", &BakeSettings::normal}, M{"Ambient occlusion", &BakeSettings::ao}, M{"Height", &BakeSettings::height}, M{"Position", &BakeSettings::position}, M{"Curvature", &BakeSettings::curvature}, M{"ID colours (polypaint)", &BakeSettings::colour}, M{"Flip green (DirectX)", &BakeSettings::flipGreen}}) {
    auto* c = check(v, QObject::tr(m.n), s->bakeS.*m.m); auto mm = m.m; QObject::connect(c, &QCheckBox::toggled, w, [s, mm](bool on) { s->bakeS.*mm = on; }); }
  auto* go = button(QObject::tr("Bake"), QObject::tr("Cast the rays: the maps come to the canvas's picker and can be saved or put into layers")); go->setProperty("primary", true); v->addWidget(go);
  auto* bar = new QProgressBar; bar->setRange(0, 100); bar->setTextVisible(false); bar->setFixedHeight(6); bar->hide(); v->addWidget(bar);
  auto* status = note(v, QString()); status->hide(); addHelp(w, QObject::tr("Pick the high-poly and the low-poly (with UVs) among the room's meshes. To bake only a part — the folds of the cloth — paint it white in the Bake mask map first: only that is cast and used.\nID colours (polypaint): the high-poly's vertex colours (import it with Polypaint on) baked into an ID map. The baked maps ▾ → ID map → a hidden layer; then Fill → Similar colour, source — the active layer, ❄ on: a click on a part gives the mask of its colour."));
  auto prog = std::make_shared<std::atomic<float>>(0); auto stop = std::make_shared<std::atomic<bool>>(false); auto running = std::make_shared<bool>(false);
  auto* tick = new QTimer(w); tick->setInterval(120); QObject::connect(tick, &QTimer::timeout, w, [bar, prog] { bar->setValue(int(prog->load() * 100)); });

  QObject::connect(go, &QPushButton::clicked, w, [=] {
    if (*running) { stop->store(true); return; }
    const auto hit = sc->items.find(s->highId), lit = sc->items.find(s->lowId);
    if (hit == sc->items.end() || lit == sc->items.end() || !hit->second.poly || !lit->second.poly) { showSt(status, QObject::tr("Pick both meshes")); return; }
    if (s->highId == s->lowId) { showSt(status, QObject::tr("The high-poly and the low-poly are the same mesh")); return; }
    auto lowM = std::make_shared<TriMesh>(triMeshFrom(*lit->second.poly)), highM = std::make_shared<TriMesh>(triMeshFrom(*hit->second.poly));
    if (lowM->uv.size() != lowM->tri.size()) { showSt(status, QObject::tr("The low-poly has no UVs: unfold it in the Topology room first")); return; }
    BakeSettings st = s->bakeS;
    // (a partial bake: the Bake mask map painted — only its texels are cast; none painted: the whole low-poly)
    const bool partial = s->doc.width() > 0 && s->doc.hasLayers(Channel::BakeMask); if (partial) st.mask = s->doc.flatten(Channel::BakeMask);
    *running = true; stop->store(false); prog->store(0); bar->setValue(0); bar->show(); tick->start(); go->setText(QObject::tr("Stop")); showSt(status, partial ? QObject::tr("Baking where the Bake mask is painted…") : QObject::tr("Baking…"));
    QPointer<QWidget> guard(w);
    auto* th = QThread::create([=] {
      auto R = std::make_shared<BakeResult>(bake(*lowM, *highM, st, [prog, stop](float p) { prog->store(p); return !stop->load(); }));
      QMetaObject::invokeMethod(s, [=] {
        *running = false; if (!guard) return; tick->stop(); bar->hide(); go->setText(QObject::tr("Bake"));
        if (R->normal.isNull() && R->ao.isNull() && R->height.isNull() && R->colour.isNull() && R->position.isNull() && R->curvature.isNull()) { showSt(status, R->log.isEmpty() ? QObject::tr("Nothing baked") : R->log); return; }
        s->baked.clear(); if (!R->normal.isNull() && st.normal) s->baked["normal"] = R->normal; if (!R->ao.isNull()) s->baked["ao"] = R->ao; if (!R->height.isNull()) s->baked["height"] = R->height;
        if (!R->position.isNull()) s->baked["position"] = R->position; if (!R->curvature.isNull()) s->baked["curvature"] = R->curvature;
        if (!R->colour.isNull()) s->baked["id"] = R->colour; else if (st.colour) { showSt(status, QObject::tr("Done: %1 — no ID map: the high-poly has no vertex colours (import it with Polypaint)").arg(R->log)); emit s->bakeChanged(); return; }
        showSt(status, QObject::tr("Done: %1").arg(R->log)); emit s->bakeChanged(); }, Qt::QueuedConnection); });
    QObject::connect(th, &QThread::finished, th, &QObject::deleteLater); th->start(); });

  // what to do with the maps
  auto* mapsBtn = button(QObject::tr("The baked maps") + QString::fromUtf8("  \u25BE"), QObject::tr("What to do with the maps once they are baked: save them, put them into layers, save the final normal map")); QMenu* mapsM = ui::moreMenu(mapsBtn); v->addWidget(mapsBtn);
  QAction* sv = ui::menuItem(mapsM, QObject::tr("Save maps…"), QObject::tr("Every baked map as a PNG into a folder"), nullptr, nullptr);
  QObject::connect(sv, &QAction::triggered, w, [s, w, status] {
    if (s->baked.isEmpty()) { showSt(status, QObject::tr("Bake first")); return; }
    const QString d = QFileDialog::getExistingDirectory(w, QObject::tr("Save the maps into")); if (d.isEmpty()) return;
    for (auto it = s->baked.begin(); it != s->baked.end(); ++it) it.value().save(d + "/" + it.key() + ".png"); showSt(status, QObject::tr("Saved into %1").arg(d)); });
  QAction* toId = ui::menuItem(mapsM, QObject::tr("ID map → a hidden layer (masks by colour)"), QObject::tr("The baked polypaint colours as a hidden layer \"ID map\", made the active one: Fill → Similar colour with ❄ on it gives the mask of one part by its colour"), nullptr, nullptr);
  QObject::connect(toId, &QAction::triggered, w, [s, status] {
    if (!s->baked.contains("id")) { showSt(status, QObject::tr("Bake with ID colours (polypaint) on first")); return; }
    s->ensureDoc(s->bakeS.size); const int id = s->doc.addPaint(Channel::Color, s->doc.freeName("ID map"), s->layer);
    paint::Layer* l = s->doc.layer(id); l->image = s->baked["id"].scaled(s->doc.width(), s->doc.height(), Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_ARGB32); l->visible = false;
    s->layer = id; s->edits++; emit s->layersChanged(); emit s->pixelsChanged({});
    ui::toast(QObject::tr("\"ID map\" made (hidden) and active: Fill → Similar colour, source: the active layer (not All layers), ❄ on — click a part of the model, its ID colour becomes the mask")); });
  QAction* toL = ui::menuItem(mapsM, QObject::tr("AO, curvature → layers, Height → bump"), QObject::tr("A ready base to paint over: the baked occlusion as a Multiply colour layer, the curvature as an Overlay layer (hollows darker, ridges lighter — at half strength), the baked height as a Height layer"), nullptr, nullptr);
  QObject::connect(toL, &QAction::triggered, w, [s, status] {
    s->ensureDoc(s->bakeS.size); int n = 0;
    if (s->baked.contains("ao")) { const int id = s->doc.addPaint(Channel::Color, "Baked AO", s->layer); s->doc.layer(id)->blend = Blend::Multiply;
      s->doc.layer(id)->image = s->baked["ao"].scaled(s->doc.width(), s->doc.height()).convertToFormat(QImage::Format_ARGB32); s->layer = id; n++; }
    if (s->baked.contains("curvature")) { const int id = s->doc.addPaint(Channel::Color, "Baked curvature", s->layer); s->doc.layer(id)->blend = Blend::Overlay; s->doc.layer(id)->opacity = 0.5f;
      s->doc.layer(id)->image = s->baked["curvature"].scaled(s->doc.width(), s->doc.height()).convertToFormat(QImage::Format_ARGB32); s->layer = id; n++; }
    if (s->baked.contains("height")) { const int id = s->doc.addPaint(Channel::Height, "Baked height", s->layer);
      s->doc.layer(id)->image = s->baked["height"].scaled(s->doc.width(), s->doc.height()).convertToFormat(QImage::Format_Grayscale16); s->layer = id; n++; }
    showSt(status, n ? QObject::tr("%1 layers added").arg(n) : QObject::tr("Bake first")); emit s->layersChanged(); emit s->pixelsChanged({}); });

  note(v, QObject::tr("Bump → normal map"));
  auto* bs = slider(v, QObject::tr("Bump strength"), 0.05, 5, 0.05, s->bumpStrength, num, QObject::tr("The slope of the painted Height map in the normal map (the canvas's \"Normal from bump\")"));
  QObject::connect(bs, &SliderRow::changed, w, [s](double x) { s->bumpStrength = float(x); emit s->pixelsChanged({}); });
  QObject::connect(s, &PaintSession::pixelsChanged, bs, [s, bs](const QRect& r) { if (r.isNull() && std::abs(bs->value() - s->bumpStrength) > 1e-4) { QSignalBlocker b(bs); bs->setValue(s->bumpStrength); } });   // (the mode bar's Normal strength is the same value: followed)
  QAction* fin = ui::menuItem(mapsM, QObject::tr("Save final normal map…"), QObject::tr("The baked normal map with the painted bump added to it (just the bump's, without a bake)"), nullptr, nullptr);
  QObject::connect(fin, &QAction::triggered, w, [s, w, status] {
    const QImage out = s->normalResult();
    if (out.isNull()) { showSt(status, QObject::tr("Nothing to save: bake or paint a height layer")); return; }
    const QString f = QFileDialog::getSaveFileName(w, QObject::tr("Save the normal map"), "normal.png", QObject::tr("PNG (*.png)")); if (f.isEmpty()) return; out.save(f); showSt(status, QObject::tr("Saved %1").arg(f)); });
  return w;
}

// ================= Bridge =================

QWidget* makePaintBridgePanel(PaintSession* s, Viewport* vp, Scene* sc) {
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(8, 6, 8, 8); v->setSpacing(5);
  auto* dc = new QLabel; dc->setWordWrap(true);
  // (what is sent: one menu; the places of the programs and folders, set once: another. The items keep the captions and the tips the
  // buttons had, so a key given to one goes on working)
  auto* sendBtn = button(QObject::tr("Send to the editor") + QString::fromUtf8("  \u25BE"), QObject::tr("The active layer, all the layers as a PSD, this view for a projection, the composed colour map: to the 2D editor; the mesh: to 3D-Coat")); QMenu* sendM = ui::moreMenu(sendBtn);
  auto* pathBtn = button(QObject::tr("Programs and folders") + QString::fromUtf8("  \u25BE"), QObject::tr("Set once: the editor's program, 3D-Coat's program, the folder 3D-Coat exports into")); QMenu* pathM = ui::moreMenu(pathBtn);
  // ---- the 2D editor: Photoshop / Krita / SAI / one's own ----
  auto* ed = combo(v, QObject::tr("2D editor"), {QString("Photoshop"), QString("Krita"), QString("PaintTool SAI"), QObject::tr("Other program…")}, 0);
  auto edApp = [](int i) { return i == 1 ? BridgeApp::Krita : i == 2 ? BridgeApp::Sai : i == 3 ? BridgeApp::Other : BridgeApp::Photoshop; };
  { const BridgeApp cur = s->bridge.editor(); ed->setCurrentIndex(cur == BridgeApp::Krita ? 1 : cur == BridgeApp::Sai ? 2 : cur == BridgeApp::Other ? 3 : 0); }
  QLabel* edPath = nullptr; (void)edPath;
  QAction* edPick = ui::menuItem(pathM, QObject::tr("Its program…"), QObject::tr("Choose the editor's .exe (Krita: krita.exe, SAI: sai2.exe, or any program that opens a PNG and saves it back)"), nullptr, nullptr);
  auto b2p = std::make_shared<QAction*>(nullptr);
  auto showEd = [s, ed, b2p] { const BridgeApp a = s->bridge.editor(); const QString e = s->bridge.exePath(a); ed->setToolTip(e.isEmpty() ? QObject::tr("%1: not found — Programs and folders → Its program…").arg(Bridge::appName(a)) : e);
    const QString b3 = s->bridge.exePath(BridgeApp::ThreeDCoat); if (*b2p) (*b2p)->setText(QObject::tr("3D-Coat… (%1)").arg(b3.isEmpty() ? QObject::tr("not found") : b3)); };
  showEd();
  QObject::connect(ed, &QComboBox::activated, w, [s, edApp, showEd](int i) { s->bridge.setEditor(edApp(i)); showEd(); });
  QObject::connect(edPick, &QAction::triggered, w, [s, w, showEd] { const QString f = QFileDialog::getOpenFileName(w, Bridge::appName(s->bridge.editor()), {}, "*.exe"); if (!f.isEmpty()) { s->bridge.setExePath(s->bridge.editor(), f); showEd(); } });
  // ---- which UV set goes: the one painted now (by default), another, all of them, or asked each time ----
  auto* setSend = combo(v, QObject::tr("UV set"), {}, 0);
  auto fillSets = [s, setSend] { const QString keep = QSettings().value("paint/sendSet", "@current").toString(); QSignalBlocker b(setSend); setSend->clear();
    setSend->addItem(QObject::tr("✓ The one painted now (%1)").arg(s->doc.activeSet().isEmpty() ? QObject::tr("single texture") : s->doc.activeSet()), "@current");
    for (const auto& x : s->doc.sets()) if (!x.key.isEmpty()) setSend->addItem(x.key, x.key);
    setSend->addItem(QObject::tr("All sets (a file each)"), "@all"); setSend->addItem(QObject::tr("Ask each time"), "@ask");
    const int i = setSend->findData(keep); setSend->setCurrentIndex(i < 0 ? 0 : i); };
  fillSets(); QObject::connect(s, &PaintSession::viewChanged, setSend, fillSets);
  QObject::connect(setSend, &QComboBox::activated, w, [setSend](int) { QSettings().setValue("paint/sendSet", setSend->currentData().toString()); });
  auto* withIsl = check(v, QObject::tr("+ the UV islands' map (a second file: put it on top as a layer)"), QSettings().value("paint/sendIslands", true).toBool(), QObject::tr("The dark ground outside the islands and their outlines — as in your Photoshop file — so the strands are seen while painting"));
  QObject::connect(withIsl, &QCheckBox::toggled, w, [](bool on) { QSettings().setValue("paint/sendIslands", on); });
  addHelp(w, QObject::tr("The active layer goes out as a PNG and the editor opens it; paint, then Save (keep the PNG) — the layer takes the picture back, into the UV set it came from."));
  auto last = std::make_shared<std::pair<int, QStringList>>(-1, QStringList());   // (the last sending: the layer and its sets — sent again in one click)
  auto sendNow = [s, w, last](int id, const QStringList& sets) {
    Layer* l = s->doc.layer(id); if (!l || l->kind != LayerKind::Paint) { ui::toast(QObject::tr("Select a paint layer")); return; }
    const QString cur = s->doc.activeSet(); const int cw = s->doc.width(), chh = s->doc.height(); int n = 0; QString err;
    const bool isl = QSettings().value("paint/sendIslands", true).toBool();
    for (const QString& set : sets) {
      bool known = set == cur; int sw = cw, sh = chh; for (const auto& x : s->doc.sets()) if (x.key == set) { known = true; sw = x.w; sh = x.h; }
      if (!known) continue;
      if (set != cur) s->doc.setActiveSet(set, sw, sh);
      Layer* L = s->doc.layer(id); QImage img = L && !L->image.isNull() ? L->image : QImage(s->doc.width(), s->doc.height(), formatOf(L ? L->channel : Channel::Color));
      if (L && L->image.isNull()) img.fill(Qt::transparent);
      if (isl && s->islands) if (const paint::IslandMap* im = s->islands(); im && !im->ids.isNull()) {   // (the islands' map beside it: dark outside, outlined)
        QImage o(im->ids.size(), QImage::Format_ARGB32); o.fill(Qt::transparent);
        for (int y = 0; y < o.height(); y++) { QRgb* r = reinterpret_cast<QRgb*>(o.scanLine(y)); for (int x = 0; x < o.width(); x++) { const int v0 = im->at(x, y);
          if (!v0) { r[x] = qRgba(11, 16, 48, 230); continue; } if (im->at(x - 1, y) != v0 || im->at(x + 1, y) != v0 || im->at(x, y - 1) != v0 || im->at(x, y + 1) != v0) r[x] = qRgba(110, 170, 255, 255); } }
        QString safe = set.isEmpty() ? QString("texture") : set; safe.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_"); o.save(s->bridge.workFolder() + "/islands_" + safe + ".png"); }
      if (!s->bridge.sendImage(s->bridge.editor(), QString("%1@%2").arg(id).arg(set), img, &err)) { if (set != cur) s->doc.setActiveSet(cur, cw, chh); ui::toast(err); return; }
      n++;
      if (set != cur) s->doc.setActiveSet(cur, cw, chh); }
    *last = {id, sets};
    ui::toast(n ? QObject::tr("%1 sent to %2 (%3 UV set(s)): save there to bring it back%4").arg(s->doc.layer(id)->name, Bridge::appName(s->bridge.editor())).arg(n).arg(isl ? QObject::tr(" · the islands' map: %1").arg(QDir::toNativeSeparators(s->bridge.workFolder())) : QString()) : QObject::tr("Nothing sent")); };
  v->addWidget(sendBtn); QAction* sp = ui::menuItem(sendM, QObject::tr("Edit layer in the editor"), QObject::tr("The active paint layer to the 2D editor, in the UV set(s) chosen above; what you save there comes back"), nullptr, nullptr);
  QObject::connect(sp, &QAction::triggered, w, [s, w, setSend, sendNow] {
    Layer* l = s->active(); if (!l || l->kind != LayerKind::Paint) { ui::toast(QObject::tr("Select a paint layer")); return; }
    QString pick = setSend->currentData().toString(); QStringList sets;
    if (pick == "@ask") { QStringList names{s->doc.activeSet()}; for (const auto& x : s->doc.sets()) if (!names.contains(x.key)) names << x.key;
      QStringList shown; for (const QString& n : names) shown << (n.isEmpty() ? QObject::tr("single texture") : n);
      bool ok = false; const QString c = QInputDialog::getItem(w, QObject::tr("Send which UV set"), QObject::tr("UV set"), shown, 0, false, &ok); if (!ok) return; pick = names[shown.indexOf(c)]; }
    if (pick == "@current") sets << s->doc.activeSet(); else if (pick == "@all") { for (const auto& x : s->doc.sets()) sets << x.key; if (sets.isEmpty()) sets << s->doc.activeSet(); } else sets << pick;
    sendNow(l->id, sets); });
  QAction* again = ui::menuItem(sendM, QObject::tr("↻ Send again"), QObject::tr("The same layer and UV set(s) as last time, at once"), nullptr, nullptr);
  QObject::connect(again, &QAction::triggered, w, [last, sendNow] { if (last->first < 0) { ui::toast(QObject::tr("Nothing sent yet")); return; } sendNow(last->first, last->second); });
  sendM->addSeparator(); QAction* sl = ui::menuItem(sendM, QObject::tr("All layers to the editor (PSD)"), QObject::tr("Every paint layer of the map and the UV set shown, as one layered Photoshop file: the layers' names, blends, opacity and eyes, and a hidden Wireframe layer on top (the UV polygons — to select by). Save it there and the layers come back: the changed ones take their new pixels, new ones are added where they lie, the ones merged or deleted there go here too. The fill and adjustment layers and the groups stay as they are"), nullptr, nullptr, "pt_psd");
  QObject::connect(sl, &QAction::triggered, w, [s] { if (s->sendLayersHook) s->sendLayersHook(); else ui::toast(s->sendLayers()); });
  QAction* sv = ui::menuItem(sendM, QObject::tr("Send this view to the editor (projection)"), QObject::tr("The viewport as it is seen now opens in the editor: paint over it and save — what you changed is carried onto the model from this camera, as a new layer \"Projection\". The camera may be moved meanwhile"), nullptr, nullptr);
  QObject::connect(sv, &QAction::triggered, w, [s] { if (s->sendProjection) s->sendProjection(); });
  QAction* sm = ui::menuItem(sendM, QObject::tr("Send composed colour map to the editor"), QObject::tr("The whole composed colour map as a new layer when it comes back"), nullptr, nullptr);
  QObject::connect(sm, &QAction::triggered, w, [s] {
    if (s->doc.width() == 0) { ui::toast(QObject::tr("Nothing to send")); return; }
    s->ensureDoc(); const int id = s->doc.addPaint(Channel::Color, "From the editor", s->layer); s->layer = id; s->doc.layer(id)->image = s->doc.compose(Channel::Color); emit s->layersChanged();
    QString err; if (!s->bridge.sendImage(s->bridge.editor(), QString("%1@%2").arg(id).arg(s->doc.activeSet()), s->doc.layer(id)->image, &err)) ui::toast(err); });
  QAction* b2 = ui::menuItem(pathM, QObject::tr("3D-Coat…"), QObject::tr("Choose the 3D-Coat executable"), nullptr, nullptr); *b2p = b2; showEd(); delete dc;
  QObject::connect(b2, &QAction::triggered, w, [s, w, showEd] { const QString f = QFileDialog::getOpenFileName(w, QObject::tr("3D-Coat"), {}, "*.exe"); if (!f.isEmpty()) { s->bridge.setExePath(BridgeApp::ThreeDCoat, f); showEd(); } });

  addHelp(w, QObject::tr("3D-Coat: the selected mesh with its UVs (OBJ) goes out with the baked maps; textures painted there come back through the return folder: export them there (File → Export → Painted Textures)."));
  QAction* rb = ui::menuItem(pathM, QObject::tr("Return folder…"), QObject::tr("The folder 3D-Coat exports into: a new or changed picture there arrives as a layer"), nullptr, nullptr); v->addWidget(pathBtn);
  auto showRf = [s, rb] { rb->setText(QObject::tr("Return folder… (%1)").arg(s->bridge.returnFolder().isEmpty() ? QObject::tr("not set") : s->bridge.returnFolder())); }; showRf();
  QObject::connect(rb, &QAction::triggered, w, [s, w, showRf] { const QString d = QFileDialog::getExistingDirectory(w, QObject::tr("Return folder")); if (!d.isEmpty()) { s->bridge.setReturnFolder(d); showRf(); } });
  sendM->addSeparator(); QAction* sd = ui::menuItem(sendM, QObject::tr("Send mesh to 3D-Coat"), QObject::tr("The selected mesh as an OBJ with its UVs, the baked maps beside it"), nullptr, nullptr);
  QObject::connect(sd, &QAction::triggered, w, [s, vp, sc] {
    const Item* m = nullptr; for (auto it = vp->selection().items.rbegin(); it != vp->selection().items.rend(); ++it) if (const Item* x = sc->item(*it); x && x->type == ItemType::IMesh && x->poly) { m = x; break; }
    if (!m) { ui::toast(QObject::tr("Select a mesh")); return; }
    QHash<QString, QImage> maps; for (auto it = s->baked.begin(); it != s->baked.end(); ++it) maps[it.key()] = it.value();
    if (s->doc.width()) maps["colour"] = s->doc.compose(Channel::Color);
    QString name = m->name; name.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_"); if (name.isEmpty()) name = "mesh";
    QString err; if (!s->bridge.sendMesh(*m->poly, name, maps, &err)) ui::toast(err); else ui::toast(QObject::tr("Sent to 3D-Coat")); });
  v->addStretch(1);
  return w;
}

// ================= Lighting & Toon =================

QWidget* makePaintLookPanel(PaintSession* s) {
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(8, 6, 8, 8); v->setSpacing(5);
  auto changed = [s] { emit s->settingsChanged(); };
  note(v, QObject::tr("Lighting"));
  auto* list = new QListWidget; list->setMaximumHeight(90); list->setFocusPolicy(Qt::NoFocus); v->addWidget(list);
  auto* br = new QHBoxLayout; auto* addL = button(QObject::tr("＋ Light")); auto* delL = button(QObject::tr("Remove")); auto* preset = new QComboBox; preset->addItems({QObject::tr("Presets…"), QObject::tr("Studio (key, fill, rim)"), QObject::tr("Soft (one big light)"), QObject::tr("Flat (even)")});
  br->addWidget(addL); br->addWidget(delL); br->addWidget(preset, 1); v->addLayout(br);
  auto* az = slider(v, QObject::tr("Azimuth"), -180, 180, 1, 0, [](double x) { return QString::number(int(x)) + "°"; });
  auto* el = slider(v, QObject::tr("Elevation"), -90, 90, 1, 0, [](double x) { return QString::number(int(x)) + "°"; });
  auto* in = slider(v, QObject::tr("Intensity"), 0, 3, 0.05, 1, num);
  auto* lc = colourBtn(v, QObject::tr("Light colour"), Qt::white, [](const QColor&) {});
  auto* fol = check(v, QObject::tr("Follows the camera"), true, QObject::tr("Off: the light stands still in the world"));
  auto cur = [s, list]() -> Light* { const int r = list->currentRow(); return r >= 0 && r < int(s->rig.lights.size()) ? &s->rig.lights[size_t(r)] : nullptr; };
  auto fillList = [s, list] { const int keep = list->currentRow(); QSignalBlocker b(list); list->clear(); for (const Light& l : s->rig.lights) { auto* it = new QListWidgetItem(l.name); it->setFlags(it->flags() | Qt::ItemIsUserCheckable); it->setCheckState(l.on ? Qt::Checked : Qt::Unchecked); list->addItem(it); }
    list->setCurrentRow(std::clamp(keep, 0, std::max(0, list->count() - 1))); };
  auto fillProps = [=] { QSignalBlocker b1(az), b2(el), b3(in), b4(fol); Light* l = cur(); if (!l) return; az->setValue(l->azimuth); el->setValue(l->elevation); in->setValue(l->intensity); fol->setChecked(l->follow); lc->setProperty("c", l->color.name()); paintBtn(lc, l->color); };
  fillList(); fillProps();
  QObject::connect(list, &QListWidget::currentRowChanged, w, [=](int) { fillProps(); });
  QObject::connect(list, &QListWidget::itemChanged, w, [=](QListWidgetItem* it) { const int r = list->row(it); if (r >= 0 && r < int(s->rig.lights.size())) { s->rig.lights[size_t(r)].on = it->checkState() == Qt::Checked; changed(); } });
  QObject::connect(az, &SliderRow::changed, w, [=](double x) { if (Light* l = cur()) { l->azimuth = float(x); changed(); } });
  QObject::connect(el, &SliderRow::changed, w, [=](double x) { if (Light* l = cur()) { l->elevation = float(x); changed(); } });
  QObject::connect(in, &SliderRow::changed, w, [=](double x) { if (Light* l = cur()) { l->intensity = float(x); changed(); } });
  QObject::connect(fol, &QCheckBox::toggled, w, [=](bool on) { if (Light* l = cur()) { l->follow = on; changed(); } });
  QObject::disconnect(lc, &QPushButton::clicked, nullptr, nullptr);
  QObject::connect(lc, &QPushButton::clicked, w, [=] { Light* l = cur(); if (!l) return; const QColor c = QColorDialog::getColor(l->color, w); if (c.isValid()) { l->color = c; paintBtn(lc, c); changed(); } });
  QObject::connect(addL, &QPushButton::clicked, w, [=] { Light l; l.name = QObject::tr("Light %1").arg(s->rig.lights.size() + 1); s->rig.lights.push_back(l); fillList(); list->setCurrentRow(list->count() - 1); fillProps(); changed(); });
  QObject::connect(delL, &QPushButton::clicked, w, [=] { const int r = list->currentRow(); if (r >= 0 && r < int(s->rig.lights.size())) { s->rig.lights.erase(s->rig.lights.begin() + r); fillList(); fillProps(); changed(); } });
  QObject::connect(preset, &QComboBox::currentIndexChanged, w, [=](int i) {
    if (i == 1) s->rig = LightRig::studio();
    else if (i == 2) { s->rig = LightRig(); Light k; k.name = "Big"; k.azimuth = 20; k.elevation = 50; k.intensity = 1.2f; s->rig.lights = {k}; s->rig.ambientIntensity = 0.9f; }
    else if (i == 3) { s->rig = LightRig(); s->rig.lights.clear(); s->rig.ambientIntensity = 1.4f; }
    else return;
    QSignalBlocker b(preset); preset->setCurrentIndex(0); fillList(); fillProps(); changed(); });
  colourBtn(v, QObject::tr("Ambient colour"), s->rig.ambient, [=](const QColor& c) { s->rig.ambient = c; changed(); });
  auto* ai = slider(v, QObject::tr("Ambient"), 0, 2, 0.05, s->rig.ambientIntensity, num); QObject::connect(ai, &SliderRow::changed, w, [=](double x) { s->rig.ambientIntensity = float(x); changed(); });
  auto* env = new QLineEdit(s->rig.environment); env->setPlaceholderText(QObject::tr("Environment (HDRI) file…")); v->addWidget(env);
  QObject::connect(env, &QLineEdit::editingFinished, w, [=] { s->rig.environment = env->text(); changed(); });
  auto* envB = button(QObject::tr("Environment…")); v->addWidget(envB); QObject::connect(envB, &QPushButton::clicked, w, [=] { const QString f = QFileDialog::getOpenFileName(w, QObject::tr("Environment"), {}, "*.hdr *.exr *.png *.jpg"); if (!f.isEmpty()) { env->setText(f); s->rig.environment = f; changed(); } });
  auto* ei = slider(v, QObject::tr("Environment strength"), 0, 3, 0.05, s->rig.envIntensity, num); QObject::connect(ei, &SliderRow::changed, w, [=](double x) { s->rig.envIntensity = float(x); changed(); });
  auto* er = slider(v, QObject::tr("Environment rotation"), -180, 180, 1, s->rig.envRotation, [](double x) { return QString::number(int(x)) + "°"; }); QObject::connect(er, &SliderRow::changed, w, [=](double x) { s->rig.envRotation = float(x); changed(); });
  auto* sh = check(v, QObject::tr("Shadows"), s->rig.shadows); QObject::connect(sh, &QCheckBox::toggled, w, [=](bool on) { s->rig.shadows = on; changed(); });

  note(v, QObject::tr("Toon shader"));
  ToonSettings& T = s->toon;
  auto* on = check(v, QObject::tr("Toon shading"), T.on); QObject::connect(on, &QCheckBox::toggled, w, [=](bool x) { s->toon.on = x; changed(); });
  auto* bands = slider(v, QObject::tr("Light steps"), 2, 8, 1, T.bands, [](double x) { return QString::number(int(x)); }); QObject::connect(bands, &SliderRow::changed, w, [=](double x) { s->toon.bands = int(x); changed(); });
  auto* thr = slider(v, QObject::tr("Threshold"), 0, 1, 0.01, T.threshold, num); QObject::connect(thr, &SliderRow::changed, w, [=](double x) { s->toon.threshold = float(x); changed(); });
  auto* soft = slider(v, QObject::tr("Softness"), 0, 0.5, 0.005, T.softness, [](double x) { return QString::number(x, 'f', 3); }); QObject::connect(soft, &SliderRow::changed, w, [=](double x) { s->toon.softness = float(x); changed(); });
  colourBtn(v, QObject::tr("Shadow tint"), T.shadow, [=](const QColor& c) { s->toon.shadow = c; changed(); });
  auto* sm = slider(v, QObject::tr("Shadow tint amount"), 0, 1, 0.01, T.shadowMix, pct); QObject::connect(sm, &SliderRow::changed, w, [=](double x) { s->toon.shadowMix = float(x); changed(); });
  auto* rim = slider(v, QObject::tr("Rim light"), 0, 1, 0.01, T.rim, pct); QObject::connect(rim, &SliderRow::changed, w, [=](double x) { s->toon.rim = float(x); changed(); });
  auto* rimW = slider(v, QObject::tr("Rim width"), 0, 1, 0.01, T.rimWidth, pct); QObject::connect(rimW, &SliderRow::changed, w, [=](double x) { s->toon.rimWidth = float(x); changed(); });
  auto* ol = slider(v, QObject::tr("Outline"), 0, 8, 0.1, T.outline, [](double x) { return QString::number(x, 'f', 1) + " px"; }); QObject::connect(ol, &SliderRow::changed, w, [=](double x) { s->toon.outline = float(x); changed(); });
  colourBtn(v, QObject::tr("Outline colour"), T.outlineColor, [=](const QColor& c) { s->toon.outlineColor = c; changed(); });

  note(v, QObject::tr("Anisotropic highlight (hair, satin): a band across the Flow map's direction"));
  auto* an = check(v, QObject::tr("Anisotropic highlight"), T.aniso); QObject::connect(an, &QCheckBox::toggled, w, [=](bool x) { s->toon.aniso = x; changed(); });
  auto* src = combo(v, QObject::tr("Flow from"), {QObject::tr("None"), QObject::tr("Painted Flow map"), QObject::tr("Hair strands (UV direction)")}, int(T.flow)); QObject::connect(src, &QComboBox::currentIndexChanged, w, [=](int i) { s->toon.flow = FlowSource(i); changed(); });
  auto* as = slider(v, QObject::tr("Strength"), 0, 1, 0.01, T.anisoStrength, pct); QObject::connect(as, &SliderRow::changed, w, [=](double x) { s->toon.anisoStrength = float(x); changed(); });
  auto* aw = slider(v, QObject::tr("Width"), 0.02, 1, 0.01, T.anisoWidth, pct); QObject::connect(aw, &SliderRow::changed, w, [=](double x) { s->toon.anisoWidth = float(x); changed(); });
  auto* ash = slider(v, QObject::tr("Shift"), -1, 1, 0.01, T.anisoShift, num, QObject::tr("Slides the band along the strand")); QObject::connect(ash, &SliderRow::changed, w, [=](double x) { s->toon.anisoShift = float(x); changed(); });
  auto* a2 = slider(v, QObject::tr("Second band"), 0, 1, 0.01, T.anisoSecondary, pct, QObject::tr("The hair's second, shifted highlight")); QObject::connect(a2, &SliderRow::changed, w, [=](double x) { s->toon.anisoSecondary = float(x); changed(); });
  colourBtn(v, QObject::tr("Highlight colour"), T.anisoColor, [=](const QColor& c) { s->toon.anisoColor = c; changed(); });
  colourBtn(v, QObject::tr("Second band colour"), T.anisoColor2, [=](const QColor& c) { s->toon.anisoColor2 = c; changed(); });
  auto* st = check(v, QObject::tr("Cut to hard bands"), T.anisoSteps); QObject::connect(st, &QCheckBox::toggled, w, [=](bool x) { s->toon.anisoSteps = x; changed(); });
  note(v, QObject::tr("The settings are kept in the project; the viewport's preview of them comes with the shader stage."));
  return w;
}


// ---- the reference ----
namespace {
class RefView : public QWidget {
public:
  QImage img; double zoom = 0; QPointF off; bool panning = false, moved = false; QPoint press, last; std::function<void(const QColor&)> picked; std::function<void(const QImage&)> dropped;
  explicit RefView(QWidget* p = nullptr) : QWidget(p) { setAcceptDrops(true); setMouseTracking(true); setFocusPolicy(Qt::ClickFocus); setMinimumSize(160, 140); setCursor(Qt::CrossCursor); }
  double fitZoom() const { return img.isNull() ? 1 : std::min(width() / double(img.width()), height() / double(img.height())); }
  double z() const { return zoom > 0 ? zoom : fitZoom(); }
  QPointF origin() const { const double k = z(); return zoom > 0 ? off : QPointF((width() - img.width() * k) / 2, (height() - img.height() * k) / 2); }
  void setImage(const QImage& i) { img = i.convertToFormat(QImage::Format_ARGB32); zoom = 0; update(); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.fillRect(rect(), QColor(30, 30, 32));
    if (img.isNull()) { p.setPen(QColor(140, 140, 150)); p.drawText(rect().adjusted(10, 10, -10, -10), Qt::AlignCenter | Qt::TextWordWrap, QObject::tr("Drop a picture here, Ctrl+V, or open one")); return; }
    p.setRenderHint(QPainter::SmoothPixmapTransform, z() < 2); const double k = z(); p.drawImage(QRectF(origin(), QSizeF(img.width() * k, img.height() * k)), img); }
  void wheelEvent(QWheelEvent* e) override {
    if (img.isNull()) return; const double k0 = z(); const QPointF o0 = origin(), at = e->position();
    const double k1 = std::clamp(k0 * std::pow(1.0015, e->angleDelta().y()), 0.02, 40.0); off = at - (at - o0) * (k1 / k0); zoom = k1; update(); }
  void mousePressEvent(QMouseEvent* e) override { press = last = e->pos(); moved = false; panning = true; if (zoom <= 0) { off = origin(); zoom = z(); } }
  void mouseMoveEvent(QMouseEvent* e) override { if (!panning) return; if ((e->pos() - press).manhattanLength() > 3) moved = true; if (moved) { off += e->pos() - last; last = e->pos(); setCursor(Qt::ClosedHandCursor); update(); } }
  void mouseReleaseEvent(QMouseEvent* e) override {
    panning = false; setCursor(Qt::CrossCursor);
    if (moved || img.isNull() || e->button() != Qt::LeftButton) return;   // (a click, not a drag: the colour under it)
    const QPointF t = (QPointF(e->pos()) - origin()) / z(); const int x = int(t.x()), y = int(t.y());
    if (x >= 0 && y >= 0 && x < img.width() && y < img.height() && picked) picked(QColor::fromRgb(img.pixel(x, y))); }
  void mouseDoubleClickEvent(QMouseEvent*) override { zoom = 0; update(); }   // (fit the window again)
  void dragEnterEvent(QDragEnterEvent* e) override { if (e->mimeData()->hasUrls() || e->mimeData()->hasImage()) e->acceptProposedAction(); }
  void dropEvent(QDropEvent* e) override {
    QImage i; if (e->mimeData()->hasImage()) i = qvariant_cast<QImage>(e->mimeData()->imageData());
    else for (const QUrl& u : e->mimeData()->urls()) { i = QImage(u.toLocalFile()); if (!i.isNull()) { QSettings().setValue("paint/refImage", u.toLocalFile()); break; } }
    if (!i.isNull()) { setImage(i); if (dropped && e->mimeData()->hasImage()) dropped(i); } }
  void keyPressEvent(QKeyEvent* e) override {
    if (e->matches(QKeySequence::Paste)) { const QImage i = QGuiApplication::clipboard()->image(); if (!i.isNull()) { setImage(i); if (dropped) dropped(i); } return; }
    QWidget::keyPressEvent(e); }
};
}  // namespace

QWidget* makePaintReferencePanel(PaintSession* s) {
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(4, 4, 4, 4); v->setSpacing(4);
  auto* view = new RefView; auto* bar = new QHBoxLayout; bar->setSpacing(3);
  auto btn = [&](const char* icon, const QString& tip) { auto* b = new QPushButton; b->setIcon(ui::icon(icon)); b->setIconSize(QSize(20, 20)); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); b->setProperty("plBtn", true); bar->addWidget(b); return b; };
  // (a pasted or dropped picture kept as a file in Documents — the next start opens it again)
  auto keep = [](const QImage& i) { const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/HairHarpy References"; QDir().mkpath(dir);
    const QString f = dir + "/pasted_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png"; if (i.save(f)) QSettings().setValue("paint/refImage", f); };
  view->dropped = keep;
  view->picked = [s](const QColor& c) { s->color = c; emit s->brushChanged(); ui::hud(QObject::tr("Colour %1 from the reference").arg(c.name()), 900); };
  QObject::connect(btn("pt_psd", QObject::tr("Open a picture")), &QPushButton::clicked, w, [view] {
    const QString f = QFileDialog::getOpenFileName(nullptr, QObject::tr("Reference"), QFileInfo(QSettings().value("paint/refImage").toString()).absolutePath(), QObject::tr("Pictures (*.png *.jpg *.jpeg *.webp *.bmp *.tga *.psd)"));
    if (f.isEmpty()) return; const QImage i(f); if (i.isNull()) { ui::toast(QObject::tr("The picture can't be read")); return; } QSettings().setValue("paint/refImage", f); view->setImage(i); });
  QObject::connect(btn("pt_paste", QObject::tr("Paste a picture from the clipboard (Ctrl+V over the picture too)")), &QPushButton::clicked, w, [view, keep] {
    const QImage i = QGuiApplication::clipboard()->image(); if (i.isNull()) { ui::toast(QObject::tr("No picture in the clipboard")); return; } view->setImage(i); keep(i); });
  QObject::connect(btn("uv_fit", QObject::tr("Fit the picture to the window (or double click on it)")), &QPushButton::clicked, w, [view] { view->zoom = 0; view->update(); });
  QObject::connect(btn("rc_delete", QObject::tr("Take the picture away")), &QPushButton::clicked, w, [view] { view->img = {}; view->update(); QSettings().remove("paint/refImage"); });
  bar->addStretch(1); v->addLayout(bar); v->addWidget(view, 1);
  if (const QString f = QSettings().value("paint/refImage").toString(); !f.isEmpty()) { const QImage i(f); if (!i.isNull()) view->setImage(i); }
  addHelp(w, QObject::tr("• Open a picture, drop one from a folder or a browser, or paste it (Ctrl+V over the picture) — it is kept for the next start\n• The wheel zooms round the pointer, a drag moves it, a double click fits it again\n• A click takes the colour under it into the palette\n• The panel floats over the work or docks to any edge, as the others"));
  return w;
}

}  // namespace hm
