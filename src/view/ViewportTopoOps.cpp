// The Topology room's selection edits and walks: Detach (the selected polygons a mesh of their own), Split (no longer
// welded to the rest), into a new polygroup, Melt (the edges between polygons gone — the polygons one), Mirror (a
// mirrored copy in the same polygroup, welded at the middle when asked), Grow / Shrink, the selection mirrored, the
// selection carried into another mode, H (the selected polygons hidden) / Unhide all; Fill's region and Outline's open
// outline. Each edit one undo step.
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/SceneGeometry.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QJsonArray>
#include <QMessageBox>
#include <QSettings>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
int xformOf(const Scene& sc, int m) { for (int x : meshmods::modsOf(sc, m)) if (sc.item(x)->raw.value("kind").toString() == "xform") return x; return -1; }
Vec3 xformCenter(const Item* x, const PolyMesh& B) {
  QJsonObject p = x ? x->raw.value("p").toObject() : QJsonObject();
  return p.value("cx").isDouble() ? Vec3(p.value("cx").toDouble(), p.value("cy").toDouble(), p.value("cz").toDouble()) : B.bounds().center();
}
// the polygroups with no polygon left dropped — only those in `had` (they had some before; nullptr — every empty one):
// → old → new numbering (−1 gone)
std::vector<int> dropEmpty(PolyMesh& d, const std::vector<int>* had) {
  const int G = int(d.groupNames.size()); std::vector<int> cnt(size_t(G), 0); for (int g : d.faceGroup) if (g >= 0 && g < G) cnt[size_t(g)]++;
  std::vector<int> map(static_cast<size_t>(G)); std::iota(map.begin(), map.end(), 0);
  for (int g = G - 1; g >= 0; g--) if (!cnt[size_t(g)] && (!had || (size_t(g) < had->size() && (*had)[size_t(g)]))) {
    if (G - (G - 1 - g) <= 1) break;                              // (one group always stays)
    topo::dropGroup(d, g); map[size_t(g)] = -1; for (int k = g + 1; k < G; k++) if (map[size_t(k)] >= 0) map[size_t(k)]--; }
  return map;
}
std::vector<int> groupCounts(const PolyMesh& d) { std::vector<int> c(d.groupNames.size(), 0); for (int g : d.faceGroup) if (g >= 0 && size_t(g) < c.size()) c[size_t(g)]++; return c; }
QString plural(int n, const QString& one, const QString& many) { return n == 1 ? one : many; }
// Melt's leftovers: of the vertices `cand`, those left in the middle of a side (two edges, off the rim) taken out of their
// polygons, those no polygon uses any more dropped → how many went
int dissolveVerts(PolyMesh& d, const std::set<int>& cand, const std::set<int>* rimOk = nullptr) {
  auto sp = std::make_shared<PolyMesh>(d); auto T = topo::ttopo(sp);
  std::vector<char> drop(d.pos.size(), 0); int n = 0;
  for (int v : cand) { if (v < 0 || v >= T->n) continue; const int s = T->voff[size_t(v)], t = T->voff[size_t(v) + 1];
    if (t - s == 0) { n++; continue; }                            // (no polygon uses it: the subset drops it)
    if (t - s != 2) continue; bool rim = false; for (int j = s; j < t; j++) if (T->ef1[size_t(T->vE[size_t(j)])] < 0) rim = true;
    if (!rim || (rimOk && rimOk->count(v))) { drop[size_t(v)] = 1; n++; } }
  if (!n) return 0;
  const bool uv = d.cornerUV.size() == d.faceVerts.size() && !d.cornerUV.empty(), nrm = d.cornerNormal.size() == d.faceVerts.size() && !d.cornerNormal.empty();
  std::vector<int> fs{0}, fv; std::vector<std::array<float, 2>> cu; decltype(d.cornerNormal) cn;
  for (int f = 0; f < d.faceCount(); f++) {
    const int s = d.faceStart[size_t(f)], e = d.faceStart[size_t(f) + 1]; int left = 0; for (int k = s; k < e; k++) if (!drop[size_t(d.faceVerts[size_t(k)])]) left++;
    for (int k = s; k < e; k++) { if (left >= 3 && drop[size_t(d.faceVerts[size_t(k)])]) continue;   // (a polygon kept whole if it would fall under three)
      fv.push_back(d.faceVerts[size_t(k)]); if (uv) cu.push_back(d.cornerUV[size_t(k)]); if (nrm) cn.push_back(d.cornerNormal[size_t(k)]); }
    fs.push_back(int(fv.size()));
  }
  d.faceStart = fs; d.faceVerts = fv; if (uv) d.cornerUV = cu; if (nrm) d.cornerNormal = cn;
  d = topo::subset(d, std::vector<char>(static_cast<size_t>(d.faceCount()), 1));   // (the vertices no polygon uses dropped)
  return n;
}
}  // namespace

#define TOPO_DONE(msg) do { scene_->revision++; commit(true); emit topoState(); ui::toast(msg); return msg; } while (0)
#define TOPO_NO(msg) do { ui::toast(msg); return "!" + QString(msg); } while (0)

void Viewport::setSelTool(const QString& t) {
  tset.selTool = t; saveTopoPrefs();
  if (tool_ != "cursor") setTool("cursor");
  thov_ = {}; syncCursor(); topoRev_++; emit topoState(); requestUpdate();
  const char* names[] = {"cursor", "Select Cursor — a drag paints the selection", "rect", "Rectangle — a drag selects in a box", "lasso", "Lasso — a drag selects inside the outline drawn",
                         "fill", "Fill — a click picks the region under the pointer (up to a cut or the selection's frame); Ctrl — the selected piece taken off",
                         "outline", "Outline — a click picks the open outline (or the picked polygons' border)",
                         "loop", "Loop — the loop under the pointer lit yellow, a click picks it", "ring", "Ring — the parallel ring under the pointer lit yellow, a click picks it"};
  for (int i = 0; i < 14; i += 2) if (t == names[i]) ui::hud(tr(names[i + 1]), 1400);
}

// what a click can pick: every shown mesh in the Object mode, only the active one in a component mode
std::vector<int> Viewport::tPickable() const {
  if (topoMode == "object") return topoMeshes();
  const int m = topoMesh(); return m >= 0 && !topoHeavy(m) ? std::vector<int>{m} : std::vector<int>{};
}

std::vector<int> Viewport::tSelFaces(int m) const {
  if (tsel_.id != m) return {};
  if (topoMode == "object" || tKey() == 'f') return {tsel_.f.begin(), tsel_.f.end()};
  const Item* it = scene_->item(m); if (!it) return {};
  auto T = topo::ttopo(baseSp(*it)); auto vl = tSelVerts(m); std::set<int> vs(vl.begin(), vl.end());
  std::vector<int> fs; for (int f = 0; f < T->nf; f++) { bool all = true; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1] && all; k++) all = vs.count(T->fv[size_t(k)]) > 0; if (all) fs.push_back(f); }
  return fs;
}

// ---------- Fill, Outline ----------
std::vector<int> Viewport::fillRegion(int m, int f0, bool inv) const {
  const Item* it = scene_->item(m); if (!it || f0 < 0) return {};
  auto B = baseSp(*it); auto T = topo::ttopo(B); if (f0 >= T->nf) return {};
  const std::vector<char> hid = meshmods::hiddenFaces(*it, *B);
  if (inv) {                                                    // (Ctrl: the selected piece under the pointer — joined through the picked polygons)
    std::vector<char> in(size_t(T->nf), 0); for (int f : tSelFaces(m)) if (f < T->nf) in[size_t(f)] = 1;
    if (!in[size_t(f0)]) return {};
    std::vector<char> seen(size_t(T->nf), 0); std::vector<int> out, st{f0}; seen[size_t(f0)] = 1;
    while (!st.empty()) { int f = st.back(); st.pop_back(); out.push_back(f);
      for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { int e = T->ce[size_t(k)], g = T->ef0[size_t(e)] == f ? T->ef1[size_t(e)] : T->ef0[size_t(e)];
        if (g >= 0 && !seen[size_t(g)] && in[size_t(g)]) { seen[size_t(g)] = 1; st.push_back(g); } } }
    return out;
  }
  std::vector<char> blockF(size_t(T->nf), 0), blockE(size_t(T->ne), 0);
  if (tsel_.id == m && topoMode != "object") {                   // (the selection's frame: picked polygons, edges, edges between picked vertices)
    const char key = tKey();
    if (key == 'f') for (int f : tsel_.f) { if (f < T->nf) blockF[size_t(f)] = 1; }
    else if (key == 'e') for (int e : tsel_.e) { if (e < T->ne) blockE[size_t(e)] = 1; }
    else for (int e = 0; e < T->ne; e++) if (tsel_.v.count(T->ea[size_t(e)]) && tsel_.v.count(T->eb[size_t(e)])) blockE[size_t(e)] = 1;
  }
  if (blockF[size_t(f0)] || (!hid.empty() && hid[size_t(f0)])) return {};
  std::vector<char> seen(size_t(T->nf), 0); std::vector<int> out, st{f0}; seen[size_t(f0)] = 1;
  while (!st.empty()) {
    int f = st.back(); st.pop_back(); out.push_back(f);
    for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { int e = T->ce[size_t(k)]; if (blockE[size_t(e)]) continue;
      int g = T->ef0[size_t(e)] == f ? T->ef1[size_t(e)] : T->ef0[size_t(e)];
      if (g < 0 || seen[size_t(g)] || blockF[size_t(g)] || (!hid.empty() && hid[size_t(g)])) continue; seen[size_t(g)] = 1; st.push_back(g); }
  }
  return out;
}
std::vector<int> Viewport::rimLoop(int m, int e0) const {
  const Item* it = scene_->item(m); if (!it || e0 < 0) return {};
  auto T = topo::ttopo(baseSp(*it)); if (e0 >= T->ne || T->ef1[size_t(e0)] >= 0) return {};
  std::vector<char> seen(size_t(T->ne), 0); std::vector<int> out, st{e0}; seen[size_t(e0)] = 1;
  while (!st.empty()) {
    int e = st.back(); st.pop_back(); out.push_back(e);
    for (int v : {T->ea[size_t(e)], T->eb[size_t(e)]}) for (int i = T->voff[size_t(v)]; i < T->voff[size_t(v) + 1]; i++) { int x = T->vE[size_t(i)];
      if (T->ef1[size_t(x)] < 0 && !seen[size_t(x)]) { seen[size_t(x)] = 1; st.push_back(x); } }
  }
  return out;
}
std::vector<int> Viewport::rimAll(int m) const {
  const Item* it = scene_->item(m); if (!it) return {}; auto T = topo::ttopo(baseSp(*it)); std::vector<int> out;
  for (int e = 0; e < T->ne; e++) if (T->ef1[size_t(e)] < 0) out.push_back(e);
  return out;
}
int Viewport::rimEdgeNear(int m, const QPointF& mp, double px) const {
  const Item* it = scene_->item(m); if (!it) return -1;
  auto T = topo::ttopo(baseSp(*it)); const auto W = ttWorld(m); Projector P = projector(); int best = -1; double bd = px;
  for (int e = 0; e < T->ne; e++) { if (T->ef1[size_t(e)] >= 0) continue;
    const ScreenPt a = P.toScreen(W[size_t(T->ea[size_t(e)])]), b = P.toScreen(W[size_t(T->eb[size_t(e)])]); if (a.z >= 1 || b.z >= 1) continue;
    const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? std::clamp(((mp.x() - a.x) * dx + (mp.y() - a.y) * dy) / l2, 0.0, 1.0) : 0;
    const double d = std::hypot(a.x + dx * t - mp.x(), a.y + dy * t - mp.y()); if (d < bd) { bd = d; best = e; } }
  return best;
}
bool Viewport::topoClickTool(const QPointF& mp, bool shift, bool ctrl) {
  if (tset.selTool == "hard") {                                  // Hard edges: the lit line picked (Shift — added, Ctrl — taken off)
    int am = topoMesh(); if (am < 0) if (auto r = tPick(mp, topoMeshes())) am = r->m; if (am < 0 || topoHeavy(am)) return true;
    const int e = hardEdgeNear(am, mp); if (e < 0) { ui::hud(tr("Point at a hard edge — its line is picked"), 1000); return true; }
    if (sel_.items.size() != 1 || sel_.items[0] != am) { sel_ = {}; sel_.items = {am}; onSelChange(); }
    if (topoMode != "edge") setTopoMode("edge", true);
    const auto L = hardLine(am, e); tselApply(am, L, ctrl ? "remove" : shift ? "add" : "set", 'e');
    ui::hud(tr("Hard edges: %1").arg(L.size()), 1000); thov_ = {}; return true;
  }
  auto r = tPick(mp, tPickable());
  if (!r && tset.selTool == "outline") { const int am = topoMesh(); if (int e = am >= 0 ? rimEdgeNear(am, mp) : -1; e >= 0) { TopoPick p; p.m = am; p.e = e; r = p; } }   // (Outline just off the border: its nearest edge)
  if (!r) return true;
  const int m = r->m; if (topoHeavy(m)) { ui::toast(tr("A high-poly mesh — Object mode only")); return true; }
  if (sel_.items.size() != 1 || sel_.items[0] != m) { sel_ = {}; sel_.items = {m}; onSelChange(); }
  const QString op = ctrl ? "remove" : shift ? "add" : "set";
  if (tset.selTool == "loop" || tset.selTool == "ring") {      // Loop / Ring: what was lit yellow
    if (topoMode == "object") setTopoMode("edge", true);
    auto L = tLoopOf(m, *r, mp, tset.selTool == "ring"); if (L.empty()) return true;
    tselApply(m, L, op); ui::hud(tr("%1: %2").arg(tset.selTool == "ring" ? tr("Ring") : tr("Loop")).arg(L.size()), 1000);
    thov_ = {}; return true;
  }
  if (tset.selTool == "fill" && ctrl) {                         // Fill + Ctrl: the selected piece under the pointer taken off
    auto reg = fillRegion(m, r->f, true); if (reg.empty()) { ui::hud(tr("Ctrl: click on the selection — its piece is taken off"), 1200); return true; }
    auto T = topo::ttopo(baseSp(*scene_->item(m))); const char key = tKey(); std::set<int> out;
    if (key == 'f') out.insert(reg.begin(), reg.end());
    else for (int f : reg) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) out.insert(key == 'e' ? T->ce[size_t(k)] : T->fv[size_t(k)]);
    tselApply(m, {out.begin(), out.end()}, "remove"); ui::hud(tr("Taken off: %1 polygon%2").arg(reg.size()).arg(reg.size() == 1 ? "" : "s"), 1200);
    thov_ = {}; return true;
  }
  if (tset.selTool == "fill") {
    auto reg = fillRegion(m, r->f); if (reg.empty()) { ui::hud(tr("On the frame itself — click inside or outside it (Ctrl — the selected piece taken off)"), 1200); return true; }
    if (topoMode != "poly") setTopoMode("poly", true);              // (an edge / vertex frame: the region as polygons)
    tselApply(m, reg, op, 'f'); ui::hud(tr("Region: %1 polygon%2").arg(reg.size()).arg(reg.size() == 1 ? "" : "s"), 1200);
    thov_ = {}; return true;
  }
  std::vector<int> es;
  if (tsel_.id == m && !tsel_.f.empty() && topoMode != "edge") { auto T = topo::ttopo(baseSp(*scene_->item(m)));   // (picked polygons: their border)
    for (int e = 0; e < T->ne; e++) { bool a = tsel_.f.count(T->ef0[size_t(e)]) > 0, b = T->ef1[size_t(e)] >= 0 && tsel_.f.count(T->ef1[size_t(e)]) > 0; if (a != b) es.push_back(e); } }
  else es = rimAll(m);                                           // (over the mesh: every border of it — a hole left open far away too)
  if (es.empty()) { ui::toast(tr("This mesh has no open border — no holes, no open edges")); return true; }
  if (topoMode != "edge") setTopoMode("edge", true);
  tselApply(m, es, op, 'e'); ui::hud(tr("Outline: %1 edge%2").arg(es.size()).arg(es.size() == 1 ? "" : "s"), 1200);
  thov_ = {}; return true;
}

// the Select Cursor's drag: the elements (meshes in the Object mode) under the brush's circle picked / taken off
void Viewport::tPaintAt(const QPointF& mp, const QString& mode) {
  const double R = 14;
  if (topoMode == "object") {
    auto r = tPick(mp, topoMeshes()); if (!r) return;
    const bool has = sel_.hasItem(r->m);
    if (mode == "remove") { if (has) { sel_.items.erase(std::remove(sel_.items.begin(), sel_.items.end(), r->m), sel_.items.end()); onSelChange(); } return; }
    if (mode == "set") { sel_ = {}; sel_.items = {r->m}; td_.mode = "add"; onSelChange(); return; }
    if (!has) { sel_.items.push_back(r->m); onSelChange(); }
    return;
  }
  const int m = topoMesh(); if (m < 0) return;
  auto list = tAreaPick(m, mp.x() - R, mp.y() - R, mp.x() + R, mp.y() + R, tKey(), [mp, R](double x, double y) { return std::hypot(x - mp.x(), y - mp.y()) <= R; });
  if (auto r = tPick(mp, {m})) { const char key = tKey(); const int el = key == 'f' ? r->f : key == 'e' ? r->e : r->v;   // (the element under the pointer always: a big polygon's middle may lie outside the circle)
    if (el >= 0 && std::find(list.begin(), list.end(), el) == list.end()) list.push_back(el); }
  if (mode == "set") { tselApply(m, list, "set"); td_.mode = "add"; return; }   // (the first dab replaces, the rest add)
  if (!list.empty()) tselApply(m, list, mode);
}

// ---------- the edits ----------
QString Viewport::tDetachSel() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) TOPO_NO(tr("Select a mesh"));
  if (topoHeavy(m)) TOPO_NO(tr("A high-poly mesh — Object mode only"));
  auto fs = tSelFaces(m); const PolyMesh d = *baseOf(*it);
  if (fs.empty()) TOPO_NO(tr("Select polygons first (or vertices / edges closing them)"));
  if (int(fs.size()) == d.faceCount()) TOPO_NO(tr("That is the whole mesh"));
  std::vector<char> take(static_cast<size_t>(d.faceCount()), 0), keep(static_cast<size_t>(d.faceCount()), 1); for (int f : fs) { take[size_t(f)] = 1; keep[size_t(f)] = 0; }
  const std::vector<int> had = groupCounts(d);
  PolyMesh part = topo::subset(d, take); dropEmpty(part, nullptr);
  PolyMesh rest = topo::subset(d, keep); auto map = dropEmpty(rest, &had);
  const int x = xformOf(*scene_, m);
  int nid = topoNewMesh(it->parent, it->name + tr(" · part"), part, m);
  it = scene_->item(m); Item* ni = scene_->item(nid);
  for (const char* k : {"look", "smoothN", "twire", "pivot", "mat", "matPreset", "matOff", "unlit", "pgView"}) if (it->raw.contains(k)) ni->raw.insert(k, it->raw.value(k));   // (look: its textures)
  ni->headTag = it->headTag; if (it->headTag) ni->raw.insert("headTag", true);
  if (it->raw.contains("selSets")) { const QJsonArray ss = topo::selSetsFollow(it->raw.value("selSets").toArray(), baseSp(*it), baseSp(*ni));   // (the selection sets: the part takes what of them it holds)
    if (!ss.isEmpty()) ni->raw.insert("selSets", ss); }
  if (x >= 0) { const Item* xi = scene_->item(x); Vec3 c = xformCenter(xi, d); QJsonObject p = xi->raw.value("p").toObject(); bool vis = xi->visible;
    int x2 = meshmods::newMeshExtra(*scene_, nid, "xform", nullptr);
    if (x2 >= 0) { p.insert("cx", c.x); p.insert("cy", c.y); p.insert("cz", c.z); scene_->item(x2)->raw.insert("p", p); scene_->item(x2)->visible = vis; } }
  topo::groupsFollow(scene_->item(m)->raw, [map](int g) { return g >= 0 && size_t(g) < map.size() ? map[size_t(g)] : g; });
  topoWrite(m, std::make_shared<PolyMesh>(std::move(rest))); tgSel = {}; tselClear();
  emit sceneEdited();
  TOPO_DONE(tr("%1 polygon%2 → a separate mesh: %3").arg(fs.size()).arg(fs.size() == 1 ? "" : "s").arg(scene_->item(nid)->name));
}

QString Viewport::tSplitSel() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) TOPO_NO(tr("Select a mesh"));
  if (topoHeavy(m)) TOPO_NO(tr("A high-poly mesh — Object mode only"));
  auto fs = tSelFaces(m); if (fs.empty()) TOPO_NO(tr("Select polygons first (or vertices / edges closing them)"));
  PolyMesh d = *baseOf(*it); auto T = topo::ttopo(baseSp(*it));
  std::vector<char> in(static_cast<size_t>(d.faceCount()), 0); for (int f : fs) in[size_t(f)] = 1;
  // the vertices the selection shares with the rest: a copy of each for the selected polygons
  std::vector<char> useIn(d.pos.size(), 0), useOut(d.pos.size(), 0);
  for (int f = 0; f < d.faceCount(); f++) for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) (in[size_t(f)] ? useIn : useOut)[size_t(d.faceVerts[size_t(k)])] = 1;
  std::vector<int> copy(d.pos.size(), -1); int n = 0;
  for (size_t v = 0; v < d.pos.size(); v++) if (useIn[v] && useOut[v]) { copy[v] = int(d.pos.size()) + n++; }
  if (!n) TOPO_NO(tr("They are not welded to the rest already"));
  for (size_t v = 0; v < copy.size(); v++) if (copy[v] >= 0) d.pos.push_back(d.pos[v]);
  for (int f : fs) for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) { int v = d.faceVerts[size_t(k)]; if (copy[size_t(v)] >= 0) d.faceVerts[size_t(k)] = copy[size_t(v)]; }
  topoWrite(m, std::make_shared<PolyMesh>(std::move(d)));
  tsel_.B = baseOf(*scene_->item(m));                              // (the polygons kept their numbers: the selection stays)
  TOPO_DONE(tr("Split off: %1 vert%2 doubled along the cut").arg(n).arg(n == 1 ? "ex" : "ices"));
}

// Hard / Soft edges: the shading broken along them (or smooth across again); the file's own normals (if it has them)
// worked out anew so the export shades the same
QString Viewport::tHardEdges(bool hard) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it || tsel_.id != m || topoMode == "object") TOPO_NO(tr("Select edges, polygons or vertices first"));
  auto T = topo::ttopo(baseSp(*it)); const char key = tKey(); std::set<int> es;
  if (key == 'e') es = tsel_.e;
  else if (key == 'f') { for (int f : tsel_.f) if (f < T->nf) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) es.insert(T->ce[size_t(k)]); }
  else for (int e = 0; e < T->ne; e++) if (tsel_.v.count(T->ea[size_t(e)]) && tsel_.v.count(T->eb[size_t(e)])) es.insert(e);
  if (es.empty()) TOPO_NO(tr("No edges in the selection"));
  PolyMesh d = *baseOf(*it);
  auto k2 = [](int a, int b) { return std::pair<int, int>(std::min(a, b), std::max(a, b)); };
  std::set<std::pair<int, int>> H; for (auto& h : d.hardEdges) H.insert(k2(h[0], h[1]));
  const size_t before = H.size();
  for (int e : es) { auto p = k2(T->ea[size_t(e)], T->eb[size_t(e)]); if (hard) H.insert(p); else H.erase(p); }
  if (H.size() == before && hard) TOPO_NO(tr("These edges are hard already"));
  if (H.size() == before && !hard) TOPO_NO(tr("These edges are soft already"));
  d.hardEdges.clear(); for (auto& [a, b] : H) d.hardEdges.push_back({a, b});
  // (custom normals: only round the changed edges' vertices — the rest of the shading kept. Soft: the corners now in one
  // smooth fan take their custom normals' average; hard: the corners on each side the standard ones of that side)
  if (d.cornerNormal.size() == d.faceVerts.size() && !d.cornerNormal.empty()) {
    std::set<int> vs; for (int e : es) { vs.insert(T->ea[size_t(e)]); vs.insert(T->eb[size_t(e)]); }
    const auto cn = hardCornerNormals(d);
    std::map<int, std::vector<size_t>> at; for (size_t k = 0; k < d.faceVerts.size(); k++) if (vs.count(d.faceVerts[k])) at[d.faceVerts[k]].push_back(k);
    for (auto& [v, ks] : at) {
      std::vector<std::vector<size_t>> fans;                       // (the corners sharing a standard normal: one smooth fan)
      for (size_t k : ks) { bool put = false; for (auto& F : fans) if ((cn[F[0]] - cn[k]).lengthSq() < 1e-10) { F.push_back(k); put = true; break; } if (!put) fans.push_back({k}); }
      for (auto& F : fans) { Vec3 n;
        if (hard) n = cn[F[0]];
        else { for (size_t k : F) n += Vec3(d.cornerNormal[k][0], d.cornerNormal[k][1], d.cornerNormal[k][2]); n = n.lengthSq() > 1e-12 ? n.normalized() : cn[F[0]]; }
        for (size_t k : F) d.cornerNormal[k] = {float(n.x), float(n.y), float(n.z)}; } } }
  auto nb = std::make_shared<PolyMesh>(std::move(d)); topo::shareTopo(nb, baseSp(*it)); topoWrite(m, nb); tsel_.B = nb.get();
  if (!it->raw.value("smoothN").toBool()) ui::hud(tr("Seen with Smooth shading (the mesh's Display)"), 1600);
  TOPO_DONE(hard ? tr("%1 edge%2 hard").arg(es.size()).arg(es.size() == 1 ? "" : "s") : tr("%1 edge%2 soft").arg(es.size()).arg(es.size() == 1 ? "" : "s"));
}

QString Viewport::tSplitToGroup() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) TOPO_NO(tr("Select a mesh"));
  auto fs = tSelFaces(m); if (fs.empty()) TOPO_NO(tr("Select polygons first (or vertices / edges closing them)"));
  PolyMesh d = *baseOf(*it); if (int(d.faceGroup.size()) != d.faceCount()) d.faceGroup.assign(size_t(d.faceCount()), 0);
  const std::vector<int> had = groupCounts(d);
  const int g0 = d.faceGroup[size_t(fs[0])]; QString base = (size_t(g0) < d.groupNames.size() ? d.groupNames[size_t(g0)] : QString("Polygroup")) + " part", name = base;
  for (int k = 2; std::find(d.groupNames.begin(), d.groupNames.end(), name) != d.groupNames.end(); k++) name = base + " " + QString::number(k);
  const int G = int(d.groupNames.size()); d.groupNames.push_back(name); for (int f : fs) d.faceGroup[size_t(f)] = G;
  auto map = dropEmpty(d, &had);
  topo::groupsFollow(it->raw, [map](int g) { return g >= 0 && size_t(g) < map.size() ? map[size_t(g)] : g; });
  const int G2 = map.size() > size_t(G) ? map[size_t(G)] : int(d.groupNames.size()) - 1;
  auto nb = std::make_shared<PolyMesh>(std::move(d)); topo::shareTopo(nb, baseSp(*it)); topoWrite(m, nb); tgSel = {m, G2 >= 0 ? G2 : int(nb->groupNames.size()) - 1};
  TOPO_DONE(tr("%1 polygon%2 → a new polygroup: %3").arg(fs.size()).arg(fs.size() == 1 ? "" : "s").arg(name));
}

QString Viewport::tMelt() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || tsel_.id != m) TOPO_NO(tr("Select edges (or polygons: the edges between them) first"));
  if (topoHeavy(m)) TOPO_NO(tr("A high-poly mesh — Object mode only"));
  const PolyMesh& B = *baseOf(*it); auto T = topo::ttopo(baseSp(*it)); const char key = tKey();
  std::vector<char> melt(size_t(T->ne), 0); int asked = 0;
  std::set<int> midV;                                            // (vertices: those with two edges — only taken out of their sides)
  if (key == 'v') for (int v : tsel_.v) if (v < T->n && T->voff[size_t(v) + 1] - T->voff[size_t(v)] == 2) midV.insert(v);
  for (int e = 0; e < T->ne; e++) {
    const int a = T->ef0[size_t(e)], b = T->ef1[size_t(e)]; if (b < 0 || a == b) continue;
    // (vertices: every edge of a picked vertex — the polygons round it one, the vertex gone)
    bool on = key == 'e' ? tsel_.e.count(e) > 0 : key == 'f' ? (tsel_.f.count(a) && tsel_.f.count(b))
            : ((tsel_.v.count(T->ea[size_t(e)]) && !midV.count(T->ea[size_t(e)])) || (tsel_.v.count(T->eb[size_t(e)]) && !midV.count(T->eb[size_t(e)])));
    if (on) { melt[size_t(e)] = 1; asked++; }
  }
  if (!asked && midV.empty()) TOPO_NO(key == 'v' ? tr("Nothing to melt — pick vertices inside the mesh") : tr("Nothing to melt — pick inner edges (or polygons side by side)"));
  // the polygons joined across the melted edges
  std::vector<int> grp(size_t(T->nf)); std::iota(grp.begin(), grp.end(), 0);
  std::function<int(int)> find = [&](int x) { while (grp[size_t(x)] != x) x = grp[size_t(x)] = grp[size_t(grp[size_t(x)])]; return x; };
  for (int e = 0; e < T->ne; e++) if (melt[size_t(e)]) { int a = find(T->ef0[size_t(e)]), b = find(T->ef1[size_t(e)]); if (a != b) grp[size_t(std::max(a, b))] = std::min(a, b); }
  std::map<int, std::vector<int>> groups; for (int f = 0; f < T->nf; f++) groups[find(f)].push_back(f);
  // each group's outline: its sides not melted inside it, chained into one loop (else the group is left as it was)
  const bool uv = B.cornerUV.size() == B.faceVerts.size() && !B.cornerUV.empty(), nrm = B.cornerNormal.size() == B.faceVerts.size() && !B.cornerNormal.empty();
  std::map<int, std::vector<int>> loops; int skipped = 0;           // (group → its corners in order)
  for (auto& [r, fs] : groups) {
    if (fs.size() < 2) continue;
    std::multimap<int, int> from;                                  // (start vertex → the corner it starts at)
    for (int f : fs) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { int e = T->ce[size_t(k)];
      if (melt[size_t(e)] && find(T->ef0[size_t(e)]) == r && T->ef1[size_t(e)] >= 0 && find(T->ef1[size_t(e)]) == r) continue;
      from.emplace(T->fv[size_t(k)], k); }
    if (from.empty()) { skipped++; continue; }
    std::vector<int> loop; size_t total = from.size(); int k = from.begin()->second; bool ok = true;
    auto nextC = [&](int c) { int f = T->cf[size_t(c)]; return c + 1 < T->fo[size_t(f) + 1] ? c + 1 : T->fo[size_t(f)]; };
    while (true) {
      loop.push_back(k); auto range = from.equal_range(T->fv[size_t(k)]);
      for (auto it2 = range.first; it2 != range.second; ++it2) if (it2->second == k) { from.erase(it2); break; }
      if (from.empty()) break;
      int endV = T->fv[size_t(nextC(k))]; auto nx = from.equal_range(endV);
      if (std::distance(nx.first, nx.second) != 1) { ok = false; break; }   // (a pinch / a hole: not one simple outline)
      k = nx.first->second;
    }
    if (!ok || loop.size() != total || loop.size() < 3) { skipped++; continue; }
    loops[r] = loop;
  }
  if (loops.empty() && midV.empty()) TOPO_NO(tr("Can't melt there — the polygons would make a ring or a hole (not one polygon)"));
  PolyMesh d; d.pos = B.pos; d.groupNames = B.groupNames; d.matNames = B.matNames; d.matMaps = B.matMaps; d.matColor = B.matColor; d.matAlpha = B.matAlpha; d.mtllib = B.mtllib; d.flipY = B.flipY;
  d.hardEdges = B.hardEdges; d.creases = B.creases; d.seams = B.seams;
  const bool fg = int(B.faceGroup.size()) == B.faceCount(), fm = int(B.faceMat.size()) == B.faceCount(); int merged = 0;
  for (int f = 0; f < T->nf; f++) {
    const int r = find(f); auto L = loops.find(r);
    std::vector<int> cs;
    if (L != loops.end()) { if (f != r) continue; cs = L->second; merged += int(groups[r].size()); }   // (the group: once, where its first polygon was)
    else for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) cs.push_back(k);
    std::vector<int> vs; for (int k : cs) vs.push_back(T->fv[size_t(k)]); d.addFace(vs.data(), int(vs.size()));
    if (uv) for (int k : cs) d.cornerUV.push_back(B.cornerUV[size_t(k)]);
    if (nrm) for (int k : cs) d.cornerNormal.push_back(B.cornerNormal[size_t(k)]);
    if (fg) d.faceGroup.push_back(B.faceGroup[size_t(f)]); if (fm) d.faceMat.push_back(B.faceMat[size_t(f)]);
    if (topo::hasUvSets(B)) topo::addFaceSets(d, B, f, cs);
  }
  std::set<int> ends; for (int e = 0; e < T->ne; e++) if (melt[size_t(e)] && loops.count(find(T->ef0[size_t(e)]))) { ends.insert(T->ea[size_t(e)]); ends.insert(T->eb[size_t(e)]); }
  std::set<int> rimOk; if (key == 'v') for (int v : tsel_.v) { ends.insert(v); rimOk.insert(v); }   // (picked vertices: gone even on the rim)
  const int gone = dissolveVerts(d, ends, &rimOk);              // (their vertices too: those left mid-side, those no polygon uses)
  topoWrite(m, std::make_shared<PolyMesh>(std::move(d))); tselClear();
  TOPO_DONE(tr("Melted: %1 polygons → %2").arg(merged).arg(loops.size()) + (gone ? tr(" · %1 vert%2 gone").arg(gone).arg(gone == 1 ? "ex" : "ices") : QString())
            + (skipped ? tr(" · %1 place%2 left (a ring or a hole)").arg(skipped).arg(skipped == 1 ? "" : "s") : QString()));
}

// Del in a component mode: the picked polygons — or the polygons on the picked edges / vertices — deleted, the vertices
// no polygon uses any more dropped (not the whole mesh)
QString Viewport::tDeleteSel() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it || tsel_.id != m || topoMode == "object") TOPO_NO(tr("Select polygons, edges or vertices first"));
  if (topoHeavy(m)) TOPO_NO(tr("A high-poly mesh — Object mode only"));
  const PolyMesh& B = *baseOf(*it); auto T = topo::ttopo(baseSp(*it)); const char key = tKey();
  std::vector<char> keep(size_t(T->nf), 1); int gone = 0;
  for (int f = 0; f < T->nf; f++) {
    bool del = false;
    if (key == 'f') del = tsel_.f.count(f) > 0;
    else for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1] && !del; k++) del = key == 'e' ? tsel_.e.count(T->ce[size_t(k)]) > 0 : tsel_.v.count(T->fv[size_t(k)]) > 0;
    if (del) { keep[size_t(f)] = 0; gone++; }
  }
  if (!gone) TOPO_NO(tr("Nothing selected to delete"));
  if (gone == T->nf) TOPO_NO(tr("That is the whole mesh — delete it in Object mode"));
  const std::vector<int> had = groupCounts(B);
  PolyMesh d = topo::subset(B, keep); auto map = dropEmpty(d, &had);
  topo::groupsFollow(it->raw, [map](int g) { return g >= 0 && size_t(g) < map.size() ? map[size_t(g)] : g; });
  topoWrite(m, std::make_shared<PolyMesh>(std::move(d))); tselClear();
  TOPO_DONE(tr("Deleted: %1 polygon%2").arg(gone).arg(gone == 1 ? "" : "s"));
}

QString Viewport::tMirrorGeo() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) TOPO_NO(tr("Select a mesh"));
  if (topoHeavy(m)) TOPO_NO(tr("A high-poly mesh — Object mode only"));
  auto fs = tSelFaces(m); if (fs.empty()) TOPO_NO(tr("Select polygons first (or vertices / edges closing them)"));
  PolyMesh d = *baseOf(*it); const double eps = std::max(1e-9, d.bounds().size().length() * 1e-4);
  std::set<int> used; for (int f : fs) for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) used.insert(d.faceVerts[size_t(k)]);
  int onPlane = 0; for (int v : used) if (std::abs(d.pos[size_t(v)].x) < eps) onPlane++;
  bool weld = false;
  if (onPlane && mirrorWeldAns >= 0) weld = mirrorWeldAns == 1;
  else if (onPlane) weld = QMessageBox::question(nullptr, tr("Mirror"), tr("The copy meets the original along the middle (X = 0, %1 vert%2). Weld them along that seam?").arg(onPlane).arg(onPlane == 1 ? "ex" : "ices"),
                                            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes;
  std::map<int, int> mv;                                           // (vertex → its mirror)
  for (int v : used) { if (weld && std::abs(d.pos[size_t(v)].x) < eps) { mv[v] = v; continue; } const Vec3& p = d.pos[size_t(v)]; mv[v] = int(d.pos.size()); d.pos.push_back({-p.x, p.y, p.z}); }
  const bool uv = d.cornerUV.size() == d.faceVerts.size() && !d.cornerUV.empty(), nrm = d.cornerNormal.size() == d.faceVerts.size() && !d.cornerNormal.empty();
  const bool fg = int(d.faceGroup.size()) == d.faceCount(), fm = int(d.faceMat.size()) == d.faceCount();
  const int nf0 = d.faceCount(); std::vector<int> made;
  for (int f : fs) {                                               // (turned the other way: a mirror flips the winding)
    const int s = d.faceStart[size_t(f)], e = d.faceStart[size_t(f) + 1]; std::vector<int> cs{s}; for (int k = e - 1; k > s; k--) cs.push_back(k);
    std::vector<int> vs; for (int k : cs) vs.push_back(mv[d.faceVerts[size_t(k)]]);
    std::vector<std::array<float, 2>> uvs; std::vector<std::array<float, 3>> ns; for (int k : cs) { if (uv) uvs.push_back(d.cornerUV[size_t(k)]); if (nrm) { auto n = d.cornerNormal[size_t(k)]; ns.push_back({-n[0], n[1], n[2]}); } }
    const int g = fg ? d.faceGroup[size_t(f)] : 0, mt = fm ? d.faceMat[size_t(f)] : 0;
    d.addFace(vs.data(), int(vs.size())); made.push_back(d.faceCount() - 1);
    d.cornerUV.insert(d.cornerUV.end(), uvs.begin(), uvs.end()); d.cornerNormal.insert(d.cornerNormal.end(), ns.begin(), ns.end());
    if (fg) d.faceGroup.push_back(g); if (fm) d.faceMat.push_back(mt);
  }
  for (auto* L : {&d.hardEdges, &d.seams}) { auto add = *L; for (auto& p : add) if (mv.count(p[0]) && mv.count(p[1])) L->push_back({mv[p[0]], mv[p[1]]}); }
  { auto add = d.creases; for (auto& c : add) if (mv.count(c.a) && mv.count(c.b)) d.creases.push_back({mv[c.a], mv[c.b], c.w}); }
  (void)nf0;
  auto nb = std::make_shared<PolyMesh>(std::move(d)); topoWrite(m, nb);
  if (topoMode != "poly") setTopoMode("poly", true);
  tselApply(m, made, "set", 'f'); tsel_.B = nb.get();
  TOPO_DONE(tr("Mirrored: %1 polygon%2 in the same polygroup").arg(made.size()).arg(made.size() == 1 ? "" : "s") + (weld ? tr(" · welded at the middle") : QString()));
}

// ---------- the selection's walks ----------
QString Viewport::tGrowSel(bool shrink) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || tsel_.id != m || topoMode == "object") TOPO_NO(tr("Select something in a component mode first"));
  auto B = baseSp(*it); auto T = topo::ttopo(B); const auto hid = meshmods::hiddenFaces(*it, *B); const char key = tKey();
  auto shownF = [&](int f) { return hid.empty() || !hid[size_t(f)]; };
  std::vector<int> out;
  if (key == 'f') {
    std::vector<char> vIn(size_t(T->n), 0), vOut(size_t(T->n), 0);
    for (int f = 0; f < T->nf; f++) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) (tsel_.f.count(f) ? vIn : vOut)[size_t(T->fv[size_t(k)])] = 1;
    for (int f = 0; f < T->nf; f++) { bool touchIn = false, touchOut = false;
      for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { touchIn |= bool(vIn[size_t(T->fv[size_t(k)])]); touchOut |= bool(vOut[size_t(T->fv[size_t(k)])]); }
      const bool sel = tsel_.f.count(f) > 0;
      if (shrink ? (sel && !touchOut) : (sel || (touchIn && shownF(f)))) out.push_back(f); }
  } else if (key == 'v') {
    for (int v = 0; v < T->n; v++) { bool sel = tsel_.v.count(v) > 0, nIn = false, nOut = false;
      for (int i = T->voff[size_t(v)]; i < T->voff[size_t(v) + 1]; i++) { int o = T->other(T->vE[size_t(i)], v); if (tsel_.v.count(o)) nIn = true; else nOut = true; }
      if (shrink ? (sel && !nOut) : (sel || nIn)) out.push_back(v); }
  } else {
    std::vector<char> vIn(size_t(T->n), 0), vOut(size_t(T->n), 0);
    for (int e = 0; e < T->ne; e++) for (int v : {T->ea[size_t(e)], T->eb[size_t(e)]}) (tsel_.e.count(e) ? vIn : vOut)[size_t(v)] = 1;
    for (int e = 0; e < T->ne; e++) { const bool sel = tsel_.e.count(e) > 0; const int a = T->ea[size_t(e)], b = T->eb[size_t(e)];
      if (shrink ? (sel && !vOut[size_t(a)] && !vOut[size_t(b)]) : (sel || vIn[size_t(a)] || vIn[size_t(b)])) out.push_back(e); }
  }
  const size_t before = key == 'f' ? tsel_.f.size() : key == 'e' ? tsel_.e.size() : tsel_.v.size();
  tselApply(m, out, "set");
  ui::hud(shrink ? tr("Shrunk: %1 → %2").arg(before).arg(out.size()) : tr("Grown: %1 → %2").arg(before).arg(out.size()), 1000);
  return {};
}

QString Viewport::tMirrorSel(int dir) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || tsel_.id != m || topoMode == "object") TOPO_NO(tr("Select something in a component mode first"));
  auto B = baseSp(*it); auto T = topo::ttopo(B); const char key = tKey(); const auto& P = B->pos;
  const auto pairs = symFirstPairs(m, P); const double eps = std::max(1e-9, B->bounds().size().length() * 1e-4);
  auto side = [&](double x) { return x > eps ? 1 : x < -eps ? -1 : 0; };    // (+1: the +X side)
  auto mirV = [&](int v) { return size_t(v) < pairs.size() ? pairs[size_t(v)] : -1; };
  std::vector<int> out; int missing = 0;
  if (key == 'v') {
    for (int v : tsel_.v) if (side(P[size_t(v)].x) != -dir) out.push_back(v);   // (the target side's own picks go)
    for (int v : tsel_.v) if (side(P[size_t(v)].x) == dir) { int w = mirV(v); if (w >= 0) out.push_back(w); else missing++; }
  } else if (key == 'e') {
    for (int e : tsel_.e) if (side((P[size_t(T->ea[size_t(e)])].x + P[size_t(T->eb[size_t(e)])].x) / 2) != -dir) out.push_back(e);
    for (int e : tsel_.e) if (side((P[size_t(T->ea[size_t(e)])].x + P[size_t(T->eb[size_t(e)])].x) / 2) == dir) {
      int a = mirV(T->ea[size_t(e)]), b = mirV(T->eb[size_t(e)]); int x = a >= 0 && b >= 0 ? T->edgeOf(a, b) : -1; if (x >= 0) out.push_back(x); else missing++; }
  } else {
    std::map<std::vector<int>, int> byVerts;                       // (a polygon by its vertices, sorted)
    for (int f = 0; f < T->nf; f++) { std::vector<int> vs; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) vs.push_back(T->fv[size_t(k)]); std::sort(vs.begin(), vs.end()); byVerts[vs] = f; }
    auto cx = [&](int f) { double s = 0; int n = 0; for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { s += P[size_t(T->fv[size_t(k)])].x; n++; } return s / std::max(1, n); };
    for (int f : tsel_.f) if (side(cx(f)) != -dir) out.push_back(f);
    for (int f : tsel_.f) if (side(cx(f)) == dir) { std::vector<int> vs; bool ok = true;
      for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1] && ok; k++) { int w = mirV(T->fv[size_t(k)]); ok = w >= 0; vs.push_back(w); }
      std::sort(vs.begin(), vs.end()); auto g = ok ? byVerts.find(vs) : byVerts.end(); if (g != byVerts.end()) out.push_back(g->second); else missing++; }
  }
  tselApply(m, out, "set");
  ui::toast(tr("The selection copied %1").arg(dir > 0 ? tr("from +X onto −X") : tr("from −X onto +X")) + (missing ? tr(" · %1 without a mirror (the mesh isn't symmetric there)").arg(missing) : QString()));
  return {};
}

QString Viewport::tConvertSel(const QString& mode) {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr;
  if (!it || tsel_.id != m || topoMode == "object" || mode == "object" || mode == topoMode) { setTopoMode(mode); return {}; }
  auto T = topo::ttopo(baseSp(*it)); const char from = tKey(), to = mode == "poly" ? 'f' : mode == "edge" ? 'e' : 'v';
  std::set<int> vs, es, out;
  if (from == 'f') for (int f : tsel_.f) for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1]; k++) { vs.insert(T->fv[size_t(k)]); es.insert(T->ce[size_t(k)]); }
  else if (from == 'e') { es = tsel_.e; for (int e : es) { vs.insert(T->ea[size_t(e)]); vs.insert(T->eb[size_t(e)]); } }
  else { vs = tsel_.v; for (int e = 0; e < T->ne; e++) if (vs.count(T->ea[size_t(e)]) && vs.count(T->eb[size_t(e)])) es.insert(e); }
  if (to == 'v') out = vs;
  else if (to == 'e') out = es;
  else for (int f = 0; f < T->nf; f++) { bool all = true;                     // (polygons all of whose edges / vertices are picked)
    for (int k = T->fo[size_t(f)]; k < T->fo[size_t(f) + 1] && all; k++) all = from == 'e' ? es.count(T->ce[size_t(k)]) > 0 : vs.count(T->fv[size_t(k)]) > 0;
    if (all) out.insert(f); }
  setTopoMode(mode);
  tselApply(m, {out.begin(), out.end()}, "set", to);
  ui::hud(tr("The selection carried over: %1").arg(out.size()), 1000);
  return {};
}

// ---------- hiding polygons ----------
// the polygons the selection is on: the picked ones, those beside the picked edges, around the picked vertices
std::set<int> Viewport::tTouchedFaces(int m) const {
  const Item* it = scene_->item(m); std::set<int> fs; if (!it || tsel_.id != m) return fs;
  auto T = topo::ttopo(baseSp(*it)); const char key = tKey();
  if (key == 'f') fs = tsel_.f;
  else if (key == 'e') { for (int e : tsel_.e) for (int f : {T->ef0[size_t(e)], T->ef1[size_t(e)]}) if (f >= 0) fs.insert(f); }
  else for (int v : tsel_.v) for (int i = T->vfo[size_t(v)]; i < T->vfo[size_t(v) + 1]; i++) fs.insert(T->vF[size_t(i)]);
  return fs;
}
// Ctrl+Shift+H in a component mode: only the selected shown — the mesh's other polygons hidden, the other meshes too;
// again — all back as it was (the selection stays)
bool Viewport::tIsolateSel() {
  if (room() != "topo") return false;
  if (tcIso_) {
    TcIso z = *tcIso_; tcIso_.reset();
    if (Item* it = scene_->item(z.id)) { if (z.hid.isString()) it->raw.insert("hidF", z.hid); else it->raw.remove("hidF"); pickCache_.erase(z.id); }
    scene_->iso = z.iso; staticRev_++; topoRev_++; allWalls_ = true; emit sceneEdited(); emit topoState(); requestUpdate(); ui::toast(tr("Everything shown again")); return true;
  }
  if (topoMode == "object") return false;
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return false;
  const std::set<int> fs = tTouchedFaces(m); if (fs.empty()) { ui::toast(tr("Select polygons, edges or vertices first")); return true; }
  const int nf = baseOf(*it)->faceCount();
  tcIso_ = TcIso{m, it->raw.value("hidF"), scene_->iso};
  std::vector<char> h(static_cast<size_t>(nf), 1); for (int f : fs) if (f < nf) h[size_t(f)] = 0;
  { const auto old = meshmods::hiddenFaces(*it, *baseOf(*it)); for (size_t f = 0; f < old.size() && f < h.size(); f++) if (old[f]) h[f] = 1; }   // (what was hidden stays so)
  it->raw.insert("hidF", topo::smEnc(h)); pickCache_.erase(m);
  Scene::Isolation iso; iso.keep.insert(m); for (int x : meshmods::modsOf(*scene_, m)) iso.keep.insert(x); scene_->iso = iso;
  staticRev_++; topoRev_++; allWalls_ = true; emit sceneEdited(); emit topoState(); requestUpdate();
  ui::toast(tr("Only the selected shown (%1 polygon%2) — Ctrl+Shift+H again to show everything").arg(fs.size()).arg(fs.size() == 1 ? "" : "s"));
  return true;
}
// the visibility layers (the mesh's panel): one shown — its hidden polygons hidden, the polygroups' hiding off
QString Viewport::visApply(int m, const QString& hid) {
  Item* it = scene_->item(m); if (!it) return "!";
  runEdit([m, hid](Scene& s, Selection&, const CurveCache&) { if (Item* x = s.item(m)) { if (hid.isEmpty()) x->raw.remove("hidF"); else x->raw.insert("hidF", hid); x->raw.remove("hidGroups"); } return QString(); }, false);
  pickCache_.erase(m); staticRev_++; topoRev_++; allWalls_ = true; emit topoState(); requestUpdate();
  return hid.isEmpty() ? tr("The whole model shown") : tr("Visibility layer shown");
}
QString Viewport::visSelect(int m, const QString& hid) {
  Item* it = scene_->item(m); if (!it) return "!"; const int nf = baseOf(*it)->faceCount();
  std::vector<char> h = hid.isEmpty() ? std::vector<char>(size_t(nf), 0) : topo::smDec(hid, nf); if (int(h.size()) != nf) h.assign(size_t(nf), 0);
  std::vector<int> fs; for (int f = 0; f < nf; f++) if (!h[size_t(f)]) fs.push_back(f);
  if (sel_.items.size() != 1 || sel_.items[0] != m) { sel_ = {}; sel_.items = {m}; onSelChange(); }
  if (topoMode != "poly") setTopoMode("poly", true);
  tselApply(m, fs, "set", 'f'); ui::hud(tr("Selected: %1 polygon%2 of the layer").arg(fs.size()).arg(fs.size() == 1 ? "" : "s"), 1000); return {};
}

QString Viewport::tHideSel() {
  const int m = topoMesh(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || tsel_.id != m) TOPO_NO(tr("Select polygons (or edges / vertices) to hide"));
  auto T = topo::ttopo(baseSp(*it)); const std::set<int> fs = tTouchedFaces(m);
  if (fs.empty()) TOPO_NO(tr("Nothing selected to hide"));
  std::vector<char> h = it->raw.value("hidF").toString().isEmpty() ? std::vector<char>(size_t(T->nf), 0) : topo::smDec(it->raw.value("hidF").toString(), T->nf);
  if (int(h.size()) != T->nf) h.assign(size_t(T->nf), 0);
  for (int f : fs) if (f < T->nf) h[size_t(f)] = 1;
  it->raw.insert("hidF", topo::smEnc(h)); pickCache_.erase(m); tselClear();
  staticRev_++; topoRev_++;
  TOPO_DONE(tr("%1 polygon%2 hidden — Unhide all (over the viewport) shows them").arg(fs.size()).arg(fs.size() == 1 ? "" : "s"));
}
bool Viewport::anyHidden() const {
  if (room() != "topo") return false;
  for (auto& [id, it] : scene_->items) if (it.type == ItemType::IMesh && (!it.raw.value("hidF").toString().isEmpty() || !it.raw.value("hidGroups").toArray().isEmpty())) return true;
  return false;
}
QString Viewport::tUnhideAll() {
  int n = 0;
  for (auto& [id, it] : scene_->items) if (it.type == ItemType::IMesh && (it.raw.contains("hidF") || !it.raw.value("hidGroups").toArray().isEmpty())) { it.raw.remove("hidF"); it.raw.remove("hidGroups"); pickCache_.erase(id); n++; }
  if (tcIso_) { scene_->iso = tcIso_->iso; tcIso_.reset(); allWalls_ = true; }   // (the selection's isolation over too)
  if (!n) TOPO_NO(tr("Nothing hidden"));
  staticRev_++; topoRev_++;
  TOPO_DONE(tr("Every hidden polygon shown (%1 mesh%2)").arg(n).arg(n == 1 ? "" : "es"));
}

#undef TOPO_DONE
#undef TOPO_NO

}  // namespace hm

namespace hm {
int Viewport::hardEdgeNear(int m, const QPointF& mp, double px) const {
  const Item* it = scene_->item(m); if (!it) return -1; auto Bs = it->base ? it->base : it->poly; if (!Bs || Bs->hardEdges.empty()) return -1;
  auto T = topo::ttopo(Bs); const auto W = ttWorld(m); Projector P = projector(); int best = -1; double bd = px;
  for (auto& h : Bs->hardEdges) { const int e = h[0] < T->n && h[1] < T->n ? T->edgeOf(h[0], h[1]) : -1; if (e < 0) continue;
    const ScreenPt a = P.toScreen(W[size_t(h[0])]), b = P.toScreen(W[size_t(h[1])]); if (a.z >= 1 || b.z >= 1) continue;
    const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? std::clamp(((mp.x() - a.x) * dx + (mp.y() - a.y) * dy) / l2, 0.0, 1.0) : 0;
    const double d = std::hypot(a.x + dx * t - mp.x(), a.y + dy * t - mp.y()); if (d < bd) { bd = d; best = e; } }
  return best;
}
// the line of hard edges through e: on through each vertex where exactly two hard edges meet (a crossing of hard lines
// — three or more there — ends it, so one line is taken, not the whole web)
std::vector<int> Viewport::hardLine(int m, int e0) const {
  const Item* it = scene_->item(m); if (!it || e0 < 0) return {}; auto Bs = it->base ? it->base : it->poly; auto T = topo::ttopo(Bs);
  std::map<int, std::vector<int>> at; std::set<int> hard;
  for (auto& h : Bs->hardEdges) { const int e = h[0] < T->n && h[1] < T->n ? T->edgeOf(h[0], h[1]) : -1; if (e < 0 || !hard.insert(e).second) continue; at[h[0]].push_back(e); at[h[1]].push_back(e); }
  if (!hard.count(e0)) return {};
  std::set<int> got{e0}; std::vector<int> q{e0};
  for (size_t i = 0; i < q.size(); i++) { const int e = q[i];
    for (int v : {T->ea[size_t(e)], T->eb[size_t(e)]}) { const auto& L = at[v]; if (L.size() != 2) continue; for (int o : L) if (got.insert(o).second) q.push_back(o); } }
  return {got.begin(), got.end()};
}
}  // namespace hm

namespace hm {
QString Viewport::testHardSpot(int m) const {   // (checks) a hard edge's middle on the screen, its line's length
  const Item* it = scene_->item(m); if (!it) return "no mesh"; auto Bs = it->base ? it->base : it->poly; const auto W = ttWorld(m); Projector P = projector();
  for (auto& h : Bs->hardEdges) { const ScreenPt s = P.toScreen((W[size_t(h[0])] + W[size_t(h[1])]) * 0.5); if (s.z < 1 && s.x > 50 && s.y > 50 && s.x < width() - 50 && s.y < height() - 50) {
      auto T = topo::ttopo(Bs); return QString("%1,%2 line %3 of %4").arg(s.x, 0, 'f', 0).arg(s.y, 0, 'f', 0).arg(hardLine(m, T->edgeOf(h[0], h[1])).size()).arg(Bs->hardEdges.size()); } }
  return "none in sight";
}
}  // namespace hm
