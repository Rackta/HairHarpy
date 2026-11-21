#include "ui/Keymap.h"
#include "app/RefCanvas.h"
#include "app/PaintCanvas.h"
#include "app/PaintPanels.h"
#include "app/PaintSession.h"
#include "scene/Looks.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QImageReader>
#include <QJsonArray>
#include <QMatrix4x4>
#include <QMimeData>
#include <QScreen>
#include <QSettings>
#include <QSlider>
#include <QVector3D>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QThreadPool>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace hm {

using namespace paint;

RefCanvas::RefCanvas(QWidget* parent, QWidget* vpBox, Viewport* vp, Scene* sc) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), vpBox_(vpBox), vp_(vp), sc_(sc) {
  setAttribute(Qt::WA_TranslucentBackground); setWindowModality(Qt::ApplicationModal); setAcceptDrops(true);   // (modal: the mouse and the keys go nowhere else)
  setStyleSheet("#rcBar{background:#262626;border-bottom:2px solid #4a7fd6} #rcSide,#rcCol{background:#262626;border-left:1px solid #3a3a3a}");
  s_ = new PaintSession(this);
  canvas_ = new PaintCanvas(s_, this); canvas_->bare = true; canvas_->setMinimumSize(1, 1);
  top_ = new QFrame(this); top_->setObjectName("rcBar");
  auto* tv = new QVBoxLayout(top_); tv->setContentsMargins(8, 5, 8, 5); tv->setSpacing(4);
  auto* bar = new QHBoxLayout; bar->setSpacing(4); tv->addLayout(bar);
  auto* bar2 = new QHBoxLayout; bar2->setSpacing(6); tv->addLayout(bar2);
  title_ = new QLabel; title_->setStyleSheet("font-weight:bold;color:#9cc2ff"); bar->addWidget(title_); bar->addSpacing(8);
  auto* ins = new QPushButton(tr("🖼 Insert image…")); ins->setToolTip(tr("Pictures onto new layers of the canvas — also Ctrl+V or dropping files here")); ins->setFocusPolicy(Qt::NoFocus); ins->setProperty("primary", true); bar->addWidget(ins);
  connect(ins, &QPushButton::clicked, this, [this] { insertDialog(); });
  { auto* sep = new QLabel("│"); sep->setProperty("mut", true); bar->addWidget(sep); }
  auto* mL = new QPushButton(tr("◧→◨")); mL->setFocusPolicy(Qt::NoFocus); mL->setToolTip(tr("Symmetric copy: the left half of the active layer mirrored onto the right (across X = 0 of the world)")); bar->addWidget(mL);
  auto* mR = new QPushButton(tr("◨→◧")); mR->setFocusPolicy(Qt::NoFocus); mR->setToolTip(tr("Symmetric copy: the right half of the active layer mirrored onto the left (across X = 0 of the world)")); bar->addWidget(mR);
  connect(mL, &QPushButton::clicked, this, [this] { mirrorCopy(true); });
  connect(mR, &QPushButton::clicked, this, [this] { mirrorCopy(false); });
  bar->addStretch(1);
  recB_ = new QPushButton(tr("● Rec")); recB_->setFocusPolicy(Qt::NoFocus); recB_->setCheckable(true); recB_->hide(); bar->addWidget(recB_);
  noteB_ = new QPushButton(tr("✎ Note")); noteB_->setFocusPolicy(Qt::NoFocus); noteB_->setToolTip(tr("F9: a note in the session's record — your text and a drawing over a picture of the window")); noteB_->hide(); bar->addWidget(noteB_);
  connect(recB_, &QPushButton::clicked, this, [this] { if (recToggle_) recToggle_(); syncRec(); canvas_->setFocus(); });
  connect(noteB_, &QPushButton::clicked, this, [this] { if (noteTake_) noteTake_(); canvas_->setFocus(); });
  auto* done = new QPushButton(tr("✔ Done (Esc)")); done->setToolTip(tr("Leave the canvas: the model can be worked with again, the drawing stays in the project")); done->setFocusPolicy(Qt::NoFocus); done->setProperty("primary", true); bar->addWidget(done);
  connect(done, &QPushButton::clicked, this, [this] { leave(); });
  auto* fitB = new QPushButton(tr("Fit (F)")); fitB->setToolTip(tr("The whole canvas in the view")); fitB->setFocusPolicy(Qt::NoFocus); bar2->addWidget(fitB);
  connect(fitB, &QPushButton::clicked, canvas_, &PaintCanvas::fit);
  rotB_ = new QPushButton(tr("⟲ 0°")); rotB_->setToolTip(tr("The view's turn: Shift+Space or Alt+Space + drag, or hold R and drag (Shift — 15° steps). Click or Shift+R — straight again. Leaving the canvas puts the view back")); rotB_->setFocusPolicy(Qt::NoFocus); bar2->addWidget(rotB_);
  connect(rotB_, &QPushButton::clicked, this, [this] { canvas_->navRotateReset(); });
  auto* panB = new QPushButton(tr("⇥ Panels (Tab)")); panB->setToolTip(tr("Hide / show the tools and the reference and layer panels")); panB->setFocusPolicy(Qt::NoFocus); bar2->addWidget(panB);
  connect(panB, &QPushButton::clicked, this, [this] { panelsOn_ = !panelsOn_; side_->setVisible(panelsOn_); tw_->setVisible(panelsOn_); });
  gridB_ = new QPushButton(tr("▦ Grid")); gridB_->setCheckable(true); gridB_->setFocusPolicy(Qt::NoFocus); gridB_->setToolTip(tr("The views' grids and world axes on / off (Ctrl+') — in a split, every window's at once"));
  bar2->addWidget(gridB_); connect(gridB_, &QPushButton::clicked, this, [this] { toggleGrid(); canvas_->setFocus(); });
  { auto* sep = new QLabel("│"); sep->setProperty("mut", true); bar2->addWidget(sep); }
  splitB_ = new QPushButton(tr("◫ Split")); splitB_->setCheckable(true); splitB_->setFocusPolicy(Qt::NoFocus);
  splitB_->setToolTip(tr("The split, as the viewport's: 2, 2+1, 1+2 or 4 windows, each a fixed view (front, back, the sides, top — no perspective) with its own canvas, all at one scale and height — draw a lock in several views and see where it goes, as a drawing. Each window's view: the list in its corner. A click in a window makes it the one drawn in (its layers in the list); moving the pointer over it changes nothing. The wheel and the middle button move the view under the pointer"));
  bar2->addWidget(splitB_);
  splitLayout_ = new QComboBox; splitLayout_->setFocusPolicy(Qt::NoFocus); splitLayout_->setToolTip(tr("The split's windows"));
  splitLayout_->addItem(tr("2 side by side"), "2"); splitLayout_->addItem(tr("2 + 1 below"), "2+1"); splitLayout_->addItem(tr("1 + 2 below"), "1+2"); splitLayout_->addItem(tr("4"), "4");
  { const int i = splitLayout_->findData(QSettings().value("refcanvas/splitLayout", "2").toString()); splitLayout_->setCurrentIndex(std::max(0, i)); }
  bar2->addWidget(splitLayout_);
  connect(splitB_, &QPushButton::clicked, this, [this](bool on) { setSplit(on); canvas_->setFocus(); });
  connect(splitLayout_, &QComboBox::activated, this, [this](int) { QSettings().setValue("refcanvas/splitLayout", splitLayout_->currentData().toString()); if (splitOn_) { setSplit(false); setSplit(true); } else setSplit(true); canvas_->setFocus(); });
  bar2->addStretch(1);
  info_ = new QLabel; info_->setProperty("mut", true); info_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); tv->addWidget(info_);

  side_ = new QScrollArea(this); side_->setObjectName("rcSide"); side_->setWidgetResizable(true); side_->setFrameShape(QFrame::NoFrame); side_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* col = new QWidget; col->setObjectName("rcCol");
  buildSide(col);
  side_->setWidget(col);
  buildTools();
  follow_ = new QTimer(this); follow_->setInterval(16); connect(follow_, &QTimer::timeout, this, [this] { followView(); });
  // the view is the viewport's: the canvas stays locked to the model on screen
  // (the view under the pointer moved — in a split the other one too, without it becoming the active one)
  canvas_->navZoom = [this](const QPointF& w, double notches) { const int p = splitOn_ ? paneAt(w) : -1; const int i = p >= 0 ? p : vp_->activePane(); Camera& c = vp_->paneCam(i); const double dpr = vp_->devicePixelRatio(); const QPointF m = w - vp_->paneRect(i).topLeft(); c.wheel(m.x() * dpr, m.y() * dpr, notches); vp_->requestUpdate(); followView(); };
  canvas_->navPan = [this](const QPointF& d) { const int i = splitOn_ && navPane_ >= 0 ? navPane_ : vp_->activePane(); const double dpr = vp_->devicePixelRatio(); vp_->paneCam(i).pan(d.x() * dpr, d.y() * dpr); vp_->requestUpdate(); followView(); };
  canvas_->pressFilter = [this](const QPointF& w, Qt::MouseButton b, bool navigating) {
    navPane_ = -1; if (!splitOn_) return false;
    const int p = paneAt(w); navPane_ = p;
    if (navigating || b != Qt::LeftButton || p < 0 || p == vp_->activePane()) return false;
    setActiveSide(p); return true;                              // (a click in the other view: it is the one drawn in now — the click itself draws nothing)
  };
  canvas_->ringHere = [this](const QPointF& w) { return !splitOn_ || paneAt(w) == vp_->activePane(); };
  canvas_->viewCentre = [this] { return seenCentre(); };
  canvas_->navRotate = [this](double d, bool snap) { rollRaw_ += d; double to = snap ? std::round(rollRaw_ / 15) * 15 : rollRaw_; rollBy(to - roll_); roll_ = to; rotB_->setText(tr("⟲ %1°").arg(int(std::lround(std::remainder(roll_, 360.0))))); };
  canvas_->navRotateReset = [this] { rollBy(-roll_); roll_ = rollRaw_ = 0; rotB_->setText(tr("⟲ 0°")); };
  canvas_->navFit = [this] {
    if (s_->doc.width() == 0) return; followView(); Camera& c = vp_->camera(); const QRectF pr = vp_->paneRect(vp_->activePane()); const QRectF tex(0, 0, s_->doc.width(), s_->doc.height());
    const QRectF b = canvas_->mapping().mapRect(tex); const double k = std::min((pr.width() - (side_->isVisible() ? side_->width() : 0)) * 0.95 / std::max(1.0, b.width()), (pr.height() - top_->height()) * 0.95 / std::max(1.0, b.height()));
    c.zoom = std::clamp(c.zoom * k, 0.02, 500.0); followView();
    const QPointF to(pr.left() + (pr.width() - (side_->isVisible() ? side_->width() : 0)) / 2, pr.top() + top_->height() + (pr.height() - top_->height()) / 2);
    const QPointF d = to - canvas_->mapping().mapRect(tex).center(); canvas_->navPan(d);
  };

  // the markers over the sheet (the picture itself: the plane in the viewport)
  canvas_->drawOver = [this](QPainter& p, const QRectF& r) {
    if (splitOn_) {                                             // (the view drawn in: framed; the other one: a word on how to switch)
      p.save(); for (int i : vp_->shownPanes()) { const QRectF pr = QRectF(vp_->paneRect(i)).adjusted(1.5, 1.5, -1.5, -1.5);
        if (i == vp_->activePane()) { p.setPen(QPen(QColor(148, 102, 190), 3)); p.setBrush(Qt::NoBrush); p.drawRect(pr); }
        else { p.fillRect(pr, QColor(0, 0, 0, overOther_ ? 20 : 45)); QFont f = p.font(); f.setPointSize(10); p.setFont(f); const QRectF tr_(pr.left(), pr.bottom() - 34, pr.width(), 24);
          p.setPen(QColor(0, 0, 0, 200)); p.drawText(tr_.translated(1, 1), Qt::AlignCenter, tr("Click — draw in this view")); p.setPen(QColor(220, 200, 245)); p.drawText(tr_, Qt::AlignCenter, tr("Click — draw in this view")); } }
      p.restore(); }
    drawFloat(p, r); drawSel(p);
    if (!sc_->refMarkersShown) return;
    RefPlane* rp = plane(); const bool side = rp && rp->view != "top";
    p.setRenderHint(QPainter::Antialiasing); QFont f = p.font(); f.setBold(true); f.setPointSize(9); p.setFont(f);
    int n = 0;
    for (const RefMarker& m : sc_->refMarkers) {
      n++; auto t = toTex(m.pos); if (!t) continue; const QPointF w = canvas_->texToWidget(*t);
      if (side) { QColor g = m.color; g.setAlpha(110); p.setPen(QPen(g, 1, Qt::DashLine)); p.drawLine(QPointF(r.left(), w.y()), QPointF(r.right(), w.y())); }   // (its height: the same in every side view)
      p.setPen(QPen(QColor(0, 0, 0, 200), 4)); p.setBrush(Qt::NoBrush); p.drawEllipse(w, 7, 7);
      p.setPen(QPen(m.color, 2)); p.drawEllipse(w, 7, 7); p.drawPoint(w);
      const QString lab = QString::number(n); const QPointF at = w + QPointF(10, -8);
      p.setPen(QColor(0, 0, 0, 220)); p.drawText(at + QPointF(1, 1), lab); p.setPen(m.color); p.drawText(at, lab);
    }
  };
  // Symmetry: the mirror of a canvas point across X = 0 of the world, seen on this plane (none where it falls on itself: side views)
  canvas_->mirror = [this](const QPointF& t) -> std::optional<QPointF> {
    if (!vp_->tset.sym) return std::nullopt;
    auto w = toWorld(t); if (!w) return std::nullopt;
    auto m = toTex(Vec3(-w->x, w->y, w->z)); if (!m || std::hypot(m->x() - t.x(), m->y() - t.y()) < 1) return std::nullopt;
    return m;
  };
  canvas_->toolInput = [this](int phase, const QPointF& t, Qt::KeyboardModifiers mods) {
    if (tool_ == "move" && moveKind_ == "all") return moveAllInput(phase, t);
    if (tool_ == "select" || tool_ == "move") return xformInput(phase, t, mods);
    if (tool_ == "selection") return selInput(phase, t, mods);
    if (tool_ == "fill") { if (phase == 0) fillAt(t); return true; }
    if (tool_ == "shapes") return shapeInput(phase, t, mods);
    if (tool_ != "marker") return false;
    if (phase == 0) {
      drag_ = -1; double best = 12 / std::max(1e-6, canvas_->zoom());   // (12 px of the screen)
      for (const RefMarker& m : sc_->refMarkers) if (auto q = toTex(m.pos)) { const double d = std::hypot(q->x() - t.x(), q->y() - t.y()); if (d < best) { best = d; drag_ = m.id; } }
      if (drag_ >= 0 && (mods & Qt::ControlModifier)) {
        auto& L = sc_->refMarkers; L.erase(std::remove_if(L.begin(), L.end(), [this](const RefMarker& m) { return m.id == drag_; }), L.end()); drag_ = -1; markersChanged(); return true; }
      if (drag_ < 0) {                                     // (a new one: on the plane, at the model's depth)
        auto w = toWorld(t); RefPlane* rp = plane(); if (!w || !rp) return true;
        const Vec3 n = normal(); const Vec3 c = refs::modelBox(*sc_, vp_->refHomeCamera(*rp)).center();
        RefMarker m; m.id = sc_->nextMarkerId++; m.pos = *w + n * (c - *w).dot(n); m.color = refs::markerColor(int(sc_->refMarkers.size()));
        sc_->refMarkers.push_back(m); drag_ = m.id; markersChanged();
      }
      return true;
    }
    if (drag_ < 0) return true;
    for (RefMarker& m : sc_->refMarkers) if (m.id == drag_) if (auto w = toWorld(t)) { const Vec3 n = normal(); m.pos = *w + n * (m.pos - *w).dot(n); }   // (only along this view: its depth kept)
    markersChanged(); if (phase == 2) drag_ = -1;
    return true;
  };

  // what is drawn: onto the plane in the viewport as it is drawn (the composed picture, at most once a frame)
  connect(s_, &PaintSession::pixelsChanged, this, [this](const QRect& r) { if (cid_ < 0) return; dirty_ = true; if (r.isNull()) pendAll_ = true; else pend_ = pend_.united(r); composeSoon(); });
  connect(s_, &PaintSession::layersChanged, this, [this] { if (cid_ < 0) return; dirty_ = true; pendAll_ = true; composeSoon(); });
  connect(vp_, &Viewport::topoState, this, [this] { syncInfo(); canvas_->update(); });   // (Symmetry switched: the mirrored cursor)
  // (the selection: the brush keeps inside it; the marching ants move)
  canvas_->clip = [this](QImage& img, const QImage& before, const QRect& r) { if (hasSel()) clipToSel(img, before, r); };
  canvas_->strokeBegan = [this] { if (!canvas_->eraser()) addRecent(s_->color); };
  canvas_->hovered = [this](const QPointF& w) {
    if (polyOn_) canvas_->update();
    if (splitOn_) { const int p = paneAt(w); const bool other = p >= 0 && p != vp_->activePane();
      if (other != overOther_) { overOther_ = other; canvas_->setCursor(other ? Qt::PointingHandCursor : canvas_->restCursor); canvas_->update(); } }
  };
  canvas_->undoHook = [this](bool redo) { return xformUndo(redo); };
  canvas_->strokeEnded = [this](bool pen, float lo, float hi) {
    lastStroke_ = pen ? tr("last stroke: pen, pressure %1–%2%").arg(int(std::lround(lo * 100))).arg(int(std::lround(hi * 100))) : tr("last stroke: mouse (no pressure)"); syncInfo(); };
  canvas_->eraserKey = [this] { setTool(tool_ == "eraser" ? "brush" : "eraser"); };
  ants_ = new QTimer(this); ants_->setInterval(180); connect(ants_, &QTimer::timeout, this, [this] { antsOff_ = (antsOff_ + 1) % 8; if (open_) canvas_->update(); });
  setTool("brush");
}

RefPlane* RefCanvas::plane() const { for (auto& r : sc_->refPlanes) if (r.id == id_) return &r; return nullptr; }

bool RefCanvas::frame(M4& M, Vec3& n, int& W, int& H) const {
  RefPlane* rp0 = plane(); if (!rp0 || s_->doc.width() == 0) return false;
  RefPlane rp = *rp0; refs::apply(rp, vp_->refHomeCamera(rp)); M = rp.matrix(); n = rp.quat.rotate({0, 0, 1}).normalized();
  W = s_->doc.width(); H = s_->doc.height(); return true;
}
Vec3 RefCanvas::normal() const { M4 M; Vec3 n; int W, H; return frame(M, n, W, H) ? n : Vec3(0, 0, 1); }
std::optional<QPointF> RefCanvas::toTex(const Vec3& world) const {
  M4 M; Vec3 n; int W, H; if (!frame(M, n, W, H)) return std::nullopt;
  const Vec3 l = m4Apply(m4Inverse(M), world); return QPointF((l.x + 0.5) * W, (0.5 - l.y) * H);
}
std::optional<Vec3> RefCanvas::toWorld(const QPointF& t) const {
  M4 M; Vec3 n; int W, H; if (!frame(M, n, W, H)) return std::nullopt;
  return m4Apply(M, {t.x() / W - 0.5, 0.5 - t.y() / H, 0});
}

void RefCanvas::composeSoon() {
  static bool queued = false; if (queued) return; queued = true;
  QTimer::singleShot(16, this, [this] { queued = false; composeNow(); });
}
void RefCanvas::composeNow(bool sync) {
  if (cid_ < 0 || (!pendAll_ && pend_.isNull())) return;
  RefSketch& sk = sc_->refSketches[cid_];
  const QSize whole(s_->doc.width(), s_->doc.height());
  const bool full = pendAll_ || pend_.isNull() || sk.flat.size() != whole;
  if (full && !sync) {                                        // (the whole picture: on a worker, the latest change winning)
    pend_ = {}; pendAll_ = false;
    if (jobRunning_) jobAgain_ = true; else startJob();
    return;
  }
  if (full) { ++jobGen_; sk.flat = s_->doc.compose(Channel::Color); }   // (a worker's picture still on its way is older: dropped)
  else { const QRect r = pend_.intersected(QRect(QPoint(0, 0), whole));
    if (!r.isEmpty()) { const QImage part = s_->doc.compose(Channel::Color, r); QPainter pa(&sk.flat); pa.setCompositionMode(QPainter::CompositionMode_Source); pa.drawImage(r.topLeft(), part); }   // (compose gives only that part: put in its place)
    if (jobRunning_) jobDirty_ = jobDirty_.united(r); }        // (laid over the worker's picture again when it comes)
  pend_ = {}; pendAll_ = false;
  sk.rev++; vp_->requestUpdate(); if (changed) changed();
}
void RefCanvas::startJob() {
  jobRunning_ = true; jobAgain_ = false; jobDirty_ = {};
  const int gen = ++jobGen_, cid = cid_; const paint::Document doc = s_->doc; QPointer<RefCanvas> me(this);   // (the layers' images shared, not copied: written to here, they part)
  QThreadPool::globalInstance()->start([me, doc, gen, cid] {
    const QImage img = doc.compose(Channel::Color);
    if (me) QMetaObject::invokeMethod(me.data(), [me, img, gen, cid] { if (me) me->jobDone(img, gen, cid); }, Qt::QueuedConnection);
  });
}
void RefCanvas::jobDone(const QImage& img, int gen, int cid) {
  jobRunning_ = false;
  if (gen == jobGen_ && cid == cid_ && sc_->refSketches.count(cid)) {
    RefSketch& sk = sc_->refSketches[cid]; sk.flat = img;
    const QRect r = jobDirty_.intersected(img.rect());
    if (!r.isEmpty()) { const QImage part = s_->doc.compose(Channel::Color, r); QPainter pa(&sk.flat); pa.setCompositionMode(QPainter::CompositionMode_Source); pa.drawImage(r.topLeft(), part); }
    sk.rev++; vp_->requestUpdate(); if (changed) changed();
  }
  jobDirty_ = {};
  if (jobAgain_) { jobAgain_ = false; if (cid_ >= 0) startJob(); }
}

void RefCanvas::markersChanged() {
  sc_->refMarkRev++; vp_->requestUpdate(); canvas_->update(); syncInfo();
  if (changed) changed();
}

void RefCanvas::setTool(const QString& t) {
  if (fl_.on && t != tool_) applyFloat();   // (another tool: the floating piece put down)
  if (t != "selection") { poly_.clear(); polyOn_ = false; selDrag_ = false; }
  shaping_ = false; tool_ = t;
  canvas_->setEraser(t == "eraser");
  canvas_->ring = t == "brush" || t == "eraser";
  canvas_->restCursor = t == "move" ? Qt::SizeAllCursor : canvas_->ring ? Qt::BlankCursor : Qt::CrossCursor; canvas_->setCursor(canvas_->restCursor);
  syncModes(); canvas_->update(); syncInfo();
}

void RefCanvas::setRecHooks(std::function<void()> toggle, std::function<void()> note, std::function<bool()> on) { recToggle_ = std::move(toggle); noteTake_ = std::move(note); recOn_ = std::move(on); recB_->show(); syncRec(); }
void RefCanvas::syncRec() {
  if (!recOn_) return; const bool on = recOn_();
  { QSignalBlocker b(recB_); recB_->setChecked(on); } recB_->setText(on ? tr("■ Stop") : tr("● Rec"));
  recB_->setToolTip(on ? tr("Stop recording the session") : tr("Record this session into a folder in Documents\\HairHarpy Sessions (nothing is sent anywhere)"));
  recB_->setStyleSheet(on ? "color:#fff;background:#b02a2a;font-weight:600" : "color:#ff8080;font-weight:600"); noteB_->setVisible(on);
}

void RefCanvas::syncInfo() {
  if (show_) { QSignalBlocker b(show_); show_->setChecked(sc_->refMarkersShown); }
  const QString sym = vp_->tset.sym ? tr(" · Symmetry on: mirrored across X = 0 (Shift+S in the viewport)") : QString();
  const QString sel = hasSel() ? tr(" · Selected: the tools keep inside it — Ctrl+D none, Delete clears it") : QString();
  QString t;
  if (tool_ == "move") t = moveKind_ == "all" ? tr("✥ All layers: drag — the whole canvas moves in the view (⟲ in the reference panel — back)") : tr("✥ Drag — move the active layer (with a selection: what is in it) · Ctrl+Z undo");
  else if (tool_ == "select") t = tr("⬚ Drag a frame — the part floats · drag inside — move · corners — scale (Shift — proportions) · round handle — turn (Shift — 15°) · Enter — put down · Esc — cancel");
  else if (tool_ == "marker") t = tr("📍 Click — an anchor (%1 now) · drag — move it along this view · Ctrl+click — delete · each one is seen on every view and in the viewport").arg(sc_->refMarkers.size());
  else if (tool_ == "selection") t = selKind_ == "poly" ? tr("Polygonal lasso: click — a corner · the first corner / double click / Enter — close · Backspace — the last corner back · Esc — drop it") : selKind_ == "wand" ? tr("Magic wand: click — what matches that colour · Shift — add · Alt — subtract") : tr("Drag — select · Shift — add · Alt — subtract · a click outside — none · Ctrl+A all · Ctrl+Shift+I invert");
  else if (tool_ == "fill") t = tr("Click — fill what matches the clicked pixel with the colour · Alt — pick a colour · X — swap the colours");
  else if (tool_ == "shapes") t = tr("Drag — the shape · Shift — square, circle, 45° · the colour, the width in the settings");
  else t = tr("The model is locked while the canvas is open · Space — move · Ctrl+Space — zoom · Shift+Space or R — rotate · Alt — pick colour · Alt+RMB drag or [ ] — size · E eraser (or hold Ctrl) · X swap colours · 1…0 opacity · Shift+click — line · Ctrl+Z / Ctrl+Shift+Z · Tab panels · Esc done");
  const QString spl = splitOn_ ? tr(" · Split: a click in the other view — draw there; the wheel / middle button move the view under the pointer") : QString();
  info_->setText(t + sel + spl + sym + (lastStroke_.isEmpty() ? QString() : "  ·  " + lastStroke_));
}

QJsonObject RefCanvas::packAll() const { return packDoc(s_->doc, placed_); }
QJsonObject RefCanvas::packDoc(const paint::Document& doc, const std::map<int, Placed>& placed) const {
  QJsonObject o = doc.packJson(); QJsonArray A;
  for (auto& [lid, pl] : placed) { const paint::Layer* l = doc.layer(lid); if (!l || l->image.cacheKey() != pl.key) continue; QByteArray b; QBuffer buf(&b); buf.open(QIODevice::WriteOnly); pl.src.save(&buf, "PNG");
    A.append(QJsonObject{{"layer", lid}, {"png", QString::fromLatin1(b.toBase64())}, {"cx", pl.cx}, {"cy", pl.cy}, {"sx", pl.sx}, {"sy", pl.sy}, {"a", pl.a}}); }
  if (!A.isEmpty()) o.insert("placed", A);
  return o;
}
void RefCanvas::loadPlaced(const QJsonObject& data) {
  placed_.clear();
  for (const QJsonValue& v : data.value("placed").toArray()) { const QJsonObject o = v.toObject(); const paint::Layer* l = s_->doc.layer(o.value("layer").toInt()); if (!l) continue;
    Placed pl; pl.src.loadFromData(QByteArray::fromBase64(o.value("png").toString().toLatin1()), "PNG"); if (pl.src.isNull()) continue; pl.src = pl.src.convertToFormat(QImage::Format_ARGB32);
    pl.cx = o.value("cx").toDouble(); pl.cy = o.value("cy").toDouble(); pl.sx = o.value("sx").toDouble(1); pl.sy = o.value("sy").toDouble(1); pl.a = o.value("a").toDouble(); pl.key = l->image.cacheKey(); placed_[l->id] = pl; }
}

// the camera for the canvas: a pane already in such a view (made the active one), else the active pane turned to it (put back on leaving)
void RefCanvas::enterView(const std::function<bool(const Camera&)>& fits, const QString& view) {
  for (int i : vp_->shownPanes()) { const Camera& c = vp_->paneCamera(i); if (c.ortho() && c.view != "uv" && fits(c)) { if (!hasCam_) savedPane_ = vp_->activePane(); vp_->activatePane(i); if (!hasCam_) { savedCam_ = vp_->camera(); hasCam_ = true; } return; } }   // (its camera kept too: the view comes back exactly as it was)
  if (!hasCam_) { savedCam_ = vp_->camera(); savedPane_ = vp_->activePane(); hasCam_ = true; }
  vp_->setViewMode(view);
}

void RefCanvas::newSheet(const QString& view) {
  capturePanes();
  if (!focused_ && focusLayout) { focusLayout(true); focused_ = true; }
  enterView([&](const Camera& c) { return c.view == view; }, view);
  openFor(makeSheet(view, vp_->camera()));
}
int RefCanvas::makeSheet(const QString& view, const Camera& cam) {
  QScreen* scr = vpBox_->screen(); const double dpr = scr->devicePixelRatio(); int W = int(std::lround(scr->size().width() * dpr)), H = int(std::lround(scr->size().height() * dpr));
  if (const int big = std::max(W, H); big > 4096) { W = W * 4096 / big; H = H * 4096 / big; }
  int n = 1; for (auto& r : sc_->refPlanes) if (r.view == view) n++;
  RefPlane rp; rp.id = sc_->nextRefId++; rp.view = view; rp.name = QString("%1 %2").arg(refs::viewLabel(view)).arg(n); rp.lock = QJsonValue(QJsonValue::Null); rp.sheet = true; rp.opacity = 1;   /* (a sheet is clear where nothing is drawn — what is drawn is solid: the axes and the grid not seen through a stroke) */ rp.over = true;
  rp.canvas = refs::newCanvas(*sc_, W, H, rp.name);
  refs::placeSheet(rp, *sc_, cam, W, H);
  sc_->refPlanes.push_back(rp); vp_->refPlanesEdited(true);
  return rp.id;
}

void RefCanvas::openFor(int planeId, bool insert) {
  RefPlane* rp = nullptr; for (auto& r : sc_->refPlanes) if (r.id == planeId) rp = &r;
  if (!rp) return;
  capturePanes();
  if (!focused_ && focusLayout) { focusLayout(true); focused_ = true; }   // (the outliner and the settings column out of the way while drawing)
  if (rp->canvas < 0 || !sc_->refSketches.count(rp->canvas)) {  // (a plane from before the canvases, not drawn on yet: its proportions, the long side 2048 px)
    const double a = std::max(0.05, std::min(20.0, std::abs(rp->scl.x) / std::max(1e-9, std::abs(rp->scl.y))));
    const int W = a >= 1 ? 2048 : std::max(16, int(std::lround(2048 * a))), H = a >= 1 ? std::max(16, int(std::lround(2048 / a))) : 2048;
    rp->canvas = refs::newCanvas(*sc_, W, H, rp->name); vp_->refPlanesEdited(true);
  }
  loadCanvas(rp->canvas);
  id_ = planeId;
  const RefPlane probe = *rp; rp->visible = true;
  enterView([&](const Camera& c) { return refs::shownIn(probe, c); }, probe.view);
  wasActive_ = vp_->activeRefPlane(); if (wasActive_ >= 0) vp_->setActiveRefPlane(-1);   // (no gizmo in the way)
  hasLoc_ = rp->follows() && rp->hasLoc; if (hasLoc_) savedLoc_ = rp->loc;
  offD_ = Vec3(); offOf_.clear(); if (!open_) rollOf_.clear();
  if (!open_) {   // (the main window's other floating windows — the Tool window, the viewport's buttons, floating panels — out of the way: the canvas is modal, they could not be used)
    hidden_.clear();
    for (QWidget* w : QApplication::topLevelWidgets())
      if (w != this && w->isVisible() && w->windowType() == Qt::Tool && !qobject_cast<QLabel*>(w) && !w->testAttribute(Qt::WA_TransparentForMouseEvents) && !(w->windowFlags() & Qt::WindowTransparentForInput)) { hidden_.push_back(w); w->hide(); }
  }
  open_ = true; follow_->start(); followView();
  fillLibrary(); syncLayerProps(); syncPlaneUi(); syncGridB(); syncRec(); syncInfo(); show(); raise(); placeTools(); if (panelsOn_) { tw_->show(); tw_->raise(); } activateWindow(); canvas_->setFocus(); vp_->requestUpdate();
  if (insert) QTimer::singleShot(0, this, [this] { insertDialog(); });
}

void RefCanvas::leave() {
  if (!open_) return;
  open_ = false; follow_->stop(); flush(); hide();
  if (RefPlane* p = plane(); p && hasLoc_) p->loc = savedLoc_;   // (a plane following the camera: back where it was in the camera's frame)
  if (hasCam_) { const Camera c = savedCam_; if (vp_->camera().view != c.view) vp_->setViewMode(c.view); vp_->camera() = c; hasCam_ = false; }
  hasLoc_ = false; roll_ = rollRaw_ = 0; rotB_->setText(tr("⟲ 0°")); offOf_.clear(); rollOf_.clear();
  vp_->activatePane(savedPane_);
  if (wasActive_ >= 0 && vp_->refPlane(wasActive_)) vp_->setActiveRefPlane(wasActive_);
  poly_.clear(); polyOn_ = false; shaping_ = selDrag_ = false;
  stash_.clear();
  { bool gone = false;                                           // (the split's own new canvases nothing was drawn on: not kept)
    for (int pid : madeHere_) { auto it = std::find_if(sc_->refPlanes.begin(), sc_->refPlanes.end(), [pid](const RefPlane& r) { return r.id == pid; }); if (it == sc_->refPlanes.end()) continue;
      const int c = it->canvas; auto sk = sc_->refSketches.find(c); if (sk == sc_->refSketches.end() || sk->second.rev > 1) continue;   // (rev 1: as newCanvas made it)
      sc_->refPlanes.erase(it); if (refs::canvasUsers(*sc_, c) == 0) sc_->refSketches.erase(c); gone = true; }
    madeHere_.clear(); if (gone) vp_->refPlanesEdited(true); }
  splitOn_ = false; panePlane_.clear(); overOther_ = false; if (splitB_) splitB_->setChecked(false); placePaneBoxes(); if (tw_) tw_->hide();
  if (hasPanes_) { vp_->setPaneState(savedPanes_); hasPanes_ = false; }
  if (focused_) { if (focusLayout) focusLayout(false); focused_ = false; }
  flHist_.clear(); flRedo_.clear(); placedByKey_.clear();   // (the windows as they were before the canvas — a split it made gone)
  for (const QPointer<QWidget>& w : hidden_) if (w) w->show();   // (the floating windows back)
  hidden_.clear();
  vp_->requestUpdate(); if (changed) changed();
}

void RefCanvas::followView() {
  if (!open_) return;
  const QRect g(vpBox_->mapToGlobal(QPoint(0, 0)), vpBox_->size()); if (geometry() != g) setGeometry(g);
  RefPlane* p = plane(); if (!p || s_->doc.width() == 0) { if (!p) leave(); return; }
  RefPlane rp = *p; const Camera& cam = vp_->camera(); refs::apply(rp, cam);
  const QMatrix4x4 PV = cam.projMatrix() * cam.viewMatrix(); const QRect pr = vp_->paneRect(vp_->activePane()); const M4 M = rp.matrix();
  auto at = [&](double u, double v) { const Vec3 w = m4Apply(M, {u, v, 0}); const QVector3D n = PV.map(QVector3D(float(w.x), float(w.y), float(w.z))); return QPointF(pr.x() + (n.x() + 1) / 2 * pr.width(), pr.y() + (1 - n.y()) / 2 * pr.height()); };
  const QPointF A = at(-0.5, 0.5), B = at(0.5, 0.5), C = at(-0.5, -0.5); const double W = s_->doc.width(), H = s_->doc.height();
  canvas_->setMapping(QTransform((B.x() - A.x()) / W, (B.y() - A.y()) / W, (C.x() - A.x()) / H, (C.y() - A.y()) / H, A.x(), A.y()));
}

QPointF RefCanvas::seenCentre() const {
  const QRectF pr = vp_->paneRect(vp_->activePane()); const double side = side_->isVisible() ? side_->width() : 0;
  return QPointF(pr.left() + (pr.width() - side) / 2, pr.top() + top_->height() + (pr.height() - top_->height()) / 2);
}
// the camera rolled by deg (the picture turns that much clockwise on the screen) round the middle of what is seen
void RefCanvas::rollBy(double deg) {
  if (std::abs(deg) < 1e-9) return; Camera& c = vp_->camera(); RefPlane* p = plane(); const QRectF pr = vp_->paneRect(vp_->activePane()); const double dpr = vp_->devicePixelRatio(); const QPointF m = seenCentre() - pr.center();
  const double k = c.worldPerPixel() * dpr; const Vec3 A = c.pos + c.right() * (m.x() * k) - c.upVec() * (m.y() * k); const double th = deg * M_PI / 180; const Vec3 axis = c.quat.rotate({0, 0, 1}).normalized();
  M4 before = m4Identity(); if (p && p->follows()) before = refs::frame(*p, c);
  c.pos = A + Quat::fromAxisAngle(axis, th).rotate(c.pos - A); c.quat = (c.quat * Quat::fromAxisAngle({0, 0, 1}, th)).normalized();
  if (p && p->follows()) p->loc = m4Mul(m4Mul(m4Inverse(refs::frame(*p, c)), before), p->loc);   // (it stays on the model, the view turns)
  vp_->requestUpdate(); followView();
}

void RefCanvas::syncPlaneUi() {
  RefPlane* p = plane(); if (!p) return;
  { QSignalBlocker a(op_); op_->setValue(int(std::lround(p->opacity * 100))); opV_->setText(QString::number(op_->value()) + "%"); frontB_->setChecked(p->over); behindB_->setChecked(!p->over); }
  auto f = sc_->refSketches.find(cid_); const int users = refs::canvasUsers(*sc_, cid_);
  title_->setText(tr("✏ Canvas: %1 — %2×%3").arg(f != sc_->refSketches.end() && !f->second.name.isEmpty() ? f->second.name : p->name).arg(s_->doc.width()).arg(s_->doc.height()) + (users > 1 ? tr("  🔗 shared by %1 views").arg(users) : QString()));
}

void RefCanvas::resizeEvent(QResizeEvent*) {
  canvas_->setGeometry(rect()); const int th = top_->sizeHint().height(); top_->setGeometry(0, 0, width(), th);
  side_->setGeometry(width() - 330, th, 330, height() - th); top_->raise(); side_->raise();
  if (tw_ && tw_->isVisible()) tw_->raise();
  if (splitOn_) placePaneBoxes();
}

void RefCanvas::insertDialog() {
  QSettings st; const QStringList fs = QFileDialog::getOpenFileNames(this, tr("Images for the canvas"), st.value("refs/dir").toString(), tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"));
  if (fs.isEmpty()) return; st.setValue("refs/dir", QFileInfo(fs[0]).absolutePath());
  for (const QString& f : fs) { QImageReader r(f); r.setAutoTransform(true); const QImage im = r.read(); if (im.isNull()) ui::toast(tr("Not an image: %1").arg(QFileInfo(f).fileName())); else pasteImage(im); }
  canvas_->setFocus();
}
void RefCanvas::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasUrls() || e->mimeData()->hasImage()) e->acceptProposedAction(); }
void RefCanvas::dropEvent(QDropEvent* e) {
  bool any = false; QPointF t = canvas_->mapping().inverted().map(e->position());
  for (const QUrl& u : e->mimeData()->urls()) if (u.isLocalFile()) { QImageReader r(u.toLocalFile()); r.setAutoTransform(true); const QImage im = r.read(); if (!im.isNull() && pasteImage(im, t)) { any = true; t += QPointF(40, 40); } }
  if (!any && e->mimeData()->hasImage()) any = pasteImage(qvariant_cast<QImage>(e->mimeData()->imageData()), t);
  if (any) { e->acceptProposedAction(); activateWindow(); canvas_->setFocus(); }
}

void RefCanvas::flush() {
  if (fl_.on) applyFloat();
  for (auto& [cid, st] : stash_) if (st.dirty && sc_->refSketches.count(cid)) { RefSketch& sk = sc_->refSketches[cid]; sk.data = packDoc(st.doc, st.placed); sk.flat = st.doc.compose(Channel::Color); sk.rev++; st.dirty = false; }
  if (cid_ < 0 || !dirty_) return;
  RefSketch& sk = sc_->refSketches[cid_]; sk.data = packAll(); sk.flat = s_->doc.compose(Channel::Color); sk.rev++;
  dirty_ = false;
}
void RefCanvas::packInto(Scene& out) const {
  for (auto& [cid, st] : stash_) if (st.dirty) out.refSketches[cid].data = packDoc(st.doc, st.placed);
  if (cid_ < 0 || !dirty_) return;
  out.refSketches[cid_].data = packAll();
}
void RefCanvas::reset() {
  if (open_) for (const QPointer<QWidget>& w : hidden_) if (w) w->show();
  if (focused_) { if (focusLayout) focusLayout(false); focused_ = false; }
  hidden_.clear(); stash_.clear(); offOf_.clear(); rollOf_.clear(); madeHere_.clear(); flHist_.clear(); flRedo_.clear(); placedByKey_.clear(); splitOn_ = false; panePlane_.clear(); hasPanes_ = false; if (tw_) tw_->hide(); placePaneBoxes(); open_ = false; follow_->stop(); hasCam_ = false; wasActive_ = -1; fl_ = {}; selecting_ = false; id_ = -1; cid_ = -1; dirty_ = false; placed_.clear(); s_->undo.clear(); s_->doc.reset(0, 0); deselect(); hide(); }

void RefCanvas::closeEvent(QCloseEvent* e) { leave(); QWidget::closeEvent(e); }

void RefCanvas::keyPressEvent(QKeyEvent* e) {
  if (e->matches(QKeySequence::Paste)) { pasteClipboard(); return; }
  if (e->matches(QKeySequence::Save)) { flush(); if (save) save(); return; }
  const int k = ui::Keymap::latinKey(e); const auto m = e->modifiers();
  const bool none = m == Qt::NoModifier || m == Qt::KeypadModifier;
  // (the polygonal lasso being put: Enter closes it, Backspace takes its last corner back, Esc drops it)
  if (polyOn_) {
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { polyClose(); return; }
    if (e->key() == Qt::Key_Backspace) { if (!poly_.isEmpty()) poly_.removeLast(); if (poly_.isEmpty()) polyOn_ = false; canvas_->update(); return; }
    if (e->key() == Qt::Key_Escape) { poly_.clear(); polyOn_ = false; canvas_->update(); return; }
  }
  // (as in Photoshop: Ctrl+T transform, Ctrl+J duplicate the layer (with a selection — what is selected), Ctrl+E merge down, Ctrl+Shift+N a new layer, Ctrl+A / D / Shift+I — the selection, Tab — the panels)
  if (e->key() == Qt::Key_Tab) { panelsOn_ = !panelsOn_; side_->setVisible(panelsOn_); tw_->setVisible(panelsOn_); return; }
  // (as in Photoshop: Alt+Backspace — the selection (or the layer) filled with the colour, Ctrl+Backspace — with the second colour)
  if ((e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) && (m == Qt::AltModifier || m == Qt::ControlModifier)) { fillSelection(m == Qt::AltModifier ? s_->color : s_->color2); return; }
  if (m == Qt::ControlModifier && k == Qt::Key_T) { setTool("select"); return; }
  if (m == Qt::ControlModifier && (e->key() == Qt::Key_Apostrophe || ui::Keymap::codeOf(e) == "Quote")) { toggleGrid(); return; }
  if (m == Qt::ControlModifier && k == Qt::Key_J) { layerAction("dup"); return; }
  if (m == Qt::ControlModifier && k == Qt::Key_E) { layerAction("merge"); return; }
  if (m == (Qt::ControlModifier | Qt::ShiftModifier) && k == Qt::Key_N) { layerAction("new"); return; }
  if (m == Qt::ControlModifier && k == Qt::Key_A) { if (fl_.on) applyFloat(); selectAll(); return; }
  if (m == Qt::ControlModifier && k == Qt::Key_D) { deselect(); return; }
  if (m == (Qt::ControlModifier | Qt::ShiftModifier) && k == Qt::Key_I) { invertSel(); return; }
  if (none && (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && hasSel()) { clearSelected(); return; }
  if (none) {
    if (k == Qt::Key_M) { setTool("marker"); return; }
    if (k == Qt::Key_B) { setTool("brush"); return; }
    if (k == Qt::Key_T) { setTool("select"); return; }
    if (k == Qt::Key_V) { setTool("move"); return; }
    if (k == Qt::Key_S) { setTool("selection"); return; }
    if (k == Qt::Key_G) { setTool("fill"); return; }
    if (k == Qt::Key_U) { setTool("shapes"); return; }
  }
  if (fl_.on && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) { applyFloat(); return; }
  if (fl_.on && e->key() == Qt::Key_Escape) { cancelFloat(); return; }
  if (e->key() == Qt::Key_Escape && hasSel()) { deselect(); ui::toast(tr("Selection dropped — Esc again: done")); return; }
  if (e->key() == Qt::Key_Escape) { leave(); return; }
  e->accept();                                                 // (nothing goes on to the viewport's hotkeys)
}

}  // namespace hm
