// ⌇ The Braid tool's window (Create braid on the selected curves; the selected braid's settings) and a braid's settings
// in the properties panel: the weave, the shape along it (start / middle / end), the stylization, the ends (separate /
// shared / point), each strand's cross-section (the profile editor), the cocoon, the mesh, Convert to locks.
#include "app/BraidPanels.h"
#include "app/ModPanels.h"
#include "app/ToolPanels.h"
#include "scene/Braid.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QMenu>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <tuple>
#include <QVBoxLayout>
#include <cmath>
#include <functional>
#include <memory>

namespace hm {

namespace {
QLabel* h4(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setWordWrap(true); l->setStyleSheet("margin-top:4px"); return l; }
QLabel* muted(const QString& t) { auto* l = new QLabel(t); l->setProperty("mut", true); l->setWordWrap(true); return l; }
QPushButton* button(const QString& t, const QString& tip = {}, bool primary = false) {
  auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); if (primary) b->setProperty("primary", true); return b;
}
QString pct(double v) { return QString("%1%").arg(int(std::lround(v * 100))); }
QString whole(double v) { return QString::number(int(std::lround(v))); }
QString deg(double v) { return QString("%1°").arg(int(std::lround(v))); }
QString times(double v) { return QString("×%1").arg(v, 0, 'f', 2); }
QString fmtLen(double v) { if (v == 0) return "0"; const int e = int(std::floor(std::log10(std::abs(v)))); return QString::number(v, 'f', std::clamp(2 - e, 0, 6)); }
QJsonObject P(const Item& it) { return it.raw.value("p").toObject(); }
void setP(Scene& sc, int id, const QString& k, const QJsonValue& v) { Item* it = sc.item(id); if (!it) return; QJsonObject p = it->raw.value("p").toObject(); p.insert(k, v); it->raw.insert("p", p); }
// (a setting let go: one undo step, the ends' connectors brought in line with it)
void commitBraid(Viewport* vp, int id) { const Vec3 cen = vp->camera().sceneCenter; vp->runEdit([id, cen](Scene& s, Selection&, const CurveCache& cc) { braid::updateConnectors(s, cc, id, cen); return QString(); }, true); vp->liveTubes(); }
int curStrand = 0;                                        // (the strand whose profile is being edited)

bool endKindOfAny(const Item& b) { return braid::endKindOf(b, 0) == "shared" || braid::endKindOf(b, 1) == "shared"; }
// a braid the selection means: a braid, its folder, its curve, one of its connectors
int braidOfSel(Viewport* vp, Scene* sc) { for (int id : vp->selection().items) if (const int b = braid::braidOfItem(*sc, id); b >= 0) return b; return -1; }
}  // namespace

void segProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id, const std::function<SliderRow*(QVBoxLayout*, const QString&, double, double, double, const QString&, double, std::function<QString(double)>, const QString&, bool, double)>& slider,
              const std::function<void(QVBoxLayout*, std::vector<std::pair<QString, QString>>, const QString&, const QString&, const QString&)>& choices,
              const std::function<void(QVBoxLayout*, const QString&, bool, const QString&, const QString&)>& check);

void braidProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  Item* it = sc->item(id); if (!it) return; const int src = it->raw.value("src").toInt(-1); const Item* c = sc->item(src);
  const braid::Params p = braid::paramsOf(*it);
  if (it->raw.value("connVer").toInt() != 3) QTimer::singleShot(0, vp, [vp, id] { commitBraid(vp, id); });   // (a braid of an earlier version: its connectors made as the end kinds want them now)
  auto live = [vp] { vp->liveTubes(); };
  auto slider = [vp, sc, id, live](QVBoxLayout* box, const QString& label, double lo, double hi, double step, const QString& key, double val, std::function<QString(double)> fmt, const QString& tip = {}, bool rebuild = false, double reset = -1e300) {
    auto* r = new SliderRow(label, lo, hi, step, val, fmt); if (!tip.isEmpty()) r->setTip(tip); if (reset > -1e299) r->setReset(reset);
    QObject::connect(r, &SliderRow::changed, vp, [sc, id, key, live](double x) { setP(*sc, id, key, x); live(); });
    QObject::connect(r, &SliderRow::released, vp, [vp, id, rebuild](double) { commitBraid(vp, id); if (rebuild) emit vp->brushChanged(); });
    box->addWidget(r); return r; };
  auto choices = [vp, sc, id, live](QVBoxLayout* box, std::vector<std::pair<QString, QString>> items, const QString& cur, const QString& key, const QString& tip = {}) {
    auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setSpacing(3); int i = 0; const int cols = items.size() > 3 ? 4 : int(items.size());
    for (auto& [k, label] : items) { auto* b = button(label, tip); b->setCheckable(true); b->setChecked(k == cur); const QString kk = k;
      QObject::connect(b, &QPushButton::clicked, vp, [vp, sc, id, key, kk, live] { setP(*sc, id, key, kk); live(); commitBraid(vp, id); emit vp->brushChanged(); }); g->addWidget(b, i / cols, i % cols); i++; }
    box->addWidget(w); };
  auto check = [vp, sc, id, live](QVBoxLayout* box, const QString& label, bool on, const QString& key, const QString& tip) {
    auto* cb = new QCheckBox(label); cb->setChecked(on); cb->setToolTip(tip);
    QObject::connect(cb, &QCheckBox::toggled, vp, [vp, sc, id, key, live](bool x) { setP(*sc, id, key, x); live(); commitBraid(vp, id); emit vp->brushChanged(); }); box->addWidget(cb); };

  // the mode: strands woven as tubes, or segments (a fake braid of lobes round a core)
  choices(v, {{"weave", QObject::tr("Woven")}, {"segments", QObject::tr("Segments")}}, braid::segMode(*it) ? "segments" : "weave", "mode",
          QObject::tr("Woven — strands woven along the curve as separate tubes; Segments — a fake braid: lobes in a row round a core, its connector ends stitched to the attached locks on Convert"));
  if (braid::segMode(*it)) { segProps(v, vp, sc, id, slider, choices, check); return; }
  v->addWidget(muted(QObject::tr("Strands woven along %1 as separate tubes — edit the curve, the braid follows.").arg(c ? c->name : QObject::tr("(deleted curve)"))));

  v->addWidget(h4(QObject::tr("Weave")));
  choices(v, {{"classic", QObject::tr("Classic")}, {"dutch", QObject::tr("Dutch")}, {"fishtail", QObject::tr("Fishtail")}, {"rope", QObject::tr("Rope")}}, p.pattern, "pattern",
          QObject::tr("Classic — the outer strand over to the middle; Dutch — under (the braid stands out); Fishtail — many thin strands, each over half of them; Rope — the strands twisted round each other"));
  slider(v, QObject::tr("Strands"), 2, 16, 1, "strands", p.strands, whole, QObject::tr("How many strands (Fishtail: 6 and more)"), true);
  slider(v, QObject::tr("Crossings"), 1, 120, 1, "cross", p.cross, whole, QObject::tr("How densely it is woven: crossings along the whole length (Rope: half-turns)"));
  const double wr = std::max(p.width, 1e-3);
  slider(v, QObject::tr("Width"), 0, wr * 4, wr / 400, "width", p.width, fmtLen, QObject::tr("Across the braid"));
  slider(v, QObject::tr("Depth"), 0, 3, 0.01, "depth", p.depth, pct, QObject::tr("How far the strands go over / under each other"));
  slider(v, QObject::tr("Strand size"), 0.2, 2.5, 0.01, "size", p.size, pct, QObject::tr("The strands' thickness"));
  slider(v, QObject::tr("Tightness"), 0, 1, 0.01, "tight", p.tight, pct, QObject::tr("0 — loose, the strands just touch; 1 — pressed into each other"));
  slider(v, QObject::tr("Rotation"), -180, 180, 1, "rot", p.rot, deg, QObject::tr("The braid turned round its curve (its flat side outwards from the head at 0°)"));

  // the shape along it: three settings at the start, middle and end, rounded between them
  v->addWidget(h4(QObject::tr("Shape along (start · middle · end)")));
  const QString at[3] = {QObject::tr("start"), QObject::tr("middle"), QObject::tr("end")};
  auto keys = [&](const QString& what, const char* key, const double* val, double lo, double hi, const QString& tip) {
    v->addWidget(muted(what));
    for (int k = 0; k < 3; k++) slider(v, QObject::tr("  at the %1").arg(at[k]), lo, hi, 0.01, QString("%1%2").arg(key).arg(k), val[k], times, tip, false, 1); };
  keys(QObject::tr("Width"), "w", p.wk, 0.1, 3, QObject::tr("How wide the braid is there (× its width) — double-click: ×1"));
  keys(QObject::tr("Density"), "d", p.dk, 0.2, 4, QObject::tr("How densely it is woven there: more — the crossings closer together (the count stays) — double-click: ×1"));
  keys(QObject::tr("Thickness"), "t", p.tk, 0.05, 3, QObject::tr("How thick the strands are there — double-click: ×1"));
  slider(v, QObject::tr("Rounding"), 0, 1, 0.01, "round", p.round, pct, QObject::tr("Between the start, middle and end: 0 — straight changes (a corner at the middle), 100% — rounded, smooth"));

  v->addWidget(h4(QObject::tr("Stylize")));
  slider(v, QObject::tr("Sharpen"), 0, 1, 0.01, "sharp", p.sharp, pct, QObject::tr("Each lobe sharpened into a triangle: thick where the strand comes out on top, tapering to a point where it goes under"));
  slider(v, QObject::tr("Squash"), 0, 1, 0.01, "squash", p.squash, pct, QObject::tr("Where the strands meet they are flattened against each other, as if pressing in"));

  v->addWidget(h4(QObject::tr("Ends")));
  const std::vector<std::pair<QString, QString>> ends{{"separate", QObject::tr("Separate")}, {"shared", QObject::tr("Shared")}, {"point", QObject::tr("Point")}};
  const QString endTip = QObject::tr("Separate — a connector round each tube's end (move it — the tube's end follows); Shared — all the tubes gathered into one, one connector round it; Point — no connector, the end comes to a point (rounded or sharp)");
  for (int e = 0; e < 2; e++) {
    const QString kind = e ? p.end1 : p.end0;
    v->addWidget(muted(e ? QObject::tr("End") : QObject::tr("Start")));
    choices(v, ends, kind, e ? "end1" : "end0", endTip);
    if (kind == "point") slider(v, QObject::tr("Tip rounding"), 0, 1, 0.01, e ? "tip1" : "tip0", e ? p.tip1 : p.tip0, pct, QObject::tr("0 — the tubes come to a sharp point, 100% — a round end"));
  }
  if (p.end0 != "separate" || p.end1 != "separate") slider(v, QObject::tr("Gathering"), 0.01, 0.4, 0.005, "conv", p.conv, pct, QObject::tr("The share of the length over which the strands come together (Shared, Point) — and how far a moved connector pulls the tube (Separate)"));
  if (p.end0 == "separate" || p.end1 == "separate") v->addWidget(muted(QObject::tr("Separate: a ring round each tube's end — move it (or its anchors), the tube's end goes with it and takes its shape; drag a lock curve's end onto it to attach the lock.")));
  if (p.end0 == "shared" || p.end1 == "shared") v->addWidget(muted(QObject::tr("Shared: one ring round the gathered tubes — reshape it, the end follows.")));
  { auto* rc = button(QObject::tr("Reset connectors"), QObject::tr("The connectors made anew on the ends' own outline, back in place (attached locks stay on them)"));
    QObject::connect(rc, &QPushButton::clicked, vp, [vp, id] { const Vec3 cen = vp->camera().sceneCenter;
      vp->runEdit([id, cen](Scene& s, Selection&, const CurveCache& cc) { braid::updateConnectors(s, cc, id, cen, true); return QString(); }, true); vp->liveTubes(); });
    v->addWidget(rc); }

  // each strand's cross-section
  v->addWidget(h4(QObject::tr("Strands' cross-section")));
  const int N = std::clamp(p.strands, 2, 16); curStrand = std::clamp(curStrand, 0, N - 1);
  check(v, QObject::tr("One profile for every strand"), p.shared, "shared", QObject::tr("Off: each strand gets a profile of its own (pick it below)"));
  auto Pr = std::make_shared<std::vector<tube::Profile>>(braid::profilesOf(*it, N));
  auto write = [sc, id, Pr, live] { Item* b = sc->item(id); if (!b) return; QJsonArray a; const bool shared = P(*b).value("shared").toBool(true);
    if (shared) a.append(tube::profileJson((*Pr)[0])); else for (auto& x : *Pr) a.append(tube::profileJson(x)); b->raw.insert("profs", a); live(); };
  const int k = p.shared ? 0 : curStrand;
  if (!p.shared) {
    auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setSpacing(3);
    for (int i = 0; i < N; i++) { auto* b = button(QString::number(i + 1), QObject::tr("Edit strand %1's profile").arg(i + 1)); b->setCheckable(true); b->setChecked(i == curStrand);
      QObject::connect(b, &QPushButton::clicked, vp, [vp, i] { curStrand = i; emit vp->brushChanged(); }); g->addWidget(b, i / 8, i % 8); }
    v->addWidget(w);
  }
  ProfileEditor::Opts o; o.glue = true;
  auto* ed = new ProfileEditor([Pr, k] { return &(*Pr)[size_t(k)]; }, [write] { write(); }, [vp, id] { commitBraid(vp, id); }, o);
  v->addWidget(ed);
  auto* presets = new QComboBox; presets->addItem(QObject::tr("Profile preset…"), QString());
  for (auto& q : tube::builtinProfiles()) presets->addItem(q.name, q.name);
  for (auto& q : tube::userProfiles()) presets->addItem(q.name, q.name);
  QObject::connect(presets, &QComboBox::activated, vp, [vp, id, presets, Pr, k, write](int) { auto q = tube::profileByName(presets->currentData().toString()); if (!q) return; (*Pr)[size_t(k)] = *q; write(); commitBraid(vp, id); emit vp->brushChanged(); });
  v->addWidget(presets);
  if (!p.shared) { auto* all = button(QObject::tr("This profile to every strand"));
    QObject::connect(all, &QPushButton::clicked, vp, [vp, id, Pr, k, write] { for (auto& x : *Pr) x = (*Pr)[size_t(k)]; write(); commitBraid(vp, id); emit vp->brushChanged(); }); v->addWidget(all); }
  v->addWidget(muted(QObject::tr("LMB — drag a point, RMB — add one, double click — delete. A point brought onto its neighbour glues to it: a hard edge along the strand (Ctrl + drag pulls them apart); pull a point inwards — a dent.")));

  // the cocoon: one round the braid (every tube's rings on its shared levels) or one round each tube (rings across itself)
  v->addWidget(h4(QObject::tr("Cocoon")));
  choices(v, {{"shared", QObject::tr("One shared")}, {"each", QObject::tr("One per tube")}}, p.cocoon, "cocoon",
          QObject::tr("One shared — a cocoon round the whole braid: every tube's rings lie on its levels, across the braid; One per tube — each tube in a cocoon of its own: its rings across the tube itself (even where it runs slanted)"));
  slider(v, QObject::tr("Rings / crossing"), 2, 40, 1, "prev", p.prev, whole, QObject::tr("How many rings each tube has per crossing"));

  // the mesh
  v->addWidget(h4(QObject::tr("Mesh")));
  { const PolyMesh M = braid::tubesMesh(*sc, vp->hair().curves, *it, vp->camera().sceneCenter);
    v->addWidget(muted(QObject::tr("%1 tubes — %2 polygons. A polygroup each; glued profile points — hard edges. Convert to geometry makes the braid a mesh of its own: not welded to the locks or to other braids.").arg(N).arg(M.faceCount()))); }
  check(v, QObject::tr("Close the cut ends"), p.caps, "caps", QObject::tr("An n-gon over each open end of a tube"));
  auto* mk = button(QObject::tr("Make separate tubes"), QObject::tr("This mesh into the scene now (an imported mesh beside the braid)"), true);
  QObject::connect(mk, &QPushButton::clicked, vp, [vp, id] { const Vec3 cen = vp->camera().sceneCenter; vp->runEdit([id, cen](Scene& s, Selection& sl, const CurveCache& cc) { return braid::makeMesh(s, cc, sl, id, cen); }, true); });
  v->addWidget(mk);

  // Convert to locks: each strand a curve with a Tube modifier (the usual pipeline from there)
  v->addWidget(h4(QObject::tr("Convert to locks")));
  v->addWidget(muted(QObject::tr("Each strand becomes a curve along its middle with a Tube modifier of its cross-section and thickness — set them up, then Convert as any lock. The braid is hidden.")));
  slider(v, QObject::tr("Anchors / crossing"), 1, 8, 1, "lockAnchors", p.lockAnchors, whole, QObject::tr("How many anchors each lock's curve gets per crossing"));
  slider(v, QObject::tr("Tube thickness"), 0.2, 2, 0.01, "lockR", p.lockR, times, QObject::tr("The modifiers' radius × the strands' thickness"), false, 1);
  check(v, QObject::tr("Point ends collapse"), p.lockTip, "lockTip", QObject::tr("A Point end: the modifier's tip collapses to a point"));
  auto* lk = button(QObject::tr("Convert to locks"), QObject::tr("Each strand a curve with a Tube modifier, in a group of its own"));
  QObject::connect(lk, &QPushButton::clicked, vp, [vp, id] { const Vec3 cen = vp->camera().sceneCenter; vp->runEdit([id, cen](Scene& s, Selection& sl, const CurveCache& cc) { return braid::toLocks(s, cc, sl, id, cen); }, true); });
  v->addWidget(lk);

  auto* scb = button(QObject::tr("Select curve")); QObject::connect(scb, &QPushButton::clicked, vp, [vp, src, c] { if (c) vp->setSelectedItems({src}); });
  auto* sel = button(QObject::tr("Select connectors"));
  QObject::connect(sel, &QPushButton::clicked, vp, [vp, sc, id] { std::vector<int> l; for (const QJsonValue& x : sc->item(id)->raw.value("conns").toArray()) if (sc->item(x.toInt())) l.push_back(x.toInt()); if (!l.empty()) vp->setSelectedItems(l); });
  auto* bar = new QWidget; auto* bl = new QHBoxLayout(bar); bl->setContentsMargins(0, 4, 0, 0); bl->setSpacing(4); bl->addWidget(scb); bl->addWidget(sel); v->addWidget(bar);
}

// ⌇ a braid of segments' settings: the type (Type 1 chevrons / Type 2 twist), the lobes' shape, the procedural topology,
// the shape along it, the ends (a connector — the lock attached there is stitched on Convert — or a point), the extra
// modifiers' brushes, the mesh
void segProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id, const std::function<SliderRow*(QVBoxLayout*, const QString&, double, double, double, const QString&, double, std::function<QString(double)>, const QString&, bool, double)>& slider,
              const std::function<void(QVBoxLayout*, std::vector<std::pair<QString, QString>>, const QString&, const QString&, const QString&)>& choices,
              const std::function<void(QVBoxLayout*, const QString&, bool, const QString&, const QString&)>& check) {
  Item* it = sc->item(id); if (!it) return; const int src = it->raw.value("src").toInt(-1); const Item* c = sc->item(src);
  const braid::Params p = braid::paramsOf(*it); const braid::SegParams S = braid::segParamsOf(*it); const bool tw = S.type == "twist";
  auto live = [vp] { vp->liveTubes(); };
  auto sl = [&](const QString& label, double lo, double hi, double step, const QString& key, double val, std::function<QString(double)> fmt, const QString& tip, double reset = -1e300) { return slider(v, label, lo, hi, step, key, val, fmt, tip, false, reset); };

  // the type: two buttons; a change takes the type's own lobe settings (its defaults) — the rest stays
  { auto* w = new QWidget; auto* g = new QHBoxLayout(w); g->setContentsMargins(0, 2, 0, 2); g->setSpacing(3);
    for (auto [k, label, tip] : {std::tuple<QString, QString, QString>{"chevron", QObject::tr("Type 1 · Chevron"), QObject::tr("A band of each strand a segment, round its side, the other strand mirrored at the same height: on the front they make a V-shape; laid as roof tiles")},
                                 {"twist", QObject::tr("Type 2 · Twist"), QObject::tr("Each segment a lemon: two cylinders joined into a figure eight, closed to points; stacked point to point and twisted")}}) {
      auto* b = button(label, tip); b->setCheckable(true); b->setChecked(S.type == k);
      QObject::connect(b, &QPushButton::clicked, vp, [vp, sc, id, k = k, live] { Item* x = sc->item(id); if (!x) return; QJsonObject q = x->raw.value("p").toObject();
        q.insert("stype", k); for (const char* d : {"sdepth", "span", "stw", "spin", "bulge", "flat", "core", "ssharp", "wrap", "around", "along", "stagger", "lean", "segs"}) q.remove(d); x->raw.insert("p", q); live(); commitBraid(vp, id); emit vp->brushChanged(); });
      g->addWidget(b, 1); }
    v->addWidget(w); }

  v->addWidget(h4(QObject::tr("Segments")));
  sl(QObject::tr("Segments"), 0, 80, 1, "segs", S.segs, [](double x) { return x < 0.5 ? QObject::tr("auto") : whole(x); }, QObject::tr("How many segments along the braid; 0 — by its length, as in the video: a segment about as long as the braid is wide. Double-click — auto"), 0);
  if (tw) sl(QObject::tr("Cylinders"), 1, 4, 1, "lobes", S.lobes, whole, QObject::tr("Cylinders in each segment, side by side round the middle (2 — a figure eight across, as in the video; 1 — round beads)"));
  if (tw) sl(QObject::tr("Crease"), 0, 0.95, 0.01, "crease", S.crease, pct, QObject::tr("How deep the dent between the cylinders is"), 0.9);
  const double wr = std::max(p.width, 1e-3);
  sl(QObject::tr("Width"), 0, wr * 4, wr / 400, "width", p.width, fmtLen, QObject::tr("Across the braid"));
  sl(QObject::tr("Depth"), 0.2, 2, 0.01, "sdepth", S.depth, pct, QObject::tr("Front to back, × the width"));
  sl(QObject::tr("Piece length"), 0.5, 4, 0.01, "span", S.span, times, QObject::tr("Each piece's length in segments: over ×1 — it goes into the next"), tw ? 1.1 : 1.4);
  sl(QObject::tr("Fullness"), 0, 1.5, 0.01, "bulge", S.bulge, pct, QObject::tr("How plump the lobes are"));
  sl(QObject::tr("Pointed ends"), 0, 1, 0.01, "ssharp", S.sharp, pct, QObject::tr("0 — round ends of the pieces, 100% — sharp points"), tw ? 0.1 : 0.3);
  if (!tw) sl(QObject::tr("Flatten"), 0, 0.95, 0.01, "flat", S.flat, pct, QObject::tr("The bands pressed flat against the braid"), 0.3);
  if (!tw) sl(QObject::tr("Stand out"), -1, 1, 0.01, "lean", S.lean, pct, QObject::tr("The bands standing out of the braid's outline (below 0 — sunk into it)"), 0.4);
  if (!tw) {
    sl(QObject::tr("Wrap"), 40, 200, 1, "wrap", S.wrap, deg, QObject::tr("How far round the braid each band goes: 90° — from its side to the middle, more — over it"), 95);
    sl(QObject::tr("Stagger"), 0, 1, 0.01, "stagger", S.stagger, pct, QObject::tr("The right strand's bands moved along the braid, in segments (0 — as in the video: both at one height, their front diamonds making the V-shapes)"), 0);
    sl(QObject::tr("Shingle"), 0, 2, 0.01, "shingle", S.shingle, pct, QObject::tr("The bands laid as roof tiles: each one's lower side out, its upper side under the one above — the steps between the V-shapes"), 0.8);
    check(v, QObject::tr("V up"), S.flip, "flip", QObject::tr("The V-shapes point up the braid instead of down"));
    check(v, QObject::tr("Back leaves"), S.back, "back", QObject::tr("Leaves on the back too (off — the front only: the braid lies against something)"));
  }
  sl(QObject::tr("Turn / segment"), -180, 180, 1, "stw", S.tw, deg, QObject::tr("Each next segment turned round the braid by this"), 0);
  if (tw) sl(QObject::tr("Spiral"), -360, 360, 1, "spin", S.spin, deg, QObject::tr("Each segment twisted along itself: the crease runs round it (90° — as in the video)"), 90);
  sl(QObject::tr("Rotation"), -180, 180, 1, "rot", p.rot, deg, QObject::tr("The braid turned round its curve (its front outwards from the head at 0°)"), 0);
  check(v, QObject::tr("Core"), S.coreOn, "coreOn", QObject::tr("A tube through the whole braid inside the lobes, filling any gaps between them. Off — only a short collar at a connector end (the lock is stitched to its ring), closed inside the lobes"));
  if (S.coreOn || endKindOfAny(*it)) sl(QObject::tr("Core size"), 0, 1, 0.01, "core", S.core, pct, QObject::tr("The core's thickness, × the braid's"), tw ? 0.35 : 0.5);

  // the procedural topology: rings × points of each lobe (the core's points twice the lobes')
  v->addWidget(h4(QObject::tr("Topology")));
  sl(QObject::tr("Around a lobe"), 3, 32, 1, "around", S.around, whole, QObject::tr("Points round each lobe (the core gets twice as many)"));
  sl(QObject::tr("Along a lobe"), 1, 32, 1, "along", S.along, whole, QObject::tr("Rings along each lobe"));
  if (auto SB = braid::buildSegments(*sc, vp->hair().curves, *it, vp->camera().sceneCenter)) {
    auto* l = muted(QObject::tr("%1 lobes · %2 polygons").arg(SB->lobes).arg(SB->mesh.faceCount())); l->setToolTip(QObject::tr("A polygroup per strand and one for the core")); v->addWidget(l); }

  // the shape along it: start / middle / end, rounded between them
  v->addWidget(h4(QObject::tr("Shape along (start · middle · end)")));
  const QString at[3] = {QObject::tr("start"), QObject::tr("middle"), QObject::tr("end")};
  auto keys = [&](const QString& what, const char* key, const double* val, double lo, double hi, const QString& tip) {
    v->addWidget(muted(what));
    for (int k = 0; k < 3; k++) slider(v, QObject::tr("  at the %1").arg(at[k]), lo, hi, 0.01, QString("%1%2").arg(key).arg(k), val[k], times, tip, false, 1); };
  keys(QObject::tr("Width"), "w", p.wk, 0.1, 3, QObject::tr("How wide the braid is there (× its width) — double-click: ×1"));
  keys(QObject::tr("Density"), "d", p.dk, 0.2, 4, QObject::tr("How close the segments are there (the count stays) — double-click: ×1"));
  keys(QObject::tr("Lobes' thickness"), "t", p.tk, 0.05, 3, QObject::tr("How thick the lobes are there — double-click: ×1"));
  sl(QObject::tr("Rounding"), 0, 1, 0.01, "round", p.round, pct, QObject::tr("Between the start, middle and end: 0 — straight changes, 100% — rounded"));

  // the ends: a connector (a ring — the lock attached to it is stitched there on Convert) or a point
  v->addWidget(h4(QObject::tr("Ends")));
  bool anyConn = false;
  for (int e = 0; e < 2; e++) {
    const QString kind = braid::endKindOf(*it, e); anyConn = anyConn || kind == "shared";
    v->addWidget(muted(e ? QObject::tr("End") : QObject::tr("Start")));
    choices(v, {{"shared", QObject::tr("Connector")}, {"point", QObject::tr("Point")}}, kind, e ? "end1" : "end0",
            QObject::tr("Connector — the lobes go into the core and the core opens onto a ring: drag a lock curve's end onto it, the lock is stitched there on Convert; Point — the braid comes to a point"));
    if (kind == "point") slider(v, QObject::tr("Tip rounding"), 0, 1, 0.01, e ? "tip1" : "tip0", e ? p.tip1 : p.tip0, pct, QObject::tr("0 — a sharp point, 100% — a round end"), false, -1e300);
  }
  if (anyConn) sl(QObject::tr("Tie"), 0.1, 1.5, 0.01, "tie", S.tie, pct, QObject::tr("The connector's ring, × the braid's half-width (as an elastic band round it)"), 0.55);
  sl(QObject::tr("Gathering"), 0.01, 0.4, 0.005, "conv", p.conv, pct, QObject::tr("The share of the length over which the lobes go into the core (a connector) or come to the point"));
  if (anyConn) { auto* rc = button(QObject::tr("Reset connectors"), QObject::tr("The connectors made anew on the ends' own outline, back in place (attached locks stay on them)"));
    QObject::connect(rc, &QPushButton::clicked, vp, [vp, id] { const Vec3 cen = vp->camera().sceneCenter;
      vp->runEdit([id, cen](Scene& s, Selection&, const CurveCache& cc) { braid::updateConnectors(s, cc, id, cen, true); return QString(); }, true); vp->liveTubes(); });
    v->addWidget(rc); }

  // the extra modifiers along it, painted with the Modifier tool's brushes
  v->addWidget(h4(QObject::tr("Modifier brushes")));
  { auto* w = new QWidget; auto* g = new QHBoxLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setSpacing(3);
    for (const char* kk : {"thick", "twist", "mask", "noise"}) { const QString k = kk; int have = 0; for (int x : tube::extrasOf(*sc, id)) if (sc->item(x)->raw.value("kind").toString() == k) have++;
      auto* b = button(tube::xkindIcon(k) + (have ? QString(" %1").arg(have) : QString()), (have ? QObject::tr("%1 — another one on this braid").arg(tube::xkindName(k)) : QObject::tr("%1 — add to this braid").arg(tube::xkindName(k))) + (tube::painted(k) ? QObject::tr(", then paint it along the braid") : QString()));
      QObject::connect(b, &QPushButton::clicked, vp, [vp, id, k] {
        vp->runEdit([id, k](Scene& s, Selection& sel, const CurveCache& cc) { sel.items = {id}; sel.anchors.clear(); return tube::addExtra(s, cc, sel, k); }, true);
        if (tube::painted(k)) brushFor(vp, k); else emit vp->brushChanged(); });
      g->addWidget(b, 1); }
    v->addWidget(w); }

  // the mesh
  v->addWidget(h4(QObject::tr("Mesh")));
  check(v, QObject::tr("Close the connector ends"), p.caps, "caps", QObject::tr("An n-gon over a connector end's ring when no lock is stitched to it"));
  auto* mk = button(QObject::tr("Make mesh"), QObject::tr("This braid's mesh into the scene now (an imported mesh beside it) — Convert to geometry stitches the attached locks too"), true);
  QObject::connect(mk, &QPushButton::clicked, vp, [vp, id] { const Vec3 cen = vp->camera().sceneCenter; vp->runEdit([id, cen](Scene& s, Selection& sl, const CurveCache& cc) { return braid::makeMesh(s, cc, sl, id, cen); }, true); });
  v->addWidget(mk);
  auto* scb = button(QObject::tr("Select curve")); QObject::connect(scb, &QPushButton::clicked, vp, [vp, src, c] { if (c) vp->setSelectedItems({src}); });
  auto* sel = button(QObject::tr("Select connectors"));
  QObject::connect(sel, &QPushButton::clicked, vp, [vp, sc, id] { std::vector<int> l; for (const QJsonValue& x : sc->item(id)->raw.value("conns").toArray()) if (sc->item(x.toInt())) l.push_back(x.toInt()); if (!l.empty()) vp->setSelectedItems(l); });
  auto* bar = new QWidget; auto* bl = new QHBoxLayout(bar); bl->setContentsMargins(0, 4, 0, 0); bl->setSpacing(4); bl->addWidget(scb); bl->addWidget(sel); v->addWidget(bar);
}

QWidget* buildBraidPanel(Viewport* vp, Scene* sc) {
  auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(4);
  v->addWidget(h4(QObject::tr("Braid")));
  int n = 0; for (int id : vp->selection().items) { const Item* it = sc->item(id); if (it && it->type == ItemType::Curve && it->ring < 0 && it->conn.isEmpty() && braid::braidOfCurve(*sc, id) < 0) n++; }
  const QString nm = QSettings().value("braid/newMode", "weave").toString();
  { auto* w = new QWidget; auto* g = new QHBoxLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setSpacing(3);
    for (auto [k, label, tip] : {std::tuple<QString, QString, QString>{"weave", QObject::tr("Woven"), QObject::tr("A new braid: strands woven as separate tubes")},
                                 {"segments", QObject::tr("Segments"), QObject::tr("A new braid: segments — lobes in a row round a core, its connector ends stitched to the attached locks on Convert")}}) {
      auto* b = button(label, tip); b->setCheckable(true); b->setChecked(nm == k);
      QObject::connect(b, &QPushButton::clicked, vp, [vp, k = k] { QSettings().setValue("braid/newMode", k); emit vp->brushChanged(); }); g->addWidget(b, 1); }
    v->addWidget(w); }
  auto* cb = button(n > 1 ? QObject::tr("Create braids on %1 curves").arg(n) : QObject::tr("Create braid"), nm == "segments" ? QObject::tr("A braid of segments along the selected curve(s)") : QObject::tr("Strands woven along the selected curve(s) as separate tubes"), true);
  cb->setEnabled(n > 0);
  QObject::connect(cb, &QPushButton::clicked, vp, [vp, nm] { const Vec3 cen = vp->camera().sceneCenter; vp->runEdit([cen, nm](Scene& s, Selection& sl, const CurveCache& cc) { return braid::createBraids(s, cc, sl, cen, nm); }, true); });
  v->addWidget(cb);
  const int b = braidOfSel(vp, sc);
  if (b >= 0) { v->addWidget(h4(sc->item(b)->name)); braidProps(v, vp, sc, b); }
  else v->addWidget(muted(QObject::tr("Draw a curve (C) along the braid's middle, select it, press Create braid. Select a braid (or its curve) — its settings show here.")));
  v->addStretch(1);
  return w;
}

}  // namespace hm
