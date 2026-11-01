#include "app/SmartRename.h"
#include "scene/Crown.h"
#include "scene/Edit.h"
#include "scene/Network.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QCheckBox>
#include <QCollator>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <optional>
#include <map>
#include <set>

namespace hm {

namespace {
struct SrState {
  QString scope = "sel", prefix, find, repl; bool perGroup = true, clean = true, regex = false;
  std::set<QString> types{"group", "curve", "surface", "tube", "xmod", "guide", "mesh", "imesh", "crown", "cap", "skin", "tip", "shell", "pinf", "cocoon"};
};
SrState SR;
const std::vector<std::pair<QString, QString>> SR_TYPES = {{"group", "Groups"}, {"curve", "Curves"}, {"surface", "Surfaces"}, {"tube", "Modifiers"}, {"xmod", "Extras"},
  {"guide", "Guides"}, {"mesh", "Meshes"}, {"imesh", "Imported"}, {"crown", "Crowns"}, {"cap", "Caps"}, {"skin", "Fill holes"}, {"tip", "Density"},
  {"shell", "Proxy shells"}, {"pinf", "Parent influence"}, {"cocoon", "Cocoons"}};

bool svc(const Item& it) { return it.type == ItemType::Group && (it.raw.value("guidesGroup").toBool() || it.raw.value("meshesGroup").toBool() || it.raw.value("importGroup").toBool()); }
bool isG(const Item* g) { return g && g->type == ItemType::Group && !svc(*g); }
const Item* parentOf(const Scene& sc, const Item* it) { return it && it->parent >= 0 ? sc.item(it->parent) : nullptr; }
std::vector<const Item*> groups(const Scene& sc) {
  std::vector<const Item*> v; for (auto& [id, it] : sc.items) if (it.type == ItemType::Group) v.push_back(&it);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->order < b->order; }); return v;
}
std::vector<int> outlinerOrder(const Scene& sc) {
  std::vector<int> out;
  std::function<void(const std::vector<int>&)> walk = [&](const std::vector<int>& ids) { for (int id : ids) { const Item* it = sc.item(id); if (!it) continue; out.push_back(id);
    if (it->type == ItemType::Group || it->type == ItemType::Cocoon) walk(it->children); } };
  walk(sc.roots); return out;
}
struct Scope { std::vector<int> list, groups; };
Scope srScope(const Scene& sc, const Selection& S) {
  std::vector<const Item*> all; for (int id : outlinerOrder(sc)) { const Item* it = sc.item(id); if (it && it->type != ItemType::Head && it->ring < 0 && !it->junc) all.push_back(it); }
  std::vector<const Item*> list = all, gs = groups(sc);
  if (SR.scope == "sel" && !S.items.empty()) {
    std::vector<const Item*> sel, roots, scopes;
    for (int id : S.items) if (const Item* it = sc.item(id)) { sel.push_back(it); if (it->type == ItemType::Group) roots.push_back(it); if (it->type == ItemType::Group || it->type == ItemType::Cocoon) scopes.push_back(it); }
    list.clear();
    for (const Item* it : all) {
      bool in = S.hasItem(it->id) || std::find(scopes.begin(), scopes.end(), it) != scopes.end();
      for (const Item* g : scopes) if (it->id != g->id && sc.isDescendant(it->id, g->id)) in = true;
      if (in) list.push_back(it);
    }
    std::vector<const Item*> g2;
    if (!roots.empty()) { for (const Item* g : groups(sc)) for (const Item* r : roots) if (sc.isDescendant(g->id, r->id)) { g2.push_back(g); break; } }
    else { std::vector<int> ps; for (const Item* it : sel) if (it->parent >= 0 && std::find(ps.begin(), ps.end(), it->parent) == ps.end()) ps.push_back(it->parent); for (int p : ps) g2.push_back(sc.item(p)); }
    gs = g2;
  }
  Scope s;
  for (const Item* it : list) if (SR.types.count(it->typeStr) && !svc(*it)) s.list.push_back(it->id);
  for (const Item* g : gs) if (g && g->type == ItemType::Group && !svc(*g)) s.groups.push_back(g->id);
  return s;
}
QString srClean(const QString& n) { static QRegularExpression re("(\\s+(copy|part|mirror)(\\s*\\d+)?)+$", QRegularExpression::CaseInsensitiveOption); QString s = n; s.remove(re); return s.trimmed(); }
QString srBase(const QString& n) { static QRegularExpression re("\\s+Main$", QRegularExpression::CaseInsensitiveOption); QString s = n; s.remove(re); return s.trimmed(); }

using Plan = std::vector<std::pair<int, QString>>;          // (in order, as a JS Map)
void planSet(Plan& p, int id, const QString& nn) { for (auto& e : p) if (e.first == id) { e.second = nn; return; } p.push_back({id, nn}); }

std::map<int, QString> netPlan(Viewport* vp, Scene& sc, int gid) {
  std::vector<int> si, sa; Network net(sc, vp->hair().curves, si, sa);
  return net.netNamePlan(gid);
}

Plan srRenumberPlan(Viewport* vp, Scene& sc, std::vector<int> list) {
  Plan plan;
  // new-logic groups: the network names (C_ / S_), not the numbering below
  std::map<int, std::map<int, QString>> nets;
  auto netName = [&](const Item* it) -> std::optional<QString> {
    const Item* p = parentOf(sc, it); if (p && p->type != ItemType::Group) return std::nullopt;
    int g = p ? p->id : -1; if (!nets.count(g)) nets[g] = netPlan(vp, sc, g);
    auto f = nets[g].find(it->id); if (f == nets[g].end()) return std::nullopt; return f->second;
  };
  std::vector<int> rest;
  for (int id : list) { const Item* it = sc.item(id);
    if (it->type != ItemType::Curve && it->type != ItemType::Surface) { rest.push_back(id); continue; }
    auto nn = netName(it); if (!nn) { rest.push_back(id); continue; }
    if (*nn != it->name) planSet(plan, id, *nn); }
  list = rest;
  auto hasSub = [&](const Item* g) { for (int c : g->children) if (isG(sc.item(c))) return true; return false; };
  std::vector<int> nested; for (int id : list) { const Item* it = sc.item(id); if (isG(it) && isG(parentOf(sc, it))) nested.push_back(id); }
  struct B { int id; QString nm, pat; bool numbered; };
  std::vector<QString> keys; std::map<QString, std::vector<B>> buckets;
  // the curves of a crown (connector → parting line, however they were named) are "Crown curve N", numbered along the
  // lower connector
  std::map<int, double> crownU;
  if (SR.clean) for (int k : crown::crowns(sc)) for (auto& r : crown::crownCurves(sc, vp->hair().curves, *sc.item(k))) crownU[r.c] = r.u;
  for (int id : list) {
    if (std::find(nested.begin(), nested.end(), id) != nested.end()) continue;   // named after its parent below
    const Item* it = sc.item(id); QString TN = itemTypeName(it->type);
    QString nm = SR.clean ? srClean(it->name) : it->name; if (nm.isEmpty()) nm = TN;
    if (it->type == ItemType::Group) { nm = srBase(nm); if (nm.isEmpty()) nm = TN; }
    // "Average curve N" is an ordinary curve: numbered with the other curves
    static QRegularExpression avg("^average curve\\b", QRegularExpression::CaseInsensitiveOption);
    if (crownU.count(id)) nm = "Crown curve 1";
    else if (SR.clean && it->type == ItemType::Curve && avg.match(nm).hasMatch()) nm.replace(avg, TN);
    // a default name with tails ("Curve 246 edge 1") is a default name: "Curve N"
    if (QRegularExpression("^" + TN + "\\s*\\d+").match(nm).hasMatch()) nm = TN + " 1";
    QRegularExpressionMatch last; auto itm = QRegularExpression("\\d+").globalMatch(nm); while (itm.hasNext()) last = itm.next();
    bool m = last.hasMatch();
    QString pat = m ? nm.left(last.capturedStart()) + "#" + nm.mid(last.capturedEnd()) : nm + " #";
    QString key = (SR.perGroup ? (it->parent >= 0 ? QString::number(it->parent) : QString("null")) : QString("all")) + "|" + it->typeStr + "|" + pat;
    if (!buckets.count(key)) keys.push_back(key);
    buckets[key].push_back({id, nm, pat, m || nm == TN});
  }
  std::map<int, QString> names;
  for (const QString& k : keys) { auto& b = buckets[k]; if (std::all_of(b.begin(), b.end(), [&](const B& x) { return crownU.count(x.id) > 0; })) std::stable_sort(b.begin(), b.end(), [&](const B& x, const B& y) { return crownU[x.id] < crownU[y.id]; }); }
  for (const QString& k : keys) { auto& b = buckets[k];
    for (size_t i = 0; i < b.size(); i++) { const Item* it = sc.item(b[i].id);
      QString nn = b.size() == 1 && !b[i].numbered ? b[i].nm : QString(b[i].pat).replace(b[i].pat.indexOf('#'), 1, QString::number(i + 1));
      if (it->type == ItemType::Group && hasSub(it)) nn += " Main";
      names[b[i].id] = nn; if (nn != it->name) planSet(plan, b[i].id, nn); } }
  // groups inside groups, parents first (outliner order)
  for (int id : nested) {
    const Item* g = sc.item(id), *p = parentOf(sc, g); std::vector<int> sibs; for (int c : p->children) if (isG(sc.item(c))) sibs.push_back(c);
    int k = int(std::find(sibs.begin(), sibs.end(), id) - sibs.begin()) + 1;
    QString nn = srBase(names.count(p->id) ? names[p->id] : p->name) + "_" + QString::number(k).rightJustified(2, '0') + (hasSub(g) ? " Main" : "");
    names[id] = nn; if (nn != g->name) planSet(plan, id, nn);
  }
  return plan;
}

std::optional<QRegularExpression> srFindRe() {
  if (SR.find.isEmpty()) return std::nullopt;
  QRegularExpression re(SR.regex ? SR.find : QRegularExpression::escape(SR.find)); if (!re.isValid()) return std::nullopt; return re;
}

// one group's children in the order of their connections
void srSortGroup(Viewport* vp, Scene& sc, int gid) {
  Item* g = sc.item(gid); if (!g) return;
  std::vector<const Item*> kids; for (int id : g->children) if (const Item* k = sc.item(id)) kids.push_back(k);
  auto np = netPlan(vp, sc, gid);
  std::vector<int> head;
  if (!np.empty()) {
    QCollator col; col.setNumericMode(true);
    auto nm = [&](const Item* it) { auto f = np.find(it->id); return f != np.end() ? f->second : it->name; };
    auto cmp = [&](const Item* a, const Item* b) { return col.compare(nm(a), nm(b)) < 0; };
    static QRegularExpression endRe("^C_\\d+$"); auto isEnd = [&](const Item* c) { return endRe.match(nm(c)).hasMatch(); };
    std::vector<const Item*> sub, ends, mids, ws, ecs;
    for (auto* k : kids) { if (k->type == ItemType::Group) sub.push_back(k); else if (k->type == ItemType::Curve && k->ring < 0 && !k->junc && np.count(k->id)) (isEnd(k) ? ends : mids).push_back(k); else if (k->type == ItemType::Surface && np.count(k->id)) ws.push_back(k); }
    std::stable_sort(ends.begin(), ends.end(), cmp); std::stable_sort(mids.begin(), mids.end(), cmp); std::stable_sort(ws.begin(), ws.end(), cmp);
    for (auto* s : ws) for (int id : {s->a, s->b}) { const Item* c = sc.item(id); if (c && c->junc && c->parent == gid && std::find(ecs.begin(), ecs.end(), c) == ecs.end()) ecs.push_back(c); }
    for (auto* v : {&sub, &ends, &mids, &ws, &ecs}) for (auto* k : *v) head.push_back(k->id);
    std::vector<int> ch = head; for (auto* k : kids) if (std::find(head.begin(), head.end(), k->id) == head.end()) ch.push_back(k->id);
    g->children = ch; return;
  }
  // (an older group: every lock as curve → its wall → next curve …, around the ring)
  std::vector<const Item*> cs, ss; for (auto* k : kids) { if (k->type == ItemType::Curve && k->ring < 0) cs.push_back(k); if (k->type == ItemType::Surface) ss.push_back(k); }
  std::map<int, std::vector<std::pair<int, int>>> nb; for (auto* c : cs) nb[c->id];
  for (auto* s : ss) if (nb.count(s->a) && nb.count(s->b) && s->a != s->b) { nb[s->a].push_back({s->id, s->b}); nb[s->b].push_back({s->id, s->a}); }
  std::vector<int> out; std::set<int> doneC, doneS;
  for (auto* c0 : cs) {
    if (doneC.count(c0->id) || nb[c0->id].empty()) continue;
    std::vector<int> comp{c0->id}; for (size_t i = 0; i < comp.size(); i++) for (auto& [s, o] : nb[comp[i]]) if (std::find(comp.begin(), comp.end(), o) == comp.end()) comp.push_back(o);
    int cur = c0->id; for (int id : comp) if (nb[id].size() == 1) { cur = id; break; }
    for (;;) {
      doneC.insert(cur); out.push_back(cur);
      std::optional<std::pair<int, int>> step; for (auto& x : nb[cur]) if (!doneS.count(x.first) && !doneC.count(x.second)) { step = x; break; }
      if (step) { doneS.insert(step->first); out.push_back(step->first); cur = step->second; continue; }
      for (auto& x : nb[cur]) if (!doneS.count(x.first)) { doneS.insert(x.first); out.push_back(x.first); break; }   // the wall closing the ring
      int left = -1; for (int id : comp) if (!doneC.count(id)) { left = id; break; } if (left < 0) break;
      cur = left;
    }
    for (int id : comp) for (auto& [s, o] : nb[id]) if (!doneS.count(s)) { doneS.insert(s); out.push_back(s); }
  }
  auto rank = [](const Item* k) { return k->type == ItemType::Group ? 0 : k->type == ItemType::Curve ? 2 : k->type == ItemType::Surface ? 3 : k->type == ItemType::Tube ? 4 : k->type == ItemType::XMod ? 5 : 6; };
  std::vector<const Item*> restI; for (auto* k : kids) if (std::find(out.begin(), out.end(), k->id) == out.end()) restI.push_back(k);
  std::vector<int> ch; for (auto* k : restI) if (k->type == ItemType::Group) ch.push_back(k->id);
  ch.insert(ch.end(), out.begin(), out.end());
  std::vector<const Item*> after; for (auto* k : restI) if (k->type != ItemType::Group) after.push_back(k);
  std::stable_sort(after.begin(), after.end(), [&](auto* a, auto* b) { return rank(a) < rank(b); });
  for (auto* k : after) ch.push_back(k->id);
  g->children = ch;
}

QLabel* h4(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setStyleSheet("margin-top:10px;font-weight:bold"); return l; }
QLabel* mut(const QString& t) { auto* l = new QLabel(t); l->setStyleSheet("color:#8a8a8a"); l->setWordWrap(true); return l; }
}  // namespace

QString smartRenamePlanDump(Viewport* vp, Scene& sc) {
  QString keep = SR.scope; SR.scope = "all"; Selection none; Scope s = srScope(sc, none); SR.scope = keep;
  QStringList o; for (auto& [id, nn] : srRenumberPlan(vp, sc, s.list)) o << QString("%1 %2 -> %3").arg(id).arg(sc.item(id)->name, nn);
  return QString("plan %1 (scope %2 items)\n").arg(o.size()).arg(s.list.size()) + o.join("\n");
}
QString smartRenameSortDump(Viewport* vp, Scene& sc, int gid) {
  srSortGroup(vp, sc, gid); QStringList o; for (int c : sc.item(gid)->children) o << QString::number(c); return o.join(",");
}

SmartRename::SmartRename(Viewport* vp, Scene* sc, QWidget* parent) : QDialog(parent), vp_(vp), sc_(sc) {
  setWindowTitle(tr("Aa Smart Rename")); setModal(true); resize(560, 720);
  auto* v = new QVBoxLayout(this); v->setContentsMargins(0, 0, 0, 0);
  auto* sa = new QScrollArea; sa->setWidgetResizable(true); sa->setFrameShape(QFrame::NoFrame); v->addWidget(sa);
  body_ = new QWidget; sa->setWidget(body_); lay_ = new QVBoxLayout(body_); lay_->setContentsMargins(14, 12, 14, 12); lay_->setSpacing(6);
  refill();
}

void SmartRename::refill() {
  while (QLayoutItem* it = lay_->takeAt(0)) { if (QWidget* w = it->widget()) w->deleteLater(); if (QLayout* l = it->layout()) { while (QLayoutItem* x = l->takeAt(0)) { if (x->widget()) x->widget()->deleteLater(); delete x; } } delete it; }
  auto again = [this] { QTimer::singleShot(0, this, [this] { refill(); }); };
  auto apply = [this, again](const Plan& plan, const QString& what) {
    if (plan.empty()) { ui::toast(tr("Nothing to change")); return; }
    vp_->runEdit([plan](Scene& s, Selection&, const CurveCache&) { for (auto& [id, nn] : plan) if (Item* it = s.item(id)) it->name = nn; edit::refreshCounters(s); return QString(); }, false);
    again(); ui::toast(tr("%1: %2 name%3 changed").arg(what).arg(plan.size()).arg(plan.size() == 1 ? "" : "s"));
  };
  const Selection& S = vp_->selection();
  Scope sc = srScope(*sc_, S);
  auto* title = new QLabel(tr("Aa Smart Rename")); title->setStyleSheet("font-size:15px;font-weight:bold"); lay_->addWidget(title);
  // scope + types
  auto* f1 = new QHBoxLayout; f1->addWidget(new QLabel(tr("Scope")));
  for (auto [k, lab] : {std::pair<QString, QString>{"sel", S.items.empty() ? tr("Selection (nothing selected — all)") : tr("Selection (groups with their content)")}, {"all", tr("Whole scene")}}) {
    auto* r = new QRadioButton(lab); r->setChecked(SR.scope == k); QString kk = k;
    connect(r, &QRadioButton::toggled, this, [kk, again](bool on) { if (on) { SR.scope = kk; again(); } }); f1->addWidget(r);
  }
  f1->addStretch(1); lay_->addLayout(f1);
  auto* ty = new QGridLayout; int n = 0;
  for (auto& [k, lab] : SR_TYPES) { auto* cb = new QCheckBox(tr(lab.toUtf8())); cb->setChecked(SR.types.count(k)); QString kk = k;
    connect(cb, &QCheckBox::toggled, this, [kk, again](bool on) { if (on) SR.types.insert(kk); else SR.types.erase(kk); again(); }); ty->addWidget(cb, n / 4, n % 4); n++; }
  lay_->addLayout(ty);
  lay_->addWidget(mut(tr("%1 item%2 in scope · %3 group%4 to sort").arg(sc.list.size()).arg(sc.list.size() == 1 ? "" : "s").arg(sc.groups.size()).arg(sc.groups.size() == 1 ? "" : "s")));
  auto chk = [this, again](const QString& lab, bool* key, const QString& tip) { auto* cb = new QCheckBox(lab); cb->setToolTip(tip); cb->setChecked(*key); connect(cb, &QCheckBox::toggled, this, [key, again](bool on) { *key = on; again(); }); return cb; };
  auto btn = [](const QString& lab, bool primary = false, const QString& tip = {}) { auto* b = new QPushButton(lab); b->setToolTip(tip); if (primary) b->setProperty("primary", true); return b; };
  // sort
  lay_->addWidget(h4(tr("⇅ Sort by connections")));
  auto* bs = new QHBoxLayout;
  auto* s1 = btn(tr("⇅ Sort"), false, tr("Inside each group: sub-groups, then every lock as curve → its wall → next curve …, free curves, modifiers, the rest"));
  auto* s2 = btn(tr("⇅ Sort, then renumber"), true, tr("Sort, then clean up and renumber in the new order — curve N stands next to wall N"));
  std::vector<int> gs = sc.groups;
  connect(s1, &QPushButton::clicked, this, [this, gs, again] {
    vp_->runEdit([this, gs](Scene& s, Selection&, const CurveCache&) { for (int g : gs) srSortGroup(vp_, s, g); return QString(); }, false);
    again(); ui::toast(tr("Sorted %1 group%2").arg(gs.size()).arg(gs.size() == 1 ? "" : "s"));
  });
  connect(s2, &QPushButton::clicked, this, [this, gs, apply] {
    for (int g : gs) srSortGroup(vp_, *sc_, g);                         // (one step with the renaming)
    apply(srRenumberPlan(vp_, *sc_, srScope(*sc_, vp_->selection()).list), tr("Sorted and renumbered"));
  });
  bs->addWidget(s1); bs->addWidget(s2); bs->addStretch(1); lay_->addLayout(bs);
  // clean up & renumber
  lay_->addWidget(h4(tr("№ Clean up & renumber")));
  auto* o1 = new QHBoxLayout;
  o1->addWidget(chk(tr("Remove \"copy\" / \"part\" tails"), &SR.clean, tr("Curve 3 copy copy → Curve … · Average curve 2 → Curve … (numbered with the other curves)")));
  o1->addWidget(chk(tr("Number in each group"), &SR.perGroup, tr("On — every group counts from 1; off — one count through the whole scope")));
  o1->addStretch(1); lay_->addLayout(o1);
  auto* nh = mut(tr("New-logic groups — names by the network: end curves C_1, C_2 … (roots, then tips, across the lock); a curve between forks — the strands through it, C_1-3 (strands 1–3), C_1-2,5; a wall — its two strands, S_2_4. Service curves keep EC names."));
  nh->setStyleSheet("color:#8a8a8a;font-size:11px"); lay_->addWidget(nh);
  Plan plan = srRenumberPlan(vp_, *sc_, sc.list);
  QStringList pv; for (size_t i = 0; i < plan.size() && i < 300; i++) { const Item* it = sc_->item(plan[i].first); pv << QString("%1 %2 → <b>%3</b>").arg(itemTypeIcon(it->type), it->name.toHtmlEscaped(), plan[i].second.toHtmlEscaped()); }
  if (plan.empty()) pv << tr("Names are already in order"); else if (plan.size() > 300) pv << tr("… and %1 more").arg(plan.size() - 300);
  auto* prev = new QLabel(pv.join("<br>")); prev->setTextFormat(Qt::RichText); prev->setStyleSheet("background:#1e1e1e;border:1px solid #3a3a3a;border-radius:4px;padding:6px;color:#ccc");
  prev->setWordWrap(true); prev->setMaximumHeight(220); prev->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  auto* pvs = new QScrollArea; pvs->setWidget(prev); pvs->setWidgetResizable(true); pvs->setMaximumHeight(220); pvs->setFrameShape(QFrame::NoFrame); lay_->addWidget(pvs);
  auto* b1 = btn(tr("Apply (%1)").arg(plan.size()), true); b1->setEnabled(!plan.empty());
  connect(b1, &QPushButton::clicked, this, [plan, apply] { apply(plan, tr("Renumbered")); });
  lay_->addWidget(b1, 0, Qt::AlignLeft);
  // prefix: your own text at the start of the names
  lay_->addWidget(h4(tr("＋ Prefix")));
  auto* f2 = new QHBoxLayout; auto* si = new QLineEdit(SR.prefix); si->setPlaceholderText(tr("e.g. L_  or  Front "));
  si->setToolTip(tr("Text put at the start of every name in scope (Remove — takes it off where a name starts with it)"));
  connect(si, &QLineEdit::textChanged, this, [](const QString& t) { SR.prefix = t; });
  auto prePlan = [this](bool add) { Plan p; if (SR.prefix.isEmpty()) return p;
    for (int id : srScope(*sc_, vp_->selection()).list) { const Item* it = sc_->item(id); bool has = it->name.startsWith(SR.prefix);
      if (add && !has) p.push_back({id, SR.prefix + it->name});
      if (!add && has) { QString r = it->name.mid(SR.prefix.size()); while (!r.isEmpty() && r[0].isSpace()) r.remove(0, 1); p.push_back({id, r.isEmpty() ? it->name : r}); } }
    return p; };
  auto* pa = btn(tr("Add")); auto* pr = btn(tr("Remove"));
  connect(pa, &QPushButton::clicked, this, [prePlan, apply] { apply(prePlan(true), tr("Prefix added")); });
  connect(pr, &QPushButton::clicked, this, [prePlan, apply] { apply(prePlan(false), tr("Prefix removed")); });
  f2->addWidget(new QLabel(tr("Prefix"))); f2->addWidget(si, 1); f2->addWidget(pa); f2->addWidget(pr); lay_->addLayout(f2);
  // find & replace
  lay_->addWidget(h4(tr("🔍 Find & replace")));
  auto* f3 = new QHBoxLayout; auto* fi = new QLineEdit(SR.find); fi->setPlaceholderText(tr("find (e.g. _L)"));
  auto* cnt = new QLabel; cnt->setStyleSheet("color:#8a8a8a"); cnt->setMinimumWidth(70);
  auto* f4 = new QHBoxLayout; auto* ri = new QLineEdit(SR.repl); ri->setPlaceholderText(tr("replace with (e.g. _R)"));
  auto matches = [this] { std::vector<int> m; auto re = srFindRe(); if (!re) return m; for (int id : srScope(*sc_, vp_->selection()).list) if (re->match(sc_->item(id)->name).hasMatch()) m.push_back(id); return m; };
  auto upd = [cnt, matches] { auto re = srFindRe(); cnt->setText(SR.find.isEmpty() ? QString() : re ? tr("%1 found").arg(matches().size()) : tr("bad pattern")); };
  connect(fi, &QLineEdit::textChanged, this, [upd](const QString& t) { SR.find = t; upd(); });
  connect(ri, &QLineEdit::textChanged, this, [](const QString& t) { SR.repl = t; });
  f3->addWidget(new QLabel(tr("Find"))); f3->addWidget(fi, 1); f3->addWidget(cnt); lay_->addLayout(f3);
  f4->addWidget(new QLabel(tr("Replace with"))); f4->addWidget(ri, 1); lay_->addLayout(f4);
  auto* o2 = new QHBoxLayout;
  o2->addWidget(chk(tr("Regular expression"), &SR.regex, tr("The find text is a regular expression ($1 … in the replacement)")));
  auto* sm = btn(tr("Select matches")); auto* ra = btn(tr("Replace all"), true);
  connect(sm, &QPushButton::clicked, this, [this, matches] {
    auto m = matches(); if (m.empty()) { ui::toast(tr("Nothing found")); return; }
    for (int id : m) for (const Item* p = parentOf(*sc_, sc_->item(id)); p; p = parentOf(*sc_, p)) sc_->item(p->id)->expanded = true;
    vp_->setSelectedItems({}); vp_->setSelectedItems(m); ui::toast(tr("%1 found and selected").arg(m.size()));
  });
  connect(ra, &QPushButton::clicked, this, [this, matches, apply] {
    auto re = srFindRe(); if (!re) { ui::toast(tr("Type what to find")); return; }
    QString rep = SR.repl; if (SR.regex) for (int d = 0; d <= 9; d++) rep.replace("$" + QString::number(d), "\\" + QString::number(d));   // ($1 → \1)
    Plan p; for (int id : matches()) { QString nn = sc_->item(id)->name; nn.replace(*re, rep); nn = nn.trimmed(); if (!nn.isEmpty() && nn != sc_->item(id)->name) p.push_back({id, nn}); }
    apply(p, tr("Replaced"));
  });
  o2->addWidget(sm); o2->addWidget(ra); o2->addStretch(1); lay_->addLayout(o2); upd();
  auto* bar = new QHBoxLayout; bar->addStretch(1); auto* cl = btn(tr("Close")); connect(cl, &QPushButton::clicked, this, &QDialog::accept); bar->addWidget(cl); lay_->addLayout(bar);
  lay_->addStretch(1);
}

}  // namespace hm
