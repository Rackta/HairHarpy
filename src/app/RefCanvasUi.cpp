// The reference canvas's panels (RefCanvas.h): the tool window — a panel of the canvas's own window, dragged by its bar: the
// tools in a column | their modes and the brush sets | their settings — and the side panel: the reference (its opacity, over or
// behind the model, the project's canvases, all the layers moved in the view) over the layers (previews, eyes, masks, drag to
// reorder) and their actions.
#include "app/ColorPicker.h"
#include "app/PaintCanvas.h"
#include "app/PaintSession.h"
#include "app/RefCanvas.h"
#include "app/RefPanels.h"
#include "app/ToolPanels.h"
#include "paint/PaintDoc.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QScreen>
#include <QGuiApplication>
#include <QToolButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hm {

using namespace paint;

namespace {
QString pct(double x) { return QString::number(int(std::lround(x * 100))) + "%"; }
QPushButton* iconBtn(const QString& icon, const QString& tip, int side = 40, bool checkable = false) {
  auto* b = new QPushButton; b->setProperty("tool", true); b->setFixedSize(side, side); b->setIcon(ui::icon(icon)); b->setIconSize(QSize(side - 6, side - 6));
  b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); b->setCheckable(checkable); return b;
}
QLabel* head(const QString& t) { auto* l = new QLabel(t); l->setObjectName("rcHead"); return l; }
QFrame* vline() { auto* f = new QFrame; f->setObjectName("rcSepV"); f->setFixedWidth(1); return f; }
void swatch(QPushButton* b, const QColor& c) { b->setStyleSheet(QString("QPushButton{background:%1;border:1px solid #666;border-radius:3px}").arg(c.name())); }

// the two colours as in Photoshop: the front square — the colour drawn with, the back one — the second; a click on the back one swaps them
class ColourPair : public QWidget {
public:
  explicit ColourPair(PaintSession* s) : s_(s) { setFixedSize(48, 42); setCursor(Qt::PointingHandCursor); setToolTip(QObject::tr("The colour (front) and the second colour (back): click the back one — swap them (X), D — black and white")); }
protected:
  void paintEvent(QPaintEvent*) override { QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    auto sq = [&](const QRectF& r, const QColor& c) { p.setPen(QPen(QColor(0, 0, 0), 1)); p.setBrush(c); p.drawRect(r); p.setPen(QPen(QColor(0xcf, 0xcf, 0xcf), 1)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(1, 1, -1, -1)); };
    sq(QRectF(18.5, 14.5, 27, 25), s_->color2); sq(QRectF(1.5, 1.5, 27, 25), s_->color); }
  void mousePressEvent(QMouseEvent* e) override { if (!QRect(0, 0, 29, 27).contains(e->pos())) { std::swap(s_->color, s_->color2); emit s_->brushChanged(); } }
private:
  PaintSession* s_;
};
}  // namespace

// ================= the layer list (drawn as the Outliner's rows) =================

class RefLayerList : public QWidget {
public:
  RefLayerList(PaintSession* s, QWidget* parent) : QWidget(parent), s_(s) {
    setMouseTracking(true); setMinimumWidth(200);
    soon_ = new QTimer(this); soon_->setSingleShot(true); soon_->setInterval(220); connect(soon_, &QTimer::timeout, this, [this] { update(); });
    connect(s, &PaintSession::layersChanged, this, [this] { refresh(); });
    connect(s, &PaintSession::activeChanged, this, [this] { refresh(); });
    connect(s, &PaintSession::brushChanged, this, [this] { update(); });          // (the mask being painted on: its frame)
    connect(s, &PaintSession::pixelsChanged, this, [this] { if (!soon_->isActive()) soon_->start(); });   // (the previews: a little later, not for every dab)
    refresh();
  }
  void refresh() {
    rows_.clear();
    std::function<void(int, int)> add = [&](int parent, int depth) {
      const auto& L = s_->doc.layers();
      for (int i = int(L.size()) - 1; i >= 0; i--) { const Layer& l = L[size_t(i)]; if (l.parent != parent) continue; rows_.push_back({l.id, depth}); if (l.kind == LayerKind::Group) add(l.id, depth + 1); }
    };
    add(-1, 0); setFixedHeight(std::max(60, int(rows_.size()) * kH + 4)); update();
  }
protected:
  static constexpr int kH = 46;
  struct Row { int id, depth; };
  QRect rowRect(int i) const { return QRect(2, 2 + i * kH, width() - 4, kH - 3); }
  QRect eyeRect(const QRect& r) const { return QRect(r.left() + 4, r.center().y() - 9, 20, 18); }
  QRect thumbRect(const QRect& r, int depth) const { return QRect(r.left() + 30 + depth * 14, r.top() + 4, 46, r.height() - 8); }
  QRect maskRect(const QRect& r, int depth) const { return thumbRect(r, depth).translated(52, 0); }
  QRect nameRect(const QRect& r, const Layer& l, int depth) const { const int x = (l.mask.isNull() ? thumbRect(r, depth) : maskRect(r, depth)).right() + 8; return QRect(x, r.top(), r.right() - x - 26, r.height()); }
  QPixmap thumb(const QImage& img, int id, bool mask, const QSize& box) {
    auto& cache = mask ? mthumbs_ : thumbs_; const qint64 key = img.cacheKey();
    auto f = cache.find(id); if (f != cache.end() && f->second.first == key && f->second.second.size() == box) return f->second.second;
    QPixmap pm(box); pm.fill(QColor(0x30, 0x30, 0x30)); QPainter p(&pm);
    if (!mask) for (int y = 0; y < box.height(); y += 6) for (int x = 0; x < box.width(); x += 6) p.fillRect(x, y, 6, 6, ((x + y) / 6) & 1 ? QColor(0x5a, 0x5a, 0x5a) : QColor(0x78, 0x78, 0x78));
    QImage src = img;
    if (!mask && !img.isNull() && img.format() == QImage::Format_ARGB32) {   // (a canvas the size of the screen with a few strokes: scaled whole they vanish — the preview shows what is drawn, its box)
      int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
      for (int y = 0; y < img.height(); y += 2) { const QRgb* r = reinterpret_cast<const QRgb*>(img.constScanLine(y)); for (int x = 0; x < img.width(); x += 2) if (qAlpha(r[x]) > 12) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); } }
      if (x1 < 0) src = {};
      else { QRect b(QPoint(x0, y0), QPoint(x1 + 1, y1 + 1)); const int pad = std::max(4, std::max(b.width(), b.height()) / 12); b.adjust(-pad, -pad, pad, pad);
        const double want = double(box.width()) / box.height(); if (b.width() < b.height() * want) { const int w = int(b.height() * want); b.adjust(-(w - b.width()) / 2, 0, (w - b.width() + 1) / 2, 0); } else { const int h = int(b.width() / want); b.adjust(0, -(h - b.height()) / 2, 0, (h - b.height() + 1) / 2); }
        src = img.copy(b); }
    }
    if (!src.isNull()) { const QImage sm = src.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation); p.drawImage(QPoint((box.width() - sm.width()) / 2, (box.height() - sm.height()) / 2), sm.format() == QImage::Format_Grayscale8 || sm.format() == QImage::Format_ARGB32 ? sm : sm.convertToFormat(QImage::Format_ARGB32)); }
    p.end(); cache[id] = {key, pm}; return pm;
  }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.fillRect(rect(), QColor(0x26, 0x26, 0x26)); p.setRenderHint(QPainter::Antialiasing);
    if (rows_.empty()) { p.setPen(QColor(0x8a, 0x8a, 0x8a)); p.drawText(rect(), Qt::AlignCenter, tr("No layers — ＋ below")); return; }
    for (int i = 0; i < int(rows_.size()); i++) {
      const Layer* l = s_->doc.layer(rows_[size_t(i)].id); if (!l) continue; const int dp = rows_[size_t(i)].depth;
      const QRect r = rowRect(i); const bool sel = l->id == s_->layer;
      QColor bg = sel ? QColor(0x3a, 0x55, 0x85) : i == hover_ ? QColor(0x2f, 0x2f, 0x2f) : QColor(0, 0, 0, 71);
      p.setPen(QColor(0x2e, 0x2e, 0x2e)); p.setBrush(bg); p.drawRoundedRect(r, 5, 5);
      if (dragging_ && i == drop_) { p.fillRect(QRect(r.left(), dropBelow_ ? r.bottom() + 1 : r.top() - 2, r.width(), 2), QColor(0x66, 0xaa, 0xff)); }
      p.drawPixmap(eyeRect(r).topLeft() + QPoint(2, 1), ui::eyePix(l->visible, l->visible ? QColor(0xcf, 0xcf, 0xcf) : QColor(0x6a, 0x6a, 0x6a)));
      const QRect tr_ = thumbRect(r, dp);
      if (l->kind == LayerKind::Group) { p.setPen(QPen(QColor(0xcf, 0xcf, 0xcf), 1.5)); p.setBrush(Qt::NoBrush); const QRect f = tr_.adjusted(8, 8, -8, -6);
        p.drawPolygon(QPolygon({f.bottomLeft(), f.topLeft() + QPoint(0, 4), f.topLeft() + QPoint(10, 4), f.topLeft() + QPoint(13, 0), f.topRight(), f.bottomRight()})); }
      else if (l->kind == LayerKind::Fill) { p.setPen(QColor(0x55, 0x55, 0x55)); p.setBrush(l->fill); p.drawRect(tr_); }
      else if (l->kind == LayerKind::Adjust) { p.setPen(Qt::NoPen); p.setBrush(QColor(0xcf, 0xcf, 0xcf)); p.drawPie(tr_.adjusted(10, 4, -10, -4), 90 * 16, 180 * 16); p.setBrush(Qt::NoBrush); p.setPen(QColor(0xcf, 0xcf, 0xcf)); p.drawEllipse(tr_.adjusted(10, 4, -10, -4)); }
      else p.drawPixmap(tr_.topLeft(), thumb(l->image, l->id, false, tr_.size()));
      const bool onMask = sel && s_->editMask && !l->mask.isNull();
      if (!l->mask.isNull()) { const QRect mr = maskRect(r, dp); p.drawPixmap(mr.topLeft(), thumb(l->mask, l->id, true, mr.size())); if (!l->maskOn) { p.setPen(QPen(QColor(220, 80, 80), 2)); p.drawLine(mr.topLeft(), mr.bottomRight()); } }
      if (sel && l->kind == LayerKind::Paint) { p.setPen(QPen(QColor(255, 255, 255), 1.5)); p.setBrush(Qt::NoBrush); p.drawRect(QRectF(onMask ? maskRect(r, dp) : tr_).adjusted(-1.5, -1.5, 1.5, 1.5)); }
      const QRect nr = nameRect(r, *l, dp); QFont f = font(); f.setBold(sel); p.setFont(f);
      p.setPen(l->visible ? QColor(0xe6, 0xe6, 0xe6) : QColor(0x80, 0x80, 0x80));
      QString sub = l->kind == LayerKind::Group ? tr("group") : l->blend != Blend::Normal ? blendName(l->blend) : QString(); if (l->opacity < 0.995f) sub += (sub.isEmpty() ? "" : " · ") + pct(l->opacity);
      if (sub.isEmpty()) p.drawText(nr, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(l->name, Qt::ElideRight, nr.width()));
      else { p.drawText(nr.adjusted(0, 6, 0, -nr.height() / 2), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(l->name, Qt::ElideRight, nr.width()));
        QFont g = font(); g.setPointSizeF(g.pointSizeF() * 0.85); p.setFont(g); p.setPen(QColor(0x8a, 0x8a, 0x8a)); p.drawText(nr.adjusted(0, nr.height() / 2, 0, -4), Qt::AlignVCenter | Qt::AlignLeft, sub); }
      if (l->locked) { const QPixmap lk = ui::image("rc_lock").scaled(18, 18, Qt::KeepAspectRatio, Qt::SmoothTransformation); p.drawPixmap(r.right() - 22, r.center().y() - 9, lk); }
    }
  }
  int rowAt(const QPoint& pt) const { const int i = (pt.y() - 2) / kH; return pt.y() >= 2 && i >= 0 && i < int(rows_.size()) ? i : -1; }
  void mousePressEvent(QMouseEvent* e) override {
    const int i = rowAt(e->pos()); press_ = i; p0_ = e->pos(); dragging_ = false; if (i < 0 || e->button() != Qt::LeftButton) return;
    Layer* l = s_->doc.layer(rows_[size_t(i)].id); if (!l) return; const QRect r = rowRect(i);
    if (eyeRect(r).adjusted(-4, -6, 4, 6).contains(e->pos())) { l->visible = !l->visible; press_ = -1; emit s_->pixelsChanged({}); update(); return; }
    const bool mask = !l->mask.isNull() && maskRect(r, rows_[size_t(i)].depth).contains(e->pos());
    if (mask && e->modifiers() & Qt::ShiftModifier) { l->maskOn = !l->maskOn; emit s_->pixelsChanged({}); update(); return; }   // (Shift+click on the mask: off / on, as in Photoshop)
    s_->editMask = mask; s_->setLayer(l->id); emit s_->brushChanged(); update();
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    const int h = rowAt(e->pos()); if (h != hover_) { hover_ = h; update(); }
    if (press_ < 0 || !(e->buttons() & Qt::LeftButton)) return;
    if (!dragging_ && (e->pos() - p0_).manhattanLength() > 6) dragging_ = true;
    if (dragging_) { int i = rowAt(e->pos()); if (i < 0) i = e->pos().y() < 2 ? 0 : int(rows_.size()) - 1; drop_ = i; dropBelow_ = e->pos().y() > rowRect(i).center().y(); update(); }
  }
  void mouseReleaseEvent(QMouseEvent*) override {
    if (dragging_ && press_ >= 0 && drop_ >= 0 && press_ < int(rows_.size())) {
      const int id = rows_[size_t(press_)].id; const Layer* a = s_->doc.layer(id); const Layer* b = s_->doc.layer(rows_[size_t(drop_)].id);
      if (a && b && a->parent == b->parent && a->id != b->id) {   // (among its neighbours: the rows shown top → bottom, the document bottom → top)
        std::vector<int> sib; for (const Row& r : rows_) if (const Layer* x = s_->doc.layer(r.id); x && x->parent == a->parent) sib.push_back(r.id);
        const int from = int(std::find(sib.begin(), sib.end(), id) - sib.begin()); int to = int(std::find(sib.begin(), sib.end(), b->id) - sib.begin());
        if (dropBelow_ && to < from) to++; if (!dropBelow_ && to > from) to--;
        for (int k = 0; k < std::abs(to - from); k++) s_->doc.move(id, to < from ? 1 : -1);
        emit s_->layersChanged(); emit s_->pixelsChanged({});
      } else if (a && b && a->parent != b->parent) ui::toast(tr("A layer moves among the layers of its own group"));
    }
    dragging_ = false; press_ = drop_ = -1; update();
  }
  void mouseDoubleClickEvent(QMouseEvent* e) override {      // (the name: typed over)
    const int i = rowAt(e->pos()); if (i < 0) return; Layer* l = s_->doc.layer(rows_[size_t(i)].id); if (!l) return;
    const QRect nr = nameRect(rowRect(i), *l, rows_[size_t(i)].depth); if (!nr.contains(e->pos())) return;
    auto* ed = new QLineEdit(l->name, this); ed->setGeometry(nr.adjusted(-2, 10, 0, -10)); ed->setStyleSheet("QLineEdit{background:#111;color:#fff;border:1px solid #4f7fd0;padding:0 3px}"); ed->selectAll(); ed->show(); ed->setFocus();
    const int id = l->id;
    connect(ed, &QLineEdit::editingFinished, this, [this, ed, id] { if (Layer* x = s_->doc.layer(id); x && !ed->text().trimmed().isEmpty() && x->name != ed->text()) { x->name = ed->text().trimmed(); emit s_->layersChanged(); } ed->deleteLater(); });
  }
  void leaveEvent(QEvent*) override { hover_ = -1; update(); }
private:
  PaintSession* s_; std::vector<Row> rows_; std::map<int, std::pair<qint64, QPixmap>> thumbs_, mthumbs_;
  int hover_ = -1, press_ = -1, drop_ = -1; bool dragging_ = false, dropBelow_ = false; QPoint p0_; QTimer* soon_;
};

// ================= the tool window =================

void RefCanvas::buildTools() {
  // (a window of its own over the canvas — dragged anywhere on the screen, over the outliner and the panels too; it never takes the
  // keyboard: the canvas's keys keep working after a click on it)
  tw_ = new QFrame(this, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus); tw_->setObjectName("rcTools");
  tw_->setAttribute(Qt::WA_ShowWithoutActivating); tw_->hide();
  tw_->setStyleSheet("#rcTools{background:#262626;border:1px solid #555;border-radius:6px} #rcTwBar{background:#2b2b2b;border-bottom:1px solid #3a3a3a;border-top-left-radius:6px;border-top-right-radius:6px}"
                     " #rcHead{color:#8a8f96;font-size:10px;letter-spacing:1px} #rcSepV{background:#3a3a3a} #rcNote{color:#8a8f96;font-size:11px}"
                     " QPushButton[tool=\"true\"]:checked{background:#3d2f52;border:1px solid #9466be}");   // (the drawing tools' own colour: the UV checker's purple)
  auto* v = new QVBoxLayout(tw_); v->setContentsMargins(1, 1, 1, 1); v->setSpacing(0);
  twBar_ = new QWidget; twBar_->setObjectName("rcTwBar"); twBar_->setFixedHeight(14); twBar_->setCursor(Qt::SizeAllCursor); twBar_->installEventFilter(this); twBar_->setToolTip(tr("Drag — move the tools")); v->addWidget(twBar_);
  auto* cols = new QHBoxLayout; cols->setContentsMargins(6, 4, 6, 6); cols->setSpacing(6); v->addLayout(cols, 1);
  auto note = [](const QString& t) { auto* l = new QLabel(t); l->setObjectName("rcNote"); l->setWordWrap(true); l->setAlignment(Qt::AlignLeft | Qt::AlignTop); return l; };

  // ---- the tools ----
  auto* lc = new QVBoxLayout; lc->setSpacing(3); lc->addWidget(head(tr("TOOLS")));
  struct T { const char *id, *icon; QString tip; };
  const T tools[] = {
    {"brush", "rc_brush", tr("Brush (B): draws into the active layer with the brush of the set. Hold Ctrl — it erases. Alt+RMB drag / [ ] — its size")},
    {"eraser", "rc_eraser", tr("Eraser (E): the same brushes, erasing")},
    {"selection", "rc_select", tr("Selection (S): rectangle, ellipse, lasso, polygonal lasso, magic wand — chosen in the next column. Shift — add, Alt — subtract · Ctrl+A all · Ctrl+D none · Ctrl+Shift+I invert · Delete — clear what is selected. The brush, the fill and the shapes keep inside it")},
    {"fill", "rc_fill", tr("Fill (G): the colour over what matches the clicked pixel (its tolerance and range in the settings)")},
    {"shapes", "rc_shapes", tr("Shapes (U): a rectangle, an ellipse or a line, filled or outlined. Shift — square, circle, 45°")},
    {"move", "rc_move", tr("Move (V): the active layer — or what is selected on it; or all the layers in the view (the next column)")},
    {"select", "rc_transform", tr("Transform (T, Ctrl+T): click a pasted picture, or drag a frame round a part of the active layer (or press on the selection): it floats — drag it to move, corners to scale (Shift — keep proportions), the round handle to turn (Shift — 15° steps). Enter — put it down, Esc — cancel")},
    {"marker", "rc_marker", tr("Anchors (M): points of the world seen on every view, to match where things are between the drawings — click — put one (at the model's depth), drag — move it along this view (its depth stays), Ctrl+click — delete it")},
  };
  for (const T& t : tools) { auto* b = iconBtn(t.icon, t.tip, 40, true); const QString id = t.id; toolBtns_[id] = b; connect(b, &QPushButton::clicked, this, [this, id] { setTool(id); canvas_->setFocus(); }); lc->addWidget(b); }
  lc->addStretch(1); cols->addLayout(lc); cols->addWidget(vline());

  // ---- the modes (and the brush sets) ----
  auto* mid = new QWidget; mid->setFixedWidth(196); auto* mc = new QVBoxLayout(mid); mc->setContentsMargins(0, 0, 0, 0); mc->setSpacing(3);
  auto* mHead = head(tr("MODES")); mc->addWidget(mHead);
  modes_ = new QStackedWidget; mc->addWidget(modes_, 1); cols->addWidget(mid); cols->addWidget(vline());
  auto page = [&](QStackedWidget* st, std::map<QString, int>& idx, std::initializer_list<const char*> keys) { auto* w = new QWidget; w->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); auto* l = new QVBoxLayout(w); l->setContentsMargins(0, 0, 0, 0); l->setSpacing(4); const int n = st->addWidget(w); for (const char* k : keys) idx[k] = n; return l; };   // (a page's text wraps in its column, not cut off by the widest page)
  auto group = [&](QVBoxLayout* into, const QString& label, std::vector<std::tuple<QString, QString, QString>> items, std::function<void(const QString&)> pick) {   // (a row of exclusive icon buttons: key, icon, tip)
    if (!label.isEmpty()) { auto* l = new QLabel(label); l->setObjectName("rcNote"); into->addWidget(l); }
    auto* row = new QHBoxLayout; row->setSpacing(3); auto* g = new QButtonGroup(tw_); g->setExclusive(true);
    for (auto& [key, icon, tip] : items) { auto* b = iconBtn(icon, tip, 36, true); g->addButton(b); modeBtns_[key] = b; row->addWidget(b); const QString k = key; connect(b, &QPushButton::clicked, this, [this, pick, k] { pick(k); syncModes(); syncInfo(); canvas_->setFocus(); }); }
    row->addStretch(1); into->addLayout(row);
  };
  { // the brush sets (brush and eraser)
    auto* l = page(modes_, modePage_, {"brush", "eraser"}); l->addWidget(head(tr("BRUSH SETS")));
    setBox_ = new QComboBox; setBox_->setFocusPolicy(Qt::NoFocus); setBox_->setToolTip(tr("The brush set shown: the built-in Basic, each imported ABR file, your own")); l->addWidget(setBox_);
    brushList_ = new QListWidget; brushList_->setIconSize(QSize(84, 26)); brushList_->setFocusPolicy(Qt::NoFocus); brushList_->setStyleSheet("QListWidget{background:#1e1e1e;border:1px solid #3a3a3a} QListWidget::item{padding:2px} QListWidget::item:selected{background:#3d2f52}"); l->addWidget(brushList_, 1);
    auto* row = new QHBoxLayout; row->setSpacing(3);
    auto* nb = iconBtn("rc_brush_new", tr("A new brush: the brush in hand (its size, hardness, flow…) kept in the set shown")); auto* ib = iconBtn("rc_brush_import", tr("Import Photoshop brushes (.abr): each file becomes a set of its own"));
    auto* ns = iconBtn("rc_set_new", tr("A new set: named, the brush in hand its first brush")); auto* db = iconBtn("rc_delete", tr("Remove the chosen brush (the built-in ones stay)"));
    for (auto* b : {nb, ib, ns, db}) { b->setFixedSize(36, 36); b->setIconSize(QSize(30, 30)); row->addWidget(b); } row->addStretch(1); l->addLayout(row);
    connect(setBox_, &QComboBox::currentIndexChanged, this, [this] { QSettings().setValue("refcanvas/set", setBox_->currentText()); fillBrushes(); });
    connect(brushList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { const int i = it->data(Qt::UserRole).toInt(); if (i < 0 || i >= int(s_->lib.brushes.size())) return;
      const BrushMode m = s_->brush.mode; s_->brush = s_->lib.brushes[size_t(i)]; if (s_->brush.mode != BrushMode::Paint && s_->brush.mode != BrushMode::Erase) s_->brush.mode = m; emit s_->brushChanged(); canvas_->setFocus(); });
    auto shownSet = [this] { const QString t = setBox_->currentText(); return t == tr("All sets") ? QString() : t; };
    connect(nb, &QPushButton::clicked, this, [this, shownSet] { Brush b = s_->brush; const QString set = shownSet(); b.set = set.isEmpty() || set == "Basic" ? QString() : set == "My brushes" ? QString() : set;
      bool ok = false; const QString n = QInputDialog::getText(this, tr("New brush"), tr("Name"), QLineEdit::Normal, b.name + " 2", &ok); if (!ok || n.trimmed().isEmpty()) return;
      b.name = n.trimmed(); s_->lib.brushes.push_back(b); s_->lib.save(); fillBrushes(); ui::toast(tr("Brush kept in %1").arg(BrushLibrary::setOf(b))); });
    connect(ib, &QPushButton::clicked, this, [this] { QSettings st; const QStringList fs = QFileDialog::getOpenFileNames(this, tr("Import brushes"), st.value("refcanvas/abrDir").toString(), tr("Photoshop brushes (*.abr)")); int total = 0; QString last;
      for (const QString& f : fs) { st.setValue("refcanvas/abrDir", QFileInfo(f).absolutePath()); QString err; const int n = s_->lib.importAbr(f, &err); if (n < 0) ui::toast(tr("%1: %2").arg(QFileInfo(f).fileName(), err)); else { total += n; last = QFileInfo(f).completeBaseName(); } }
      if (total) { fillBrushes(); const int i = setBox_->findText(last); if (i >= 0) setBox_->setCurrentIndex(i); ui::toast(tr("%1 brushes imported").arg(total)); } });
    connect(ns, &QPushButton::clicked, this, [this] { bool ok = false; const QString n = QInputDialog::getText(this, tr("New brush set"), tr("The set's name"), QLineEdit::Normal, tr("My set"), &ok); if (!ok || n.trimmed().isEmpty()) return;
      Brush b = s_->brush; b.set = n.trimmed(); s_->lib.brushes.push_back(b); s_->lib.save(); fillBrushes(); const int i = setBox_->findText(b.set); if (i >= 0) setBox_->setCurrentIndex(i); });
    connect(db, &QPushButton::clicked, this, [this] { auto* it = brushList_->currentItem(); const int i = it ? it->data(Qt::UserRole).toInt() : -1;
      if (i < s_->lib.fixed() || i >= int(s_->lib.brushes.size())) { ui::toast(i < s_->lib.builtin ? tr("Choose one of your brushes — the built-in ones stay") : tr("A preset stays: it is read from %1").arg(BrushLibrary::presetFolder())); return; }
      s_->lib.brushes.erase(s_->lib.brushes.begin() + i); s_->lib.save(); fillBrushes(); });
  }
  { auto* l = page(modes_, modePage_, {"selection"});
    group(l, tr("Shape"), {{"sel:rect", "rc_sel_rect", tr("Rectangle")}, {"sel:ellipse", "rc_sel_ellipse", tr("Ellipse")}, {"sel:lasso", "rc_sel_lasso", tr("Lasso: drawn by hand")}}, [this](const QString& k) { selKind_ = k.mid(4); poly_.clear(); polyOn_ = false; });
    group(l, {}, {{"sel:poly", "rc_sel_polylasso", tr("Polygonal lasso: click its corners; a click on the first one, a double click or Enter closes it, Backspace — the last corner back, Esc — dropped")}, {"sel:wand", "rc_sel_wand", tr("Magic wand: what matches the clicked colour (its tolerance and range in the settings)")}}, [this](const QString& k) { selKind_ = k.mid(4); poly_.clear(); polyOn_ = false; });
    // (the two rows are one choice)
    { auto* g = new QButtonGroup(tw_); for (const char* k : {"sel:rect", "sel:ellipse", "sel:lasso", "sel:poly", "sel:wand"}) g->addButton(modeBtns_[k]); }
    group(l, tr("Combine"), {{"sel:op0", "rc_sel_new", tr("New: replaces what was selected")}, {"sel:op1", "rc_sel_add", tr("Add to it (or hold Shift)")}, {"sel:op2", "rc_sel_sub", tr("Subtract from it (or hold Alt)")}}, [this](const QString& k) { selOp_ = k.right(1).toInt(); });
    auto* row = new QHBoxLayout; row->setSpacing(3);
    auto* a = iconBtn("rc_sel_all", tr("All (Ctrl+A)"), 36), *n = iconBtn("rc_sel_none", tr("None (Ctrl+D)"), 36), *iv = iconBtn("rc_sel_invert", tr("Invert (Ctrl+Shift+I)"), 36);
    connect(a, &QPushButton::clicked, this, [this] { selectAll(); }); connect(n, &QPushButton::clicked, this, [this] { deselect(); }); connect(iv, &QPushButton::clicked, this, [this] { invertSel(); });
    auto* lab = new QLabel(tr("Selection")); lab->setObjectName("rcNote"); l->addWidget(lab); row->addWidget(a); row->addWidget(n); row->addWidget(iv); row->addStretch(1); l->addLayout(row); l->addStretch(1);
  }
  { auto* l = page(modes_, modePage_, {"shapes"});
    group(l, tr("Shape"), {{"shape:rect", "rc_shape_rect", tr("Rectangle")}, {"shape:ellipse", "rc_shape_ellipse", tr("Ellipse")}, {"shape:line", "rc_shape_line", tr("Line")}}, [this](const QString& k) { shapeKind_ = k.mid(6); });
    group(l, tr("Drawn"), {{"shape:outline", "rc_shape_outline", tr("Outlined: its line as wide as the settings say")}, {"shape:filled", "rc_shape_filled", tr("Filled")}}, [this](const QString& k) { shapeFilled_ = k == "shape:filled"; });
    l->addStretch(1);
  }
  { auto* l = page(modes_, modePage_, {"move"});
    group(l, tr("What moves"), {{"move:layer", "rc_move_layer", tr("The active layer (or what is selected on it)")}, {"move:all", "rc_move_all", tr("All the layers: the whole canvas moved in the view (its plane shifted). ⟲ in the reference panel puts it back")}}, [this](const QString& k) { moveKind_ = k.mid(5); });
    l->addStretch(1);
  }
  { auto* l = page(modes_, modePage_, {"fill"});
    auto* c1 = new QCheckBox(tr("Touching only")); c1->setToolTip(tr("Only the matching pixels joined to the clicked one (off — every matching pixel of the canvas)")); c1->setChecked(fillContig_); c1->setFocusPolicy(Qt::NoFocus);
    auto* c2 = new QCheckBox(tr("Look at all layers")); c2->setToolTip(tr("Match the colours of the whole picture (off — of the active layer only); the fill goes into the active layer")); c2->setChecked(fillAll_); c2->setFocusPolicy(Qt::NoFocus);
    connect(c1, &QCheckBox::toggled, this, [this](bool on) { fillContig_ = on; }); connect(c2, &QCheckBox::toggled, this, [this](bool on) { fillAll_ = on; });
    l->addWidget(c1); l->addWidget(c2); l->addStretch(1);
  }
  { auto* l = page(modes_, modePage_, {"select"});
    auto* lab = new QLabel(tr("The piece — or the pasted picture, what is selected, the whole layer")); lab->setObjectName("rcNote"); lab->setWordWrap(true); l->addWidget(lab);
    auto* row = new QHBoxLayout; row->setSpacing(3);
    auto* fh = iconBtn("rc_flip_h", tr("Flip it left ↔ right"), 36); auto* fv = iconBtn("rc_flip_v", tr("Flip it top ↔ bottom"), 36); auto* rs = iconBtn("rc_xf_reset", tr("Back as it was: no turn, no flip, its own size (a pasted picture — as pasted), where it was lifted from"), 36);
    row->addWidget(fh); row->addWidget(fv); row->addWidget(rs); row->addStretch(1); l->addLayout(row);
    connect(fh, &QPushButton::clicked, this, [this] { flipFloat(true); canvas_->setFocus(); }); connect(fv, &QPushButton::clicked, this, [this] { flipFloat(false); canvas_->setFocus(); }); connect(rs, &QPushButton::clicked, this, [this] { resetFloat(); canvas_->setFocus(); });
    auto* row2 = new QHBoxLayout; row2->setSpacing(3);
    auto* ok = new QPushButton(tr("✔ Put down (Enter)")); ok->setFocusPolicy(Qt::NoFocus); auto* no = new QPushButton(tr("✖ Cancel (Esc)")); no->setFocusPolicy(Qt::NoFocus);
    row2->addWidget(ok); row2->addWidget(no); l->addLayout(row2);
    connect(ok, &QPushButton::clicked, this, [this] { applyFloat(); canvas_->setFocus(); }); connect(no, &QPushButton::clicked, this, [this] { cancelFloat(); canvas_->setFocus(); });
    l->addWidget(note(tr("Ctrl+Z — a step of the transform back (all of them — the piece put back as it was), Ctrl+Shift+Z — forward")));
    l->addStretch(1); }
  { auto* l = page(modes_, modePage_, {"marker"}); l->addWidget(note(tr("This tool has no modes"))); l->addStretch(1); }

  // ---- the settings ----
  auto* right = new QWidget; auto* rc = new QVBoxLayout(right); rc->setContentsMargins(0, 0, 0, 0); rc->setSpacing(3); rc->addWidget(head(tr("SETTINGS")));
  auto* sa = new QScrollArea; sa->setWidgetResizable(true); sa->setFrameShape(QFrame::NoFrame); sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); sa->setStyleSheet("QScrollArea{background:transparent} QScrollArea > QWidget > QWidget{background:transparent}");
  opts_ = new QStackedWidget; sa->setWidget(opts_); rc->addWidget(sa, 1); right->setFixedWidth(236); cols->addWidget(right);
  auto colours = [this](QVBoxLayout* l) {                    // (the two colours: click — choose, ⇄ — swap them (X))
    auto* row = new QHBoxLayout; row->setSpacing(4); auto* c1 = new QPushButton; auto* c2 = new QPushButton; auto* sw = new QPushButton("⇄");
    for (auto* b : {c1, c2}) { b->setFixedSize(34, 24); b->setFocusPolicy(Qt::NoFocus); } sw->setFixedSize(26, 24); sw->setFocusPolicy(Qt::NoFocus);
    c1->setToolTip(tr("The colour (Alt — pick one from the canvas)")); c2->setToolTip(tr("The second colour (X — swap, D — black and white)")); sw->setToolTip(tr("Swap the colours (X)"));
    auto* lab = new QLabel(tr("Colour")); row->addWidget(lab); row->addStretch(1); row->addWidget(c1); row->addWidget(sw); row->addWidget(c2); l->addLayout(row);
    auto sync = [this, c1, c2] { swatch(c1, s_->color); swatch(c2, s_->color2); }; sync(); connect(s_, &PaintSession::brushChanged, c1, sync);
    connect(c1, &QPushButton::clicked, this, [this, c1] { colourPopup(c1, s_->color, [this](const QColor& c) { s_->color = c; emit s_->brushChanged(); }); });
    connect(c2, &QPushButton::clicked, this, [this, c2] { colourPopup(c2, s_->color2, [this](const QColor& c) { s_->color2 = c; emit s_->brushChanged(); }); });
    connect(sw, &QPushButton::clicked, this, [this] { std::swap(s_->color, s_->color2); emit s_->brushChanged(); });
  };
  auto sl = [](QVBoxLayout* l, const QString& label, double lo, double hi, double step, double val, std::function<QString(double)> fmt, const QString& tip = {}) { auto* s = new SliderRow(label, lo, hi, step, val, fmt); if (!tip.isEmpty()) s->setTip(tip); l->addWidget(s); return s; };
  { auto* l = page(opts_, optPage_, {"brush", "eraser"}); colours(l);
    auto px = [](double x) { return QString::number(int(std::lround(x))) + " px"; }; auto deg = [](double x) { return QString::number(int(x)) + "°"; };
    auto* size = sl(l, tr("Size"), 1, 1024, 1, s_->brush.size, px, tr("In canvas pixels · [ ] or Alt+RMB drag on the canvas")); size->setLog(true);
    auto* op = sl(l, tr("Opacity"), 0.01, 1, 0.01, s_->brush.opacity, pct, tr("The most one stroke lays down · 1…0 keys"));
    auto* fl = sl(l, tr("Flow"), 0.01, 1, 0.01, s_->brush.flow, pct, tr("What one dab adds: low flow builds up"));
    auto* hd = sl(l, tr("Hardness"), 0, 1, 0.01, s_->brush.hardness, pct, tr("Of a round tip · Shift+[ ]"));
    auto* sm = sl(l, tr("Smoothing"), 0, 1, 0.01, s_->brush.smoothing, pct, tr("The stroke kept smooth: the brush follows the pointer on a string this long (0 — off). At the stroke's end it catches up with the pointer"));
    auto* sp = sl(l, tr("Spacing"), 0.01, 1, 0.01, s_->brush.spacing, pct);
    auto* an = sl(l, tr("Angle"), -180, 180, 1, s_->brush.angle, deg);
    auto* sj = sl(l, tr("Size jitter"), 0, 1, 0.01, s_->brush.sizeJitter, pct); auto* aj = sl(l, tr("Angle jitter"), 0, 1, 0.01, s_->brush.angleJitter, pct); auto* sc = sl(l, tr("Scatter"), 0, 2, 0.01, s_->brush.scatter, pct);
    auto chk = [&](const QString& t, bool on) { auto* c = new QCheckBox(t); c->setChecked(on); c->setFocusPolicy(Qt::NoFocus); l->addWidget(c); return c; };
    auto* ps = chk(tr("Pen pressure → size"), s_->brush.pressureSize); auto* pf = chk(tr("Pen pressure → flow"), s_->brush.pressureFlow); auto* fo = chk(tr("Tip follows the stroke"), s_->brush.followStroke);
    l->addStretch(1);
    auto bind = [this](SliderRow* r, float Brush::*m) { connect(r, &SliderRow::changed, this, [this, m](double x) { s_->brush.*m = float(x); emit s_->brushChanged(); }); };
    bind(sm, &Brush::smoothing); bind(size, &Brush::size); bind(op, &Brush::opacity); bind(fl, &Brush::flow); bind(hd, &Brush::hardness); bind(sp, &Brush::spacing); bind(an, &Brush::angle); bind(sj, &Brush::sizeJitter); bind(aj, &Brush::angleJitter); bind(sc, &Brush::scatter);
    connect(ps, &QCheckBox::toggled, this, [this](bool on) { s_->brush.pressureSize = on; }); connect(pf, &QCheckBox::toggled, this, [this](bool on) { s_->brush.pressureFlow = on; }); connect(fo, &QCheckBox::toggled, this, [this](bool on) { s_->brush.followStroke = on; });
    connect(s_, &PaintSession::brushChanged, size, [=] { QSignalBlocker b1(size), b2(op), b3(fl), b4(hd), b5(sp), b6(an), b7(sj), b8(aj), b9(sc), b10(ps), b11(pf), b12(fo), b13(sm); sm->setValue(s_->brush.smoothing);
      size->setValue(s_->brush.size); op->setValue(s_->brush.opacity); fl->setValue(s_->brush.flow); hd->setValue(s_->brush.hardness); sp->setValue(s_->brush.spacing); an->setValue(s_->brush.angle);
      sj->setValue(s_->brush.sizeJitter); aj->setValue(s_->brush.angleJitter); sc->setValue(s_->brush.scatter); ps->setChecked(s_->brush.pressureSize); pf->setChecked(s_->brush.pressureFlow); fo->setChecked(s_->brush.followStroke); });
  }
  { auto* l = page(opts_, optPage_, {"selection"});
    auto* tol = sl(l, tr("Wand tolerance"), 0, 255, 1, wandTol_, [](double x) { return QString::number(int(x)); }, tr("The magic wand: how far a colour may differ from the clicked one and still be taken"));
    connect(tol, &SliderRow::changed, this, [this](double x) { wandTol_ = int(x); });
    auto* c1 = new QCheckBox(tr("Wand: touching only")); c1->setChecked(wandContig_); c1->setFocusPolicy(Qt::NoFocus); c1->setToolTip(tr("Only the matching pixels joined to the clicked one")); l->addWidget(c1);
    auto* c2 = new QCheckBox(tr("Wand: look at all layers")); c2->setChecked(wandAll_); c2->setFocusPolicy(Qt::NoFocus); c2->setToolTip(tr("Match the colours of the whole picture, not only the active layer")); l->addWidget(c2);
    connect(c1, &QCheckBox::toggled, this, [this](bool on) { wandContig_ = on; }); connect(c2, &QCheckBox::toggled, this, [this](bool on) { wandAll_ = on; });
    auto* clr = new QPushButton(tr("Clear what is selected (Delete)")); clr->setFocusPolicy(Qt::NoFocus); l->addWidget(clr); connect(clr, &QPushButton::clicked, this, [this] { clearSelected(); });
    auto* f1 = new QPushButton(tr("Fill with the colour (Alt+Backspace)")); f1->setFocusPolicy(Qt::NoFocus); l->addWidget(f1); connect(f1, &QPushButton::clicked, this, [this] { fillSelection(s_->color); });
    auto* f2 = new QPushButton(tr("Fill with the second colour (Ctrl+Backspace)")); f2->setFocusPolicy(Qt::NoFocus); l->addWidget(f2); connect(f2, &QPushButton::clicked, this, [this] { fillSelection(s_->color2); });
    l->addWidget(note(tr("Shift — add, Alt — subtract. The brush, the eraser, the fill and the shapes stay inside the selection; Move or Transform on it lifts that part of the layer.")));
    l->addStretch(1);
  }
  { auto* l = page(opts_, optPage_, {"fill"}); colours(l);
    auto* tol = sl(l, tr("Tolerance"), 0, 255, 1, fillTol_, [](double x) { return QString::number(int(x)); }, tr("How far a colour may differ from the clicked one and still be filled")); connect(tol, &SliderRow::changed, this, [this](double x) { fillTol_ = int(x); });
    auto* op = sl(l, tr("Opacity"), 0.01, 1, 0.01, fillOp_, pct); connect(op, &SliderRow::changed, this, [this](double x) { fillOp_ = x; });
    l->addStretch(1);
  }
  { auto* l = page(opts_, optPage_, {"shapes"}); colours(l);
    auto* w = sl(l, tr("Line width"), 1, 200, 1, shapeW_, [](double x) { return QString::number(int(x)) + " px"; }); connect(w, &SliderRow::changed, this, [this](double x) { shapeW_ = x; });
    auto* op = sl(l, tr("Opacity"), 0.01, 1, 0.01, shapeOp_, pct); connect(op, &SliderRow::changed, this, [this](double x) { shapeOp_ = x; });
    l->addStretch(1);
  }
  { auto* l = page(opts_, optPage_, {"move"}); l->addWidget(note(tr("Drag on the canvas. The active layer: the whole layer (or, with a selection, what is in it) follows the pointer. All layers: the canvas moves in the view over the model — ⟲ in the reference panel puts it back."))); l->addStretch(1); }
  { auto* l = page(opts_, optPage_, {"select"}); l->addWidget(note(tr("Drag a frame round a part of the active layer (or press on the selection, or click a pasted picture): drag inside — move, the corners — scale (Shift — keep proportions), the round handle — turn (Shift — 15°). Enter — put it down, Esc — cancel."))); l->addStretch(1); }
  { auto* l = page(opts_, optPage_, {"marker"});
    show_ = new QCheckBox(tr("Show the anchors")); show_->setToolTip(tr("The anchor markers on the canvases and in the viewport")); show_->setFocusPolicy(Qt::NoFocus); l->addWidget(show_);
    connect(show_, &QCheckBox::toggled, this, [this](bool on) { sc_->refMarkersShown = on; markersChanged(); });
    auto* clr = new QPushButton(tr("✖ Delete all the anchors")); clr->setFocusPolicy(Qt::NoFocus); l->addWidget(clr); connect(clr, &QPushButton::clicked, this, [this] { sc_->refMarkers.clear(); markersChanged(); });
    l->addWidget(note(tr("An anchor is a point of the world: it shows on every canvas (with a guide line at its height on the side views) and in every pane of the viewport — put one at a lock's tip in the front view and the side view shows where that tip has to be.")));
    l->addStretch(1);
  }
  tw_->resize(52 + 196 + 236 + 40, 470);
  { QSettings st; const QString set = st.value("refcanvas/set").toString(); fillBrushes(); const int i = setBox_->findText(set); if (i >= 0) setBox_->setCurrentIndex(i); }
  // (the modes' buttons as the state is)
  modeBtns_["sel:rect"]->setChecked(true); modeBtns_["sel:op0"]->setChecked(true); modeBtns_["shape:rect"]->setChecked(true); modeBtns_["shape:outline"]->setChecked(true); modeBtns_["move:layer"]->setChecked(true);
  syncModes();
}

void RefCanvas::fillBrushes() {
  if (!setBox_) return;
  const QString was = setBox_->currentText();
  { QSignalBlocker b(setBox_); setBox_->clear(); setBox_->addItem(tr("All sets")); for (const QString& n : s_->lib.sets()) setBox_->addItem(n); const int i = setBox_->findText(was); setBox_->setCurrentIndex(i >= 0 ? i : 0); }
  const QString shown = setBox_->currentIndex() <= 0 ? QString() : setBox_->currentText();
  brushList_->clear();
  for (size_t i = 0; i < s_->lib.brushes.size(); i++) { const Brush& br = s_->lib.brushes[i]; if (!shown.isEmpty() && BrushLibrary::setOf(br) != shown) continue;
    auto* it = new QListWidgetItem(QIcon(QPixmap::fromImage(br.preview(QSize(84, 26)))), br.name); it->setData(Qt::UserRole, int(i)); it->setToolTip(br.name + "  ·  " + BrushLibrary::setOf(br)); brushList_->addItem(it); }
}

void RefCanvas::syncModes() {
  if (!modes_) return;
  for (auto& [id, b] : toolBtns_) b->setChecked(id == tool_);
  if (auto f = modePage_.find(tool_); f != modePage_.end()) modes_->setCurrentIndex(f->second);
  if (auto f = optPage_.find(tool_); f != optPage_.end()) opts_->setCurrentIndex(f->second);
  auto on = [&](const char* k, bool v) { if (auto f = modeBtns_.find(k); f != modeBtns_.end() && v) f->second->setChecked(true); };
  on("sel:rect", selKind_ == "rect"); on("sel:ellipse", selKind_ == "ellipse"); on("sel:lasso", selKind_ == "lasso"); on("sel:poly", selKind_ == "poly"); on("sel:wand", selKind_ == "wand");
  on("sel:op0", selOp_ == 0); on("sel:op1", selOp_ == 1); on("sel:op2", selOp_ == 2);
  on("shape:rect", shapeKind_ == "rect"); on("shape:ellipse", shapeKind_ == "ellipse"); on("shape:line", shapeKind_ == "line"); on("shape:filled", shapeFilled_); on("shape:outline", !shapeFilled_);
  on("move:layer", moveKind_ == "layer"); on("move:all", moveKind_ == "all");
}

void RefCanvas::placeTools() {
  if (!tw_) return;
  const QJsonObject st = QJsonDocument::fromJson(QSettings().value("refcanvas/tools").toByteArray()).object();
  const QPoint o = vpBox_->window()->mapToGlobal(QPoint(0, 0));   // (its place kept from the main window's corner)
  QPoint p = st.contains("gx") ? o + QPoint(st.value("gx").toInt(), st.value("gy").toInt()) : mapToGlobal(QPoint(10, top_->sizeHint().height() + 10));
  QScreen* sc = QGuiApplication::screenAt(p + QPoint(40, 10)); if (!sc) sc = screen();
  const QRect g = sc->availableGeometry();   // (all of it on the screen: nothing cut off at an edge)
  p.setX(std::clamp(p.x(), g.left(), std::max(g.left(), g.right() - tw_->width()))); p.setY(std::clamp(p.y(), g.top(), std::max(g.top(), g.bottom() - tw_->height())));
  tw_->move(p); tw_->raise();
}
void RefCanvas::saveTools() const { if (!tw_) return; const QPoint p = tw_->pos() - vpBox_->window()->mapToGlobal(QPoint(0, 0)); QSettings().setValue("refcanvas/tools", QJsonDocument(QJsonObject{{"gx", p.x()}, {"gy", p.y()}}).toJson(QJsonDocument::Compact)); }

bool RefCanvas::eventFilter(QObject* o, QEvent* e) {
  if (o != twBar_) return QWidget::eventFilter(o, e);
  if (e->type() == QEvent::MouseButtonPress) { auto* m = static_cast<QMouseEvent*>(e); if (m->button() == Qt::LeftButton) { twDrag_ = true; twGrab_ = m->globalPosition().toPoint() - tw_->pos(); } return true; }
  if (e->type() == QEvent::MouseMove && twDrag_) { const QPoint gp = static_cast<QMouseEvent*>(e)->globalPosition().toPoint(); QPoint p = gp - twGrab_;
    QScreen* sc = QGuiApplication::screenAt(gp); if (!sc) sc = tw_->screen(); const QRect g = sc->availableGeometry();
    p.setX(std::clamp(p.x(), g.left() - tw_->width() + 60, g.right() - 60)); p.setY(std::clamp(p.y(), g.top(), g.bottom() - 20)); tw_->move(p); return true; }
  if (e->type() == QEvent::MouseButtonRelease && twDrag_) { twDrag_ = false; saveTools(); canvas_->setFocus(); return true; }
  return QWidget::eventFilter(o, e);
}

// ================= the colour =================

void RefCanvas::buildColour(QVBoxLayout* v) {
  QSettings st;
  v->addWidget(head(tr("COLOUR")));
  auto* row = new QHBoxLayout; row->setSpacing(4); auto* pair = new ColourPair(s_); row->addWidget(pair);
  auto* sw = new QPushButton("⇄"); sw->setFixedSize(26, 24); sw->setFocusPolicy(Qt::NoFocus); sw->setToolTip(tr("Swap the colours (X)")); row->addWidget(sw, 0, Qt::AlignTop);
  auto* bw = new QPushButton("◩"); bw->setFixedSize(26, 24); bw->setFocusPolicy(Qt::NoFocus); bw->setToolTip(tr("Black and white (D)")); row->addWidget(bw, 0, Qt::AlignTop);
  row->addStretch(1);
  auto* mode = new QComboBox; mode->setFocusPolicy(Qt::NoFocus); mode->setToolTip(tr("The picker's view: quad, triangle, round, bars, or an image to take colours from"));
  for (const QString& k : ColorPicker::modes()) mode->addItem(ColorPicker::modeName(k), k);
  row->addWidget(mode, 0, Qt::AlignTop); v->addLayout(row);
  picker_ = new ColorPicker; picker_->setFixedHeight(180); v->addWidget(picker_);
  { const int i = mode->findData(st.value("refcanvas/pickerMode", "quad").toString()); mode->setCurrentIndex(std::max(0, i)); picker_->setMode(mode->currentData().toString()); }
  connect(mode, &QComboBox::activated, this, [this, mode](int) { picker_->setMode(mode->currentData().toString()); QSettings().setValue("refcanvas/pickerMode", mode->currentData().toString()); });
  auto* hr = new QHBoxLayout; hr->setSpacing(4);
  hex_ = new QLineEdit; hex_->setFixedWidth(84); hex_->setToolTip(tr("The colour as #RRGGBB — type one and press Enter")); hr->addWidget(hex_);
  auto* hsv = new QLabel; hsv->setObjectName("rcNote"); hr->addWidget(hsv, 1);
  v->addLayout(hr);
  auto sync = [this, hsv, pair] { picker_->setColor(s_->color); if (!hex_->hasFocus()) hex_->setText(s_->color.name().toUpper());
    const QColor c = s_->color.toHsv(); hsv->setText(tr("H %1°  S %2%  V %3%").arg(std::max(0, c.hsvHue())).arg(int(std::lround(c.hsvSaturationF() * 100))).arg(int(std::lround(c.valueF() * 100)))); pair->update(); };
  sync(); connect(s_, &PaintSession::brushChanged, picker_, sync);
  picker_->changed = [this](const QColor& c) { s_->color = c; emit s_->brushChanged(); };
  picker_->committed = [this](const QColor& c) { addRecent(c); };
  picker_->onLoad = [this] { QSettings st; const QString f = QFileDialog::getOpenFileName(this, tr("A picture to take colours from"), st.value("refs/dir").toString(), tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)")); if (!f.isEmpty()) picker_->loadImage(f); };
  connect(hex_, &QLineEdit::editingFinished, this, [this] { QString t = hex_->text().trimmed(); if (!t.startsWith('#')) t.prepend('#'); const QColor c(t); if (!c.isValid()) { hex_->setText(s_->color.name().toUpper()); return; } if (c != s_->color) { s_->color = c; emit s_->brushChanged(); addRecent(c); } canvas_->setFocus(); });
  connect(sw, &QPushButton::clicked, this, [this] { std::swap(s_->color, s_->color2); emit s_->brushChanged(); });
  connect(bw, &QPushButton::clicked, this, [this] { s_->color = Qt::black; s_->color2 = Qt::white; emit s_->brushChanged(); });
  // (the kept colours and the recent ones: each a fold of its own)
  auto load = [](const QString& key) { std::vector<QColor> o; for (const QString& n : QSettings().value(key).toStringList()) { const QColor c(n); if (c.isValid()) o.push_back(c); } return o; };
  auto store = [](const QString& key, const std::vector<QColor>& cs) { QStringList l; for (const QColor& c : cs) l << c.name(); QSettings().setValue(key, l); };
  auto fold = [&](const QString& title, const QString& key, SwatchGrid* g) {
    auto* b = new QToolButton; b->setText(title); b->setCheckable(true); b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); b->setAutoRaise(true); b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet("QToolButton{color:#cfcfcf;border:none;padding:1px 0;font-size:11px}"); const bool open = QSettings().value(key, true).toBool(); b->setChecked(open); b->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    v->addWidget(b); v->addWidget(g); g->setVisible(open);
    connect(b, &QToolButton::toggled, this, [b, g, key](bool on) { g->setVisible(on); b->setArrowType(on ? Qt::DownArrow : Qt::RightArrow); QSettings().setValue(key, on); });
  };
  savedG_ = new SwatchGrid; savedG_->plus = true; savedG_->setColors(load("refcanvas/colours/saved"));
  fold(tr("Saved colours"), "refcanvas/colours/savedOpen", savedG_);
  savedG_->picked = [this](const QColor& c) { s_->color = c; emit s_->brushChanged(); addRecent(c); };
  savedG_->added = [this, store] { auto cs = savedG_->colors(); if (std::find(cs.begin(), cs.end(), s_->color) == cs.end()) cs.push_back(s_->color); savedG_->setColors(cs); store("refcanvas/colours/saved", cs); };
  savedG_->removed = [this, store](int i) { auto cs = savedG_->colors(); if (i >= 0 && i < int(cs.size())) cs.erase(cs.begin() + i); savedG_->setColors(cs); store("refcanvas/colours/saved", cs); };
  recentG_ = new SwatchGrid; recentG_->setColors(load("refcanvas/colours/recent"));
  fold(tr("Recent colours"), "refcanvas/colours/recentOpen", recentG_);
  recentG_->picked = [this](const QColor& c) { s_->color = c; emit s_->brushChanged(); };
}

// a colour drawn with: to the front of the recent ones (once; the newest 24 kept)
void RefCanvas::addRecent(const QColor& c0) {
  if (!recentG_ || !c0.isValid()) return; const QColor c = QColor(c0.rgb());
  auto cs = recentG_->colors(); if (!cs.empty() && cs.front() == c) return;
  cs.erase(std::remove(cs.begin(), cs.end(), c), cs.end()); cs.insert(cs.begin(), c); if (cs.size() > 24) cs.resize(24);
  recentG_->setColors(cs);
  QStringList l; for (const QColor& x : cs) l << x.name(); QSettings().setValue("refcanvas/colours/recent", l);
}

// a colour chosen in a little window under a button (the tool window's squares, a fill layer's colour): the same picker, its own view
void RefCanvas::colourPopup(QWidget* anchor, const QColor& init, std::function<void(const QColor&)> set, std::function<void()> done) {
  auto* f = new QFrame(this, Qt::Popup); f->setAttribute(Qt::WA_DeleteOnClose); f->setObjectName("rcPop"); f->setStyleSheet("#rcPop{background:#262626;border:1px solid #555;border-radius:6px}");
  auto* v = new QVBoxLayout(f); v->setContentsMargins(8, 8, 8, 8); v->setSpacing(5);
  auto* mode = new QComboBox; for (const QString& k : ColorPicker::modes()) mode->addItem(ColorPicker::modeName(k), k); v->addWidget(mode);
  auto* pk = new ColorPicker; pk->setFixedSize(280, 180); pk->setColor(init); v->addWidget(pk);
  auto* hex = new QLineEdit(init.name().toUpper()); v->addWidget(hex);
  { const int i = mode->findData(QSettings().value("refcanvas/pickerMode", "quad").toString()); mode->setCurrentIndex(std::max(0, i)); pk->setMode(mode->currentData().toString()); }
  connect(mode, &QComboBox::activated, pk, [pk, mode](int) { pk->setMode(mode->currentData().toString()); });
  pk->changed = [set, hex](const QColor& c) { hex->setText(c.name().toUpper()); set(c); };
  pk->committed = [this](const QColor& c) { addRecent(c); };
  pk->onLoad = [this, pk] { const QString fn = QFileDialog::getOpenFileName(this, tr("A picture to take colours from"), QSettings().value("refs/dir").toString(), tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)")); if (!fn.isEmpty()) pk->loadImage(fn); };
  connect(hex, &QLineEdit::editingFinished, pk, [pk, hex, set] { QString t = hex->text().trimmed(); if (!t.startsWith('#')) t.prepend('#'); const QColor c(t); if (c.isValid()) { pk->setColor(c); set(c); } });
  if (done) connect(f, &QObject::destroyed, this, [done] { done(); });
  f->adjustSize(); QPoint at = anchor->mapToGlobal(QPoint(0, anchor->height() + 4));
  if (QScreen* sc = anchor->screen()) { const QRect g = sc->availableGeometry(); at.setX(std::clamp(at.x(), g.left(), g.right() - f->width())); if (at.y() + f->height() > g.bottom()) at.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - f->height() - 4); }
  f->move(at); f->show();
}

// ================= the side panel =================

void RefCanvas::buildSide(QWidget* col) {
  col->setStyleSheet("#rcHead{color:#9a9a9a;font-size:11px;font-weight:bold;letter-spacing:1px;margin-top:4px} #rcNote{color:#8a8f96;font-size:11px}"
                     " QPushButton[tool=\"true\"]:checked{background:#3d2f52;border:1px solid #9466be}");
  auto* v = new QVBoxLayout(col); v->setContentsMargins(8, 6, 8, 8); v->setSpacing(5);
  buildColour(v);
  v->addSpacing(4); v->addWidget(head(tr("REFERENCE")));
  { auto* row = new QHBoxLayout; row->setSpacing(6); row->addWidget(new QLabel(tr("Opacity")));
    op_ = new QSlider(Qt::Horizontal); op_->setRange(2, 100); op_->setFocusPolicy(Qt::NoFocus); op_->setToolTip(tr("How see-through the whole canvas is over (or behind) the model")); row->addWidget(op_, 1);
    opV_ = new QLabel("85%"); opV_->setFixedWidth(34); opV_->setProperty("mut", true); row->addWidget(opV_); v->addLayout(row);
    connect(op_, &QSlider::valueChanged, this, [this](int x) { opV_->setText(QString::number(x) + "%"); if (RefPlane* p = plane()) { p->opacity = x / 100.0; vp_->refPlanesEdited(false); } });
    connect(op_, &QSlider::sliderReleased, this, [this] { vp_->refPlanesEdited(true); }); }
  { auto* row = new QHBoxLayout; row->setSpacing(4); auto* g = new QButtonGroup(col);
    auto mk = [&](const QString& icon, const QString& text, const QString& tip) { auto* b = new QPushButton(text); b->setIcon(ui::icon(icon)); b->setIconSize(QSize(24, 24)); b->setCheckable(true); b->setProperty("tool", true); b->setMinimumHeight(32); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); g->addButton(b); row->addWidget(b, 1); return b; };
    frontB_ = mk("rc_front", tr("In front"), tr("The canvas drawn over the model")); behindB_ = mk("rc_behind", tr("Behind"), tr("The canvas behind the model: the model covers it"));
    connect(frontB_, &QPushButton::clicked, this, [this] { if (RefPlane* p = plane()) { p->over = true; vp_->refPlanesEdited(true); } });
    connect(behindB_, &QPushButton::clicked, this, [this] { if (RefPlane* p = plane()) { p->over = false; vp_->refPlanesEdited(true); } });
    v->addLayout(row); }
  { auto* row = new QHBoxLayout; row->setSpacing(4); auto* ic = new QLabel; ic->setPixmap(ui::image("rc_library").scaled(26, 26, Qt::KeepAspectRatio, Qt::SmoothTransformation)); ic->setToolTip(tr("The project's canvases")); row->addWidget(ic);
    lib_ = new QComboBox; lib_->setFocusPolicy(Qt::NoFocus); lib_->setToolTip(tr("The library of the project's canvases: choose which one this view shows (a canvas shown by several views is shared — a change reaches them all)")); row->addWidget(lib_, 1);
    auto* cp = iconBtn("rc_canvas_copy", tr("Make this view's canvas a separate copy (its layers copied; the other views keep the old one)"), 32); row->addWidget(cp); v->addLayout(row);
    connect(lib_, &QComboBox::activated, this, [this](int) { const int id = lib_->currentData().toInt(); RefPlane* p = plane(); if (!p || id == cid_ || !sc_->refSketches.count(id)) return;
      if (fl_.on) applyFloat(); flush();
      // (the plane takes the new canvas's proportions: its height kept — as the 🖼 menu's list does; the place kept for leaving follows)
      const bool fl = p->follows() && p->hasLoc;
      refPlaneSetCanvas(*p, id, *sc_);
      if (fl && hasLoc_) { Vec3 t, s; Quat q; m4Decompose(p->loc, t, q, s); const double a = std::abs(s.y) > 0 ? std::abs(s.x) / std::abs(s.y) : 1;
        m4Decompose(savedLoc_, t, q, s); s.x = std::abs(s.y) * a * (s.x < 0 ? -1 : 1); savedLoc_ = m4Compose(t, q, s); }
      vp_->refPlanesEdited(true); loadCanvas(id); syncPlaneUi(); followView(); if (changed) changed(); });
    connect(cp, &QPushButton::clicked, this, [this] { RefPlane* p = plane(); if (!p || cid_ < 0) return; if (fl_.on) applyFloat(); dirty_ = true; flush();
      const int nid = refs::copyCanvas(*sc_, cid_); if (nid < 0) return; p->canvas = nid; vp_->refPlanesEdited(true); loadCanvas(nid); syncPlaneUi(); if (changed) changed(); ui::toast(tr("This view has its own copy of the canvas now")); }); }
  { auto* row = new QHBoxLayout; row->setSpacing(4); auto* lab = new QLabel(tr("All layers in the view")); lab->setObjectName("rcNote"); row->addWidget(lab, 1);
    auto* mv = iconBtn("rc_move_all", tr("Move all the layers: the whole canvas moved in the view over the model (✥ Move, all layers)"), 32); auto* bk = iconBtn("rc_offset_reset", tr("Put the canvas back where it was when it was opened"), 32);
    row->addWidget(mv); row->addWidget(bk); v->addLayout(row);
    connect(mv, &QPushButton::clicked, this, [this] { moveKind_ = "all"; setTool("move"); canvas_->setFocus(); });
    connect(bk, &QPushButton::clicked, this, [this] { if (offD_.x == 0 && offD_.y == 0 && offD_.z == 0) { ui::toast(tr("The canvas is where it was opened")); return; } shiftPlane(Vec3() - offD_); offD_ = Vec3(); vp_->refPlanesEdited(true); if (changed) changed(); }); }

  v->addSpacing(4); v->addWidget(head(tr("LAYERS")));
  { auto* row = new QHBoxLayout; row->setSpacing(4);
    blend_ = new QComboBox; blend_->setFocusPolicy(Qt::NoFocus); blend_->setToolTip(tr("How the layer mixes with what is under it"));
    for (Blend b : {Blend::Normal, Blend::Multiply, Blend::Screen, Blend::Overlay, Blend::Add, Blend::Subtract, Blend::SoftLight, Blend::Darken, Blend::Lighten}) blend_->addItem(blendName(b), int(b));
    row->addWidget(blend_, 1);
    fillCol_ = new QPushButton; fillCol_->setFixedSize(30, 26); fillCol_->setFocusPolicy(Qt::NoFocus); fillCol_->setToolTip(tr("The fill layer's colour")); fillCol_->hide(); row->addWidget(fillCol_);
    connect(fillCol_, &QPushButton::clicked, this, [this] { Layer* l = s_->active(); if (!l || l->kind != LayerKind::Fill) return;
      colourPopup(fillCol_, l->fill, [this](const QColor& c) { if (Layer* x = s_->active(); x && x->kind == LayerKind::Fill) { x->fill = c; swatch(fillCol_, c); emit s_->pixelsChanged({}); } }, [this] { emit s_->layersChanged(); }); });
    lockB_ = iconBtn("rc_lock", tr("Lock the layer: nothing draws on it"), 30, true); row->addWidget(lockB_); v->addLayout(row);
    connect(blend_, &QComboBox::activated, this, [this](int) { if (Layer* l = s_->active()) { l->blend = Blend(blend_->currentData().toInt()); emit s_->pixelsChanged({}); emit s_->layersChanged(); } });
    connect(lockB_, &QPushButton::toggled, this, [this](bool on) { if (Layer* l = s_->active(); l && l->locked != on) { l->locked = on; emit s_->layersChanged(); } }); }
  lop_ = new SliderRow(tr("Opacity"), 0, 1, 0.01, 1, pct); lop_->setTip(tr("The active layer's opacity")); v->addWidget(lop_);
  connect(lop_, &SliderRow::changed, this, [this](double x) { if (Layer* l = s_->active()) { l->opacity = float(x); emit s_->pixelsChanged({}); } });
  connect(lop_, &SliderRow::released, this, [this](double) { emit s_->layersChanged(); });
  auto* sa = new QScrollArea; sa->setWidgetResizable(false); sa->setFrameShape(QFrame::NoFrame); sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); sa->setStyleSheet("QScrollArea{background:#262626;border:1px solid #3a3a3a;border-radius:4px}");
  layerList_ = new RefLayerList(s_, nullptr); sa->setWidget(layerList_); sa->setMinimumHeight(240); v->addWidget(sa, 1);
  { // (the list as wide as its frame)
    struct Fit : QObject { QScrollArea* a; QWidget* w; Fit(QScrollArea* a_, QWidget* w_) : QObject(a_), a(a_), w(w_) { a->viewport()->installEventFilter(this); }
      bool eventFilter(QObject*, QEvent* e) override { if (e->type() == QEvent::Resize) w->setFixedWidth(a->viewport()->width()); return false; } };
    new Fit(sa, layerList_); }
  { auto* row = new QHBoxLayout; row->setSpacing(2);
    struct A { const char *what, *icon; QString tip; };
    const A acts[] = {{"new", "rc_layer_new", tr("A new layer above the active one (Ctrl+Shift+N)")}, {"fill", "rc_fill_layer", tr("A fill layer: one colour, white to begin with — put at the bottom, under the drawing, as its paper. Its colour: the square by the blend mode; move it up, mask it or blend it like any layer")},
      {"group", "rc_group_new", tr("A new group: layers put into it blend as one")},
      {"mask", "rc_mask", tr("A mask on the active layer — from the selection when there is one; it has one already: its menu (paint on it, invert, apply, delete)")},
      {"merge", "rc_merge", tr("Merge down: the active layer onto the one under it (Ctrl+E)")}, {"up", "rc_layer_up", tr("Move the layer up")}, {"down", "rc_layer_down", tr("Move the layer down")},
      {"dup", "rc_layer_dup", tr("Duplicate the layer (Ctrl+J) — with a selection: a copy of what is selected on a new layer")}, {"delete", "rc_delete", tr("Delete the layer (a group — with its layers)")}};
    for (const A& a : acts) { auto* b = iconBtn(a.icon, a.tip, 31); const QString w = a.what; row->addWidget(b); connect(b, &QPushButton::clicked, this, [this, w, b] { layerAction(w, b); canvas_->setFocus(); }); }
    row->addStretch(1); v->addLayout(row); }
  { auto* n = new QLabel(tr("Click — the active layer · the eye — show / hide · the mask's picture — paint on the mask (Shift+click — mask off) · double-click the name — rename · drag a row — reorder")); n->setObjectName("rcNote"); n->setWordWrap(true); v->addWidget(n); }
  connect(s_, &PaintSession::activeChanged, this, [this] { syncLayerProps(); });
  connect(s_, &PaintSession::layersChanged, this, [this] { syncLayerProps(); });
}

void RefCanvas::syncLayerProps() {
  Layer* l = s_->active(); const bool has = l != nullptr;
  blend_->setEnabled(has && l->kind != LayerKind::Adjust); lop_->setEnabled(has); lockB_->setEnabled(has);
  fillCol_->setVisible(has && l->kind == LayerKind::Fill); if (has && l->kind == LayerKind::Fill) swatch(fillCol_, l->fill);
  if (!has) return;
  QSignalBlocker a(blend_), b(lop_), c(lockB_);
  blend_->setCurrentIndex(std::max(0, blend_->findData(int(l->blend)))); lop_->setValue(l->opacity); lockB_->setChecked(l->locked);
}

void RefCanvas::fillLibrary() {
  if (!lib_) return;
  QSignalBlocker b(lib_); lib_->clear();
  for (auto& [id, sk] : sc_->refSketches) { const int users = refs::canvasUsers(*sc_, id);
    lib_->addItem((sk.name.isEmpty() ? tr("Canvas %1").arg(id) : sk.name) + (users > 1 ? tr("  · %1 views").arg(users) : users == 0 ? tr("  · not shown") : QString()), id); }
  const int i = lib_->findData(cid_); if (i >= 0) lib_->setCurrentIndex(i);
}

void RefCanvas::layerAction(const QString& what, QWidget* anchor) {
  if (fl_.on) applyFloat();
  s_->ensureDoc(); Layer* l = s_->active();
  auto done = [this] { emit s_->layersChanged(); emit s_->activeChanged(); emit s_->pixelsChanged({}); };
  if (what == "new") { s_->layer = s_->doc.addPaint(Channel::Color, tr("Layer"), s_->layer); s_->editMask = false; done(); return; }
  if (what == "fill") {   // (a white sheet under the drawing: at the bottom of the layers — its colour by the blend mode, moved up like any layer)
    const int id = s_->doc.addFill(Channel::Color, Qt::white, tr("Fill")); s_->doc.move(id, -int(s_->doc.layers().size())); s_->layer = id; s_->editMask = false; done(); return; }
  if (what == "group") { s_->layer = s_->doc.addGroup(tr("Group"), s_->layer); done(); return; }
  if (!l) { ui::toast(tr("No layer chosen")); return; }
  auto maskFromSel = [this](Layer* x) { x->mask = QImage(selMask_.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < selMask_.height(); y++) memcpy(x->mask.scanLine(y), selMask_.constScanLine(y), size_t(selMask_.width())); };
  if (what == "mask") {
    if (l->mask.isNull()) { s_->doc.addMask(l->id, true); if (hasSel() && !l->mask.isNull()) { maskFromSel(l); deselect(); } s_->editMask = true; emit s_->brushChanged(); done(); ui::toast(tr("A mask: paint black (Ctrl — erase) to hide, white to show")); return; }
    QMenu m(this); QAction* paintOn = m.addAction(tr("Paint on the mask")); paintOn->setCheckable(true); paintOn->setChecked(s_->editMask);
    QAction* off = m.addAction(tr("Mask on")); off->setCheckable(true); off->setChecked(l->maskOn);
    QAction* fromSel = hasSel() ? m.addAction(tr("The mask from the selection")) : nullptr;
    m.addSeparator(); QAction* inv = m.addAction(tr("Invert the mask")); QAction* app = m.addAction(tr("Apply: burn it into the layer")); QAction* del = m.addAction(tr("Delete the mask"));
    QAction* r = m.exec(anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QCursor::pos()); if (!r) return;
    if (r == paintOn) { s_->editMask = paintOn->isChecked(); emit s_->brushChanged(); }
    else if (r == off) { l->maskOn = off->isChecked(); }
    else if (r == fromSel) { maskFromSel(l); deselect(); }
    else if (r == inv) s_->doc.invertMask(l->id);
    else if (r == app) { s_->doc.applyMask(l->id); s_->editMask = false; }
    else if (r == del) { s_->doc.removeMask(l->id); s_->editMask = false; }
    emit s_->brushChanged(); done(); return;
  }
  if (what == "merge") { int into = -1; if (!s_->doc.mergeDown(l->id, &into)) { ui::toast(tr("Nothing to merge it onto: the layer under it must be a colour paint layer of the same group")); return; } if (!s_->doc.layer(s_->layer)) s_->layer = into; done(); return; }
  if (what == "up" || what == "down") { s_->doc.move(l->id, what == "up" ? 1 : -1); done(); return; }
  if (what == "dup") {
    if (hasSel() && l->kind == LayerKind::Paint && l->channel == Channel::Color) {   // (Ctrl+J with a selection: what is selected onto a new layer)
      const int srcId = l->id;   // (addPaint grows the layer list: l is not to be read after it)
      const int id = s_->doc.addPaint(Channel::Color, l->name + tr(" part"), srcId); Layer* n = s_->doc.layer(id); const Layer* src = s_->doc.layer(srcId);
      if (n && src) { n->image = src->image.copy(); for (int y = 0; y < n->image.height(); y++) { QRgb* q = reinterpret_cast<QRgb*>(n->image.scanLine(y)); const uchar* m = selMask_.constScanLine(y); for (int x = 0; x < n->image.width(); x++) q[x] = qRgba(qRed(q[x]), qGreen(q[x]), qBlue(q[x]), qAlpha(q[x]) * m[x] / 255); } }
      s_->layer = id; done(); return; }
    const int n = s_->doc.duplicate(l->id); if (n >= 0) s_->layer = n; done(); return;
  }
  if (what == "delete") {
    if (s_->doc.layers().size() <= 1) { ui::toast(tr("The canvas keeps one layer at least")); return; }
    const int i = s_->doc.indexOf(l->id); s_->doc.remove(l->id); s_->editMask = false;
    s_->layer = s_->doc.layers().empty() ? -1 : s_->doc.layers()[size_t(std::clamp(i - 1, 0, int(s_->doc.layers().size()) - 1))].id; done(); return;
  }
}

void RefCanvas::loadCanvas(int cid) {
  if (cid == cid_) return;
  flush(); cid_ = -1; s_->undo.clear(); s_->redo.clear(); placed_.clear(); deselect(); s_->editMask = false;
  stash_.erase(cid);   // (its kept copy, written into the scene by the flush: read from there now — an older copy never taken up over newer drawing)
  const RefSketch& sk = sc_->refSketches[cid];
  if (s_->doc.unpackJson(sk.data)) { s_->layer = s_->doc.layers().empty() ? -1 : s_->doc.layers().back().id; loadPlaced(sk.data); }
  emit s_->layersChanged(); emit s_->pixelsChanged({}); emit s_->activeChanged();   // (before cid_: opening is not a change)
  cid_ = cid; dirty_ = false; fillLibrary();
}

}  // namespace hm
