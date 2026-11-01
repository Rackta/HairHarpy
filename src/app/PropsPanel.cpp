#include "app/PropsPanel.h"
#include "scene/Crown.h"
#include "app/TopoPanels.h"
#include "app/ToolPanels.h"
#include "app/BraidPanels.h"
#include "app/ModPanels.h"
#include "app/CrownPanels.h"
#include "app/RibPanels.h"
#include "app/MeshPanels.h"
#include "scene/TopoEdit.h"
#include "scene/MeshMods.h"
#include "scene/WallTools.h"
#include "app/ShellPanels.h"
#include "scene/Cocoons.h"
#include "scene/Convert.h"
#include "scene/Cut.h"
#include "scene/Edit.h"
#include "scene/Symmetry.h"
#include "scene/Walls.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QCursor>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <array>
#include <cmath>
#include <functional>

namespace hm {

namespace {
const double COCOON_OP = 0.2, COCOON_BEND = 2, COCOON_TIP = 1.5; const int COCOON_FADE = 3;

QLabel* h4(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setWordWrap(true); return l; }
QLabel* muted(const QString& t) { auto* l = new QLabel(t); l->setProperty("mut", true); l->setWordWrap(true); return l; }
QLabel* text(const QString& t) { auto* l = new QLabel(t); l->setWordWrap(true); return l; }
QString pct(double v) { return QString("%1%").arg(int(std::lround(v * 100))); }
// JS toPrecision(3)
QString fmtLen(double v) {
  if (v == 0) return "0.00";
  int e = int(std::floor(std::log10(std::abs(v))));
  if (e < -6 || e >= 3) { QString s = QString::number(v, 'e', 2); s.replace(QRegularExpression("e([+-])0*(\\d)"), "e\\1\\2"); return s; }
  return QString::number(v, 'f', std::max(0, 2 - e));
}
QWidget* row(const QString& label, QWidget* w, const QString& tip = {}) {
  auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 2, 0, 2); h->setSpacing(6);
  auto* l = new QLabel(label); l->setProperty("mut", true); l->setFixedWidth(96); h->addWidget(l); h->addWidget(w, 1);
  if (!tip.isEmpty()) r->setToolTip(tip);
  return r;
}
QPushButton* button(const QString& t, const QString& tip = {}, bool primary = false) {
  auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); if (primary) b->setProperty("primary", true); return b;
}
QCheckBox* check(const QString& t, bool on, const QString& tip = {}) { auto* c = new QCheckBox(t); c->setChecked(on); c->setToolTip(tip); c->setFocusPolicy(Qt::NoFocus); return c; }
bool svc(const Item& it) { return it.raw.value("guidesGroup").toBool() || it.raw.value("meshesGroup").toBool() || it.raw.value("importGroup").toBool(); }
QJsonObject P(const Item& it) { return it.raw.value("p").toObject(); }
void setP(Scene& sc, int id, const QString& k, const QJsonValue& v) { Item* it = sc.item(id); QJsonObject p = it->raw.value("p").toObject(); p.insert(k, v); it->raw.insert("p", p); }
auto noop = [](Scene&, Selection&, const CurveCache&) { return QString(); };
}  // namespace

QWidget* buildPropsPanel(Viewport* vp, Scene* sc, int pinned) {
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setSpacing(4);
  const Selection& S = vp->selection();
  auto finish = [&] { v->addStretch(1); return w; };
  // ---- anchors ----
  if (!S.anchors.empty() && pinned < 0) {
    std::vector<int> as = S.anchors; const Anchor* last = sc->anchor(as.back());
    v->addWidget(h4(QObject::tr("Anchors: %1").arg(as.size())));
    auto* sm = new SliderRow(QObject::tr("Smoothness"), 0, 1, 0.01, last ? last->smooth : 1, [](double x) { return QString::number(x, 'f', 2); });
    QObject::connect(sm, &SliderRow::changed, vp, [vp, sc, as](double x) { for (int a : as) if (Anchor* A = sc->anchor(a)) A->smooth = x; vp->liveAnchors(as); });
    QObject::connect(sm, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    v->addWidget(sm);
    // anchors on a crown's lower connector: a sharp corner there — on the connector alone (a lock attached there stays smooth)
    std::vector<int> onLow; for (int a : as) if (crown::lowConnAt(*sc, a) >= 0) onLow.push_back(a);
    if (!onLow.empty()) {
      bool all = true; for (int a : onLow) if (!crown::sharpCorner(*sc, a)) all = false;
      auto* sc2 = check(QObject::tr("Sharp corner on the lower connector"), all,
                        QObject::tr("The crown's lower connector breaks at these anchors. A lock attached there keeps its smooth start — no hard step between the crown and the lock. A symmetric crown gets it on both sides"));
      QObject::connect(sc2, &QCheckBox::toggled, vp, [vp, onLow](bool on) { vp->runEdit([onLow, on](Scene& s, Selection&, const CurveCache&) { for (int a : onLow) crown::setSharpCorner(s, a, on); return QString(); }, true); });
      v->addWidget(sc2);
    }
    v->addWidget(muted(QObject::tr("B + LMB drag does the same in the viewport. Curves through the last anchor: %1").arg(edit::curvesWith(*sc, as.back()).size())));
    ribAnchorProps(v, vp, sc);                                // (anchors of flat locks: their width and rotation)
    return finish();
  }
  // curves and walls selected together: the curves' settings (the last selected curve), else the last selected item
  int id = pinned;
  if (id < 0) { for (auto it = S.items.rbegin(); it != S.items.rend(); ++it) if (const Item* x = sc->item(*it); x && x->type == ItemType::Curve) { id = *it; break; }
                if (id < 0 && !S.items.empty()) id = S.items.back(); }
  const Item* it = sc->item(id);
  if (!it) { v->addWidget(muted(QObject::tr("Nothing selected"))); return finish(); }
  v->addWidget(h4(QString("%1 %2").arg(itemTypeIcon(it->type), it->name)));
  const int iid = it->id;

  if (it->type == ItemType::Group) {
    if (!svc(*it)) {                                          // (the service groups have no marker colour)
      auto* cb = button(""); cb->setFixedSize(46, 20);
      cb->setStyleSheet(QString("background:%1;border:1px solid #555;border-radius:3px").arg(it->color.isEmpty() ? "#4a7fd6" : it->color));
      QObject::connect(cb, &QPushButton::clicked, vp, [vp, sc, iid] {
        const Item* g = sc->item(iid); QColor c = QColorDialog::getColor(QColor(g->color.isEmpty() ? "#4a7fd6" : g->color), nullptr, QObject::tr("Marker colour"));
        if (c.isValid()) vp->runEdit([iid, c](Scene& s, Selection&, const CurveCache&) { s.item(iid)->color = c.name(); return QString(); }, false); vp->sceneChanged(false);   // (the walls re-tinted)
      });
      auto* rc = button(QObject::tr("Reset colour"));
      QObject::connect(rc, &QPushButton::clicked, vp, [vp, iid] { vp->runEdit([iid](Scene& s, Selection&, const CurveCache&) { s.item(iid)->color.clear(); return QString(); }, false); vp->sceneChanged(false); });
      auto* cr = new QWidget; auto* ch = new QHBoxLayout(cr); ch->setContentsMargins(0, 0, 0, 0); ch->addWidget(cb); ch->addStretch(1);
      v->addWidget(row(QObject::tr("Marker colour"), cr)); v->addWidget(rc, 0, Qt::AlignLeft);
    }
    v->addWidget(muted(QObject::tr("Items inside: %1").arg(it->children.size())));
    if (sc->room == "topo" && it->raw.value("vsym").isObject()) {   // ---- a virtual symmetry: its kind, its axes and sides, the seam, Apply ----
      const QJsonObject vs = it->raw.value("vsym").toObject(); const QString type = vs.value("type").toString("mirror");
      auto setVs = [vp, iid](std::function<void(QJsonObject&)> fn) { vp->runEdit([iid, fn](Scene& s, Selection&, const CurveCache&) { Item* g = s.item(iid); if (!g) return QString("!");
        QJsonObject o = g->raw.value("vsym").toObject(); fn(o); g->raw.insert("vsym", o); return QObject::tr("Symmetry changed"); }, false); };
      v->addWidget(h4(QObject::tr("⧉ Symmetry")));
      auto* ty = new QComboBox; for (auto [k, l] : std::vector<std::pair<QString, QString>>{{"mirror", QObject::tr("Mirror")}, {"radial", QObject::tr("Radial")}, {"radialMirror", QObject::tr("Radial mirror")}, {"radialHalf", QObject::tr("Radial half")}}) ty->addItem(l, k);
      ty->setCurrentIndex(std::max(0, ty->findData(type)));
      QObject::connect(ty, &QComboBox::activated, vp, [ty, setVs](int) { const QString k = ty->currentData().toString(); setVs([k](QJsonObject& o) { o.insert("type", k); }); });
      v->addWidget(row(QObject::tr("Type"), ty, QObject::tr("Mirror — by the axes picked (each with its kept side); Radial — copies round an axis; Radial mirror — each copy and its mirror; Radial half — round half of it")));
      const int ax0 = std::clamp(vs.value("axis").toInt(0), 0, 2);
      if (type == "mirror") {
        // (the axes: X+ / X− … — several at once; a side is the one kept, mirrored onto the other; again — that axis off)
        QStringList axes; for (const QJsonValue& x : vs.value("axes").toArray()) axes << x.toString();
        if (!vs.value("axes").isArray()) axes << QString("xyz")[ax0] + QString("+");
        auto* aw = new QWidget; auto* ah = new QGridLayout(aw); ah->setContentsMargins(0, 0, 0, 0); ah->setSpacing(3);
        for (int a = 0; a < 3; a++) for (const char* sgn : {"+", "-"}) {
          const QString key = QString("xyz")[a] + QString(sgn); auto* b = button(QString(QString("XYZ")[a]) + (QString(sgn) == "+" ? QString("+") : QString("−")));
          b->setCheckable(true); b->setChecked(axes.contains(key)); b->setFixedWidth(34);
          b->setToolTip(QObject::tr("%1: the %2 side kept and mirrored onto the other (several axes — a copy for each mix of them); again — this axis off").arg(QString("XYZ")[a]).arg(QString(sgn) == "+" ? "+" : "−"));
          QObject::connect(b, &QPushButton::clicked, vp, [setVs, axes, key] { QStringList n = axes; const QString other = key.left(1) + (key.endsWith('+') ? "-" : "+");
            if (n.contains(key)) n.removeAll(key); else { n.removeAll(other); n << key; }
            setVs([n](QJsonObject& o) { o.insert("axes", QJsonArray::fromStringList(n)); if (!n.isEmpty()) o.insert("axis", QString("xyz").indexOf(n[0].left(1))); }); });
          ah->addWidget(b, QString(sgn) == "+" ? 0 : 1, a); }
        ah->setColumnStretch(3, 1); v->addWidget(row(QObject::tr("Axes"), aw));
        if (axes.isEmpty()) v->addWidget(muted(QObject::tr("No axis — nothing mirrored")));
        // (the seam: the points within the distance of a plane put onto it — the halves meet, welded on Apply)
        const bool weld = vs.value("weld").toBool(true);
        auto* wc = check(QObject::tr("Weld the seam"), weld, QObject::tr("The points within the distance of a mirror plane are put onto it — the halves meet without a gap; Apply welds them into one"));
        QObject::connect(wc, &QCheckBox::toggled, vp, [setVs](bool x) { setVs([x](QJsonObject& o) { o.insert("weld", x); }); });
        auto* wd = new QDoubleSpinBox; wd->setRange(0, 1000); wd->setDecimals(3); wd->setSingleStep(0.01); wd->setSuffix(" cm"); wd->setValue(vs.value("weldDist").toDouble(0.05)); wd->setEnabled(weld);
        wd->setToolTip(QObject::tr("How near a plane a point is put onto it (and the polygons a hair over it kept)"));
        QObject::connect(wd, &QDoubleSpinBox::editingFinished, vp, [wd, setVs] { const double x = wd->value(); setVs([x](QJsonObject& o) { o.insert("weldDist", x); }); });
        v->addWidget(wc); v->addWidget(row(QObject::tr("Distance"), wd));
      } else {
        auto* ac = new QComboBox; for (int a = 0; a < 3; a++) ac->addItem(QString("XYZ")[a], a); ac->setCurrentIndex(ax0);
        QObject::connect(ac, &QComboBox::activated, vp, [ac, setVs](int) { const int a = ac->currentData().toInt(); setVs([a](QJsonObject& o) { o.insert("axis", a); }); });
        v->addWidget(row(QObject::tr("Axis"), ac, QObject::tr("The line the copies turn round (through the group's point)")));
        auto* cn = new QSpinBox; cn->setRange(2, 64); cn->setValue(std::max(2, vs.value("count").toInt(6)));
        QObject::connect(cn, &QSpinBox::editingFinished, vp, [cn, setVs] { const int n = cn->value(); setVs([n](QJsonObject& o) { o.insert("count", n); }); });
        v->addWidget(row(QObject::tr("Copies"), cn));
      }
      v->addWidget(muted(QObject::tr("About the group's point and axes (Transform; inside an object — its gizmo)")));
      // (the group's point and axes put on a child object's own gizmo — what is in it stays where it is)
      { std::vector<int> kids; std::function<void(int)> walk = [&](int pid) { for (int c : sc->item(pid)->children) if (const Item* x = sc->item(c)) { if (x->type == ItemType::IMesh && x->poly) kids.push_back(c); walk(c); } }; walk(iid);
        auto* tg = button(QObject::tr("⌖ To a child's local axes"), QObject::tr("The group's point and axes put on the gizmo of an object inside it (its pivot and axes — Align / Alt + drag set them); the meshes stay where they are"));
        tg->setEnabled(!kids.empty());
        auto put = [vp, iid](int kid) { const Affine G = vp->meshFrame(kid); vp->runEdit([iid, G](Scene& s, Selection&, const CurveCache&) {
          Item* g = s.item(iid); if (!g) return QString("!");
          auto qj = [](const QJsonValue& x) { const QJsonArray a = x.toArray(); return a.size() == 4 ? Quat{a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()}.normalized() : Quat{}; };
          Quat P; for (const Item* p = s.item(g->parent); p; p = s.item(p->parent)) P = (qj(p->raw.value("q")) * P).normalized();
          const Quat W = Quat::lookRotation(-G.lin({0, 0, 1}).normalized(), G.lin({0, 1, 0}).normalized()), q = (P.conj() * W).normalized();
          g->raw.insert("gpivot", QJsonArray{G.t.x, G.t.y, G.t.z}); g->raw.insert("q", QJsonArray{q.x, q.y, q.z, q.w});
          return QObject::tr("Symmetry: on the child's local axes"); }); };
        QObject::connect(tg, &QPushButton::clicked, vp, [vp, sc, kids, put] {
          if (kids.size() == 1) { put(kids[0]); return; }
          QMenu menu; for (int k : kids) if (const Item* x = sc->item(k)) QObject::connect(menu.addAction(x->name), &QAction::triggered, vp, [put, k] { put(k); });
          menu.exec(QCursor::pos()); });
        v->addWidget(tg, 0, Qt::AlignLeft); }
      auto* ap = button(QObject::tr("✔ Apply to the object"), QObject::tr("The copies become the meshes' own geometry — the seam welded, the other sides' polygons out; the group stays, its symmetry gone"), true);
      QObject::connect(ap, &QPushButton::clicked, vp, [vp, iid] { QTimer::singleShot(0, vp, [vp, iid] { vp->vsymApply(iid); }); });
      auto* rm = button(QObject::tr("✕ Remove the symmetry"), QObject::tr("Only the original kept — the preview gone"));
      QObject::connect(rm, &QPushButton::clicked, vp, [vp, iid] { vp->runEdit([iid](Scene& s, Selection&, const CurveCache&) { s.item(iid)->raw.remove("vsym"); return QObject::tr("Virtual symmetry removed"); }); });
      v->addWidget(ap, 0, Qt::AlignLeft); v->addWidget(rm, 0, Qt::AlignLeft);
    }
    if (sc->room == "topo") {   // (the Topology room: no curves' settings)
      v->addWidget(muted(QObject::tr("A group of meshes — drag meshes into it in the outliner")));
      auto* ic = check(QObject::tr("Skip in the Inspector"), it->raw.value("inspOff").toBool(), QObject::tr("The Inspector checks nothing inside this group (high-poly sources, references) — no markers over them"));
      QObject::connect(ic, &QCheckBox::toggled, vp, [vp, iid](bool x) { vp->runEdit([iid, x](Scene& s, Selection&, const CurveCache&) { Item* g = s.item(iid); if (x) g->raw.insert("inspOff", true); else g->raw.remove("inspOff"); return QString(); }, false); vp->inspChanged(); });
      v->addWidget(ic);
      return finish(); }
    if (!svc(*it)) {
      auto* gs = new QComboBox; gs->addItem(QObject::tr("Auto"), "auto"); gs->addItem(QObject::tr("Show"), "show"); gs->addItem(QObject::tr("Hide"), "hide");
      QString mode = it->raw.value("guides").toString("auto"); gs->setCurrentIndex(std::max(0, gs->findData(mode)));
      gs->setToolTip(QObject::tr("Auto: curves and anchors are shown only while you work with something inside this group"));
      QObject::connect(gs, &QComboBox::currentIndexChanged, vp, [vp, iid, gs](int) { QString m = gs->currentData().toString(); vp->runEdit([iid, m](Scene& s, Selection&, const CurveCache&) { s.item(iid)->raw.insert("guides", m); return QString(); }, false); });
      v->addWidget(row(QObject::tr("Curves & anchors"), gs));
    }
    // the group's own modifiers (Density, Cap, Fill): a click selects one
    std::vector<const Item*> gm; for (int c : it->children) if (const Item* x = sc->item(c); x && (x->type == ItemType::Tip || x->type == ItemType::Even || x->type == ItemType::Shell || x->type == ItemType::Cap || x->type == ItemType::Skin)) gm.push_back(x);
    if (!gm.empty()) {
      v->addWidget(h4(QObject::tr("⬡ Modifiers")));
      for (const Item* x : gm) { auto* b = button(QString("%1 %2%3").arg(itemTypeIcon(x->type), x->name, x->visible ? QString() : QObject::tr(" (off)")), QObject::tr("Select this modifier — its settings open here (and in the Modifiers tool)"));
        b->setStyleSheet("text-align:left"); int xid = x->id; QObject::connect(b, &QPushButton::clicked, vp, [vp, xid] { vp->setSelectedItems({xid}); }); v->addWidget(b); }
    }
    if (!it->raw.value("guidesGroup").toBool() && !it->raw.value("meshesGroup").toBool()) {
      v->addWidget(h4(QObject::tr("🏷 Labels")));
      QJsonObject L = it->raw.value("labels").toObject();
      struct Lb { const char* k; const char* lab; const char* tip; };
      for (const Lb& lb : {Lb{"loose", "Free curves visible", "Free curves visible: curves joined to no NURBS surface and no modifier stay shown in the viewport even while the group isn't selected (on by default)"},
                           Lb{"conn", "Connectors visible", "Connectors visible: the crown's parting line and lower connector stay shown while you work in any other group (on by default)"}}) {
        if (QString(lb.k) == "conn") { bool crownG = false; for (int c : it->children) if (const Item* x = sc->item(c); x && x->type == ItemType::Crown) crownG = true; if (!crownG) continue; }   // (a crown's group only)
        bool on = QString(lb.k) == "loose" || QString(lb.k) == "conn" ? !(L.contains(lb.k) && L.value(lb.k) == QJsonValue(false)) : L.value(lb.k).toBool();
        auto* c = check(QObject::tr(lb.lab), on, QObject::tr(lb.tip)); QString k = lb.k;
        QObject::connect(c, &QCheckBox::toggled, vp, [vp, iid, k](bool x) { vp->runEdit([iid, k, x](Scene& s, Selection&, const CurveCache&) { Item* g = s.item(iid); QJsonObject l = g->raw.value("labels").toObject(); l.insert(k, x); g->raw.insert("labels", l); return QString(); }, true); });
        v->addWidget(c);
      }
      // loop step of this group against the project's
      double own = edit::groupConv(*sc, sc->item(iid)).value("step").toDouble(), ps = projectStep(*sc); bool off = std::abs(own / ps - 1) > 0.01;
      auto* d = muted(QObject::tr("Loop step: %1 · project: %2").arg(fmtLen(own), fmtLen(ps))); if (off) d->setStyleSheet("color:#e0b060"); v->addWidget(d);
      auto* rb = button(QObject::tr("↻ Match project loop step"), QObject::tr("Recalculate this group (and groups inside it) with the project loop step — for NURBS made by modifiers or brought in from elsewhere"), off);
      QObject::connect(rb, &QPushButton::clicked, vp, [vp, sc, iid] { QString n = sc->item(iid)->name; double st = projectStep(*sc);
        vp->runEdit([iid, n, st](Scene& s, Selection&, const CurveCache&) { edit::matchProjectStep(s, iid); return QObject::tr("\"%1\": loop step %2 (project)").arg(n, fmtLen(st)); }, true); });
      v->addWidget(rb, 0, Qt::AlignLeft);
      { std::vector<int> cs; for (const Item* c : sc->curves()) if (c->ring < 0 && sc->isDescendant(c->id, iid)) cs.push_back(c->id);
        int nx = 0; for (int a : cut::xAnchors(*sc, cs)) if (!cut::wallsAcrossX(*sc, a).empty()) nx++;
        if (nx) { auto* xb = button(QObject::tr("✂ Separate zones at X junctions (%1)").arg(nx), QObject::tr("Where curves cross or welded strands swap partners (an X), the walls are cut across there: the walls above and below become separate locks (their loops are built independently), joined only at the cut. The curves stay whole — the new walls run on edge curves that follow them"), true);
          QObject::connect(xb, &QPushButton::clicked, vp, [vp, iid] { vp->runEdit([iid](Scene& s, Selection& sl, const CurveCache&) { return cut::separateGroupX(s, sl, iid); }, true); });
          v->addWidget(xb, 0, Qt::AlignLeft); } }
      auto* db = button(QObject::tr("⛓ Detach from neighbouring groups"), QObject::tr("Curves of this group welded to curves of other groups get anchors of their own there (welds inside the group stay)"));
      QObject::connect(db, &QPushButton::clicked, vp, [vp, iid] { vp->runEdit([iid](Scene& s, Selection& sl, const CurveCache&) { return edit::detachGroup(s, iid, sl); }, true); });
      v->addWidget(db, 0, Qt::AlignLeft);
    }
    // the group's own point: where the gizmo stands for it (Move carries everything inside); groups start at 0, 0, 0. A
    // symmetric group inside mirrors across it
    if (!svc(*it)) {
      const Vec3 gp = sym::groupPoint(*it);
      v->addWidget(h4(QObject::tr("Position")));
      auto* pr = new QWidget; auto* ph = new QHBoxLayout(pr); ph->setContentsMargins(0, 0, 0, 0); ph->setSpacing(4);
      std::array<QDoubleSpinBox*, 3> ps{};
      for (int k = 0; k < 3; k++) { auto* sp = new QDoubleSpinBox; sp->setRange(-1e6, 1e6); sp->setDecimals(2); sp->setButtonSymbols(QAbstractSpinBox::NoButtons); sp->setMinimumWidth(40); sp->setSingleStep(0.1); sp->setPrefix(QString("XYZ"[k]) + " ");
        sp->setValue(k == 0 ? gp.x : k == 1 ? gp.y : gp.z); sp->setKeyboardTracking(false); sp->setToolTip(QObject::tr("The group's point — changed, everything inside moves with it")); ps[size_t(k)] = sp; ph->addWidget(sp, 1); }
      for (auto* sp : ps) QObject::connect(sp, &QDoubleSpinBox::valueChanged, vp, [vp, iid, ps](double) { vp->setGroupPoint(iid, Vec3(ps[0]->value(), ps[1]->value(), ps[2]->value()), true); });
      v->addWidget(pr);
      const Vec3 hc = vp->camera().sceneCenter;
      auto* hb = new QWidget; auto* hh = new QHBoxLayout(hb); hh->setContentsMargins(0, 0, 0, 0); hh->setSpacing(4);
      auto* mv = button(QObject::tr("✥ Move to the head's centre"), QObject::tr("The group with everything inside moved so that its point is at the head's centre (%1, %2, %3)").arg(hc.x, 0, 'f', 2).arg(hc.y, 0, 'f', 2).arg(hc.z, 0, 'f', 2));
      QObject::connect(mv, &QPushButton::clicked, vp, [vp, iid] { vp->setGroupPoint(iid, vp->camera().sceneCenter, true); ui::toast(QObject::tr("The group moved to the head's centre")); });
      auto* pt = button(QObject::tr("⌖ Point only"), QObject::tr("Only the group's point goes to the head's centre — what is inside stays where it is (a symmetric group inside then mirrors across the head's centre)"));
      QObject::connect(pt, &QPushButton::clicked, vp, [vp, iid] { vp->setGroupPoint(iid, vp->camera().sceneCenter, false); ui::toast(QObject::tr("The group's point is at the head's centre — the contents stayed")); });
      hh->addWidget(mv); hh->addWidget(pt); hh->addStretch(1); v->addWidget(hb);
    }
    // symmetry: everything inside gets a live mirrored copy
    if (!svc(*it)) {
      QJsonObject sym = it->raw.value("sym").toObject(); QString axis = sym.value("axis").toString("x");
      v->addWidget(h4(QObject::tr("Symmetry")));
      auto* on = check(QObject::tr("Enabled"), sym.value("on").toBool());
      QObject::connect(on, &QCheckBox::toggled, vp, [vp, iid](bool x) { vp->runEdit([iid, x](Scene& s, Selection&, const CurveCache&) { Item* g = s.item(iid); QJsonObject o = g->raw.value("sym").toObject(); if (!o.contains("axis")) o.insert("axis", "x"); o.insert("on", x); g->raw.insert("sym", o); return QString(); }, true); });
      v->addWidget(on);
      auto* ax = new QComboBox; for (const char* a : {"x", "y", "z"}) ax->addItem(QString(a).toUpper(), a); ax->setCurrentIndex(std::max(0, ax->findData(axis)));
      QObject::connect(ax, &QComboBox::currentIndexChanged, vp, [vp, iid, ax](int) { QString a = ax->currentData().toString(); vp->runEdit([iid, a](Scene& s, Selection&, const CurveCache&) { Item* g = s.item(iid); QJsonObject o = g->raw.value("sym").toObject(); o.insert("axis", a); if (!o.contains("on")) o.insert("on", false); g->raw.insert("sym", o); return QString(); }, true); });
      v->addWidget(row(QObject::tr("Axis"), ax));
      const Vec3 hc0 = vp->camera().sceneCenter, cen = sym::symCenter(*sc, *it, hc0); double cv = axis == "y" ? cen.y : axis == "z" ? cen.z : cen.x;   // (S.center — or the point of the group holding it)
      v->addWidget(muted(QObject::tr("Mirror plane: %1 = %2 (%3)").arg(axis.toUpper()).arg(cv, 0, 'f', 2).arg((cen - hc0).lengthSq() < 1e-18 ? QObject::tr("head centre") : QObject::tr("the parent group's point"))));
      auto* ap = button(QObject::tr("Apply symmetry"), QObject::tr("Turn the mirrored copy into real, editable curves and surfaces")); ap->setEnabled(sym.value("on").toBool());
      QObject::connect(ap, &QPushButton::clicked, vp, [vp, iid] { Vec3 c = vp->camera().sceneCenter; vp->runEdit([iid, c](Scene& s, Selection& sl, const CurveCache&) { return sym::applySymmetry(s, iid, c, sl); }, true); });
      v->addWidget(ap, 0, Qt::AlignLeft);
    }
  } else if (it->type == ItemType::Cocoon) {
    const auto& R = vp->hair().cocoons(); auto f = R.find(iid); size_t nr = f != R.end() ? f->second.rings.size() : 0;
    CocoonSystem* cs = vp->hair().cocoonSystem(); int lg = cs ? cs->lockGroup(iid) : -1;
    double step = lg >= 0 ? edit::groupConv(*sc, sc->item(lg)).value("step").toDouble() : projectStep(*sc);
    v->addWidget(text(QObject::tr("Rings: %1 · loop step %2").arg(nr).arg(fmtLen(step))));
    // where the rings are drawn: on the cocoon's shell, or projected onto its lock
    { auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 4, 0, 4); h->setSpacing(4); h->addWidget(new QLabel(QObject::tr("Rings:")));
      bool lock = it->raw.value("ringMode").toString() == "lock";
      for (auto [m, lab, tip] : {std::tuple<QString, QString, QString>{"shell", QObject::tr("On the cocoon"), QObject::tr("The rings drawn on the cocoon's shell")},
                                 {"lock", QObject::tr("On the lock"), QObject::tr("The rings projected onto the lock: where each ring's plane cuts the lock's walls")}}) {
        auto* b = button(lab, tip); b->setCheckable(true); b->setChecked((m == "lock") == lock); QString mm = m;
        QObject::connect(b, &QPushButton::clicked, vp, [vp, iid, mm, lock] { if ((mm == "lock") == lock) return;
          vp->runEdit([iid, mm](Scene& s, Selection&, const CurveCache&) { s.item(iid)->raw.insert("ringMode", mm == "lock" ? QJsonValue("lock") : QJsonValue()); return QString(); }, true); });
        h->addWidget(b);
      }
      h->addStretch(1); v->addWidget(r); }
    // the shell's opacity (double-click — the default)
    double op = it->raw.value("op").isDouble() ? it->raw.value("op").toDouble() : COCOON_OP;
    auto* os = new SliderRow(QObject::tr("Opacity"), 0, 1, 0.01, op, pct); os->setReset(COCOON_OP);
    os->setTip(QObject::tr("How see-through the cocoon's shell is. Double-click — default (%1)").arg(pct(COCOON_OP)));
    QObject::connect(os, &SliderRow::changed, vp, [vp, sc, iid](double x) { sc->item(iid)->raw.insert("op", x); vp->liveView(); });
    QObject::connect(os, &SliderRow::released, vp, [vp, sc, iid](double x) { if (std::abs(x - COCOON_OP) < 1e-9) sc->item(iid)->raw.insert("op", QJsonValue()); vp->commitLive(); });
    v->addWidget(os);
    // denser rings on bends / toward the tip (×1 — even)
    struct Dn { const char* key; const char* lab; double def; QString tip; };
    for (const Dn& d : {Dn{"bendD", "Denser on bends", COCOON_BEND, QObject::tr("How much closer the rings come on bends (×1 — no closer; full on a bend of 30° a loop step). Double-click — default (×%1)").arg(COCOON_BEND)},
                        Dn{"tipD", "Denser to the tip", COCOON_TIP, QObject::tr("How much closer the rings come toward the tip, over its last stretch (×1 — no closer). Double-click — default (×%1)").arg(COCOON_TIP)}}) {
      auto fmt = [](double x) { QString s = QString::number(x, 'f', 2); if (s.endsWith('0')) s.chop(1); return "×" + s; };
      double val = it->raw.value(d.key).isDouble() ? it->raw.value(d.key).toDouble() : d.def;
      auto* s = new SliderRow(QObject::tr(d.lab), 1, 2.5, 0.05, val, fmt); s->setReset(d.def); s->setTip(d.tip); QString k = d.key; double def = d.def;
      QObject::connect(s, &SliderRow::changed, vp, [vp, sc, iid, k](double x) { sc->item(iid)->raw.insert(k, x); vp->liveCocoons({iid}); });
      QObject::connect(s, &SliderRow::released, vp, [vp, sc, iid, k, def](double x) { if (std::abs(x - def) < 1e-9) { sc->item(iid)->raw.insert(k, QJsonValue()); vp->liveCocoons({iid}); } vp->commitLive(); });
      v->addWidget(s);
    }
    const Item* pc = it->parent >= 0 && sc->item(it->parent) && sc->item(it->parent)->type == ItemType::Cocoon ? sc->item(it->parent) : nullptr;
    int kids = 0; for (int c : it->children) if (const Item* x = sc->item(c); x && x->type == ItemType::Cocoon) kids++;
    v->addWidget(muted(pc ? QObject::tr("Branch of %1").arg(pc->name) : QObject::tr("Trunk") + (kids ? QObject::tr(" · branches: %1").arg(kids) : QString())));
    QString src = it->raw.value("src").toString(); bool live = it->raw.value("live").isObject();
    if (live) v->addWidget(text(QObject::tr("Follows the lock%1: %2 strands (root → tip); branches added later stay out").arg(src.isEmpty() ? QString() : " " + src).arg(it->raw.value("live").toObject().value("ends").toArray().size())));
    v->addWidget(muted(QObject::tr("A NURBS shell %1%2 — no curves; its rings are the loops: the loop step and Deviation of its group, closer on bends. A branch has its rings where its parent's rings cross it while it runs inside (or on) the parent, and turns to its own spacing over %3 loop steps after it leaves. Every ring is square to its own cocoon.")
                         .arg(live ? QObject::tr("of its lock's shape") : QObject::tr("of fixed shape")).arg(src.isEmpty() ? QString() : QObject::tr(" made from %1").arg(src)).arg(COCOON_FADE)));
  } else if (it->type == ItemType::Curve) {
    std::vector<int> list; if (pinned < 0) for (int x : S.items) if (const Item* c = sc->item(x); c && c->type == ItemType::Curve) list.push_back(x);
    if (list.empty()) list = {iid};
    v->addWidget(muted(QObject::tr("Anchors: %1. RMB on the line inserts an anchor.").arg(it->anchors.size())));
    if (it->hasPath()) {
      QStringList r; for (auto& p : it->path) if (const Item* c = sc->item(p[0])) r << c->name;
      v->addWidget(text(QObject::tr("Route: ") + r.join(" → ")));
      auto* cr = button(QObject::tr("⤳ Change the route…"), QObject::tr("Another way through the forks for this service curve — the part where it went along the wrong branch is replaced; its walls follow (Shift+RMB with it selected — the same)"));
      QObject::connect(cr, &QPushButton::clicked, vp, [] { ui::toast(QObject::tr("The route pie comes with the selection extras of this rewrite")); });
      v->addWidget(cr, 0, Qt::AlignLeft);
    }
    auto* br = new QWidget; auto* bh = new QHBoxLayout(br); bh->setContentsMargins(0, 4, 0, 4); bh->setSpacing(4);
    auto* rc = button(QObject::tr("↺ Recalculate all anchors"), QObject::tr("Reset frozen handles and smoothness of every anchor of the selected curve(s) — back to clean automatic arcs"));
    auto* rv = button(QObject::tr("⇄ Reverse direction"), QObject::tr("Swap the start (root) and the end (tip) of the selected curve(s). The diamond anchor marks the start (root); modifiers sweep from there"));
    QObject::connect(rc, &QPushButton::clicked, vp, [vp, sc, list] { vp->recalcAnchors(edit::anchorsOf(*sc, list)); });
    QObject::connect(rv, &QPushButton::clicked, vp, [vp, list] { vp->reverseCurves(list); });
    bh->addWidget(rc); bh->addWidget(rv); bh->addStretch(1); v->addWidget(br);
    auto anchorsOfList = [sc, list] { return edit::anchorsOf(*sc, list); };
    auto* rr = new SliderRow(QObject::tr("Roundness"), 0, 1, 0.01, it->round, pct);
    rr->setTip(QObject::tr("0 — smooth spline (with 3 anchors a parabola); 1 — every bend follows the circular arc through its anchors. Applies to all selected curves"));
    QObject::connect(rr, &SliderRow::changed, vp, [vp, sc, list, anchorsOfList](double x) { for (int c : list) { Item* ci = sc->item(c); ci->round = x; ci->roundSet = true; } vp->liveAnchors(anchorsOfList()); });
    QObject::connect(rr, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    v->addWidget(rr);
    auto* fr = new SliderRow(QObject::tr("Smoothing"), 0, 1, 0.01, it->fair, pct); fr->setReset(0);
    fr->setTip(QObject::tr("Moves the anchors' handles towards the smoothest curve through them — no curvature jumps. Frozen handles and anchors shared with other curves stay. Double-click — 0. Applies to all selected curves"));
    QObject::connect(fr, &SliderRow::changed, vp, [vp, sc, list, anchorsOfList](double x) { for (int c : list) { Item* ci = sc->item(c); ci->fair = x; ci->fairSet = true; } vp->liveAnchors(anchorsOfList()); });
    QObject::connect(fr, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    v->addWidget(fr);
    if (it->ring < 0) {
      auto* er = new SliderRow(QObject::tr("Even out anchors"), 0, 1, 0.01, it->raw.value("asm").toDouble(0), pct); er->setReset(0);
      er->setTip(QObject::tr("Moves the anchors so the curve runs smoothly — takes out the wobble of a wavy line (e.g. one made from an uneven edge path) and keeps its overall shape. Curve ends and anchors shared with other curves stay. Always from the anchors' positions before, so it can go back to 0. Double-click — 0. Applies to all selected curves"));
      QObject::connect(er, &SliderRow::changed, vp, [vp, sc, list](double x) { vp->liveAnchors(edit::evenAnchors(*sc, list, x, sceneScale(*sc))); });
      QObject::connect(er, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
      v->addWidget(er);
    }
    QStringList users; for (auto& [sid, s] : sc->items) if (s.type == ItemType::Surface && (s.a == iid || s.b == iid)) users << s.name;
    if (!users.isEmpty()) v->addWidget(muted(QObject::tr("Drives: ") + users.join(", ")));
    if (it->raw.value("rib").isObject()) ribCurveProps(v, vp, sc, iid);
  } else if (it->type == ItemType::Surface) {
    v->addWidget(muted(QObject::tr("Driver curves:")));
    for (int cid : {it->a, it->b}) {
      const Item* c = sc->item(cid); auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 0, 0, 0);
      h->addWidget(new QLabel("∿ " + (c ? c->name : QObject::tr("(deleted)"))), 1); auto* b = button(QObject::tr("Select"));
      QObject::connect(b, &QPushButton::clicked, vp, [vp, sc, cid] { if (sc->item(cid)) vp->setSelectedItems({cid}); }); h->addWidget(b); v->addWidget(r);
    }
    int a = it->a, b = it->b;
    auto* both = button(QObject::tr("Select both curves")); QObject::connect(both, &QPushButton::clicked, vp, [vp, sc, a, b] { std::vector<int> l; for (int x : {a, b}) if (sc->item(x)) l.push_back(x); vp->setSelectedItems(l); });
    v->addWidget(both, 0, Qt::AlignLeft);
    // ⚠ an anchor of a side jutting in toward the other side (the red ring in the viewport): the wall dents there
    for (auto& k : vp->kinks()) { if (k.w != iid) continue;
      auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 0, 0, 0);
      auto* l = new QLabel(QObject::tr("⚠ Dent: %1, anchor %2 — breaks %3° toward the other side").arg(sc->item(k.c) ? sc->item(k.c)->name : QString()).arg(k.i + 1).arg(int(std::lround(k.turn))));
      l->setWordWrap(true); l->setStyleSheet("color:#ff8a7a"); h->addWidget(l, 1);
      r->setToolTip(QObject::tr("The curve turns %1° here, %2° more than at the anchors next to it, in toward the wall's other side — the wall dents there. Move the anchor out (or use Smoothing on the curve).").arg(int(std::lround(k.turn))).arg(int(std::lround(k.ex))));
      auto* sb = button(QObject::tr("Select")); int a = k.a; QObject::connect(sb, &QPushButton::clicked, vp, [vp, a] { vp->setSelectedAnchors({a}); }); h->addWidget(sb); v->addWidget(r); }
    v->addWidget(muted(QObject::tr("Flat wall of a lock. For a volume lock select 3+ curves and choose \"Volume lock (closed)\".")));
    QJsonObject p = P(*it); int segs = std::max(1, p.value("segs").toInt(1)); double bulge = p.value("bulge").toDouble(0);
    std::vector<int> selW; for (int x : S.items) if (const Item* s = sc->item(x); s && s->type == ItemType::Surface) selW.push_back(x);
    auto live = [vp, iid] { vp->liveWalls({iid}); };
    auto* sg = new SliderRow(QObject::tr("Width segments"), 0, 11, 1, segs - 1, [](double x) { return QString::number(int(x)); });
    QObject::connect(sg, &SliderRow::changed, vp, [sc, iid, live](double x) { setP(*sc, iid, "segs", int(x) + 1); live(); });
    QObject::connect(sg, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    v->addWidget(sg);
    auto segSlider = [&](const QString& label, const QString& key, const QString& tip) {
      auto* s = new SliderRow(label, -1, 1, 0.01, p.value(key).toDouble(0), pct); s->setReset(0); s->setTip(tip + QObject::tr(". Double-click — 0"));
      QObject::connect(s, &SliderRow::changed, vp, [sc, iid, key, live](double x) { setP(*sc, iid, key, x); live(); });
      QObject::connect(s, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
      v->addWidget(s);
    };
    const Item* ca = sc->item(it->a); const Item* cb2 = sc->item(it->b);
    if (segs > 1) segSlider(QObject::tr("Segment offset"), "segOff", QObject::tr("Shifts the segment lines towards %1 (−) or %2 (+), keeping their proportions").arg(ca ? ca->name : QObject::tr("the first curve"), cb2 ? cb2->name : QObject::tr("the second curve")));
    if (segs > 2) segSlider(QObject::tr("Spread from centre"), "segSpread", QObject::tr("Moves the segment lines towards the middle of the wall (−) or towards its edges (+)"));
    if (segs > 1) { auto* sb = button(QObject::tr("✂ Segments → curves%1").arg(selW.size() > 1 ? QObject::tr(" (%1 surfaces)").arg(selW.size()) : QString()), QObject::tr("Every segment line (with the bulge) becomes a curve; the surface is replaced by flat one-segment walls between them"));
      std::vector<int> list = selW.size() > 1 ? selW : std::vector<int>{iid};
      QObject::connect(sb, &QPushButton::clicked, vp, [vp, list] { const auto* W = &vp->hair().walls; vp->runEdit([list, W](Scene& s, Selection& sl, const CurveCache& c) { return walltools::segmentsToCurves(s, c, *W, sl, list); }, true); }); v->addWidget(sb, 0, Qt::AlignLeft); }
    auto* bu = new SliderRow(QObject::tr("Bulge"), -1, 1, 0.01, bulge, [](double x) { return QString::number(x, 'f', 2); }); bu->setReset(0); bu->setTip(QObject::tr("Double-click — 0 (flat, no volume)"));
    QObject::connect(bu, &SliderRow::changed, vp, [sc, iid, live](double x) { setP(*sc, iid, "bulge", x); live(); });
    QObject::connect(bu, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    v->addWidget(bu);
    auto* fl = check(QObject::tr("Bulge to the other side"), p.value("flip").toBool());
    QObject::connect(fl, &QCheckBox::toggled, vp, [vp, iid](bool x) { vp->runEdit([iid, x](Scene& s, Selection&, const CurveCache&) { setP(s, iid, "flip", x); return QString(); }, false); vp->liveWalls({iid}); });
    v->addWidget(fl);
    if (p.contains("jfade") && !p.value("jfade").isNull()) {                 // a junction wall
      auto* jf = new SliderRow(QObject::tr("Bulge fade (junction)"), 0.02, 1, 0.01, p.value("jfade").toDouble(), pct); jf->setReset(0.3);
      jf->setTip(QObject::tr("Share of the lock length over which the bulge fades in from the junction (shorter — sharper). Double-click — 30%"));
      QObject::connect(jf, &SliderRow::changed, vp, [sc, iid, live](double x) { setP(*sc, iid, "jfade", x); live(); });
      QObject::connect(jf, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
      v->addWidget(jf);
    }
    // a second surface on the same two curves (only while this one bulges)
    auto* b2 = check(QObject::tr("Second surface"), p.value("b2on").toBool(), bulge ? QObject::tr("A second wall between the same two curves with its own settings — together they make a closed lock (e.g. triangular)") : QObject::tr("Available when Bulge is not 0"));
    b2->setEnabled(bulge != 0);
    QObject::connect(b2, &QCheckBox::toggled, vp, [vp, iid](bool x) {
      vp->runEdit([iid, x](Scene& s, Selection&, const CurveCache&) {
        QJsonObject q = s.item(iid)->raw.value("p").toObject(); q.insert("b2on", x);
        if (x && (!q.contains("b2bulge") || q.value("b2bulge").isNull())) { q.insert("b2bulge", q.value("bulge").toDouble(0)); q.insert("b2segs", std::max(1, q.value("segs").toInt(1))); q.insert("b2flip", !q.value("flip").toBool()); }
        s.item(iid)->raw.insert("p", q); return QString(); }, false);
      vp->liveWalls({iid});
    });
    v->addWidget(b2);
    if (p.value("b2on").toBool() && bulge != 0) {
      v->addWidget(muted(QObject::tr("Second surface:")));
      auto* s2 = new SliderRow(QObject::tr("Width segments"), 0, 11, 1, std::max(1, p.value("b2segs").toInt(1)) - 1, [](double x) { return QString::number(int(x)); });
      QObject::connect(s2, &SliderRow::changed, vp, [sc, iid, live](double x) { setP(*sc, iid, "b2segs", int(x) + 1); live(); });
      QObject::connect(s2, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
      v->addWidget(s2);
      auto* b2b = new SliderRow(QObject::tr("Bulge"), -1, 1, 0.01, p.value("b2bulge").toDouble(), [](double x) { return QString::number(x, 'f', 2); }); b2b->setReset(0);
      QObject::connect(b2b, &SliderRow::changed, vp, [sc, iid, live](double x) { setP(*sc, iid, "b2bulge", x); live(); });
      QObject::connect(b2b, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
      v->addWidget(b2b);
      auto* f2 = check(QObject::tr("Bulge to the other side"), p.value("b2flip").toBool());
      QObject::connect(f2, &QCheckBox::toggled, vp, [vp, iid](bool x) { vp->runEdit([iid, x](Scene& s, Selection&, const CurveCache&) { setP(s, iid, "b2flip", x); return QString(); }, false); vp->liveWalls({iid}); });
      v->addWidget(f2);
    }
  } else if (it->type == ItemType::IMesh) {
    topoMeshProps(v, vp, sc, iid);                             // (both rooms: its polygroups, snapping, display, selection sets, transform)
  } else if (it->type == ItemType::Mesh || it->type == ItemType::IMesh) {
    int q = 0, t = 0, nv = it->poly ? int(it->poly->pos.size()) : 0;
    if (it->poly) for (int f = 0; f < it->poly->faceCount(); f++) { int n = it->poly->faceStart[size_t(f + 1)] - it->poly->faceStart[size_t(f)]; if (n == 4) q++; else t++; }
    if (it->type == ItemType::Mesh) v->addWidget(muted(QObject::tr("%1Vertices: %2, quads: %3, tris: %4").arg(it->raw.value("pending").toBool() ? QObject::tr("Awaiting confirmation. ") : QString()).arg(nv).arg(q).arg(t)));
    else {
      v->addWidget(muted(QObject::tr("Imported mesh: %1 vertices, %2 polygons. The file itself stays as it was — changes are modifiers.").arg(nv).arg(q + t)));
      auto* hc = check(QObject::tr("☺ Head"), it->headTag, QObject::tr("The head mark: every mesh marked so is the Head in the Hair room (one object there — not listed in Meshes); off — an ordinary mesh"));
      QString nm = it->name;
      QObject::connect(hc, &QCheckBox::toggled, vp, [vp, iid, nm](bool x) { vp->runEdit([iid, x, nm](Scene& s, Selection&, const CurveCache&) { Item* m = s.item(iid); m->headTag = x; m->raw.insert("headTag", x);
        return x ? QObject::tr("☺ %1 — a part of the Head").arg(nm) : QObject::tr("%1 — no longer the head").arg(nm); }, false); vp->refreshSceneBox(); vp->sceneChanged(false); });
      v->addWidget(hc);
    }
    // the display only: flat faces by default; an export is smooth either way
    std::vector<int> ms; for (int x : S.items) if (const Item* o = sc->item(x); o && o->type == it->type) ms.push_back(x);
    if (std::find(ms.begin(), ms.end(), iid) == ms.end()) ms.push_back(iid);
    int on = 0; for (int m : ms) on += sc->item(m)->raw.value("smoothN").toBool();
    auto* sn = check(ms.size() > 1 ? QObject::tr("Smooth normals (display) · %1 meshes").arg(ms.size()) : QObject::tr("Smooth normals (display)"), it->raw.value("smoothN").toBool(),
                     QObject::tr("Show the mesh with smoothed normals (off — flat faces). Only how it looks here: the exported mesh is smooth either way") + (ms.size() > 1 ? QObject::tr(" — applies to all %1 selected meshes").arg(ms.size()) : QString()));
    if (on > 0 && on < int(ms.size())) { sn->setTristate(true); sn->setCheckState(Qt::PartiallyChecked); }
    QObject::connect(sn, &QCheckBox::clicked, vp, [vp, ms](bool x) { vp->runEdit([ms, x](Scene& s, Selection&, const CurveCache&) { for (int m : ms) s.item(m)->raw.insert("smoothN", x); return QString(); }, false); vp->liveView(); });
    v->addWidget(sn);
    // (the clean-up the conversion now does itself — for the meshes made before it: no edge of three polygons or more,
    // no parts touching only at a vertex, no doubled polygons)
    { auto* cb = button(ms.size() > 1 ? QObject::tr("🧹 Clean the geometry · %1 meshes").arg(ms.size()) : QObject::tr("🧹 Clean the geometry"),
                        QObject::tr("Make the mesh manifold, as a conversion now leaves it: an edge of three polygons or more split into pairs that carry on into each other, a vertex where parts only touch split, doubled polygons out — nothing else changes"));
      QObject::connect(cb, &QPushButton::clicked, vp, [vp, ms] { vp->runEdit([ms](Scene& s, Selection&, const CurveCache&) {
        int fixed = 0, changes = 0;
        for (int m : ms) { Item* x = s.item(m); if (!x || !x->poly) continue; auto src = x->base ? x->base : x->poly; auto p = std::make_shared<PolyMesh>(*src); const int n = tedit::makeManifold(*p); if (!n) continue;
          const bool sameShown = x->poly == src; if (x->base) x->base = p; if (sameShown || !x->base) x->poly = p; fixed++; changes += n; }
        return fixed ? QObject::tr("Clean geometry: %1 mesh%2 made manifold (%3 changes)").arg(fixed).arg(fixed == 1 ? "" : "es").arg(changes) : QObject::tr("Clean geometry: nothing to clean — already manifold"); }); });
      v->addWidget(cb); }
    if (it->type == ItemType::IMesh) imeshExtras(v, vp, sc, iid);   // (polygroups, Thickness & mask, its modifiers)
    if (!it->raw.value("pending").toBool() && !meshmods::heavy(*it)) { auto* mc = button(QObject::tr("〰 Mesh → curves & surfaces"), QObject::tr("Curves along the mesh's locks and walls between them"));
      QObject::connect(mc, &QPushButton::clicked, vp, [vp, iid] { vp->meshToCurves(iid); }); v->addWidget(mc, 0, Qt::AlignLeft); }
  } else if (it->type == ItemType::Tip || it->type == ItemType::PInf || it->type == ItemType::Even || it->type == ItemType::Guide) {
    shellModProps(v, vp, sc, iid);
  } else if (it->type == ItemType::Crown) {
    crownProps(v, vp, sc, iid);
  } else if (it->type == ItemType::Braid) {
    braidProps(v, vp, sc, iid);
  } else if (it->type == ItemType::Tube) {
    tubeProps(v, vp, sc, iid);
  } else if (it->type == ItemType::XMod) {
    xmodProps(v, vp, sc, iid);
  } else {
    v->addWidget(muted(QObject::tr("The settings of this kind come with its tool in the next parts of this rewrite.")));
  }
  return finish();
}

}  // namespace hm
