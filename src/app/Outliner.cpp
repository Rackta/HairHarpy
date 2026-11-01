#include "app/Outliner.h"
#include "app/Scheme.h"
#include "app/TexWindow.h"
#include "app/Extras.h"
#include "scene/Cocoons.h"
#include "scene/Looks.h"
#include "scene/Edit.h"
#include "ui/Keymap.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QColorDialog>
#include <QDrag>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace hm {

namespace {
const char* MIME = "application/x-hm-outliner";
const QColor TXT(0xcf, 0xcf, 0xcf), MUT(0x8a, 0x8a, 0x8a), GON(0x6f, 0xdc, 0x8c), GOFF(0x77, 0x77, 0x77);

const Item* parentOf(const Scene& sc, const Item* it) { return it && it->parent >= 0 ? sc.item(it->parent) : nullptr; }
bool rawB(const Item& it, const char* k) { return it.raw.value(k).toBool(); }
bool svcGroup(const Item& it) { return it.type == ItemType::Group && (rawB(it, "guidesGroup") || rawB(it, "meshesGroup") || rawB(it, "importGroup")); }
bool sceneGroup(const Item* g) { return g && (svcGroup(*g) || g->raw.value("side").toString() == "scene"); }
bool sceneType(ItemType t) { return t == ItemType::Head || t == ItemType::Mesh || t == ItemType::IMesh || t == ItemType::Guide || t == ItemType::Cocoon; }
bool shellMod(ItemType t) { return t == ItemType::Tip || t == ItemType::Even || t == ItemType::PInf || t == ItemType::Guide; }
bool groupHasGuides(const Scene& sc, const Item& g) {
  for (int id : g.children) { const Item* c = sc.item(id); if (c && (c->type == ItemType::Guide || (c->type == ItemType::Group && groupHasGuides(sc, *c)))) return true; }
  return false;
}
void descendants(const Scene& sc, const Item& it, std::vector<int>& out) {
  for (int id : it.children) if (const Item* c = sc.item(id)) { out.push_back(id); descendants(sc, *c, out); }
}
std::vector<const Item*> ofType(const Scene& sc, ItemType t) {
  std::vector<const Item*> v; for (auto& [id, it] : sc.items) if (it.type == t) v.push_back(&it);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->order < b->order; });   // (the items Map's order)
  return v;
}
const Item* cocoonParent(const Scene& sc, const Item& k) { const Item* p = parentOf(sc, &k); return p && p->type == ItemType::Cocoon ? p : nullptr; }
std::vector<int>& kidsOf(Scene& sc, int parent) { if (parent < 0) return sc.roots; return sc.item(parent)->children; }
bool guideOn(const Scene& sc, const Item& g) {
  if (rawB(g, "off")) return false;
  for (const Item* p = parentOf(sc, &g); p; p = parentOf(sc, p)) if (p->type == ItemType::Group && rawB(*p, "guidesOff")) return false;
  return true;
}
QString xkindIcon(const QString& k) {
  if (k == "mask") return "◐"; if (k == "thick") return "⬤"; if (k == "twist") return "⟳"; if (k == "noise") return "≋"; if (k == "curl") return "➰"; if (k == "xform") return "✥";
  return "✦";
}
// the 🏷 of a label, tinted as the HTML version's hue-rotate filters
QPixmap tagPix(int hue, double sat) {
  static std::map<int, QPixmap> cache; auto it = cache.find(hue); if (it != cache.end()) return it->second;
  QImage img(32, 32, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
  { QPainter p(&img); QFont f = QApplication::font(); f.setPixelSize(22); p.setFont(f); p.drawText(img.rect(), Qt::AlignCenter, "🏷"); }
  if (hue) for (int y = 0; y < img.height(); y++) for (int x = 0; x < img.width(); x++) {
    QColor c = img.pixelColor(x, y); if (!c.alpha()) continue;
    float h, s, l, a; c.getHslF(&h, &s, &l, &a); if (h < 0) h = 0;
    c.setHslF(std::fmod(h + hue / 360.f, 1.f), std::min(1.f, float(s * sat)), l, a); img.setPixelColor(x, y, c);
  }
  QPixmap pm = QPixmap::fromImage(img); cache[hue] = pm; return pm;
}
QPixmap grey(const QPixmap& p) { QImage i = p.toImage().convertToFormat(QImage::Format_ARGB32); for (int y = 0; y < i.height(); y++) for (int x = 0; x < i.width(); x++) { QColor c = i.pixelColor(x, y); int g = qGray(c.rgb()); i.setPixelColor(x, y, QColor(g, g, g, c.alpha())); } return QPixmap::fromImage(i); }

// a border to drag (the width of a column; double-click — its default)
class Grip : public QWidget {
public:
  std::function<void(int)> moved; std::function<void()> done, reset;
  explicit Grip(QWidget* p) : QWidget(p) { setCursor(Qt::SplitHCursor); setFixedWidth(6); setAttribute(Qt::WA_Hover); }
protected:
  void paintEvent(QPaintEvent*) override { if (underMouse() || drag_) { QPainter p(this); p.fillRect(rect(), QColor(111, 159, 232, 90)); } }
  void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) { drag_ = true; x0_ = int(e->globalPosition().x()); } }
  void mouseMoveEvent(QMouseEvent* e) override { if (drag_ && moved) { int x = int(e->globalPosition().x()); moved(x - x0_); x0_ = x; } }
  void mouseReleaseEvent(QMouseEvent*) override { if (drag_ && done) done(); drag_ = false; update(); }
  void mouseDoubleClickEvent(QMouseEvent*) override { if (reset) reset(); }
  bool event(QEvent* e) override { if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave) update(); return QWidget::event(e); }
private:
  bool drag_ = false; int x0_ = 0;
};

class EscFilter : public QObject {
public:
  std::function<void()> esc;
  using QObject::QObject;
  bool eventFilter(QObject*, QEvent* e) override {
    if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) { if (esc) esc(); return true; }
    return false;
  }
};
}  // namespace

// ======================= one tree =======================
OutlinerView::OutlinerView(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true); setAcceptDrops(true); setAttribute(Qt::WA_OpaquePaintEvent);
}

void OutlinerView::begin() {
  rows_.clear(); boxes_.clear(); links_.clear(); y_ = 4; hover_ = -1; dropRow_ = -1; hoverLink_ = -1;
  for (QWidget* w : widgets_) { w->hide(); w->deleteLater(); }
  widgets_.clear();
  if (edit_) { edit_->hide(); edit_->deleteLater(); edit_ = nullptr; }
}

OutlinerView::Row& OutlinerView::add(int x0, int x1, int h) {
  rows_.emplace_back(); Row& r = rows_.back(); r.h = h; r.r = QRect(x0, y_, std::max(0, x1 - x0), h); return r;
}

int OutlinerView::boxOpen(int x) { boxes_.push_back({x, y_, y_}); return int(boxes_.size()) - 1; }
void OutlinerView::boxClose(int i) { boxes_[size_t(i)].y1 = y_; }

void OutlinerView::addWidget(QWidget* w, int x0, int x1) {
  w->setParent(this); w->adjustSize();
  int h = w->sizeHint().height(); w->setGeometry(x0, y_, x1 - x0, h); w->show();
  widgets_.push_back(w); y_ += h;
}

void OutlinerView::place(Row& r) {
  QFontMetrics fm(font()); QFont small = font(); small.setPixelSize(9); QFontMetrics fs(small);
  int x = r.r.left() + r.pad, cy = r.r.top() + 2, ch = r.h - 4;
  for (Cell& c : r.left) { if (c.w < 0) c.w = (c.tagBox ? fs : fm).horizontalAdvance(c.text) + 8; c.r = QRect(x, cy, c.w, ch); x += c.w + r.gap; }
  int xr = r.r.right() + 1 - r.padR;
  for (auto it = r.right.rbegin(); it != r.right.rend(); ++it) { Cell& c = *it; if (c.w < 0) c.w = (c.tagBox ? fs : fm).horizontalAdvance(c.text) + 8; xr -= c.w; c.r = QRect(xr, cy, c.w, ch); xr -= r.gap; }
  r.name.r = QRect(x, cy, std::max(0, xr - x), ch);
}

void OutlinerView::finish() {
  for (Row& r : rows_) place(r);
  y_ += 4;
  setMinimumHeight(y_);
  update();
}

const OutlinerView::Row* OutlinerView::row(int id) const { for (auto& r : rows_) if (r.id == id && !r.empty) return &r; return nullptr; }

int OutlinerView::rowAt(const QPoint& p) const {
  for (size_t i = 0; i < rows_.size(); i++) if (!rows_[i].empty && rows_[i].r.contains(p)) return int(i);
  return -1;
}

const OutlinerView::Cell* OutlinerView::cellAt(const Row& r, const QPoint& p) const {
  for (auto* list : {&r.left, &r.right}) for (const Cell& c : *list) if ((c.click || c.dbl || c.rclick || c.eye) && c.r.contains(p)) return &c;
  return nullptr;
}

void OutlinerView::paintEvent(QPaintEvent* e) {
  QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(e->rect(), QColor(0x26, 0x26, 0x26));
  for (const Box& b : boxes_) {
    QRectF r(b.x + 0.5, b.y0 + 0.5, width() - 3 - b.x - 1, b.y1 - b.y0 - 1);
    p.setPen(QColor(0x2e, 0x2e, 0x2e)); p.setBrush(QColor(0, 0, 0, 71)); p.drawRoundedRect(r, 5, 5);
  }
  QFont base = font(), small = font(); small.setPixelSize(9); QFont pre = font(); pre.setPixelSize(11);
  for (size_t i = 0; i < rows_.size(); i++) {
    const Row& r = rows_[i]; if (r.empty || !r.r.intersects(e->rect())) continue;
    bool sel = (r.flags & Sel) || (r.id >= 0 && boxHit_.count(r.id));
    QColor bg;
    if (sel) bg = QColor(0x3a, 0x55, 0x85);
    else if (r.flags & LockSel) bg = QColor(0x2a, 0x3a, 0x3c);
    else if (r.flags & Rel) bg = QColor(0x4d, 0x3b, 0x2c);
    else if (r.flags & ChildSel) bg = QColor(0x2b, 0x34, 0x46);
    else if (int(i) == hover_ && !(r.flags & Hint)) bg = QColor(0x2f, 0x2f, 0x2f);
    if (int(i) == dropRow_ && dropZone_ == "into") bg = QColor(0x2d, 0x4a, 0x33);
    if (r.id >= 0 && r.id == flashId_) bg = QColor(0x5a, 0x6f, 0x9a);
    p.setPen(Qt::NoPen);
    if (bg.isValid()) p.fillRect(r.r, bg);
    if ((r.flags & LockSel) && !sel) p.fillRect(QRect(r.r.left(), r.r.top(), 3, r.r.height()), QColor(0x7f, 0xc8, 0xd0));
    if (r.flags & Section) p.fillRect(QRect(r.r.left(), r.r.top(), r.r.width(), 1), QColor(0x3a, 0x3a, 0x3a));
    if (int(i) == dropRow_ && (dropZone_ == "before" || dropZone_ == "after"))
      p.fillRect(QRect(r.r.left(), dropZone_ == "before" ? r.r.top() : r.r.bottom() - 1, r.r.width(), 2), QColor(0x66, 0xaa, 0xff));
    bool hid = r.flags & Hid;
    auto drawCell = [&](const Cell& c, bool dim) {
      double op = c.op * (dim && hid ? 0.4 : 1);
      p.setOpacity(op);
      if (c.swatchCell) {
        QRectF s(c.r.center().x() - 7 + 0.5, c.r.center().y() - 7 + 0.5, 14, 14);
        p.setPen(QColor(0x55, 0x55, 0x55)); p.setBrush(c.swatchOn ? QBrush(c.swatch) : QBrush(QColor(0x30, 0x30, 0x30))); p.drawRoundedRect(s, 3, 3);
        if (!c.swatchOn) { p.setPen(QPen(QColor(0x66, 0x66, 0x66), 1)); p.drawLine(s.bottomLeft() + QPointF(2, -2), s.topRight() + QPointF(-2, 2)); }
      } else if (!c.pix.isNull()) {
        QRect pr(c.r.center().x() - 8, c.r.center().y() - 8, 16, 16); p.drawPixmap(pr, c.pix);
      } else if (c.tagBox) {
        QRectF t(c.r.left() + 0.5, c.r.center().y() - 6.5, c.r.width() - 2, 13);
        p.setPen(c.tagBorder); p.setBrush(c.tagBg); p.drawRoundedRect(t, 3, 3);
        p.setFont(small); p.setPen(c.tagFg); p.drawText(t, Qt::AlignCenter, c.text); p.setFont(base);
      } else if (!c.text.isEmpty()) {
        p.setPen(c.color.isValid() ? c.color : TXT); p.drawText(c.r, Qt::AlignCenter, c.text);
      }
      p.setOpacity(1);
    };
    for (size_t k = 0; k < r.left.size(); k++) drawCell(r.left[k], k > 0);
    for (const Cell& c : r.right) drawCell(c, false);
    // the name (a dim prefix first)
    QRect nr = r.name.r; if (nr.width() <= 0) continue;
    p.setOpacity(hid ? 0.4 : 1);
    if (!r.name.prefix.isEmpty()) {
      p.setFont(pre); QFontMetrics fp(pre); QString t = fp.elidedText(r.name.prefix, Qt::ElideRight, nr.width());
      p.setPen(r.name.prefixColor); p.drawText(nr, Qt::AlignVCenter | Qt::AlignLeft, t);
      nr.setLeft(nr.left() + fp.horizontalAdvance(t)); p.setFont(base);
    }
    QColor nc = r.nameColor.isValid() ? r.nameColor : (r.flags & Section) ? QColor(0x99, 0xaa, 0xbb) : (r.flags & Hint) ? MUT
              : (r.flags & LockSel) && !sel ? QColor(0xbf, 0xee, 0xf2) : (r.flags & ChildSel) && !sel ? QColor(0xcf, 0xe0, 0xff) : TXT;
    p.setPen(nc);
    if (r.flags & Hint) { QFont f = font(); f.setPixelSize(11); p.setFont(f); p.drawText(nr.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, r.name.text); p.setFont(base); }
    else p.drawText(nr, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(r.name.text, Qt::ElideRight, nr.width()));
    p.setOpacity(1);
  }
  // 🔗 the links in the gutter
  for (size_t i = 0; i < links_.size(); i++) {
    double yb = 0; QPainterPath path = linkPath(i, &yb); if (path.isEmpty()) continue;
    p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor(0xe8, 0xa8, 0x5a), int(i) == hoverLink_ ? 3 : 1.5)); p.drawPath(path);
    QPainterPath tip; tip.moveTo(9, yb - 3); tip.lineTo(13, yb); tip.lineTo(9, yb + 3); tip.closeSubpath(); p.fillPath(tip, QColor(0xe8, 0xa8, 0x5a));
  }
  if (ld_.on) {                                                       // (Alt + drag: the line to the pointer)
    if (const Row* r = row(ld_.from)) { p.setPen(QPen(QColor(0xe8, 0xa8, 0x5a), 2, Qt::DashLine)); p.drawLine(QPointF(r->r.left() + 8, r->r.center().y()), QPointF(ld_.at)); }
  }
  if (!box_.isNull()) { p.setPen(QColor(0x99, 0xcc, 0xff)); p.setBrush(QColor(0x99, 0xcc, 0xff, 30)); p.drawRect(QRectF(box_).adjusted(0.5, 0.5, -0.5, -0.5)); }
  if (dropZone_ == "root") { p.setPen(Qt::NoPen); p.setBrush(QColor(111, 159, 232, 20)); p.drawRect(rect()); }
}

QPainterPath OutlinerView::linkPath(size_t i, double* yb) const {
  const Row *a = row(links_[i].first), *b = row(links_[i].second); if (!a || !b) return {};
  double ya = a->r.center().y(), y2 = b->r.center().y(), x = 3 + double(i % 3) * 3;
  QPainterPath P; P.moveTo(14, ya); P.lineTo(x, ya); P.lineTo(x, y2); P.lineTo(12, y2);
  if (yb) *yb = y2; return P;
}
int OutlinerView::linkAt(const QPoint& p) const {
  if (p.x() > 15) return -1;
  QPainterPathStroker st; st.setWidth(8);
  for (size_t i = 0; i < links_.size(); i++) { QPainterPath path = linkPath(i); if (!path.isEmpty() && st.createStroke(path).contains(p)) return int(i); }
  return -1;
}

void OutlinerView::mousePressEvent(QMouseEvent* e) {
  if (pie && ui::Keymap::get().pieMouse("main", ui::Keymap::buttonOf(e->button()), ui::Keymap::modsOf(e->modifiers()))) { pie(e->globalPosition().toPoint()); return; }   // (its own pie)
  if (e->button() != Qt::LeftButton) return;
  if (int li = linkAt(e->position().toPoint()); li >= 0) { auto l = links_[size_t(li)]; if (linkClicked) linkClicked(l.first, l.second, e->modifiers()); return; }
  if (e->modifiers() & Qt::AltModifier) {                             // Alt + drag from a cocoon: a link
    int i = rowAt(e->position().toPoint());
    if (i >= 0 && rows_[size_t(i)].drag == 2) { ld_ = {true, rows_[size_t(i)].id, e->position().toPoint()}; grabKeyboard(); update(); return; }
  }
  pr_ = {}; pr_.on = true; pr_.at = e->position().toPoint(); pr_.mods = e->modifiers(); pr_.row = rowAt(pr_.at);
  if (pr_.row >= 0) {
    const Cell* c = cellAt(rows_[size_t(pr_.row)], pr_.at);
    if (c && c->eye && (e->modifiers() & Qt::ShiftModifier) && eyeStart && rows_[size_t(pr_.row)].id >= 0) { pr_.eye = true; int id = rows_[size_t(pr_.row)].id; eyeStart(id); return; }
  }
}

void OutlinerView::mouseMoveEvent(QMouseEvent* e) {
  QPoint m = e->position().toPoint();
  if (ld_.on) {
    ld_.at = m; int i = rowAt(m); dropRow_ = i >= 0 && rows_[size_t(i)].drop == 2 && rows_[size_t(i)].id != ld_.from ? i : -1; dropZone_ = dropRow_ >= 0 ? "into" : QString();
    update(); return;
  }
  if (int li = linkAt(m); li != hoverLink_) { hoverLink_ = li; update(); }
  int h = rowAt(m); if (h != hover_) { hover_ = h; update(); }
  if (!pr_.on) return;
  if (pr_.eye) {
    if (h >= 0) { const Cell* c = cellAt(rows_[size_t(h)], m); if (c && c->eye && rows_[size_t(h)].id >= 0 && eyeOver) { int id = rows_[size_t(h)].id; eyeOver(id); } }
    return;
  }
  if (!pr_.moved && (m - pr_.at).manhattanLength() < 5) return;
  pr_.moved = true;
  if (!pr_.box) {
    const Row* R = pr_.row >= 0 ? &rows_[size_t(pr_.row)] : nullptr;
    if (R && cellAt(*R, pr_.at)) { pr_.on = false; return; }      // (from a cell: neither a drag nor a box)
    bool dragSel = R && R->drag == 1 && selected && selected(R->id) && !(pr_.mods & Qt::ControlModifier);
    if (R && (dragSel || R->drag == 2)) {
      int kind = R->drag, id = R->id; pr_.on = false;
      std::vector<int> ids = kind == 1 && dragIds ? dragIds(id) : std::vector<int>{id};
      QStringList s; for (int x : ids) s << QString::number(x);
      auto* md = new QMimeData; md->setData(MIME, QString("%1:%2").arg(kind).arg(s.join(',')).toUtf8());
      auto* d = new QDrag(this); d->setMimeData(md); d->exec(Qt::MoveAction);
      return;
    }
    pr_.box = true;
  }
  QRect b = QRect(pr_.at, m).normalized(); b.setLeft(std::max(0, b.left())); b.setRight(std::min(width() - 1, b.right()));
  box_ = b; boxHit_.clear();
  for (auto& r : rows_) if (r.id >= 0 && !r.empty && !(r.flags & (Section | Hint)) && r.r.intersects(b)) boxHit_.insert(r.id);
  update();
}

void OutlinerView::mouseReleaseEvent(QMouseEvent* e) {
  if (ld_.on) {
    int to = dropRow_ >= 0 ? rows_[size_t(dropRow_)].id : -1, from = ld_.from;
    ld_ = {}; dropRow_ = -1; dropZone_.clear(); releaseKeyboard(); update();
    if (to >= 0 && linkMade) linkMade(from, to);
    return;
  }
  if (e->button() != Qt::LeftButton || !pr_.on) { pr_.on = false; return; }
  Press p = pr_; pr_ = {};
  if (p.eye) { if (eyeEnd) eyeEnd(); return; }
  if (p.box) { std::set<int> hit; hit.swap(boxHit_); box_ = {}; update(); if (boxSelect) boxSelect(hit, p.mods & (Qt::ShiftModifier | Qt::ControlModifier)); return; }
  if (p.moved || p.dbl) return;
  Qt::KeyboardModifiers mods = e->modifiers();
  if (p.row < 0) { if (emptyClick) emptyClick(mods); return; }
  const Row& R = rows_[size_t(p.row)];
  const Cell* c = cellAt(R, p.at);
  Click f = c ? c->click : R.click;
  if (f) f(mods);                                   // (it may rebuild the rows: a copy called)
}

void OutlinerView::mouseDoubleClickEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  QPoint m = e->position().toPoint(); int i = rowAt(m);
  pr_ = {}; pr_.on = true; pr_.at = m; pr_.row = i; pr_.mods = e->modifiers();
  if (i < 0) return;
  const Row& R = rows_[size_t(i)]; const Cell* c = cellAt(R, m);
  Click f = c ? c->dbl : (R.name.dbl && R.name.r.contains(m)) ? R.name.dbl : R.dbl;
  if (!f) return;                                    // (no double-click here: the second click is a click)
  pr_.dbl = true;
  f(e->modifiers());
}

void OutlinerView::contextMenuEvent(QContextMenuEvent* e) {
  if (pie && ui::Keymap::get().pieMouse("main", ui::Keymap::buttonOf(Qt::RightButton), ui::Keymap::modsOf(e->modifiers()))) return;   // (the pie opened on the press)
  int i = rowAt(e->pos()); if (i < 0) return;
  const Cell* c = cellAt(rows_[size_t(i)], e->pos());
  if (c && c->rclick) { Click f = c->rclick; f(e->modifiers()); }
}

void OutlinerView::leaveEvent(QEvent*) { if (hover_ >= 0) { hover_ = -1; update(); } }

bool OutlinerView::event(QEvent* e) {
  if (e->type() == QEvent::KeyPress && ld_.on && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) { ld_ = {}; dropRow_ = -1; dropZone_.clear(); releaseKeyboard(); update(); return true; }
  if (e->type() == QEvent::ToolTip) {
    auto* h = static_cast<QHelpEvent*>(e); int i = rowAt(h->pos());
    QString tip;
    if (int li = linkAt(h->pos()); li >= 0 && linkTip) { QToolTip::showText(h->globalPos(), linkTip(links_[size_t(li)].first, links_[size_t(li)].second), this); return true; }
    if (i >= 0) {
      const Row& R = rows_[size_t(i)];
      for (auto* list : {&R.left, &R.right}) for (const Cell& c : *list) if (c.r.contains(h->pos()) && !c.tip.isEmpty()) tip = c.tip;
      if (tip.isEmpty()) tip = R.name.r.contains(h->pos()) && !R.name.tip.isEmpty() ? R.name.tip : R.tip;
    }
    if (tip.isEmpty()) QToolTip::hideText(); else QToolTip::showText(h->globalPos(), tip, this);
    return true;
  }
  return QWidget::event(e);
}

void OutlinerView::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  if (width() != lastW_) { lastW_ = width(); if (resized) QTimer::singleShot(0, this, [this] { if (resized) resized(); }); }
}

void OutlinerView::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasFormat(MIME)) e->acceptProposedAction(); }

void OutlinerView::dragMoveEvent(QDragMoveEvent* e) {
  QString s = QString::fromUtf8(e->mimeData()->data(MIME)); int kind = s.section(':', 0, 0).toInt();
  int dragged = s.section(':', 1).section(',', 0, 0).toInt();
  QPoint m = e->position().toPoint(); int i = rowAt(m);
  dropRow_ = -1; dropZone_ = "root";
  if (i >= 0) {
    const Row& R = rows_[size_t(i)];
    if (kind == 2) { if (R.drop == 2 && R.id != dragged) { dropRow_ = i; dropZone_ = "into"; } }
    else if (R.drop == 1) {
      double y = double(m.y() - R.r.top()) / R.r.height();
      dropRow_ = i; dropZone_ = R.group ? (y < 0.25 ? "before" : y > 0.75 ? "after" : "into") : (y < 0.5 ? "before" : "after");
    }
  }
  e->acceptProposedAction(); update();
}

void OutlinerView::dragLeaveEvent(QDragLeaveEvent*) { dropRow_ = -1; dropZone_.clear(); update(); }

void OutlinerView::dropEvent(QDropEvent* e) {
  QString s = QString::fromUtf8(e->mimeData()->data(MIME)); int kind = s.section(':', 0, 0).toInt();
  std::vector<int> ids; for (const QString& x : s.section(':', 1).split(',', Qt::SkipEmptyParts)) ids.push_back(x.toInt());
  int target = dropRow_ >= 0 ? rows_[size_t(dropRow_)].id : -1; QString zone = dropZone_;
  dropRow_ = -1; dropZone_.clear(); update();
  e->acceptProposedAction();
  if (dropped && !ids.empty()) { auto f = dropped; QTimer::singleShot(0, this, [f, kind, ids, target, zone] { f(kind, ids, target, zone); }); }
}

void OutlinerView::editName(int id, const QString& text, std::function<void(const QString&)> done) {
  const Row* R = row(id); if (!R) return;
  if (edit_) { edit_->deleteLater(); edit_ = nullptr; }
  auto* ed = new QLineEdit(text, this); edit_ = ed;
  ed->setStyleSheet("QLineEdit{background:#111;color:#fff;border:1px solid #4f7fd0;padding:0 3px}");
  ed->setGeometry(R->name.r.adjusted(0, 1, 0, -1)); ed->show(); ed->setFocus(); ed->selectAll();
  auto* f = new EscFilter(ed); ed->installEventFilter(f);
  auto fin = std::make_shared<bool>(false);
  auto finish = [this, ed, fin, done](bool keep) {
    if (*fin) return; *fin = true;
    QString t = keep ? ed->text() : QString();
    if (edit_ == ed) edit_ = nullptr;
    ed->hide(); ed->deleteLater();
    if (keep) done(t);
  };
  f->esc = [finish] { finish(false); };
  connect(ed, &QLineEdit::editingFinished, this, [finish] { finish(true); });
}

void OutlinerView::flash(int id) {
  flashId_ = id; update();
  QTimer::singleShot(700, this, [this, id] { if (flashId_ == id) { flashId_ = -1; update(); } });
}

// ======================= the outliner =======================
Outliner::Outliner(Viewport* vp, Scene* sc, QWidget* parent) : QWidget(parent), vp_(vp), sc_(sc) {
  setObjectName("left"); setAttribute(Qt::WA_StyledBackground);
  setStyleSheet("#left{background:#262626;border-right:1px solid #3a3a3a} #ccol{border-left:1px solid #3a3a3a}"
                "#ccHead{padding:3px 6px;font-size:11px;color:#99aabb;border-bottom:1px solid #3a3a3a}"
                "QScrollArea{background:#262626;border:none}");
  QSettings st; olw_ = st.value("outliner/olw", 235).toInt(); ccw_ = st.value("outliner/ccw", 340).toInt();
  auto* h = new QHBoxLayout(this); h->setContentsMargins(0, 0, 1, 0); h->setSpacing(0);
  auto scroll = [](OutlinerView* v) { auto* s = new QScrollArea; s->setWidget(v); s->setWidgetResizable(true); s->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); s->setFrameShape(QFrame::NoFrame); return s; };
  sceneV_ = new OutlinerView; sceneScroll_ = scroll(sceneV_);
  sceneScroll_->setLayoutDirection(Qt::RightToLeft); sceneV_->setLayoutDirection(Qt::LeftToRight);   // (its scroll bar on the left)
  curvesV_ = new OutlinerView; curvesScroll_ = scroll(curvesV_);
  ccol_ = new QWidget; ccol_->setObjectName("ccol"); ccol_->setAttribute(Qt::WA_StyledBackground);
  auto* cv = new QVBoxLayout(ccol_); cv->setContentsMargins(1, 0, 0, 0); cv->setSpacing(0);
  auto* head = new QLabel(tr("〰 Curves")); head->setObjectName("ccHead");
  head->setToolTip(tr("Curves: the groups of curves (locks) — only here; meshes, guides, cocoons and mesh modifiers live in the outliner on the left. Open a group to see its scheme: stations where curves fork and merge, tracks — the curves"));
  cv->addWidget(head); cv->addWidget(curvesScroll_, 1);
  h->addWidget(sceneScroll_); h->addWidget(ccol_);
  // the borders: between the two (the outliner's width), the right edge (the Curves column's)
  auto* g1 = new Grip(this); auto* g2 = new Grip(this);
  g1->setToolTip(tr("Drag — the border between the outliner and the Curves column (double-click — default)"));
  g2->setToolTip(tr("Drag — the width of the Curves column; double-click — default"));
  g1->moved = [this](int dx) { setWidths(olw_ + dx, ccw_, false); };
  g2->moved = [this](int dx) { setWidths(olw_, ccw_ + dx, false); };
  g1->done = g2->done = [this] { setWidths(olw_, ccw_, true); };
  g1->reset = [this] { setWidths(235, ccw_, true); };
  g2->reset = [this] { setWidths(olw_, 340, true); };
  g1->setObjectName("g1"); g2->setObjectName("g2");
  setWidths(olw_, ccw_, false);

  for (OutlinerView* v : {sceneV_, curvesV_}) {
    v->selected = [this](int id) { return vp_->isItemSelected(id); };
    v->pie = [this](const QPoint& g) { if (pieRequested) pieRequested(g); };
    v->emptyClick = [this](Qt::KeyboardModifiers m) { const Selection& s = vp_->selection(); if (!(m & (Qt::ShiftModifier | Qt::ControlModifier)) && !s.empty()) vp_->setSelectedItems({}); };
    v->boxSelect = [this](const std::set<int>& hit, bool add) {
      std::vector<int> out; if (add) out = vp_->selection().items;
      for (int id : hit) if (sc_->item(id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
      vp_->setSelectedItems({}); vp_->setSelectedItems(out);
    };
    v->eyeStart = [this](int id) {
      Item* it = sc_->item(id); if (!it) return;
      eyeState_ = !it->visible; eyeN_ = 1; it->visible = eyeState_;
      std::vector<int> d; descendants(*sc_, *it, d);
      for (int x : d) if (Item* c = sc_->item(x); c && c->visible != eyeState_) { c->visible = eyeState_; eyeN_++; }
      sc_->revision++; vp_->sceneChanged(false); rebuild();
    };
    v->eyeOver = [this](int id) {
      Item* it = sc_->item(id); if (!it || it->visible == eyeState_) return;
      it->visible = eyeState_; eyeN_++; sc_->revision++; vp_->sceneChanged(false); rebuild();
    };
    v->eyeEnd = [this] { if (eyeN_) edit([](Scene&) {}); eyeN_ = 0; };
    v->dropped = [this](int kind, const std::vector<int>& ids, int target, const QString& zone) {
      if (kind == 2) reparentCocoon(ids[0], zone == "into" ? target : -1, false);
      else moveItems(ids, target, zone == "into" || zone == "before" || zone == "after" ? zone : "root");
    };
    v->dragIds = [this](int id) { auto t = topSelected(); return t.empty() ? std::vector<int>{id} : t; };
    v->linkMade = [this](int from, int to) { linkCocoon(from, to); };
    v->linkClicked = [this](int from, int to, Qt::KeyboardModifiers m) { if (m & Qt::AltModifier) unlinkCocoon(from, to); else ui::toast(tr("Alt + click on the line — break the link")); };
    v->linkTip = [this](int from, int to) { const Item *k = sc_->item(from), *t = sc_->item(to); return tr("🔗 %1 takes the loops of %2 where it crosses it\nAlt + click — break the link").arg(k ? k->name : QString(), t ? t->name : QString()); };
    v->resized = [this] { rebuild(); };
  }
}

void Outliner::setWidths(int olw, int ccw, bool save) {
  olw_ = std::clamp(olw, 120, 700); ccw_ = std::clamp(ccw, 160, 900);
  sceneScroll_->setFixedWidth(olw_); ccol_->setFixedWidth(ccw_ + 1);
  const bool one = sc_->room == "topo";
  setFixedWidth(one ? olw_ + 1 : olw_ + ccw_ + 2);
  if (save) { QSettings st; st.setValue("outliner/olw", olw_); st.setValue("outliner/ccw", ccw_); }
  if (auto* g = findChild<QWidget*>("g1")) { g->setGeometry(olw_ - 3, 0, 6, height()); g->raise(); }
  if (auto* g = findChild<QWidget*>("g2")) { g->setGeometry(olw_ + ccw_ + 2 - 6, 0, 6, height()); g->raise(); g->setVisible(!one); }
  emit widthChanged(one ? olw_ + 1 : olw_ + ccw_ + 2);
}

void Outliner::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); setWidths(olw_, ccw_, false); }

bool Outliner::pointerOver() const { return rect().contains(mapFromGlobal(QCursor::pos())); }

void Outliner::edit(const std::function<void(Scene&)>& fn, bool topology) {
  vp_->runEdit([fn](Scene& s, Selection&, const CurveCache&) { fn(s); s.revision++; return QString(); }, topology);
  vp_->sceneChanged(false);
}

QString Outliner::sideOf(const Item* it) const {
  if (!it) return "scene";
  if (it->type == ItemType::XMod) return it->raw.value("mesh").isNull() || !it->raw.contains("mesh") ? "curves" : "scene";
  if (it->type == ItemType::Group) {
    if (sceneGroup(it)) return "scene";
    auto has = [&](const QString& t) {
      for (int id : it->children) { const Item* c = sc_->item(id); if (!c) continue;
        if (c->type == ItemType::Group ? sideOf(c) == t : c->ring < 0 && c->type != ItemType::Cocoon && c->type != ItemType::Crown && sideOf(c) == t) return true; }
      return false;
    };
    if (!has("curves") && !has("scene") && sc_->room == "topo") return "scene";   // (an empty group in the Topology room: a group for meshes)
    return has("curves") || !has("scene") ? "curves" : "scene";
  }
  return sceneType(it->type) ? "scene" : "curves";
}

bool Outliner::shows(const Item& it, Side side) {
  if (it.type != ItemType::Group) return sideOf(&it) == (side == Side::Scene ? "scene" : "curves");
  std::function<std::pair<bool, bool>(const Item&)> kinds = [&](const Item& g) -> std::pair<bool, bool> {
    auto f = kinds_.find(g.id); if (f != kinds_.end()) return f->second;
    std::pair<bool, bool> k{false, false};
    for (int id : g.children) { const Item* c = sc_->item(id); if (!c || c->ring >= 0 || c->type == ItemType::Cocoon || c->type == ItemType::Crown) continue;
      if (c->type == ItemType::Group) { auto s = kinds(*c); k.first |= s.first; k.second |= s.second; }
      else if (sideOf(c) == "scene") k.first = true; else k.second = true; }
    kinds_[g.id] = k; return k;
  };
  auto k = kinds(it);
  if (!k.first && !k.second && !sceneGroup(&it) && sc_->room == "topo") return side == Side::Scene;   // (empty in the Topology room: a group for meshes)
  return side == Side::Scene ? (sceneGroup(&it) || k.first) : (!sceneGroup(&it) && (k.second || !k.first));
}

// ---- rows ----
void Outliner::itemRow(OutlinerView::Row& r, const Item& it, int depth) {
  using Cell = OutlinerView::Cell;
  const int id = it.id; const bool sel = vp_->isItemSelected(id);
  r.id = id;
  r.flags = (sel ? OutlinerView::Sel : hl_.count(id) ? OutlinerView::ChildSel : rel_.count(id) ? OutlinerView::Rel : 0) | (sc_->isVisible(it) ? 0 : OutlinerView::Hid);
  r.pad = 4 + depth * 14;
  const bool group = it.type == ItemType::Group, tree = group || ((it.type == ItemType::Cocoon || it.type == ItemType::IMesh) && !it.children.empty());   // (an object: what is put under it — a symmetry group, its capsule)
  Cell ar; ar.w = 12; ar.text = tree ? (it.expanded ? "▾" : "▸") : ""; ar.color = MUT; ar.op = 1;
  if (tree) ar.click = [this, id](Qt::KeyboardModifiers m) {
    Item* x = sc_->item(id); if (!x) return; bool v = !x->expanded;
    std::function<void(Item&)> deep = [&](Item& g) { if (g.type == ItemType::Group || g.type == ItemType::Cocoon) g.expanded = v; for (int c : g.children) if (Item* ci = sc_->item(c); ci && (ci->type == ItemType::Group || ci->type == ItemType::Cocoon)) deep(*ci); };
    if (m & Qt::ShiftModifier) deep(*x); else x->expanded = v;
    rebuild();
  };
  Cell ico; ico.op = 0.85;
  ico.text = it.type == ItemType::Guide && it.raw.value("kind").toString() == "disc" ? "◎" : it.type == ItemType::XMod ? xkindIcon(it.raw.value("kind").toString()) : itemTypeIcon(it.type);
  if (it.type == ItemType::Mesh && rawB(it, "pending")) ico.text = "◇";
  if ((it.type == ItemType::Group && rawB(it, "braidGroup")) || it.type == ItemType::Braid) { static QPixmap bp = ui::image("tool_braid").scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation); ico.text.clear(); ico.pix = bp; ico.op = 1; }   // (a braid and its folder: the braid icon)
  r.name.text = it.name;
  r.name.tip = it.name + "\n" + (it.junc ? tr("Service curve — shown only while selected") : group ? tr("Double-click — select everything inside · F2 — rename") : tr("Double-click or F2 — rename"));
  r.name.dbl = [this, id, group](Qt::KeyboardModifiers) {
    if (!group) { startRename(id); return; }
    std::vector<int> out; std::function<void(const Item&)> walk = [&](const Item& g) { for (int c : g.children) if (const Item* x = sc_->item(c); x && x->ring < 0) { if (x->type == ItemType::Group) walk(*x); else out.push_back(c); } };
    const Item* g = sc_->item(id); if (!g) return; walk(*g);
    vp_->setSelectedItems({}); vp_->setSelectedItems(out);
    ui::toast(tr("Selected %1 item%2 inside %3").arg(out.size()).arg(out.size() == 1 ? "" : "s").arg(g->name));
  };
  r.left.push_back(ar); if (!it.junc) r.left.push_back(ico);

  auto selectOnly = [this, id](Qt::KeyboardModifiers) { vp_->setSelectedItems({}); vp_->setSelectedItems({id}); };
  if (group) {
    const QJsonObject labels = it.raw.value("labels").toObject();
    bool looseOn = !(labels.contains("loose") && labels.value("loose") == QJsonValue(false)), hasLoose = false;
    for (int c : it.children) if (vp_->display().looseCurves.count(c)) hasLoose = true;
    if (looseOn && hasLoose) { Cell t; t.pix = tagPix(80, 1.8); t.op = 1; t.tip = tr("Free curves visible: curves joined to no NURBS surface and no modifier stay shown in the viewport even while the group isn't selected (on by default)") + tr(" — click: group properties"); t.click = selectOnly; r.right.push_back(t); }
    if (!svcGroup(it)) {
      QString mode = it.raw.value("guides").toString("auto"); if (mode != "show" && mode != "hide") mode = "auto";
      Cell gm; gm.text = mode == "show" ? "∿" : mode == "hide" ? "⌀" : "A";
      gm.tip = (mode == "show" ? tr("Curves & anchors: always shown") : mode == "hide" ? tr("Curves & anchors: hidden") : tr("Curves & anchors: Auto (shown only while working inside this group)")) + tr(" — click to switch");
      gm.click = [this, id](Qt::KeyboardModifiers m) {
        edit([&](Scene& s) {
          Item* g = s.item(id); if (!g) return;
          QString cur = g->raw.value("guides").toString("auto"); QStringList order{"auto", "show", "hide"};
          QString nx = order[(std::max(0, int(order.indexOf(cur))) + 1) % 3]; g->raw.insert("guides", nx);
          if (m & Qt::ShiftModifier) { std::vector<int> d; descendants(s, *g, d); for (int x : d) if (Item* c = s.item(x); c && c->type == ItemType::Group && !svcGroup(*c)) c->raw.insert("guides", nx); }
        });
      };
      if (sc_->room != "topo") r.right.push_back(gm);   // (the Topology room: groups of meshes — no curves to show / hide)
      Cell sw; sw.swatchCell = true; sw.op = 1; sw.swatchOn = !it.color.isEmpty(); if (sw.swatchOn) sw.swatch = QColor(it.color);
      sw.tip = tr("Group colour (double-click or RMB — reset). A parent group's colour covers its sub-groups");
      auto reset = [this, id] { const Item* g = sc_->item(id); if (g && !g->color.isEmpty()) edit([id](Scene& s) { s.item(id)->color.clear(); }); };
      sw.click = [this, id](Qt::KeyboardModifiers) {
        swatchPending_ = id;
        QTimer::singleShot(250, this, [this, id] {
          if (swatchPending_ != id) return; swatchPending_ = -1;
          const Item* g = sc_->item(id); if (!g) return;
          QColor c = QColorDialog::getColor(QColor(g->color.isEmpty() ? "#4a7fd6" : g->color), this, tr("Group colour"));
          if (c.isValid()) edit([id, c](Scene& s) { if (Item* x = s.item(id)) x->color = c.name(); });
        });
      };
      sw.dbl = [this, reset](Qt::KeyboardModifiers) { swatchPending_ = -1; reset(); };
      sw.rclick = [reset](Qt::KeyboardModifiers m) { if (!(m & Qt::ShiftModifier)) reset(); };
      r.right.push_back(sw);
      // the group's symmetry (Hair room): a live mirrored copy of everything inside
      QJsonObject sym = it.raw.value("sym").toObject(); bool so = sym.value("on").toBool();
      Cell sy; static QPixmap on = ui::image("topo_sym").scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation), off = grey(on);
      sy.pix = so ? on : off; sy.op = so ? 1 : 0.3;
      sy.tip = (so ? tr("Symmetry ON (%1) — a live mirrored copy of everything inside").arg(sym.value("axis").toString("x").toUpper()) : tr("Symmetry OFF")) + tr(" — click to switch; the axis and Apply — in the group's settings");
      sy.click = [this, id](Qt::KeyboardModifiers) { edit([id](Scene& s) { Item* g = s.item(id); QJsonObject o = g->raw.value("sym").toObject(); if (!o.contains("axis")) o.insert("axis", "x"); o.insert("on", !o.value("on").toBool()); g->raw.insert("sym", o); }, true); };
      if (sc_->room != "topo") r.right.push_back(sy);   // (the Hair room's)
    }
  }
  if (it.type == ItemType::Surface) {
    Cell pb; pb.text = "⚙"; pb.tip = tr("Properties: driver curves");
    pb.click = [this, selectOnly](Qt::KeyboardModifiers m) { selectOnly(m); emit propsFlash(); };
    r.right.push_back(pb);
  }
  // ⏻ on a guide and on a group holding guides: the guides' effect on the loops (hiding them doesn't switch it)
  if (it.type == ItemType::Guide || (group && groupHasGuides(*sc_, it))) {
    bool isG = it.type == ItemType::Guide, on = isG ? !rawB(it, "off") : !rawB(it, "guidesOff");
    Cell pw; pw.text = "⏻"; bool lookOff = !on || (isG && !guideOn(*sc_, it));
    pw.color = lookOff ? GOFF : GON; pw.op = lookOff ? 0.55 * 0.7 : 0.7;
    pw.tip = (isG ? (on ? tr("The guide is ON — it shapes the loops") : tr("The guide is OFF — no effect on the loops (it stays visible / editable)"))
                  : (on ? tr("Guides in this group are ON") : tr("Guides in this group are OFF — none of them shapes the loops"))) + tr(" — click to switch. Hiding (eye) doesn't switch a guide off");
    if (isG && on && !guideOn(*sc_, it)) pw.tip = tr("This guide is on, but a group above it switches its guides off");
    pw.click = [this, id, isG](Qt::KeyboardModifiers m) {
      edit([&](Scene& s) {
        Item* x = s.item(id); if (!x) return;
        if (isG) x->raw.insert("off", !rawB(*x, "off")); else x->raw.insert("guidesOff", !rawB(*x, "guidesOff"));
        if ((m & Qt::ShiftModifier) && !isG) { bool v = rawB(*x, "guidesOff"); std::vector<int> d; descendants(s, *x, d);
          for (int c : d) if (Item* ci = s.item(c)) { if (ci->type == ItemType::Guide) ci->raw.insert("off", v); else if (ci->type == ItemType::Group && groupHasGuides(s, *ci)) ci->raw.insert("guidesOff", v); } }
      }, true);
    };
    r.right.push_back(pw);
  }
  // ⛨ on an imported mesh (and on the Imported group — all its meshes): drawn curves stick to it like to the head
  if (it.type == ItemType::IMesh || (group && rawB(it, "importGroup"))) {
    std::vector<int> ms; if (it.type == ItemType::IMesh) ms = {id}; else for (const Item* m : ofType(*sc_, ItemType::IMesh)) if (sc_->isDescendant(m->id, id)) ms.push_back(m->id);
    bool on = !ms.empty(); for (int m : ms) if (!rawB(*sc_->item(m), "collide")) on = false;
    static const QPixmap collOn = ui::image("collision").scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation), collOff = grey(collOn);
    Cell cb; cb.pix = on ? collOn : collOff; cb.op = on ? 1 : 0.45;
    cb.tip = (on ? tr("Collision ON — drawn curves stick to ") : tr("Collision OFF — click: drawn curves stick to ")) + (it.type == ItemType::IMesh ? tr("this mesh") : tr("every mesh in this group")) + tr(" like to the head and don't pass through it (also while hidden)");
    cb.click = [this, ms, on](Qt::KeyboardModifiers) {
      if (ms.empty()) return;
      edit([&](Scene& s) { for (int m : ms) s.item(m)->raw.insert("collide", !on); });
      ui::toast(tr("Collision %1: %2").arg(on ? tr("off") : tr("on")).arg(ms.size() == 1 ? sc_->item(ms[0])->name : tr("%1 meshes").arg(ms.size())));
    };
    r.right.push_back(cb);
  }
  if (it.type == ItemType::IMesh && looks::hasUV(it)) lookCells(r.right, {id}, id);
  if (it.type == ItemType::IMesh && sc_->room == "topo" && rawB(it, "tref")) {   // (a reference: picked only here, in the outliner)
    Cell rf; rf.text = "REF"; rf.tagBox = true; rf.w = -1; rf.tagFg = QColor(0x9f, 0xc4, 0xff); rf.tagBorder = QColor(0x4a, 0x6a, 0x9a); rf.tagBg = QColor(0x22, 0x2c, 0x3a); rf.op = 1;
    rf.tip = tr("A reference: it can't be picked in the viewport (only here, in the outliner); its settings — Use as reference"); r.right.push_back(rf); }
  if (!it.junc) {
    Cell eye; eye.eye = true; eye.pix = ui::eyePix(it.visible); eye.tip = tr("Hide / show (H). Shift + drag over the eyes — the same for every row passed");
    eye.click = [this, id](Qt::KeyboardModifiers) { bool cocoon = sc_->item(id) && sc_->item(id)->type == ItemType::Cocoon; edit([id](Scene& s) { if (Item* x = s.item(id)) x->visible = !x->visible; }, cocoon); };
    r.right.push_back(eye);
  }
  r.click = [this, id](Qt::KeyboardModifiers m) { vp_->selectItem(id, m & (Qt::ShiftModifier | Qt::ControlModifier)); };
  r.drag = it.type == ItemType::Head ? 0 : 1; r.drop = it.type == ItemType::Head ? 0 : 1; r.group = group || it.type == ItemType::IMesh;   // (an object: a symmetry group goes into it)
}

// ▦ textures on / off (double-click — the texture window), ◐ shaded / ○ flat (unlit): of a mesh with UVs, of the head's
void Outliner::lookCells(std::vector<OutlinerView::Cell>& right, const std::vector<int>& ms, int rowKey) {
  QJsonObject L = looks::lookOf(*sc_->item(ms[0])); bool tex = L.value("tex").toBool(), flat = L.value("flat").toBool();
  int n = int(looks::setsOf(*sc_->item(ms[0])).size());
  OutlinerView::Cell tx; tx.text = "▦"; tx.color = tex ? GON : GOFF; tx.op = tex ? 0.7 : 0.55 * 0.7;
  tx.tip = (tex ? tr("Textures ON") : tr("Textures OFF")) + (rowKey == -7 ? QString() : tr(" (%1 texture set%2)").arg(n).arg(n == 1 ? "" : "s")) + tr(" — click to switch; double-click or RMB — the texture sets: maps, on / off");
  tx.click = [this, ms, rowKey, tex](Qt::KeyboardModifiers) {
    if (texPending_ == rowKey) return; texPending_ = rowKey;
    QTimer::singleShot(250, this, [this, ms, rowKey, tex] { if (texPending_ != rowKey) return; texPending_ = -1; lookChanged(vp_, ms, [tex](QJsonObject& L) { L.insert("tex", !tex); }); });
  };
  tx.dbl = [this, ms](Qt::KeyboardModifiers) { texPending_ = -1; openTexWindow(this, vp_, sc_, ms[0]); };
  tx.rclick = tx.dbl;                                                 // (RMB: the texture sets at once)
  right.push_back(tx);
  OutlinerView::Cell fl; fl.text = flat ? "○" : "◐"; fl.color = GON; fl.op = 0.7;
  fl.tip = (flat ? tr("Flat: unlit, no shading — colours / textures as they are") : tr("Shaded: lit like a model")) + tr(" — click to switch");
  fl.click = [this, ms, flat](Qt::KeyboardModifiers) { lookChanged(vp_, ms, [flat](QJsonObject& L) { L.insert("flat", !flat); }); };
  right.push_back(fl);
}

// the Head of the Hair room: the meshes marked ☺ as one row
void Outliner::headRow(OutlinerView* v, int W) {
  std::vector<int> ms; for (const Item* m : ofType(*sc_, ItemType::IMesh)) if (m->headTag) ms.push_back(m->id);
  if (ms.empty()) return;
  bool sel = std::all_of(ms.begin(), ms.end(), [this](int id) { return vp_->isItemSelected(id); });
  // (HEADV: shown while any of its meshes is; the eye shows / hides them all)
  bool anyOn = std::any_of(ms.begin(), ms.end(), [this](int id) { return sc_->item(id)->visible; });
  auto& r = v->add(0, W); r.id = -7;
  r.flags = (sel ? OutlinerView::Sel : 0) | (!anyOn || (sc_->iso && !sc_->iso->head) ? OutlinerView::Hid : 0);
  QStringList names; for (int id : ms) names << sc_->item(id)->name;
  r.name.text = ms.size() > 1 ? tr("Head (%1 meshes)").arg(ms.size()) : tr("Head");
  r.name.tip = tr("The head: %1 — the meshes marked ☺ Head (Meshes › Imported in the Topology room; the mark — in a mesh's properties)").arg(names.join(", "));
  OutlinerView::Cell ar; ar.w = 12; OutlinerView::Cell ico; ico.text = "☺"; ico.op = 0.85;
  r.left = {ar, ico};
  if (auto T = headLookTargets(*sc_); !T.empty()) lookCells(r.right, T, -7);
  OutlinerView::Cell eye; eye.pix = ui::eyePix(anyOn); eye.tip = tr("Hide / show the head (all its meshes)");
  eye.click = [this, ms, anyOn](Qt::KeyboardModifiers) { edit([&](Scene& s) { for (int id : ms) s.item(id)->visible = !anyOn; }); };
  r.right.push_back(eye);
  r.click = [this, ms](Qt::KeyboardModifiers) { vp_->setSelectedItems({}); vp_->setSelectedItems(ms); };
  v->next(27);
}

void Outliner::walk(const std::vector<int>& ids, int depth, bool inGroup, OutlinerView* v, Side side, int W) {
  std::vector<const Item*> cs, js, ss;
  if (inGroup && side == Side::Curves) for (int id : ids) if (const Item* it = sc_->item(id); it && it->ring < 0) {
    if (it->type == ItemType::Curve) (it->junc ? js : cs).push_back(it); else if (it->type == ItemType::Surface) ss.push_back(it);
  }
  bool two = (!cs.empty() || !js.empty()) && !ss.empty(), shown = false;
  std::vector<std::vector<const Item*>*> cols = js.empty() ? std::vector<std::vector<const Item*>*>{&cs, &ss} : std::vector<std::vector<const Item*>*>{&cs, &js, &ss};
  for (int id : ids) {
    const Item* it = sc_->item(id); if (!it) continue;
    if (it->ring >= 0 || it->type == ItemType::Cocoon || it->type == ItemType::Crown) continue;   // (the cocoons, the crowns: their own sections)
    const bool one = sc_->room == "topo";                       // (the Topology room: one tree — everything on the left)
    if (it->type == ItemType::IMesh && it->headTag && !one) continue;                                     // (the head's meshes: the Head row)
    if (it->type == ItemType::XMod && it->raw.contains("mesh") && !it->raw.value("mesh").isNull()) { const Item* m = sc_->item(it->raw.value("mesh").toInt()); if (m && m->headTag && !one) continue;
      if (one && it->raw.value("kind").toString() == "xform") continue; }   // (Topology: a mesh's Transform lives in its settings)
    if (!one && !shows(*it, side)) continue;
    if (two && (it->type == ItemType::Curve || it->type == ItemType::Surface)) {
      if (shown) continue; shown = true;
      size_t n = 0; for (auto* c : cols) n = std::max(n, c->size());
      int x = 4 + depth * 14 + 2, wc = 84, we = 34;
      for (size_t i = 0; i < n; i++) {
        int x0 = x;
        for (size_t k = 0; k < cols.size(); k++) {
          int w = k == 0 ? wc : (cols.size() == 3 && k == 1) ? we : W - x0;
          if (k == 0) w = std::min(wc, std::max(0, W - x0));
          int x1 = k + 1 == cols.size() ? W : x0 + w;
          auto& r = v->add(x0, x1);
          if (i < cols[k]->size()) { itemRow(r, *(*cols[k])[i], 0); r.pad = 2; r.gap = 2; r.padR = 3; if (!r.left.empty()) r.left.erase(r.left.begin()); }
          else r.empty = true;
          x0 = x1;
        }
        v->next(27);
      }
      continue;
    }
    auto& r = v->add(0, W); itemRow(r, *it, depth); v->next(27);
    if ((it->type == ItemType::Group || it->type == ItemType::IMesh) && it->expanded && !it->children.empty()) {
      v->next(1); int b = v->boxOpen(depth * 14 + 6); v->next(2);
      // a curve group open in the Curves column: its scheme (forks, merges, walls, service curves) above its sub-groups
      bool scheme = false;
      if (side == Side::Curves && SchemeView::hasScheme(*sc_, *it)) {
        if (it->raw.value("view").toString() == "list") {
          auto* box = new QWidget; auto* h = new QHBoxLayout(box); h->setContentsMargins(10, 2, 6, 2);
          auto* bt = new QPushButton(tr("⎇ Scheme")); bt->setToolTip(tr("Show this group as a scheme")); bt->setStyleSheet("QPushButton{padding:1px 8px;font-size:11px}"); bt->setFocusPolicy(Qt::NoFocus);
          int gid = it->id; connect(bt, &QPushButton::clicked, this, [this, gid] { if (Item* g = sc_->item(gid)) g->raw.insert("view", QJsonValue()); QTimer::singleShot(0, this, [this] { rebuild(); }); });
          h->addWidget(bt); h->addStretch(1); box->setFixedHeight(26);
          v->addWidget(box, depth * 14 + 6, W - 3);
        } else {
          auto* sv = new SchemeView(vp_, sc_, it->id, W);
          sv->rename = [this](int id) { startRename(id); };
          sv->changed = [this] { rebuild(); };
          v->addWidget(sv, 0, W); scheme = true;
        }
      }
      if (scheme) { std::vector<int> gs; for (int c : it->children) if (const Item* x = sc_->item(c); x && x->type == ItemType::Group) gs.push_back(c); walk(gs, depth + 1, true, v, side, W); }
      else walk(it->children, depth + 1, true, v, side, W);
      v->next(2); v->boxClose(b); v->next(4);
    }
  }
}

void Outliner::sections(OutlinerView* v, int W) {
  using Cell = OutlinerView::Cell;
  // the crowns: their own folder, above the cocoons' section
  auto cr = ofType(*sc_, ItemType::Crown);
  v->next(6);
  {
    auto& r = v->add(0, W); r.flags = OutlinerView::Section;
    Cell ar; ar.w = 12; ar.color = MUT; ar.op = 1; ar.text = cr.empty() ? "" : crownsFolded_ ? "▸" : "▾";
    Cell ic; static QPixmap crown = ui::image("tool_crown").scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation); ic.pix = crown; ic.op = 1;
    r.left = {ar, ic}; r.name.text = tr("Crowns (%1)").arg(cr.size());
    r.tip = tr("The crowns (Crown tool, S) — click: fold / unfold · double-click: select them all");
    r.click = [this](Qt::KeyboardModifiers) { crownsFolded_ = !crownsFolded_; rebuild(); };
    std::vector<int> ids; for (auto* c : cr) ids.push_back(c->id);
    r.dbl = [this, ids](Qt::KeyboardModifiers) { if (ids.empty()) return; vp_->setSelectedItems({}); vp_->setSelectedItems(ids); };
    if (!cr.empty()) {
      bool allOn = std::all_of(cr.begin(), cr.end(), [](const Item* c) { return c->visible; });
      Cell ey; ey.pix = ui::eyePix(allOn); ey.tip = tr("Hide / show all the crowns");
      ey.click = [this, ids, allOn](Qt::KeyboardModifiers) { edit([&](Scene& s) { for (int id : ids) s.item(id)->visible = !allOn; }); };
      r.right.push_back(ey);
    }
    v->next(27);
  }
  if (!crownsFolded_) for (auto* c : cr) { auto& r = v->add(0, W); itemRow(r, *c, 1); v->next(27); }
  // the braids: their own section too (each in its folder with its curve and connectors)
  auto br = ofType(*sc_, ItemType::Braid);
  if (!br.empty()) {
    v->next(6);
    auto& r = v->add(0, W); r.flags = OutlinerView::Section;
    Cell ar; ar.w = 12; ar.color = MUT; ar.op = 1; ar.text = braidsFolded_ ? "▸" : "▾";
    Cell ic; static QPixmap braidPix = ui::image("tool_braid").scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation); ic.pix = braidPix; ic.op = 1;
    r.left = {ar, ic}; r.name.text = tr("Braids (%1)").arg(br.size());
    r.tip = tr("The braids (Braid tool, B) — click: fold / unfold · double-click: select them all");
    r.click = [this](Qt::KeyboardModifiers) { braidsFolded_ = !braidsFolded_; rebuild(); };
    std::vector<int> ids; for (auto* c : br) ids.push_back(c->id);
    r.dbl = [this, ids](Qt::KeyboardModifiers) { vp_->setSelectedItems({}); vp_->setSelectedItems(ids); };
    bool allOn = std::all_of(br.begin(), br.end(), [](const Item* c) { return c->visible; });
    Cell ey; ey.pix = ui::eyePix(allOn); ey.tip = tr("Hide / show all the braids");
    ey.click = [this, ids, allOn](Qt::KeyboardModifiers) { edit([&](Scene& s) { for (int id : ids) s.item(id)->visible = !allOn; }); };
    r.right.push_back(ey);
    v->next(27);
    if (!braidsFolded_) for (auto* c : br) { auto& rr = v->add(0, W); itemRow(rr, *c, 1); v->next(27); }
  }
  // the cocoons' section
  v->next(6);
  {
    auto ks = ofType(*sc_, ItemType::Cocoon);
    auto& r = v->add(0, W); r.flags = OutlinerView::Section;
    Cell ar; ar.w = 12; ar.color = MUT; ar.op = 1; ar.text = cocoonsFolded_ ? "▸" : "▾";
    Cell ic; ic.text = "◍"; ic.op = 0.85;
    r.left = {ar, ic}; r.name.text = tr("Cocoons (%1)").arg(ks.size());
    r.click = [this](Qt::KeyboardModifiers) { toggleCocoons(); };
    bool off = sc_->fileRaw.value("shellModsOff").toBool();
    Cell sp; sp.text = "⏻"; sp.color = off ? GOFF : GON; sp.op = off ? 0.55 * 0.7 : 0.7;
    sp.tip = off ? tr("All the shell modifiers are OFF — the rings as with none (click — back on, each as it was)") : tr("Switch off all the shell modifiers at once (Density, Parent influence, guide lines, discs) — to see the rings without them");
    sp.click = [this, off](Qt::KeyboardModifiers) {
      edit([off](Scene& s) { if (off) s.fileRaw.remove("shellModsOff"); else s.fileRaw.insert("shellModsOff", true); }, true);
      ui::toast(off ? tr("Shell modifiers back on") : tr("All shell modifiers off"));
    };
    r.right.push_back(sp);
    v->next(27);
  }
  if (!cocoonsFolded_) cocoonRows(v, W);
}

int Outliner::paneWidth() const { return sc_->room == "topo" ? olw_ + 1 : olw_ + ccw_ + 2; }

void Outliner::toggleCocoons() { cocoonsFolded_ = !cocoonsFolded_; rebuild(); }

void Outliner::cocoonRows(OutlinerView* v, int W) {
  using Cell = OutlinerView::Cell;
  auto all = ofType(*sc_, ItemType::Cocoon);
  std::vector<const Item*> roots; for (auto* k : all) if (!cocoonParent(*sc_, *k)) roots.push_back(k);
  if (roots.empty()) { auto& r = v->add(0, W); r.flags = OutlinerView::Hint; r.name.text = tr("No cocoons yet"); r.pad = 4; v->next(27); return; }
  CocoonSystem* cs = vp_->hair().cocoonSystem();
  // 🔗 the links (a cocoon → the ones it takes loops from): drawn in a gutter on the left
  std::vector<std::pair<int, int>> links;
  for (auto* k : all) for (const QJsonValue& x : k->raw.value("links").toArray()) { const Item* t = sc_->item(x.toInt()); if (t && t->type == ItemType::Cocoon && t != k) links.push_back({k->id, t->id}); }
  const int X0 = links.empty() ? 0 : 14;
  v->setLinks(links);
  // a cocoon whose lock is selected: its group, or anything in that group (selected anchors — their curves)
  std::vector<int> selIds; for (int id : vp_->selection().items) if (const Item* x = sc_->item(id); x && x->type != ItemType::Cocoon) selIds.push_back(id);
  { std::set<int> as(vp_->selection().anchors.begin(), vp_->selection().anchors.end());
    if (!as.empty()) for (const Item* c : sc_->curves()) for (int a : c->anchors) if (as.count(a)) { selIds.push_back(c->id); break; } }
  auto ofLock = [&](const Item& k) { if (selIds.empty() || !cs) return false; int g = cs->lockGroup(k.id); if (g < 0) return false;
    for (int id : selIds) if (id == g || sc_->isDescendant(id, g)) return true; return false; };
  // (the lit cocoon hidden in a folded branch: the outermost folded one up its chain is pale orange)
  std::set<int> holdsLit;
  for (auto* k : all) if (ofLock(*k)) { std::vector<const Item*> up; for (const Item* p = cocoonParent(*sc_, *k); p; p = cocoonParent(*sc_, *p)) up.insert(up.begin(), p);
    for (auto* p : up) if (!p->expanded) { holdsLit.insert(p->id); break; } }
  std::vector<const Item*> guides = ofType(*sc_, ItemType::Guide);
  auto linkNo = [&](const Item& g) {
    std::set<QString> ls; for (auto* x : guides) { QString l = x->raw.value("link").toVariant().toString(); if (l.isEmpty()) continue;
      for (auto* y : guides) if (y != x && y->raw.value("link").toVariant().toString() == l) { ls.insert(l); break; } }
    QString me = g.raw.value("link").toVariant().toString(); int i = 1; for (auto& l : ls) { if (l == me) return i; i++; } return 0;
  };
  std::function<void(const Item&, int)> walkK = [&](const Item& k, int depth) {
    const int id = k.id;
    auto& r = v->add(X0, W); r.id = id;
    r.flags = (vp_->isItemSelected(id) ? OutlinerView::Sel : ofLock(k) ? OutlinerView::LockSel : holdsLit.count(id) ? OutlinerView::Rel : hl_.count(id) ? OutlinerView::ChildSel : 0) | (sc_->isVisible(k) ? 0 : OutlinerView::Hid);
    r.pad = 4 + depth * 12;
    std::vector<const Item*> kids, mods;
    for (int c : k.children) if (const Item* x = sc_->item(c)) { if (x->type == ItemType::Cocoon) kids.push_back(x); else if (shellMod(x->type)) mods.push_back(x); }
    Cell ar; ar.w = 12; ar.color = MUT; ar.op = 1; ar.text = kids.empty() && mods.empty() ? "" : k.expanded ? "▾" : "▸";
    if (!ar.text.isEmpty()) ar.click = [this, id](Qt::KeyboardModifiers m) {
      Item* x = sc_->item(id); if (!x) return; bool val = !x->expanded;
      std::function<void(Item&)> deep = [&](Item& g) { g.expanded = val; for (int c : g.children) if (Item* ci = sc_->item(c); ci && (ci->type == ItemType::Group || ci->type == ItemType::Cocoon)) deep(*ci); };
      if (m & Qt::ShiftModifier) deep(*x); else x->expanded = val;
      rebuild();
    };
    Cell ic; ic.text = "◍"; ic.op = 0.85;
    r.left = {ar, ic};
    int lk = cs ? cs->lockGroup(id) : -1; const Item* lg = lk >= 0 ? sc_->item(lk) : nullptr;
    QString lkName = lg && !k.name.contains(lg->name) ? lg->name : QString();
    r.name.text = k.name; if (!lkName.isEmpty()) { r.name.prefix = lkName + " ▸ "; r.name.prefixColor = QColor(0x7a, 0x8a, 0x8c); }
    QString src = k.raw.value("src").toString();
    r.tip = k.name + (lg ? tr("\nLock: %1").arg(lg->name) : QString()) + (depth ? QString() : tr("\nTrunk") + (src.isEmpty() ? QString() : tr(" — made from %1").arg(src)))
          + tr("\nDouble-click — select it with all its branch cocoons (the trunk — all) · F2 — rename · drag onto another cocoon — make it its branch");
    r.name.tip = r.tip;
    r.dbl = [this, id](Qt::KeyboardModifiers) {
      std::vector<int> out{id}; for (const Item* c : ofType(*sc_, ItemType::Cocoon)) if (c->id != id && sc_->isDescendant(c->id, id)) out.push_back(c->id);
      vp_->setSelectedItems({}); vp_->setSelectedItems(out); ui::toast(tr("%1 cocoon%2 selected").arg(out.size()).arg(out.size() == 1 ? "" : "s"));
    };
    bool lockMode = k.raw.value("ringMode").toString() == "lock", ringsOff = rawB(k, "ringsOff");
    Cell rg; rg.text = "◎"; rg.color = ringsOff ? QColor(0x66, 0x66, 0x66) : QColor(0x7f, 0xc8, 0xd0);
    rg.tip = (ringsOff ? tr("Rings hidden — click to show them") : tr("Hide the rings%1 (the cocoon stays)").arg(lockMode ? tr(" projected onto the lock") : QString())) + tr(" · Shift + click — of all its branch cocoons too");
    rg.click = [this, id](Qt::KeyboardModifiers m) {
      edit([&](Scene& s) { Item* x = s.item(id); bool val = !rawB(*x, "ringsOff"); x->raw.insert("ringsOff", val);
        if (m & Qt::ShiftModifier) { std::vector<int> d; descendants(s, *x, d); for (int c : d) if (Item* ci = s.item(c); ci && ci->type == ItemType::Cocoon) ci->raw.insert("ringsOff", val); } }, true);
    };
    Cell eye; eye.pix = ui::eyePix(k.visible);
    eye.tip = tr("Hide / show the cocoon (H)") + (lockMode ? tr(" — its rings on the lock stay: ◎ hides them") : QString()) + tr(" · Shift + click — with all its branch cocoons and modifiers");
    eye.click = [this, id](Qt::KeyboardModifiers m) {
      edit([&](Scene& s) { Item* x = s.item(id); x->visible = !x->visible;
        if (m & Qt::ShiftModifier) { std::vector<int> d; descendants(s, *x, d); for (int c : d) if (Item* ci = s.item(c)) ci->visible = x->visible; } }, true);
    };
    r.right = {rg, eye};
    r.click = [this, id](Qt::KeyboardModifiers m) { if (m & Qt::AltModifier) return; vp_->selectItem(id, m & (Qt::ShiftModifier | Qt::ControlModifier)); };
    r.drag = 2; r.drop = 2;
    v->next(27);
    if (k.expanded) for (const Item* m : mods) {
      const int mid = m->id;
      QString link = m->raw.value("link").toVariant().toString();
      std::vector<int> peers; if (m->type == ItemType::Guide && !link.isEmpty()) for (auto* x : guides) if (x != m && x->raw.value("link").toVariant().toString() == link) peers.push_back(x->id);
      bool linkRel = !peers.empty() && !vp_->isItemSelected(mid) && std::any_of(peers.begin(), peers.end(), [this](int p) { return vp_->isItemSelected(p); });
      auto& mr = v->add(X0, W); mr.id = mid;
      mr.flags = (vp_->isItemSelected(mid) ? OutlinerView::Sel : linkRel ? OutlinerView::Rel : 0) | (!m->visible || (m->type == ItemType::Guide && rawB(*m, "off")) ? OutlinerView::Hid : 0);
      mr.pad = 4 + (depth + 1) * 12;
      Cell a0; a0.w = 12; Cell mi; mi.op = 0.85;
      mi.text = m->type == ItemType::Tip ? "▤" : m->type == ItemType::Even ? "☰" : m->type == ItemType::PInf ? "⇅" : m->raw.value("kind").toString() == "disc" ? "◎" : "┼";
      mr.left = {a0, mi};
      mr.name.text = m->name;
      mr.name.tip = (m->type == ItemType::Tip ? tr("Density — more / fewer rings where painted") : m->type == ItemType::Even ? tr("Even loops — removed, does nothing (delete it)")
                   : m->type == ItemType::PInf ? tr("Parent influence — the parent's pull weakened (red) / added (green) where painted")
                   : m->raw.value("kind").toString() == "disc" ? tr("NURBS disc — a ring lies on it") : tr("Guide line — a ring lies along it")) + tr("\nDouble-click or F2 — rename");
      mr.name.dbl = [this, mid](Qt::KeyboardModifiers) { startRename(mid); };
      if (!peers.empty()) {
        bool lon = !(m->raw.contains("linkOn") && m->raw.value("linkOn") == QJsonValue(false));
        Cell lb; lb.text = QString("🔗%1").arg(linkNo(*m)); lb.w = -1; lb.color = lon ? QColor(0xff, 0xb0, 0x70) : QColor(0x77, 0x77, 0x77); lb.op = 1;
        QStringList ps; for (int p : peers) { const Item* x = sc_->item(p); const Item* kk = parentOf(*sc_, x); ps << QString("%1 (%2)").arg(x->name, kk && kk->type == ItemType::Cocoon ? kk->name : "—"); }
        lb.tip = tr("Linked guide lines (link %1): with %2").arg(linkNo(*m)).arg(ps.join(", ")) + (lon ? tr("\nSettings linked") : tr("\nSettings NOT linked (this one set on its own)")) + tr("\nClick — select all of the link");
        std::vector<int> all{mid}; all.insert(all.end(), peers.begin(), peers.end());
        lb.click = [this, all](Qt::KeyboardModifiers) { vp_->setSelectedItems({}); vp_->setSelectedItems(all); };
        mr.right.push_back(lb);
      }
      if (m->type == ItemType::Guide) {
        bool off = rawB(*m, "off");
        Cell pw; pw.text = "⏻"; pw.color = off ? GOFF : GON; pw.op = off ? 0.55 * 0.7 : 0.7;
        pw.tip = off ? tr("The guide is OFF — no effect on the rings (click — switch it on)") : tr("The guide is ON — it shapes the rings (click — switch it off)");
        pw.click = [this, mid](Qt::KeyboardModifiers) { edit([mid](Scene& s) { Item* x = s.item(mid); x->raw.insert("off", !rawB(*x, "off")); }, true); };
        mr.right.push_back(pw);
      }
      Cell ey; ey.pix = ui::eyePix(m->visible); ey.tip = tr("Switch it off / on (hide)");
      ey.click = [this, mid](Qt::KeyboardModifiers) { edit([mid](Scene& s) { Item* x = s.item(mid); x->visible = !x->visible; }, true); };
      mr.right.push_back(ey);
      mr.click = [this, mid](Qt::KeyboardModifiers mm) { vp_->selectItem(mid, mm & (Qt::ShiftModifier | Qt::ControlModifier)); };
      v->next(27);
    }
    if (k.expanded) for (const Item* c : kids) walkK(*c, depth + 1);
  };
  for (const Item* k : roots) walkK(*k, 0);
}

// ---- renderOutliner ----
void Outliner::rebuild() {
  if (building_) return;
  building_ = true;
  vp_->setHoverCurves({});                               // (the schemes drawn anew — no leave from the old ones)
  // fixed top: the imported model, the finished hair ('Meshes'), the guides; the rest keeps its order
  auto rank = [this](int id) { const Item* it = sc_->item(id); return it && rawB(*it, "importGroup") ? -1 : it && rawB(*it, "meshesGroup") ? 0 : it && rawB(*it, "guidesGroup") ? 1 : 2; };
  std::stable_sort(sc_->roots.begin(), sc_->roots.end(), [&](int a, int b) { return rank(a) < rank(b); });
  kinds_.clear(); hl_.clear(); rel_.clear();
  // groups holding something selected; related rows (the walls of a selected curve, the two curves of a selected wall)
  auto up = [this](const Item* it) { for (const Item* p = parentOf(*sc_, it); p; p = parentOf(*sc_, p)) hl_.insert(p->id); };
  const Selection& S = vp_->selection();
  for (int id : S.items) up(sc_->item(id));
  if (!S.anchors.empty()) { std::set<int> as(S.anchors.begin(), S.anchors.end());
    for (const Item* c : sc_->curves()) for (int a : c->anchors) if (as.count(a)) { up(c); if (c->ring >= 0) up(sc_->item(c->ring)); break; } }
  for (int id : S.items) { const Item* it = sc_->item(id); if (!it) continue;
    if (it->type == ItemType::Curve) for (auto& [sid, s] : sc_->items) if (s.type == ItemType::Surface && (s.a == id || s.b == id)) rel_.insert(sid);
    if (it->type == ItemType::Surface) { rel_.insert(it->a); rel_.insert(it->b); } }
  int W1 = sceneV_->width(), W2 = curvesV_->width();
  const bool one = sc_->room == "topo";
  if (ccol_->isHidden() != one) { ccol_->setVisible(!one); setWidths(olw_, ccw_, false); }
  sceneV_->begin();
  if (!one) headRow(sceneV_, W1);
  walk(sc_->roots, 0, false, sceneV_, Side::Scene, W1);
  if (!one) sections(sceneV_, W1);
  if (one && sceneV_->y() <= 4) { auto& r = sceneV_->add(0, W1, 40); r.flags = OutlinerView::Hint; r.name.text = tr("No meshes yet — import one (+)"); sceneV_->next(40); }
  sceneV_->finish();
  curvesV_->begin();
  walk(sc_->roots, 0, false, curvesV_, Side::Curves, W2);
  if (curvesV_->y() <= 4) { auto& r = curvesV_->add(0, W2, 40); r.flags = OutlinerView::Hint; r.name.text = tr("No curves yet — draw one (C) or create a group"); curvesV_->next(40); }
  curvesV_->finish();
  building_ = false;
}

std::vector<int> Outliner::topSelected() const {
  std::vector<int> order;
  std::function<void(const std::vector<int>&)> walk = [&](const std::vector<int>& ids) { for (int id : ids) { const Item* it = sc_->item(id); if (!it) continue; order.push_back(id); if (it->type == ItemType::Group || it->type == ItemType::Cocoon || it->type == ItemType::IMesh) walk(it->children); } };
  walk(sc_->roots);
  std::vector<int> out;
  for (int id : order) {
    if (!vp_->isItemSelected(id)) continue;
    bool top = true; for (const Item* p = parentOf(*sc_, sc_->item(id)); p; p = parentOf(*sc_, p)) if (vp_->isItemSelected(p->id)) top = false;
    if (top) out.push_back(id);
  }
  return out;
}

void Outliner::moveItems(std::vector<int> ids, int target, const QString& zone) {
  ids.erase(std::remove_if(ids.begin(), ids.end(), [&](int id) { const Item* it = sc_->item(id); return id == target || !it || it->type == ItemType::Head; }), ids.end());
  if (ids.empty()) { rebuild(); return; }
  int np = -1;
  if (zone != "root") { const Item* t = sc_->item(target); if (!t) return; np = zone == "into" ? t->id : t->parent; }
  if (np >= 0) for (int id : ids) if (sc_->isDescendant(np, id)) { ui::toast(tr("Cannot put a group inside itself")); rebuild(); return; }
  // (into an object: the scene's things — objects, groups (a symmetry group then mirrors about the object's gizmo); they go
  // with it when it is moved)
  if (const Item* pm = np >= 0 ? sc_->item(np) : nullptr; pm && pm->type == ItemType::IMesh)
    for (int id : ids) { const Item* it = sc_->item(id); if (it->type != ItemType::IMesh && it->type != ItemType::Group && it->type != ItemType::Mesh) { ui::toast(tr("Only objects and groups go into an object")); rebuild(); return; } }
  // curve groups (the Curves column) hold only curve things; the scene's groups — only scene things
  if (np >= 0) {
    const Item* pg = sc_->item(np); QString ps = sideOf(pg);
    for (int id : ids) { const Item* it = sc_->item(id);
      if (it->type != ItemType::Cocoon && sideOf(it) != ps && !(it->type == ItemType::Group && it->children.empty())) {
        ui::toast(ps == "curves" ? tr("%1 is a curve group — meshes, guides, cocoons and mesh modifiers stay in the outliner").arg(pg->name) : tr("%1 is a scene group — curves and walls go into the Curves column").arg(pg->name));
        rebuild(); return; } }
  }
  { std::vector<int> fl; for (int f : followers(ids)) if (sc_->item(f)->parent != np) fl.push_back(f);   // (the service curves and walls of moved curves go along)
    ids.insert(ids.end(), fl.begin(), fl.end()); }
  edit([&](Scene& s) {
    for (int id : ids) { Item* it = s.item(id); auto& old = kidsOf(s, it->parent); old.erase(std::remove(old.begin(), old.end(), id), old.end()); it->parent = np; }
    auto& arr = kidsOf(s, np);
    if (zone == "before" || zone == "after") { auto at = std::find(arr.begin(), arr.end(), target); if (at != arr.end() && zone == "after") ++at; arr.insert(at, ids.begin(), ids.end()); }
    else arr.insert(arr.end(), ids.begin(), ids.end());
    if (zone == "into") s.item(np)->expanded = true;
    s.reindex();
    // (a symmetry group into an object: put on that object's own gizmo — it mirrors there, and goes with the object)
    if (const Item* pm = np >= 0 ? s.item(np) : nullptr; pm && pm->type == ItemType::IMesh) {
      const Affine G = vp_->meshFrame(np); const Vec3 z = G.lin({0, 0, 1}), y = G.lin({0, 1, 0}); const Quat W = Quat::lookRotation(-z.normalized(), y.normalized());
      auto qj = [](const QJsonValue& v) { const QJsonArray a = v.toArray(); return a.size() == 4 ? Quat{a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()}.normalized() : Quat{}; };
      Quat P; for (const Item* p = pm; p; p = s.item(p->parent)) P = (qj(p->raw.value("q")) * P).normalized();
      const Quat q = (P.conj() * W).normalized();
      for (int id : ids) if (Item* g = s.item(id); g && g->type == ItemType::Group && g->raw.value("vsym").isObject()) { g->raw.insert("gpivot", QJsonArray{G.t.x, G.t.y, G.t.z}); g->raw.insert("q", QJsonArray{q.x, q.y, q.z, q.w}); }
    }
  }, true);
}

// what goes along with the moved items: the service curves (HC junctions, EC edge curves) on their curves and the walls
// between them — the walls of those service curves too. Only what is not inside the moved items already
std::vector<int> Outliner::followers(const std::vector<int>& ids) const {
  std::set<int> moved, curves;
  std::function<void(int)> add = [&](int id) { const Item* it = sc_->item(id); if (!it || !moved.insert(id).second) return;
    if (it->type == ItemType::Curve) curves.insert(id); for (int c : it->children) add(c); };
  for (int id : ids) add(id);
  if (curves.empty()) return {};
  std::vector<int> out;
  for (bool grew = true; grew;) {
    grew = false;
    std::set<int> at; for (int c : curves) for (int a : sc_->item(c)->anchors) at.insert(a);
    for (auto& [id, it] : sc_->items) {
      if (moved.count(id)) continue;
      bool go = false;
      if (it.type == ItemType::Surface) go = curves.count(it.a) || curves.count(it.b);
      else if (it.type == ItemType::Curve && it.junc) {
        if (it.hasPath()) { for (auto& p : it.path) if (curves.count(p[0])) go = true; }
        else for (int a : it.anchors) if (at.count(a)) { go = true; break; }
      }
      if (!go) continue;
      moved.insert(id); out.push_back(id); if (it.type == ItemType::Curve) { curves.insert(id); grew = true; }
    }
  }
  return out;
}

// Alt+G (and the outliner's pie): the selected — the topmost of them, of one side (curve things or scene things) — into a new
// group where the first of them was; their service curves and walls along. The Topology room: the meshes, in Meshes
void Outliner::groupSelection() {
  std::vector<int> top; for (int id : topSelected()) { const Item* it = sc_->item(id);
    if (it && it->type != ItemType::Head && !(it->type == ItemType::Group && (rawB(*it, "meshesGroup") || rawB(*it, "guidesGroup")))) top.push_back(id); }
  if (top.empty()) { ui::toast(tr("Select what to group first (in the outliner or the viewport)")); return; }
  const QString side = sideOf(sc_->item(top[0]));
  std::vector<int> ids; for (int id : top) if (sideOf(sc_->item(id)) == side) ids.push_back(id);
  const bool skipped = ids.size() < top.size();
  int par = sc_->item(ids[0])->parent;
  for (int id : ids) if (sc_->item(id)->parent != par) { par = -1; break; }
  if (par >= 0 && sideOf(sc_->item(par)) != side) par = -1;
  const bool topo = sc_->room == "topo";
  int gid = -1; const int first = ids[0];
  edit([&](Scene& s) {
    if (par < 0 && topo) par = meshesGroup(s);
    auto& kids = kidsOf(s, par); const auto at = std::find(kids.begin(), kids.end(), first); const long pos = at == kids.end() ? long(kids.size()) : long(at - kids.begin());
    Item& g = Viewport::newGroup(s, -1, topo || side == "scene"); gid = g.id;
    s.roots.erase(std::remove(s.roots.begin(), s.roots.end(), gid), s.roots.end());   // (made at the root — put where the first one was)
    g.parent = par; auto& k2 = kidsOf(s, par); k2.insert(k2.begin() + std::min<long>(pos, long(k2.size())), gid);
    std::vector<int> all = ids; for (int f : followers(ids)) all.push_back(f);
    for (int id : all) { Item* it = s.item(id); auto& old = kidsOf(s, it->parent); old.erase(std::remove(old.begin(), old.end(), id), old.end()); it->parent = gid; s.item(gid)->children.push_back(id); }
    s.reindex();
  }, true);
  if (gid < 0) return;
  vp_->setSelectedItems({gid});
  const Item* g = sc_->item(gid);
  ui::toast(tr("%1: %2 item%3 grouped%4").arg(g ? g->name : QString()).arg(ids.size()).arg(ids.size() == 1 ? "" : "s").arg(skipped ? tr(" (the rest are of the other column — left where they were)") : QString()));
}

// pid: the new parent cocoon (-1 — a trunk of its own); quiet — no commit / toast (P does a batch)
bool Outliner::reparentCocoon(int id, int pid, bool quiet) {
  Item* k = sc_->item(id); if (!k || k->type != ItemType::Cocoon) return false;
  Item* P = pid >= 0 ? sc_->item(pid) : nullptr;
  if (P && (P == k || sc_->isDescendant(P->id, k->id))) { ui::toast(tr("A cocoon can't become a branch of its own branch")); return false; }
  CocoonSystem* cs = vp_->hair().cocoonSystem();
  if (P && cs && cs->dependsOn(P->id, k->id)) { ui::toast(tr("%1 takes its loops from %2 (a link) — it can't be its parent").arg(P->name, k->name)); return false; }
  const Item* cp = cocoonParent(*sc_, *k);
  if (P ? cp == P : !cp) return false;
  auto change = [=](Scene& s) {
    Item* kk = s.item(id);
    if (!P && !kk->raw.value("conv").isObject()) { Item* t = kk; while (const Item* q = cocoonParent(s, *t)) t = s.item(q->id); kk->raw.insert("conv", edit::groupConv(s, t)); }
    const Item* g = parentOf(s, kk); while (g && g->type == ItemType::Cocoon) g = parentOf(s, g);
    int np = P ? pid : (g ? g->id : -1);
    auto& old = kidsOf(s, kk->parent); old.erase(std::remove(old.begin(), old.end(), id), old.end());
    kk->parent = np; kidsOf(s, np).push_back(id);
    if (P) s.item(pid)->expanded = true;
  };
  if (quiet) { change(*sc_); return true; }
  QString kn = k->name, pn = P ? P->name : QString();
  edit(change, true);
  ui::toast(P ? tr("%1 — a branch of %2").arg(kn, pn) : tr("%1 — a trunk of its own").arg(kn));
  return true;
}

// a cocoon takes the loops of the cocoons linked to it where it runs inside them (as of its parent); no loops — the one it
// links to may not take loops from it (by parent or by link, any steps away)
bool Outliner::linkCocoon(int from, int to) {
  const Item *k = sc_->item(from), *L = sc_->item(to);
  if (!k || !L || k->type != ItemType::Cocoon || L->type != ItemType::Cocoon || k == L) return false;
  if (k->parent == to) { ui::toast(tr("%1 is its parent — it takes its loops already").arg(L->name)); return false; }
  for (const QJsonValue& x : k->raw.value("links").toArray()) if (x.toInt() == to) { ui::toast(tr("Linked already")); return false; }
  CocoonSystem* cs = vp_->hair().cocoonSystem();
  if (cs && cs->dependsOn(to, from)) { ui::toast(tr("%1 takes its loops from %2 — a link back would go round in a circle").arg(L->name, k->name)); return false; }
  QString kn = k->name, ln = L->name;
  edit([from, to](Scene& s) { Item* x = s.item(from); QJsonArray a = x->raw.value("links").toArray(); a.append(to); x->raw.insert("links", a); }, true);
  ui::toast(tr("🔗 %1 takes the loops of %2 where it crosses it").arg(kn, ln));
  return true;
}
bool Outliner::unlinkCocoon(int from, int to) {
  const Item* k = sc_->item(from); if (!k) return false;
  QJsonArray a = k->raw.value("links").toArray(), b; bool had = false; for (const QJsonValue& x : a) { if (x.toInt() == to) had = true; else b.append(x); }
  if (!had) return false;
  QString kn = k->name, tn = sc_->item(to) ? sc_->item(to)->name : QString();
  edit([from, b](Scene& s) { Item* x = s.item(from); if (b.isEmpty()) x->raw.remove("links"); else x->raw.insert("links", b); }, true);
  ui::toast(tr("Link broken: %1 — %2").arg(kn, tn));
  return true;
}

void Outliner::parentSelection() {
  std::vector<int> order; for (int id : vp_->selection().items) if (sc_->item(id)) order.push_back(id);
  if (order.size() < 2) { ui::toast(tr("Select the objects, then the new parent (a group or a cocoon) last")); return; }
  const Item* target = sc_->item(order.back());
  std::vector<int> rest(order.begin(), order.end() - 1);
  if (target->type == ItemType::Cocoon) {
    std::vector<int> ks; for (int id : rest) if (sc_->item(id)->type == ItemType::Cocoon) ks.push_back(id);
    if (ks.empty()) { ui::toast(tr("Only cocoons can be branches of a cocoon")); return; }
    // (checked and moved one by one, as the HTML version — then one undo step)
    std::vector<QString> done; int tid = target->id; QString tn = target->name;
    vp_->runEdit([&](Scene&, Selection&, const CurveCache&) { for (int id : ks) { QString n = sc_->item(id)->name; if (reparentCocoon(id, tid, true)) done.push_back(n); } return done.empty() ? QString("!") : QString(); }, true);
    vp_->sceneChanged(false);
    if (!done.empty()) ui::toast(tr("%1 — branch%2 of %3").arg(done.size() > 1 ? tr("%1 cocoons").arg(done.size()) : done[0]).arg(done.size() > 1 ? "es" : "").arg(tn));
    if (ks.size() < rest.size()) ui::toast(tr("Only cocoons can be branches of a cocoon — the rest stayed where they were"));
    return;
  }
  if (target->type == ItemType::IMesh) { std::vector<int> gs; for (int id : rest) if (const Item* x = sc_->item(id); x && (x->type == ItemType::IMesh || x->type == ItemType::Group || x->type == ItemType::Mesh)) { bool inner = false; for (int o : rest) if (o != id && sc_->isDescendant(id, o)) inner = true; if (!inner) gs.push_back(id); }
    if (gs.empty()) { ui::toast(tr("Only objects and groups can be put under an object")); return; }
    moveItems(gs, target->id, "into"); ui::toast(tr("%1 — under %2: they go with it").arg(gs.size() == 1 ? sc_->item(gs[0])->name : tr("%1 items").arg(gs.size()), target->name)); return; }
  if (target->type != ItemType::Group) { ui::toast(tr("The last selected object must be a group or a cocoon")); return; }
  std::vector<int> ids; for (int id : rest) { bool inner = false; for (int o : rest) if (o != id && sc_->isDescendant(id, o)) inner = true; if (!inner) ids.push_back(id); }
  moveItems(ids, target->id, "into");
}

void Outliner::unparentSelection() {
  std::vector<int> top = topSelected(), branches, ids;
  for (int id : top) { const Item* x = sc_->item(id); if (x->type == ItemType::Cocoon && cocoonParent(*sc_, *x)) branches.push_back(id); }
  std::vector<QString> done;
  if (!branches.empty()) {
    vp_->runEdit([&](Scene&, Selection&, const CurveCache&) { for (int id : branches) { QString n = sc_->item(id)->name; if (reparentCocoon(id, -1, true)) done.push_back(n); } return done.empty() ? QString("!") : QString(); }, true);
    vp_->sceneChanged(false);
  }
  for (int id : top) { const Item* x = sc_->item(id); if (x->parent >= 0 && std::find(branches.begin(), branches.end(), id) == branches.end()) ids.push_back(id); }
  if (!done.empty()) ui::toast(tr("%1 — a trunk%2 of %3 own").arg(done.size() > 1 ? tr("%1 cocoons").arg(done.size()) : done[0]).arg(done.size() > 1 ? "s" : "").arg(done.size() > 1 ? tr("their") : tr("its")));
  if (!ids.empty()) moveItems(ids, -1, "root");
  else if (done.empty()) ui::toast(tr("Nothing to take out of a group"));
}

void Outliner::startRename(int id) {
  const Item* it = sc_->item(id); if (!it) return;
  if (it->junc) { ui::toast(tr("Service curves keep their short names")); return; }
  if (it->type == ItemType::Group && rawB(*it, "meshesGroup")) { ui::toast(tr("The Meshes folder keeps its name — every mesh is gathered in it")); return; }
  OutlinerView* v = sceneV_->row(id) ? sceneV_ : curvesV_->row(id) ? curvesV_ : nullptr;
  auto done = [this, id](const QString& t) {
    const Item* x = sc_->item(id); if (!x) return;
    QString nn = t.trimmed(); if (nn.isEmpty() || nn == x->name) { rebuild(); return; }
    edit([id, nn](Scene& s) { s.item(id)->name = nn; });
    static const QRegularExpression nonEn("(?![A-Za-z])\\p{L}", QRegularExpression::UseUnicodePropertiesOption);
    if (nonEn.match(nn).hasMatch()) extras::grant("english");   // (a letter that isn't English)
  };
  if (v) { v->editName(id, it->name, done); return; }
  bool ok = false; QString t = QInputDialog::getText(this, tr("Rename"), tr("Name"), QLineEdit::Normal, it->name, &ok);
  if (ok) done(t);
}

OutlinerView* Outliner::rowAt(int id, QPoint* at, int dx) const {
  for (OutlinerView* v : {sceneV_, curvesV_}) if (const auto* r = v->row(id)) { *at = QPoint(dx == INT_MIN ? r->name.r.center().x() : dx >= 0 ? r->r.left() + dx : r->r.right() + 1 + dx, r->r.center().y()); return v; }
  return nullptr;
}

void Outliner::renameSelected() { if (!vp_->selection().items.empty()) startRename(vp_->selection().items.back()); }

void Outliner::reveal() {
  const Selection& S = vp_->selection();
  const Item* it = S.items.empty() ? nullptr : sc_->item(S.items.back());
  if (!it && !S.anchors.empty()) {
    auto cs = edit::curvesWith(*sc_, S.anchors.back());
    for (int c : cs) { const Item* x = sc_->item(c); if (x->conn.isEmpty() && x->ring < 0) { it = x; break; } }
    if (!it && !cs.empty()) it = sc_->item(cs[0]);
  }
  if (it && it->ring >= 0) it = sc_->item(it->ring);
  if (!it) { ui::toast(tr("Nothing selected")); return; }
  for (const Item* p = parentOf(*sc_, it); p; p = parentOf(*sc_, p)) sc_->item(p->id)->expanded = true;
  if (it->type == ItemType::Cocoon) cocoonsFolded_ = false;
  if (it->type == ItemType::Crown) crownsFolded_ = false;
  rebuild();
  int id = it->id;
  for (auto [v, s] : {std::pair{sceneV_, sceneScroll_}, std::pair{curvesV_, curvesScroll_}}) if (const auto* r = v->row(id)) {
    s->ensureVisible(r->r.center().x(), r->r.center().y(), 0, s->viewport()->height() / 2);
    v->flash(id);
  }
}

}  // namespace hm
