#include "app/ShellPanels.h"
#include "app/ToolPanels.h"
#include "scene/Convert.h"
#include "scene/Edit.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

namespace hm {

namespace {
const double PI = 3.14159265358979323846;
QLabel* h4(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); return l; }
QLabel* muted(const QString& t) { auto* l = new QLabel(t); l->setProperty("mut", true); l->setWordWrap(true); return l; }
QLabel* warn(const QString& t) { auto* l = muted(t); l->setStyleSheet("color:#e0b060"); return l; }
QString pct(double v) { return QString("%1%").arg(int(std::lround(v * 100))); }
QString times(double v) { return "×" + QString::number(v, 'f', 2); }
QString prec(double v, int p) { if (v == 0) return QString::number(0.0, 'f', p - 1); int e = int(std::floor(std::log10(std::abs(v)))); return e < -6 || e >= p ? QString::number(v, 'e', p - 1) : QString::number(v, 'f', std::max(0, p - 1 - e)); }
QPushButton* button(const QString& t, const QString& tip = {}, bool primary = false) { auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); if (primary) b->setProperty("primary", true); return b; }
const Item* parentOf(const Scene& sc, const Item* it) { return it && it->parent >= 0 ? sc.item(it->parent) : nullptr; }
const Item* modCocoon(const Scene& sc, const Item* m) { const Item* p = parentOf(sc, m); return p && p->type == ItemType::Cocoon ? p : nullptr; }
bool painted(const Item& m, bool signedMask) {
  for (const char* f : {"cmask", "emask"}) for (const QJsonValue& x : m.raw.value(f).toArray()) if (signedMask ? std::abs(x.toDouble()) > 1e-3 : x.toDouble() > 0) return true;
  return false;
}
double headH(const Scene& sc) { Box3 hb; for (auto& [id, it] : sc.items) if (it.headTag && it.poly) hb.unite(it.poly->bounds()); double h = hb.empty() ? sc.bounds().size().y : hb.size().y; return h > 0 ? h : 1; }
void setRawP(Scene& sc, int id, const QString& k, const QJsonValue& v) { Item* it = sc.item(id); QJsonObject p = it->raw.value("p").toObject(); p.insert(k, v); it->raw.insert("p", p); }
// a row of the panel: a label and its buttons (the tabs)
QWidget* modes(const QString& label, const std::vector<std::tuple<QString, QString, QString>>& list, const QString& cur, std::function<void(const QString&)> set) {
  auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 0, 0, 3); h->setSpacing(4);
  if (!label.isEmpty()) { auto* l = new QLabel(label); l->setStyleSheet("color:#9a9a9a;font-size:11px"); l->setFixedWidth(74); h->addWidget(l); }
  for (auto& [k, lab, tip] : list) { auto* b = button(lab, tip); b->setCheckable(true); b->setChecked(k == cur); QString kk = k; QObject::connect(b, &QPushButton::clicked, r, [set, kk] { set(kk); }); h->addWidget(b, 1); }
  return r;
}
QFrame* hline() { auto* f = new QFrame; f->setFixedHeight(1); f->setStyleSheet("background:#4a4a4a;margin:6px 0 8px"); return f; }

// a scene disc's cocoons (discTakes): dropped here from the outliner's Cocoons section, ✖ — off the list
class DropBox : public QFrame {
public:
  std::function<void(const std::vector<int>&)> dropped;
  DropBox() { setAcceptDrops(true); setObjectName("disccc"); setStyleSheet("#disccc{border:1px dashed #4a5a5c;border-radius:4px}"); }
protected:
  void dragEnterEvent(QDragEnterEvent* e) override { if (e->mimeData()->data("application/x-hm-outliner").startsWith("2:")) { e->acceptProposedAction(); setStyleSheet("#disccc{border:1px dashed #8fe8b0;border-radius:4px}"); } }
  void dragLeaveEvent(QDragLeaveEvent*) override { setStyleSheet("#disccc{border:1px dashed #4a5a5c;border-radius:4px}"); }
  void dropEvent(QDropEvent* e) override {
    setStyleSheet("#disccc{border:1px dashed #4a5a5c;border-radius:4px}");
    QString s = QString::fromUtf8(e->mimeData()->data("application/x-hm-outliner")); std::vector<int> ids;
    for (const QString& x : s.section(':', 1).split(',', Qt::SkipEmptyParts)) ids.push_back(x.toInt());
    e->acceptProposedAction(); if (dropped && !ids.empty()) dropped(ids);
  }
};

void refresh(Viewport* vp) { emit vp->brushChanged(); }       // (the panels drawn anew)

// ---- Density ----
void tipControls(QVBoxLayout* v, Viewport* vp, Scene* sc, int k) {
  double val = k >= 0 ? sc->item(k)->raw.value("p").toObject().value("dens").toDouble(1.5) : vp->tipDefaults.value("dens").toDouble(1.5);
  auto* s = new SliderRow(QObject::tr("Density"), 0.25, 4, 0.05, val, times); s->setReset(1.5);
  s->setTip(QObject::tr("Loop density where the mask is painted: ×2 — twice as many loops, ×0.5 — half as many (in between — by the painted strength). Double-click — ×1.5"));
  QObject::connect(s, &SliderRow::changed, vp, [vp, sc, k](double x) { if (k < 0) { vp->tipDefaults.insert("dens", x); return; } setRawP(*sc, k, "dens", x); vp->touchShellMods({k}); });
  QObject::connect(s, &SliderRow::released, vp, [vp, k](double) { if (k >= 0) vp->commitLive(); });
  v->addWidget(s);
}
void brushControls(QVBoxLayout* v, Viewport* vp, const QString& tab, const std::vector<int>& ks) {
  bool tip = tab == "tip"; Viewport::ShellBrush& B = tip ? vp->tipBrush : vp->pinfBrush;
  bool on = vp->tool() == "guide" && vp->shTab == tab && vp->shellBrushActive();
  auto* bar = new QWidget; auto* h = new QHBoxLayout(bar); h->setContentsMargins(0, 6, 0, 0); h->setSpacing(4);
  auto* pb = button(tip ? QObject::tr("Density brush") : QObject::tr("Influence brush"),
                    tip ? QObject::tr("LMB — paint where the density works, Ctrl — erase, Shift — smooth (both masks).\nCtrl+Shift — the second mode: Even spacing (cyan) — the rings there equally apart (no crowding on bends or toward the tip); Ctrl+Shift+Alt — erase it.\nWith density modifiers selected only their locks are painted")
                        : QObject::tr("LMB — weaken the parent's influence (red), Ctrl — inverted: add it (green), Ctrl+Alt — erase, Shift — smooth. With Parent influence modifiers selected only their cocoons are painted"));
  pb->setCheckable(true); pb->setChecked(on); pb->setIcon(ui::icon("brush_paint")); pb->setIconSize(QSize(18, 18));
  QObject::connect(pb, &QPushButton::clicked, vp, [vp, tab, on] {
    Viewport::ShellBrush& b = tab == "tip" ? vp->tipBrush : vp->pinfBrush;
    if (on) b.on = false; else { vp->tipBrush.on = vp->pinfBrush.on = false; b.on = true; vp->shTab = tab; if (vp->tool() != "guide") vp->setTool("guide"); }
    vp->shellChanged(); refresh(vp);
  });
  auto* cl = button(ks.size() == 1 ? QObject::tr("Clear mask") : QObject::tr("Clear all masks"), QObject::tr("Remove the painted mask — the modifier does nothing until painted again"));
  QObject::connect(cl, &QPushButton::clicked, vp, [vp, ks] { vp->clearMasks(ks); });
  h->addWidget(pb); h->addWidget(cl); h->addStretch(1); v->addWidget(bar);
  if (!on) return;
  auto* st = new SliderRow(tip ? QObject::tr("Strength") : QObject::tr("Brush"), 0, 1, 0.01, B.strength, pct);
  QObject::connect(st, &SliderRow::changed, vp, [vp, tab](double x) { (tab == "tip" ? vp->tipBrush : vp->pinfBrush).strength = x; });
  auto* rr = new SliderRow(QObject::tr("Radius"), 8, 400, 1, vp->brushR, [](double x) { return QString("%1px").arg(int(std::lround(x))); });
  QObject::connect(rr, &SliderRow::changed, vp, [vp](double x) { vp->brushR = x; vp->shellChanged(); });
  v->addWidget(st); v->addWidget(rr);
}
// ---- Parent influence ----
void pinfControls(QVBoxLayout* v, Viewport* vp, Scene* sc, int k) {
  double val = k >= 0 ? sc->item(k)->raw.value("p").toObject().value("str").toDouble(1) : vp->pinfDefaults.value("str").toDouble(1);
  auto* s = new SliderRow(QObject::tr("Strength"), 0, 1, 0.01, val, pct); s->setReset(1);
  s->setTip(QObject::tr("How strongly the painted mask acts (× its painted value). Double-click — 100%"));
  QObject::connect(s, &SliderRow::changed, vp, [vp, sc, k](double x) { if (k < 0) { vp->pinfDefaults.insert("str", x); return; } setRawP(*sc, k, "str", x); vp->touchShellMods({k}); });
  QObject::connect(s, &SliderRow::released, vp, [vp, k](double) { if (k >= 0) vp->commitLive(); });
  v->addWidget(s);
}

// ---- guides ----
// the disc's ring moved radially by the change (hand deformations kept; a ring never shrinks to nothing)
void setDiscRadius(Viewport* vp, Scene& sc, int gid, bool out, double r) {
  Item* g = sc.item(gid); QJsonArray rs = g->raw.value("rings").toArray(); if (rs.size() < 2) return;
  Item* ring = sc.item(rs[out ? 0 : 1].toInt()); const Item* other = sc.item(rs[out ? 1 : 0].toInt()); if (!ring) return;
  double eps = sceneScale(sc) * 0.002; r = std::max(r, eps);
  QJsonObject p = g->raw.value("p").toObject(); QString key = out ? "rOut" : "rIn"; double dr = r - p.value(key).toDouble(); p.insert(key, r); g->raw.insert("p", p);
  Vec3 C; int n = 0; for (int k = 0; k < 2; k++) if (const Item* c = sc.item(rs[k].toInt())) for (int a : c->anchors) { C += sc.anchor(a)->pos; n++; }
  C = C / double(std::max(n, 1));
  const Item* o0 = sc.item(rs[0].toInt()); Vec3 N;
  for (size_t i = 0; o0 && i < o0->anchors.size(); i++) { const Vec3 &a = sc.anchor(o0->anchors[i])->pos, &b = sc.anchor(o0->anchors[(i + 1) % o0->anchors.size()])->pos; N.x += (a.y - b.y) * (a.z + b.z); N.y += (a.z - b.z) * (a.x + b.x); N.z += (a.x - b.x) * (a.y + b.y); }
  if (N.lengthSq() < 1e-16) N = {0, 1, 0}; N = N.normalized();
  Vec3 u = std::abs(N.y) > 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0); u = (u - N * u.dot(N)).normalized(); Vec3 v = N.cross(u);
  auto radial = [&](const Vec3& q) { Vec3 d = q - C; return d - N * d.dot(N); };
  for (size_t k = 0; k < ring->anchors.size(); k++) {
    Vec3& pos = sc.anchor(ring->anchors[k])->pos; Vec3 d = radial(pos); double len = d.length();
    if (len < eps * 0.5) {                                          // (collapsed: back to the centre, then out)
      Vec3 o = other && k < other->anchors.size() ? radial(sc.anchor(other->anchors[k])->pos) : Vec3();
      d = o.lengthSq() > eps * eps * 0.25 ? o : u * std::cos(k * PI / 3) + v * std::sin(k * PI / 3);
      pos = pos - radial(pos); len = 0;
    }
    pos = pos + d.normalized() * (std::max(len + dr, eps) - len);
  }
  vp->liveAnchors(ring->anchors);
}
void resetDisc(Scene& sc, int gid) {
  Item* g = sc.item(gid); QJsonArray rs = g->raw.value("rings").toArray(); if (rs.size() < 2) return;
  Vec3 C; int n = 0; for (int k = 0; k < 2; k++) if (const Item* c = sc.item(rs[k].toInt())) for (int a : c->anchors) { C += sc.anchor(a)->pos; n++; }
  C = C / double(std::max(n, 1));
  Item* o0 = sc.item(rs[0].toInt()); Vec3 N;
  for (size_t i = 0; i < o0->anchors.size(); i++) { const Vec3 &a = sc.anchor(o0->anchors[i])->pos, &b = sc.anchor(o0->anchors[(i + 1) % o0->anchors.size()])->pos; N.x += (a.y - b.y) * (a.z + b.z); N.y += (a.z - b.z) * (a.x + b.x); N.z += (a.x - b.x) * (a.y + b.y); }
  if (N.lengthSq() < 1e-16) N = {0, 1, 0}; N = N.normalized();
  Vec3 d0 = sc.anchor(o0->anchors[0])->pos - C; d0 = d0 - N * d0.dot(N);
  Vec3 u = d0.lengthSq() > 1e-12 ? d0.normalized() : (std::abs(N.y) > 0.9 ? Vec3(1, 0, 0) : Vec3(0, 1, 0)); if (d0.lengthSq() <= 1e-12) u = (u - N * u.dot(N)).normalized();
  Vec3 v = N.cross(u); QJsonObject p = g->raw.value("p").toObject();
  for (int ri = 0; ri < 2; ri++) { Item* c = sc.item(rs[ri].toInt()); double r = p.value(ri == 0 ? "rOut" : "rIn").toDouble();
    for (size_t k = 0; k < c->anchors.size(); k++) { Anchor* a = sc.anchor(c->anchors[k]); a->pos = C + u * (r * std::cos(k * PI / 3)) + v * (r * std::sin(k * PI / 3)); a->smooth = 1; }
    c->ovr.clear(); }
}

void guideControls(QVBoxLayout* v, Viewport* vp, Scene* sc, int gid) {
  const Item* g = gid >= 0 ? sc->item(gid) : nullptr;
  QJsonObject tgt = g ? g->raw.value("p").toObject() : vp->guideDefaultsNow();
  if (g) {
    const Item* k = modCocoon(*sc, g); bool disc = g->raw.value("kind").toString() == "disc";
    if (disc) v->addWidget(muted(k ? QObject::tr("NURBS disc on %1. E / R / T move, rotate, transform it, the Brush shapes it.").arg(k->name)
                                   : QObject::tr("NURBS disc in the scene — a ring lies on it on every cocoon it crosses (where a cocoon takes its parent's / a linked one's loops, theirs rule). E / R / T move, rotate, transform it, the Brush shapes it.")));
    else if (g->raw.value("on").isObject()) v->addWidget(muted(k ? QObject::tr("Guide line across %1 — a ring lies in its plane. It follows the cocoon; to move it, draw a new one and delete this.").arg(k->name) : QObject::tr("Guide line on no cocoon — it does nothing.")));
    else v->addWidget(muted(QObject::tr("Guide line across %1 curves%2.").arg(g->raw.value("hits").toArray().size()).arg(k ? QObject::tr(" — on %1").arg(k->name) : QObject::tr(" (an older one, on no cocoon — it does nothing)"))));
    // a scene disc's cocoons: left out of its calculation, or only these
    if (disc && !k) {
      auto* box = new DropBox; auto* bv = new QVBoxLayout(box); bv->setContentsMargins(6, 5, 6, 5); bv->setSpacing(2);
      QJsonObject p = g->raw.value("p").toObject(); bool only = p.value("ccOnly").toBool();
      std::vector<int> L; for (const QJsonValue& x : p.value("cc").toArray()) { int id = x.toObject().value("id").toInt(); if (const Item* c = sc->item(id); c && c->type == ItemType::Cocoon) L.push_back(id); }
      auto setList = [vp, gid](const std::vector<int>& list) {
        vp->runEdit([gid, list](Scene& s, Selection&, const CurveCache&) { QJsonArray a; for (int id : list) a.append(QJsonObject{{"id", id}}); setRawP(s, gid, "cc", a); return QString(); }, false);
        vp->touchShellMods({gid});
      };
      auto* hd = new QWidget; auto* hh = new QHBoxLayout(hd); hh->setContentsMargins(0, 0, 0, 0);
      auto* ht = new QLabel(only ? QObject::tr("Cocoons — only these") : QObject::tr("Cocoons — left out")); ht->setStyleSheet("color:#9a9a9a;font-size:11px");
      auto* ob = new QCheckBox(QObject::tr("Only these")); ob->setChecked(only);
      hd->setToolTip(QObject::tr("Off — the cocoons in the list are left out of the disc's calculation (it acts on every other cocoon it crosses). On — it acts on the listed cocoons only"));
      QObject::connect(ob, &QCheckBox::toggled, vp, [vp, gid](bool x) { vp->runEdit([gid, x](Scene& s, Selection&, const CurveCache&) { setRawP(s, gid, "ccOnly", x); return QString(); }, false); vp->touchShellMods({gid}); });
      hh->addWidget(ht, 1); hh->addWidget(ob); bv->addWidget(hd);
      for (int id : L) {
        auto* r = new QWidget; auto* rh = new QHBoxLayout(r); rh->setContentsMargins(0, 0, 0, 0);
        auto* nm = button(sc->item(id)->name, QObject::tr("Click — select this cocoon")); nm->setFlat(true);
        nm->setStyleSheet(QString("QPushButton{text-align:left;border:none;background:none;color:%1;%2}").arg(only ? "#8fe8b0" : "#c98a8a", only ? "" : "text-decoration:line-through"));
        QObject::connect(nm, &QPushButton::clicked, vp, [vp, id] { vp->setSelectedItems({id}); });
        auto* del = button("✖", QObject::tr("Take it off the list")); del->setFlat(true); del->setStyleSheet("QPushButton{border:none;background:none;color:#888}");
        std::vector<int> rest; for (int x : L) if (x != id) rest.push_back(x);
        QObject::connect(del, &QPushButton::clicked, vp, [setList, rest] { setList(rest); });
        rh->addWidget(nm, 1); rh->addWidget(del); bv->addWidget(r);
      }
      auto* hint = muted(L.empty() ? (only ? QObject::tr("Empty — it acts on every cocoon it crosses. Drag cocoons here from the outliner: only they") : QObject::tr("Drag cocoons here from the outliner to leave them out"))
                                   : only ? QObject::tr("It acts on these cocoons only. Drag more here from the outliner") : QObject::tr("Left out of its calculation. Drag more here from the outliner"));
      hint->setStyleSheet("color:#8a8a8a;font-size:11px"); bv->addWidget(hint);
      box->dropped = [vp, sc, L, setList](const std::vector<int>& ids0) {
        // (several selected: all of them)
        std::vector<int> ids = ids0;
        if (vp->isItemSelected(ids0[0])) { ids.clear(); for (int i : vp->selection().items) if (const Item* x = sc->item(i); x && x->type == ItemType::Cocoon) ids.push_back(i); }
        std::vector<int> list = L, add; for (int i : ids) if (const Item* x = sc->item(i); x && x->type == ItemType::Cocoon && std::find(list.begin(), list.end(), i) == list.end()) add.push_back(i);
        if (add.empty()) { ui::toast(QObject::tr("Already in the list")); return; }
        list.insert(list.end(), add.begin(), add.end()); setList(list);
      };
      v->addWidget(box);
    }
    // linked guides (drawn together across several cocoons): their settings go together while "Linked settings" is on
    QString link = g->raw.value("link").toString();
    std::vector<const Item*> all; if (!link.isEmpty()) for (auto& [id, x] : sc->items) if (x.type == ItemType::Guide && x.raw.value("link").toString() == link) all.push_back(&x);
    if (all.size() > 1) {
      bool lon = !(g->raw.contains("linkOn") && g->raw.value("linkOn") == QJsonValue(false));
      QStringList others; for (auto* x : all) if (x != g) { const Item* kk = modCocoon(*sc, x); others << (kk ? kk->name : x->name); }
      auto* cb = new QCheckBox(QObject::tr("🔗 Linked settings (%1)").arg(all.size())); cb->setChecked(lon);
      cb->setToolTip(QObject::tr("Linked with %1 other guide%2 (%3): on — Influence radius, Blend and Loop density go together; off — this one is set on its own").arg(all.size() - 1).arg(all.size() > 2 ? "s" : "").arg(others.join(", ")));
      QObject::connect(cb, &QCheckBox::toggled, vp, [vp, gid, link](bool x) {
        vp->runEdit([gid, link, x](Scene& s, Selection&, const CurveCache&) {
          Item* gg = s.item(gid); gg->raw.insert("linkOn", x);
          if (x) for (auto& kv : s.items) { const Item& o = kv.second; if (kv.first != gid && o.type == ItemType::Guide && o.raw.value("link").toString() == link && o.raw.value("linkOn") != QJsonValue(false)) {
            QJsonObject p = gg->raw.value("p").toObject(), op = o.raw.value("p").toObject(); for (const char* k : {"radius", "blend", "density"}) p.insert(k, op.value(k)); gg->raw.insert("p", p); break; } }
          return QString(); }, false);
        vp->touchShellMods({gid});
      });
      v->addWidget(cb);
    }
  } else v->addWidget(muted(QObject::tr("Settings for new guides (select a guide to edit it):")));
  // Influence radius, Blend, Loop density (a guide — live, its link's too; none — the defaults for new guides)
  double hh = headH(*sc);
  struct R { const char* key; const char* lab; double lo, hi, step; std::function<QString(double)> fmt; QString tip; };
  for (const R& r : {R{"radius", "Influence radius", 0, 0.5, 0.005, [hh](double x) { return prec(x * hh, 2); }, QObject::tr("Zone along the lock around the guide where loops become parallel to it. 0 — only the loop on the guide")},
                     R{"blend", "Blend", 0, 1, 0.01, pct, QObject::tr("0 — every loop in the radius is parallel to the guide; higher — from the edge of the radius inwards loops mix back to their own positions. Loop density fades the same way")},
                     R{"density", "Loop density", 0.25, 5, 0.05, times, QObject::tr("More (or fewer) loops inside the zone. ×1 — no extra loops, the guide only aligns them. Fades like the guide: Blend 0 — the whole zone gets the density; Blend 50% — mainly round the centre, weaker towards the edge")}}) {
    double val = tgt.value(r.key).toDouble(QString(r.key) == "blend" ? 1 : QString(r.key) == "density" ? 1 : 0.05);
    auto* s = new SliderRow(QObject::tr(r.lab), r.lo, r.hi, r.step, val, r.fmt); s->setTip(r.tip); QString key = r.key;
    QObject::connect(s, &SliderRow::changed, vp, [vp, sc, gid, key](double x) {
      if (gid < 0) { QJsonObject d = vp->guideDefaultsNow(); d.insert(key, x); vp->setGuideDefaults(d); return; }
      setRawP(*sc, gid, key, x); std::vector<int> touched{gid};
      const Item* g2 = sc->item(gid); QString link = g2->raw.value("link").toString();
      if (!link.isEmpty() && g2->raw.value("linkOn") != QJsonValue(false))
        for (auto& [id, o] : sc->items) if (id != gid && o.type == ItemType::Guide && o.raw.value("link").toString() == link && o.raw.value("linkOn") != QJsonValue(false)) {
          for (const char* k : {"radius", "blend", "density"}) setRawP(*sc, id, k, g2->raw.value("p").toObject().value(k)); touched.push_back(id); }
      vp->touchShellMods(touched);
      if (key != "density") vp->showZone(gid);                   // (the zone shows while the slider moves)
    });
    QObject::connect(s, &SliderRow::released, vp, [vp, gid](double) { if (gid >= 0) vp->commitLive(); });
    v->addWidget(s);
  }
  if (g && g->raw.value("kind").toString() == "disc") {
    double mn = sceneScale(*sc) * 0.002, mx = sceneScale(*sc) * 1.5; QJsonObject p = g->raw.value("p").toObject();
    auto fl = [](double x) { return prec(x, 3); };
    auto* ro = new SliderRow(QObject::tr("Outer radius"), 2 * mn, mx, mn, p.value("rOut").toDouble(), fl);
    QObject::connect(ro, &SliderRow::changed, vp, [vp, sc, gid, mn](double x) { double rin = sc->item(gid)->raw.value("p").toObject().value("rIn").toDouble(); setDiscRadius(vp, *sc, gid, true, std::max(x, rin + mn)); vp->touchShellMods({gid}); });
    QObject::connect(ro, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    auto* ri = new SliderRow(QObject::tr("Inner radius"), mn, mx, mn, p.value("rIn").toDouble(), fl);
    QObject::connect(ri, &SliderRow::changed, vp, [vp, sc, gid, mn](double x) { double rout = sc->item(gid)->raw.value("p").toObject().value("rOut").toDouble(); setDiscRadius(vp, *sc, gid, false, std::min(x, rout - mn)); vp->touchShellMods({gid}); });
    QObject::connect(ri, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    v->addWidget(ro); v->addWidget(ri);
    auto* rb = button(QObject::tr("Reset deformations"));
    QObject::connect(rb, &QPushButton::clicked, vp, [vp, sc, gid] {
      vp->runEdit([gid](Scene& s, Selection&, const CurveCache&) { resetDisc(s, gid); return QString(); }, true);
      vp->touchShellMods({gid}); (void)sc;
    });
    v->addWidget(rb, 0, Qt::AlignLeft);
    auto* rr = new QWidget; auto* rh = new QHBoxLayout(rr); rh->setContentsMargins(0, 4, 0, 0); rh->setSpacing(4);
    QJsonArray rs = g->raw.value("rings").toArray();
    for (int i = 0; i < rs.size() && i < 2; i++) {
      int rid = rs[i].toInt(); auto* b = button(i == 0 ? QObject::tr("Select outer ring") : QObject::tr("Select inner ring"),
        QObject::tr("Selects the %1 ring of the disc with its anchors — move / rotate / scale it with E / R / T, shape it with the Brush").arg(i == 0 ? QObject::tr("outer") : QObject::tr("inner")));
      QObject::connect(b, &QPushButton::clicked, vp, [vp, sc, rid] { const Item* c = sc->item(rid); if (!c) return;
        vp->runEdit([rid, c](Scene&, Selection& sel, const CurveCache&) { sel.items = {rid}; sel.anchors = c->anchors; return QString(); }, false); });
      rh->addWidget(b);
    }
    rh->addStretch(1); v->addWidget(rr);
  }
  if (g && g->raw.value("kind").toString() != "disc" && !g->raw.value("on").isObject() && !g->raw.value("hits").toArray().isEmpty()) {
    auto* b = button(QObject::tr("Select crossed curves"));
    QObject::connect(b, &QPushButton::clicked, vp, [vp, sc, gid] { std::vector<int> ids; for (const QJsonValue& h : sc->item(gid)->raw.value("hits").toArray()) { int c = h.toObject().value("c").toInt(); if (sc->item(c)) ids.push_back(c); } ids.push_back(gid); vp->setSelectedItems(ids); });
    v->addWidget(b, 0, Qt::AlignLeft);
  }
}

std::vector<int> modsOf(const Scene& sc, ItemType t) { std::vector<int> v; for (auto& [id, it] : sc.items) if (it.type == t && modCocoon(sc, &it)) v.push_back(id); return v; }
}  // namespace

QWidget* buildShellPanel(Viewport* vp, Scene* sc) {
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setSpacing(4);
  v->addWidget(h4(QObject::tr("◍ Shell modifiers")));
  if (vp->shTab != "tip" && vp->shTab != "pinf" && vp->shTab != "line" && vp->shTab != "disc") vp->shTab = "tip";
  auto set = [vp](const QString& k) {
    vp->shTab = k; vp->tipBrush.on = k == "tip" && vp->tipBrush.on; vp->pinfBrush.on = k == "pinf" && vp->pinfBrush.on;
    if ((k == "line" || k == "disc") && vp->tool() != "guide") vp->setTool("guide");
    vp->shellChanged(); refresh(vp);
  };
  v->addWidget(modes(QObject::tr("Topology"), {{"tip", QObject::tr("Density"), QObject::tr("Density: more (or fewer) rings where you paint it along a cocoon")},
                                               {"pinf", QObject::tr("Parent influence"), QObject::tr("Parent influence: a mask painted along a cocoon — LMB weakens the pull of its parent (and linked cocoons): its own rings there; Ctrl adds it: their loops taken further out")}}, vp->shTab, set));
  v->addWidget(modes(QObject::tr("Guidelines"), {{"line", QObject::tr("Line"), QObject::tr("A line across a cocoon: a ring of it lies in the line's plane")},
                                                 {"disc", QObject::tr("NURBS disc"), QObject::tr("A disc across a cocoon: a ring lies on it")}}, vp->shTab, set));
  v->addWidget(hline());
  auto ks = vp->shellTargets(); QString on1 = ks.size() == 1 ? sc->item(ks[0])->name : QString();
  if (vp->shTab == "tip") {
    v->addWidget(muted(QObject::tr("Density: more (or fewer) rings on a cocoon where you paint it with its brush. Select a cocoon (or its lock), add it, set the density, paint along the cocoon.")));
    tipControls(v, vp, sc, -1);
    auto* ap = button(ks.size() > 1 ? QObject::tr("Add density to %1 cocoons").arg(ks.size()) : ks.size() ? QObject::tr("Add density to %1").arg(on1) : QObject::tr("Add density"), QObject::tr("Select a cocoon (or its lock), then add"), true);
    QObject::connect(ap, &QPushButton::clicked, vp, [vp] {
      QJsonObject def = vp->tipDefaults; def.insert("v", 2);
      auto made = vp->addShellMods("tip", def, QObject::tr("Density")); if (made.empty()) return;
      vp->pinfBrush.on = false; vp->tipBrush.on = true; vp->shTab = "tip"; vp->setTool("guide"); vp->shellChanged(); refresh(vp);
      ui::toast(QObject::tr("%1 — now paint along the cocoon where it works (Ctrl — erase, Shift — smooth)").arg(made.size() > 1 ? QObject::tr("%1 density modifiers").arg(made.size()) : vp->scene()->item(made[0])->name));
    });
    v->addWidget(ap);
    auto list = modsOf(*sc, ItemType::Tip);
    if (!list.empty()) {
      std::vector<int> sel; for (int id : list) if (vp->isItemSelected(id)) sel.push_back(id);
      v->addWidget(muted(QObject::tr("The painted mask is shown along the cocoon: dark — weak, yellow — full. Ctrl+Shift — Even spacing (cyan, beside it): the rings there equally apart; Ctrl+Shift+Alt — erase it.")));
      brushControls(v, vp, "tip", sel.empty() ? list : sel);
    }
  } else if (vp->shTab == "pinf") {
    v->addWidget(muted(QObject::tr("Parent influence: a mask painted along a cocoon. Where you paint it, the parent's (and the linked cocoons') influence weakens — the cocoon spaces its own rings there even inside them; inverted (Ctrl) it adds — the cocoon takes their loops further out, beside them too.")));
    pinfControls(v, vp, sc, -1);
    auto* ap = button(ks.size() > 1 ? QObject::tr("Add Parent influence to %1 cocoons").arg(ks.size()) : ks.size() ? QObject::tr("Add Parent influence to %1").arg(on1) : QObject::tr("Add Parent influence"), QObject::tr("Select a cocoon (or its lock), then add"), true);
    QObject::connect(ap, &QPushButton::clicked, vp, [vp] {
      auto made = vp->addShellMods("pinf", vp->pinfDefaults, QObject::tr("Parent influence")); if (made.empty()) return;
      vp->tipBrush.on = false; vp->pinfBrush.on = true; vp->shTab = "pinf"; vp->setTool("guide"); vp->shellChanged(); refresh(vp);
      ui::toast(QObject::tr("%1 — paint along the cocoon: LMB weakens the parent's influence, Ctrl adds it, Ctrl+Alt — erase, Shift — smooth").arg(made.size() > 1 ? QObject::tr("%1 Parent influence modifiers").arg(made.size()) : vp->scene()->item(made[0])->name));
    });
    v->addWidget(ap);
    auto list = modsOf(*sc, ItemType::PInf);
    if (!list.empty()) {
      std::vector<int> sel; for (int id : list) if (vp->isItemSelected(id)) sel.push_back(id);
      v->addWidget(muted(QObject::tr("The painted mask is shown along the cocoon: red — weakened, green — added.")));
      brushControls(v, vp, "pinf", sel.empty() ? list : sel);
    }
  } else {
    v->addWidget(muted(vp->shTab == "line"
      ? QObject::tr("Select the cocoon, then drag a line across it: a ring of the cocoon lies in the line's plane there (shown as that ring); it follows the cocoon. Where the cocoon takes its parent's / a linked cocoon's loops, theirs rule. A click on a guide makes it active.")
      : QObject::tr("A cocoon selected: click on it (or press Create) — a disc across it, its ring lies on the disc. Nothing selected: click (at that height) or Create — a level disc in the scene through all the cocoons, a ring of each cocoon it crosses lies on it (general alignment). Move (E), rotate (R), transform (T) it, shape it with the Brush.")));
    if (vp->shTab == "disc") {
      auto* cb = button(ks.size() > 1 ? QObject::tr("＋ Create on %1 cocoons").arg(ks.size()) : ks.size() ? QObject::tr("＋ Create on %1").arg(on1) : QObject::tr("＋ Create"),
                        QObject::tr("A NURBS disc across the middle of the selected cocoon (or the cocoon of the selected lock). Nothing selected — a level disc in the scene through all the cocoons (a ring of each it crosses lies on it)"), true);
      QObject::connect(cb, &QPushButton::clicked, vp, [vp] { vp->createDiscs(); });
      v->addWidget(cb);
    }
    int g = -1; for (auto it = vp->selection().items.rbegin(); it != vp->selection().items.rend(); ++it) if (const Item* x = sc->item(*it); x && x->type == ItemType::Guide) { g = *it; break; }
    guideControls(v, vp, sc, g);
  }
  v->addStretch(1);
  return w;
}

void shellModProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  const Item* k = sc->item(id); if (!k) return;
  const Item* c = modCocoon(*sc, k);
  if (k->type == ItemType::Tip) {
    v->addWidget(muted(c ? QObject::tr("More (or fewer) rings on %1 where its mask is painted. Hide it (eye) to switch it off.").arg(c->name) : QObject::tr("An older Density modifier on a group — it does nothing now (Density works on a cocoon: add it with the Shell modifiers tool).")));
    tipControls(v, vp, sc, id);
    if (!painted(*k, false)) v->addWidget(warn(QObject::tr("Nothing painted yet — it does nothing. Paint with the Density brush.")));
    brushControls(v, vp, "tip", {id});
  } else if (k->type == ItemType::PInf) {
    v->addWidget(muted(c ? QObject::tr("Where its mask is painted, the influence of the cocoons %1 takes its loops from is weakened (red) or added (green). Hide it (eye) to switch it off.").arg(c->name) : QObject::tr("A Parent influence modifier on no cocoon — it does nothing.")));
    pinfControls(v, vp, sc, id);
    if (!painted(*k, true)) v->addWidget(warn(QObject::tr("Nothing painted yet — it does nothing. Paint with the Influence brush.")));
    brushControls(v, vp, "pinf", {id});
  } else if (k->type == ItemType::Even) {
    v->addWidget(warn(QObject::tr("Even loops — removed from the program: this older modifier does nothing now. Delete it (Del).")));
  } else if (k->type == ItemType::Guide) guideControls(v, vp, sc, id);
}

}  // namespace hm
