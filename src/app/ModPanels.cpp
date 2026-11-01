#include "app/ModPanels.h"
#include "app/MeshPanels.h"
#include "app/ToolPanels.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <memory>
#include <set>

namespace hm {

namespace {
QLabel* h4(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setWordWrap(true); return l; }
QLabel* muted(const QString& t) { auto* l = new QLabel(t); l->setProperty("mut", true); l->setWordWrap(true); return l; }
QPushButton* button(const QString& t, const QString& tip = {}, bool primary = false) {
  auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); if (primary) b->setProperty("primary", true); return b;
}
QString pct(double v) { return QString("%1%").arg(int(std::lround(v * 100))); }
QString deg(double v) { return QString("%1°").arg(int(std::lround(v))); }
QString fmtLen(double v) {                                   // (JS toPrecision(3))
  if (v == 0) return "0.00";
  int e = int(std::floor(std::log10(std::abs(v))));
  if (e < -6 || e >= 3) { QString s = QString::number(v, 'e', 2); s.replace(QRegularExpression("e([+-])0*(\\d)"), "e\\1\\2"); return s; }
  return QString::number(v, 'f', std::max(0, 2 - e));
}
QWidget* hbox(std::initializer_list<QWidget*> ws, bool wrap = false) {
  auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 4, 0, 4); h->setSpacing(4);
  for (QWidget* x : ws) h->addWidget(x);
  if (!wrap) h->addStretch(1);
  return w;
}
QJsonObject P(const Item& it) { return it.raw.value("p").toObject(); }
void setP(Scene& sc, int id, const QString& k, const QJsonValue& v) { Item* it = sc.item(id); QJsonObject p = it->raw.value("p").toObject(); p.insert(k, v); it->raw.insert("p", p); }
void later(QObject* ctx, std::function<void()> f) { QTimer::singleShot(0, ctx, std::move(f)); }   // (after the widget's own event: the panels are rebuilt)

std::optional<tube::Clip> modClip;                            // Copy / Paste modifier
std::set<int> secOpen{0};                                     // the open profile sections of the modifier's settings
}  // namespace

// ---------- the profile editor ----------
ProfileEditor::ProfileEditor(std::function<tube::Profile*()> get, std::function<void()> onEdit, std::function<void()> onDone, Opts opts, QWidget* parent)
    : QWidget(parent), get_(std::move(get)), onEdit_(std::move(onEdit)), onDone_(std::move(onDone)), opts_(std::move(opts)) {
  setFixedSize(238, 188); setMouseTracking(true); setCursor(Qt::CrossCursor); setContextMenuPolicy(Qt::PreventContextMenu);
  setToolTip(tr("LMB — move a point (Shift — snap) · RMB — add a point · double click — delete a point") +
             (opts_.glue ? tr(" · bring a point onto its neighbour — they glue (one point); Ctrl + drag — pull one apart") : QString()));
}
namespace { const double SC = std::min(238 / 2.5, 188 / 2.4); }
QPointF ProfileEditor::toS(const std::array<double, 2>& q) const { return {238 / 2.0 + q[0] * SC, 188 / 2.0 - q[1] * SC}; }
std::array<double, 2> ProfileEditor::toP(const QPointF& s) const { return {clamp((s.x() - 238 / 2.0) / SC, -1.2, 1.2), clamp((188 / 2.0 - s.y()) / SC, -1.1, 1.1)}; }
int ProfileEditor::nearest(const QPointF& s) const {
  int bi = -1; double bd = 9; const tube::Profile* pts = get_();
  for (size_t i = 0; pts && i < pts->size(); i++) { QPointF p = toS((*pts)[i]); double d = std::hypot(p.x() - s.x(), p.y() - s.y()); if (d < bd) { bd = d; bi = int(i); } }
  return bi;
}
void ProfileEditor::paintEvent(QPaintEvent*) {
  QPainter g(this); g.setRenderHint(QPainter::Antialiasing);
  g.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a)); g.setPen(QColor(0x3a, 0x3a, 0x3a)); g.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 4, 4);
  for (double v = -1; v <= 1.001; v += 0.5) {
    g.setPen(QPen(std::abs(v) < 1e-6 ? QColor(0x4a, 0x4a, 0x4a) : QColor(0x2c, 0x2c, 0x2c), 1));
    double x = toS({v, 0}).x(), y = toS({0, v}).y();
    g.drawLine(QPointF(x, 0), QPointF(x, height())); g.drawLine(QPointF(0, y), QPointF(width(), y));
  }
  QFont f = font(); f.setPixelSize(10); g.setFont(f); g.setPen(QColor(0x6d, 0x6d, 0x6d));
  g.drawText(QPointF(4, 12), tr("↑ outwards")); g.drawText(QPointF(width() - 104, height() - 5), tr("← across the lock →"));
  g.setPen(Qt::NoPen); g.setBrush(QColor(0xc9, 0xb8, 0xff)); g.drawEllipse(QPointF(width() / 2.0, height() / 2.0), 2.5, 2.5);   // (the curve passes here)
  const tube::Profile* pts = get_(); if (!pts) return;
  if (!pts->empty()) {
    QPainterPath path; for (size_t i = 0; i < pts->size(); i++) { QPointF p = toS((*pts)[i]); if (i) path.lineTo(p); else path.moveTo(p); } path.closeSubpath();
    g.setBrush(QColor(143, 120, 224, 71)); g.setPen(QPen(QColor(0xb9, 0xa3, 0xff), 1.5)); g.drawPath(path);
  }
  for (size_t i = 0; i < pts->size(); i++) {
    QPointF p = toS((*pts)[i]); bool glued = opts_.glue && tube::gluedRun(*pts, int(i)).size() > 1, hot = int(i) == dragI_ || int(i) == hoverI_;
    if (glued) { g.setBrush(Qt::NoBrush); g.setPen(QPen(QColor(0x5f, 0xd0, 0xff), 2)); g.drawEllipse(p, 8, 8); }   // (glued points: a blue ring)
    g.setBrush(int(i) == dragI_ ? QColor(0xff, 0x9a, 0x2e) : int(i) == hoverI_ ? QColor(0xff, 0xd2, 0x7a) : tube::profColor(int(i)));
    g.setPen(QPen(hot ? QColor(0x11, 0x11, 0x11) : QColor(0xe8, 0xe8, 0xe8), 1)); double r = hot ? 6 : 5; g.drawEllipse(p, r, r);
  }
}
void ProfileEditor::mousePressEvent(QMouseEvent* e) {
  tube::Profile* pts = get_(); if (!pts) return;
  QPointF s = e->position();
  auto snap = [&](std::array<double, 2> q) { if (e->modifiers() & Qt::ShiftModifier) for (double& v : q) v = std::round(v * 20) / 20; return q; };
  if (e->button() == Qt::LeftButton) {
    dragI_ = nearest(s);
    if (dragI_ >= 0) { group_ = opts_.glue && !(e->modifiers() & Qt::ControlModifier) ? tube::gluedRun(*pts, dragI_) : std::vector<int>{dragI_}; update(); }   // (glued points move together; Ctrl — this one only)
    return;
  }
  if (e->button() == Qt::RightButton) {                       // a new point on the nearest edge
    int bi = 0; double bd = 1e300, bt = 0.5;
    for (size_t i = 0; i < pts->size(); i++) {
      QPointF a = toS((*pts)[i]), b = toS((*pts)[(i + 1) % pts->size()]); double vx = b.x() - a.x(), vy = b.y() - a.y(), l2 = vx * vx + vy * vy;
      double t = l2 > 0 ? clamp(((s.x() - a.x()) * vx + (s.y() - a.y()) * vy) / l2, 0, 1) : 0, d = std::hypot(a.x() + vx * t - s.x(), a.y() + vy * t - s.y());
      if (d < bd) { bd = d; bi = int(i); bt = t; }
    }
    auto q = snap(toP(s));
    if (opts_.onInsert) opts_.onInsert(bi, bt, q); else pts->insert(pts->begin() + bi + 1, q);
    hoverI_ = bi + 1; update(); onEdit_(); later(this, onDone_);
  }
}
void ProfileEditor::mouseMoveEvent(QMouseEvent* e) {
  tube::Profile* pts = get_(); if (!pts) return;
  QPointF s = e->position();
  if (dragI_ >= 0) {
    const int n = int(pts->size()); auto q = toP(s);
    if (e->modifiers() & Qt::ShiftModifier) for (double& v : q) v = std::round(v * 20) / 20;
    // glue: the neighbours of the moved run (before its first, after its last point) within 8 px take it over
    if (opts_.glue && !(e->modifiers() & Qt::ControlModifier)) {
      QPointF x = toS(q);
      for (int k : {(group_.front() - 1 + n) % n, (group_.back() + 1) % n}) {
        if (std::find(group_.begin(), group_.end(), k) != group_.end()) continue;
        QPointF kp = toS((*pts)[size_t(k)]); if (std::hypot(kp.x() - x.x(), kp.y() - x.y()) < 8) { q = (*pts)[size_t(k)]; break; }
      }
    }
    for (int i : group_) (*pts)[size_t(i)] = q;
    update(); onEdit_(); return;
  }
  int h = nearest(s); if (h != hoverI_) { hoverI_ = h; update(); }
}
void ProfileEditor::mouseReleaseEvent(QMouseEvent*) {
  if (dragI_ < 0) return;
  dragI_ = -1; group_.clear(); update(); later(this, onDone_);
}
void ProfileEditor::mouseDoubleClickEvent(QMouseEvent* e) {
  tube::Profile* pts = get_(); if (!pts) return;
  int i = nearest(e->position()); if (i < 0) return;
  if (pts->size() <= 3) { ui::toast(tr("A profile needs at least 3 points")); return; }
  if (opts_.onDelete) opts_.onDelete(i); else pts->erase(pts->begin() + i);
  hoverI_ = -1; dragI_ = -1; update(); onEdit_(); later(this, onDone_);
}
void ProfileEditor::leaveEvent(QEvent*) { if (hoverI_ >= 0 && dragI_ < 0) { hoverI_ = -1; update(); } }

// ---------- the Modifier tool's window ----------
void brushFor(Viewport* vp, const QString& kind) {
  vp->maskBrush.on = true; vp->maskBrush.kind = kind; vp->modTab = "extra";
  if (vp->tool() != "modifier") vp->setTool("modifier");
  emit vp->brushChanged();
}

namespace {
// the extras of the selected modifiers, as rows that can be dragged onto a mask's "Affects only" slot
class ExtraRow : public QWidget {
public:
  ExtraRow(int id, const QString& text, bool sel, std::function<void()> onSelect) : id_(id) {
    auto* h = new QHBoxLayout(this); h->setContentsMargins(6, 2, 4, 2); h->setSpacing(4);
    auto* l = new QLabel(text); h->addWidget(l, 1);
    auto* b = button(QObject::tr("Select")); QObject::connect(b, &QPushButton::clicked, this, [onSelect] { onSelect(); }); h->addWidget(b);
    setCursor(Qt::OpenHandCursor); setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(sel ? "background:#3a5585;border:1px solid #6f9fe8;border-radius:3px" : "background:#333;border-radius:3px");
  }
protected:
  void mousePressEvent(QMouseEvent* e) override { start_ = e->position(); }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (!(e->buttons() & Qt::LeftButton) || (e->position() - start_).manhattanLength() < 6) return;
    auto* d = new QDrag(this); auto* md = new QMimeData; md->setText(QString("xmod:%1").arg(id_)); d->setMimeData(md); d->exec(Qt::CopyAction);
  }
private:
  int id_; QPointF start_;
};
}  // namespace

QWidget* buildModPanel(Viewport* vp, Scene* sc) {
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setSpacing(4); v->setContentsMargins(0, 0, 0, 0);
  // header with the pin: a pinned modifier menu stays with every tool (that tool's menu goes below it)
  bool pinned = QSettings().value("pinMod").toBool();
  auto* hd = h4(QObject::tr("〰 Curve modifiers")); auto* pin = button("📌", pinned ? QObject::tr("Unpin the modifier menu") : QObject::tr("Pin the modifier menu — it stays in the viewport with the other tools"));
  pin->setFlat(true); pin->setStyleSheet(pinned ? "" : "color:#777");
  QObject::connect(pin, &QPushButton::clicked, vp, [vp, pinned] { QSettings().setValue("pinMod", !pinned); emit vp->brushChanged(); });
  auto* hrow = new QWidget; auto* hh = new QHBoxLayout(hrow); hh->setContentsMargins(0, 0, 0, 0); hh->addWidget(hd, 1); hh->addWidget(pin); v->addWidget(hrow);
  // tabs: Profile (a cross-section swept along a curve) and Extra (on top of it)
  if (vp->modTab != "extra") vp->modTab = "profile";
  auto* tabs = new QWidget; auto* th = new QHBoxLayout(tabs); th->setContentsMargins(0, 0, 0, 6); th->setSpacing(4);
  for (auto [k, lab, tip] : {std::tuple<QString, QString, QString>{"profile", QObject::tr("Profile"), QObject::tr("Sweep a cross-section profile along a curve")},
                             {"extra", QObject::tr("Extra"), QObject::tr("Mask, Thickness, Twist, Noise, Curls — on top of a profile modifier")}}) {
    auto* b = new QPushButton(lab); b->setCheckable(true); b->setChecked(vp->modTab == k); b->setToolTip(tip);
    QObject::connect(b, &QPushButton::clicked, vp, [vp, k = k] { vp->modTab = k; vp->shellChanged(); emit vp->brushChanged(); });
    th->addWidget(b, 1);
  }
  v->addWidget(tabs);

  if (vp->modTab == "extra") {
    auto ts = tube::modTubes(*sc, vp->selection());
    v->addWidget(muted(ts.empty() ? QObject::tr("Select a profile modifier (or its curve), then add:")
                                  : QObject::tr("Add to %1:").arg(ts.size() == 1 ? sc->item(ts[0])->name : QObject::tr("%1 modifiers").arg(ts.size()))));
    auto* grid = new QWidget; auto* gl = new QGridLayout(grid); gl->setContentsMargins(0, 0, 0, 6); gl->setSpacing(4);
    const char* kinds[] = {"mask", "thick", "twist", "noise", "curl"};
    const QString tips[] = {QObject::tr("A mask painted along the lock: where it is 100% the extra modifiers do nothing"), QObject::tr("Paint where the lock gets fatter or thinner"),
                            QObject::tr("Paint where the profile turns: clockwise or anticlockwise (seen from the root along the lock)"), QObject::tr("Sine-like noise along the lock"),
                            QObject::tr("Twist the lock or coil it into a volume curl")};
    for (int i = 0; i < 5; i++) {
      QString k = kinds[i]; auto* b = button("+ " + tube::xkindIcon(k) + " " + tube::xkindName(k), tips[i]); b->setEnabled(!ts.empty());
      QObject::connect(b, &QPushButton::clicked, vp, [vp, k] {
        vp->runEdit([k](Scene& s, Selection& sl, const CurveCache& c) { return tube::addExtra(s, c, sl, k); }, true);
        if (tube::painted(k)) { vp->maskBrush.on = true; vp->maskBrush.kind = k; emit vp->brushChanged(); }
      });
      gl->addWidget(b, i / 2, i % 2);
    }
    v->addWidget(grid);
    // the brushes of the kinds there are
    std::vector<QString> have; for (const char* k : {"mask", "thick", "twist"}) for (auto& [id, it] : sc->items) if (it.type == ItemType::XMod && it.raw.value("kind").toString() == k) { have.push_back(k); break; }
    if (!have.empty()) {
      auto& mb = vp->maskBrush; if (std::find(have.begin(), have.end(), mb.kind) == have.end()) mb.kind = have[0];
      auto* bb = new QWidget; auto* bh = new QHBoxLayout(bb); bh->setContentsMargins(0, 0, 0, 0); bh->setSpacing(4);
      for (const QString& k : have) {
        auto* b = new QPushButton("🖌 " + tube::xkindName(k) + QObject::tr(" brush")); b->setCheckable(true); b->setChecked(mb.on && mb.kind == k);
        b->setToolTip(k == "mask" ? QObject::tr("LMB — paint the mask (black), Ctrl — erase, Shift — smooth. With modifiers selected only they are painted")
                      : k == "twist" ? QObject::tr("LMB — paint the twist (green — clockwise, pink — anticlockwise), Ctrl — the other way, Ctrl+Alt — erase to 0, Shift — smooth")
                      : QObject::tr("LMB — paint the intensity (orange — fatter, blue — thinner), Ctrl — inverted, Ctrl+Alt — erase to 0, Shift — smooth"));
        QObject::connect(b, &QPushButton::clicked, vp, [vp, k] { auto& m = vp->maskBrush; if (m.on && m.kind == k) m.on = false; else { m.on = true; m.kind = k; } vp->shellChanged(); emit vp->brushChanged(); });
        bh->addWidget(b, 1);
      }
      v->addWidget(bb);
      if (mb.on) {
        auto* md = new QWidget; auto* mh = new QHBoxLayout(md); mh->setContentsMargins(0, 6, 0, 6); mh->setSpacing(4);
        for (auto [k, lab, tip] : {std::tuple<QString, QString, QString>{"add", QObject::tr("Add"), QObject::tr("Every stroke adds up")}, {"abs", QObject::tr("Absolute"), QObject::tr("The weight is set to the brush value")}}) {
          auto* b = new QPushButton(lab); b->setCheckable(true); b->setChecked(mb.mode == k); b->setToolTip(tip);
          QObject::connect(b, &QPushButton::clicked, vp, [vp, k = k] { vp->maskBrush.mode = k; emit vp->brushChanged(); }); mh->addWidget(b, 1);
        }
        v->addWidget(md);
        bool thick = mb.kind == "thick", twist = mb.kind == "twist";
        double cur = thick ? mb.intensity : twist ? mb.twist : mb.strength;
        auto fmt = [thick, twist](double x) {
          if (thick) return (x > 0 ? QString("+") : QString()) + QString("%1%").arg(int(std::lround(x * 100)));
          if (twist) return QString(x > 0 ? "↻" : x < 0 ? "↺" : "") + QString::number(std::abs(int(std::lround(x * 100))));
          return pct(x); };
        auto* r = new SliderRow(thick ? QObject::tr("Intensity") : QObject::tr("Strength"), thick || twist ? -1 : 0, 1, 0.01, cur, fmt);
        if (thick || twist) { r->setReset(0); r->setTip(thick ? QObject::tr("−100% — thinner (to a point) … +100% — twice as thick. Double-click — 0") : QObject::tr("↺100 — anticlockwise … ↻100 — clockwise (by the twist's angle at 100%). Double-click — 0")); }
        QObject::connect(r, &SliderRow::changed, vp, [vp, thick, twist](double x) { auto& m = vp->maskBrush; (thick ? m.intensity : twist ? m.twist : m.strength) = x; });
        v->addWidget(r);
        auto* rr = new SliderRow(QObject::tr("Radius"), 8, 400, 1, vp->brushR, [](double x) { return QString("%1px").arg(int(std::lround(x))); });
        QObject::connect(rr, &SliderRow::changed, vp, [vp](double x) { vp->brushR = x; vp->requestUpdate(); });
        v->addWidget(rr);
        if (thick || twist) v->addWidget(muted(QObject::tr("To erase: Ctrl+Alt + LMB brings the %1 back to 0 under the brush (or Absolute mode with %2 0). Clear in the modifier's properties erases it all; deleting the modifier in the outliner removes it.")
                                                 .arg(twist ? QObject::tr("twist") : QObject::tr("thickness"), twist ? QObject::tr("Strength") : QObject::tr("Intensity"))));
      }
    }
    std::vector<int> list; for (int t : ts) for (int x : tube::extrasOf(*sc, t)) list.push_back(x);
    if (!list.empty()) {
      v->addWidget(muted(QObject::tr("Extras (drag one onto a mask's \"Affects only\" slot):")));
      for (int x : list) { const Item* xi = sc->item(x);
        v->addWidget(new ExtraRow(x, tube::xkindIcon(xi->raw.value("kind").toString()) + " " + xi->name, vp->selection().hasItem(x), [vp, x] { vp->setSelectedItems({x}); })); }
    }
    return w;
  }

  // Profile: presets, the editor, Save / Delete preset, Apply
  if (vp->modProfile.empty()) { auto p = tube::profileByName(vp->modPreset); vp->modProfile = p ? *p : *tube::profileByName("Quad"); }
  auto* sel = new QComboBox; sel->addItem(QObject::tr("— edited —"), QString());
  sel->insertSeparator(1); for (auto& p : tube::builtinProfiles()) sel->addItem(p.name, p.name);
  auto ups = tube::userProfiles(); if (!ups.empty()) { sel->insertSeparator(sel->count()); for (auto& p : ups) sel->addItem(p.name, p.name); }
  int at = !vp->modPreset.isEmpty() && tube::profileByName(vp->modPreset) ? sel->findData(vp->modPreset) : 0; sel->setCurrentIndex(std::max(0, at));
  auto* prow = new QWidget; auto* ph = new QHBoxLayout(prow); ph->setContentsMargins(0, 2, 0, 2); auto* pl = new QLabel(QObject::tr("Profile")); pl->setProperty("mut", true); pl->setFixedWidth(96);
  ph->addWidget(pl); ph->addWidget(sel, 1); v->addWidget(prow);
  auto* ed = new ProfileEditor([vp] { return &vp->modProfile; }, [vp, sel] { vp->modPreset.clear(); QSignalBlocker b(sel); sel->setCurrentIndex(0); }, [] {});
  QObject::connect(sel, &QComboBox::currentIndexChanged, vp, [vp, sel, ed](int) { QString n = sel->currentData().toString(); if (n.isEmpty()) return; vp->modPreset = n; vp->modProfile = *tube::profileByName(n); ed->update(); });
  v->addWidget(ed);
  auto* save = button(QObject::tr("Save preset"), QObject::tr("Save this profile as your own preset"));
  QObject::connect(save, &QPushButton::clicked, vp, [vp, w] {
    bool builtin = false; for (auto& p : tube::builtinProfiles()) if (p.name == vp->modPreset) builtin = true;
    bool ok = false; QString name = QInputDialog::getText(w, QObject::tr("Save preset"), QObject::tr("Preset name"), QLineEdit::Normal, !vp->modPreset.isEmpty() && !builtin ? vp->modPreset : QObject::tr("My profile"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    for (auto& p : tube::builtinProfiles()) if (p.name == name) { ui::toast(QObject::tr("That name belongs to a built-in preset")); return; }
    auto l = tube::userProfiles(); l.erase(std::remove_if(l.begin(), l.end(), [&](auto& p) { return p.name == name; }), l.end()); l.push_back({name, vp->modProfile}); tube::saveUserProfiles(l);
    vp->modPreset = name; emit vp->brushChanged(); ui::toast(QObject::tr("Preset \"%1\" saved").arg(name));
  });
  auto* del = button(QObject::tr("Delete preset"), QObject::tr("Delete the selected preset of your own (built-in presets stay)"));
  QObject::connect(del, &QPushButton::clicked, vp, [vp, sel] {
    QString name = sel->currentData().toString(); bool builtin = false; for (auto& p : tube::builtinProfiles()) if (p.name == name) builtin = true;
    if (name.isEmpty() || builtin) { ui::toast(QObject::tr("Pick one of your own presets to delete")); return; }
    auto l = tube::userProfiles(); l.erase(std::remove_if(l.begin(), l.end(), [&](auto& p) { return p.name == name; }), l.end()); tube::saveUserProfiles(l);
    vp->modPreset.clear(); emit vp->brushChanged(); ui::toast(QObject::tr("Preset \"%1\" deleted").arg(name));
  });
  v->addWidget(hbox({save, del}));
  size_t n = tube::modTargets(*sc, vp->selection(), vp->hair().curves).size();
  auto* ap = button(n > 1 ? QObject::tr("Apply to %1 curves").arg(n) : QObject::tr("Apply"), QObject::tr("Sweep this profile along the selected curve(s): a temporary surface you can shape, then convert"), true);
  QObject::connect(ap, &QPushButton::clicked, vp, [vp] { auto prof = vp->modProfile; vp->runEdit([prof](Scene& s, Selection& sl, const CurveCache& c) { return tube::applyModifier(s, c, sl, prof); }, true); });
  v->addWidget(ap);
  v->addWidget(muted(QObject::tr("Draw a curve (C), select it, press Apply. The modifier's radius, rotation and tip are in the properties panel; Convert there makes curves and walls.")));
  return w;
}

// ---------- the modifier's settings ----------
void tubeProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  Item* it = sc->item(id); const int src = it->raw.value("src").toInt(-1); const Item* c = sc->item(src);
  v->addWidget(muted(QObject::tr("Profile swept along %1. Edit the curve — the surface follows.").arg(c ? c->name : QObject::tr("(deleted curve)"))));
  tube::ensureTubeRef(*sc, vp->hair().curves, *it);
  QJsonObject p = P(*it);
  double r0 = p.value("r0").toDouble(), r1 = p.value("r1").toDouble(), r2 = p.value("r2").toDouble();
  double ref = p.value("rRef").toDouble(); if (!ref) ref = std::max({r0, r1, r2}); if (!ref) ref = 0.01;
  double rmax = std::max(8 * ref, 1.05 * std::max({r0, r1, r2})), rs = rmax / 1600;
  // three sections — start, middle, end — each with its radius, rotation and profile. A point added in one profile goes
  // into the profiles after it at the same place on that edge, and into the ones before it glued to its neighbour (the
  // lock forks where it comes apart); a deleted point goes from all three
  auto Pr = std::make_shared<std::array<tube::Profile, 3>>(tube::tubeProfs(*it));
  auto write = [vp, sc, id, Pr] { Item* t = sc->item(id); if (!t) return; QJsonArray a; for (auto& x : *Pr) a.append(tube::profileJson(x)); t->raw.insert("profs", a); t->raw.insert("profile", tube::profileJson((*Pr)[0])); vp->liveTubes(); };
  if (!it->raw.value("profs").isArray() || it->raw.value("profs").toArray().size() != 3) { QJsonArray a; for (auto& x : *Pr) a.append(tube::profileJson(x)); it->raw.insert("profs", a); }   // (ownProfs)
  auto eds = std::make_shared<std::vector<ProfileEditor*>>(3, nullptr);
  auto redrawAll = [eds] { for (auto* e : *eds) if (e) e->update(); };
  auto slider = [vp, sc, id](QVBoxLayout* box, const QString& label, double lo, double hi, double step, const QString& key, std::function<QString(double)> fmt) {
    auto* r = new SliderRow(label, lo, hi, step, P(*sc->item(id)).value(key).toDouble(), fmt);
    QObject::connect(r, &SliderRow::changed, vp, [vp, sc, id, key](double x) { setP(*sc, id, key, x); vp->liveTubes(); });
    QObject::connect(r, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    box->addWidget(r); return r;
  };
  const QString names[3] = {QObject::tr("Start"), QObject::tr("Middle"), QObject::tr("End")};
  for (int k = 0; k < 3; k++) {
    auto* sec = new QFrame; sec->setStyleSheet("QFrame#sec{border:1px solid #3a3a3a;border-radius:4px}"); sec->setObjectName("sec");
    auto* sv = new QVBoxLayout(sec); sv->setContentsMargins(6, 4, 6, 4); sv->setSpacing(2);
    auto* hd = new QLabel(names[k].toUpper()); hd->setStyleSheet("color:#cfcfcf;font-weight:600;font-size:11px"); sv->addWidget(hd);
    slider(sv, QObject::tr("Radius"), 0, rmax, rs, QString("r%1").arg(k), fmtLen);
    slider(sv, QObject::tr("Rotation"), -180, 180, 1, QString("rot%1").arg(k), deg);
    bool open = secOpen.count(k);
    auto* tg = new QPushButton((open ? "▾ " : "▸ ") + QObject::tr("Profile")); tg->setFlat(true); tg->setStyleSheet("color:#b9a3ff;text-align:left;padding:0");
    tg->setToolTip(QObject::tr("Show / hide the profile of this part of the lock"));
    QObject::connect(tg, &QPushButton::clicked, vp, [vp, k, open] { if (open) secOpen.erase(k); else secOpen.insert(k); emit vp->brushChanged(); });
    sv->addWidget(tg, 0, Qt::AlignLeft);
    if (open) {
      ProfileEditor::Opts o; o.glue = true;
      o.onInsert = [Pr, k](int bi, double s, std::array<double, 2> q) {
        for (int i = 0; i < 3; i++) { auto& A = (*Pr)[size_t(i)]; auto a = A[size_t(bi)], b = A[(size_t(bi) + 1) % A.size()];
          std::array<double, 2> nq = i == k ? q : i > k ? std::array<double, 2>{a[0] + (b[0] - a[0]) * s, a[1] + (b[1] - a[1]) * s} : (s < 0.5 ? a : b);
          A.insert(A.begin() + bi + 1, nq); }
      };
      o.onDelete = [Pr](int i) { for (auto& A : *Pr) A.erase(A.begin() + i); };
      auto* ed = new ProfileEditor([Pr, k] { return &(*Pr)[size_t(k)]; }, [write, redrawAll] { write(); redrawAll(); }, [vp] { vp->commitLive(); }, o);
      (*eds)[size_t(k)] = ed; sv->addWidget(ed);
      sv->addWidget(muted(QObject::tr("Ctrl + drag — unglue a point from its neighbour")));
      if (k > 0) {
        auto* cp = button(QObject::tr("= %1 profile").arg(k == 1 ? QObject::tr("Start") : QObject::tr("Middle")), QObject::tr("This profile becomes the same as the %1 one").arg(k == 1 ? QObject::tr("start") : QObject::tr("middle")));
        QObject::connect(cp, &QPushButton::clicked, vp, [vp, Pr, k, write, redrawAll] { (*Pr)[size_t(k)] = (*Pr)[size_t(k - 1)]; write(); redrawAll(); vp->commitLive(); });
        sv->addWidget(cp, 0, Qt::AlignLeft);
      }
    }
    v->addWidget(sec);
  }
  v->addWidget(muted(QObject::tr("Profile: RMB — add a point (it goes into the next profiles too, and glued into the earlier ones), double click — delete it (from all three). Bring a point onto its neighbour — they glue; Ctrl + drag — pull them apart. Where glued points come apart along the lock, Convert makes a Y fork.")));
  auto* cb = new QCheckBox(QObject::tr("Collapse the tip")); cb->setChecked(p.value("collapse").toBool()); cb->setToolTip(QObject::tr("The end of the lock narrows to one point (on by default)"));
  QObject::connect(cb, &QCheckBox::toggled, vp, [vp, id](bool x) { vp->runEdit([id, x](Scene& s, Selection&, const CurveCache&) { setP(s, id, "collapse", x); return QString(); }, false); vp->liveTubes(); });
  v->addWidget(cb);
  if (p.value("collapse").toBool()) { auto* tl = slider(v, QObject::tr("Tip length"), 0.02, 0.8, 0.01, "tipLen", pct); tl->setTip(QObject::tr("Share of the length over which the tip narrows")); if (!p.contains("tipLen")) tl->setValue(0.25); }
  auto* cv = button(QObject::tr("Convert to curves & walls"), QObject::tr("A curve along every profile corner, NURBS walls between them (a closed volume lock); the modifier and its source curve are deleted"), true);
  QObject::connect(cv, &QPushButton::clicked, vp, [vp, id] { Vec3 cen = vp->camera().sceneCenter; vp->runEdit([id, cen](Scene& s, Selection& sl, const CurveCache& cc) { return tube::convertTube(s, cc, sl, id, cen); }, true); });
  auto* ar = button(QObject::tr("Auto radius"), QObject::tr("Radius from the width of your existing locks (or the curve length); the sliders are re-centred on it"));
  QObject::connect(ar, &QPushButton::clicked, vp, [vp, id] { vp->runEdit([id](Scene& s, Selection&, const CurveCache& cc) { setP(s, id, "rRef", 0); tube::ensureTubeRef(s, cc, *s.item(id)); return QString(); }, false); vp->liveTubes(); });
  auto* scb = button(QObject::tr("Select curve")); QObject::connect(scb, &QPushButton::clicked, vp, [vp, src, c] { if (c) vp->setSelectedItems({src}); });
  auto* cp = button(QObject::tr("Copy modifier"), QObject::tr("Copy the profile and all settings, to paste onto other curves"));
  QObject::connect(cp, &QPushButton::clicked, vp, [vp, sc, id] { const Item* t = sc->item(id);
    modClip = tube::Clip{t->raw.value("profile"), t->raw.value("profs").isArray() ? t->raw.value("profs") : QJsonValue(), P(*t)};
    ui::toast(QObject::tr("Modifier copied — select a curve and press Paste modifier")); emit vp->brushChanged(); });
  v->addWidget(cv);
  std::vector<QWidget*> more{ar, scb, cp};
  if (modClip) { auto* ps = button(QObject::tr("Paste modifier"), QObject::tr("Replace this modifier's profile and settings with the copied ones"));
    QObject::connect(ps, &QPushButton::clicked, vp, [vp] { auto clip = *modClip; vp->runEdit([clip](Scene& s, Selection& sl, const CurveCache& c) { return tube::pasteModifier(s, c, sl, clip); }, true); }); more.push_back(ps); }
  auto* bar = new QWidget; auto* bl = new QGridLayout(bar); bl->setContentsMargins(0, 4, 0, 0); bl->setSpacing(4);
  for (size_t i = 0; i < more.size(); i++) bl->addWidget(more[i], int(i / 2), int(i % 2));
  v->addWidget(bar);
}

// ---------- an extra modifier's settings ----------
namespace {
// the mask's "Affects only" list: drop extras here (from the Extra tab or the outliner)
class OnlyZone : public QFrame {
public:
  std::function<void(std::vector<int>)> dropped;
  OnlyZone() { setAcceptDrops(true); setObjectName("oz"); setStyleSheet("QFrame#oz{border:1px dashed #666;border-radius:4px}"); }
protected:
  static std::vector<int> ids(const QMimeData* md) {
    std::vector<int> out;
    if (md->hasText() && md->text().startsWith("xmod:")) out.push_back(md->text().mid(5).toInt());
    else if (md->hasFormat("application/x-hm-outliner")) { QString s = QString::fromUtf8(md->data("application/x-hm-outliner")); if (s.startsWith("1:")) for (const QString& x : s.mid(2).split(",")) out.push_back(x.toInt()); }
    return out;
  }
  void dragEnterEvent(QDragEnterEvent* e) override { if (!ids(e->mimeData()).empty()) { e->acceptProposedAction(); setStyleSheet("QFrame#oz{border:1px dashed #8fb4f0;border-radius:4px}"); } }
  void dragLeaveEvent(QDragLeaveEvent*) override { setStyleSheet("QFrame#oz{border:1px dashed #666;border-radius:4px}"); }
  void dropEvent(QDropEvent* e) override { auto l = ids(e->mimeData()); e->acceptProposedAction(); if (dropped) { auto f = dropped; QTimer::singleShot(0, this, [f, l] { f(l); }); } }
};
}  // namespace

void xmodProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  Item* x = sc->item(id); const QString kind = x->raw.value("kind").toString();
  if (x->raw.value("mesh").isDouble()) { meshXmodProps(v, vp, sc, id); return; }
  const int tid = x->raw.value("tube").toInt(-1); const Item* t = sc->item(tid);
  v->addWidget(muted(QObject::tr("Extra modifier on %1. The eye in the outliner switches it off.").arg(t ? t->name : QObject::tr("(deleted)"))));
  auto row = [vp, sc, id, v](const QString& label, double lo, double hi, double step, const QString& key, std::function<QString(double)> fmt, const QString& tip = {}) {
    auto* r = new SliderRow(label, lo, hi, step, P(*sc->item(id)).value(key).toDouble(), fmt); if (!tip.isEmpty()) r->setTip(tip);
    QObject::connect(r, &SliderRow::changed, vp, [vp, sc, id, key](double val) { setP(*sc, id, key, val); vp->liveTubes(); });
    QObject::connect(r, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    v->addWidget(r); return r;
  };
  auto check = [vp, sc, id, v](const QString& label, const QString& key, const QString& tip) {
    auto* cb = new QCheckBox(label); cb->setChecked(P(*sc->item(id)).value(key).toBool()); cb->setToolTip(tip);
    QObject::connect(cb, &QCheckBox::toggled, vp, [vp, id, key](bool on) { vp->runEdit([id, key, on](Scene& s, Selection&, const CurveCache&) { setP(s, id, key, on); return QString(); }, false); vp->liveTubes(); });
    v->addWidget(cb);
  };
  auto maskOp = [vp, id](std::function<double(double)> f) {
    vp->runEdit([id, f](Scene& s, Selection&, const CurveCache&) { QJsonArray a; for (double m : tube::maskOf(s.item(id)->raw.value("mask"))) a.append(f(m)); s.item(id)->raw.insert("mask", a); return QString(); }, false); vp->liveTubes();
  };
  auto brushBtn = [vp, kind](const QString& lab) { auto* b = new QPushButton(lab); b->setIcon(ui::icon("brush_paint")); b->setIconSize(QSize(18, 18)); b->setCheckable(true); b->setChecked(vp->maskBrush.on && vp->maskBrush.kind == kind);
    QObject::connect(b, &QPushButton::clicked, vp, [vp, kind] { brushFor(vp, kind); }); return b; };
  auto setDefault = [x](const char* k, double d) { QJsonObject p = x->raw.value("p").toObject(); if (!p.value(k).isDouble()) { p.insert(k, d); x->raw.insert("p", p); } };
  if (kind == "mask") {
    // "Affects only": a list of extras (drag & drop or "+"); empty — all extras of this modifier
    auto only = tube::onlyList(*sc, *x);
    auto setOnly = [vp, id](std::vector<int> l) { vp->runEdit([id, l](Scene& s, Selection&, const CurveCache&) { QJsonArray a; for (int i : l) a.append(i); setP(s, id, "only", a); return QString(); }, false); vp->liveTubes(); };
    auto* zone = new OnlyZone; auto* zv = new QVBoxLayout(zone); zv->setContentsMargins(3, 3, 3, 3); zv->setSpacing(3);
    zone->setToolTip(QObject::tr("Drag extra modifiers here (from the outliner or the Extra tab) or press + — the mask then works only on them"));
    if (only.empty()) zv->addWidget(muted(QObject::tr("All extras of this modifier — drop one here or press +")));
    for (int oid : only) {
      const Item* y = sc->item(oid); auto* r = new QWidget; auto* rh = new QHBoxLayout(r); rh->setContentsMargins(4, 0, 0, 0);
      rh->addWidget(new QLabel(tube::xkindIcon(y->raw.value("kind").toString()) + " " + y->name), 1);
      auto* cx = button("✕", QObject::tr("Remove from the list")); std::vector<int> rest; for (int z : only) if (z != oid) rest.push_back(z);
      QObject::connect(cx, &QPushButton::clicked, vp, [setOnly, rest] { setOnly(rest); }); rh->addWidget(cx); zv->addWidget(r);
    }
    std::vector<int> cands; for (int y : tube::extrasOf(*sc, tid)) if (y != id && sc->item(y)->raw.value("kind").toString() != "mask" && std::find(only.begin(), only.end(), y) == only.end()) cands.push_back(y);
    auto* add = button("＋", QObject::tr("Add an extra modifier of this lock to the list")); add->setEnabled(!cands.empty());
    QObject::connect(add, &QPushButton::clicked, vp, [add, sc, cands, only, setOnly] {
      QMenu m; for (int y : cands) { const Item* yi = sc->item(y); QAction* a = m.addAction(tube::xkindIcon(yi->raw.value("kind").toString()) + " " + yi->name); a->setData(y); }
      QAction* a = m.exec(add->mapToGlobal(QPoint(0, add->height()))); if (!a) return;
      auto l = only; l.push_back(a->data().toInt()); setOnly(l);
    });
    zv->addWidget(add, 0, Qt::AlignLeft);
    zone->dropped = [sc, id, tid, only, setOnly](std::vector<int> ids) {
      std::vector<int> ys; bool other = false;
      for (int i : ids) { const Item* y = sc->item(i); if (!y || y->type != ItemType::XMod || i == id || y->raw.value("kind").toString() == "mask" || std::find(only.begin(), only.end(), i) != only.end()) continue; ys.push_back(i); if (y->raw.value("tube").toInt(-1) != tid) other = true; }
      if (ys.empty()) { ui::toast(QObject::tr("Drop an extra modifier (Thickness, Noise or Curls)")); return; }
      if (other) ui::toast(QObject::tr("Note: an extra of another modifier — this mask only affects its own modifier"));
      auto l = only; l.insert(l.end(), ys.begin(), ys.end()); setOnly(l);
    };
    v->addWidget(muted(QObject::tr("Affects only:"))); v->addWidget(zone);
    check(QObject::tr("Ignore"), "ignore", QObject::tr("On: the mask affects every extra modifier of the lock EXCEPT the ones in the list"));
    auto* cl = button(QObject::tr("Clear")); QObject::connect(cl, &QPushButton::clicked, vp, [maskOp] { maskOp([](double) { return 0.0; }); });
    auto* fl = button(QObject::tr("Fill")); QObject::connect(fl, &QPushButton::clicked, vp, [maskOp] { maskOp([](double) { return 1.0; }); });
    auto* iv = button(QObject::tr("Invert")); QObject::connect(iv, &QPushButton::clicked, vp, [maskOp] { maskOp([](double m) { return 1 - m; }); });
    v->addWidget(hbox({cl, fl, iv, brushBtn(QObject::tr("🖌 Mask brush"))}));
  } else if (kind == "thick" || kind == "twist") {
    v->addWidget(muted(kind == "thick" ? QObject::tr("Painted thickness along the lock: orange — fatter (up to ×2), blue — thinner (down to a point). A mask can limit it.")
                                       : QObject::tr("Painted twist along the lock: green — the profile turns clockwise, pink — anticlockwise (seen from the root along the lock). A mask can limit it.")));
    if (kind == "twist") { setDefault("deg", 180); row(QObject::tr("Angle at 100%"), 0, 720, 1, "deg", deg, QObject::tr("How far the profile turns where the weight is 100% (↻ or ↺)")); }
    auto* cl = button(QObject::tr("Clear")); QObject::connect(cl, &QPushButton::clicked, vp, [maskOp] { maskOp([](double) { return 0.0; }); });
    v->addWidget(hbox({cl, brushBtn(kind == "thick" ? QObject::tr("🖌 Thickness brush") : QObject::tr("🖌 Twist brush"))}));
    v->addWidget(muted(QObject::tr("To erase: Clear — all of it; with the brush Ctrl+Alt + LMB — back to 0 under the brush.")));
  } else if (kind == "noise") {
    setDefault("pure", 0);
    QJsonObject p = P(*x); double ref = p.value("ampRef").toDouble(); if (!ref) ref = p.value("amp").toDouble(); if (!ref) ref = 0.01;
    row(QObject::tr("Amplitude"), 0, ref * 4, ref / 100, "amp", fmtLen, QObject::tr("How far the lock moves from its path"));
    row(QObject::tr("Frequency"), 0.5, 20, 0.1, "freq", [](double f) { return QString::number(f, 'f', 1); }, QObject::tr("Waves along the lock"));
    row(QObject::tr("Pure sine"), 0, 1, 0.01, "pure", pct, QObject::tr("0 — irregular noise; 100% — one clean sine wave (with both spreads on — a clean spiral)"));
    row(QObject::tr("Spread ↔"), 0, 1, 0.01, "sx", pct, QObject::tr("Share of the noise across the lock"));
    row(QObject::tr("Spread ↕"), 0, 1, 0.01, "sy", pct, QObject::tr("Share of the noise outwards (off in Plane mode)"));
    check(QObject::tr("Plane"), "plane", QObject::tr("The noise works in one plane only (across the lock, turned by the rotations below)"));
    row(QObject::tr("Rotation: start"), -180, 180, 1, "rot0", deg, QObject::tr("Turns the noise directions at the root"));
    row(QObject::tr("Rotation: middle"), -180, 180, 1, "rot1", deg); row(QObject::tr("Rotation: end"), -180, 180, 1, "rot2", deg);
    row(QObject::tr("Seed"), 1, 50, 1, "seed", [](double s) { return QString::number(int(s)); }, QObject::tr("Another variant of the noise"));
  } else if (kind == "curl") {
    QJsonObject p = P(*x); double ref = p.value("radRef").toDouble(); if (!ref) ref = p.value("radius").toDouble(); if (!ref) ref = p.value("r0").toDouble(); if (!ref) ref = 0.01;
    for (int k = 0; k < 4; k++) setDefault(qPrintable(QString("r%1").arg(k)), p.value("radius").isDouble() ? p.value("radius").toDouble() : ref * 1.5);   // (older saves: one radius)
    for (const char* k : {"smooth", "shrink", "thin"}) setDefault(k, 0);
    auto sec = [v](const QString& t) { auto* h = new QLabel(t.toUpper()); h->setStyleSheet("color:#cfcfcf;font-weight:600;font-size:11px;margin-top:9px;border-bottom:1px solid #3a3a3a"); v->addWidget(h); };
    sec(QObject::tr("Coil radius"));
    const QString rl[4] = {QObject::tr("Root"), "1/3", "2/3", QObject::tr("Tip")};
    for (int k = 0; k < 4; k++) row(rl[k], 0, ref * 6, ref / 100, QString("r%1").arg(k), fmtLen, QObject::tr("0 — the lock is only twisted here; more — a volume curl, as if it wound around something"));
    row(QObject::tr("Start angle"), -180, 180, 1, "phase", deg, QObject::tr("Where the spiral starts around the lock"));
    sec(QObject::tr("Density (turns)"));
    const QString dl[4] = {"1/4", "2/4", "3/4", "4/4"};
    for (int k = 0; k < 4; k++) row(dl[k], 0, 4, 0.05, QString("d%1").arg(k), [](double d) { return QString::number(d, 'f', 2); }, QObject::tr("Turns over this quarter of the lock (1/4 — root … 4/4 — tip)"));
    sec(QObject::tr("Shape"));
    row(QObject::tr("Thickness"), 0.1, 3, 0.01, "thick", [](double t) { return "×" + QString::number(t, 'f', 2); }, QObject::tr("Lock thickness inside the curl"));
    row(QObject::tr("Rotation in spiral"), -180, 180, 1, "spin", deg, QObject::tr("Turns the cross-section inside the spiral"));
    check(QObject::tr("Reverse direction"), "rev", QObject::tr("The spiral winds the other way round (clockwise ↔ anticlockwise)"));
    check(QObject::tr("Twist the profile"), "follow", QObject::tr("The cross-section turns with the spiral (twisted lock); off — it keeps its direction"));
    sec(QObject::tr("Anti-clipping"));
    row(QObject::tr("Smooth the path"), 0, 1, 0.01, "smooth", pct, QObject::tr("The spiral winds around a smoothed path, so on bends of a curved lock the coils don't cut into each other (the lock shape gets softer)"));
    row(QObject::tr("Shrink on bends"), 0, 1, 0.01, "shrink", pct, QObject::tr("Where the lock bends tighter than the coil, the coil radius smoothly gets smaller (moves the path — the lock may turn)"));
    row(QObject::tr("Thin where crowded"), 0, 1, 0.01, "thin", pct, QObject::tr("Where coils or bends of the lock come closer than its thickness, the lock gets thinner there so the surfaces don't cut into each other. Only the thickness changes — the lock doesn't turn"));
  }
}

}  // namespace hm
