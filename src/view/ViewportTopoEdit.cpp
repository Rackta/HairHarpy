// The Topology room's edits (the HTML version's tg* / sm* / tt* / topoWrite): each geometry change a new source of the
// mesh (an edited copy, 'pmesh' — the file stays, undo goes back to the old one), the polygroups (delete, duplicate,
// merge, move the selection in, detach, rename, their list's tree and visibility, isolate), the transform's pivot and
// Bake, the snap mask (from the picked polygroups — the covered parts left out —, the brush, the lasso, mirror, invert),
// the transform table under Settings.
#include "scene/MeshMods.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <cmath>

namespace hm {

namespace {
const PolyMesh* baseOf(const Item& m) { return m.base ? m.base.get() : m.poly.get(); }
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
int xformOf(const Scene& sc, int m) { for (int x : meshmods::modsOf(sc, m)) if (sc.item(x)->raw.value("kind").toString() == "xform") return x; return -1; }
Vec3 xformCenter(const Item* x, const PolyMesh& B) {
  QJsonObject p = x ? x->raw.value("p").toObject() : QJsonObject();
  return p.value("cx").isDouble() ? Vec3(p.value("cx").toDouble(), p.value("cy").toDouble(), p.value("cz").toDouble()) : B.bounds().center();
}
const double UNITS[] = {0.1, 1, 100, 2.54};    // mm cm m in (centimetres in one unit)
double unitK(const QString& u) { return u == "mm" ? UNITS[0] : u == "m" ? UNITS[2] : u == "in" ? UNITS[3] : UNITS[1]; }
QString plural(int n, const QString& one, const QString& many) { return n == 1 ? one : many; }
}  // namespace

// ---------- preferences (kept between runs) ----------
void Viewport::loadTopoPrefs() {
  QSettings st;
  QJsonObject s = QJsonDocument::fromJson(st.value("topo/set").toByteArray()).object(); tset.snap = s.value("snap").toBool(); tset.esnap = s.value("esnap").toBool(); tset.esnapMode = s.value("esnapMode").toString("vert"); tset.esnapPx = std::clamp(s.value("esnapPx").toDouble(14), 4.0, 60.0); tset.sym = false; /* (Symmetry: off at every start — never brought back from another run) */ tset.uv = false; /* (the UV mode: never brought back from another run) */ tset.keepUv = s.value("keepUv").toBool();
  tset.selThrough = s.value("selThrough").toBool(); { QString k = s.value("selTool").toString("cursor"); tset.selTool = k == "rect" || k == "lasso" || k == "fill" || k == "outline" || k == "loop" || k == "ring" || k == "hard" ? k : "cursor"; }
  tset.loopStopSel = s.value("loopStopSel").toBool(); tset.loopStopNgon = s.value("loopStopNgon").toBool(); tset.loopStopTri = s.value("loopStopTri").toBool();
  { QString o = s.value("gzOrient").toString("world"); tset.gzOrient = o == "local" || o == "normal" || o == "camera" ? o : "world"; }
  tset.gzFree = s.value("gzFree").toBool(); tset.gzStep = std::clamp(s.value("gzStep").toDouble(1), 0.001, 1000.0); tset.gzWhat = s.value("gzWhat").toString() == "each" ? "each" : "all"; tset.gzSlide = s.value("gzSlide").toBool(); { QString x = s.value("alAxis").toString("x"); tset.alAxis = QStringList{"x", "y", "z", "xy", "yz", "xz"}.contains(x) ? x : "x"; }
  { QString x = s.value("alTo").toString("sel"); tset.alTo = QStringList{"sel", "normal", "view", "world"}.contains(x) ? x : "sel"; }   // (the old To the vertex / Along the edges — The selection)
  { QString b = s.value("brushMode").toString("screen"); tset.brushMode = b == "surface" || b == "topo" ? b : "screen"; }
  tset.brushStr = std::clamp(s.value("brushStr").toDouble(0.5), 0.02, 1.0); tset.relaxStars = s.value("relaxStars").toBool(true); tset.brushAll = s.value("brushAll").toBool();
  { QString k = s.value("brushKind").toString("move"); tset.brushKind = k == "smooth" || k == "relax" || k == "inflate" || k == "shrink" ? k : "move"; }
  tset.soft = s.value("soft").toBool(); tset.softR = std::clamp(s.value("softR").toDouble(2), 0.01, 1e5); tset.softMode = s.value("softMode").toString() == "volume" ? "volume" : "surface";
  symo.fromJson(QJsonDocument::fromJson(QSettings().value("topo/symmetry").toByteArray()).object());
  QJsonObject uo = QJsonDocument::fromJson(st.value("topo/uv").toByteArray()).object();
  uvo.checker = uo.value("edChecker").toBool(false); uvo.c3d = uo.value("c3d").toBool(); uvo.s3d = uo.value("s3d").toBool(); uvo.dispOp = std::clamp(uo.value("dispOp").toDouble(0.6), 0.05, 1.0); uvo.tex = uo.value("tex").toBool(true); uvo.stretch = uo.value("stretch").toBool(); uvo.rotate = uo.value("rotate").toBool(true);
  uvo.density = std::clamp(uo.value("cells").toDouble(64), 8.0, 256.0); uvo.strAmt = std::clamp(uo.value("strAmt").toDouble(0.7), 0.0, 1.0); uvo.strUp = uo.value("strUp").toBool(true); uvo.strSpread = uo.value("strSpread").toBool(true); uvo.boxMid = uo.value("boxMid").toBool(); uvo.warpN = std::clamp(uo.value("warpN").toInt(3), 1, 12);   // (the labelled checker: 64 cells across the tile by default) uvo.pad = std::clamp(uo.value("pad").toDouble(0.01), 0.0, 0.1); uvo.angle = std::clamp(uo.value("angle").toDouble(50), 1.0, 179.0); uvo.symUnfold = uo.value("symUnfold").toBool(true); uvo.symTint = uo.value("symTint").toBool(true); uvo.pivShells = uo.value("pivShells").toBool(); uvo.diag = uo.value("diag").toBool(); uvo.stretchAngle = uo.value("stretchAngle").toBool(); uvo.symAxis = std::clamp(uo.value("symAxis").toDouble(0.5), -10.0, 10.0);
  QJsonObject d = QJsonDocument::fromJson(st.value("topo/disp").toByteArray()).object();
  tdisp.unlit = d.value("unlit").toBool(); tdisp.noTex = d.value("noTex").toBool(); tdisp.op = clamp(d.value("op").toDouble(1), 0, 1); tdisp.pdo = clamp(d.value("pdo").toDouble(0), 0, 1); tdisp.pdoOn = d.value("pdoOn").toBool(true);
  QJsonObject m = QJsonDocument::fromJson(st.value("topo/snapMask").toByteArray()).object();
  smo.cover = m.value("cover").toBool(true); smo.show = m.value("show").toBool(true); smo.thru = m.value("thru").toBool(); smo.lasso = m.value("lasso").toBool(); smo.dist = m.value("dist").toDouble(0.02);
  QString u = st.value("topo/unit", "cm").toString(); tunit = u == "mm" || u == "m" || u == "in" ? u : "cm"; tlocal = st.value("topo/tlocal", false).toBool();
}
void Viewport::saveTopoPrefs() const {
  QSettings st;
  st.setValue("topo/set", QJsonDocument(QJsonObject{{"snap", tset.snap}, {"esnap", tset.esnap}, {"esnapMode", tset.esnapMode}, {"esnapPx", tset.esnapPx}, {"sym", tset.sym}, {"uv", tset.uv}, {"keepUv", tset.keepUv}, {"selThrough", tset.selThrough}, {"selTool", tset.selTool},
                                                       {"loopStopSel", tset.loopStopSel}, {"loopStopNgon", tset.loopStopNgon}, {"loopStopTri", tset.loopStopTri}, {"soft", tset.soft}, {"softR", tset.softR}, {"softMode", tset.softMode},
                                                       {"gzOrient", tset.gzOrient == "custom" ? QString("world") : tset.gzOrient}, {"gzFree", tset.gzFree}, {"gzStep", tset.gzStep}, {"gzWhat", tset.gzWhat}, {"gzSlide", tset.gzSlide}, {"alAxis", tset.alAxis}, {"alTo", tset.alTo}, {"brushMode", tset.brushMode}, {"brushStr", tset.brushStr}, {"relaxStars", tset.relaxStars}, {"brushAll", tset.brushAll}, {"brushKind", tset.brushKind}}).toJson(QJsonDocument::Compact));
  st.setValue("topo/symmetry", QJsonDocument(symo.toJson()).toJson(QJsonDocument::Compact));   // (Symmetry's settings — both rooms)
  st.setValue("topo/disp", QJsonDocument(QJsonObject{{"unlit", tdisp.unlit}, {"noTex", tdisp.noTex}, {"op", tdisp.op}, {"pdo", tdisp.pdo}, {"pdoOn", tdisp.pdoOn}}).toJson(QJsonDocument::Compact));
  st.setValue("topo/snapMask", QJsonDocument(QJsonObject{{"cover", smo.cover}, {"show", smo.show}, {"thru", smo.thru}, {"lasso", smo.lasso}, {"dist", smo.dist}}).toJson(QJsonDocument::Compact));
  st.setValue("topo/unit", tunit); st.setValue("topo/tlocal", tlocal);
}

// ---------- the geometry written as a new source ----------
void Viewport::topoWrite(int m, std::shared_ptr<PolyMesh> B) {
  Item* it = scene_->item(m); if (!it || !B) return;
  if (thov_.m == m) thov_ = {};                              // (what a click would pick was found on the old polygons: gone with them)
  std::shared_ptr<PolyMesh> old = baseSp(*it);
  // (a drag's end: the base edited in place — what is shown followed it already; nothing made again)
  if (old.get() == B.get() && it->base && it->poly) {
    it->raw.insert("src", QJsonObject{{"ext", "pmesh"}, {"live", true}});
    if (auto pc = pickCache_.find(m); pc != pickCache_.end()) pc->second.box = it->poly->bounds();
    topoRev_++; scene_->revision++; heavyStale(m); return;
  }
  // (the Transform's pivot stays where it was: its centre was the file's — the mesh would jump)
  if (int x = xformOf(*scene_, m); x >= 0 && old) { Item* xi = scene_->item(x); QJsonObject p = xi->raw.value("p").toObject();
    if (!p.value("cx").isDouble()) { QJsonArray pv = it->raw.value("pivot").toArray(); Vec3 c = pv.size() == 3 ? Vec3(pv[0].toDouble(), pv[1].toDouble(), pv[2].toDouble()) : old->bounds().center();
      p.insert("cx", c.x); p.insert("cy", c.y); p.insert("cz", c.z); xi->raw.insert("p", p); } }
  // (painted weights belong to vertices: a changed vertex count drops them; the snap mask belongs to the polygons)
  if (old && old->pos.size() != B->pos.size()) for (int x : meshmods::modsOf(*scene_, m)) { const Item* xi = scene_->item(x); if (xi->raw.value("kind").toString() != "xform") { scene_->removeFromParent(x); scene_->items.erase(x); } }
  if (old && old->faceCount() != B->faceCount()) { it->raw.remove("smask"); it->raw.remove("hidF"); }   // (by polygon number: no longer theirs)
  if (old && it->raw.contains("selSets") && (old->faceCount() != B->faceCount() || old->pos.size() != B->pos.size() || old->faceVerts.size() != B->faceVerts.size())) {   // (the same topology: their numbers still hold)
    const QJsonArray s = topo::selSetsFollow(it->raw.value("selSets").toArray(), old, B);   // (the selection sets: onto the new geometry by place)
    if (s.isEmpty()) it->raw.remove("selSets"); else it->raw.insert("selSets", s); }
  it->base = B; it->poly = B;
  it->raw.insert("src", QJsonObject{{"ext", "pmesh"}, {"live", true}});
  meshSig_.erase(m); pickCache_.erase(m); staticRev_++; topoRev_++; scene_->revision++;
  if (old.get() == B.get()) heavyStale(m);                      // (changed in place: the LOD / hierarchy made again)
}

int Viewport::topoNewMesh(int parent, const QString& name, const PolyMesh& d, int after) {
  if (d.pos.empty() || d.faceCount() <= 0) return -1;
  if ((parent < 0 || !scene_->item(parent)) && room() == "topo") parent = meshesGroup(*scene_);   // (a new mesh of the Topology room: inside Meshes)
  auto B = std::make_shared<PolyMesh>(d);
  Item& it = scene_->addItem(ItemType::IMesh, parent, name);
  it.raw = QJsonObject{{"src", QJsonObject{{"ext", "pmesh"}, {"live", true}}}};
  it.base = B; it.poly = B;
  if (const Item* a = after >= 0 ? scene_->item(after) : nullptr; a && a->parent == it.parent) {   // (right under the mesh it came from, not at the end)
    const int id = it.id; std::vector<int>& L = it.parent >= 0 && scene_->item(it.parent) ? scene_->item(it.parent)->children : scene_->roots;
    L.erase(std::remove(L.begin(), L.end(), id), L.end()); auto at = std::find(L.begin(), L.end(), after); L.insert(at == L.end() ? L.end() : at + 1, id); scene_->reindex(); return id; }
  return it.id;
}

namespace {
// (an edit done: one undo step, the panels told; "!…" — nothing done)
}
#define TOPO_DONE(msg) do { scene_->revision++; commit(true); emit topoState(); ui::toast(msg); return msg; } while (0)
#define TOPO_NO(msg) do { ui::toast(msg); return "!" + QString(msg); } while (0)

QString Viewport::tgDelete(int m, int gi) {
  Item* it = scene_->item(m); if (!it) return "!";
  const PolyMesh& d = *baseOf(*it); if (d.groupNames.size() < 2) TOPO_NO(tr("The last polygroup — delete the mesh itself instead"));
  std::vector<char> keep(static_cast<size_t>(d.faceCount())); for (int f = 0; f < d.faceCount(); f++) keep[size_t(f)] = d.faceGroup[size_t(f)] != gi;
  PolyMesh s = topo::subset(d, keep); QString name = d.groupNames[size_t(gi)]; topo::dropGroup(s, gi);
  topo::groupsFollow(it->raw, [gi](int g) { return g == gi ? -1 : g > gi ? g - 1 : g; });
  topoWrite(m, std::make_shared<PolyMesh>(std::move(s))); tgSel = {};
  TOPO_DONE(tr("Polygroup %1 deleted").arg(name));
}

QString Viewport::tgDuplicate(int m, int gi) {
  Item* it = scene_->item(m); if (!it) return "!";
  const PolyMesh& d = *baseOf(*it); PolyMesh o = d; const int nf = d.faceCount(), n = int(d.pos.size()), G = int(d.groupNames.size());
  const bool uv = d.cornerUV.size() == d.faceVerts.size() && !d.cornerUV.empty(), fm = int(d.faceMat.size()) == nf;
  const bool nrm = d.cornerNormal.size() == d.faceVerts.size() && !d.cornerNormal.empty();
  std::unordered_map<int, int> vmap;
  for (int f = 0; f < nf; f++) if (d.faceGroup[size_t(f)] == gi) {
    for (int k = d.faceStart[size_t(f)]; k < d.faceStart[size_t(f) + 1]; k++) { int v = d.faceVerts[size_t(k)]; auto r = vmap.emplace(v, n + int(vmap.size())); if (r.second) { o.pos.push_back(d.pos[size_t(v)]); if (d.vcol.size() == d.pos.size() && !d.vcol.empty()) o.vcol.push_back(d.vcol[size_t(v)]); }
      o.faceVerts.push_back(r.first->second); if (uv) o.cornerUV.push_back(d.cornerUV[size_t(k)]); if (nrm) o.cornerNormal.push_back(d.cornerNormal[size_t(k)]); }
    o.faceStart.push_back(int(o.faceVerts.size())); o.faceGroup.push_back(G); if (fm) o.faceMat.push_back(d.faceMat[size_t(f)]);
  }
  // (the copy's hard edges and creases: those of the original whose two ends were copied)
  auto cp = [&](int a, int b, auto&& put) { auto ia = vmap.find(a), ib = vmap.find(b); if (ia != vmap.end() && ib != vmap.end()) put(ia->second, ib->second); };
  for (auto& e : d.hardEdges) cp(e[0], e[1], [&](int a, int b) { o.hardEdges.push_back({a, b}); });
  for (auto& c : d.creases) cp(c.a, c.b, [&](int a, int b) { o.creases.push_back({a, b, c.w}); });
  for (auto& e : d.seams) cp(e[0], e[1], [&](int a, int b) { o.seams.push_back({a, b}); });
  o.groupNames.push_back(d.groupNames[size_t(gi)] + " copy");
  if (it->raw.value("gOrder").isArray()) { QJsonArray a = it->raw.value("gOrder").toArray(), b; for (const QJsonValue& x : a) { b.append(x); if (x.toInt() == gi) b.append(G); } it->raw.insert("gOrder", b); }
  if (it->raw.value("gTree").isArray()) {                       // (the copy right under its original in the list)
    auto tree = topo::tgTree(it->raw.value("gTree"), G, it->raw.value("gOrder"));
    std::function<bool(std::vector<topo::TgNode>&)> ins = [&](std::vector<topo::TgNode>& L) {
      for (size_t i = 0; i < L.size(); i++) { if (!L[i].isFolder && L[i].g == gi) { topo::TgNode c; c.id = topo::tgNid(); c.g = G; L.insert(L.begin() + long(i) + 1, c); return true; } if (ins(L[i].kids)) return true; }
      return false; };
    ins(tree); it->raw.insert("gTree", topo::tgJson(tree));
  }
  QString msg = tr("%1 — a copy of %2 (lies on it)").arg(o.groupNames.back(), d.groupNames[size_t(gi)]);
  topoWrite(m, std::make_shared<PolyMesh>(std::move(o))); tgSel = {m, G};
  TOPO_DONE(msg);
}

QString Viewport::tgMerge(int m, int gi, int gj) {
  Item* it = scene_->item(m); if (!it || gi == gj) return "!";
  PolyMesh d = *baseOf(*it); for (int& g : d.faceGroup) if (g == gi) g = gj;
  QString a = d.groupNames[size_t(gi)], b = d.groupNames[size_t(gj)]; topo::dropGroup(d, gi);
  topo::groupsFollow(it->raw, [gi](int g) { return g == gi ? -1 : g > gi ? g - 1 : g; });
  auto nb = std::make_shared<PolyMesh>(std::move(d)); topo::shareTopo(nb, baseSp(*it));
  topoWrite(m, nb); tgSel = {m, gj > gi ? gj - 1 : gj};
  TOPO_DONE(tr("%1 merged into %2").arg(a, b));
}

QString Viewport::tgMoveSelHere(int m, int gi) {
  Item* it = scene_->item(m); if (!it) return "!";
  if (tsel_.id != m || tsel_.f.empty()) TOPO_NO(tr("Select polygons first (Polygon mode)"));
  PolyMesh d = *baseOf(*it);
  std::vector<int> had(d.groupNames.size(), 0); for (int g : d.faceGroup) if (g >= 0 && size_t(g) < had.size()) had[size_t(g)]++;
  int n = 0; for (int f : tsel_.f) if (size_t(f) < d.faceGroup.size() && d.faceGroup[size_t(f)] != gi) { d.faceGroup[size_t(f)] = gi; n++; }
  std::vector<int> cnt(d.groupNames.size(), 0); for (int g : d.faceGroup) cnt[size_t(g)]++;
  // (a group this move emptied goes; one empty already — made with ＋ — stays)
  std::vector<int> gone; for (int g = int(d.groupNames.size()) - 1; g >= 0; g--) if (!cnt[size_t(g)] && had[size_t(g)]) { topo::dropGroup(d, g); gone.push_back(g); }
  topo::groupsFollow(it->raw, [&](int g) { int r = g; for (int x : gone) { if (g == x) return -1; if (g > x) r--; } return r; });
  int gi2 = gi; for (int x : gone) if (x < gi) gi2--;
  QString name = size_t(gi2) < d.groupNames.size() ? d.groupNames[size_t(gi2)] : QString();
  auto nb = std::make_shared<PolyMesh>(std::move(d)); topo::shareTopo(nb, baseSp(*it)); topoWrite(m, nb);
  TOPO_DONE(tr("%1 polygon%2 moved into %3").arg(n).arg(n == 1 ? "" : "s").arg(name) + (gone.empty() ? QString() : tr(" · %1 empty group%2 removed").arg(gone.size()).arg(gone.size() == 1 ? "" : "s")));
}

QString Viewport::tgDetach(int m, int gi) {
  Item* it = scene_->item(m); if (!it) return "!";
  const PolyMesh d = *baseOf(*it); if (d.groupNames.size() < 2) TOPO_NO(tr("The only polygroup — the mesh is that already"));
  std::vector<char> take(static_cast<size_t>(d.faceCount())), keep(size_t(d.faceCount()));
  for (int f = 0; f < d.faceCount(); f++) { take[size_t(f)] = d.faceGroup[size_t(f)] == gi; keep[size_t(f)] = !take[size_t(f)]; }
  PolyMesh part = topo::subset(d, take); std::fill(part.faceGroup.begin(), part.faceGroup.end(), 0); part.groupNames = {d.groupNames[size_t(gi)]};
  PolyMesh rest = topo::subset(d, keep); topo::dropGroup(rest, gi);
  const int x = xformOf(*scene_, m); const QString gname = d.groupNames[size_t(gi)];
  // the piece: a mesh of its own next to this one, placed as this one is
  int nid = topoNewMesh(it->parent, it->name + " · " + gname, part, m);
  it = scene_->item(m); Item* ni = scene_->item(nid);
  for (const char* k : {"look", "smoothN", "twire", "pivot", "mat", "matPreset", "matOff", "unlit", "pgView"}) if (it->raw.contains(k)) ni->raw.insert(k, it->raw.value(k));
  ni->headTag = it->headTag; if (it->headTag) ni->raw.insert("headTag", true);
  if (x >= 0) { const Item* xi = scene_->item(x); Vec3 c = xformCenter(xi, d); QJsonObject p = xi->raw.value("p").toObject(); bool vis = xi->visible;
    int x2 = meshmods::newMeshExtra(*scene_, nid, "xform", nullptr);
    if (x2 >= 0) { p.insert("cx", c.x); p.insert("cy", c.y); p.insert("cz", c.z); scene_->item(x2)->raw.insert("p", p); scene_->item(x2)->visible = vis; } }
  topo::groupsFollow(scene_->item(m)->raw, [gi](int g) { return g == gi ? -1 : g > gi ? g - 1 : g; });
  topoWrite(m, std::make_shared<PolyMesh>(std::move(rest))); tgSel = {};
  emit sceneEdited();
  TOPO_DONE(tr("%1 → a separate mesh: %2").arg(gname, scene_->item(nid)->name));
}

QString Viewport::tgRename(int m, int gi, const QString& name0) {
  Item* it = scene_->item(m); QString name = name0.trimmed(); if (!it || name.isEmpty()) return "!";
  PolyMesh d = *baseOf(*it); if (size_t(gi) >= d.groupNames.size() || d.groupNames[size_t(gi)] == name) return "!";
  d.groupNames[size_t(gi)] = name;
  auto nb = std::make_shared<PolyMesh>(std::move(d)); topo::shareTopo(nb, baseSp(*it)); topoWrite(m, nb);
  scene_->revision++; commit(true); emit topoState(); return {};
}

void Viewport::tgSetVisible(int m, const std::vector<int>& gs, bool show) {
  Item* it = scene_->item(m); if (!it) return;
  std::set<int> s; for (const QJsonValue& v : it->raw.value("hidGroups").toArray()) s.insert(v.toInt());
  for (int g : gs) { if (show) s.erase(g); else s.insert(g); }
  QJsonArray a; for (int g : s) a.append(g); it->raw.insert("hidGroups", a);
  if (show && it->raw.contains("hidF")) {                       // (shown: its polygons hidden with H too — all of it back)
    const PolyMesh* B = baseOf(*it); const int nf = B->faceCount(); auto h = topo::smDec(it->raw.value("hidF").toString(), nf);
    if (int(h.size()) == nf && int(B->faceGroup.size()) == nf) { std::set<int> gss(gs.begin(), gs.end()); bool any = false;
      for (int f = 0; f < nf; f++) if (h[size_t(f)] && gss.count(B->faceGroup[size_t(f)])) { h[size_t(f)] = 0; any = true; }
      if (any) { if (std::any_of(h.begin(), h.end(), [](char x) { return x != 0; })) it->raw.insert("hidF", topo::smEnc(h)); else it->raw.remove("hidF"); } }
  }
  if (!show) {                                                  // (hidden: no longer picked anywhere — not lit, not in an edit of the picked)
    if (tgPick.id == m) for (int g : gs) tgPick.set.erase(g);
    if (tgSel.id == m && std::find(gs.begin(), gs.end(), tgSel.gi) != gs.end()) tgSel = {};
    if (tgHover_.id == m && std::find(gs.begin(), gs.end(), tgHover_.gi) != gs.end()) tgHover_ = {};
    if (tgList.id == m) tgList = {};
    if (tsel_.id == m && !tsel_.f.empty()) { const PolyMesh* B = baseOf(*it); std::set<int> hs(gs.begin(), gs.end());
      for (auto f = tsel_.f.begin(); f != tsel_.f.end();) { if (size_t(*f) < B->faceGroup.size() && hs.count(B->faceGroup[size_t(*f)])) f = tsel_.f.erase(f); else ++f; } tsel_.ver++; }
  }
  pickCache_.erase(m); staticRev_++; topoRev_++; requestUpdate(); emit topoState();
}

QString Viewport::tGroupBorder(int m, int gi) {
  const Item* it = scene_->item(m); if (!it) return "!";
  auto B = baseSp(*it); auto T = topo::ttopo(B); auto es = topo::groupBorder(*T, *B, gi);
  if (es.empty()) TOPO_NO(tr("This polygroup has no border (it is a closed part of its own)"));
  auto groupFacesAlong = [&] { std::set<int> fs; for (int e : es) for (int f : {T->ef0[size_t(e)], T->ef1[size_t(e)]}) if (f >= 0 && B->faceGroup[size_t(f)] == gi) fs.insert(f); return std::vector<int>(fs.begin(), fs.end()); };
  if (topoHeavy(m)) { auto fs = groupFacesAlong(); tSelectFaces(m, fs); ui::toast(tr("Border of %1: %2 polygons").arg(B->groupNames[size_t(gi)]).arg(fs.size())); return {}; }
  if (topoMode == "object") setTopoMode("edge");
  const char key = tKey(); std::vector<int> list = es;
  if (key == 'v') { std::set<int> vs; for (int e : es) { vs.insert(T->ea[size_t(e)]); vs.insert(T->eb[size_t(e)]); } list.assign(vs.begin(), vs.end()); }
  else if (key == 'f') list = groupFacesAlong();
  if (sel_.items.size() != 1 || sel_.items[0] != m) { sel_ = {}; sel_.items = {m}; onSelChange(); }
  tselApply(m, list, "set"); ui::toast(tr("Border of %1: %2").arg(B->groupNames[size_t(gi)]).arg(list.size()));
  return {};
}

QString Viewport::tSelectIdentical(int m, int gi) {
  const Item* it = scene_->item(m); if (!it) return "!";
  ui::BusyScope bz(tr("Comparing the parts"));
  int parts = 0, own = 0; auto fs = topo::identicalFaces(baseSp(*it), gi, &parts, &own);
  ui::toast(parts == own ? tr("No other part of this mesh is topologically the same") : tr("%1 topologically identical part%2").arg(parts).arg(parts == 1 ? "" : "s"));
  tSelectFaces(m, fs); return {};
}
QString Viewport::tSelectSymmetric(int m, int gi) {
  const Item* it = scene_->item(m); if (!it) return "!";
  ui::BusyScope bz(tr("Comparing the parts"));
  QString msg; auto fs = topo::symmetricFaces(baseSp(*it), gi, &msg);
  if (msg.startsWith('!')) { ui::toast(msg.mid(1)); return msg; }
  ui::toast(msg); tSelectFaces(m, fs); return {};
}

// Bake: the Transform written into the points (a mirroring scale turns the polygons back out), the Transform removed
QString Viewport::topoBakeXform(int m) {
  Item* it = scene_->item(m); if (!it) return "!";
  if (topoHeavy(m)) TOPO_NO(tr("A high-poly mesh (over 300 000 triangles) is not edited — Object mode only"));
  int x = xformOf(*scene_, m); if (x < 0) return "!";
  const Item* xi = scene_->item(x); PolyMesh d = *baseOf(*it); Affine M = meshmods::xformMatrix(xi->raw.value("p").toObject(), d.bounds().center());
  Vec3 piv = M.apply(xformCenter(xi, *baseOf(*it)));
  // (the gizmo's own axes set — Alt + drag, Align: they stay the mesh's axes — the points turned back by them about the pivot, a
  // Transform of that turn made again)
  const bool keepAxes = xi->raw.value("p").toObject().contains("gq"); const Quat Qg = (meshOwnQ(m) * meshGizmoQ(m)).normalized();
  Affine G; for (int j = 0; j < 3; j++) { const Vec3 c = Qg.rotate(Vec3(j == 0, j == 1, j == 2)); G.L[0][j] = c.x; G.L[1][j] = c.y; G.L[2][j] = c.z; }
  G.t = piv - G.lin(piv); const Affine Gi = G.inverse();
  for (auto& p : d.pos) { p = M.apply(p); if (keepAxes) p = Gi.apply(p); }
  for (auto& n : d.cornerNormal) { Vec3 r = M.normal({n[0], n[1], n[2]}); if (keepAxes) r = Gi.lin(r); n = {float(r.x), float(r.y), float(r.z)}; }
  if (M.det() < 0) {
    std::vector<int> fv = d.faceVerts; auto uv = d.cornerUV; auto nv = d.cornerNormal;
    const bool hasUV = uv.size() == fv.size() && !uv.empty(), hasN = nv.size() == fv.size() && !nv.empty();
    for (int f = 0; f < d.faceCount(); f++) { int s = d.faceStart[size_t(f)], n = d.faceStart[size_t(f) + 1] - s;
      for (int k = 1; k < n; k++) { d.faceVerts[size_t(s + k)] = fv[size_t(s + n - k)]; if (hasUV) d.cornerUV[size_t(s + k)] = uv[size_t(s + n - k)]; if (hasN) d.cornerNormal[size_t(s + k)] = nv[size_t(s + n - k)]; } }
  }
  it->raw.insert("pivot", QJsonArray{piv.x, piv.y, piv.z});        // (the pivot stays where it was shown)
  scene_->removeFromParent(x); scene_->items.erase(x);
  topoWrite(m, std::make_shared<PolyMesh>(std::move(d)));
  emit sceneEdited();
  if (keepAxes) { meshmods::setMeshMatrix(*scene_, m, G);
    if (Item* nx = xformOf(*scene_, m) >= 0 ? scene_->item(xformOf(*scene_, m)) : nullptr) { QJsonObject p = nx->raw.value("p").toObject(); p.insert("cx", piv.x); p.insert("cy", piv.y); p.insert("cz", piv.z); for (const char* k : {"px", "py", "pz"}) p.insert(k, 0); nx->raw.insert("p", p); }
    TOPO_DONE(tr("%1: the transform baked — the gizmo's axes are now the mesh's own").arg(it->name)); }
  TOPO_DONE(tr("%1: the transform baked into the geometry").arg(it->name));
}

Vec3 Viewport::meshPivotLocal(int m) const {
  const Item* it = scene_->item(m); if (!it) return {};
  int x = xformOf(*scene_, m); if (x >= 0) { QJsonObject p = scene_->item(x)->raw.value("p").toObject(); if (p.value("cx").isDouble()) return {p.value("cx").toDouble(), p.value("cy").toDouble(), p.value("cz").toDouble()}; }
  QJsonArray pv = it->raw.value("pivot").toArray(); if (pv.size() == 3) return {pv[0].toDouble(), pv[1].toDouble(), pv[2].toDouble()};
  return baseOf(*it) ? baseBox(baseOf(*it)).center() : Vec3();
}
// a geometry's box, kept while it stays (a heavy mesh's millions of points: not walked at every panel's sync)
Box3 Viewport::baseBox(const PolyMesh* B) const {
  if (!B) return {};
  for (auto& c : boxCache_) if (c.B == B && c.rev == scene_->revision && c.srev == staticRev_ && c.n == B->pos.size()) return c.box;
  if (boxCache_.size() > 32) boxCache_.erase(boxCache_.begin());
  boxCache_.push_back({B, scene_->revision, staticRev_, B->pos.size(), B->bounds()});
  return boxCache_.back().box;
}
// the pivot moved to c2 (file space) — the mesh stays where it is: the Transform's move makes up for the new centre
void Viewport::setMeshPivot(int m, const Vec3& c2) {
  Item* it = scene_->item(m); if (!it) return;
  if (int x = xformOf(*scene_, m); x >= 0) {
    Item* xi = scene_->item(x); QJsonObject p = xi->raw.value("p").toObject(); Vec3 c = xformCenter(xi, *baseOf(*it));
    Affine M = meshmods::xformMatrix(p, baseOf(*it)->bounds().center()); Vec3 d = c - c2;
    Vec3 t = Vec3(p.value("px").toDouble(), p.value("py").toDouble(), p.value("pz").toDouble()) + d - M.lin(d);
    p.insert("cx", c2.x); p.insert("cy", c2.y); p.insert("cz", c2.z); p.insert("px", t.x); p.insert("py", t.y); p.insert("pz", t.z); xi->raw.insert("p", p);
  }
  it->raw.insert("pivot", QJsonArray{c2.x, c2.y, c2.z});
  scene_->revision++; commit(false); emit topoState();
}

// Ctrl+H on picked polygroups: only they shown — the mesh's other groups hidden, the other meshes too; again — all back
bool Viewport::tgIsolateToggle() {
  if (room() != "topo") return false;
  if (tgIso_) {
    TgIso z = *tgIso_; tgIso_.reset();
    if (Item* it = scene_->item(z.id)) { if (z.hid.isArray()) it->raw.insert("hidGroups", z.hid); else it->raw.remove("hidGroups"); pickCache_.erase(z.id); }
    scene_->iso = z.iso; staticRev_++; topoRev_++; allWalls_ = true; emit sceneEdited(); requestUpdate(); ui::toast(tr("Everything shown again")); return true;
  }
  int m = topoMesh(); if (m < 0) return false;
  auto gs = smPickedGroups(m); if (gs.empty()) return false;
  Item* it = scene_->item(m); const PolyMesh* B = baseOf(*it);
  tgIso_ = TgIso{m, it->raw.value("hidGroups"), scene_->iso};
  QJsonArray h; for (int g = 0; g < int(B->groupNames.size()); g++) if (!gs.count(g)) h.append(g); it->raw.insert("hidGroups", h); pickCache_.erase(m);
  Scene::Isolation iso; iso.keep.insert(m); for (int x : meshmods::modsOf(*scene_, m)) iso.keep.insert(x); scene_->iso = iso;
  staticRev_++; topoRev_++; allWalls_ = true; emit sceneEdited(); requestUpdate();
  ui::toast(tr("Only %1 shown — Ctrl+H again to show everything").arg(gs.size() == 1 ? B->groupNames[size_t(*gs.begin())] : tr("%1 polygroups").arg(gs.size())));
  return true;
}

void Viewport::tgTreeEdit(int m, const std::function<void(std::vector<topo::TgNode>&)>& fn, bool commitNow, bool quiet) {
  Item* it = scene_->item(m); if (!it) return; const PolyMesh* B = baseOf(*it); if (!B) return;
  auto tree = topo::tgTree(it->raw.value("gTree"), int(B->groupNames.size()), it->raw.value("gOrder"));
  fn(tree); it->raw.insert("gTree", topo::tgJson(tree));
  if (quiet) { emit changed(); return; }                         // (a folder opened / closed: kept with the project, nothing rebuilt)
  if (commitNow) { scene_->revision++; commit(false); }
  emit topoState();
}

// ---------- the snap mask ----------
std::vector<char> Viewport::smArr(int m) const {
  ProfScope prof_("smArr");
  const Item* it = scene_->item(m); if (!it) return {};
  QString s = it->raw.value("smask").toString(); const PolyMesh* B = baseOf(*it); if (s.isEmpty() || !B) return {};
  return topo::smDec(s, B->faceCount());
}
void Viewport::smSet(Item& m, const std::vector<char>& a) {
  ProfScope prof_("smSet");
  bool any = std::any_of(a.begin(), a.end(), [](char x) { return x != 0; });
  if (any) m.raw.insert("smask", topo::smEnc(a)); else m.raw.remove("smask");
  static qint64 rev = QDateTime::currentMSecsSinceEpoch() * 1000; m.raw.insert("smRev", double(++rev));   // (the mask's version: what is made from it — the green, the ghost — made again by it)
  topoRev_++; requestUpdate();
}

QString Viewport::smCreate(int m) {
  Item* it = scene_->item(m); if (!it || !it->poly) return "!";
  auto gs = smPickedGroups(m);
  if (gs.empty()) TOPO_NO(tr("Pick the polygroups to snap to first (Ctrl+Shift in the Object mode, or in the list)"));
  const PolyMesh& B = *baseOf(*it); const auto& P = it->poly->pos; const int nf = B.faceCount();
  std::vector<char> arr(size_t(nf), 0); std::vector<int> cand; for (int f = 0; f < nf; f++) if (gs.count(B.faceGroup[size_t(f)])) cand.push_back(f);
  int cut = 0;
  if (!smo.cover) for (int f : cand) arr[size_t(f)] = 1;
  else if (auto px = maskProxy(m)) {                          // (a high-poly mesh: the covering looked for on its proxy — in the file's space)
    const auto& X = *px; const auto& LP = X.mesh->pos; const auto& I = X.bvh->idx; const size_t np = I.size() / 3;
    const double size = std::max(1e-9, X.mesh->bounds().size().length()), D = size * smo.dist, eps = size * 1e-4;
    std::vector<char> cov(np, 0);
    for (size_t t = 0; t < np; t++) { if (!gs.count(X.mesh->faceGroup[t])) continue;
      const Vec3 &a = LP[I[t * 3]], &b = LP[I[t * 3 + 1]], &c = LP[I[t * 3 + 2]]; const Vec3 n = (b - a).cross(c - a).normalized(), mid = (a + b + c) * (1.0 / 3);
      if (X.bvh->ray(mid + n * eps, n, D)) cov[t] = 1; }
    for (int f : cand) { const int p = X.f2p[size_t(f)]; if (p >= 0 && cov[size_t(p)]) cut++; else arr[size_t(f)] = 1; }
  } else {
    auto bvh = bvhOf(m, false); double size = it->poly->bounds().size().length(); if (!(size > 0)) size = 1;
    const double D = size * smo.dist, eps = size * 1e-4;
    ui::BusyScope bz(tr("Making the snap mask"));
    for (size_t i = 0; i < cand.size(); i++) {
      int f = cand[i], s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1]; Vec3 n, c;
      for (int k = s; k < e; k++) { const Vec3& a = P[size_t(B.faceVerts[size_t(k)])]; const Vec3& b = P[size_t(B.faceVerts[size_t(k + 1 < e ? k + 1 : s)])];
        n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); c += a; }
      n = n.normalized(); c = c * (1.0 / (e - s));
      if (bvh && bvh->ray(c + n * eps, n, D)) cut++; else arr[size_t(f)] = 1;   // (a layer of the mesh over it: covered — left out)
      if (i % 20000 == 19999) bz.progress(double(i) / cand.size());
    }
  }
  smSet(*it, arr); smPickedOff(m);
  int got = int(cand.size()) - cut;
  TOPO_DONE(tr("Snap mask: %1 polygon%2 of %3 polygroup%4").arg(got).arg(got == 1 ? "" : "s").arg(gs.size()).arg(gs.size() == 1 ? "" : "s") + (cut ? tr(" · %1 covered left out").arg(cut) : QString()));
}

QString Viewport::smAddGroups(int m, const std::set<int>& gs) {
  Item* it = scene_->item(m); if (!it) return "!";
  const PolyMesh& B = *baseOf(*it); auto a = smArr(m); if (a.size() != size_t(B.faceCount())) a.assign(size_t(B.faceCount()), 0);
  int n = 0; for (size_t f = 0; f < a.size(); f++) if (!a[f] && f < B.faceGroup.size() && gs.count(B.faceGroup[f])) { a[f] = 1; n++; }
  if (!n) TOPO_NO(tr("Those polygons are in the snap mask already"));
  smSet(*it, a);
  TOPO_DONE(tr("%1 polygon%2 into the snap mask").arg(n).arg(n == 1 ? "" : "s"));
}

QString Viewport::tgAddGroup(int m, int after) {
  Item* it = scene_->item(m); if (!it) return "!";
  PolyMesh d = *baseOf(*it); const int G = int(d.groupNames.size());
  if (int(d.faceGroup.size()) != d.faceCount()) d.faceGroup.assign(size_t(d.faceCount()), 0);
  QString name; for (int k = G + 1;; k++) { name = QString("Polygroup %1").arg(k); if (std::find(d.groupNames.begin(), d.groupNames.end(), name) == d.groupNames.end()) break; }
  d.groupNames.push_back(name);
  if (after >= 0 && it->raw.value("gTree").isArray()) {          // (in the list right under the one picked)
    auto tree = topo::tgTree(it->raw.value("gTree"), G + 1, it->raw.value("gOrder"));
    std::vector<topo::TgNode>* arr = nullptr; topo::TgNode node; bool found = false;
    topo::tgWalk(tree, [&](const topo::TgNode& n, int) { if (!n.isFolder && n.g == G) { node = n; found = true; } });
    if (found) { std::vector<topo::TgNode>* a0 = nullptr; topo::TgNode* x = topo::tgFind(tree, node.id, &a0); if (x) a0->erase(a0->begin() + (x - a0->data()));
      std::function<bool(std::vector<topo::TgNode>&)> ins = [&](std::vector<topo::TgNode>& L) {
        for (size_t i = 0; i < L.size(); i++) { if (!L[i].isFolder && L[i].g == after) { L.insert(L.begin() + long(i) + 1, node); return true; } if (ins(L[i].kids)) return true; } return false; };
      if (!ins(tree)) tree.push_back(node); it->raw.insert("gTree", topo::tgJson(tree)); }
    (void)arr;
  }
  auto nb = std::make_shared<PolyMesh>(std::move(d)); topo::shareTopo(nb, baseSp(*it)); topoWrite(m, nb); tgSel = {m, G};
  TOPO_DONE(tr("%1 — a new empty polygroup: select polygons and ⇲ move them into it").arg(name));
}

QString Viewport::smRemoveGroups(int m, const std::set<int>& gs) {
  Item* it = scene_->item(m); auto a = smArr(m); if (!it || a.empty()) return "!";
  const PolyMesh& B = *baseOf(*it); int n = 0; for (size_t f = 0; f < a.size(); f++) if (a[f] && gs.count(B.faceGroup[f])) { a[f] = 0; n++; }
  smSet(*it, a);
  TOPO_DONE(tr("%1 polygon%2 out of the snap mask").arg(n).arg(n == 1 ? "" : "s"));
}

// the mask mirrored across the mesh's X = 0: dir +1 — the +X side copied onto −X, −1 — the other way
QString Viewport::smMirror(int m, int dir) {
  Item* it = scene_->item(m); auto a0 = smArr(m); if (!it || a0.empty()) TOPO_NO(tr("No snap mask on this mesh"));
  auto Pp = topo::smPartners(baseSp(*it)); auto a = a0; int n = 0;
  for (size_t f = 0; f < a.size(); f++) if (Pp->cx[f] * dir < -Pp->plane) { int p = Pp->part[f]; a[f] = p >= 0 ? a0[size_t(p)] : 0; n += a[f]; }
  smSet(*it, a);
  TOPO_DONE(tr("Snap mask mirrored %1: %2 polygon%3").arg(dir > 0 ? "+X → −X" : "−X → +X").arg(n).arg(n == 1 ? "" : "s"));
}

// the picked polygroups added to the snap mask / taken off it (the rest of it stays)
QString Viewport::smGroups(int m, bool add) {
  Item* it = scene_->item(m); if (!it || !it->poly) return "!";
  auto gs = smPickedGroups(m); if (gs.empty()) TOPO_NO(tr("Pick a polygroup first (Ctrl+Shift in the Object mode, or in the list)"));
  const PolyMesh& B = *baseOf(*it); const int nf = B.faceCount(); auto a = smArr(m); if (int(a.size()) != nf) a.assign(size_t(nf), 0);
  int n = 0; for (int f = 0; f < nf && size_t(f) < B.faceGroup.size(); f++) if (gs.count(B.faceGroup[size_t(f)])) { a[size_t(f)] = add ? 1 : 0; n++; }
  smSet(*it, a); smPickedOff(m); scene_->revision++; commit(false); emit topoState();
  const QString msg = add ? tr("Added to the snap mask: %1 polygon%2").arg(n).arg(n == 1 ? "" : "s") : tr("Taken off the snap mask: %1 polygon%2").arg(n).arg(n == 1 ? "" : "s"); ui::toast(msg); return msg;
}
void Viewport::smPickedOff(int m) {
  if (tgPick.id == m) tgPick = {}; if (tgSel.id == m) tgSel = {}; tgHover_ = {};
  if (tsel_.id == m && (!tsel_.f.empty() || !tsel_.e.empty() || !tsel_.v.empty())) tselClear(); else { topoRev_++; emit topoState(); }
}
QString Viewport::smClear(int m, bool invert) {
  Item* it = scene_->item(m); auto a = smArr(m); if (!it) return "!";
  if (a.empty()) { if (!invert || !it->poly) return "!"; a.assign(size_t(baseOf(*it)->faceCount()), 0); }   // (no mask yet: inverted — all of it)
  if (invert) { for (char& x : a) x ^= 1; smSet(*it, a); } else smSet(*it, {});
  scene_->revision++; commit(false); emit topoState(); return {};
}

// the brush: the polygons within its circle on the surface (facing the camera; not covered — unless Through / X-ray)
void Viewport::smPaint(const QPointF& mp) {
  const int m = td_.m; Item* it = scene_->item(m); if (!it || !it->poly) return;
  std::optional<TopoPick> r;                                   // (a high-poly mesh: its hierarchy found the surface — no picking table made for millions of triangles)
  if (td_.px) { if (auto hr = heavyRay(m, ray(mp), false)) { if (!*hr) return; r = TopoPick{m, -1, -1, -1, (*hr)->point}; } }
  if (!r) r = tPick(mp, {m}); if (!r) return;
  const bool thru0 = smo.thru;                                // (only what the camera sees — unless Paint through)
  if (td_.px) {                                                // (a high-poly mesh: its proxy's triangles — in the file's space)
    const MaskProxy& X = *td_.px; const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse(); const double s = Mi.lin(Vec3(1, 0, 0)).length();
    const double rad = brushR * worldPerPixelAt(r->point) * s, tol = std::max(1e-9, X.mesh->bounds().size().length()) * 1e-4; const bool ortho = cam_.ortho();
    const auto& P = X.mesh->pos; const auto& I = X.bvh->idx; const char v = td_.erase ? 0 : 1; const double far = X.mesh->bounds().size().length() * 10;
    const PxVis& V = pxVis(m, X);                              // (its hidden polygroups: neither painted nor hiding what is behind them)
    const double otol = std::max(tol, rad * 0.3) / std::max(1e-12, s);   // (another mesh in front: by more than this — world units)
    auto pass = [&](const Vec3& c, const Vec3& eye, const Vec3& dir, bool mirrored) {
      X.bvh->sphere(c, rad, [&](int t) {
        if (size_t(t) >= td_.parr.size() || td_.parr[size_t(t)] == v || (!V.hid.empty() && V.hid[size_t(t)])) return;
        const Vec3 &a = P[I[size_t(t) * 3]], &b = P[I[size_t(t) * 3 + 1]], &cc = P[I[size_t(t) * 3 + 2]]; const Vec3 mid = (a + b + cc) * (1.0 / 3);
        if (mid.distanceTo(c) > rad) return;
        if (!thru0) { const Vec3 vv = ortho ? dir : mid - eye; if ((b - a).cross(cc - a).dot(vv) > 0) return;
          if (!pxSeen(m, X, V, t, M.apply(eye), M, ortho, M.lin(dir).normalized())) return;   // (the real surface in front of it — a plate over the cloth — keeps it)
          if (!mirrored && !td_.occ.empty()) { const Vec3 wm = M.apply(mid), wo = ortho ? wm - cam_.dir() * (far / std::max(1e-12, s)) : cam_.pos; if (occludedBy(td_.occ, wo, wm, otol)) return; } }
        td_.parr[size_t(t)] = v; });
    };
    const Vec3 c = Mi.apply(r->point), eye = Mi.apply(cam_.pos), dir = Mi.lin(cam_.dir()).normalized();
    const bool blocked = !thru0 && !td_.occ.empty() && occludedBy(td_.occ, ortho ? r->point - cam_.dir() * (far / std::max(1e-12, s)) : cam_.pos, r->point, otol);   // (the pointer over another mesh: nothing)
    if (!blocked) pass(c, eye, dir, false);
    if (!blocked && tset.sym) { auto mx = [](Vec3 q) { q.x = -q.x; return q; }; pass(mx(c), mx(eye), mx(dir), true); }   // (mirrored across the mesh's X = 0)
    maskPaintRev_++;
    if (const qint64 now = QDateTime::currentMSecsSinceEpoch(); now - maskPaintShown_ > 40) { maskPaintShown_ = now; topoRev_++; requestUpdate(); }
    return;
  }
  auto bvh = bvhOf(m, false); const PickCache* pc = pickCacheOf(m); if (!bvh || !pc) return;
  const double rad = brushR * worldPerPixelAt(r->point), size = std::max(1e-9, it->poly->bounds().size().length()), tol = size * 1e-4;
  const bool thru = smo.thru || scene_->fileRaw.value("settings").toObject().value("view").toObject().value("xray").toDouble(0.2) >= 0.5;
  const char v = td_.erase ? 0 : 1; const bool ortho = cam_.ortho(); const auto& P = it->poly->pos;
  auto pass = [&](const Vec3& c, const Vec3& eye, const Vec3& dir) {
    std::vector<std::pair<int, Vec3>> cand;
    bvh->sphere(c, rad, [&](int t) {
      const Vec3 &a = P[pc->tris[size_t(t) * 3]], &b = P[pc->tris[size_t(t) * 3 + 1]], &cc = P[pc->tris[size_t(t) * 3 + 2]];
      Vec3 mid = (a + b + cc) * (1.0 / 3), nrm = (b - a).cross(cc - a), vv = ortho ? dir : mid - eye;
      if (!thru && nrm.dot(vv) > 0) return;                    // (the back side stays)
      cand.push_back({t, mid});
    });
    for (auto& [t, p] : cand) {
      if (!thru) { Vec3 o = ortho ? p - dir * (size * 10) : eye; double L = o.distanceTo(p); if (bvh->ray(o, (p - o).normalized(), L - tol)) continue; }   // (something in front of it)
      int f = pc->tri2f[size_t(t)]; if (size_t(f) < td_.arr.size() && td_.arr[size_t(f)] != v) td_.arr[size_t(f)] = v;
    }
  };
  pass(r->point, cam_.pos, cam_.dir());
  if (tset.sym) {                                             // (the same stroke mirrored across the mesh's X = 0 — seen from the mirrored camera)
    Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse();
    auto mx = [&](const Vec3& q) { Vec3 l = Mi.apply(q); l.x = -l.x; return M.apply(l); };
    auto md = [&](const Vec3& q) { Vec3 l = Mi.lin(q); l.x = -l.x; return M.lin(l).normalized(); };
    pass(mx(r->point), mx(cam_.pos), md(cam_.dir()));
  }
  maskPaintRev_++;                                             // (shown as it goes — a few times a second; written into the mesh when the stroke ends: one undo step)
  if (const qint64 now = QDateTime::currentMSecsSinceEpoch(); now - maskPaintShown_ > 60) { maskPaintShown_ = now; topoRev_++; requestUpdate(); }
}

// the brush's lasso: Shift + drag — the polygons whose middle is inside the outline added, Shift + Ctrl — erased
void Viewport::smLasso() {
  const auto& L = td_.pts; if (L.size() < 3) return;
  double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300; for (auto [x, y] : L) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
  if (x1 - x0 < 3 && y1 - y0 < 3) return;
  const int m = td_.m; Item* it = scene_->item(m); if (!it || !it->poly) return;
  if (auto px = maskProxy(m)) {                               // (a high-poly mesh: its proxy's triangles the camera sees — in the file's space)
    const MaskProxy& X = *px; const Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse(); const auto& P = X.mesh->pos; const auto& I = X.bvh->idx;
    const double size = std::max(1e-9, X.mesh->bounds().size().length()), tol = size * 1e-4; const bool ortho = cam_.ortho();
    const Vec3 eye = Mi.apply(cam_.pos), dir = Mi.lin(cam_.dir()).normalized(); Projector pr = projector();
    auto pm = pxMaskOf(m, X); const auto pm0 = pm; const char v = td_.erase ? 0 : 1; int n = 0;
    const PxVis& V = pxVis(m, X); const Occluders occ = meshOccluders(m); const double wscale = M.lin(Vec3(1, 0, 0)).length();
    auto seen = [&](const Vec3& mid, const Vec3& nrm, int t) {   // (t: the triangle itself — the real surface asked; -1: a mirror image — the proxy)        // (inside the outline, facing the camera, nothing of the mesh in front)
      const ScreenPt s = pr.toScreen(M.apply(mid)); if (s.z > 1 || s.x < x0 || s.x > x1 || s.y < y0 || s.y > y1 || !insidePolygon(L, s.x, s.y)) return false;
      if (nrm.dot(ortho ? dir : mid - eye) > 0) return false;
      if (t >= 0) { if (!pxSeen(m, X, V, t, cam_.pos, M, ortho, cam_.dir())) return false; }
      else { const Vec3 o = ortho ? mid - dir * (size * 10) : eye; if (V.bvh->ray(o, (mid - o).normalized(), o.distanceTo(mid) - tol)) return false; }
      if (occ.empty()) return true; const Vec3 wm = M.apply(mid); return !occludedBy(occ, ortho ? wm - cam_.dir() * (size * 10 * wscale) : cam_.pos, wm, size * 2e-3 * wscale); };   // (another mesh in front: not painted)
    ui::BusyScope bz(tr("Lasso on the snap mask"));
    for (size_t t = 0; t < pm.size(); t++) {
      if (pm[t] == v || (!V.hid.empty() && V.hid[t])) continue;   // (a hidden polygroup: left as it is)
      const Vec3 &a = P[I[t * 3]], &b = P[I[t * 3 + 1]], &c = P[I[t * 3 + 2]]; const Vec3 mid = (a + b + c) * (1.0 / 3), nrm = (b - a).cross(c - a);
      bool in = seen(mid, nrm, int(t));
      if (!in && tset.sym) in = seen(Vec3(-mid.x, mid.y, mid.z), Vec3(-nrm.x, nrm.y, nrm.z), -1);   // (its mirror image in the outline: the other side's)
      if (in) { pm[t] = v; n++; }
    }
    if (!n) return;
    smSet(*it, pxApply(X, pm0, pm, smArr(m), faceLocked(m))); scene_->revision++; commit(false); emit topoState();
    ui::toast(tr("%1 triangle%2 of the proxy %3 the snap mask").arg(n).arg(n == 1 ? "" : "s").arg(td_.erase ? tr("out of") : tr("into")));
    return;
  }
  const PickCache* pc = pickCacheOf(m); auto bvh = bvhOf(m, false); if (!pc || !bvh) return;
  const bool thru = false;                                     // (the lasso: only what the camera sees)
  const auto& P = it->poly->pos; const PolyMesh& B = *baseOf(*it); const double size = std::max(1e-9, it->poly->bounds().size().length()), tol = size * 1e-4;
  auto a0 = smArr(m); std::vector<char> arr = !a0.empty() ? a0 : std::vector<char>(size_t(B.faceCount()), td_.erase ? 1 : 0); const char v = td_.erase ? 0 : 1;
  Projector pr = projector(); const bool ortho = cam_.ortho(); const Vec3 eye = cam_.pos, dir = cam_.dir(); std::vector<int> ch;
  ui::BusyScope bz(tr("Lasso on the snap mask"));
  for (size_t t = 0; t < pc->tri2f.size(); t++) {
    const Vec3 &a = P[pc->tris[t * 3]], &b = P[pc->tris[t * 3 + 1]], &c = P[pc->tris[t * 3 + 2]]; Vec3 mid = (a + b + c) * (1.0 / 3);
    ScreenPt s = pr.toScreen(mid); if (s.z > 1 || s.x < x0 || s.x > x1 || s.y < y0 || s.y > y1 || !insidePolygon(L, s.x, s.y)) continue;
    int f = pc->tri2f[t]; if (arr[size_t(f)] == v) continue;
    if (!thru) {
      Vec3 nrm = (b - a).cross(c - a), vv = ortho ? dir : mid - eye; if (nrm.dot(vv) > 0) continue;
      Vec3 o = ortho ? mid - dir * (size * 10) : eye; double Ld = o.distanceTo(mid); if (bvh->ray(o, (mid - o).normalized(), Ld - tol)) continue;
    }
    arr[size_t(f)] = v; ch.push_back(f);
  }
  if (tset.sym && !ch.empty()) { auto Pp = topo::smPartners(baseSp(*it)); size_t n0 = ch.size(); for (size_t i = 0; i < n0; i++) { int q = Pp->part[size_t(ch[i])]; if (q >= 0 && arr[size_t(q)] != v) { arr[size_t(q)] = v; ch.push_back(q); } } }
  if (ch.empty()) return;
  smSet(*it, arr); scene_->revision++; commit(false); emit topoState();
  ui::toast(tr("%1 polygon%2 %3 the snap mask").arg(ch.size()).arg(ch.size() == 1 ? "" : "s").arg(td_.erase ? tr("out of") : tr("into")));
}

// ---------- the transform table ----------
namespace {
Quat qOfJ(const QJsonValue& v) { const QJsonArray a = v.toArray(); return a.size() == 4 ? Quat{a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()}.normalized() : Quat{}; }
QJsonArray jOfQ(const Quat& q) { return QJsonArray{q.x, q.y, q.z, q.w}; }
Quat parentsQ(const Scene& sc, const Item& it) { Quat q; for (const Item* p = sc.item(it.parent); p; p = sc.item(p->parent)) q = (qOfJ(p->raw.value("q")) * q).normalized(); return q; }
const Item* parentObject(const Scene& sc, const Item& it) { for (const Item* p = sc.item(it.parent); p; p = sc.item(p->parent)) if (p->type == ItemType::IMesh) return p; return nullptr; }
Quat rotOf(const Affine& M) { const Vec3 z = M.lin({0, 0, 1}), y = M.lin({0, 1, 0}); return z.lengthSq() > 0 && y.lengthSq() > 0 ? Quat::lookRotation(-z.normalized(), y.normalized()) : Quat{}; }
Affine affOf(const Quat& q, const Vec3& about) { Affine A; const Vec3 E[3] = {q.rotate({1, 0, 0}), q.rotate({0, 1, 0}), q.rotate({0, 0, 1})};
  for (int i = 0; i < 3; i++) { A.L[0][i] = E[i].x; A.L[1][i] = E[i].y; A.L[2][i] = E[i].z; } A.t = about - A.lin(about); return A; }
// (the XYZ angles, degrees, as the Transform's — and back)
std::array<double, 3> eulerOf(const Quat& q) {
  const Affine A = affOf(q, {}); const double m13 = std::clamp(A.L[0][2], -1.0, 1.0); double x, y = std::asin(m13), z;
  if (std::abs(m13) < 0.9999999) { x = std::atan2(-A.L[1][2], A.L[2][2]); z = std::atan2(-A.L[0][1], A.L[0][0]); } else { x = std::atan2(A.L[2][1], A.L[1][1]); z = 0; }
  const double d = 180 / M_PI; auto r = [](double v) { return std::abs(v) < 1e-9 ? 0.0 : v; }; return {r(x * d), r(y * d), r(z * d)};
}
Quat quatOfEuler(const std::array<double, 3>& e) {
  const double a = std::cos(e[0] * M_PI / 180), b = std::sin(e[0] * M_PI / 180), c = std::cos(e[1] * M_PI / 180), d = std::sin(e[1] * M_PI / 180), f0 = std::cos(e[2] * M_PI / 180), f = std::sin(e[2] * M_PI / 180);
  const Vec3 y(-c * f, a * f0 - b * f * d, b * f0 + a * f * d), z(d, -b * c, a * c);   // (the columns of the XYZ turn)
  return Quat::lookRotation(-z, y);
}
}  // namespace

// what is under an item (objects, groups — however deep) given the change D: the objects' matrices, the groups' points
// and, under an object, their axes (a group under a group turns with it by itself)
void Viewport::carryUnder(int id, const Affine& D) {
  const Item* top = scene_->item(id); if (!top) return; const Quat R = rotOf(D);
  std::function<void(int)> walk = [&](int pid) { for (int c : scene_->item(pid)->children) { Item* ci = scene_->item(c); if (!ci) continue;
    if (ci->type == ItemType::IMesh && ci->poly) meshmods::setMeshMatrix(*scene_, c, Affine::mul(D, meshmods::meshMatrix(*scene_, *ci)));
    else if (ci->type == ItemType::Group) {
      const QJsonArray a = ci->raw.value("gpivot").toArray(); const Vec3 q = D.apply(a.size() == 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3()); ci->raw.insert("gpivot", QJsonArray{q.x, q.y, q.z});
      if (const Item* p = scene_->item(ci->parent); p && p->type != ItemType::Group) { const Quat P = parentsQ(*scene_, *ci); ci->raw.insert("q", jOfQ((P.conj() * R * P * qOfJ(ci->raw.value("q"))).normalized())); } }
    walk(c); } };
  walk(id);
}
void Viewport::turnGroupsUnder(int id, const Quat& R) {
  std::function<void(int)> walk = [&](int pid) { for (int c : scene_->item(pid)->children) { Item* ci = scene_->item(c); if (!ci) continue;
    if (ci->type == ItemType::Group) if (const Item* p = scene_->item(ci->parent); p && p->type != ItemType::Group) { const Quat P = parentsQ(*scene_, *ci); ci->raw.insert("q", jOfQ((P.conj() * R * P * qOfJ(ci->raw.value("q"))).normalized())); }
    walk(c); } };
  walk(id);
}
Affine Viewport::meshFrame(int m) const {
  Affine G; const Item* it = scene_->item(m); if (!it) return G;
  const Quat Q = (meshOwnQ(m) * meshGizmoQ(m)).normalized(); const Vec3 E[3] = {Q.rotate({1, 0, 0}), Q.rotate({0, 1, 0}), Q.rotate({0, 0, 1})};
  for (int i = 0; i < 3; i++) { G.L[0][i] = E[i].x; G.L[1][i] = E[i].y; G.L[2][i] = E[i].z; }
  G.t = meshmods::meshMatrix(*scene_, *it).apply(meshPivotLocal(m)); return G;
}
Viewport::TtTarget Viewport::ttTarget() const {
  TtTarget t; if (room() != "topo") return t;
  // (a group alone picked, Object mode: its own point and axes)
  if (topoMode == "object" && sel_.items.size() == 1) if (const Item* g = scene_->item(sel_.items[0]); g && g->type == ItemType::Group) { t.m = g->id; t.kind = 4; return t; }
  int m = topoMesh();
  if (m < 0) for (int id : sel_.items) if (const Item* it = scene_->item(id); it && it->type == ItemType::IMesh && it->poly) m = id;
  if (m < 0) return t;
  t.m = m;
  if (topoMode != "object") { t.verts = tSelVerts(m); t.kind = t.verts.empty() ? 3 : 2; return t; }
  t.kind = 1; return t;
}
std::array<std::optional<std::array<double, 3>>, 3> Viewport::ttValues(const TtTarget& t) const {
  std::array<std::optional<std::array<double, 3>>, 3> out; const double k = unitK(tunit);
  const Item* it = scene_->item(t.m); if (!it || t.kind == 0 || t.kind == 3) return out;
  if (t.kind == 4) {                                           // (a group: its point, its axes — Local: from its object, along that object's axes)
    const QJsonArray a = it->raw.value("gpivot").toArray(); Vec3 gp = a.size() == 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3();
    Quat q = (parentsQ(*scene_, *it) * qOfJ(it->raw.value("q"))).normalized();
    if (const Item* po = tlocal ? parentObject(*scene_, *it) : nullptr) { const Affine M = meshmods::meshMatrix(*scene_, *po); gp = M.inverse().apply(gp); q = (rotOf(M).conj() * q).normalized(); }
    out[0] = std::array<double, 3>{gp.x / k, gp.y / k, gp.z / k}; out[1] = eulerOf(q); return out;
  }
  if (t.kind == 2) {                                           // (the centre; the size — the selection's box along the world axes)
    // (Local: about the object's pivot, along its gizmo's axes)
    Affine Gi; if (tlocal) Gi = meshFrame(t.m).inverse();
    const auto& W = it->poly->pos; Vec3 c; Box3 b; for (int v : t.verts) { const Vec3 q = Gi.apply(W[size_t(v)]); c += q; b.expand(q); }
    c = c * (1.0 / t.verts.size()); Vec3 s = b.size();
    out[0] = std::array<double, 3>{c.x / k, c.y / k, c.z / k}; out[2] = std::array<double, 3>{s.x / k, s.y / k, s.z / k}; return out;
  }
  int x = xformOf(*scene_, t.m); QJsonObject p = x >= 0 ? scene_->item(x)->raw.value("p").toObject() : QJsonObject();
  Vec3 c = meshPivotLocal(t.m), bs = baseBox(baseOf(*it)).size();
  Vec3 pw(c.x + p.value("px").toDouble(), c.y + p.value("py").toDouble(), c.z + p.value("pz").toDouble());
  if (tlocal) pw = meshFrame(t.m).inverse().lin(pw);            // (Local: the place along the object's own axes)
  out[0] = std::array<double, 3>{pw.x / k, pw.y / k, pw.z / k};
  out[1] = std::array<double, 3>{p.value("rx").toDouble(), p.value("ry").toDouble(), p.value("rz").toDouble()};
  out[2] = std::array<double, 3>{bs.x * p.value("sx").toDouble(1) / k, bs.y * p.value("sy").toDouble(1) / k, bs.z * p.value("sz").toDouble(1) / k};
  return out;
}
// one cell set to v (shown units / degrees); live — no undo step yet (ttDone)
void Viewport::ttSet(TtTarget& t, int r, int c, double v) {
  const double k = unitK(tunit); Item* it = scene_->item(t.m); if (!it) return;
  if (t.kind == 4) {                                           // (a group moved / turned: what is in it goes along)
    if (r == 2) return; const auto vals = ttValues(t); if (!vals[size_t(r)]) return;
    const QJsonArray a = it->raw.value("gpivot").toArray(); const Vec3 gp = a.size() == 3 ? Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()) : Vec3();
    const Item* po = tlocal ? parentObject(*scene_, *it) : nullptr; const Affine M = po ? meshmods::meshMatrix(*scene_, *po) : Affine{};
    auto e = *vals[size_t(r)]; e[size_t(c)] = v; Affine D;
    if (r == 0) { const Vec3 w = M.apply(Vec3(e[0] * k, e[1] * k, e[2] * k)); D.t = w - gp; it->raw.insert("gpivot", QJsonArray{w.x, w.y, w.z}); }
    else { const Quat P = parentsQ(*scene_, *it), q0 = (P * qOfJ(it->raw.value("q"))).normalized(); const Quat q1 = ((po ? rotOf(M) : Quat{}) * quatOfEuler(e)).normalized();
      D = affOf((q1 * q0.conj()).normalized(), gp); it->raw.insert("q", jOfQ((P.conj() * q1).normalized())); }
    carryUnder(t.m, D); scene_->revision++; requestUpdate(); return;
  }
  if (t.kind == 2) {
    if (r == 1) return;
    const PolyMesh& B = *baseOf(*it); std::vector<Vec3> P = B.pos; Affine M = meshmods::meshMatrix(*scene_, *it), Mi = M.inverse();
    if (t.cbase.empty()) t.cbase = P;
    if (tlocal) { const Affine G = meshFrame(t.m), Gi = G.inverse(); M = Affine::mul(Gi, M); Mi = Affine::mul(Mi, G); }   // (Local: worked in the gizmo's frame — then back)
    if (r == 2) {
      // the size: the selection scaled along that world axis about its box's middle (0 — flattened onto that plane)
      double lo = 1e300, hi = -1e300; for (int vv : t.verts) { double x = M.apply(t.cbase[size_t(vv)])[c]; lo = std::min(lo, x); hi = std::max(hi, x); }
      double size = hi - lo, mid = (lo + hi) / 2; if (size < 1e-9) return;   // (already flat: there's nothing to stretch)
      double f = std::max(0.0, v * k) / size;
      for (int vv : t.verts) { Vec3 w = M.apply(P[size_t(vv)]), wb = M.apply(t.cbase[size_t(vv)]); double nx = mid + (wb[c] - mid) * f; w = Vec3(c == 0 ? nx : w.x, c == 1 ? nx : w.y, c == 2 ? nx : w.z); P[size_t(vv)] = Mi.apply(w); }
    } else {
      Vec3 cw; for (int vv : t.verts) cw += P[size_t(vv)]; cw = M.apply(cw * (1.0 / t.verts.size()));
      Vec3 d(c == 0 ? v * k - cw.x : 0, c == 1 ? v * k - cw.y : 0, c == 2 ? v * k - cw.z : 0), l = Mi.lin(d);
      for (int vv : t.verts) P[size_t(vv)] += l;
    }
    std::vector<int> pairs; if (tset.sym) pairs = symFirstPairs(t.m, B.pos);
    { const bool s0 = tset.snap; tset.snap = false; topoAfterMove(t.m, P, t.verts, B.pos, tset.sym ? &pairs : nullptr); tset.snap = s0; }   // (a typed value is exact: not laid back onto the surface)
    if (tset.keepUv && uv::has(B)) { auto U = uvKeep(t.m, B.pos, B.cornerUV, P); topoLive(t.m, P, &U); } else topoLive(t.m, P);
    return;
  }
  int x = xformOf(*scene_, t.m);
  if (x < 0) { x = meshmods::newMeshExtra(*scene_, t.m, "xform", nullptr); if (x < 0) return; scene_->item(x)->visible = true; emit sceneEdited(); }
  Item* xi = scene_->item(x); QJsonObject p = xi->raw.value("p").toObject(); const QString ax = QString("xyz")[c];
  const Affine M0 = meshmods::meshMatrix(*scene_, *it);
  // (what is under the object goes with it: the change of its matrix put on theirs)
  const auto follow = [this, M0, m = t.m] { carryUnder(m, Affine::mul(meshmods::meshMatrix(*scene_, *scene_->item(m)), M0.inverse())); };
  if (r == 0 && tlocal) {                                       // (Local: the place along the object's own axes — all three set from it)
    const Vec3 c0 = xformCenter(xi, *baseOf(*it)); const Affine G = meshFrame(t.m); Vec3 l = G.inverse().lin(Vec3(c0.x + p.value("px").toDouble(), c0.y + p.value("py").toDouble(), c0.z + p.value("pz").toDouble()));
    l = Vec3(c == 0 ? v * k : l.x, c == 1 ? v * k : l.y, c == 2 ? v * k : l.z); const Vec3 w = G.lin(l);
    p.insert("px", w.x - c0.x); p.insert("py", w.y - c0.y); p.insert("pz", w.z - c0.z); }
  else if (r == 0) p.insert("p" + ax, v * k - xformCenter(xi, *baseOf(*it))[c]);
  else if (r == 1) p.insert("r" + ax, v);
  else { double b = baseOf(*it)->bounds().size()[c]; if (b > 1e-9) p.insert("s" + ax, std::max(1e-6, v * k / b)); }
  xi->raw.insert("p", p); follow(); requestUpdate();
}
void Viewport::ttDone(const TtTarget& t) {
  if (t.kind == 2) if (Item* it = scene_->item(t.m)) topoWrite(t.m, baseSp(*it));
  scene_->revision++; commit(false); emit topoState();
}

#undef TOPO_DONE
#undef TOPO_NO

}  // namespace hm

namespace hm {
#define TOPO_NO(msg) do { ui::toast(msg); return "!" + QString(msg); } while (0)
// the mesh's hidden parts (its hidden polygroups, the polygons hidden with H) taken off its snap mask
QString Viewport::smHiddenOff(int m) {
  Item* it = scene_->item(m); if (!it || !it->poly) return "!"; auto a = smArr(m); if (a.empty()) TOPO_NO(tr("No snap mask yet"));
  const auto lk = faceLocked(m); if (lk.empty()) TOPO_NO(tr("Nothing of this mesh is hidden"));
  int n = 0; for (size_t f = 0; f < a.size() && f < lk.size(); f++) if (lk[f] && a[f]) { a[f] = 0; n++; }
  if (!n) TOPO_NO(tr("The hidden parts are not in the mask"));
  smSet(*it, a); scene_->revision++; commit(false); emit topoState();
  const QString msg = tr("Hidden parts taken off the snap mask: %1 polygon%2").arg(n).arg(n == 1 ? "" : "s"); ui::toast(msg); return msg;
}
}  // namespace hm
#undef TOPO_NO
