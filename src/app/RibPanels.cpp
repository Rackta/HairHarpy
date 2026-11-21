#include "app/RibPanels.h"
#include "ui/Theme.h"
#include "app/ToolPanels.h"
#include "scene/Edit.h"
#include "scene/Rib.h"
#include "view/Viewport.h"
#include <QCheckBox>
#include <QHBoxLayout>
#include <QTimer>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <cmath>

namespace hm {

namespace {
QLabel* h4(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setWordWrap(true); return l; }
QLabel* muted(const QString& t) { auto* l = new QLabel(t); l->setProperty("mut", true); l->setWordWrap(true); return l; }
QPushButton* button(const QString& t, const QString& tip = {}, bool primary = false) {
  auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); if (primary) b->setProperty("primary", true); return b;
}
QString pct(double v) { return QString("%1%").arg(int(std::lround(v * 100))); }
QString fmtLen(double v) {
  if (v == 0) return "0.00";
  int e = int(std::floor(std::log10(std::abs(v))));
  if (e < -6 || e >= 3) { QString s = QString::number(v, 'e', 2); s.replace(QRegularExpression("e([+-])0*(\\d)"), "e\\1\\2"); return s; }
  return QString::number(v, 'f', std::max(0, 2 - e));
}
QWidget* hbox(std::initializer_list<QWidget*> ws) { auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 4, 0, 0); h->setSpacing(4); for (QWidget* x : ws) h->addWidget(x); h->addStretch(1); return w; }
QJsonObject ribOf(const Item& c) { return c.raw.value("rib").toObject(); }
void setRib(Scene& sc, int c, const QString& k, const QJsonValue& v) { Item* it = sc.item(c); QJsonObject r = ribOf(*it); if (v.isNull()) r.remove(k); else r.insert(k, v); it->raw.insert("rib", r); }
// width / rotation at every anchor (an anchor without its own takes them from its neighbours)
std::pair<std::vector<double>, std::vector<double>> anchorVals(const Item& c, double scale) {
  QJsonObject K = ribOf(c).value("k").toObject(); int n = int(c.anchors.size()); std::vector<int> known;
  for (int i = 0; i < n; i++) if (K.contains(QString::number(c.anchors[size_t(i)]))) known.push_back(i);
  auto at = [&](int i, const char* key, double def) {
    if (known.empty()) return def; int lo = -1, hi = -1; for (int j : known) { if (j <= i) lo = j; if (j >= i && hi < 0) hi = j; }
    auto v = [&](int j) { return K.value(QString::number(c.anchors[size_t(j)])).toObject().value(key).toDouble(0); };
    if (lo < 0) return v(hi); if (hi < 0 || hi == lo) return v(lo); return v(lo) + (v(hi) - v(lo)) * (i - lo) / (hi - lo);
  };
  std::vector<double> w, r; for (int i = 0; i < n; i++) { w.push_back(at(i, "w", scale * 0.02)); r.push_back(at(i, "r", 0)); }
  return {w, r};
}
}  // namespace

void setRibRootDir(Viewport* vp, int c, const QString& md) {
  vp->runEdit([c, md, vp](Scene& s, Selection&, const CurveCache&) -> QString {
    Item* k = s.item(c); if (!k || k->anchors.empty()) return "!";
    const auto ends = rib::ribEnds(s, *k); if (ends.empty()) return "!" + QObject::tr("This lock doesn't start from anchors");
    const int root = k->anchors[0];
    setRib(s, c, "rootDir", md == "along" ? QJsonValue() : QJsonValue(md));
    if (md == "free") { k->ovr.erase(root); return QObject::tr("Lock root: free — toward its next point"); }
    const auto d = rib::trunkDir(s, ends); if (!d) return "!" + QObject::tr("No direction from the anchors it starts from");
    double len = 0; if (auto f = k->ovr.find(root); f != k->ovr.end() && f->second.out) len = f->second.out->length();
    if (!(len > 0)) len = std::max(rib::ribRoot(s, ends, d).w0 * 2.5, vp->camera().sceneScale * 0.01);
    k->ovr[root].out = *d * (md == "flip" ? -len : len);
    return md == "flip" ? QObject::tr("Lock root: the other way") : QObject::tr("Lock root: along the curves");
  }, true);
  vp->liveRibs();
}

// ⧉ Detach into a new group (21.11, the author): the lock leaves the group of the curves it was drawn from for a group of its own
// (beside that one, its symmetry taken over) — hidden, isolated, picked as a lock by itself; off — back into the group it came from,
// the emptied group gone. (Its root stays on the anchors it started from.)
void setRibDetached(Viewport* vp, int c, bool on) {
  vp->runEdit([c, on](Scene& s, Selection& sl, const CurveCache& cc) -> QString {
    Item* k = s.item(c); if (!k) return "!";
    auto kids = [&s](int p) -> std::vector<int>& { return p >= 0 ? s.item(p)->children : s.roots; };
    auto moveTo = [&](int np, int after) { auto& o = kids(k->parent); o.erase(std::remove(o.begin(), o.end(), c), o.end()); k->parent = np;
      auto& a = kids(np); auto at = std::find(a.begin(), a.end(), after); a.insert(at == a.end() ? a.end() : at + 1, c); };
    const QJsonValue from = ribOf(*k).value("detachFrom");
    if (on) {
      if (from.isDouble()) return "!";
      const int old = k->parent; const Item* og = s.item(old); const int gp = og ? og->parent : -1;
      Item& g = Viewport::newGroup(s, gp, false); const int gid = g.id; g.name = QObject::tr("%1 lock").arg(k->name);
      if (og) for (const char* key : {"sym", "spos", "logic", "gpivot", "vsym", "q"}) if (og->raw.contains(key)) g.raw.insert(key, og->raw.value(key));
      { auto& a = kids(gp); a.erase(std::remove(a.begin(), a.end(), gid), a.end()); auto at = std::find(a.begin(), a.end(), old); a.insert(at == a.end() ? a.end() : at + 1, gid); }   // (right after the group it left)
      moveTo(gid, -1); setRib(s, c, "detachFrom", old); s.reindex();
      sl.anchors.clear(); sl.items = {c};
      return QObject::tr("%1: detached into the new group %2").arg(k->name, s.item(gid)->name);
    }
    if (!from.isDouble()) return "!";
    const int back = from.toInt(), was = k->parent; const Item* bg = s.item(back);
    if (back >= 0 && (!bg || bg->type != ItemType::Group)) { setRib(s, c, "detachFrom", QJsonValue()); return QObject::tr("The group it came from is gone — the lock stays in its own"); }   // (-1: it came from the top level)
    moveTo(back, -1); setRib(s, c, "detachFrom", QJsonValue()); s.reindex();
    if (const Item* wg = s.item(was); wg && wg->type == ItemType::Group && wg->children.empty()) { Selection d; d.items = {was}; edit::deleteItems(s, cc, d); s.reindex(); }
    sl.anchors.clear(); sl.items = {c};
    return QObject::tr("%1: back in %2").arg(k->name, back >= 0 ? s.item(back)->name : QObject::tr("the top level"));
  }, true);
  vp->liveRibs();
}

void ribAnchorProps(QVBoxLayout* v, Viewport* vp, Scene* sc) {
  struct AC { int a, c; }; std::vector<AC> list;
  for (int a : vp->selection().anchors) for (int c : edit::curvesWith(*sc, a)) if (rib::isRib(*sc->item(c))) list.push_back({a, c});
  if (list.empty()) return;
  const double scale = vp->camera().sceneScale;
  const AC last = list.back(); const Item& lc = *sc->item(last.c); auto cur = anchorVals(lc, scale);
  int li = int(std::find(lc.anchors.begin(), lc.anchors.end(), last.a) - lc.anchors.begin());
  v->addWidget(h4(QObject::tr("▬ Flat lock at the anchor") + (list.size() > 1 ? QObject::tr("s (%1)").arg(list.size()) : QString())));
  auto ends = rib::ribEnds(*sc, lc);
  if (!ends.empty() && li == 0) { v->addWidget(muted(QObject::tr("The root spans the %1 anchors it started from — its shape follows them.").arg(ends.size()))); return; }
  auto set = [vp, sc, list, scale](const char* key, double val) {
    for (auto& x : list) { Item* c = sc->item(x.c); auto vals = anchorVals(*c, scale); int i = int(std::find(c->anchors.begin(), c->anchors.end(), x.a) - c->anchors.begin());
      QJsonObject r = ribOf(*c), K = r.value("k").toObject(); QString id = QString::number(x.a);
      QJsonObject o = K.contains(id) ? K.value(id).toObject() : QJsonObject{{"w", vals.first[size_t(i)]}, {"r", vals.second[size_t(i)]}};
      o.insert(key, val); K.insert(id, o); r.insert("k", K); c->raw.insert("rib", r); }
    vp->liveRibs();
  };
  double w = cur.first[size_t(li)], wmax = std::max(w * 3, scale * 0.05);
  auto* rw = new SliderRow(QObject::tr("Width"), 0, wmax, wmax / 400, w, [](double x) { return fmtLen(x * 2); });
  rw->setTip(QObject::tr("Width of the lock at this anchor (Profile smoothing evens it out along the lock)"));
  QObject::connect(rw, &SliderRow::changed, vp, [set](double x) { set("w", x); }); QObject::connect(rw, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
  auto* rr = new SliderRow(QObject::tr("Rotation"), -180, 180, 1, cur.second[size_t(li)], [](double x) { return QString("%1°").arg(int(std::lround(x))); });
  rr->setTip(QObject::tr("Turns the lock around its middle line at this anchor (in between it turns gradually)"));
  QObject::connect(rr, &SliderRow::changed, vp, [set](double x) { set("r", x); }); QObject::connect(rr, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
  v->addWidget(rw); v->addWidget(rr);
}

void ribCurveProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int c) {
  const Item& it = *sc->item(c); auto ne = rib::ribEnds(*sc, it); bool vol = ne.size() > 2; QJsonObject R = ribOf(it);
  v->addWidget(h4(vol ? QObject::tr("◉ Volume branch (%1 anchors)").arg(ne.size()) : QObject::tr("▬ Flat lock")));
  auto row = [&](const QString& label, double lo, double hi, double step, const QString& key, double def, std::function<QString(double)> fmt, const QString& tip) {
    auto* r = new SliderRow(label, lo, hi, step, R.value(key).isDouble() ? R.value(key).toDouble() : def, fmt); r->setTip(tip);
    QObject::connect(r, &SliderRow::changed, vp, [vp, sc, c, key](double x) { setRib(*sc, c, key, x); vp->liveRibs(); });
    QObject::connect(r, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    v->addWidget(r);
  };
  row(QObject::tr("Profile smoothing"), 0, 1, 0.01, "smooth", 0.4, pct, QObject::tr("Evens out the width and the rotation along the lock: 0 — exactly as set at the anchors, more — smoother changes between them"));
  row(QObject::tr("Tip"), 0, 0.8, 0.01, "tip", 0.25, pct, QObject::tr("Share of the length over which the tip narrows to a point (0 — a cut end)"));
  // ⤴ the root's direction (a lock started between anchors): on along the curves it starts from (as made), the other way, or
  // free — no handle of its own, the root turns to its next point (20.11, the author: "the lock drawn by a curve — a change of
  // direction: by the bend it took its direction wrongly, went up at an angle — a kink")
  if (!ne.empty() && !it.anchors.empty()) {
    const QString mode = R.value("rootDir").toString("along");
    const QString cap = mode == "flip" ? QObject::tr("the other way") : mode == "free" ? QObject::tr("free") : QObject::tr("along the curves");
    auto* db = new QPushButton(QObject::tr("⤴ Root direction: %1 ▾").arg(cap)); db->setFocusPolicy(Qt::NoFocus);
    db->setToolTip(QObject::tr("Where the lock leaves its root: on along the curves it starts from, the other way, or free (toward its next point)"));
    QObject::connect(db, &QPushButton::clicked, vp, [vp, sc, c, db, mode] {
      QMenu m;
      auto apply = [vp, c](const QString& md) { setRibRootDir(vp, c, md); };
      for (auto [md, lab] : {std::pair<QString, QString>{"along", QObject::tr("Along the curves it starts from")}, {"flip", QObject::tr("The other way")}, {"free", QObject::tr("Free — toward its next point")}}) {
        QAction* a = m.addAction(lab); a->setCheckable(true); a->setChecked(md == mode); QObject::connect(a, &QAction::triggered, vp, [apply, md] { apply(md); }); }
      m.exec(db->mapToGlobal(QPoint(0, db->height())));
      (void)sc; });
    v->addWidget(db, 0, Qt::AlignLeft);
  }
  { auto* dg = new QCheckBox(QObject::tr("Detach into a new group")); dg->setFocusPolicy(Qt::NoFocus); dg->setChecked(R.value("detachFrom").isDouble());
    dg->setToolTip(QObject::tr("The lock goes into a group of its own beside the one it was drawn in (its symmetry taken over); off — back where it was"));
    QObject::connect(dg, &QCheckBox::toggled, vp, [vp, c](bool on) { QTimer::singleShot(0, vp, [vp, c, on] { setRibDetached(vp, c, on); }); });
    v->addWidget(dg, 0, Qt::AlignLeft); }
  if (!vol) {
    auto brushBtn = [&](const QString& kind, const QString& label, const QString& tip, std::vector<QString> clearKeys) {
      auto& b = vp->ribBrush; bool on = b.on && b.kind == kind;
      auto* pb = new QPushButton(label + QObject::tr(" brush")); pb->setIcon(ui::icon("brush_paint")); pb->setIconSize(QSize(18, 18)); pb->setCheckable(true); pb->setChecked(on); pb->setToolTip(tip);
      QObject::connect(pb, &QPushButton::clicked, vp, [vp, kind, on] { vp->ribBrush.on = !on; vp->ribBrush.kind = kind; vp->liveRibs(); emit vp->brushChanged(); });
      auto* cl = button(QObject::tr("Clear"), QObject::tr("Remove all painted weights of this section"));
      QObject::connect(cl, &QPushButton::clicked, vp, [vp, c, clearKeys] { vp->runEdit([c, clearKeys](Scene& s, Selection&, const CurveCache&) { for (auto& k : clearKeys) setRib(s, c, k, QJsonValue()); return QString(); }, false); vp->liveRibs(); });
      v->addWidget(hbox({pb, cl}));
      if (on) {
        bool off = kind == "off";
        auto* r = new SliderRow(off ? QObject::tr("Intensity") : QObject::tr("Strength"), 0.05, 1, 0.01, off ? b.intensity : b.strength, pct); r->setTip(QObject::tr("How much one stroke paints"));
        QObject::connect(r, &SliderRow::changed, vp, [vp, off](double x) { (off ? vp->ribBrush.intensity : vp->ribBrush.strength) = x; });
        v->addWidget(r);
      }
    };
    v->addWidget(h4(QObject::tr("⬆ Extrude")));
    auto* inv = new QPushButton(QObject::tr("⇅ Invert extrude")); inv->setCheckable(true); inv->setChecked(R.value("extInv").toBool()); inv->setToolTip(QObject::tr("Push the ribbon out to the other side"));
    QObject::connect(inv, &QPushButton::clicked, vp, [vp, c](bool x) { vp->runEdit([c, x](Scene& s, Selection&, const CurveCache&) { setRib(s, c, "extInv", x); return QString(); }, false); vp->liveRibs(); });
    v->addWidget(inv, 0, Qt::AlignLeft);
    row(QObject::tr("Depth"), 0, 3, 0.01, "extH", 1, [](double x) { return QString::number(x, 'f', 2) + "×"; }, QObject::tr("How far 100% of the Extrude weight pushes the ribbon out (in half widths of the lock)"));
    row(QObject::tr("Fade to tip"), 0, 1, 0.01, "extFade", 0, pct, QObject::tr("Share of the length at the tip over which the extrusion dies out"));
    brushBtn("ext", QObject::tr("Extrude"), QObject::tr("LMB — extrude, Ctrl — erase, Shift — smooth (Ctrl+RMB drag — brush size)"), {"ext"});
    v->addWidget(h4(QObject::tr("↔ Offset")));
    row(QObject::tr("Fade to tip"), 0, 1, 0.01, "offFade", 0, pct, QObject::tr("Share of the length at the tip over which the offset dies out"));
    brushBtn("off", QObject::tr("Offset"), QObject::tr("LMB — move the extruded edge nearest the brush in (orange; at 100% to the other edge), Ctrl — out (blue; up to one more width), Ctrl+Alt — erase, Shift — smooth. The two edges are independent"), {"off", "offL", "offR"});
    v->addWidget(h4(QObject::tr("⋎ Glue faces")));
    brushBtn("glue", QObject::tr("Glue"), QObject::tr("LMB — close the top face of the extrusion (green; at 100% its two edges become one — a Y profile), Ctrl — erase, Shift — smooth"), {"glue"});
    v->addWidget(muted(QObject::tr("Brushes work on the selected flat lock; click a brush again to leave it. Extrude must be painted first.")));
  }
  auto* cv = button(vol ? QObject::tr("✔ Convert to %1 curves & walls").arg(ne.size()) : QObject::tr("✔ Convert to 2 curves & a wall"),
                    vol ? QObject::tr("A curve from every anchor the branch started on, along the branch (each goes on back along its anchor's curve — a common trunk), walls round them (closed). The middle curve is deleted")
                        : QObject::tr("Two curves along the edges and a flat NURBS wall between them; started from two anchors — the curves start on them. The middle curve is deleted"), true);
  QObject::connect(cv, &QPushButton::clicked, vp, [vp, c] { vp->ribConvert(c); });
  auto* pl = button(QObject::tr("Plain curve"), QObject::tr("Drop the flat lock, keep the curve"));
  QObject::connect(pl, &QPushButton::clicked, vp, [vp, c] { vp->runEdit([c](Scene& s, Selection&, const CurveCache&) { s.item(c)->raw.remove("rib"); s.item(c)->hasRib = false; return QString(); }, false); vp->liveRibs(); });
  v->addWidget(hbox({cv, pl}));
}

}  // namespace hm
