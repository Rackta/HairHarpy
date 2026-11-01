// Shift+RMB: the actions for what is selected (the tools are on the Space ring), as the HTML version's openMainPie.
// Anchors: delete / keep shape / recalc, zip, curve through / between them, optimize, rotate junction, snap to path,
// unglue, separate at an X, the lock's curves. Curves: walls (between / along paths / volume / fill), the paths through
// the forks, routes of service curves, splitting a wall or a trunk, recalc / optimize / reverse. Items marked "more" open
// the next menu at the pointer when held on (as a marking menu); hovering an item shows what it works on.
#include "app/MainWindow.h"
#include "app/Outliner.h"
#include "scene/AnchorOps.h"
#include "scene/Crown.h"
#include "scene/Cut.h"
#include "scene/Edit.h"
#include "scene/HairState.h"
#include "scene/MeshCurves.h"
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QCursor>
#include <QPushButton>
#include <QTimer>

namespace hm {

namespace {
int idxOf(const std::vector<int>& v, int x) { auto it = std::find(v.begin(), v.end(), x); return it == v.end() ? -1 : int(it - v.begin()); }
ui::PieItem item(const QString& label, std::function<void()> act, bool danger = false) { ui::PieItem it; it.label = label; it.act = std::move(act); it.danger = danger; return it; }
// an item opening the next menu (held on it — or clicked)
ui::PieItem more(const QString& label, std::function<void()> go) { ui::PieItem it; it.label = label + " ▸"; it.act = go; it.dwell = go; return it; }
QString few(const Scene& sc, const std::vector<int>& l) { QStringList n; for (size_t i = 0; i < l.size() && i < 3; i++) n << sc.item(l[i])->name; return n.join(", ") + (l.size() > 3 ? " …" : ""); }
}  // namespace

void MainWindow::openMainPie(const QPoint& global) {
  if (viewport_->openSurfCutPie(global)) return;            // (while cutting a wall: only the cut's menu)
  if (scene_.room == "topo") { openTopoPie(global); return; }
  if (viewport_->tool() == "edge") {                         // (Edges: the path → curves, Mesh → curves, an empty group)
    Viewport* vp = viewport_; const Item* em = scene_.item(vp->edges.mesh); size_t n = em && em->poly ? vp->edges.list.size() : 0;
    ui::PieSpec s; s.title = n ? tr("Edges selected: %1").arg(n) : tr("Edges");
    if (n) {
      size_t k = meshc::edgeChains(*meshc::topoOf(*em->poly), vp->edges.list).size();
      s.top.push_back(item(k == 1 ? tr("∿ Convert the path to a curve") : tr("∿ Convert the path to %1 curves").arg(k), [vp] { vp->edgesToCurves(); }));
      s.bottom.push_back(item(tr("✖ Deselect edges"), [vp] { vp->clearEdges(); }));
    }
    if (int mm = vp->convertibleMesh(); mm >= 0) s.bottom.push_back(item(tr("◆ %1: mesh → curves & surfaces").arg(scene_.item(mm)->name), [vp, mm] { vp->meshToCurves(mm); }));
    s.bottom.push_back(item(tr("▣ Create empty group"), [vp] { vp->createEmptyGroup(); }));
    ui::Pie::open(this, global, s, true);
    return;
  }
  Viewport* vp = viewport_; Scene& sc = scene_; const CurveCache& cc = vp->hair().curves; const Selection S = vp->selection();
  auto vizOff = [vp] { vp->setPieViz({}, {}); };
  auto curvePts = [&cc](int id) { auto it = cc.find(id); return it == cc.end() ? std::vector<Vec3>{} : it->second.p; };
  auto edit = [vp](std::function<QString(Scene&, Selection&, const CurveCache&)> fn) { vp->runEdit(std::move(fn), true); };
  auto fillItem = [this, global] { return item(tr("◎ Detect the fill outline here"), [this, global] { viewport_->startFillAt(global); }); };
  auto open = [this](ui::PieSpec s, const QPoint& at, bool hold) { ui::Pie::open(this, at, s, hold); };
  // ◎ the curves of the lock the selection belongs to
  std::optional<ui::PieItem> lk;
  { auto cs = aops::lockCurvesFrom(sc, S.items, S.anchors);
    if (!cs.empty()) lk = item(tr("◎ Select all curves of the lock (%1)").arg(cs.size()), [vp, cs] { vp->setSelectedItems({}); vp->setSelectedItems(cs); ui::toast(tr("%1 curve%2 of the lock selected").arg(cs.size()).arg(cs.size() == 1 ? "" : "s")); }); }

  // ---- the sub-menus ----
  // ⫘ zip the curves at an anchor: which side, then onto what
  std::function<void(int, bool)> openZipTarget = [this, vp, &sc, vizOff, open](int J, bool up) {
    auto Z = aops::zipParts(sc, J); auto ps = up ? Z.up : Z.down; if (ps.size() < 2) return;
    auto avg = aops::zipAverage(ps); Projector P(vp->camera(), vp->width(), vp->height());
    std::map<QString, int> cnt; for (auto& p : ps) cnt[sc.item(p.c)->name]++;
    std::vector<ui::PieItem> ring;
    std::vector<const aops::ZipPart*> order; for (auto& p : ps) order.push_back(&p);
    std::stable_sort(order.begin(), order.end(), [&](auto* a, auto* b) { return P.toScreen(b->pts.back()).x < P.toScreen(a->pts.back()).x; });
    std::vector<std::vector<Vec3>> all; for (auto& p : ps) all.push_back(p.pts);
    for (auto* p : order) { const Item* c = sc.item(p->c); const Item* par = sc.item(c->parent);
      QString nm = cnt[c->name] > 1 && par ? par->name + " · " + c->name : c->name; int cid = p->c; auto pts = p->pts;
      ui::PieItem it = item(nm, [vp, J, up, cid] { vp->runEdit([=](Scene& s, Selection& sl, const CurveCache& c2) { return aops::zipApply(s, c2, sl, J, up, cid); }, true); });
      it.hot = [vp, all, pts](bool on) { vp->setPieViz(all, on ? std::vector<std::vector<Vec3>>{pts} : std::vector<std::vector<Vec3>>{}); }; ring.push_back(it); }
    ui::PieSpec s; s.title = tr("Zip %1 curves %2 the anchor — onto:").arg(ps.size()).arg(up ? tr("above") : tr("below"));
    ui::PieItem av = item(tr("⊙ The averaged position"), [vp, J, up] { vp->runEdit([=](Scene& s2, Selection& sl, const CurveCache& c2) { return aops::zipApply(s2, c2, sl, J, up, -1); }, true); });
    av.hot = [vp, all, avg](bool on) { vp->setPieViz(all, on ? std::vector<std::vector<Vec3>>{avg} : std::vector<std::vector<Vec3>>{}); };
    s.top = {av}; s.ring = ring; s.arc0 = 0; s.arc1 = M_PI; s.onEnd = vizOff;
    vp->setPieViz(all, {}); open(s, QCursor::pos(), false);
  };
  auto zipItem = [&](int J) -> std::optional<ui::PieItem> {
    auto Z = aops::zipParts(sc, J); if (Z.up.size() < 2 && Z.down.size() < 2) return std::nullopt;
    return more(tr("⫘ Zip the curves"), [this, vp, &sc, J, openZipTarget, vizOff, open] {
      auto Z2 = aops::zipParts(sc, J);
      auto side = [&](bool up, const QString& lab) {
        auto& ps = up ? Z2.up : Z2.down; std::vector<std::vector<Vec3>> pts; for (auto& p : ps) pts.push_back(p.pts);
        if (ps.size() < 2) return item(tr("%1 — fewer than 2 curves").arg(lab), [] {});
        ui::PieItem it = more(tr("%1 (%2 curves)").arg(lab).arg(ps.size()), [J, up, openZipTarget] { openZipTarget(J, up); });
        it.hot = [vp, pts](bool on) { vp->setPieViz(on ? pts : std::vector<std::vector<Vec3>>{}, {}); }; return it;
      };
      ui::PieSpec s; s.title = tr("Zip the curves at the anchor"); s.top = {side(true, tr("▲ Above the anchor"))}; s.bottom = {side(false, tr("▼ Below the anchor"))}; s.onEnd = vizOff;
      open(s, QCursor::pos(), false);
    });
  };
  // ⇶ snap a part onto the path last copied with ⧉ SEL
  auto snapItem = [&](int a) -> std::optional<ui::PieItem> {
    if (vp->snapSrc.empty()) return std::nullopt;
    auto T = aops::snapTrack(sc, cc, vp->snapSrc); if (!T || T->has(sc, a)) return std::nullopt;
    QString tn = T->name; std::vector<int> ids = vp->snapSrc;
    return more(tr("⇶ Snap to path — onto %1").arg(tn), [this, vp, &sc, &cc, a, ids, tn, vizOff, open] {
      auto T2 = aops::snapTrack(sc, cc, ids); if (!T2) return;
      auto O = aops::snapOptions(sc, cc, a, *T2);
      if (O.empty()) { ui::toast(tr("⇶ Nothing to lay onto %1 from this anchor").arg(tn)); return; }
      ui::PieSpec s; s.title = tr("Snap onto %1 — which part?").arg(tn); s.onEnd = vizOff;
      for (auto& o : O) {
        int same = 0; for (auto& x : O) if (x.up == o.up) same++;
        ui::PieItem it = item(QString("%1%2 (%3 anchors%4)").arg(o.up ? tr("▲ Upper part") : tr("▼ Lower part")).arg(same > 1 ? ": " + o.names : QString()).arg(o.segs.size() + 1).arg(o.part ? tr(", partly") : QString()),
          [vp, a, ids, o] { vp->runEdit([=](Scene& s2, Selection& sl, const CurveCache& c2) { return aops::snapApply(s2, c2, sl, a, ids, o); }, true); });
        auto pa = o.pts, pb = o.tpts; it.hot = [vp, pa, pb](bool on) { if (on) vp->setPieViz({pa}, {pb}); else vp->setPieViz({}, {}); };
        (o.up ? s.top : s.bottom).push_back(it);
      }
      open(s, QCursor::pos(), false);
    });
  };
  // ⌖ the paths through the forks
  auto pathItem = [&](const std::vector<int>& curves) -> std::optional<ui::PieItem> {
    std::vector<int> sel; for (int id : curves) if (const Item* c = sc.item(id); segNet(*c) && c->anchors.size() >= 2) sel.push_back(id);
    if (sel.empty()) return std::nullopt;
    auto P = aops::pathsFor(sc, cc, sel); if (P.empty()) return std::nullopt;
    std::vector<std::vector<Vec3>> allPts; for (auto& p : P) allPts.push_back(aops::pathPts(sc, cc, p));
    ui::PieItem it = more(tr("⌖ Select — paths through the forks (%1)").arg(P.size() >= 200 ? QString("200+") : QString::number(P.size())), [this, vp, &sc, &cc, sel, vizOff, open] {
      auto P2 = aops::pathsFor(sc, cc, sel); if (P2.empty()) return;
      auto on = [&](const NetPath& p) { for (auto& s : p) if (!vp->isItemSelected(s.c)) return false; return true; };
      auto len = [&](const NetPath& p) { double L = 0; for (auto& s : p) { auto f = cc.find(s.c); if (f != cc.end() && !f->second.arc.empty()) L += f->second.arc.back(); } return L; };
      std::vector<NetPath> shown = P2; std::stable_sort(shown.begin(), shown.end(), [&](const NetPath& a, const NetPath& b) { if (on(a) != on(b)) return int(on(a)) < int(on(b)); return len(b) < len(a); });
      if (shown.size() > 16) shown.resize(16);
      auto ends = [&](const NetPath& p) { return p.size() == 1 ? sc.item(p[0].c)->name : sc.item(p[0].c)->name + " → " + sc.item(p.back().c)->name; };
      std::vector<std::vector<Vec3>> all; for (auto& p : P2) all.push_back(aops::pathPts(sc, cc, p));
      ui::PieSpec s; std::vector<int> uni; for (auto& p : P2) for (auto& st : p) if (idxOf(uni, st.c) < 0) uni.push_back(st.c);
      s.title = tr("Paths through %1: %2%3 · a click adds the path (a selected one — off)").arg(sel.size() == 1 ? sc.item(sel[0])->name : tr("the %1 selected curves").arg(sel.size())).arg(P2.size()).arg(P2.size() > 16 ? tr(" (the 16 longest here)") : QString());
      ui::PieItem every = item(tr("⊙ Every curve of these paths (%1)").arg(uni.size()), [vp, uni] { std::vector<int> l = vp->selection().items; for (int c : uni) { l.erase(std::remove(l.begin(), l.end(), c), l.end()); l.push_back(c); } vp->setSelectedItems({}); vp->setSelectedItems(l); });
      every.hot = [vp, all](bool o) { vp->setPieViz(all, o ? all : std::vector<std::vector<Vec3>>{}); }; s.top = {every};
      for (auto& p : shown) {
        std::optional<int> via; for (auto& q : shown) if (&q != &p && ends(q) == ends(p)) for (auto& st : p) { bool inQ = false; for (auto& t : q) if (t.c == st.c) inQ = true; if (!inQ) { via = st.c; break; } }
        QString nm = QString("%1%2%3 (%4)").arg(on(p) ? "✓ " : "").arg(ends(p)).arg(via ? tr(" via %1").arg(sc.item(*via)->name) : QString()).arg(p.size());
        auto pts = aops::pathPts(sc, cc, p); NetPath pc = p;
        ui::PieItem it2 = item(nm, [vp, &sc, &cc, pc] { Selection sl = vp->selection(); QString m = aops::selectPath(sc, cc, sl, pc); vp->setSelectedItems({}); vp->setSelectedItems(sl.items); ui::toast(m); });
        it2.hot = [vp, all, pts](bool o) { vp->setPieViz(all, o ? std::vector<std::vector<Vec3>>{pts} : std::vector<std::vector<Vec3>>{}); };
        s.bottom.push_back(it2);
      }
      s.onEnd = vizOff; vp->setPieViz(all, {}); open(s, QCursor::pos(), false);
    });
    it.hot = [vp, allPts](bool on) { vp->setPieViz(on ? allPts : std::vector<std::vector<Vec3>>{}, {}); };
    return it;
  };
  // ⤳ another route for a service curve (wall >= 0: one side of that wall)
  std::function<void(int, int, char)> openRoute = [this, vp, &sc, &cc, curvePts, vizOff, open](int c, int wall, char side) {
    auto alts = aops::routeAlternatives(sc, cc, c); if (alts.empty()) { ui::toast(tr("No other route through the forks for this curve")); return; }
    auto ptsOf = [&](const aops::RouteAlt& a) { std::vector<Vec3> o; for (auto& x : a.st) { auto f = cc.find(x.c); if (f == cc.end()) continue; int lo = std::min(x.i0, x.i1), hi = std::max(x.i0, x.i1); std::vector<Vec3> q;
        for (size_t k = 0; k < f->second.p.size(); k++) if (f->second.seg[k] >= lo && f->second.seg[k] < hi) q.push_back(f->second.p[k]); if (x.i0 > x.i1) std::reverse(q.begin(), q.end()); o.insert(o.end(), q.begin(), q.end()); } return o; };
    auto cur = curvePts(c);
    QStringList rn; const Item* ci = sc.item(c); if (ci->hasPath()) for (auto& p : ci->path) { if (sc.item(p[0])) rn << sc.item(p[0])->name; } else rn << ci->name;
    ui::PieSpec s; s.title = tr("Route of %1%2: %3 — replace with:").arg(wall >= 0 ? sc.item(wall)->name + " · " : QString(), ci->name, rn.join(" → "));
    s.top = {item(tr("↺ Keep it as it is"), [] {})};
    for (auto& a : alts) {
      auto pts = ptsOf(a); std::vector<int> N = a.N;
      ui::PieItem it = item(!a.drop.empty() ? tr("%1 instead of %2").arg(few(sc, a.add), few(sc, a.drop)) : "+ " + few(sc, a.add),
                            [vp, c, N, wall, side] { vp->runEdit([=](Scene& s2, Selection& sl, const CurveCache& c2) { return aops::setRoute(s2, c2, sl, c, N, wall, side); }, true); });
      it.hot = [vp, cur, pts](bool on) { vp->setPieViz({cur}, on ? std::vector<std::vector<Vec3>>{pts} : std::vector<std::vector<Vec3>>{}); };
      s.bottom.push_back(it);
    }
    s.onEnd = vizOff; vp->setPieViz({cur}, {}); open(s, QCursor::pos(), false);
  };
  auto routeItems = [&]() {
    std::vector<ui::PieItem> out;
    for (int id : S.items) {
      const Item* it = sc.item(id); if (!it) continue;
      if (it->type == ItemType::Curve && it->hasPath()) out.push_back(more(tr("⤳ Change the route of %1").arg(it->name), [openRoute, id] { openRoute(id, -1, 0); }));
      if (it->type == ItemType::Surface && S.items.size() == 1) {
        auto Z = aops::wallZones(sc, id);
        if (!Z.empty()) {
          std::vector<std::vector<Vec3>> runs;
          for (auto& z : Z) for (auto [cid, i, j] : {std::tuple{it->a, z.a0, z.a1}, std::tuple{it->b, z.b0, z.b1}}) { auto Q = curvePts(cid); int n = int(sc.item(cid)->anchors.size()) - 1; if (Q.empty() || n <= 0) continue;
            auto f = [&](int x) { return int(std::lround(double(x) / n * (Q.size() - 1))); }; runs.push_back(std::vector<Vec3>(Q.begin() + f(std::min(i, j)), Q.begin() + f(std::max(i, j)) + 1)); }
          ui::PieItem sp = item(tr("✂ Split %1 into %2 walls").arg(it->name).arg(Z.size()), [vp, id] { vp->runEdit([id](Scene& s2, Selection& sl, const CurveCache& c2) { return aops::splitWall(s2, c2, sl, id); }, true); });
          sp.hot = [vp, runs](bool on) { vp->setPieViz({}, on ? runs : std::vector<std::vector<Vec3>>{}); }; out.push_back(sp);
        }
        for (char k : {'a', 'b'}) {
          int cid = k == 'a' ? it->a : it->b; const Item* c = sc.item(cid); if (!c || c->type != ItemType::Curve || !(c->hasPath() || segNet(*c))) continue;
          ui::PieItem ri = more(tr("⤳ Change the route of side %1 (%2)").arg(QChar(k).toUpper()).arg(c->name), [openRoute, cid, id, k] { openRoute(cid, id, k); });
          auto pts = curvePts(cid); ri.hot = [vp, pts](bool on) { vp->setPieViz(on ? std::vector<std::vector<Vec3>>{pts} : std::vector<std::vector<Vec3>>{}, {}); }; out.push_back(ri);
        }
      }
    }
    return out;
  };
  // ✂ separate a trunk
  auto trunkItem = [&](int T) -> std::optional<ui::PieItem> {
    auto opts = aops::trunkSplitOptions(sc, cc, T); if (opts.size() < 2) return std::nullopt;
    auto doIt = [vp, T](aops::TrunkOpt o) { vp->runEdit([=](Scene& s2, Selection& sl, const CurveCache& c2) { return aops::separateTrunk(s2, c2, sl, T, o); }, true); };
    if (opts.size() == 2 && opts[0].e == opts[1].e) return item(tr("✂ Separate the trunk — %1 | %2").arg(sc.item(opts[0].P)->name, sc.item(opts[1].P)->name), [doIt, o = opts[0]] { doIt(o); });
    return more(tr("✂ Separate the trunk (%1 branches)").arg(opts.size()), [this, vp, &sc, curvePts, T, opts, doIt, vizOff, open] {
      ui::PieSpec s; s.title = tr("Separate %1: which branch gets a trunk of its own").arg(sc.item(T)->name);
      const auto& A = sc.item(T)->anchors; int upE = sc.anchor(A.front())->pos.y >= sc.anchor(A.back())->pos.y ? 0 : 1;
      bool both = false; for (auto& q : opts) if (q.e != opts[0].e) both = true;
      auto tp = curvePts(T);
      for (auto& o : opts) { ui::PieItem it = item(QString("✂ %1%2").arg(sc.item(o.P)->name).arg(both ? (o.e == upE ? tr(" (above)") : tr(" (below)")) : QString()), [doIt, o] { doIt(o); });
        auto pp = curvePts(o.P); it.hot = [vp, tp, pp](bool on) { vp->setPieViz({tp}, on ? std::vector<std::vector<Vec3>>{pp, tp} : std::vector<std::vector<Vec3>>{}); }; s.bottom.push_back(it); }
      s.onEnd = vizOff; vp->setPieViz({tp}, {}); open(s, QCursor::pos(), false);
    });
  };

  // ---- anchors selected ----
  if (!S.anchors.empty()) {
    std::vector<int> as = S.anchors; int a = as.back();
    ui::PieSpec s; s.title = tr("Anchors selected: %1").arg(as.size());
    std::vector<ui::PieItem> bottom;
    if (lk) bottom.push_back(*lk);
    if (cut::isX(sc, a) && !cut::wallsAcrossX(sc, a).empty()) bottom.push_back(item(tr("✂ Separate the zones at this X (cut the lock across)"), [this, vp, a] {
      vp->runEdit([a](Scene& s2, Selection& sl, const CurveCache&) { int made = 0, kept = 0; if (!cut::separateAtX(s2, sl, a, &made, &kept)) return QString("!") + tr("This anchor is no X junction");
        return tr("Zones separated at the X: %1 walls above and below it — the curves stay whole%2").arg(made).arg(kept ? tr(" (%1 wall%2 with a curve too far from it — left as they were)").arg(kept).arg(kept > 1 ? "s" : "") : QString()); }, true); }));
    if (as.size() >= 2) bottom.insert(bottom.begin(), item(tr("∿ Create curve through the anchors (%1, in pick order)").arg(as.size()), [vp, as] { vp->runEdit([as](Scene& s2, Selection& sl, const CurveCache& c2) { return aops::curveThroughAnchors(s2, c2, as, sl); }, true); }));
    { std::set<int> selA(as.begin(), as.end()); std::vector<int> cs;
      for (const Item* c : sc.curves()) if (c->ring < 0 && !c->closed) for (int x : c->anchors) if (selA.count(x)) { cs.push_back(c->id); break; }
      if (!cs.empty()) bottom.push_back(item(tr("⚙ Optimize the selected anchors — keep the shape (%1)").arg(as.size()), [vp, cs, selA] { vp->runEdit([cs, selA](Scene& s2, Selection& sl, const CurveCache& c2) { sl.anchors.clear(); return aops::optimizeAnchors(s2, c2, cs, &selA); }, true); })); }
    if (as.size() == 2) bottom.insert(bottom.begin(), item(tr("∿ Create curve between the anchors"), [this, vp, as] {
      int fb = edit::targetGroup(scene_, vp->selection());
      vp->runEdit([this, as, fb](Scene& s2, Selection& sl, const CurveCache& c2) { return aops::curveBetween(s2, c2, as[0], as[1], fb, [this](const Vec3& mid, double span) { return viewport_->liftOffHead(mid, span); }, sl); }, true); }));
    if (auto jr = aops::junctionChoices(sc, a); !jr.empty()) {
      ui::PieItem rj; rj.label = tr("⤾ Rotate junction");
      for (auto& ch : jr) { auto pass = ch.pass; rj.sub.push_back(item(ch.label, [vp, a, pass] { vp->runEdit([a, pass](Scene& s2, Selection&, const CurveCache&) { s2.anchor(a)->pass = pass; return pass ? tr("Junction rotated") : tr("Junction: automatic tangents"); }, true); })); }
      bottom.push_back(rj);
    }
    if (as.size() == 1) if (auto sn = snapItem(a)) bottom.insert(bottom.begin(), *sn);
    { // an anchor on a crown connector (attached, or shared by a connector and a lock curve): the curve comes off it
      const Anchor* an = sc.anchor(a); const Item* ac = an->raw.value("at").isObject() ? sc.item(an->raw.value("at").toObject().value("c").toInt(-1)) : nullptr;
      bool onConn = false, onLock = false; for (int c : edit::curvesWith(sc, a)) { const Item* x = sc.item(c); if (!x->conn.isEmpty()) onConn = true; else if (x->ring < 0) onLock = true; }
      if ((ac && !ac->conn.isEmpty()) || (onConn && onLock)) bottom.push_back(item(tr("⛓ Detach from the connector"), [vp, a] { vp->runEdit([a](Scene& s2, Selection& sl, const CurveCache&) { return crown::detachFromConnector(s2, a, sl); }, true); }));
    }
    std::vector<int> glued; for (int x : as) { const Anchor* an = sc.anchor(x); const Item* h = an->raw.value("at").isObject() ? sc.item(an->raw.value("at").toObject().value("c").toInt(-1)) : nullptr;
      if (an->raw.contains("lk") || (an->raw.value("at").isObject() && !(h && !h->conn.isEmpty()))) glued.push_back(x); }
    if (!glued.empty()) bottom.push_back(item(tr("⇶ Unglue from the curve (%1)").arg(glued.size()), [vp, glued] {
      vp->runEdit([glued](Scene& s2, Selection&, const CurveCache&) {
        int n = 0; for (int x : glued) { Anchor* an = s2.anchor(x);
          if (an->raw.contains("lk")) { int b = an->raw.value("lk").toInt(); if (Anchor* pb = s2.anchor(b)) pb->raw.remove("lk"); an->raw.remove("lk"); n++; }
          else if (an->raw.value("at").isObject()) { an->raw.remove("at"); an->raw.remove("attPos"); n++; } }
        return n ? tr("⇶ %1 anchor%2 unglued — %3 on %4 own now").arg(n).arg(n == 1 ? "" : "s").arg(n == 1 ? tr("it moves") : tr("they move")).arg(n == 1 ? tr("its") : tr("their")) : QString("!") + tr("None of these anchors is glued"); }, true); }));
    s.top.push_back(item(tr("✖ Delete anchor"), [vp, as] { vp->deleteAnchors(as, false); }, true));
    s.top.push_back(item(tr("✖ Delete anchor, keep shape"), [vp, as] { vp->deleteAnchors(as, true); }, true));
    s.top.push_back(item(tr("↺ Recalculate handles"), [vp, as] { vp->recalcAnchors(as); }));
    if (auto z = zipItem(a)) s.top.push_back(*z); else s.top.push_back(fillItem());
    s.bottom = bottom;
    open(s, global, true);
    return;
  }

  // ---- items / curves ----
  std::vector<ui::PieItem> bottom{item(tr("▣ Create empty group"), [vp] { vp->createEmptyGroup(); })};
  if (lk) bottom.insert(bottom.begin(), *lk);
  std::vector<int> scs; for (int id : S.items) if (const Item* c = sc.item(id); c && c->type == ItemType::Curve) scs.push_back(id);
  auto n = [&](size_t k) { return k == 1 ? tr("curve") : tr("%1 curves").arg(k); };
  auto names = [&](const std::vector<edit::Path>& P) { QStringList l; for (auto& p : P) l << sc.item(p[0].c)->name; return l.join(", "); };
  if (scs.size() >= 2) {                                              // (2+ curves: making walls is the likely wish — first)
    bool tr2 = edit::twoTrunks(sc, cc, scs);
    auto walls = [vp, scs](bool closed, bool exact) { vp->runEdit([scs, closed, exact](Scene& s2, Selection& sl, const CurveCache& c2) { QString m; auto made = edit::createSurfaces(s2, c2, scs, closed, exact, &m); if (made.empty()) return "!" + m; sl.anchors.clear(); sl.items = made; return m; }, true); };
    std::vector<ui::PieItem> list{item(tr2 ? tr("▦ Wall between the two trunks (facing branches)") : scs.size() == 2 ? tr("▦ Create NURBS surface (wall)") : tr("▦ Walls between %1 curves (open)").arg(scs.size()), [walls] { walls(false, false); })};
    if (tr2) list.insert(list.begin(), item(scs.size() == 2 ? tr("▦ Wall between exactly these two curves") : tr("▦ Walls between exactly these %1 curves").arg(scs.size()), [walls] { walls(false, true); }));
    if (scs.size() >= 3) list.push_back(item(tr("◈ Volume lock from %1 curves (closed)").arg(scs.size()), [walls] { walls(true, true); }));
    list.push_back(item(tr("◭ Fill the hole between these curves"), [vp, scs] { vp->runEdit([scs](Scene& s2, Selection& sl, const CurveCache& c2) { int w = -1; QString m = edit::fillHole(s2, c2, scs, &w); if (w < 0) return "!" + m; sl.anchors.clear(); sl.items = {w}; return m; }, true); }));
    if (scs.size() == 2) list.push_back(item(tr("∿ Create average curve"), [vp, scs] { vp->runEdit([scs](Scene& s2, Selection& sl, const CurveCache& c2) { int id = edit::averageCurve(s2, c2, scs[0], scs[1]); if (id < 0) return QString("!"); sl.anchors.clear(); sl.items = {id}; return tr("Average curve created"); }, true); }));
    list.push_back(item(tr("⛓ Merge anchors at identical places"), [vp, scs] { vp->mergeCoincidentCurves(scs); }));
    bottom.insert(bottom.begin(), list.begin(), list.end());
  }
  // new logic: the selected pieces make whole paths — the walls go along them (service curves), not piece by piece
  { bool allSeg = scs.size() >= 2; for (int id : scs) if (!sc.item(id)->seg) allSeg = false;
    if (allSeg) if (auto pc = edit::pathCover(sc, cc, scs, false); pc && pc->size() >= 2 && pc->size() < scs.size()) { auto P = *pc;
      bottom.insert(bottom.begin(), item(tr("▦ Walls along the selected paths (%1 paths: %2)").arg(P.size()).arg(names(P)), [vp, P] { vp->runEdit([P](Scene& s2, Selection& sl, const CurveCache& c2) { QString m; auto made = edit::wallsAlongPaths(s2, c2, P, &m); if (made.empty()) return "!" + m; sl.anchors.clear(); sl.items = made; return m; }, true); })); } }
  if (!scs.empty()) if (auto ps = pathItem(scs)) { auto at = lk ? bottom.begin() + 1 : bottom.begin(); bottom.insert(at, *ps); }
  { auto rt = routeItems(); bottom.insert(bottom.begin(), rt.begin(), rt.end()); }
  if (!scs.empty()) {
    bottom.push_back(item(tr("↺ Recalculate all anchors (%1)").arg(n(scs.size())), [vp, &sc, scs] { vp->recalcAnchors(edit::anchorsOf(sc, scs)); }));
    bottom.push_back(item(tr("⚙ Optimize anchors — keep the shape (%1)").arg(n(scs.size())), [vp, scs] { vp->runEdit([scs](Scene& s2, Selection& sl, const CurveCache& c2) { sl.anchors.clear(); return aops::optimizeAnchors(s2, c2, scs, nullptr); }, true); }));
    bottom.push_back(item(tr("⇄ Reverse direction (%1)").arg(n(scs.size())), [vp, scs] { vp->reverseCurves(scs); }));
  }
  // several curves of the new logic (only curves): up — path Select, the fill outline; down — one way to make walls
  std::vector<int> netSel; for (int id : scs) { const Item* c = sc.item(id); if (!c->junc && !c->hasPath()) netSel.push_back(id); }
  bool allNet = netSel.size() >= 2; for (int id : netSel) if (!segNet(*sc.item(id)) || !edit::isNewLogic(sc, *sc.item(id))) allNet = false;
  if (allNet) {
    ui::PieSpec s; auto pc = edit::pathCover(sc, cc, netSel, true).value_or(std::vector<edit::Path>{});
    if (auto ps = pathItem(netSel)) s.top.push_back(*ps);
    s.top.push_back(fillItem());
    QString nn = tr("%1 curves").arg(netSel.size());
    s.bottom.push_back(item(tr("▦ Create NURBS surface (%1 path%2%3)").arg(pc.size()).arg(pc.size() == 1 ? "" : "s").arg(pc.size() >= 2 ? ": " + names(pc) : QString()), [vp, pc] {
      if (pc.size() < 2) { ui::toast(tr("The selection is one path — select two or more for a wall between them")); return; }
      vp->runEdit([pc](Scene& s2, Selection& sl, const CurveCache& c2) { QString m; auto made = edit::wallsAlongPaths(s2, c2, pc, &m); if (made.empty()) return "!" + m; sl.anchors.clear(); sl.items = made; return m; }, true); }));
    if (netSel.size() == 2) s.bottom.push_back(item(tr("∿ Create average curve"), [vp, netSel] { vp->runEdit([netSel](Scene& s2, Selection& sl, const CurveCache& c2) { int id = edit::averageCurve(s2, c2, netSel[0], netSel[1]); if (id < 0) return QString("!"); sl.anchors.clear(); sl.items = {id}; return tr("Average curve created"); }, true); }));
    s.bottom.push_back(item(tr("⛓ Merge anchors at identical places"), [vp, netSel] { vp->mergeCoincidentCurves(netSel); }));
    s.bottom.push_back(item(tr("↺ Recalculate all anchors (%1)").arg(nn), [vp, &sc, netSel] { vp->recalcAnchors(edit::anchorsOf(sc, netSel)); }));
    s.bottom.push_back(item(tr("⚙ Optimize anchors — keep the shape (%1)").arg(nn), [vp, netSel] { vp->runEdit([netSel](Scene& s2, Selection& sl, const CurveCache& c2) { sl.anchors.clear(); return aops::optimizeAnchors(s2, c2, netSel, nullptr); }, true); }));
    s.bottom.push_back(item(tr("⇄ Reverse direction (%1)").arg(nn), [vp, netSel] { vp->reverseCurves(netSel); }));
    open(s, global, true);
    return;
  }
  // one curve selected: the path Select up top (above the fill outline), no lock Select, no Create empty group
  ui::PieSpec s; s.top.push_back(fillItem());
  if (netSel.size() == 1 && (S.items.size() == 1 || edit::isNewLogic(sc, *sc.item(netSel[0])))) {
    std::vector<ui::PieItem> b2; std::optional<ui::PieItem> pathTop;
    for (auto& b : bottom) { if ((lk && b.label == lk->label) || b.label == tr("▣ Create empty group")) continue; if (b.label.contains(tr("paths through the forks")) && !pathTop) { pathTop = b; continue; } b2.push_back(b); }
    bottom = b2; if (pathTop) s.top.insert(s.top.begin(), *pathTop);
    if (auto tp = trunkItem(netSel[0])) bottom.insert(bottom.begin(), *tp);
  }
  s.bottom = bottom;
  open(s, global, true);
}


// the Topology room's Shift+RMB: a ring — the selection's kinds (what a held LMB does), the edits of the selected, the
// selection's walks
void MainWindow::syncUnhide() { if (unhideBtn_) unhideBtn_->setVisible(scene_.room == "topo" && viewport_->anyHidden()); }


// Shift+RMB: the selection's kinds (what a held LMB does) and, inside, the kind's settings — switched in place (the menu
// stays); Ctrl+RMB (openTopoOpsPie): the edits of the selected
void MainWindow::openTopoPie(const QPoint& global, bool hold) {
  Viewport* vp = viewport_;
  auto build = [vp]() -> ui::PieSpec {
    ui::PieSpec s; s.bare = true;
    auto kind = [vp](const QString& k, const QString& label, const QString& tip) {
      ui::PieItem it; it.icon = "tsel_" + k; it.label = label; it.tip = tip; it.act = [vp, k] { vp->setSelTool(k); }; it.cur = vp->tool() == "cursor" && vp->tset.selTool == k; return it; };
    s.ring = {
      kind("cursor", tr("Select Cursor"), tr("A click picks, a held LMB paints the selection; a double click — the whole piece; a click and a double click beside it — the loop, further on — every so many along it")),
      kind("lasso", tr("Lasso"), tr("A held LMB draws the outline to select inside")),
      kind("outline", tr("Outline"), tr("A click picks the open outline — or the picked polygons' border")),
      kind("loop", tr("Loop"), tr("The loop under the pointer lit yellow — a click picks it (Shift adds, Ctrl takes off)")),
      kind("ring", tr("Ring"), tr("The parallel ring under the pointer lit yellow — a click picks it (Shift adds, Ctrl takes off)")),
      kind("hard", tr("Hard edges"), tr("The line of hard edges under the pointer lit — a click picks it (Shift adds, Ctrl takes off)")),
      kind("fill", tr("Fill"), tr("A click picks the region under the pointer — up to a cut or the selection's frame; Ctrl — the selected piece taken off")),
      kind("rect", tr("Rectangle"), tr("A held LMB draws a box to select in")),
    };
    // (the inner ring: the kind's settings — a click switches one, the menu stays as it is)
    auto opt = [vp](const QString& label, bool on, const QString& tip, std::function<void()> flip) {
      ui::PieItem it; it.label = (on ? "☑ " : "☐ ") + label; it.tip = tip; it.cur = on; it.keep = true;
      it.act = [vp, flip] { flip(); vp->saveTopoPrefs(); vp->topoChanged(); }; return it; };
    const QString st = vp->tset.selTool;
    if (st == "rect" || st == "lasso" || st == "cursor")
      s.inner.push_back(opt(tr("Visible only"), !vp->tset.selThrough, tr("On — only what the camera sees is picked; off — the hidden side too (everything inside)"),
                            [vp] { vp->tset.selThrough = !vp->tset.selThrough; }));
    if (st == "loop" || st == "ring") {
      s.inner.push_back(opt(tr("Stop before selected"), vp->tset.loopStopSel, tr("The walk ends before an element already picked"), [vp] { vp->tset.loopStopSel = !vp->tset.loopStopSel; }));
      s.inner.push_back(opt(tr("Stop before n-gon"), vp->tset.loopStopNgon, tr("The walk ends before a polygon of more than four sides"), [vp] { vp->tset.loopStopNgon = !vp->tset.loopStopNgon; }));
      s.inner.push_back(opt(tr("Stop before tris"), vp->tset.loopStopTri, tr("The walk ends before a triangle"), [vp] { vp->tset.loopStopTri = !vp->tset.loopStopTri; }));
    }
    if (vp->tset.soft) {
      const QString tip = tr("Soft Selection (B): Object only — only what is joined to the selection moves, the parts near it don't; Radius — everything of the mesh within the radius");
      s.inner.push_back(opt(tr("Soft: object only"), vp->tset.softMode == "surface", tip, [vp] { vp->tset.softMode = "surface"; }));
      s.inner.push_back(opt(tr("Soft: radius"), vp->tset.softMode == "volume", tip, [vp] { vp->tset.softMode = "volume"; }));
    }
    return s;
  };
  ui::PieSpec s = build(); s.rebuild = build;
  ui::Pie::open(this, global, s, hold);
}

void MainWindow::openTopoOpsPie(const QPoint& global, bool hold) {
  Viewport* vp = viewport_; ui::PieSpec s; s.bare = true;
  auto ic = [vp](const QString& icon, const QString& label, const QString& tip, std::function<void()> act) { ui::PieItem it; it.icon = icon; it.label = label; it.tip = tip; it.act = [vp, label, act] { vp->heavyWork(label, act); }; return it; };   // (a high-poly mesh: the wait shown)
  ui::PieItem selMir = ic("top_selmirror", tr("Select Mirror"), tr("The selection copied onto the other side of X"), [this, vp] {
    ui::PieSpec m; m.title = tr("Select Mirror");
    ui::PieItem a; a.label = tr("+X → −X  (right onto left)"); a.act = [vp] { vp->tMirrorSel(1); };
    ui::PieItem b; b.label = tr("−X → +X  (left onto right)"); b.act = [vp] { vp->tMirrorSel(-1); };
    m.top = {a, b}; ui::Pie::open(this, QCursor::pos(), m, false); });
  s.ring = {
    ic("top_mirror", tr("Mirror"), tr("A mirrored copy of the selected polygons in the same polygroup (welded at the middle when asked)"), [vp] { vp->tMirrorGeo(); }),
    ic("top_melt", tr("Melt"), tr("Edges / polygons: the edges between them gone — the polygons one; vertices: dissolved — the polygons round each one"), [vp] { vp->tMelt(); }),
    selMir,
    ic("top_selset", tr("Selection Set"), tr("The selection kept as a set — in the mesh's settings, Selection sets"), [vp] { vp->tSelSetCreate(); }),
    ic("top_shrink", tr("Shrink"), tr("The selection one ring smaller"), [vp] { vp->tGrowSel(true); }),
    ic("top_grow", tr("Grow"), tr("The selection one ring bigger"), [vp] { vp->tGrowSel(false); }),
    ic("top_invert", tr("Invert"), tr("Invert the selection: every element of the mode not picked becomes picked, the picked ones not (Ctrl+I)"), [vp] { vp->tInvertSel(); }),
    ic("top_splitpg", tr("Split2PG"), tr("Split to Polygroup: the selected polygons into a new polygroup"), [vp] { vp->tSplitToGroup(); }),
    ic("top_hard", tr("Hard Edges"), tr("The selected edges hard: the shading broken along them (polygons — all their edges; vertices — the edges between them)"), [vp] { vp->tHardEdges(true); }),
    ic("top_soft", tr("Soft Edges"), tr("The selected edges soft: smooth across them again"), [vp] { vp->tHardEdges(false); }),
    ic("top_split", tr("Split"), tr("The selected polygons no longer welded to the rest (the polygroup kept)"), [vp] { vp->tSplitSel(); }),
    ic("top_detach", tr("Detach"), tr("The selected polygons a separate mesh"), [vp] { vp->tDetachSel(); }),
  };
  ui::Pie::open(this, global, s, hold);
}

// the outliner's own pie (its button over the outliner): an empty group, an empty group at the selection (its own
// point there — e.g. a vertex picked on a mesh, or the middle of what is picked), the picked meshes merged into one
// (where its pivot goes asked next: the world's origin, the geometry's middle, or one of the merged meshes' pivots)
void MainWindow::openOutlinerPie(const QPoint& global) {
  Viewport* vp = viewport_; ui::PieSpec s; s.title = tr("Outliner");
  if (!vp->selection().items.empty()) s.top.push_back(item(tr("▣ Group the selected (Alt+G)"), [this] { outliner_->groupSelection(); }));
  s.top.push_back(item(tr("▣ Create empty group"), [vp] { vp->createEmptyGroup(); }));
  if (auto c = vp->selectionCentre()) s.top.push_back(item(tr("⊕ Empty group at the selection"), [vp, p = *c] { vp->createEmptyGroup(&p); }));
  const auto ms = vp->mergeableMeshes();
  if (ms.size() >= 2) s.bottom.push_back(more(tr("⧉ Merge %1 meshes into one").arg(ms.size()), [this, vp, ms] {
    // (the pivot of the merged mesh: chosen in the next menu)
    ui::PieSpec q; q.title = tr("Where does the gizmo go?");
    Box3 b; for (int id : ms) if (const Item* it = scene_.item(id); it && it->poly) b.unite(it->poly->bounds());
    q.top.push_back(item(tr("⊙ World origin (0, 0, 0)"), [vp, ms] { vp->mergeMeshes(ms, Vec3()); }));
    q.top.push_back(item(tr("◈ Centre of the geometry"), [vp, ms, c = b.center()] { vp->mergeMeshes(ms, c); }));
    for (int id : ms) { const Item* it = scene_.item(id); const Vec3 p = meshmods::meshMatrix(scene_, *it).apply(vp->meshPivotLocal(id));
      const Quat ax = (vp->meshOwnQ(id) * vp->meshGizmoQ(id)).normalized();   // (its gizmo's axes too — not only its point)
      q.bottom.push_back(item(tr("✥ Pivot of %1").arg(it->name), [vp, ms, p, ax] { vp->mergeMeshes(ms, p, ax); })); }
    ui::Pie::open(this, QCursor::pos(), q, false); }));
  else s.bottom.push_back(item(tr("⧉ Merge — pick two meshes or more"), [] { ui::toast(QObject::tr("Pick two meshes or more in the outliner (Ctrl / Shift + click) to merge them")); }));
  ui::Pie::open(this, global, s, true);
}
}  // namespace hm
