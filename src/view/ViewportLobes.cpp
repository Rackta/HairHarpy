// Lobe Brush: a mesh's polygons painted into lobes (layers kept with the mesh). LMB paints the active lobe, Ctrl erases
// it, Shift fills from the pointer up to the crevices, Ctrl+Shift picks the lobe under the pointer; Ctrl+RMB drag — the
// size; Enter — the lobe done, a new one started. The active lobe white, the others in their colours. The retopology the
// user makes on one lobe (in any tool) shown on the other lobes — each laid onto a square (scene/Lobes.cpp) — and made
// real there on Apply
#include "scene/Lobes.h"
#include "scene/MeshMods.h"
#include "scene/TopoEdit.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <optional>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace hm {

namespace {
std::shared_ptr<PolyMesh> baseSp(const Item& m) { return m.base ? m.base : m.poly; }
const PolyMesh* baseOf(const Item& m) { return baseSp(m).get(); }
}  // namespace

QColor lobeColor(int k) { return QColor::fromHslF(std::fmod(k * 137.508 + 25, 360.0) / 360.0, 0.62, 0.55); }   // (the golden angle round the hues: neighbours far apart)

// ---------- the layers ----------
int Viewport::lobeTarget() const {
  const Item* it = lb_.m >= 0 ? scene_->item(lb_.m) : nullptr;
  if (it && it->type == ItemType::IMesh && it->poly && scene_->isVisible(*it)) return lb_.m;
  for (auto& [id, x] : scene_->items) if (x.type == ItemType::IMesh && x.poly && scene_->isVisible(x) && x.raw.contains("lobes")) return id;   // (one painted before)
  return -1;
}
std::vector<QString> Viewport::lobeNames(int m) const {
  std::vector<QString> o; const Item* it = scene_->item(m); if (!it) return o;
  for (const QJsonValue& v : it->raw.value("lobes").toArray()) o.push_back(v.toString()); return o;
}
int Viewport::lobeActive(int m) const { const Item* it = scene_->item(m); if (!it) return 0; const int n = int(it->raw.value("lobes").toArray().size()); return n ? std::clamp(it->raw.value("lobeAct").toInt(1), 1, n) : 0; }
const std::vector<int>& Viewport::lobeLabels(int m) {
  const Item* it = scene_->item(m); static const std::vector<int> none; if (!it || !it->poly) return none;
  const PolyMesh* B = baseOf(*it); const QString key = it->raw.value("lobeF").toString();
  if (lb_.m != m || lb_.B != B || lb_.labKey != key || int(lb_.lab.size()) != B->faceCount()) { lb_.m = m; lb_.B = B; lb_.labKey = key; lb_.lab = lobes::dec(key, B->faceCount()); }
  return lb_.lab;
}
std::vector<int> Viewport::lobeCounts(int m) {
  std::vector<int> c(lobeNames(m).size() + 1, 0); for (int x : lobeLabels(m)) if (x > 0 && size_t(x) < c.size()) c[size_t(x)]++; return c;
}
// the labels written into the mesh (one undo step)
void Viewport::lobeWrite(int m, const std::vector<int>& lab, bool commitIt) {
  Item* it = scene_->item(m); if (!it) return;
  const QString s = lobes::enc(lab); if (s.isEmpty()) it->raw.remove("lobeF"); else it->raw.insert("lobeF", s);
  lb_.labKey = s; lb_.lab = lab; lb_.drawKey.clear(); lb_.paintRev++; topoRev_++; requestUpdate();
  if (commitIt) { scene_->revision++; commit(false); emit topoState(); }
}
void Viewport::lobeSetActive(int k) {
  const int m = lobeTarget(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return;
  const int n = int(lobeNames(m).size()); if (k < 1 || k > n) return;
  it->raw.insert("lobeAct", k); lb_.drawKey.clear(); topoRev_++; requestUpdate(); emit topoState();
}
QString Viewport::lobeNew() {
  const int m = lobeTarget(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) { ui::toast(tr("Paint on a mesh first")); return "!"; }
  QJsonArray a = it->raw.value("lobes").toArray();
  const int act = lobeActive(m); const auto c = lobeCounts(m);
  if (act > 0 && size_t(act) < c.size() && c[size_t(act)] == 0) { ui::hud(tr("%1 is still empty — paint it").arg(a.at(act - 1).toString()), 1200); return "!"; }   // (the active one unpainted: kept for it)
  std::set<QString> taken; for (const QJsonValue& v : a) taken.insert(v.toString());
  QString name; for (int i = int(a.size()) + 1; name.isEmpty() || taken.count(name); i++) name = QString("Lobe %1").arg(i);
  a.append(name); it->raw.insert("lobes", a); it->raw.insert("lobeAct", int(a.size()));
  lb_.drawKey.clear(); topoRev_++; requestUpdate(); scene_->revision++; commit(false); emit topoState();
  ui::hud(tr("%1 — paint it (the others keep their colours)").arg(name), 1200); return name;
}
QString Viewport::lobeDelete(int k) {
  const int m = lobeTarget(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  QJsonArray a = it->raw.value("lobes").toArray(); if (k < 1 || k > int(a.size())) return "!";
  const QString name = a.at(k - 1).toString(); a.removeAt(k - 1);
  std::vector<int> lab = lobeLabels(m); for (int& x : lab) x = x == k ? 0 : x > k ? x - 1 : x;   // (the later ones a number down)
  if (a.isEmpty()) { it->raw.remove("lobes"); it->raw.remove("lobeAct"); } else { it->raw.insert("lobes", a); it->raw.insert("lobeAct", std::clamp(lobeActive(m) > k ? lobeActive(m) - 1 : lobeActive(m), 1, int(a.size()))); }
  QJsonArray fl; for (const QJsonValue& v : it->raw.value("lobeFlip").toArray()) { const int l = v.toInt(); if (l != k) fl.append(l > k ? l - 1 : l); }
  if (fl.isEmpty()) it->raw.remove("lobeFlip"); else it->raw.insert("lobeFlip", fl);
  lobeWrite(m, lab, true); ui::toast(tr("%1 deleted").arg(name)); return name;
}
void Viewport::lobeRename(int k, const QString& name) {
  const int m = lobeTarget(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || name.trimmed().isEmpty()) return;
  QJsonArray a = it->raw.value("lobes").toArray(); if (k < 1 || k > int(a.size())) return;
  a[k - 1] = name.trimmed(); it->raw.insert("lobes", a); scene_->revision++; commit(false); emit topoState();
}
QString Viewport::lobeClear() {
  const int m = lobeTarget(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  it->raw.remove("lobes"); it->raw.remove("lobeAct"); lobeWrite(m, std::vector<int>(size_t(baseOf(*it)->faceCount()), 0), true);
  ui::toast(tr("Every lobe cleared")); return {};
}

// ---------- the pointer ----------
bool Viewport::lobePress(QMouseEvent* e) {
  const QPointF mp = e->position(); const bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  std::vector<int> list; for (auto& [id, x] : scene_->items) if (x.type == ItemType::IMesh && x.poly && scene_->isVisible(x)) list.push_back(id);
  const auto r = tPick(mp, list); if (!r || r->f < 0) { ui::hud(tr("Lobe Brush: on a mesh"), 900); return true; }
  Item* it = scene_->item(r->m); if (!it) return true;
  if (r->m != lb_.m) { lb_.m = r->m; lb_.drawKey.clear(); }
  const auto& lab0 = lobeLabels(r->m); if (size_t(r->f) >= lab0.size()) return true;
  if (ctrl && shift) {                                          // (the lobe under the pointer: the one painted now)
    const int k = lab0[size_t(r->f)]; if (k <= 0) { ui::hud(tr("No lobe here"), 900); return true; }
    lobeSetActive(k); ui::hud(tr("%1 — active").arg(lobeNames(r->m)[size_t(k - 1)]), 900); return true; }
  if (lobeNames(r->m).empty()) { QJsonArray a{QString("Lobe 1")}; it->raw.insert("lobes", a); it->raw.insert("lobeAct", 1); }   // (the first stroke: the first lobe)
  const int act = lobeActive(r->m);
  if (shift) {                                                  // (a fill up to the crevices round it)
    ui::BusyScope bz(tr("Filling the lobe"));
    auto T = topo::ttopo(baseSp(*it));
    // (the crevices: seams folded sharper than 70° … 20° as Crevices goes up; over its middle the hollows too — the top
    // 0 … 15% of the surface by concavity)
    if (lb_.cB != baseOf(*it) || lb_.conc.size() != size_t(T->nf)) { lb_.cB = baseOf(*it); lb_.conc = lobes::concavity(*T, it->poly->pos); lb_.crSens = -1; }
    if (lb_.crSens != lb_.sens) { lb_.crSens = lb_.sens; const double s = std::clamp(lb_.sens, 0.0, 1.0);
      lb_.cr = lobes::crevices(*T, it->poly->pos, lb_.conc, 70 - 50 * s, s > 0.5 ? lobes::quantile(lb_.conc, 1.0 - (s - 0.5) * 0.3) : 1e30f); }
    // (the crevices closed wider and wider: a fill through a gap in them is much bigger than the next one — the first
    // that the next closing doesn't shrink much is the lobe)
    auto lab = lab0; const size_t cap = std::max<size_t>(400, size_t(T->nf) / 4); std::vector<std::vector<int>> tries;
    for (int close : {0, 1, 2, 4, 7}) tries.push_back(lobes::fill(*T, lb_.conc, lb_.cr, lab, r->f, act, lb_.overwrite, close, cap));
    size_t pick = tries.size() - 1; for (size_t i = 0; i + 1 < tries.size(); i++) if (tries[i].size() <= cap && tries[i + 1].size() * 10 >= tries[i].size() * 6) { pick = i; break; }
    const std::vector<int>& fs = tries[pick]; lb_.lastFill.clear(); for (auto& t : tries) lb_.lastFill += QString::number(t.size()) + " "; lb_.lastFill += "→ " + QString::number(pick);
    if (fs.size() > cap) { ui::toast(tr("The fill runs out through a gap in the crevices — raise Crevices, or paint this lobe with the brush")); return true; }
    for (int f : fs) lab[size_t(f)] = act;
    lobeWrite(r->m, lab, true); ui::hud(tr("%1: %2 polygons filled").arg(lobeNames(r->m)[size_t(act - 1)]).arg(fs.size()), 1200); return true; }
  lb_.painting = true; lb_.erase = ctrl; lb_.last = mp; lb_.lab = lab0;
  tt_.drag = 40; td_ = {}; td_.type = 20; drag_ = Drag::Topo;
  lobePaint(mp); return true;
}
bool Viewport::lobeRmb(QMouseEvent* e) {
  if (!(e->modifiers() & Qt::ControlModifier)) return false;   // (without Ctrl: the pies, as anywhere)
  bd_ = {}; bd_.start = e->position(); bd_.r0 = brushR; bd_.h0 = brushHard; drag_ = Drag::BrushAdjust;   // (Ctrl+RMB drag: the size, as the brushes)
  return true;
}
bool Viewport::lobeMove(const QPointF& mp) {
  if (lb_.painting && drag_ == Drag::Topo) {                     // (painted along the way: steps of a third of the radius)
    const int n = std::max(1, int(std::ceil(std::hypot(mp.x() - lb_.last.x(), mp.y() - lb_.last.y()) / std::max(2.0, brushR / 3))));
    for (int i = 1; i <= n; i++) lobePaint(lb_.last + (mp - lb_.last) * (double(i) / n));
    lb_.last = mp; }
  overlayRev_++; requestUpdate(); return true;                  // (the circle follows the pointer)
}
bool Viewport::lobeRelease(const QPointF&) {
  if (!lb_.painting) return true; lb_.painting = false;
  ProfScope prof_("lobeRelease"); lobeWrite(lb_.m, lb_.lab, true); return true;
}
bool Viewport::lobeKey(int key) {
  if (key == Qt::Key_Return || key == Qt::Key_Enter) { lobeNew(); return true; }
  return false;
}
// the polygons within the brush's circle on the surface — facing the camera, not covered (as the snap mask's brush)
void Viewport::lobePaint(const QPointF& mp) {
  const int m = lb_.m; Item* it = scene_->item(m); if (!it || !it->poly) return;
  const auto r = tPick(mp, {m}); if (!r) return;
  auto bvh = bvhOf(m, false); const PickCache* pc = pickCacheOf(m); if (!bvh || !pc) return;
  const double rad = brushR * worldPerPixelAt(r->point), size = std::max(1e-9, it->poly->bounds().size().length()), tol = size * 1e-4;
  const int act = lobeActive(m); const bool ortho = cam_.ortho(); const auto& P = it->poly->pos; const Vec3 eye = cam_.pos, dir = cam_.dir();
  std::vector<std::pair<int, Vec3>> cand;
  bvh->sphere(r->point, rad, [&](int t) {
    const Vec3 &a = P[pc->tris[size_t(t) * 3]], &b = P[pc->tris[size_t(t) * 3 + 1]], &cc = P[pc->tris[size_t(t) * 3 + 2]];
    const Vec3 mid = (a + b + cc) * (1.0 / 3); if (mid.distanceTo(r->point) > rad) return;
    if ((b - a).cross(cc - a).dot(ortho ? dir : mid - eye) > 0) return;   // (the back side stays)
    cand.push_back({t, mid}); });
  for (auto& [t, p] : cand) {
    const int f = pc->tri2f[size_t(t)]; if (size_t(f) >= lb_.lab.size()) continue; int& x = lb_.lab[size_t(f)];
    const int to = lb_.erase ? 0 : act; if (x == to) continue;
    if (!lb_.overwrite && (lb_.erase ? x != act : x != 0)) continue;   // (the other lobes kept — unless Paint over the others)
    const Vec3 o = ortho ? p - dir * (size * 10) : eye; const double Ld = o.distanceTo(p); if (bvh->ray(o, (p - o).normalized(), Ld - tol)) continue;   // (something of it in front)
    x = to; }
  lb_.paintRev++;
  if (const qint64 now = QDateTime::currentMSecsSinceEpoch(); now - lb_.shown > 50) { lb_.shown = now; topoRev_++; requestUpdate(); }
}

// ---------- drawn: the active lobe white, the others in their colours ----------
void Viewport::lobeDraw(TopoDraw& D) {
  const int m = lobeTarget(); const Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it || !it->poly) return;
  const auto& lab = lb_.painting && lb_.m == m ? lb_.lab : lobeLabels(m); const int act = lobeActive(m);
  const PolyMesh& B = *baseSp(*it); const auto& W = it->poly->pos; if (W.size() != B.pos.size() || int(lab.size()) != B.faceCount()) return;
  const QByteArray key = QByteArray::number(m) + "|" + QByteArray::number(quintptr(it->poly.get())) + "|" + QByteArray::number(lb_.paintRev) + "|" + QByteArray::number(act) + "|" + lb_.labKey.left(64).toLatin1() + QByteArray::number(lb_.labKey.size());
  if (key != lb_.drawKey) {
    lb_.drawKey = key; lb_.drawn.clear(); std::map<int, size_t> at;
    for (int f = 0; f < B.faceCount(); f++) { const int k = lab[size_t(f)]; if (k <= 0) continue;
      auto f0 = at.find(k); if (f0 == at.end()) { f0 = at.insert({k, lb_.drawn.size()}).first; lb_.drawn.push_back({k == act ? QColor(Qt::white) : lobeColor(k), {}}); }
      auto& L = lb_.drawn[f0->second].second; const int s = B.faceStart[size_t(f)], e = B.faceStart[size_t(f) + 1];
      for (int q = s + 1; q + 1 < e; q++) { L.push_back(W[size_t(B.faceVerts[size_t(s)])]); L.push_back(W[size_t(B.faceVerts[size_t(q)])]); L.push_back(W[size_t(B.faceVerts[size_t(q + 1)])]); } }
  }
  for (auto& [c, L] : lb_.drawn) { TopoDraw::Tris t; t.c = c; t.op = c == QColor(Qt::white) ? 0.62f : 0.58f; t.p = L; D.tris.push_back(std::move(t)); }
}

}  // namespace hm

namespace hm {
// (tests: the polygons taken as crevices at sensitivity q shown as one lobe — the others cleared)
QString Viewport::testLobeCrev(double q) {
  const int m = lobeTarget(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return "!";
  auto T = topo::ttopo(baseSp(*it));
  if (q > 1) {   // (tests: the polygons with an edge folded more than q degrees)
    const auto& P = it->poly->pos; std::vector<Vec3> N(size_t(T->nf));
    for (int f = 0; f < T->nf; f++) { Vec3 n; const int s = T->fo[size_t(f)], e = T->fo[size_t(f) + 1]; for (int k = s; k < e; k++) { const Vec3& a = P[size_t(T->fv[size_t(k)])]; const Vec3& b = P[size_t(T->fv[size_t(k + 1 < e ? k + 1 : s)])]; n.x += (a.y - b.y) * (a.z + b.z); n.y += (a.z - b.z) * (a.x + b.x); n.z += (a.x - b.x) * (a.y + b.y); } N[size_t(f)] = n.normalized(); }
    std::vector<int> lab(size_t(T->nf), 0); int n = 0; const double cq = std::cos(q * M_PI / 180);
    for (int e = 0; e < T->ne; e++) { const int a = T->ef0[size_t(e)], b = T->ef1[size_t(e)]; if (b < 0) { lab[size_t(a)] = 2; continue; } if (N[size_t(a)].dot(N[size_t(b)]) < cq) { lab[size_t(a)] = lab[size_t(b)] = 1; } }
    for (int x : lab) n += x > 0;
    it->raw.insert("lobes", QJsonArray{QString("Folds"), QString("Rim")}); it->raw.insert("lobeAct", 1); lobeWrite(m, lab, false); return QString("%1 faces folded").arg(n); }
const auto c = lobes::concavity(*T, it->poly->pos); const float thr = lobes::quantile(c, 1.0 - q);
  std::vector<int> lab(c.size(), 0); int n = 0; for (size_t f = 0; f < c.size(); f++) if (c[f] >= thr) { lab[f] = 1; n++; }
  it->raw.insert("lobes", QJsonArray{QString("Crevices")}); it->raw.insert("lobeAct", 1); lobeWrite(m, lab, false);
  return QString("thr %1, %2 faces; q01 %3 q50 %4 q99 %5").arg(thr).arg(n).arg(lobes::quantile(c, 0.01)).arg(lobes::quantile(c, 0.5)).arg(lobes::quantile(c, 0.99));
}
}  // namespace hm

namespace hm {
using tedit::MeshEd;
// ---------- the retopology carried onto the other lobes ----------
const std::vector<lobes::LobeMap>& Viewport::lobeMaps(int m) {
  static const std::vector<lobes::LobeMap> none; const Item* it = scene_->item(m); if (!it || !it->poly) return none;
  const auto& lab = lobeLabels(m);
  const QByteArray key = QByteArray::number(m) + "|" + QByteArray::number(quintptr(it->poly.get())) + "|" + lb_.labKey.toLatin1();
  if (key != lb_.mapKey) { ui::BusyScope bz(tr("Laying the lobes flat")); ProfScope prof_("lobeMaps"); lb_.mapKey = key; auto T = topo::ttopo(baseSp(*it)); lb_.maps = lobes::maps(*T, it->poly->pos, lab); }
  return lb_.maps;
}
bool Viewport::lobeFlipped(int m, int k) const {
  const Item* it = scene_->item(m); if (!it) return false; for (const QJsonValue& v : it->raw.value("lobeFlip").toArray()) if (v.toInt() == k) return true; return false;
}
void Viewport::lobeFlip(int k) {
  const int m = lobeTarget(); Item* it = m >= 0 ? scene_->item(m) : nullptr; if (!it) return;
  QJsonArray a; bool was = false; for (const QJsonValue& v : it->raw.value("lobeFlip").toArray()) { if (v.toInt() == k) was = true; else a.append(v); } if (!was) a.append(k);
  if (a.isEmpty()) it->raw.remove("lobeFlip"); else it->raw.insert("lobeFlip", a);
  topoRev_++; requestUpdate(); scene_->revision++; commit(false); emit topoState();
}
// the mesh being built: the picked one (not the painted high-poly), else the last one
int Viewport::lobeSource() const {
  const int t = lobeTarget();
  auto good = [&](int id) { const Item* it = id >= 0 ? scene_->item(id) : nullptr; return it && id != t && it->type == ItemType::IMesh && it->poly && scene_->isVisible(*it) && baseOf(*it)->faceCount() > 0 && !meshmods::heavy(*it); };
  if (good(topoMesh())) return topoMesh(); return good(lb_.src) ? lb_.src : -1;
}
// what goes where: the polygons of the mesh being built on the lobe holding most of them (the template) — onto every lobe
// none of them lies on yet, through the squares (a lobe turned round: the copy turned too)
void Viewport::lobeGhostBuild() {
  const int m = lobeTarget(), s = lobeSource(); if (s >= 0) lb_.src = s;
  const Item* ti = m >= 0 ? scene_->item(m) : nullptr; const Item* si = s >= 0 ? scene_->item(s) : nullptr;
  if (!ti || !si || lobeNames(m).empty()) { lb_.gh.clear(); lb_.ghLines.clear(); lb_.ghKey.clear(); lb_.ghTemplate = 0; lb_.ghFaces = 0; return; }
  const auto& M = lobeMaps(m);
  const QByteArray key = QByteArray::number(m) + "|" + QByteArray::number(s) + "|" + QByteArray::number(quintptr(si->poly.get())) + "|" + QByteArray::number(quintptr(baseOf(*si))) + "|" + QByteArray::number(geomStamp_.count(s) ? geomStamp_.at(s) : 0)
                         + "|" + QJsonDocument(ti->raw.value("lobeFlip").toArray()).toJson(QJsonDocument::Compact) + "|" + lb_.mapKey;
  if (key == lb_.ghKey) return;
  lb_.ghKey = key; lb_.gh.clear(); lb_.ghLines.clear(); lb_.ghTemplate = 0; lb_.ghFaces = 0; if (M.empty()) return;
  ProfScope prof_("lobeGhost");
  const PolyMesh& B = *baseOf(*si); const auto W = tWorld(s); const int nf = B.faceCount();
  double el = 0; int ne = 0;
  for (int f = 0; f < nf; f++) { const int a = B.faceStart[size_t(f)], b = B.faceStart[size_t(f) + 1];
    for (int k = a; k < b; k++) { el += W[size_t(B.faceVerts[size_t(k)])].distanceTo(W[size_t(B.faceVerts[size_t(k + 1 < b ? k + 1 : a)])]); ne++; } }
  el /= std::max(1, ne);
  // (each polygon: the lobe it lies on — the nearest, near enough)
  std::vector<int> onL(size_t(nf), -1); std::vector<int> count(M.size(), 0);
  for (int f = 0; f < nf; f++) { Vec3 c; const int a = B.faceStart[size_t(f)], b = B.faceStart[size_t(f) + 1]; for (int k = a; k < b; k++) c += W[size_t(B.faceVerts[size_t(k)])]; c = c * (1.0 / std::max(1, b - a));
    double bd = el * 1.5; int bi = -1; for (size_t j = 0; j < M.size(); j++) { double d = 0; if (M[j].bvh && M[j].bvh->closest(c, nullptr, &d) && d < bd) { bd = d; bi = int(j); } }
    if (bi >= 0) { onL[size_t(f)] = bi; count[size_t(bi)]++; } }
  const int tpl = int(std::max_element(count.begin(), count.end()) - count.begin()); if (count[size_t(tpl)] == 0) return;
  lb_.ghTemplate = M[size_t(tpl)].id;
  std::vector<int> faces; std::set<int> verts;
  // (a lobe with a few polygons only — the template's edge reaching over the seam: those the template's, the lobe still free)
  const int few = std::max(3, count[size_t(tpl)] / 4); auto built = [&](int j) { return j == tpl || count[size_t(j)] > few; };
  for (int f = 0; f < nf; f++) if (onL[size_t(f)] >= 0 && (onL[size_t(f)] == tpl || !built(onL[size_t(f)]))) { faces.push_back(f);
    for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) verts.insert(B.faceVerts[size_t(k)]); }
  lb_.ghFaces = int(faces.size());
  // (each other lobe: the template lobe laid over it — turned, moved, sized, mirrored if that fits better; ⟲ — the next
  // best way; found once per lobes and template)
  const QByteArray fk = lb_.mapKey + "|" + QByteArray::number(tpl);
  if (fk != lb_.fitKey) { ui::BusyScope bz(tr("Laying the lobes over each other")); ProfScope prof2_("lobeFits"); lb_.fitKey = fk; lb_.fits.clear();
    for (size_t j = 0; j < M.size(); j++) if (int(j) != tpl) lb_.fits[int(j)] = lobes::fits(M[size_t(tpl)], M[j]); }
  for (size_t j = 0; j < M.size(); j++) { if (built(int(j))) continue;   // (a lobe built on already: left as it is)
    const auto& fl = lb_.fits[int(j)]; if (fl.empty()) continue;
    const lobes::Fit& fit = fl[lobeFlipped(m, M[j].id) && fl.size() > 1 ? 1 : 0];
    LobeGhost g; g.id = M[j].id; g.mi = int(j); g.mirror = fit.mirror; std::map<int, int> vi; std::map<int, Vec3> raw;
    auto onB = [&](const Vec3& p) { const auto q = M[j].bvh->closest(p, nullptr); return q ? *q : p; };   // (onto that lobe's surface)
    for (int v : verts) raw[v] = fit.apply(W[size_t(v)]);
    for (int f : faces) { std::vector<int> poly;
      for (int k = B.faceStart[size_t(f)]; k < B.faceStart[size_t(f) + 1]; k++) { const int v = B.faceVerts[size_t(k)]; auto it = vi.find(v);
        if (it == vi.end()) { g.P.push_back(onB(raw[v])); it = vi.insert({v, int(g.P.size()) - 1}).first; }
        poly.push_back(it->second); }
      if (g.mirror) std::reverse(poly.begin(), poly.end());   // (mirrored: turned the other way — facing out still)
      g.F.push_back(poly); }
    // (its edges in short pieces laid onto the lobe — what is behind it hidden)
    std::set<std::pair<int, int>> seen;
    for (auto& f : g.F) for (size_t k = 0; k < f.size(); k++) { const int a = f[k], b = f[(k + 1) % f.size()]; if (!seen.insert({std::min(a, b), std::max(a, b)}).second) continue;
      Vec3 prev = g.P[size_t(a)];
      for (int i = 1; i <= 6; i++) { const Vec3 q = i == 6 ? g.P[size_t(b)] : onB(g.P[size_t(a)] + (g.P[size_t(b)] - g.P[size_t(a)]) * (i / 6.0));
        lb_.ghLines.push_back(prev); lb_.ghLines.push_back(q); prev = q; } }
    lb_.gh.push_back(std::move(g)); }
}
void Viewport::lobeGhostDraw(TopoDraw& D) {
  if (!lb_.ghost || room() != "topo") return;
  lobeGhostBuild(); if (lb_.ghLines.empty()) return;
  auto up = [&](const Vec3& p) { const Vec3 to = cam_.ortho() ? cam_.dir() * -1.0 : (cam_.pos - p).normalized(); return p + to * (worldPerPixelAt(p) * 2); };
  TopoDraw::Lines l; l.c = QColor(0x3c, 0xf0, 0xf0); l.w = 1.6f; l.noPdo = true; for (const Vec3& p : lb_.ghLines) l.p.push_back(up(p)); D.lines.push_back(std::move(l));
}
// the copies made real: into the mesh being built, their points welded onto its vertices (and each other's) where they meet
QString Viewport::lobeApply() {
  lobeGhostBuild(); if (lb_.gh.empty()) { const QString msg = tr("Nothing to carry: build the retopology of one lobe first"); ui::toast(msg); return "!" + msg; }
  const int s = lb_.src; Item* it = scene_->item(s); if (!it) return "!";
  const PolyMesh& B = *baseOf(*it); MeshEd E = MeshEd::from(B); const Affine Mi = meshmods::meshMatrix(*scene_, *it).inverse(); const double sc = Mi.lin(Vec3(1, 0, 0)).length();
  double el = 0; int ne = 0; for (auto& g : lb_.gh) for (auto& f : g.F) for (size_t k = 0; k < f.size(); k++) { el += g.P[size_t(f[k])].distanceTo(g.P[size_t(f[(k + 1) % f.size()])]); ne++; }
  const double tol = el / std::max(1, ne) * 0.3 * sc;
  std::vector<int> made; int lobesDone = 0;
  for (auto& g : lb_.gh) { std::vector<int> id(g.P.size(), -1);
    for (size_t i = 0; i < g.P.size(); i++) { const Vec3 q = Mi.apply(g.P[i]); double bd = tol; int bv = -1;
      for (size_t v = 0; v < E.pos.size(); v++) { const double d = E.pos[v].distanceTo(q); if (d < bd) { bd = d; bv = int(v); } }
      id[i] = bv >= 0 ? bv : E.addVert(q); }
    for (auto& f : g.F) { std::vector<int> vs; for (int x : f) if (vs.empty() || vs.back() != id[size_t(x)]) vs.push_back(id[size_t(x)]); while (vs.size() > 1 && vs.front() == vs.back()) vs.pop_back();
      std::set<int> u(vs.begin(), vs.end()); if (vs.size() >= 3 && u.size() == vs.size()) made.push_back(E.addFace(vs)); }
    lobesDone++; }
  if (made.empty()) return "!";
  const QString msg = tr("The retopology carried onto %1 lobe%2: %3 polygons").arg(lobesDone).arg(lobesDone == 1 ? "" : "s").arg(made.size());
  return ttDone(s, E, B, msg, 'f', made);
}
QString Viewport::lobeGhostInfo() {
  lobeGhostBuild(); const int m = lobeTarget(); if (m < 0) return {};
  if (lobeSource() < 0) return tr("Pick the mesh you build (not the high-poly) — retopologize one lobe on it");
  if (!lb_.ghTemplate) return tr("Retopologize one lobe (PolyPen, PatchTool …) — it shows on the other lobes");
  const auto names = lobeNames(m); const QString tn = lb_.ghTemplate - 1 < int(names.size()) ? names[size_t(lb_.ghTemplate - 1)] : QString::number(lb_.ghTemplate);
  return tr("Template: %1 — %2 polygons → %3 lobe%4").arg(tn).arg(lb_.ghFaces).arg(lb_.gh.size()).arg(lb_.gh.size() == 1 ? "" : "s");
}
}  // namespace hm

namespace hm {
// (tests: the polygons of mesh src lying on lobe k — a new mesh of them, picked: a template made by hand)
QString Viewport::testLobeTemplate(int src, int k) {
  const int m = lobeTarget(); if (m < 0) return "!";
  const auto& M = lobeMaps(m); const lobes::LobeMap* L = nullptr; for (auto& x : M) if (x.id == k) L = &x; if (!L) return "!no map";
  if (src < 0) {                                                 // (src −1: a 8 × 4 grid on the lobe's square instead)
    PolyMesh d; const int nu = 8, nv = 4; for (int j = 0; j <= nv; j++) for (int i = 0; i <= nu; i++) d.pos.push_back(lobes::fromUV(*L, double(i) / nu, double(j) / nv));
    for (int j = 0; j < nv; j++) for (int i = 0; i < nu; i++) { const int a = j * (nu + 1) + i; const int q[4] = {a, a + 1, a + nu + 2, a + nu + 1}; d.addFace(q, 4); }
    d.faceGroup.assign(size_t(d.faceCount()), 0); d.groupNames = {"Polygroup"};
    const int id = topoNewMesh(-1, "LobeTemplate", d); sel_ = {}; sel_.items = {id}; onSelChange(); scene_->revision++; emit sceneEdited(); commit(true); emit topoState();
    return QString("grid template %1").arg(id); }
  const Item* si = scene_->item(src); if (!si) return "!";
  const PolyMesh& B = *baseOf(*si); const auto W = tWorld(src); PolyMesh d; std::map<int, int> vi;
  for (int f = 0; f < B.faceCount(); f++) { Vec3 c; const int a = B.faceStart[size_t(f)], b = B.faceStart[size_t(f) + 1]; for (int q = a; q < b; q++) c += W[size_t(B.faceVerts[size_t(q)])]; c = c * (1.0 / (b - a));
    double dist = 0; double best = 1e300; int bi = -1; for (size_t j = 0; j < M.size(); j++) { if (M[j].bvh && M[j].bvh->closest(c, nullptr, &dist) && dist < best) { best = dist; bi = int(j); } }
    if (bi < 0 || M[size_t(bi)].id != k) continue;
    std::vector<int> vs; for (int q = a; q < b; q++) { const int v = B.faceVerts[size_t(q)]; auto it = vi.find(v); if (it == vi.end()) { d.pos.push_back(W[size_t(v)]); it = vi.insert({v, int(d.pos.size()) - 1}).first; } vs.push_back(it->second); }
    d.addFace(vs.data(), int(vs.size())); }
  if (!d.faceCount()) return "!none";
  d.faceGroup.assign(size_t(d.faceCount()), 0); d.groupNames = {"Polygroup"};
  const int id = topoNewMesh(-1, "LobeTemplate", d); sel_ = {}; sel_.items = {id}; onSelChange(); scene_->revision++; emit sceneEdited(); commit(true); emit topoState();
  return QString("template %1: %2 polygons").arg(id).arg(d.faceCount());
}
}  // namespace hm
