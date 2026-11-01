#include "app/ToolPanels.h"
#include "app/BraidPanels.h"
#include "app/CrownPanels.h"
#include "app/ModPanels.h"
#include "app/ShellPanels.h"
#include <QSettings>
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QHash>
#include <QApplication>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>

namespace hm {

namespace {
// (the plain sliders — not the rows': a double click back to the value each had when first seen, Ctrl + drag finely)
class SliderAssist : public QObject {
public:
  using QObject::QObject;
  bool eventFilter(QObject* o, QEvent* e) override {
    auto* s = qobject_cast<QSlider*>(o); if (!s || s->property("srow").toBool()) return false;
    if (!s->property("def0").isValid()) s->setProperty("def0", s->value());
    if (e->type() == QEvent::MouseButtonDblClick) { s->setValue(s->property("def0").toInt()); emit s->sliderReleased(); return true; }
    if (e->type() != QEvent::MouseButtonPress && e->type() != QEvent::MouseMove && e->type() != QEvent::MouseButtonRelease) return false;
    auto* m = static_cast<QMouseEvent*>(e);
    if (e->type() == QEvent::MouseButtonPress && m->button() == Qt::LeftButton && (m->modifiers() & Qt::ControlModifier)) { s_ = s; x0_ = m->globalPosition().x(); v0_ = s->value(); return true; }
    if (s_ == s && e->type() == QEvent::MouseMove) { const double span = std::max(120, s->width()); s->setValue(int(std::lround(v0_ + (m->globalPosition().x() - x0_) / span * (s->maximum() - s->minimum()) * 0.1))); return true; }
    if (s_ == s && e->type() == QEvent::MouseButtonRelease) { s_ = nullptr; emit s->sliderReleased(); return true; }
    return false;
  }
private:
  QSlider* s_ = nullptr; double x0_ = 0, v0_ = 0;
};
}  // namespace
void installSliderAssist() { static bool done = false; if (done) return; done = true; qApp->installEventFilter(new SliderAssist(qApp)); }

SliderRow::SliderRow(const QString& label, double lo, double hi, double step, double value, std::function<QString(double)> fmt, QWidget* parent)
    : QWidget(parent), lo_(lo), hi_(hi), step_(step), fmt_(std::move(fmt)) {
  auto* h = new QHBoxLayout(this); h->setContentsMargins(0, 2, 0, 2); h->setSpacing(6);
  auto* l = new QLabel(label); l->setProperty("mut", true); l->setFixedWidth(96); if (label.isEmpty()) l->hide();   // (no name: the slider takes its room)
  s_ = new QSlider(Qt::Horizontal); s_->setRange(0, int(std::lround((hi - lo) / step))); s_->setProperty("srow", true);
  v_ = new QLineEdit; v_->setFixedWidth(48); v_->setAlignment(Qt::AlignRight | Qt::AlignVCenter); v_->setFrame(false);
  v_->setStyleSheet("QLineEdit{color:#aaa;background:transparent;border:none} QLineEdit:focus{color:#eee;background:#1b1b1b;border:1px solid #4a6fb0;border-radius:3px}");
  v_->setToolTip(tr("Type a value (Enter); Ctrl + drag — finely"));
  h->addWidget(l); h->addWidget(s_, 1); h->addWidget(v_);
  setValue(value);
  // (its default: the value it first had in this run, unless setReset says otherwise)
  static QHash<QString, double> first; const QString key = label + "|" + QString::number(lo) + "|" + QString::number(hi);
  if (!first.contains(key)) first.insert(key, value); reset_ = first.value(key); hasReset_ = true;
  s_->installEventFilter(this); v_->installEventFilter(this);
  connect(s_, &QSlider::valueChanged, this, [this](int x) { hasExact_ = false; double v = fromPos(x); v_->setText(fmt_(v)); emit changed(v); if (!s_->isSliderDown() && !fine_) emit released(v); });   // (a click on the groove, the keys: done at once)
  connect(s_, &QSlider::sliderReleased, this, [this] { emit released(this->value()); });
  connect(v_, &QLineEdit::editingFinished, this, [this] { typed(); });
}
int SliderRow::toPos(double v) const {
  if (log_ && lo_ > 0 && hi_ > lo_) return int(std::lround(std::log(std::clamp(v, lo_, hi_) / lo_) / std::log(hi_ / lo_) * s_->maximum()));
  return int(std::lround((v - lo_) / step_));
}
double SliderRow::fromPos(int p) const {
  if (log_ && lo_ > 0 && hi_ > lo_) return lo_ * std::pow(hi_ / lo_, double(p) / std::max(1, s_->maximum()));
  return lo_ + p * step_;
}
// (the number shown against the value: what a typed number is divided by — 100 when it shows percent, …)
double SliderRow::shownK() const {
  auto num = [](const QString& s) { QRegularExpressionMatch m = QRegularExpression("[-+]?[0-9]*[.,]?[0-9]+(?:[eE][-+]?[0-9]+)?").match(s); return m.hasMatch() ? m.captured(0).replace(',', '.').toDouble() : 0.0; };
  for (double ref : {hi_, lo_, (lo_ + hi_) / 2}) { if (std::abs(ref) < 1e-12) continue; const double k = num(fmt_(ref)) / ref; if (std::isfinite(k) && std::abs(k) > 1e-9) return k; }
  return 1;
}
void SliderRow::typed() {
  QRegularExpressionMatch m = QRegularExpression("[-+]?[0-9]*[.,]?[0-9]+(?:[eE][-+]?[0-9]+)?").match(v_->text());
  if (!m.hasMatch()) { v_->setText(fmt_(value())); return; }
  const double v = std::clamp(m.captured(0).replace(',', '.').toDouble() / shownK(), std::min(lo_, hi_), std::max(lo_, hi_));
  setValue(v); exact_ = v; hasExact_ = true; v_->setText(fmt_(v)); v_->clearFocus(); emit changed(v); emit released(v);
}
void SliderRow::setLog(bool on) { const double v = value(); log_ = on; if (on) s_->setRange(0, 1000); setValue(v); }
void SliderRow::setReset(double v) { reset_ = v; hasReset_ = true; }
void SliderRow::setTip(const QString& t) { setToolTip(t); s_->setToolTip(t); }
double SliderRow::value() const { return hasExact_ ? exact_ : fromPos(s_->value()); }
bool SliderRow::eventFilter(QObject* o, QEvent* e) {
  if (o == s_ && hasReset_ && e->type() == QEvent::MouseButtonDblClick) { setValue(reset_); emit changed(reset_); emit released(reset_); return true; }
  if ((o == s_ || o == v_) && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseMove || e->type() == QEvent::MouseButtonRelease)) {
    auto* m = static_cast<QMouseEvent*>(e);
    if (e->type() == QEvent::MouseButtonPress && m->button() == Qt::LeftButton && (m->modifiers() & Qt::ControlModifier)) {   // (Ctrl + drag: finely)
      fine_ = true; fineX0_ = m->globalPosition().x(); fineV0_ = log_ ? double(s_->value()) : value(); return true; }
    if (fine_ && e->type() == QEvent::MouseMove) {
      const double dx = m->globalPosition().x() - fineX0_, span = std::max(120, s_->width());
      if (log_) { QSignalBlocker b(s_); s_->setValue(int(std::lround(fineV0_ + dx / span * s_->maximum() * 0.1))); hasExact_ = false; const double v = fromPos(s_->value()); v_->setText(fmt_(v)); emit changed(v); }
      else { const double v = std::clamp(fineV0_ + dx / span * (hi_ - lo_) * 0.1, std::min(lo_, hi_), std::max(lo_, hi_)); setValue(v); exact_ = v; hasExact_ = true; emit changed(v); }
      return true; }
    if (fine_ && e->type() == QEvent::MouseButtonRelease) { fine_ = false; emit released(value()); return true; }
  }
  return QWidget::eventFilter(o, e);
}
void SliderRow::setValue(double v) { QSignalBlocker b(s_); s_->setValue(toPos(v)); hasExact_ = false; if (!v_->hasFocus()) v_->setText(fmt_(v)); }

namespace {
QLabel* h4(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); return l; }
QLabel* muted(const QString& t) { auto* l = new QLabel(t); l->setProperty("mut", true); l->setWordWrap(true); return l; }
QWidget* modes(const std::vector<std::tuple<QString, QString, QString>>& list, const QString& cur, std::function<void(const QString&)> set) {
  auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 6); h->setSpacing(4);
  for (auto& [k, lab, tip] : list) { auto* b = new QPushButton(lab); b->setCheckable(true); b->setChecked(k == cur); b->setToolTip(tip); QString kk = k; QObject::connect(b, &QPushButton::clicked, w, [set, kk] { set(kk); }); h->addWidget(b, 1); }
  return w;
}
QString pct(double v) { return QString("%1%").arg(int(std::lround(v * 100))); }
// the Curve tool's modes: Curve, Lock — drawn; Edges — a mesh's edges picked and made into curves (the Edges tool)
QWidget* curveModes(Viewport* vp) {
  const QString cur = vp->tool() == "edge" ? QString("edges") : vp->drawMode;
  return modes({{"curve", QObject::tr("Curve"), QObject::tr("Draw a plain curve: LMB — points, Esc — finish")},
                {"lock", QObject::tr("Lock"), QObject::tr("Draw a flat lock: the curve is its middle line, the circle at the cursor — its width")},
                {"edges", QObject::tr("Edges"), QObject::tr("Pick a mesh's edges (an imported or a converted mesh) and make curves of them — Path → curve, Mesh → curves & surfaces (V)")}},
               cur, [vp](const QString& k) { if (vp->drawingCurve() >= 0) vp->finishDrawing(); vp->drawMode = k; vp->setTool(k == "edges" ? "edge" : "draw"); vp->shellChanged(); emit vp->brushChanged(); });
}
}  // namespace

namespace {
QWidget* ownPanel(Viewport* vp, Scene* sc);
}  // namespace

// a cocoon (or a modifier of one) selected: the Shell modifiers menu is open whatever the tool — on top, the tool's own
// menu below it; the tool isn't switched
QWidget* buildToolPanel(Viewport* vp, Scene* sc) {
  if (vp->tool() == "guide") return buildShellPanel(vp, sc);
  bool ccSel = false;
  for (int id : vp->selection().items) if (const Item* it = sc->item(id)) {
    const Item* p = it->parent >= 0 ? sc->item(it->parent) : nullptr;
    if (it->type == ItemType::Cocoon || ((it->type == ItemType::Tip || it->type == ItemType::Even || it->type == ItemType::PInf || it->type == ItemType::Guide) && p && p->type == ItemType::Cocoon)) ccSel = true;
  }
  QWidget* own = ownPanel(vp, sc);
  // 📌 a pinned modifier menu stays with every tool (that tool's menu goes below it)
  if (vp->tool() != "modifier" && QSettings().value("pinMod").toBool()) {
    auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
    v->addWidget(buildModPanel(vp, sc));
    if (own) { auto* f = new QFrame; f->setFixedHeight(1); f->setStyleSheet("background:#4a4a4a;margin:10px 0 8px"); v->addWidget(f); v->addWidget(own); }
    own = w;
  }
  if (!ccSel) return own;
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
  v->addWidget(buildShellPanel(vp, sc));
  if (own) { auto* f = new QFrame; f->setFixedHeight(1); f->setStyleSheet("background:#4a4a4a;margin:10px 0 8px"); v->addWidget(f); v->addWidget(own); }
  return w;
}

namespace {
QWidget* ownPanel(Viewport* vp, Scene* sc) {
  const QString& t = vp->tool();
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setSpacing(4);
  if (t == "brush") {
    v->addWidget(h4(QObject::tr("◍ Brush")));
    v->addWidget(modes({{"screen", QObject::tr("Screen"), QObject::tr("Everything inside the circle on screen moves, whatever its depth")},
                        {"surface", QObject::tr("Surface"), QObject::tr("Only what is near the touched point in 3D moves (the front-most hair under the cursor)")}},
                       vp->brushMode(), [vp](const QString& k) { vp->setBrushMode(k); emit vp->brushChanged(); }));
    auto* r = new SliderRow(QObject::tr("Radius"), 8, 400, 1, vp->brushR, [](double x) { return QString("%1px").arg(int(std::lround(x))); });
    auto* hd = new SliderRow(QObject::tr("Hardness"), 0, 1, 0.01, vp->brushHard, pct);
    QObject::connect(r, &SliderRow::changed, vp, [vp](double x) { vp->brushR = x; vp->requestUpdate(); });
    QObject::connect(hd, &SliderRow::changed, vp, [vp](double x) { vp->brushHard = x; vp->requestUpdate(); });
    v->addWidget(r); v->addWidget(hd);
    v->addWidget(muted(QObject::tr("Shift — smooth. Ctrl + drag — inflate the lock, Ctrl+Alt + drag — shrink it. With curves (or a disc) selected, only they are affected. Ctrl+RMB drag — radius / hardness.")));
  } else if (t == "draw") {
    v->addWidget(h4(QObject::tr("✎ Curve")));
    v->addWidget(curveModes(vp));
    if (vp->drawMode == "lock") {
      auto* r = new SliderRow(QObject::tr("Width"), 3, 400, 1, vp->ribR, [](double x) { return QString("%1px").arg(int(std::lround(x * 2))); });
      r->setTip(QObject::tr("Width of the lock at the next point, on the screen (the circle at the cursor). Ctrl + wheel or Ctrl+RMB drag in the viewport — the same"));
      QObject::connect(r, &SliderRow::changed, vp, [vp](double x) { vp->ribR = x; vp->shellChanged(); });
      v->addWidget(r);
      v->addWidget(muted(QObject::tr("LMB — points of the lock's middle line (each gets the circle's width there), Esc — finish. Ctrl + wheel / Ctrl+RMB drag — the width. Anchors of other curves selected before (Cursor, Shift + click): two — a flat lock starts between them, as wide as they are apart; three or more (across a lock) — a volume branch starts through all of them, its cross-section their ring. The first click only starts it; on Convert its curves go on from those anchors back along their curves (a common trunk). Then select the curve: its settings — profile smoothing, tip, Convert to 2 curves & a wall; select its anchors — width and rotation there.")));
    } else v->addWidget(muted(QObject::tr("LMB — the curve's points (on the head, or on the plane through the last point facing you), Esc / Enter — finish.")));
  } else if (t == "cut") {
    v->addWidget(h4(QObject::tr("✂ Scissors")));
    v->addWidget(modes({{"curve", QObject::tr("Curve"), QObject::tr("Cut curves: drag a line across them")},
                        {"surface", QObject::tr("NURBS surface"), QObject::tr("Cut a NURBS wall in two along a curve placed on it anchor by anchor — from one of its curves to the other, or from its top / bottom edge (or anywhere on it): an end not brought to an edge is carried there")},
                        {"ends", QObject::tr("Split ends"), QObject::tr("Click a curve: from there to its tip it splits into two branches (a Y curve). Ctrl+RMB drag — how far the ends spread")}},
                       vp->cutMode, [vp, sc](const QString& k) {
                         vp->cutMode = k; if (k != "surface") vp->cancelSurfCut();
                         std::vector<int> w; for (int id : vp->selection().items) if (const Item* x = sc->item(id); x && x->type == ItemType::Surface) w.push_back(id);
                         if (k == "surface" && !vp->surfCutting() && w.size() == 1) vp->startSurfCut(w[0]);   // (a wall already selected — that one)
                         emit vp->brushChanged(); }));
    if (vp->cutMode == "ends") {
      auto* r = new SliderRow(QObject::tr("Spread"), 2, 300, 1, vp->splitR, [](double x) { return QString("%1px").arg(int(std::lround(x))); });
      r->setTip(QObject::tr("How far each end moves from where the tip was, in screen pixels (the circle at the cursor). Ctrl+RMB drag in the viewport — the same"));
      QObject::connect(r, &SliderRow::changed, vp, [vp](double x) { vp->splitR = x; vp->requestUpdate(); });
      v->addWidget(r);
      v->addWidget(muted(QObject::tr("Click a curve where its end should split: from there to the tip it becomes a Y curve — the curve and a new branch sharing its trunk; their ends spread apart (the preview shows both). A wall on one side stays with the curve, a wall on the other side goes over to the new branch; a tip shared with other curves splits too — each keeps the tip of its side. Ctrl+RMB drag — the spread (circle radius). Click near an anchor — the split starts there.")));
    } else if (vp->cutMode == "surface") {
      v->addWidget(muted(vp->surfCutting() ? QObject::tr("Cutting the selected wall.") : QObject::tr("Click the NURBS wall to cut.")));
      v->addWidget(muted(QObject::tr("Click one of the wall's curves where the cut starts — on an anchor, or a new anchor goes in there keeping the curve's shape. Click on the wall to add anchors (they stick to it), click its other curve to end. Then: drag the anchors (the ends slide along the curves), RMB on the cut — insert an anchor, double click — delete one. The cut stays on the wall; anchors it needs to follow a bent wall are added. Enter (or Shift+RMB → Cut) — cut: the wall splits in two along a new curve. While cutting, the Shift+RMB menu holds only the cut. Backspace — undo the last anchor while placing, Esc — cancel.")));
      if (vp->surfCutReady() || vp->surfCutEdgeReady()) {   // (from curve to curve — or begun on the wall / its edge: from edge to edge)
        auto* bar = new QWidget; auto* bh = new QHBoxLayout(bar); bh->setContentsMargins(0, 6, 0, 0); bh->setSpacing(4);
        auto* ok = new QPushButton(QObject::tr("✂ Cut (Enter)")); ok->setProperty("primary", true); auto* no = new QPushButton(QObject::tr("Cancel (Esc)"));
        QObject::connect(ok, &QPushButton::clicked, vp, [vp] { vp->surfCutFinish(); });
        QObject::connect(no, &QPushButton::clicked, vp, [vp] { vp->cancelSurfCut(); emit vp->brushChanged(); });
        bh->addWidget(ok, 1); bh->addWidget(no); v->addWidget(bar);
      }
    } else {
      int n = 0; QString one; for (int id : vp->selection().items) if (const Item* c = sc->item(id); c && c->type == ItemType::Curve) { n++; one = c->name; }
      v->addWidget(muted(n ? QObject::tr("To cut: %1.").arg(n == 1 ? one : QObject::tr("%1 curves").arg(n)) : QObject::tr("No curve picked yet.")));
      v->addWidget(muted(QObject::tr("RMB on a curve picks it (Shift — add more; or select curves before). Then drag a line across it with LMB → pie: Split into two curves / Split, delete the upper part / Split, delete the lower part (upper — the part whose free end is higher). The shape is kept; a wall between two curves cut by the same stroke is cut too (cut the whole lock — it stays a lock), a wall with only one of its curves cut is deleted; guide hits and modifiers follow the pieces.")));
    }
  } else if (t == "edge") {
    // (the Mesh → curves settings: kept in the app's settings)
    static bool loaded = false;
    if (!loaded) { loaded = true; QSettings st; vp->m2c.maxC = st.value("m2c/maxC", 8).toInt(); vp->m2c.angle = st.value("m2c/angle", 30).toDouble(); vp->m2c.count = st.value("m2c/count", 6).toInt(); vp->m2c.root = st.value("m2c/root", "top").toString(); }
    v->addWidget(h4(QObject::tr("✎ Curve")));
    v->addWidget(curveModes(vp));
    v->addWidget(muted(QObject::tr("On an imported or a converted mesh: LMB — an edge, double click — the edge loop up to a fork, Shift — add, Ctrl — deselect (double click — the loop, drag — a box), Ctrl+Shift — the shortest path from the last picked edge. Esc — deselect. Shift+RMB — the path → a curve.")));
    const Item* em = sc->item(vp->edges.mesh); size_t n = em ? vp->edges.list.size() : 0;
    auto* st = new QLabel(n ? QObject::tr("%1 edge%2 on %3").arg(n).arg(n == 1 ? "" : "s").arg(em->name) : QObject::tr("No edges selected")); st->setStyleSheet("color:#ccc"); v->addWidget(st);
    auto* cv = new QPushButton(QObject::tr("∿ Path → curve")); cv->setProperty("primary", true); cv->setEnabled(n > 0);
    cv->setToolTip(QObject::tr("Every chain of the selected edges becomes a curve (chains meeting at a vertex share the anchor there). New curves go into the selected group, else into \"<mesh> paths\""));
    QObject::connect(cv, &QPushButton::clicked, vp, [vp] { vp->edgesToCurves(); });
    auto* cl = new QPushButton(QObject::tr("Deselect")); cl->setEnabled(n > 0); QObject::connect(cl, &QPushButton::clicked, vp, [vp] { vp->clearEdges(); });
    auto* bar = new QWidget; auto* bh = new QHBoxLayout(bar); bh->setContentsMargins(0, 4, 0, 4); bh->setSpacing(4); bh->addWidget(cv); bh->addWidget(cl); bh->addStretch(1); v->addWidget(bar);
    v->addWidget(h4(QObject::tr("◆ Mesh → curves & surfaces")));
    v->addWidget(muted(QObject::tr("Every separate part of the mesh is taken for a lock: curves run along its edges (the creases), walls between them. Quad meshes follow their edge loops; other meshes are sliced across. Everything goes into a new group.")));
    auto save = [vp] { QSettings st; st.setValue("m2c/maxC", vp->m2c.maxC); st.setValue("m2c/angle", vp->m2c.angle); st.setValue("m2c/count", vp->m2c.count); st.setValue("m2c/root", vp->m2c.root); };
    auto* r1 = new SliderRow(QObject::tr("Max curves around"), 3, 16, 1, vp->m2c.maxC, [](double x) { return QString::number(int(x)); });
    r1->setTip(QObject::tr("A lock with this many edge lines around it (or fewer) gets a curve on every one of them; more lines — only the creases"));
    QObject::connect(r1, &SliderRow::changed, vp, [vp](double x) { vp->m2c.maxC = int(x); }); QObject::connect(r1, &SliderRow::released, vp, [save](double) { save(); });
    auto* r2 = new SliderRow(QObject::tr("Crease angle"), 5, 90, 1, vp->m2c.angle, [](double x) { return QString("%1°").arg(int(x)); });
    r2->setTip(QObject::tr("Lines where the surface bends at least this much count as creases (edges of the lock)"));
    QObject::connect(r2, &SliderRow::changed, vp, [vp](double x) { vp->m2c.angle = x; }); QObject::connect(r2, &SliderRow::released, vp, [save](double) { save(); });
    auto* r3 = new SliderRow(QObject::tr("Smooth lock: curves"), 3, 12, 1, vp->m2c.count, [](double x) { return QString::number(int(x)); });
    r3->setTip(QObject::tr("A round lock without creases (or a mesh that isn't made of quads) gets this many curves around it"));
    QObject::connect(r3, &SliderRow::changed, vp, [vp](double x) { vp->m2c.count = int(x); }); QObject::connect(r3, &SliderRow::released, vp, [save](double) { save(); });
    v->addWidget(r1); v->addWidget(r2); v->addWidget(r3);
    v->addWidget(modes({{"top", QObject::tr("Root: top end"), QObject::tr("The highest end — hair hanging down")}, {"head", QObject::tr("Root: nearest the head"), QObject::tr("The end closest to the head surface — locks sticking up")}},
                       vp->m2c.root, [vp, save](const QString& k) { vp->m2c.root = k; save(); emit vp->brushChanged(); }));
    int mm = vp->convertibleMesh();
    auto* mb = new QPushButton(mm >= 0 ? QObject::tr("◆ Convert %1").arg(sc->item(mm)->name) : QObject::tr("◆ Convert (select a mesh)")); mb->setProperty("primary", true); mb->setEnabled(mm >= 0);
    QObject::connect(mb, &QPushButton::clicked, vp, [vp, mm] { vp->meshToCurves(mm); });
    v->addWidget(mb);
  } else if (t == "lockex") {
    v->addWidget(muted(QObject::tr("Click a flat lock (curves joined by walls in a row); Shift — add one more. Then drag:\nLMB ←→ — extrude away from the head / toward it (the tip stays welded), ↑↓ — the back's segments (curves in its middle, standing out: one — a pentagon across)\nRMB ←→ — the thickness narrowing at the root, ↑↓ — narrowing toward the tip\nAnother tool or another object — the extrusion is done.")));
    const lockex::Params P = vp->lockExParams();
    auto row = [&](const QString& lab, double lo, double hi, double step, double val, std::function<QString(double)> fmt, std::function<void(lockex::Params&, double)> set) {
      auto* r = new SliderRow(lab, lo, hi, step, val, fmt);
      QObject::connect(r, &SliderRow::changed, vp, [vp, set](double x) { lockex::Params q = vp->lockExParams(); set(q, x); vp->lockExSet(q); });
      v->addWidget(r); r->setEnabled(vp->lockExActive()); };
    row(QObject::tr("Thickness"), -2, 2, 0.01, P.d, [](double x) { return QString("%1%").arg(int(std::lround(x * 100))); }, [](lockex::Params& q, double x) { q.d = x; });
    row(QObject::tr("Back segments"), 1, 16, 1, P.segs, [](double x) { return QString::number(int(x)); }, [](lockex::Params& q, double x) { q.segs = int(x); });
    row(QObject::tr("Root narrowing"), 0, 1, 0.01, P.root, [](double x) { return QString("%1%").arg(int(std::lround(x * 100))); }, [](lockex::Params& q, double x) { q.root = x; });
    row(QObject::tr("Tip narrowing"), 0.02, 1, 0.01, P.tip, [](double x) { return QString("%1%").arg(int(std::lround(x * 100))); }, [](lockex::Params& q, double x) { q.tip = x; });
  } else if (t == "braid") {
    delete w; return buildBraidPanel(vp, sc);
  } else if (t == "crown") {
    delete w; return buildCrownPanel(vp, sc);
  } else if (t == "modifier") {
    delete w; return buildModPanel(vp, sc);
  } else if (t == "cursor" || t == "move" || t == "rotate" || t == "gizmo" || (sc->room == "topo" && t.startsWith('t'))) {   // (the Topology room's tools: their options in the tools' own panel — no card here)
    delete w; return nullptr;
  } else {
    v->addWidget(h4(t));
    v->addWidget(muted(QObject::tr("This tool comes with the next parts of this rewrite.")));
  }
  v->addStretch(1);
  return w;
}
}  // namespace

}  // namespace hm
